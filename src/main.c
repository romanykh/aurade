#include "server.h"
#include "window/xdg_shell.h"
#include "window/workspace.h"
#include "window/xwayland.h"
#include "shell/wallpaper.h"
#include "shell/topbar.h"
#include "shell/dock.h"
#include "shell/mission_control.h"
#include "shell/launchpad.h"
#include "shell/control_center.h"
#include "shell/osd.h"
#include "shell/screenshot.h"
#include "shell/notifications.h"
#include "shell/music_pill.h"
#include "gestures/gesture_engine.h"

#include <xkbcommon/xkbcommon.h>
#include <wlr/backend/libinput.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <limits.h>
#include <signal.h>
#include <execinfo.h>

// Touchpad travel arrives in 1920x1080 reference pixels (gesture_engine.c).
// macOS moves the Spaces carousel further than the fingers travel, so one
// workspace completes after roughly 40% of the touchpad width instead of a
// full-width swipe.
#define WORKSPACE_GESTURE_GAIN 2.4
#define GESTURE_REFERENCE_PX 1920.0

// A flick is a release fast enough to finish the gesture in about half a
// second; the config stores it in reference pixels per second.
#define GESTURE_FLICK_REFERENCE 380.0

// A compositor that dies on the real session should leave evidence behind.
// GDM only shows the login screen again, so dump a backtrace to disk.
static void crash_handler(int sig) {
    int fd = open("/tmp/aurade-crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char header[128];
        int n = snprintf(header, sizeof(header), "\n=== AuraDE crash: signal %d ===\n", sig);
        if (n > 0) { ssize_t r = write(fd, header, (size_t)n); (void)r; }
        void *frames[64];
        int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, fd);
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

// Config reload watcher callback
static int config_inotify_handler(int fd, uint32_t mask, void *data) {
    (void)mask; // Unused
    aurade_server_t *server = data;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool relevant = false;
    ssize_t len;
    while ((len = read(fd, buf, sizeof(buf))) > 0) {
        char *ptr = buf;
        while (ptr < buf + len) {
            struct inotify_event *ev = (struct inotify_event *)ptr;
            if (ev->len == 0 || strcmp(ev->name, "config.json") == 0) {
                relevant = true;
            }
            ptr += sizeof(struct inotify_event) + ev->len;
        }
    }
    if (!relevant) {
        return 0;
    }

    char prev_wallpaper[512];
    snprintf(prev_wallpaper, sizeof(prev_wallpaper), "%s", server->config.wallpaper_path);
    double prev_blur_strength = server->config.blur_strength;

    // Re-add watch on config file in case it was replaced atomically
    const char *cfg_path = config_get_default_path();
    inotify_add_watch(server->config_inotify_fd, cfg_path, IN_MODIFY | IN_ATTRIB);

    config_load(&server->config, NULL);
    fprintf(stderr, "[AuraDE] Config reloaded from %s (DWT=%s)\n",
            config_get_default_path(),
            server->config.disable_while_typing ? "enabled" : "disabled");

    // Apply live: re-render everything that depends on configuration.
    if (server->config.num_workspaces > 0 && server->config.num_workspaces != server->num_workspaces) {
        server_set_num_workspaces(server, server->config.num_workspaces);
    }
    if (server->wallpaper && (strcmp(prev_wallpaper, server->config.wallpaper_path) != 0 ||
                              fabs(prev_blur_strength - server->config.blur_strength) > 0.01)) {
        wallpaper_reload(server->wallpaper);
    }
    if (server->dock) {
        server->dock->blur_dirty = true;
        dock_update_running_state(server->dock);
        dock_render(server->dock);
        dock_update_visibility(server->dock);
    }
    if (server->topbar) {
        server->topbar->blur_dirty = true;
        topbar_render(server->topbar);
    }
    if (server->launchpad) launchpad_render(server->launchpad);
    if (server->mission_control) mission_control_render(server->mission_control);
    if (server->control_center) control_center_render(server->control_center);
    if (server->music_pill) music_pill_update(server->music_pill);
    server_update_touchpad_settings(server);
    return 0;
}

// One-second tick so the menu-bar clock stays accurate even when nothing
// else is animating.
static int clock_tick(void *data) {
    aurade_server_t *server = data;
    if (server->topbar) topbar_render(server->topbar);
    if (server->clock_timer) wl_event_source_timer_update(server->clock_timer, 1000);
    return 0;
}

static void output_frame(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_output_t *output = wl_container_of(listener, output, frame);
    aurade_server_t *server = output->server;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (output->last_time.tv_sec != 0) {
        double dt = (now.tv_sec - output->last_time.tv_sec) + (now.tv_nsec - output->last_time.tv_nsec) / 1e9;
        if (dt > 0.0 && dt < 0.1) {
            server_tick_animations(server, dt);
        }
    }
    output->last_time = now;

    // Render dirty overlay buffers at VBlank (triple-buffered, synchronized)
    if (server->mission_control && server->mission_control->needs_render) {
        mission_control_render(server->mission_control);
    }
    if (server->launchpad && server->launchpad->needs_render) {
        launchpad_render(server->launchpad);
    }
    if (server->topbar && server->topbar->needs_render) {
        topbar_render(server->topbar);
    }
    if (server->dock && server->dock->needs_render) {
        dock_render(server->dock);
    }

    // Ensure any surface or subsurface commits that arrived before VBlank keep their active scale
    aurade_view_t *v;
    wl_list_for_each(v, &server->views, link) {
        if (!v->minimized && v->scene_tree && v->scene_tree->node.enabled &&
            v->current_scale > 0.0 && fabs(v->current_scale - 1.0) >= 0.001) {
            view_set_scale(v, v->current_scale, v->current_scale);
        }
    }

    wlr_scene_output_commit(output->scene_output, NULL);
    wlr_scene_output_send_frame_done(output->scene_output, &now);

    bool view_animating = false;
    wl_list_for_each(v, &server->views, link) {
        if (v->anim_type != VIEW_ANIM_NONE) {
            view_animating = true;
            break;
        }
    }
    bool dock_animating = server->dock && (dock_is_bouncing(server->dock) || server->dock->needs_render);
    bool topbar_animating = server->topbar && server->topbar->needs_render;

    if (view_animating || dock_animating || topbar_animating ||
        server->gesture_animating || server->workspace_animating || server->gesture_in_progress ||
        (server->launchpad && server->launchpad->page_animating) ||
        (server->osd && server->osd->visible) ||
        (server->notifications && server->notifications->count > 0)) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

void schedule_all_outputs(aurade_server_t *server) {
    if (!server) return;
    aurade_output_t *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}


static void output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_output_t *output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    aurade_output_t *output = calloc(1, sizeof(*output));
    output->server = server;
    output->wlr_output = wlr_output;
    output->width = wlr_output->width;
    output->height = wlr_output->height;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
    output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, output->scene_output);

    // Remember the primary output size: shell layout and workspace sliding
    // must never assume 1920x1080.
    if (server->screen_width == 0 || wl_list_length(&server->outputs) == 1) {
        server->screen_width = output->width;
        server->screen_height = output->height;
        server_update_workspace_positions(server);
    }

    // Resize shell components to output dimensions
    if (server->wallpaper) wallpaper_resize(server->wallpaper, output->width, output->height);
    if (server->topbar) topbar_resize(server->topbar, output->width);
    if (server->dock) dock_resize(server->dock, output->width, output->height);
    if (server->mission_control) mission_control_resize(server->mission_control, output->width, output->height);
    if (server->launchpad) launchpad_resize(server->launchpad, output->width, output->height);
    if (server->control_center) control_center_resize(server->control_center, output->width, output->height);
    if (server->screenshot) screenshot_resize(server->screenshot, output->width, output->height);
    if (server->osd) osd_resize(server->osd, output->width, output->height);
    if (server->notifications) notifications_resize(server->notifications, output->width, output->height);
}

// Keep edge resizing forgiving: the visible border is only a few pixels and
// CSD clients additionally draw a transparent shadow outside the window
// geometry. A 10 px band inside the edge, plus a small CSD shadow slop, makes
// the resize handles feel like a normal desktop without stealing clicks from
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

#define RESIZE_MARGIN_OUT 8
#define RESIZE_MARGIN_IN  8

static const char *resize_cursor_name(uint32_t edges) {
    if ((edges & WLR_EDGE_LEFT) && (edges & WLR_EDGE_TOP)) return "nwse-resize";
    if ((edges & WLR_EDGE_RIGHT) && (edges & WLR_EDGE_BOTTOM)) return "nwse-resize";
    if ((edges & WLR_EDGE_RIGHT) && (edges & WLR_EDGE_TOP)) return "nesw-resize";
    if ((edges & WLR_EDGE_LEFT) && (edges & WLR_EDGE_BOTTOM)) return "nesw-resize";
    if (edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) return "ew-resize";
    if (edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) return "ns-resize";
    return "default";
}

static aurade_view_t *server_view_resize_at(aurade_server_t *server, double lx, double ly, uint32_t *out_edges) {
    if (!server || !server->focused_view) return NULL;
    aurade_view_t *view = server->focused_view;
    if (view->workspace != server->current_workspace || view->minimized || view->maximized) {
        return NULL;
    }
    int vx = view->x;
    int vy = view->y;
    int vw = view->width;
    int vh = view->height;
    if (vw <= 0 || vh <= 0) return NULL;

    // Check if within outer bounding box with margin
    if (lx < vx - RESIZE_MARGIN_OUT || lx > vx + vw + RESIZE_MARGIN_OUT ||
        ly < vy - RESIZE_MARGIN_OUT || ly > vy + vh + RESIZE_MARGIN_OUT) {
        return NULL;
    }

    // Inside inner content area (not on edge) -> no resize
    if (lx > vx + RESIZE_MARGIN_IN && lx < vx + vw - RESIZE_MARGIN_IN &&
        ly > vy + RESIZE_MARGIN_IN && ly < vy + vh - RESIZE_MARGIN_IN) {
        return NULL;
    }

    uint32_t edges = 0;
    if (lx <= vx + RESIZE_MARGIN_IN) edges |= WLR_EDGE_LEFT;
    if (lx >= vx + vw - RESIZE_MARGIN_IN) edges |= WLR_EDGE_RIGHT;
    if (ly <= vy + RESIZE_MARGIN_IN) edges |= WLR_EDGE_TOP;
    if (ly >= vy + vh - RESIZE_MARGIN_IN) edges |= WLR_EDGE_BOTTOM;

    if (edges != 0) {
        if (out_edges) *out_edges = edges;
        return view;
    }
    return NULL;
}

static bool super_is_held(aurade_server_t *server) {
    if (!server || !server->seat) return false;
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
    if (!kb || !kb->xkb_state) return false;
    return xkb_state_mod_name_is_active(kb->xkb_state, XKB_MOD_NAME_LOGO,
                                        XKB_STATE_MODS_EFFECTIVE) > 0;
}

static aurade_view_t *view_from_surface(aurade_server_t *server, struct wlr_surface *surface) {
    if (!surface || !server) return NULL;
    struct wlr_surface *root = wlr_surface_get_root_surface(surface);
    if (!root) root = surface;
    aurade_view_t *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->xdg_toplevel && view->xdg_toplevel->base &&
            (view->xdg_toplevel->base->surface == root || view->xdg_toplevel->base->surface == surface)) {
            return view;
        }
        if (view->xwayland_surface &&
            (view->xwayland_surface->surface == root || view->xwayland_surface->surface == surface)) {
            return view;
        }
    }
    return NULL;
}

static void handle_constraint_destroy(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, constraint_destroy);
    struct wlr_pointer_constraint_v1 *constraint = data;
    if (server->active_constraint == constraint) {
        wl_list_remove(&server->constraint_destroy.link);
        wl_list_init(&server->constraint_destroy.link);
        wl_list_remove(&server->constraint_set_region.link);
        wl_list_init(&server->constraint_set_region.link);
        server->active_constraint = NULL;
    }
}

static void handle_constraint_set_region(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
}

void server_set_pointer_constraint(aurade_server_t *server,
                                   struct wlr_pointer_constraint_v1 *constraint) {
    if (server->active_constraint == constraint) {
        return;
    }

    if (server->active_constraint) {
        if (server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED &&
            server->active_constraint->current.cursor_hint.enabled) {
            aurade_view_t *view = view_from_surface(server, server->active_constraint->surface);
            if (view) {
                wlr_cursor_warp(server->cursor, NULL,
                                view->x + server->active_constraint->current.cursor_hint.x,
                                view->y + server->active_constraint->current.cursor_hint.y);
            }
        }
        wlr_pointer_constraint_v1_send_deactivated(server->active_constraint);
        wl_list_remove(&server->constraint_destroy.link);
        wl_list_init(&server->constraint_destroy.link);
        wl_list_remove(&server->constraint_set_region.link);
        wl_list_init(&server->constraint_set_region.link);
        server->active_constraint = NULL;
    }

    if (constraint) {
        server->active_constraint = constraint;
        wl_signal_add(&constraint->events.destroy, &server->constraint_destroy);
        wl_signal_add(&constraint->events.set_region, &server->constraint_set_region);
        wlr_pointer_constraint_v1_send_activated(constraint);

        if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED &&
            constraint->current.cursor_hint.enabled) {
            aurade_view_t *view = view_from_surface(server, constraint->surface);
            if (view) {
                wlr_cursor_warp(server->cursor, NULL,
                                view->x + constraint->current.cursor_hint.x,
                                view->y + constraint->current.cursor_hint.y);
            }
        }
    }
}

static struct wlr_pointer_constraint_v1 *get_constraint_for_surface(aurade_server_t *server,
                                                                   struct wlr_surface *surface) {
    if (!server || !server->pointer_constraints || !surface) return NULL;

    struct wlr_pointer_constraint_v1 *c =
        wlr_pointer_constraints_v1_constraint_for_surface(server->pointer_constraints,
                                                          surface, server->seat);
    if (c) return c;

    struct wlr_surface *root = wlr_surface_get_root_surface(surface);
    if (root && root != surface) {
        c = wlr_pointer_constraints_v1_constraint_for_surface(server->pointer_constraints,
                                                              root, server->seat);
        if (c) return c;
    }

    aurade_view_t *view = view_from_surface(server, surface);
    if (view) {
        struct wlr_pointer_constraint_v1 *constraint;
        wl_list_for_each(constraint, &server->pointer_constraints->constraints, link) {
            if (constraint->seat == server->seat) {
                if (view_from_surface(server, constraint->surface) == view) {
                    return constraint;
                }
            }
        }
    }
    return NULL;
}

static void handle_new_pointer_constraint(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, new_pointer_constraint);
    struct wlr_pointer_constraint_v1 *constraint = data;

    struct wlr_surface *focused = server->seat->pointer_state.focused_surface;
    aurade_view_t *focused_view = server->focused_view;
    aurade_view_t *constraint_view = view_from_surface(server, constraint->surface);

    bool should_activate = false;
    if (focused) {
        if (focused == constraint->surface ||
            wlr_surface_get_root_surface(focused) == wlr_surface_get_root_surface(constraint->surface)) {
            should_activate = true;
        }
    }
    if (focused_view && constraint_view && focused_view == constraint_view) {
        should_activate = true;
    }

    if (should_activate) {
        server_set_pointer_constraint(server, constraint);
    }
}

static void cursor_motion(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    if (server->gesture_in_progress) {
        return;
    }

    // Unconditionally deliver relative motion events to listening clients (FPS games, Minecraft, etc.)
    if (server->relative_pointer_mgr) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
            server->relative_pointer_mgr,
            server->seat,
            (uint64_t)event->time_msec * 1000,
            event->delta_x, event->delta_y,
            event->unaccel_dx, event->unaccel_dy);
    }

    // If no active constraint yet, check if focused surface or view has a pending constraint
    if (!server->active_constraint && server->seat->pointer_state.focused_surface) {
        struct wlr_pointer_constraint_v1 *c =
            get_constraint_for_surface(server, server->seat->pointer_state.focused_surface);
        if (c) {
            server_set_pointer_constraint(server, c);
        }
    }

    // Locked pointer constraint: keep cursor static and suppress standard wl_pointer.motion
    if (server->active_constraint &&
        server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        return;
    }

    // Confined pointer constraint: constrain cursor inside surface bounds / region
    if (server->active_constraint &&
        server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
        struct wlr_pointer_constraint_v1 *constraint = server->active_constraint;
        aurade_view_t *view = view_from_surface(server, constraint->surface);
        if (view) {
            double next_lx = server->cursor->x + event->delta_x;
            double next_ly = server->cursor->y + event->delta_y;
            double sx = next_lx - view->x;
            double sy = next_ly - view->y;

            bool inside = false;
            if (pixman_region32_not_empty(&constraint->region)) {
                inside = pixman_region32_contains_point(&constraint->region,
                                                        (int)sx, (int)sy, NULL);
            } else {
                inside = (sx >= 0 && sx < constraint->surface->current.width &&
                          sy >= 0 && sy < constraint->surface->current.height);
            }

            if (!inside) {
                if (sx < 0) sx = 0;
                if (sx >= constraint->surface->current.width)
                    sx = constraint->surface->current.width > 0 ? constraint->surface->current.width - 1 : 0;
                if (sy < 0) sy = 0;
                if (sy >= constraint->surface->current.height)
                    sy = constraint->surface->current.height > 0 ? constraint->surface->current.height - 1 : 0;

                wlr_cursor_warp(server->cursor, NULL, view->x + sx, view->y + sy);
                wlr_seat_pointer_notify_motion(server->seat, event->time_msec, sx, sy);
                return;
            }
        }
    }

    if (event->delta_x != 0 || event->delta_y != 0) {
        wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    }

    double lx = server->cursor->x;
    double ly = server->cursor->y;

    if (server->screenshot && screenshot_handle_motion(server->screenshot, lx, ly)) {
        return;
    }

    if (server->cursor_mode == CURSOR_MOVE && server->grabbed_view) {
        server->grabbed_view->x = (int)(lx - server->grab_x);
        server->grabbed_view->y = (int)(ly - server->grab_y);
        wlr_scene_node_set_position(&server->grabbed_view->scene_tree->node,
                                    server->grabbed_view->x, server->grabbed_view->y);
        if (server->dock) {
            dock_update_visibility(server->dock);
            if (server->config.blur_dock && server->grabbed_view) {
                aurade_view_t *gv = server->grabbed_view;
                if (gv->workspace == server->current_workspace && !gv->minimized &&
                    gv->y + gv->height > server->dock->pill_y && gv->y < server->dock->pill_y + server->dock->pill_height) {
                    server->dock->blur_dirty = true;
                    server->dock->needs_render = true;
                }
            }
        }
        if (server->topbar && server->config.blur_topbar && server->grabbed_view) {
            aurade_view_t *gv = server->grabbed_view;
            if (gv->workspace == server->current_workspace && !gv->minimized &&
                gv->y <= TOPBAR_HEIGHT + 24) {
                server->topbar->blur_dirty = true;
                server->topbar->needs_render = true;
            }
        }
        schedule_all_outputs(server);
        return;
    }

    if (server->cursor_mode == CURSOR_RESIZE && server->grabbed_view) {
        aurade_view_t *view = server->grabbed_view;
        double dx = lx - server->grab_x;
        double dy = ly - server->grab_y;
        int new_x = server->grab_geobox.x;
        int new_y = server->grab_geobox.y;
        int new_w = server->grab_geobox.width;
        int new_h = server->grab_geobox.height;

        if (server->resize_edges & WLR_EDGE_RIGHT) new_w += (int)round(dx);
        if (server->resize_edges & WLR_EDGE_BOTTOM) new_h += (int)round(dy);
        if (server->resize_edges & WLR_EDGE_LEFT) {
            new_w -= (int)round(dx);
            new_x += (int)round(dx);
        }
        if (server->resize_edges & WLR_EDGE_TOP) {
            new_h -= (int)round(dy);
            new_y += (int)round(dy);
        }

        if (new_w < 240) {
            if (server->resize_edges & WLR_EDGE_LEFT) {
                new_x = server->grab_geobox.x + server->grab_geobox.width - 240;
            }
            new_w = 240;
        }
        if (new_h < 180) {
            if (server->resize_edges & WLR_EDGE_TOP) {
                new_y = server->grab_geobox.y + server->grab_geobox.height - 180;
            }
            new_h = 180;
        }

        view->x = new_x;
        view->y = new_y;
        view->width = new_w;
        view->height = new_h;
        view->maximized = false;
        wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
        if (server->dock && server->config.blur_dock) {
            if (view->workspace == server->current_workspace && !view->minimized &&
                view->y + view->height > server->dock->pill_y && view->y < server->dock->pill_y + server->dock->pill_height) {
                server->dock->blur_dirty = true;
                server->dock->needs_render = true;
            }
        }
        if (server->topbar && server->config.blur_topbar) {
            if (view->workspace == server->current_workspace && !view->minimized &&
                view->y <= TOPBAR_HEIGHT + 24) {
                server->topbar->blur_dirty = true;
                server->topbar->needs_render = true;
            }
        }
        schedule_all_outputs(server);
        if (view->xdg_toplevel && view->xdg_toplevel->base->initialized) {
            view->last_configure_ms = now_ms();
            wlr_xdg_toplevel_set_size(view->xdg_toplevel, new_w, new_h);
        } else if (view->xdg_toplevel) {
            view->pending_configure = true;
        } else if (view->xwayland_surface) {
            wlr_xwayland_surface_configure(view->xwayland_surface, view->x, view->y, new_w, new_h);
        }
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
                               resize_cursor_name(server->resize_edges));
        if (server->dock) dock_update_visibility(server->dock);
        return;
    }

    // Overlays capture the pointer while open.
    if (server->mission_control && server->mission_control->active) {
        mission_control_handle_motion(server->mission_control, lx, ly);
        wlr_seat_pointer_clear_focus(server->seat);
        server_set_pointer_constraint(server, NULL);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
                               server->mission_control->is_dragging ? "grabbing" : "default");
        return;
    }
    if (server->launchpad && server->launchpad->active) {
        if (server->dock && (dock_pointer_owns(server->dock, lx, ly) || server->dock->mouse_over_dock)) {
            dock_handle_motion(server->dock, lx, ly);
        }
        launchpad_handle_motion(server->launchpad, lx, ly);
        wlr_seat_pointer_clear_focus(server->seat);
        server_set_pointer_constraint(server, NULL);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
                               server->launchpad->is_dragging ? "grabbing" : "default");
        return;
    }
    if (server->control_center && server->control_center->active) {
        control_center_handle_motion(server->control_center, lx, ly);
        wlr_seat_pointer_clear_focus(server->seat);
        server_set_pointer_constraint(server, NULL);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
                               control_center_cursor(server->control_center, lx, ly));
        return;
    }

    // Hover state for shell chrome (when overlays are not active).
    if (server->notifications) {
        if (notifications_handle_motion(server->notifications, lx, ly)) {
            wlr_seat_pointer_clear_focus(server->seat);
            server_set_pointer_constraint(server, NULL);
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
            return;
        }
    }
    if (server->music_pill && music_pill_is_expanded(server->music_pill)) {
        if (music_pill_overlay_owns(server->music_pill, lx, ly)) {
            music_pill_handle_overlay_motion(server->music_pill, lx, ly);
            wlr_seat_pointer_clear_focus(server->seat);
            server_set_pointer_constraint(server, NULL);
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
            return;
        }
    }
    if (server->topbar) topbar_handle_motion(server->topbar, lx, ly);
    if (server->dock) {
        dock_handle_motion(server->dock, lx, ly);
        if (server->dock->is_dragging) {
            wlr_seat_pointer_clear_focus(server->seat);
            server_set_pointer_constraint(server, NULL);
            return;
        }
    }

    // Normal client surface focus & hit testing
    struct wlr_surface *surface = NULL;
    double sx, sy;
    aurade_view_t *view = server_view_at(server, lx, ly, &surface, &sx, &sy);

    // If mouse buttons are held down on a client surface (e.g. text selection or client drag),
    // maintain implicit pointer grab on that surface so dragging past window edges doesn't
    // lose focus or abort selection.
    if (server->pointer_buttons_forwarded != 0 &&
        server->seat->pointer_state.focused_surface != NULL) {
        if (surface == server->seat->pointer_state.focused_surface) {
            wlr_seat_pointer_notify_motion(server->seat, event->time_msec, sx, sy);
        } else {
            if (server->focused_view) {
                sx = lx - server->focused_view->x;
                sy = ly - server->focused_view->y;
            }
            wlr_seat_pointer_notify_motion(server->seat, event->time_msec, sx, sy);
        }
        return;
    }

    struct wlr_xdg_surface *xdg_surf = surface
        ? wlr_xdg_surface_try_from_wlr_surface(surface) : NULL;
    bool over_popup = xdg_surf && xdg_surf->role == WLR_XDG_SURFACE_ROLE_POPUP;

    // Check resize handles around window edges (unless hovering a popup menu)
    if (!over_popup) {
        uint32_t resize_edges = 0;
        aurade_view_t *resize_view = server_view_resize_at(server, lx, ly, &resize_edges);
        if (resize_view && resize_edges != 0 && (!view || view == resize_view)) {
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
                                   resize_cursor_name(resize_edges));
            wlr_seat_pointer_clear_focus(server->seat);
            server_set_pointer_constraint(server, NULL);
            return;
        }
    }

    if (!surface) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server->seat);
        server_set_pointer_constraint(server, NULL);
        (void)view;
    } else {
        if (server->seat->pointer_state.focused_surface != surface) {
            wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        }
        struct wlr_pointer_constraint_v1 *constraint =
            get_constraint_for_surface(server, surface);
        server_set_pointer_constraint(server, constraint);
        if (server->active_constraint &&
            server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            return;
        }
        wlr_seat_pointer_notify_motion(server->seat, event->time_msec, sx, sy);
    }
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    if (server->active_constraint &&
        server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        return;
    }
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    struct wlr_pointer_motion_event fake_event = {
        .pointer = event->pointer,
        .time_msec = event->time_msec,
        .delta_x = 0,
        .delta_y = 0,
        .unaccel_dx = 0,
        .unaccel_dy = 0,
    };
    cursor_motion(&server->cursor_motion, &fake_event);
}

// Animate the current overlay back to closed. Used by Escape, outside clicks,
// and by the opposite/toggle gesture while an overlay is already open.
static void gesture_spring_close(aurade_server_t *server) {
    if (server->gesture_mode == GESTURE_MODE_IDLE) return;
    if (server->gesture_animating) return;
    if (server->gesture_mode == GESTURE_MODE_WORKSPACE_CAROUSEL) return;

    double cur_p = server->gesture_progress;
    spring_simulation_start(&server->gesture_spring, &server->config.spring,
                            cur_p > 0.01 ? cur_p : 1.0, 0.0, 0.0);
    server->gesture_animating = true;
    server->gesture_in_progress = false;
}

static void close_overlay(aurade_server_t *server) {
    if (server->dock && dock_context_menu_is_open(server->dock)) {
        dock_close_context_menu(server->dock);
    }
    gesture_spring_close(server);
}

void server_close_overlay(aurade_server_t *server) {
    if (!server) return;
    close_overlay(server);
}

static void unfocus_view(aurade_server_t *server) {
    view_unfocus(server);
}

static inline uint32_t pointer_button_bit(uint32_t button) {
    if (button >= BTN_MOUSE && button < BTN_MOUSE + 32) {
        return (uint32_t)1 << (button - BTN_MOUSE);
    }
    return (uint32_t)1 << 31;
}

static void cursor_button(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    double lx = server->cursor->x;
    double ly = server->cursor->y;

    bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
    bool left = event->button == BTN_LEFT;
    uint32_t button_bit = pointer_button_bit(event->button);
    if (server->screenshot &&
        screenshot_handle_button(server->screenshot, lx, ly, event->button, pressed)) {
        if (pressed) server_dismiss_popups(server);
        return;
    }

    if (!pressed) {
        // Only the button that started a compositor grab can end it. Releasing
        // the right button while dragging with the left button, for example,
        // must not cancel the resize.
        if (server->grab_button == event->button ||
            (server->grab_button == 0 && server->cursor_mode != CURSOR_PASSTHROUGH)) {
            if (server->cursor_mode == CURSOR_RESIZE && server->grabbed_view) {
                if (server->grabbed_view->xdg_toplevel) {
                    wlr_xdg_toplevel_set_resizing(server->grabbed_view->xdg_toplevel, false);
                    wlr_xdg_toplevel_set_size(server->grabbed_view->xdg_toplevel,
                                              server->grabbed_view->width,
                                              server->grabbed_view->height);
                } else if (server->grabbed_view->xwayland_surface) {
                    wlr_xwayland_surface_configure(server->grabbed_view->xwayland_surface,
                                                  server->grabbed_view->x,
                                                  server->grabbed_view->y,
                                                  server->grabbed_view->width,
                                                  server->grabbed_view->height);
                }
            }
            server->cursor_mode = CURSOR_PASSTHROUGH;
            server->grabbed_view = NULL;
            server->grab_button = 0;
            if (server->dock) dock_update_visibility(server->dock);
        }

        // A press delivered to a client gets its matching release; a press
        // consumed by shell chrome does not.
        bool forwarded = (server->pointer_buttons_forwarded & button_bit) != 0;
        server->pointer_buttons_forwarded &= ~button_bit;

        if (left && server->launchpad && server->launchpad->active) {
            if (server->launchpad->is_dragging && server->launchpad->dragged_app) {
                if (server->dock && dock_contains_point(server->dock, lx, ly)) {
                    desktop_app_t *dapp = server->launchpad->dragged_app;
                    dock_pin_app(server->dock, dapp->name, dapp->exec, dapp->icon_name, dapp->icon_surface);
                    launchpad_cancel_drag(server->launchpad);
                    return;
                }
            }
            int res = launchpad_handle_release(server->launchpad, lx, ly);
            if (res == 2) {
                close_overlay(server);
                return;
            } else if (res == 1) {
                return;
            }
        }

        if (left && server->mission_control && server->mission_control->active) {
            mission_control_handle_release(server->mission_control, lx, ly);
            schedule_all_outputs(server);
            return;
        }

        // Control Center sliders commit their final value on release.
        if (left && server->control_center && server->control_center->active) {
            control_center_handle_release(server->control_center, lx, ly);
            return;
        }

        // Dock drag-to-reorder or icon activation commits on release.
        if (left && server->dock) {
            if (dock_handle_release(server->dock, lx, ly)) {
                return;
            }
        }

        if (forwarded) {
            wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                           event->button, event->state);
            wlr_seat_pointer_notify_frame(server->seat);
        }
        return;
    }

    // Music pill expanded dynamic island consumes clicks
    if (server->music_pill && music_pill_is_expanded(server->music_pill)) {
        if (music_pill_overlay_owns(server->music_pill, lx, ly)) {
            if (left) {
                music_pill_handle_overlay_click(server->music_pill, lx, ly);
            }
            server_dismiss_popups(server);
            wlr_seat_pointer_clear_focus(server->seat);
            return;
        } else if (left) {
            music_pill_collapse(server->music_pill);
        }
    }

    // Notifications popups consume clicks first.
    if (server->notifications && notifications_pointer_owns(server->notifications, lx, ly)) {
        if (left) {
            notifications_handle_click(server->notifications, lx, ly);
        }
        server_dismiss_popups(server);
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    // Dock context menu consumes clicks while open.
    if (server->dock && dock_context_menu_is_open(server->dock)) {
        if (left) {
            if (dock_handle_context_menu_click(server->dock, lx, ly)) {
                server_dismiss_popups(server);
                wlr_seat_pointer_clear_focus(server->seat);
                return;
            }
        }
        dock_close_context_menu(server->dock);
    }

    // Pressed — the Apple menu is modal and consumes the click. A secondary
    // click dismisses it without activating a row.
    if (server->topbar && topbar_power_menu_is_open(server->topbar)) {
        if (left) {
            topbar_handle_power_menu_click(server->topbar, lx, ly);
        } else {
            topbar_close_power_menu(server->topbar);
        }
        server_dismiss_popups(server);
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    // Pressed — overlays first. Left click operates the overlay; any other
    // button dismisses it instead of toggling a control by accident.
    if (server->launchpad && server->launchpad->active) {
        if (server->dock && dock_pointer_owns(server->dock, lx, ly)) {
            if (left) {
                if (dock_handle_press(server->dock, lx, ly)) {
                    dock_handle_release(server->dock, lx, ly);
                    close_overlay(server);
                }
            } else {
                dock_handle_context_click(server->dock, lx, ly);
            }
            server_dismiss_popups(server);
            wlr_seat_pointer_clear_focus(server->seat);
            return;
        }

        if (left) {
            if (!launchpad_handle_press(server->launchpad, lx, ly)) close_overlay(server);
        } else {
            close_overlay(server);
        }
        server_dismiss_popups(server);
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }
    if (server->mission_control && server->mission_control->active) {
        if (left) {
            mission_control_handle_press(server->mission_control, lx, ly);
            schedule_all_outputs(server);
        } else {
            close_overlay(server);
        }
        server_dismiss_popups(server);
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }
    if (server->control_center && server->control_center->active) {
        if (left) {
            if (!control_center_handle_click(server->control_center, lx, ly)) close_overlay(server);
        } else {
            close_overlay(server);
        }
        server_dismiss_popups(server);
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    // The menu bar only responds to primary clicks. Never let a secondary
    // click fall through to a window hidden underneath the bar.
    if (server->topbar && ly <= server->topbar->height) {
        if (left) topbar_handle_click(server->topbar, lx, ly);
        server_dismiss_popups(server);
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    // Dock icons activate on press/drag and right-click opens the context menu.
    if (server->dock) {
        if (left) {
            if (dock_handle_press(server->dock, lx, ly)) {
                server_dismiss_popups(server);
                wlr_seat_pointer_clear_focus(server->seat);
                return;
            }
        } else {
            if (dock_handle_context_click(server->dock, lx, ly)) {
                server_dismiss_popups(server);
                wlr_seat_pointer_clear_focus(server->seat);
                return;
            }
            dock_handle_motion(server->dock, lx, ly);
            if (dock_pointer_owns(server->dock, lx, ly)) {
                server_dismiss_popups(server);
                wlr_seat_pointer_clear_focus(server->seat);
                return;
            }
        }
    }

    // Window click & resize hit test
    struct wlr_surface *surface = NULL;
    double sx, sy;
    aurade_view_t *view = server_view_at(server, lx, ly, &surface, &sx, &sy);

    struct wlr_xdg_surface *xdg_surf = surface
        ? wlr_xdg_surface_try_from_wlr_surface(surface) : NULL;
    bool over_popup = xdg_surf && xdg_surf->role == WLR_XDG_SURFACE_ROLE_POPUP;

    if (left && !over_popup) {
        uint32_t resize_edges = 0;
        aurade_view_t *resize_view = server_view_resize_at(server, lx, ly, &resize_edges);
        if (resize_view && resize_edges != 0 && (!view || view == resize_view)) {
            if (resize_view->anim_type != VIEW_ANIM_NONE) {
                view_anim_settle(resize_view);
            }
            view_focus(resize_view);
            if (resize_view->maximized) {
                view_toggle_maximize(resize_view);
            }
            server->grabbed_view = resize_view;
            server->cursor_mode = CURSOR_RESIZE;
            server->grab_button = event->button;
            server->resize_edges = resize_edges;
            server->grab_x = lx;
            server->grab_y = ly;
            server->grab_geobox.x = resize_view->x;
            server->grab_geobox.y = resize_view->y;
            server->grab_geobox.width = resize_view->width;
            server->grab_geobox.height = resize_view->height;
            if (resize_view->xdg_toplevel) {
                wlr_xdg_toplevel_set_resizing(resize_view->xdg_toplevel, true);
            }
            wlr_seat_pointer_clear_focus(server->seat);
            return;
        }
    }

    if (view) {
        if (view->anim_type != VIEW_ANIM_NONE) {
            view_anim_settle(view);
        }
        // Super + drag moves a window from anywhere in its surface: there is
        // no compositor-drawn titlebar to grab any more.
        if (left && !over_popup && super_is_held(server)) {
            view_focus(view);
            server->grabbed_view = view;
            server->cursor_mode = CURSOR_MOVE;
            server->grab_button = event->button;
            server->grab_x = lx - view->x;
            server->grab_y = ly - view->y;
            wlr_seat_pointer_clear_focus(server->seat);
            return;
        }

        view_focus(view);

        // Client content: make sure the seat's pointer focus is actually on
        // the surface under the cursor before delivering the press. Taps can
        // arrive without a preceding motion event, otherwise the click goes to
        // a stale surface (or nowhere at all).
        if (surface) {
            if (server->seat->pointer_state.focused_surface != surface) {
                wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
            }
            struct wlr_pointer_constraint_v1 *constraint =
                get_constraint_for_surface(server, surface);
            server_set_pointer_constraint(server, constraint);
            wlr_seat_pointer_notify_motion(server->seat, event->time_msec, sx, sy);
            wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                           event->button, event->state);
            wlr_seat_pointer_notify_frame(server->seat);
            server->pointer_buttons_forwarded |= button_bit;
        }
        return;
    }

    // Clicking the desktop deselects the foreground window, like Finder.
    server_dismiss_popups(server);
    unfocus_view(server);
    wlr_seat_pointer_clear_focus(server->seat);
    server_set_pointer_constraint(server, NULL);
}

aurade_view_t *server_view_at(aurade_server_t *server, double lx, double ly,
                              struct wlr_surface **surface, double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);

    // The dock renders into an oversized transparent buffer: it needs headroom
    // for magnified icons and tooltips. A hit on that empty headroom must not
    // eat a click meant for the window underneath (that was the dead strip
    // above the dock). Retry the hit test with just the dock hidden; the pill
    // itself keeps capturing the pointer via dock_handle_motion().
    if (node && server->dock && server->dock->scene_buffer &&
        node == &server->dock->scene_buffer->node &&
        !dock_pointer_owns(server->dock, lx, ly)) {
        wlr_scene_node_set_enabled(node, false);
        node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
        dock_apply_slide(server->dock);   // restores the real enabled state
    }

    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;

    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (scene_surface) {
        *surface = scene_surface->surface;
    }

    // Traverse up to find view
    struct wlr_scene_tree *tree = node->parent;
    while (tree && tree->node.parent != &server->scene->tree) {
        // Client popups live in their own topmost layer, outside the window
        // tree, and carry their owning view on the container node.
        if (tree->node.data) return tree->node.data;
        aurade_view_t *view;
        wl_list_for_each(view, &server->views, link) {
            if (view->scene_tree == tree) return view;
        }
        tree = tree->node.parent;
    }
    return NULL;
}

static void cursor_axis(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;

    double delta = event->delta;
    int32_t delta_discrete = event->delta_discrete;

    if (server->launchpad && server->launchpad->active) {
        launchpad_handle_axis(server->launchpad, event->orientation, delta, delta_discrete);
        return;
    }

    // libinput defaults to traditional scrolling. "Natural Direction" only
    // affects two-finger scrolling; touchpad gestures always follow the
    // physical direction of the fingers.
    if (server->config.natural_scroll) {
        delta = -delta;
        delta_discrete = -delta_discrete;
    }

    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
                                 delta, delta_discrete, event->source,
                                 event->relative_direction);
}


static void cursor_frame(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_server_t *server = wl_container_of(listener, server, cursor_frame);
    if (!server->seat) return;
    // Flush the pointer events emitted for this input frame. Without this,
    // clients can keep a stale hover/button state, which makes taps feel
    // unreliable after the pointer has crossed shell chrome.
    wlr_seat_pointer_notify_frame(server->seat);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused = server->seat->pointer_state.focused_client;
    if (focused && focused == event->seat_client) {
        if (!event->surface) {
            wlr_cursor_set_surface(server->cursor, NULL, 0, 0);
        } else {
            // Force server's cursor theme (Aura-Circle circular dot) so client-side
            // fallback cursors can never override the system cursor theme.
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        }
    }
}

static void seat_request_set_shape(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, request_set_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    struct wlr_seat_client *focused = server->seat->pointer_state.focused_client;
    if (focused && focused == event->seat_client) {
        const char *name = wlr_cursor_shape_v1_name(event->shape);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name ? name : "default");
    }
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void handle_request_activate(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, request_activate);
    struct wlr_xdg_activation_v1_request_activate_event *event = data;

    struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(event->surface);
    if (!xdg_surface || xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;

    aurade_view_t *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->xdg_toplevel == xdg_surface->toplevel) {
            // Focus stealing prevention:
            // Do not switch workspace or steal focus from an active user session.
            if (!server->focused_view) {
                if (view->workspace == server->current_workspace) {
                    view_restore(view);
                }
            } else if (server->focused_view == view) {
                wlr_scene_node_raise_to_top(&view->scene_tree->node);
            } else {
                // Background window requested activation: mark urgency
                view->demands_attention = true;
                if (server->dock) dock_refresh(server);
            }
            break;
        }
    }
}

// Each keyboard gets its own listeners. Attaching one shared listener to every
// keyboard is unsafe: when a device is unplugged wlroots asserts that the
// events.key listener list is empty (wlr_keyboard_finish).
typedef struct aurade_keyboard {
    struct aurade_server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener key;
    struct wl_listener modifiers;
    struct wl_listener destroy;
} aurade_keyboard_t;

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_keyboard_t *kbd = wl_container_of(listener, kbd, destroy);
    wl_list_remove(&kbd->key.link);
    wl_list_remove(&kbd->modifiers.link);
    wl_list_remove(&kbd->destroy.link);
    free(kbd);
}

static void keyboard_send_modifiers(aurade_server_t *server, struct wlr_keyboard *keyboard) {
    if (!server || !keyboard) return;
    // With multiple keyboards (internal + input-remapper + USB) the seat must
    // follow the keyboard that is actually being typed on.
    wlr_seat_set_keyboard(server->seat, keyboard);
    wlr_seat_keyboard_notify_modifiers(server->seat, &keyboard->modifiers);
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_keyboard_t *kbd = wl_container_of(listener, kbd, modifiers);
    keyboard_send_modifiers(kbd->server, kbd->wlr_keyboard);
}

// xkbcommon layouts contain one group per layout ("us,ua" = two groups).
// Switch groups and tell the seat, which makes Caps Lock/Shift and the active
// layout visible to Wayland clients.
static void keyboard_cycle_layout(aurade_server_t *server, struct wlr_keyboard *keyboard) {
    if (!server || !keyboard || !keyboard->xkb_state || !keyboard->keymap) return;

    xkb_layout_index_t layouts = xkb_keymap_num_layouts(keyboard->keymap);
    if (layouts <= 1) return;

    xkb_layout_index_t group = xkb_state_serialize_layout(keyboard->xkb_state,
                                                          XKB_STATE_LAYOUT_EFFECTIVE);
    group = (group + 1) % layouts;

    xkb_mod_mask_t depressed = xkb_state_serialize_mods(keyboard->xkb_state,
                                                        XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t latched = xkb_state_serialize_mods(keyboard->xkb_state,
                                                      XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t locked = xkb_state_serialize_mods(keyboard->xkb_state,
                                                     XKB_STATE_MODS_LOCKED);

    xkb_state_update_mask(keyboard->xkb_state, depressed, latched, locked, 0, 0, group);

    keyboard->modifiers.depressed = depressed;
    keyboard->modifiers.latched = latched;
    keyboard->modifiers.locked = locked;
    keyboard->modifiers.group = group;

    keyboard_send_modifiers(server, keyboard);

    const char *name = xkb_keymap_layout_get_name(keyboard->keymap, group);
    fprintf(stderr, "[AuraDE] Keyboard layout -> %s\n", name ? name : "?");
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct wlr_keyboard_key_event *event = data;
    aurade_keyboard_t *kbd = wl_container_of(listener, kbd, key);
    aurade_server_t *server = kbd->server;
    struct wlr_keyboard *keyboard = kbd->wlr_keyboard;
    if (!keyboard) return;

    // Keep the seat bound to the keyboard receiving events. Otherwise a
    // hot-plugged virtual keyboard can leave clients with a stale modifier
    // state (Shift/Caps Lock never reaching the focused surface).
    wlr_seat_set_keyboard(server->seat, keyboard);

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->xkb_state, keycode, &syms);

    // Shift changes the reported sym (Shift+1 is exclam), so use the level-0
    // symbol for the Super+Shift+1..4 "move window to workspace" shortcuts.
    const xkb_keysym_t *base_syms = NULL;
    int n_base_syms = 0;
    if (keyboard->keymap) {
        xkb_layout_index_t group = xkb_state_serialize_layout(keyboard->xkb_state,
                                                              XKB_STATE_LAYOUT_EFFECTIVE);
        n_base_syms = xkb_keymap_key_get_syms_by_level(keyboard->keymap, keycode,
                                                       group, 0, &base_syms);
    }

    bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    bool super = xkb_state_mod_name_is_active(keyboard->xkb_state, XKB_MOD_NAME_LOGO,
                                              XKB_STATE_MODS_EFFECTIVE) > 0;
    bool shift = xkb_state_mod_name_is_active(keyboard->xkb_state, XKB_MOD_NAME_SHIFT,
                                              XKB_STATE_MODS_EFFECTIVE) > 0;

    for (int i = 0; i < nsyms; i++) {
        xkb_keysym_t sym = syms[i];

        // A screengrab selection is modal: it owns Escape and swallows
        // keystrokes so the focused application cannot react while the user is
        // drawing the capture rectangle.
        if (server->screenshot &&
            screenshot_handle_key(server->screenshot, sym, pressed)) {
            return;
        }

        // Control Center gets text input and keys (Esc, Enter, Wi-Fi password) first.
        if (server->control_center && control_center_is_open(server->control_center)) {
            if (control_center_handle_key(server->control_center, sym, pressed)) {
                return;
            }
        }

        // Launchpad gets text input first.
        if (server->launchpad && server->launchpad->active) {
            if (launchpad_handle_key(server->launchpad, sym, pressed)) {
                return;
            }
        }

        if (!pressed) continue;

        // Hardware multimedia keys (Volume & Brightness)
        if (sym == XKB_KEY_XF86AudioRaiseVolume) {
            server_adjust_volume(server, 0.05);
            return;
        }
        if (sym == XKB_KEY_XF86AudioLowerVolume) {
            server_adjust_volume(server, -0.05);
            return;
        }
        if (sym == XKB_KEY_XF86AudioMute) {
            server_toggle_mute(server);
            return;
        }
        if (sym == XKB_KEY_XF86AudioMicMute) {
            int rc = system("pactl set-source-mute @DEFAULT_SOURCE@ toggle 2>/dev/null || wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle 2>/dev/null");
            (void)rc;
            return;
        }
        if (sym == XKB_KEY_XF86MonBrightnessUp) {
            server_adjust_brightness(server, 0.05);
            return;
        }
        if (sym == XKB_KEY_XF86MonBrightnessDown) {
            server_adjust_brightness(server, -0.05);
            return;
        }

        // Print / Sys_Req key handling:
        // If recording is active, pressing Print stops recording gracefully.
        // Otherwise, opens the interactive Screenshot & Screen Recording suite
        // (or direct region selection if Shift is held).
        if (sym == XKB_KEY_Print || sym == XKB_KEY_Sys_Req) {
            if (server->screenshot) {
                if (screenshot_is_recording(server->screenshot)) {
                    screenshot_stop_recording(server->screenshot);
                } else if (shift) {
                    screenshot_begin_region(server->screenshot);
                } else {
                    screenshot_begin(server->screenshot);
                }
            }
            return;
        }

        // Escape dismisses the power menu before anything else.
        if (sym == XKB_KEY_Escape && topbar_power_menu_is_open(server->topbar)) {
            topbar_close_power_menu(server->topbar);
            return;
        }

        // Escape dismisses dock context menu.
        if (sym == XKB_KEY_Escape && server->dock && dock_context_menu_is_open(server->dock)) {
            dock_close_context_menu(server->dock);
            return;
        }

        // Escape dismisses any open overlay (Mission Control, Launchpad, ...).
        if (sym == XKB_KEY_Escape && server->gesture_mode != GESTURE_MODE_IDLE) {
            close_overlay(server);
            return;
        }

        // Global macOS-style shortcuts.
        if (super) {
            if (shift && (sym == XKB_KEY_s || sym == XKB_KEY_S)) {
                if (server->screenshot) screenshot_begin(server->screenshot);
                return;
            }
            if (shift && (sym == XKB_KEY_r || sym == XKB_KEY_R)) {
                if (server->screenshot) screenshot_begin_video(server->screenshot);
                return;
            }
            if (shift && (sym == XKB_KEY_5 || (n_base_syms > 0 && base_syms[0] == XKB_KEY_5))) {
                if (server->screenshot) screenshot_begin(server->screenshot);
                return;
            }
            if (sym == XKB_KEY_q) {
                if (server->focused_view) view_close(server->focused_view);
                return;
            }
            if (sym == XKB_KEY_space) {
                // macOS/GNOME input-source switch. Cycles the XKB groups
                // loaded from the system layout, e.g. us -> ua -> us.
                keyboard_cycle_layout(server, keyboard);
                return;
            }
            if (n_base_syms > 0 && base_syms[0] >= XKB_KEY_1 && base_syms[0] <= XKB_KEY_4) {
                int ws = base_syms[0] - XKB_KEY_1;
                if (shift) {
                    if (server->focused_view) {
                        aurade_view_t *view = server->focused_view;
                        view->workspace = ws;
                        server_switch_workspace(server, ws, true);
                        wlr_scene_node_reparent(&view->scene_tree->node,
                                                server->workspace_trees[ws]);
                        server_update_workspace_positions(server);
                    }
                } else {
                    server_switch_workspace(server, ws, true);
                }
                return;
            }
            switch (sym) {
                case XKB_KEY_Return:
                case XKB_KEY_KP_Enter:
                    app_launch_command("foot");
                    return;
                case XKB_KEY_q:
                    if (server->focused_view) { view_close(server->focused_view); return; }
                    break;
                case XKB_KEY_m:
                    if (server->focused_view) { view_minimize(server->focused_view); return; }
                    break;
                case XKB_KEY_f:
                case XKB_KEY_Up:
                    if (server->focused_view) { view_toggle_maximize(server->focused_view); return; }
                    break;
                case XKB_KEY_Left:
                    if (server->focused_view) { view_tile(server->focused_view, true); return; }
                    break;
                case XKB_KEY_Right:
                    if (server->focused_view) { view_tile(server->focused_view, false); return; }
                    break;
                case XKB_KEY_F1:
                    server_adjust_brightness(server, -0.05);
                    return;
                case XKB_KEY_F2:
                    server_adjust_brightness(server, 0.05);
                    return;
                case XKB_KEY_F10:
                    server_toggle_mute(server);
                    return;
                case XKB_KEY_F11:
                case XKB_KEY_minus:
                    server_adjust_volume(server, -0.05);
                    return;
                case XKB_KEY_F12:
                case XKB_KEY_equal:
                case XKB_KEY_plus:
                    server_adjust_volume(server, 0.05);
                    return;
                case XKB_KEY_Tab:
                    // Cycle focus through the visible windows.
                    if (!wl_list_empty(&server->views)) {
                        aurade_view_t *first = wl_container_of(server->views.next, first, link);
                        aurade_view_t *next = first;
                        if (server->focused_view) {
                            aurade_view_t *cur = server->focused_view;
                            next = wl_container_of(cur->link.next, next, link);
                            if (&next->link == &server->views) next = first;
                        }
                        if (!next->minimized) view_focus(next);
                    }
                    return;
                default:
                    break;
            }
        }

        if (sym == XKB_KEY_F1) {
            server_adjust_brightness(server, -0.05);
            return;
        }
        if (sym == XKB_KEY_F2) {
            server_adjust_brightness(server, 0.05);
            return;
        }
        if (sym == XKB_KEY_F3) {
            if (server->gesture_mode == GESTURE_MODE_MISSION_CONTROL) {
                close_overlay(server);
            } else {
                server_handle_gesture_begin(server, ACTION_MISSION_CONTROL);
                spring_simulation_start(&server->gesture_spring, &server->config.spring,
                                        0.0, 1.0, 0.0);
                server->gesture_animating = true;
                server->gesture_in_progress = false;
                schedule_all_outputs(server);
            }
            return;
        }
        if (sym == XKB_KEY_F4) {
            server_handle_gesture_begin(server, ACTION_LAUNCHPAD);
            server_handle_gesture_update(server, 0, 0, 0.30);
            server_handle_gesture_end(server, false, 0, 0);
            return;
        }
        if (sym == XKB_KEY_F10) {
            server_toggle_mute(server);
            return;
        }
        if (sym == XKB_KEY_F11) {
            server_adjust_volume(server, -0.05);
            return;
        }
        if (sym == XKB_KEY_F12) {
            server_adjust_volume(server, 0.05);
            return;
        }
    }

    wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode, event->state);
}

// -------------------------------------------------------------
// Keyboard layout detection
// -------------------------------------------------------------

static bool read_vconsole_value(const char *key, char *out, size_t size) {
    FILE *f = fopen("/etc/vconsole.conf", "r");
    if (!f) return false;

    char line[512];
    bool found = false;
    size_t key_len = strlen(key);

    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (strncmp(s, key, key_len) != 0 || s[key_len] != '=') continue;

        s += key_len + 1;
        char *end = s + strlen(s);
        while (end > s && (end[-1] == '\n' || end[-1] == '\r')) *--end = '\0';
        if (*s == '"' || *s == '\'') {
            char quote = *s++;
            char *close = strchr(s, quote);
            if (close) *close = '\0';
        }
        snprintf(out, size, "%s", s);
        found = true;
        break;
    }

    fclose(f);
    return found;
}

static void resolve_keymap_string(const char *configured, const char *env_name,
                                  const char *vconsole_key, const char *fallback,
                                  char *out, size_t size) {
    if (configured && configured[0]) {
        snprintf(out, size, "%s", configured);
        return;
    }
    const char *env = getenv(env_name);
    if (env && env[0]) {
        snprintf(out, size, "%s", env);
        return;
    }
    if (vconsole_key && read_vconsole_value(vconsole_key, out, size) && out[0]) {
        return;
    }
    snprintf(out, size, "%s", fallback ? fallback : "");
}

// Build the keymap from the user's system layout. This is the difference
// between "us only" and the configured us+ua layouts, and it also makes the
// Ukrainian layout reachable with Super+Space.
static struct xkb_keymap *create_keymap(aurade_server_t *server) {
    char layout[128], variant[128], model[64], options[256];

    resolve_keymap_string(server->config.kb_layout, "XKB_DEFAULT_LAYOUT",
                          "XKBLAYOUT", "us", layout, sizeof(layout));
    resolve_keymap_string(server->config.kb_variant, "XKB_DEFAULT_VARIANT",
                          "XKBVARIANT", "", variant, sizeof(variant));
    resolve_keymap_string(server->config.kb_model, "XKB_DEFAULT_MODEL",
                          "XKBMODEL", "pc105", model, sizeof(model));
    resolve_keymap_string(server->config.kb_options, "XKB_DEFAULT_OPTIONS",
                          "XKBOPTIONS", "", options, sizeof(options));

    struct xkb_rule_names names = {
        .rules = "evdev",
        .model = model,
        .layout = layout,
        .variant = variant[0] ? variant : NULL,
        .options = options[0] ? options : NULL,
    };

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names,
                                                          XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        fprintf(stderr, "[AuraDE] Invalid keyboard layout \"%s\" (variant \"%s\"), using us\n",
                layout, variant);
        names.layout = "us";
        names.variant = NULL;
        names.options = NULL;
        keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }

    if (keymap) {
        fprintf(stderr, "[AuraDE] Keyboard keymap: layout=\"%s\" variant=\"%s\" model=\"%s\"\n",
                layout, variant, model);
    }
    xkb_context_unref(context);
    return keymap;
}

static void input_device_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_input_device_t *dev = wl_container_of(listener, dev, destroy);
    wl_list_remove(&dev->link);
    wl_list_remove(&dev->destroy.link);
    free(dev);
}

static void configure_libinput_device(struct wlr_input_device *device, aurade_server_t *server) {
    if (!wlr_input_device_is_libinput(device)) {
        return;
    }
    struct libinput_device *libinput_dev = wlr_libinput_get_device_handle(device);
    if (!libinput_dev) {
        return;
    }

    if (libinput_device_config_tap_get_finger_count(libinput_dev) > 0) {
        // Enable tap-to-click (1 finger = left, 2 fingers = right, 3 fingers = middle)
        libinput_device_config_tap_set_enabled(libinput_dev, LIBINPUT_CONFIG_TAP_ENABLED);
        libinput_device_config_tap_set_button_map(libinput_dev, LIBINPUT_CONFIG_TAP_MAP_LRM);

        // Tap-and-drag: enable double-tap dragging, but disable drag-lock so double-tap does not latch
        libinput_device_config_tap_set_drag_enabled(libinput_dev, LIBINPUT_CONFIG_DRAG_ENABLED);
        libinput_device_config_tap_set_drag_lock_enabled(libinput_dev, LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);

        // Click method: clickfinger enables 1-finger left click, 2-finger right click,
        // and 3-finger physical press = middle click across the entire touchpad surface.
        uint32_t click_methods = libinput_device_config_click_get_methods(libinput_dev);
        if (click_methods & LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER) {
            libinput_device_config_click_set_method(libinput_dev, LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER);
            libinput_device_config_click_set_clickfinger_button_map(libinput_dev, LIBINPUT_CONFIG_CLICKFINGER_MAP_LRM);
        }
    }

    // Enable middle button emulation (e.g. simultaneous left+right click = middle click)
    // for all devices that support it, including touchpads and mice
    if (libinput_device_config_middle_emulation_is_available(libinput_dev)) {
        libinput_device_config_middle_emulation_set_enabled(libinput_dev,
            LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED);
    }

    // Configure Disable-While-Typing (DWT) and Disable-While-Trackpointing (DWTP) for ANY device supporting them
    if (libinput_device_config_dwt_is_available(libinput_dev)) {
        enum libinput_config_dwt_state dwt = server->config.disable_while_typing ?
            LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED;
        libinput_device_config_dwt_set_enabled(libinput_dev, dwt);
    }
    if (libinput_device_config_dwtp_is_available(libinput_dev)) {
        enum libinput_config_dwtp_state dwtp = server->config.disable_while_typing ?
            LIBINPUT_CONFIG_DWTP_ENABLED : LIBINPUT_CONFIG_DWTP_DISABLED;
        libinput_device_config_dwtp_set_enabled(libinput_dev, dwtp);
    }

    fprintf(stderr, "[AuraDE] Configured libinput device '%s' (DWT=%s, DWTP=%s)\n",
            device->name ? device->name : "device",
            server->config.disable_while_typing ? "enabled" : "disabled",
            server->config.disable_while_typing ? "enabled" : "disabled");
}

void server_update_touchpad_settings(aurade_server_t *server) {
    if (!server) return;
    aurade_input_device_t *dev;
    wl_list_for_each(dev, &server->input_devices, link) {
        if (!dev->wlr_device) continue;
        if (dev->wlr_device->type == WLR_INPUT_DEVICE_POINTER) {
            configure_libinput_device(dev->wlr_device, server);
        }
    }
}

static void server_new_input(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    aurade_input_device_t *input_dev = calloc(1, sizeof(*input_dev));
    if (input_dev) {
        input_dev->server = server;
        input_dev->wlr_device = device;
        input_dev->destroy.notify = input_device_handle_destroy;
        wl_signal_add(&device->events.destroy, &input_dev->destroy);
        wl_list_insert(&server->input_devices, &input_dev->link);
    }

    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        struct wlr_keyboard *keyboard = wlr_keyboard_from_input_device(device);
        struct xkb_keymap *keymap = create_keymap(server);
        if (keymap) {
            wlr_keyboard_set_keymap(keyboard, keymap);
            xkb_keymap_unref(keymap);
        }

        wlr_keyboard_set_repeat_info(keyboard, 25, 400);
        wlr_seat_set_keyboard(server->seat, keyboard);

        aurade_keyboard_t *kbd = calloc(1, sizeof(*kbd));
        kbd->server = server;
        kbd->wlr_keyboard = keyboard;
        kbd->key.notify = keyboard_handle_key;
        wl_signal_add(&keyboard->events.key, &kbd->key);
        kbd->modifiers.notify = keyboard_handle_modifiers;
        wl_signal_add(&keyboard->events.modifiers, &kbd->modifiers);
        kbd->destroy.notify = keyboard_handle_destroy;
        wl_signal_add(&keyboard->base.events.destroy, &kbd->destroy);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
        configure_libinput_device(device, server);
        wlr_cursor_attach_input_device(server->cursor, device);
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (wlr_seat_get_keyboard(server->seat)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

// -------------------------------------------------------------
// Core Gesture Dispatcher & macOS Physics Integration
// -------------------------------------------------------------

void server_handle_gesture_begin(aurade_server_t *server, gesture_action_type_t action) {
    if (action == ACTION_NONE) return;

    // The screenshot region selector owns the screen until the user captures
    // or cancels; do not let a stray three-finger swipe move workspaces behind
    // the selection overlay.
    if (server->screenshot && screenshot_is_active(server->screenshot)) {
        return;
    }

    // Opening an overlay dismisses the Apple menu.
    if (server->topbar) topbar_close_power_menu(server->topbar);

    // An overlay is already visible. Direct manipulation closing:
    if (server->gesture_mode != GESTURE_MODE_IDLE) {
        gesture_mode_t cur = server->gesture_mode;

        // Swiping while Mission Control is open closes Mission Control!
        if (cur == GESTURE_MODE_MISSION_CONTROL && (action == ACTION_APP_EXPOSE || action == ACTION_MISSION_CONTROL)) {
            server->gesture_in_progress = true;
            server->gesture_animating = false;
            server->gesture_is_closing = true;
            server->gesture_start_progress = server->gesture_progress > 0.0 ? server->gesture_progress : 1.0;
            return;
        }

        // Launchpad gestures: pinching out closes, swiping left/right switches pages
        if (cur == GESTURE_MODE_LAUNCHPAD) {
            if (action == ACTION_LAUNCHPAD) {
                server->gesture_in_progress = true;
                server->gesture_animating = false;
                server->gesture_is_closing = true;
                server->gesture_start_progress = server->gesture_progress > 0.0 ? server->gesture_progress : 1.0;
                return;
            } else if (action == ACTION_WORKSPACE_NEXT) {
                if (server->launchpad && server->launchpad->current_page < server->launchpad->total_pages - 1) {
                    launchpad_switch_page(server->launchpad, server->launchpad->current_page + 1, true);
                }
                return;
            } else if (action == ACTION_WORKSPACE_PREV) {
                if (server->launchpad && server->launchpad->current_page > 0) {
                    launchpad_switch_page(server->launchpad, server->launchpad->current_page - 1, true);
                }
                return;
            }
        }

        bool toggle =
            (cur == GESTURE_MODE_CONTROL_CENTER && action == ACTION_CONTROL_CENTER) ||
            (cur == GESTURE_MODE_SHOW_DESKTOP   && action == ACTION_SHOW_DESKTOP);

        if (toggle) {
            if (!server->gesture_animating) {
                fprintf(stderr, "[AuraDE] Gesture: closing overlay (mode=%d)\n", cur);
                gesture_spring_close(server);
            }
        }
        return;
    }

    server->gesture_in_progress = true;
    server->gesture_progress = 0.0;
    server->gesture_animating = false;
    server->gesture_is_closing = false;
    server->gesture_start_progress = 0.0;

    switch (action) {
        case ACTION_MISSION_CONTROL:
        case ACTION_APP_EXPOSE:
            server->gesture_mode = GESTURE_MODE_MISSION_CONTROL;
            mission_control_begin(server->mission_control);
            fprintf(stderr, "[AuraDE] Gesture: Mission Control BEGIN\n");
            break;
        case ACTION_WORKSPACE_NEXT:
        case ACTION_WORKSPACE_PREV:
            // A previous swipe may still be settling. Freeze it at its current
            // visual position so the new gesture continues the carousel from
            // there instead of fighting the old spring animation.
            if (server->workspace_animating) {
                server->workspace_animating = false;

                // Re-anchor the continuous carousel position to the space at or
                // before it. Shifting both values by the same whole screens
                // leaves every scene node exactly where it is visually, but
                // makes the new gesture relative to the nearest space.
                int screen_w = server->screen_width > 0 ? server->screen_width : 1920;
                double p = server->current_workspace -
                           server->workspace_offset / (double)screen_w;
                int anchor = (int)round(p);
                if (anchor < 0) anchor = 0;
                int num_ws = server->num_workspaces > 0 ? server->num_workspaces : DEFAULT_WORKSPACES;
                if (anchor >= num_ws) anchor = num_ws - 1;
                if (anchor != server->current_workspace) {
                    server->workspace_offset +=
                        (double)(anchor - server->current_workspace) * screen_w;
                    server->current_workspace = anchor;
                    server_update_workspace_positions(server);
                }
            }
            server->gesture_base_offset = server->workspace_offset;
            server->gesture_mode = GESTURE_MODE_WORKSPACE_CAROUSEL;
            break;
        case ACTION_LAUNCHPAD:
            server->gesture_mode = GESTURE_MODE_LAUNCHPAD;
            launchpad_begin(server->launchpad);
            fprintf(stderr, "[AuraDE] Gesture: Launchpad BEGIN\n");
            break;
        case ACTION_SHOW_DESKTOP:
            server->gesture_mode = GESTURE_MODE_SHOW_DESKTOP;
            break;
        case ACTION_CONTROL_CENTER:
            server->gesture_mode = GESTURE_MODE_CONTROL_CENTER;
            control_center_begin(server->control_center);
            break;
        case ACTION_CUSTOM:
            break;
        default:
            server->gesture_mode = GESTURE_MODE_IDLE;
            break;
    }
}

void server_handle_gesture_update(aurade_server_t *server, double delta_x, double delta_y, double scale) {
    if (!server->gesture_in_progress) return;

    double sens = server->config.sensitivity;
    if (sens <= 0.0) sens = 1.0;

    switch (server->gesture_mode) {
        case GESTURE_MODE_MISSION_CONTROL: {
            double p;
            if (server->gesture_is_closing) {
                p = server->gesture_start_progress - (fabs(delta_y) * sens) / 380.0;
            } else {
                p = (fabs(delta_y) * sens) / 380.0;
            }
            if (p < 0.0) p = rubber_band_calculate(p, 0.25);
            else if (p > 1.0) p = 1.0 + rubber_band_calculate(p - 1.0, 0.25);
            server->gesture_progress = p;
            mission_control_update_progress(server->mission_control, p);
            break;
        }
        case GESTURE_MODE_WORKSPACE_CAROUSEL: {
            int screen_w = server->screen_width > 0 ? server->screen_width : 1920;

            // Reference pixels -> screen pixels, then the macOS-like gain that
            // lets a short swipe move a whole space.
            double ref_to_screen = (double)screen_w / GESTURE_REFERENCE_PX;
            double offset = server->gesture_base_offset +
                            delta_x * sens * WORKSPACE_GESTURE_GAIN * ref_to_screen;

            // Elastic resistance applies to the overshoot only. Compressing the
            // whole carousel offset (the old behaviour) made the first and last
            // workspaces lurch sideways while the fingers were still down.
            int num_ws = server->num_workspaces > 0 ? server->num_workspaces : DEFAULT_WORKSPACES;
            double max_offset = (double)server->current_workspace * screen_w;
            double min_offset =
                (double)(server->current_workspace - (num_ws - 1)) * screen_w;
            double limit = server->config.rubber_band_limit;

            if (offset > max_offset) {
                offset = max_offset + rubber_band_calculate(offset - max_offset, limit);
            } else if (offset < min_offset) {
                offset = min_offset + rubber_band_calculate(offset - min_offset, limit);
            }

            server->workspace_offset = offset;
            server_update_workspace_positions(server);
            break;
        }
        case GESTURE_MODE_LAUNCHPAD: {
            double p;
            if (server->gesture_is_closing) {
                // Pinching OUT (scale > 1.0) reduces progress towards 0.0
                p = server->gesture_start_progress - (scale - 1.0) / 0.35;
            } else {
                // Pinching IN (scale < 1.0) increases progress towards 1.0
                p = (1.0 - scale) / 0.35;
            }
            if (p < 0.0) p = 0.0;
            if (p > 1.0) p = 1.0 + rubber_band_calculate(p - 1.0, 0.2);
            server->gesture_progress = p;
            launchpad_update_progress(server->launchpad, p);
            break;
        }
        case GESTURE_MODE_SHOW_DESKTOP: {
            double p = (scale - 1.0) / 0.35;
            if (p < 0.0) p = 0.0;
            if (p > 1.0) p = 1.0;
            server->gesture_progress = p;

            // Scatter windows radially away from screen center
            aurade_view_t *view;
            wl_list_for_each(view, &server->views, link) {
                if (view->workspace == server->current_workspace) {
                    double center_x = view->x + view->width / 2.0;
                    double center_y = view->y + view->height / 2.0;
                    double dir_x = (center_x >= server->screen_width / 2.0) ? 1.0 : -1.0;
                    double dir_y = (center_y >= server->screen_height / 2.0) ? 1.0 : -1.0;
                    double offset_x = dir_x * server->screen_width * 0.7 * p;
                    double offset_y = dir_y * server->screen_height * 0.7 * p;
                    wlr_scene_node_set_position(&view->scene_tree->node, view->x + offset_x, view->y + offset_y);
                }
            }
            break;
        }
        case GESTURE_MODE_CONTROL_CENTER: {
            double p = (-delta_x * sens) / 330.0;
            if (p < 0.0) p = 0.0;
            if (p > 1.0) p = 1.0;
            server->gesture_progress = p;
            control_center_update_progress(server->control_center, p);
            break;
        }
        default:
            break;
    }
    schedule_all_outputs(server);
}

void server_handle_gesture_end(aurade_server_t *server, bool cancelled, double velocity_x, double velocity_y) {
    if (!server->gesture_in_progress) return;
    server->gesture_in_progress = false;

    double snap_thresh = server->config.snap_threshold;
    double sens = server->config.sensitivity > 0.0 ? server->config.sensitivity : 1.0;

    // The configured flick speed is expressed in reference pixels per second.
    // Converting it to "progress per second" makes the same flick feel the same
    // for a 380 px overlay, the 330 px Control Center and the full-screen
    // workspace carousel. Before this, the value was read and then ignored:
    // every release above 266 px/s counted as a flick.
    double flick_px = server->config.flick_velocity_threshold;
    if (flick_px <= 1.0) flick_px = 700.0;
    double flick_norm = flick_px / GESTURE_FLICK_REFERENCE;

    if (server->gesture_mode == GESTURE_MODE_WORKSPACE_CAROUSEL) {
        int screen_w = server->screen_width > 0 ? server->screen_width : 1920;
        double ref_to_screen = (double)screen_w / GESTURE_REFERENCE_PX;
        double offset = server->workspace_offset;

        // Finger velocity -> carousel progress per second. The carousel moves
        // WORKSPACE_GESTURE_GAIN times faster than the fingers, so the flick
        // test has to use the amplified value.
        double ws_flick_px = flick_norm * GESTURE_REFERENCE_PX / WORKSPACE_GESTURE_GAIN;

        // The carousel can be grabbed mid-transition, so decide from its
        // continuous position rather than assuming offset is zero-relative.
        // floor(p) / ceil(p) are the two spaces the carousel is between.
        double p = server->current_workspace - offset / (double)screen_w;
        int lower = (int)floor(p);
        int upper = lower + 1;

        int target_workspace = server->current_workspace;
        if (cancelled) {
            // An aborted gesture (device lost, dropped events) must not finish
            // the transition by accident: spring back to the anchored space.
        } else if (velocity_x < -ws_flick_px) {
            // Flicking left advances to the space on the right.
            target_workspace = upper;
        } else if (velocity_x > ws_flick_px) {
            // Flicking right settles down onto the space on the left.
            target_workspace = lower;
        } else if (p >= server->current_workspace) {
            // Slow release: use the same threshold relative to the workspace
            // the gesture was anchored to.
            if (p - server->current_workspace >= snap_thresh) {
                target_workspace = server->current_workspace + 1;
            }
        } else {
            if (server->current_workspace - p >= snap_thresh) {
                target_workspace = server->current_workspace - 1;
            }
        }

        // A single gesture moves at most one space.
        if (target_workspace > server->current_workspace + 1) {
            target_workspace = server->current_workspace + 1;
        }
        if (target_workspace < server->current_workspace - 1) {
            target_workspace = server->current_workspace - 1;
        }
        if (target_workspace < 0) target_workspace = 0;
        int num_ws = server->num_workspaces > 0 ? server->num_workspaces : DEFAULT_WORKSPACES;
        if (target_workspace >= num_ws) target_workspace = num_ws - 1;

        int old_ws = server->current_workspace;
        int diff = target_workspace - server->current_workspace;
        double new_offset = offset + ((double)diff * screen_w);
        server->current_workspace = target_workspace;
        server->workspace_offset = new_offset;

        // Hand the spring the velocity the carousel actually had on screen.
        double offset_velocity = velocity_x * sens * WORKSPACE_GESTURE_GAIN * ref_to_screen;

        spring_simulation_start(&server->workspace_spring, &server->config.spring,
                                new_offset, 0.0, offset_velocity);
        server->workspace_animating = true;
        server->gesture_base_offset = 0.0;
        server->gesture_mode = GESTURE_MODE_IDLE;
        server_update_workspace_positions(server);
        if (server->dock) dock_update_visibility(server->dock);
        if (old_ws != target_workspace) {
            server_workspace_auto_focus(server);
        }
        return;
    }

    // Single-axis progress spring animations
    double cur_p = server->gesture_progress;
    double target_p = 0.0;
    double vel = 0.0;
    double completion_px = GESTURE_FLICK_REFERENCE;

    if (server->gesture_mode == GESTURE_MODE_MISSION_CONTROL) {
        completion_px = 380.0;
        vel = -velocity_y / completion_px;
    } else if (server->gesture_mode == GESTURE_MODE_CONTROL_CENTER) {
        completion_px = 330.0;
        vel = -velocity_x / completion_px;
    } else if (server->gesture_mode == GESTURE_MODE_LAUNCHPAD) {
        // Pinch velocity is folded into the progress already; a quick release
        // past the snap threshold is what opens or closes Launchpad.
        vel = 0.0;
    }

    if (!cancelled) {
        double flick_progress = flick_px / completion_px;

        if (server->gesture_is_closing) {
            bool flick_closed = (vel < -flick_progress * 0.35);
            bool flick_reopen = (vel > flick_progress * 0.5);
            if (flick_closed || cur_p <= 0.70) {
                target_p = 0.0;
            } else if (flick_reopen || cur_p > 0.85) {
                target_p = 1.0;
            } else {
                target_p = 0.0;
            }
        } else {
            bool flick_open = (vel > flick_progress * 0.4);
            bool flick_close = (vel < -flick_progress * 0.4);
            if (cur_p >= snap_thresh || flick_open) {
                target_p = 1.0;
            } else if (flick_close) {
                target_p = 0.0;
            } else {
                target_p = 0.0;
            }
        }
    } else {
        target_p = server->gesture_is_closing ? 1.0 : 0.0;
    }

    spring_simulation_start(&server->gesture_spring, &server->config.spring,
                            cur_p, target_p, vel);
    server->gesture_animating = true;
    schedule_all_outputs(server);
}

// -------------------------------------------------------------
// Hardware Media Controls (Volume & Brightness)
// -------------------------------------------------------------

static bool run_command_capture(const char *cmd, char *out, size_t size) {
    if (!out || size < 2) return false;
    out[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    size_t n = fread(out, 1, size - 1, fp);
    out[n] = '\0';
    int status = pclose(fp);
    if (status != 0 || n == 0) return false;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' ' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
    return true;
}

static bool query_hardware_volume(double *out_vol, bool *out_muted) {
    char buf[256];
    if (run_command_capture("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", buf, sizeof(buf))) {
        double v = 0.0;
        if (sscanf(buf, "Volume: %lf", &v) == 1) {
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            if (out_vol) *out_vol = v;
            if (out_muted) *out_muted = (strstr(buf, "[MUTED]") != NULL);
            return true;
        }
    }
    if (run_command_capture("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", buf, sizeof(buf))) {
        char *pct = strchr(buf, '%');
        if (pct) {
            const char *p = pct;
            while (p > buf && (isdigit((unsigned char)p[-1]) || p[-1] == ' ')) p--;
            int val = atoi(p);
            if (val < 0) val = 0;
            if (val > 100) val = 100;
            if (out_vol) *out_vol = val / 100.0;
            bool muted = false;
            char mute_buf[128];
            if (run_command_capture("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null", mute_buf, sizeof(mute_buf))) {
                muted = (strstr(mute_buf, "yes") != NULL);
            }
            if (out_muted) *out_muted = muted;
            return true;
        }
    }
    return false;
}

static bool query_hardware_brightness(double *out_br) {
    DIR *dir = opendir("/sys/class/backlight");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char b_path[512], m_path[512];
            snprintf(b_path, sizeof(b_path), "/sys/class/backlight/%s/brightness", entry->d_name);
            snprintf(m_path, sizeof(m_path), "/sys/class/backlight/%s/max_brightness", entry->d_name);
            FILE *fb = fopen(b_path, "r");
            FILE *fm = fopen(m_path, "r");
            if (fb && fm) {
                long cur = 0, max = 0;
                if (fscanf(fb, "%ld", &cur) == 1 && fscanf(fm, "%ld", &max) == 1 && max > 0) {
                    fclose(fb);
                    fclose(fm);
                    closedir(dir);
                    double val = (double)cur / (double)max;
                    if (val < 0.01) val = 0.01;
                    if (val > 1.0) val = 1.0;
                    if (out_br) *out_br = val;
                    return true;
                }
            }
            if (fb) fclose(fb);
            if (fm) fclose(fm);
        }
        closedir(dir);
    }

    char buf[256];
    if (run_command_capture("brightnessctl -m 2>/dev/null", buf, sizeof(buf))) {
        char *pct = strchr(buf, '%');
        if (pct) {
            const char *p = pct;
            while (p > buf && (isdigit((unsigned char)p[-1]) || p[-1] == ' ')) p--;
            int val = atoi(p);
            if (val < 1) val = 1;
            if (val > 100) val = 100;
            if (out_br) *out_br = val / 100.0;
            return true;
        }
    }
    return false;
}

void server_adjust_volume(aurade_server_t *server, double delta) {
    if (!server) return;
    int step = (int)round(fabs(delta) * 100.0);
    if (step < 1) step = 5;

    char cmd[256];
    if (delta > 0) {
        snprintf(cmd, sizeof(cmd),
                 "wpctl set-mute @DEFAULT_AUDIO_SINK@ 0 2>/dev/null || pactl set-sink-mute @DEFAULT_SINK@ 0 2>/dev/null; "
                 "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ %d%%+ 2>/dev/null || pactl set-sink-volume @DEFAULT_SINK@ +%d%% 2>/dev/null",
                 step, step);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%- 2>/dev/null || pactl set-sink-volume @DEFAULT_SINK@ -%d%% 2>/dev/null",
                 step, step);
    }
    int rc = system(cmd);
    (void)rc;

    double vol = 0.0;
    bool muted = false;
    if (query_hardware_volume(&vol, &muted)) {
        if (server->osd) {
            osd_show_volume(server->osd, vol, muted);
        }
        if (server->control_center) {
            control_center_sync_audio(server->control_center, vol, muted);
        }
        schedule_all_outputs(server);
    }
}

void server_toggle_mute(aurade_server_t *server) {
    if (!server) return;
    const char *cmd = "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle 2>/dev/null || pactl set-sink-mute @DEFAULT_SINK@ toggle 2>/dev/null";
    int rc = system(cmd);
    (void)rc;

    double vol = 0.0;
    bool muted = false;
    if (query_hardware_volume(&vol, &muted)) {
        if (server->osd) {
            osd_show_volume(server->osd, vol, muted);
        }
        if (server->control_center) {
            control_center_sync_audio(server->control_center, vol, muted);
        }
        schedule_all_outputs(server);
    }
}

void server_adjust_brightness(aurade_server_t *server, double delta) {
    if (!server) return;
    int step = (int)round(fabs(delta) * 100.0);
    if (step < 1) step = 5;

    char cmd[128];
    if (delta > 0) {
        snprintf(cmd, sizeof(cmd), "brightnessctl set +%d%% 2>/dev/null", step);
    } else {
        snprintf(cmd, sizeof(cmd), "brightnessctl set %d%%- 2>/dev/null", step);
    }
    int rc = system(cmd);
    (void)rc;

    double br = 0.5;
    if (query_hardware_brightness(&br)) {
        if (server->osd) {
            osd_show_brightness(server->osd, br);
        }
        if (server->control_center) {
            control_center_sync_brightness(server->control_center, br);
        }
        schedule_all_outputs(server);
    }
}

void server_tick_animations(aurade_server_t *server, double dt) {
    // 1. Workspace Carousel Spring Animation
    if (server->workspace_animating) {
        server_step_workspace_spring(server, dt);
    }

    // 2. Overlay Gestures Spring Animation
    if (server->gesture_animating) {
        bool active = spring_simulation_step(&server->gesture_spring, dt);
        double p = server->gesture_spring.current_pos;
        server->gesture_progress = p;

        switch (server->gesture_mode) {
            case GESTURE_MODE_MISSION_CONTROL:
                mission_control_update_progress(server->mission_control, p);
                if (!active) {
                    server->gesture_animating = false;
                    server->gesture_is_closing = false;
                    mission_control_end(server->mission_control, p > 0.5);
                    if (p <= 0.5) {
                        server->gesture_mode = GESTURE_MODE_IDLE;
                        server->gesture_progress = 0.0;
                    } else {
                        server->gesture_progress = 1.0;
                    }
                }
                break;
            case GESTURE_MODE_LAUNCHPAD:
                launchpad_update_progress(server->launchpad, p);
                if (!active) {
                    server->gesture_animating = false;
                    server->gesture_is_closing = false;
                    launchpad_end(server->launchpad, p > 0.5);
                    if (p <= 0.5) {
                        server->gesture_mode = GESTURE_MODE_IDLE;
                        server->gesture_progress = 0.0;
                    } else {
                        server->gesture_progress = 1.0;
                    }
                }
                break;
            case GESTURE_MODE_SHOW_DESKTOP: {
                aurade_view_t *view;
                wl_list_for_each(view, &server->views, link) {
                    if (view->workspace == server->current_workspace) {
                        double cx = view->x + view->width / 2.0;
                        double cy = view->y + view->height / 2.0;
                        double dir_x = (cx >= server->screen_width / 2.0) ? 1.0 : -1.0;
                        double dir_y = (cy >= server->screen_height / 2.0) ? 1.0 : -1.0;
                        wlr_scene_node_set_position(&view->scene_tree->node,
                                                    view->x + dir_x * server->screen_width * 0.7 * p,
                                                    view->y + dir_y * server->screen_height * 0.7 * p);
                    }
                }
                if (!active) {
                    server->gesture_animating = false;
                    if (p <= 0.5) server->gesture_mode = GESTURE_MODE_IDLE;
                }
                break;
            }
            case GESTURE_MODE_CONTROL_CENTER:
                control_center_update_progress(server->control_center, p);
                if (!active) {
                    server->gesture_animating = false;
                    control_center_end(server->control_center, p > 0.5);
                    if (p <= 0.5) server->gesture_mode = GESTURE_MODE_IDLE;
                }
                break;
            default:
                server->gesture_animating = false;
                break;
        }
    }

    // 3. Launchpad Page Switch Spring Animation
    if (server->launchpad && server->launchpad->page_animating) {
        launchpad_tick(server->launchpad, dt);
    }

    // 4. OSD HUD Fadeout Animation
    if (server->osd && server->osd->visible) {
        osd_tick(server->osd, dt);
    }

    // 5. Notifications slide & fade animation
    if (server->notifications) {
        notifications_tick(server->notifications, dt);
    }

    // 6. Window View Animations (Map pop, Scale minimize/restore, Geometry maximize/tile)
    server_tick_view_animations(server, dt);

    // 7. Dock Icon Launch Bounce Physics
    if (server->dock && dock_is_bouncing(server->dock)) {
        dock_tick_bounce(server->dock, dt);
    }
}

static int handle_sigusr1(int sig, void *data) {
    (void)sig;
    aurade_server_t *server = data;
    if (server && server->screenshot) {
        screenshot_begin(server->screenshot);
    }
    return 0;
}

static int handle_sigusr2(int sig, void *data) {
    (void)sig;
    aurade_server_t *server = data;
    if (server && server->screenshot) {
        if (screenshot_is_recording(server->screenshot)) {
            screenshot_stop_recording(server->screenshot);
        } else {
            screenshot_begin_video(server->screenshot);
        }
    }
    return 0;
}

// -------------------------------------------------------------
// Main Entry Point
// -------------------------------------------------------------

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("====================================================\n");
    printf("   AuraDE - Fluid macOS-Logic Gesture Compositor    \n");
    printf("====================================================\n");

    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGILL, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGPIPE, SIG_IGN);

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || access(runtime_dir, W_OK) != 0) {
        static char fallback[256];
        snprintf(fallback, sizeof(fallback), "/tmp/aurade-runtime-%d", getuid());
        mkdir(fallback, 0700);
        setenv("XDG_RUNTIME_DIR", fallback, 1);
    }

    aurade_server_t server = {0};
    config_load(&server.config, NULL);

    // Build + effective tuning in the session log. This is the quickest way to
    // tell whether the running compositor is the freshly built binary and what
    // values the gesture engine is actually using.
    fprintf(stderr,
            "[AuraDE] build %s %s | ref-px %dx%d gain %.1f | deadzone %.0f px "
            "snap %.2f flick %.0f px/s | dock autohide %s\n",
            __DATE__, __TIME__, 1920, 1080, WORKSPACE_GESTURE_GAIN,
            server.config.deadzone_px, server.config.snap_threshold,
            server.config.flick_velocity_threshold,
            server.config.dock_autohide ? "on" : "off");

    server.wl_display = wl_display_create();
    struct wl_event_loop *event_loop = wl_display_get_event_loop(server.wl_display);
    wl_event_loop_add_signal(event_loop, SIGUSR1, handle_sigusr1, &server);
    wl_event_loop_add_signal(event_loop, SIGUSR2, handle_sigusr2, &server);

    server.backend = wlr_backend_autocreate(event_loop, NULL);
    if (!server.backend) {
        fprintf(stderr, "Failed to create wlr_backend\n");
        return 1;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    wlr_renderer_init_wl_display(server.renderer, server.wl_display);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);

    server.compositor = wlr_compositor_create(server.wl_display, 5, server.renderer);
    wlr_subcompositor_create(server.wl_display);
    wlr_data_device_manager_create(server.wl_display);

    // Standard desktop protocols: screenshots/recording, clipboard managers,
    // primary selection (select-to-copy, middle-click paste), and presentation-time feedback.
    server.screencopy = wlr_screencopy_manager_v1_create(server.wl_display);
    server.data_control = wlr_data_control_manager_v1_create(server.wl_display);
    server.ext_data_control = wlr_ext_data_control_manager_v1_create(server.wl_display, 1);
    server.primary_selection_mgr = wlr_primary_selection_v1_device_manager_create(server.wl_display);
    server.presentation = wlr_presentation_create(server.wl_display, server.backend, 1);
    server.xdg_activation = wlr_xdg_activation_v1_create(server.wl_display);
    server.request_activate.notify = handle_request_activate;
    wl_signal_add(&server.xdg_activation->events.request_activate, &server.request_activate);

    // Gaming & relative pointer lock support (Minecraft Java/Bedrock, 3D FPS camera controls)
    server.relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(server.wl_display);
    server.pointer_constraints = wlr_pointer_constraints_v1_create(server.wl_display);
    server.active_constraint = NULL;
    server.new_pointer_constraint.notify = handle_new_pointer_constraint;
    wl_signal_add(&server.pointer_constraints->events.new_constraint, &server.new_pointer_constraint);
    server.constraint_destroy.notify = handle_constraint_destroy;
    wl_list_init(&server.constraint_destroy.link);
    server.constraint_set_region.notify = handle_constraint_set_region;
    wl_list_init(&server.constraint_set_region.link);

    // Initialize Scene Trees
    server.scene = wlr_scene_create();
    server.output_layout = wlr_output_layout_create(server.wl_display);
    server.xdg_output_manager = wlr_xdg_output_manager_v1_create(server.wl_display,
                                                                  server.output_layout);
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    server.wallpaper_tree = wlr_scene_tree_create(&server.scene->tree);
    server_init_workspaces(&server);
    server.shell_tree = wlr_scene_tree_create(&server.scene->tree);
    server.overlay_tree = wlr_scene_tree_create(&server.scene->tree);
    server.dock_tree = wlr_scene_tree_create(&server.scene->tree);
    // Client popups (context menus, dropdowns) sit above all desktop chrome.
    server.popup_tree = wlr_scene_tree_create(&server.scene->tree);

    wl_list_init(&server.views);
    wl_list_init(&server.outputs);
    wl_list_init(&server.input_devices);

    server_init_xdg_shell(&server);

    // Outputs listener
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    // Cursor and Seat
    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    const char *cursor_theme = getenv("XCURSOR_THEME");
    if (!cursor_theme || !cursor_theme[0]) {
        cursor_theme = "Aura-Circle";
        setenv("XCURSOR_THEME", "Aura-Circle", 1);
    }
    server.cursor_mgr = wlr_xcursor_manager_create(cursor_theme, 24);
    if (!server.cursor_mgr) {
        server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    }
    wlr_xcursor_manager_load(server.cursor_mgr, 1);
    wlr_xcursor_manager_load(server.cursor_mgr, 1.5);
    wlr_xcursor_manager_load(server.cursor_mgr, 2);
    wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");

    server.cursor_motion.notify = cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);

    server.cursor_motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);

    server.cursor_button.notify = cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);

    server.cursor_axis.notify = cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);

    server.cursor_frame.notify = cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    server.seat = wlr_seat_create(server.wl_display, "seat0");

    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);

    server.cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(server.wl_display, 1);
    server.request_set_shape.notify = seat_request_set_shape;
    wl_signal_add(&server.cursor_shape_mgr->events.request_set_shape, &server.request_set_shape);

    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

    server.request_set_primary_selection.notify = seat_request_set_primary_selection;
    wl_signal_add(&server.seat->events.request_set_primary_selection, &server.request_set_primary_selection);

    // Initialize Xwayland
    server_init_xwayland(&server);

    // Load Applications
    server.apps = apps_scanner_load();

    // Create Shell Elements
    server.wallpaper = wallpaper_create(&server);
    server.topbar = topbar_create(&server);
    server.dock = dock_create(&server);
    server.mission_control = mission_control_create(&server);
    server.launchpad = launchpad_create(&server);
    server.control_center = control_center_create(&server);
    server.screenshot = screenshot_create(&server);
    server.osd = osd_create(&server);
    server.notifications = notifications_create(&server);
    server.music_pill = music_pill_create(&server);

// Config reload watcher setup moved above

// After creating gesture engine
if (getenv("AURADE_NO_GESTURES")) {
    fprintf(stderr, "[AuraDE] Gesture engine disabled by AURADE_NO_GESTURES\n");
} else {
    server.gesture_engine = gesture_engine_create(&server, event_loop);
}

// Setup inotify for config hot‑reload
server.config_inotify_fd = inotify_init1(IN_NONBLOCK);
if (server.config_inotify_fd >= 0) {
    const char *cfg_path = config_get_default_path();
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s", cfg_path);
    char *slash = strrchr(dir_path, '/');
    if (slash) *slash = '\0';

    // Watch parent directory for replacement events (rename/os.replace, creation)
    inotify_add_watch(server.config_inotify_fd, dir_path, IN_MOVED_TO | IN_CLOSE_WRITE | IN_CREATE);
    // Watch the file directly for in-place modifications
    inotify_add_watch(server.config_inotify_fd, cfg_path, IN_MODIFY | IN_ATTRIB);

    server.config_inotify_source = wl_event_loop_add_fd(event_loop,
        server.config_inotify_fd,
        WL_EVENT_READABLE,
        config_inotify_handler,
        &server);
    if (!server.config_inotify_source) {
        fprintf(stderr, "[AuraDE] Failed to register config inotify source\n");
    }
} else {
    fprintf(stderr, "[AuraDE] inotify_init failed, config hot‑reload disabled\n");
}

    if (!wlr_backend_start(server.backend)) {
        fprintf(stderr, "Failed to start backend\n");
        return 1;
    }

    server.clock_timer = wl_event_loop_add_timer(event_loop, clock_tick, &server);
    if (server.clock_timer) {
        wl_event_source_timer_update(server.clock_timer, 1000);
    }

    // Wayland socket setup
    const char *socket_name = wl_display_add_socket_auto(server.wl_display);
    if (!socket_name) {
        fprintf(stderr, "Failed to create Wayland socket\n");
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket_name, 1);
    setenv("XDG_CURRENT_DESKTOP", "AuraDE", 1);
    system("dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP 2>/dev/null &");
    system("systemctl --user import-environment WAYLAND_DISPLAY XDG_CURRENT_DESKTOP 2>/dev/null &");
    printf("[AuraDE] Running on WAYLAND_DISPLAY=%s\n", socket_name);
    printf("[AuraDE] macOS Gestures Active: Swipe Up (Mission Control), Down (Exposé), Horizontal (Spaces), Pinch In (Launchpad), Spread (Show Desktop)\n");

    wl_display_run(server.wl_display);

    // Cleanup
    server_finish_xwayland(&server);
    if (server.notifications) notifications_destroy(server.notifications);
    if (server.osd) osd_destroy(server.osd);
    screenshot_destroy(server.screenshot);
    gesture_engine_destroy(server.gesture_engine);
    apps_scanner_free(server.apps);
    wl_display_destroy_clients(server.wl_display);
    wl_display_destroy(server.wl_display);
    return 0;
}
