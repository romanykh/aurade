#include "xdg_shell.h"
#include "../render/cairo_buffer.h"
#include "../shell/topbar.h"
#include "../shell/dock.h"
#include "../shell/mission_control.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/types.h>


#include "../shell/launchpad.h"

#include <wlr/types/wlr_subcompositor.h>

static void view_apply_geometry(aurade_view_t *view);
static void view_apply_decoration_request(aurade_view_t *view);

struct view_scale_data {
    aurade_view_t *view;
    double scale_x;
    double scale_y;
};

static void view_scale_buffer_iterator(struct wlr_scene_buffer *scene_buffer,
                                       int sx, int sy, void *user_data) {
    (void)sx; (void)sy;
    struct view_scale_data *data = user_data;
    if (!scene_buffer) return;

    aurade_view_t *view = data->view;
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    bool is_root_xdg = (view && view->xdg_toplevel && view->xdg_toplevel->base &&
                        scene_surface && scene_surface->surface == view->xdg_toplevel->base->surface);

    bool is_scaled = (data->scale_x > 0.0 && data->scale_y > 0.0 &&
                      (fabs(data->scale_x - 1.0) >= 0.001 || fabs(data->scale_y - 1.0) >= 0.001));

    if (is_root_xdg) {
        struct wlr_box geo = view->xdg_toplevel->base->geometry;
        if (geo.width <= 0 || geo.height <= 0) {
            geo.x = 0;
            geo.y = 0;
            geo.width = view->width > 0 ? view->width : scene_surface->surface->current.width;
            geo.height = view->height > 0 ? view->height : scene_surface->surface->current.height;
        }

        if (is_scaled) {
            struct wlr_fbox src_box = {
                .x = (double)geo.x,
                .y = (double)geo.y,
                .width = (double)geo.width,
                .height = (double)geo.height,
            };
            wlr_scene_buffer_set_source_box(scene_buffer, &src_box);
            int dest_w = (int)round(geo.width * data->scale_x);
            int dest_h = (int)round(geo.height * data->scale_y);
            if (dest_w < 1) dest_w = 1;
            if (dest_h < 1) dest_h = 1;
            wlr_scene_buffer_set_dest_size(scene_buffer, dest_w, dest_h);
            wlr_scene_buffer_set_filter_mode(scene_buffer, WLR_SCALE_FILTER_BILINEAR);
        } else {
            wlr_scene_buffer_set_source_box(scene_buffer, NULL);
            int orig_w = scene_surface->surface->current.width;
            int orig_h = scene_surface->surface->current.height;
            wlr_scene_buffer_set_dest_size(scene_buffer, orig_w, orig_h);
        }
        return;
    }

    int orig_w = 0;
    int orig_h = 0;

    if (scene_surface && scene_surface->surface) {
        orig_w = scene_surface->surface->current.width;
        orig_h = scene_surface->surface->current.height;

        // If this surface is a Wayland subsurface, scale its parent scene tree node position
        struct wlr_subsurface *subsurface = wlr_subsurface_try_from_wlr_surface(scene_surface->surface);
        if (subsurface && scene_buffer->node.parent) {
            struct wlr_scene_tree *sub_tree = scene_buffer->node.parent;
            int sub_x = subsurface->current.x;
            int sub_y = subsurface->current.y;
            if (is_scaled) {
                sub_x = (int)round(sub_x * data->scale_x);
                sub_y = (int)round(sub_y * data->scale_y);
            }
            if (sub_tree->node.x != sub_x || sub_tree->node.y != sub_y) {
                wlr_scene_node_set_position(&sub_tree->node, sub_x, sub_y);
            }
        }
    }

    if (orig_w <= 0 || orig_h <= 0) {
        if (scene_buffer->buffer) {
            orig_w = scene_buffer->buffer->width;
            orig_h = scene_buffer->buffer->height;
        } else {
            orig_w = scene_buffer->WLR_PRIVATE.buffer_width;
            orig_h = scene_buffer->WLR_PRIVATE.buffer_height;
        }
    }

    if (orig_w <= 0 || orig_h <= 0) return;

    if (!is_scaled) {
        wlr_scene_buffer_set_source_box(scene_buffer, NULL);
        int target_w = (scene_surface && scene_surface->surface) ? orig_w : 0;
        int target_h = (scene_surface && scene_surface->surface) ? orig_h : 0;
        if (scene_buffer->dst_width != target_w || scene_buffer->dst_height != target_h) {
            wlr_scene_buffer_set_dest_size(scene_buffer, target_w, target_h);
        }
    } else {
        int dest_w = (int)round(orig_w * data->scale_x);
        int dest_h = (int)round(orig_h * data->scale_y);
        if (dest_w < 1) dest_w = 1;
        if (dest_h < 1) dest_h = 1;
        if (scene_buffer->dst_width != dest_w || scene_buffer->dst_height != dest_h) {
            wlr_scene_buffer_set_dest_size(scene_buffer, dest_w, dest_h);
            wlr_scene_buffer_set_filter_mode(scene_buffer, WLR_SCALE_FILTER_BILINEAR);
        }
    }
}

void view_set_scale(aurade_view_t *view, double scale_x, double scale_y) {
    if (!view || !view->scene_tree) return;
    view->current_scale = scale_x;

    if (view->content_tree) {
        if (view->content_tree->node.x != 0 || view->content_tree->node.y != 0) {
            wlr_scene_node_set_position(&view->content_tree->node, 0, 0);
        }
    }

    struct view_scale_data data = {
        .view = view,
        .scale_x = scale_x,
        .scale_y = scale_y,
    };
    wlr_scene_node_for_each_buffer(&view->scene_tree->node, view_scale_buffer_iterator, &data);
}

struct view_opacity_data {
    float opacity;
};

static void view_opacity_buffer_iterator(struct wlr_scene_buffer *scene_buffer,
                                         int sx, int sy, void *user_data) {
    (void)sx; (void)sy;
    struct view_opacity_data *data = user_data;
    if (!scene_buffer) return;
    wlr_scene_buffer_set_opacity(scene_buffer, data->opacity);
}

void view_set_opacity(aurade_view_t *view, float opacity) {
    if (!view || !view->scene_tree) return;
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    view->anim_alpha = opacity;
    struct view_opacity_data data = { .opacity = opacity };
    wlr_scene_node_for_each_buffer(&view->scene_tree->node, view_opacity_buffer_iterator, &data);
}

void view_anim_settle(aurade_view_t *view) {
    if (!view) return;
    view_anim_type_t prev = view->anim_type;
    view->anim_type = VIEW_ANIM_NONE;
    if (!view->scene_tree) return;

    if (view->y < TOPBAR_HEIGHT) view->y = TOPBAR_HEIGHT;

    if (prev == VIEW_ANIM_MINIMIZE) {
        view->minimized = true;
        wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    } else {
        wlr_scene_node_set_enabled(&view->scene_tree->node, !view->minimized);
    }
    view_set_scale(view, 1.0, 1.0);
    view_set_opacity(view, 1.0f);
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
}

void server_tick_view_animations(aurade_server_t *server, double dt) {
    if (!server) return;
    aurade_view_t *view, *tmp;
    wl_list_for_each_safe(view, tmp, &server->views, link) {
        if (view->anim_type == VIEW_ANIM_NONE) continue;
        if (!view->scene_tree) {
            view->anim_type = VIEW_ANIM_NONE;
            continue;
        }

        bool active = spring_simulation_step(&view->anim_spring, dt);
        double p = view->anim_spring.current_pos;

        switch (view->anim_type) {
            case VIEW_ANIM_MAP: {
                // p: 0.0 -> 1.0 with subtle natural overshoot (macOS window pop)
                double p_clamp = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
                double scale = 0.88 + 0.12 * p;
                if (scale < 0.1) scale = 0.1;
                double opacity = p_clamp;

                int render_x = (int)round(view->x + (view->width * (1.0 - scale)) / 2.0);
                int render_y = (int)round(view->y + (view->height * (1.0 - scale)) / 2.0);

                wlr_scene_node_set_position(&view->scene_tree->node, render_x, render_y);
                view_set_scale(view, scale, scale);
                view_set_opacity(view, (float)opacity);

                if (!active) {
                    view_anim_settle(view);
                }
                break;
            }
            case VIEW_ANIM_MINIMIZE: {
                // p: 0.0 -> 1.0 (macOS Scale effect towards Dock icon)
                double p_clamp = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
                double scale = 1.0 - 0.85 * p_clamp;
                double opacity = 1.0 - p_clamp;

                double start_cx = view->anim_start_x + view->anim_start_w / 2.0;
                double start_cy = view->anim_start_y + view->anim_start_h / 2.0;
                double target_cx = view->anim_target_x;
                double target_cy = view->anim_target_y;

                double cur_cx = start_cx + (target_cx - start_cx) * p_clamp;
                double cur_cy = start_cy + (target_cy - start_cy) * p_clamp;

                double cur_w = view->anim_start_w * scale;
                double cur_h = view->anim_start_h * scale;

                int render_x = (int)round(cur_cx - cur_w / 2.0);
                int render_y = (int)round(cur_cy - cur_h / 2.0);

                wlr_scene_node_set_position(&view->scene_tree->node, render_x, render_y);
                view_set_scale(view, scale, scale);
                view_set_opacity(view, (float)opacity);

                if (!active) {
                    view->minimized = true;
                    wlr_scene_node_set_enabled(&view->scene_tree->node, false);
                    view_set_scale(view, 1.0, 1.0);
                    view_set_opacity(view, 1.0f);
                    view->anim_type = VIEW_ANIM_NONE;
                }
                break;
            }
            case VIEW_ANIM_RESTORE: {
                // p: 0.0 -> 1.0 (macOS Scale effect from Dock icon)
                double p_clamp = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
                double scale = 0.15 + 0.85 * p_clamp;
                double opacity = p_clamp;

                double start_cx = view->anim_start_x;
                double start_cy = view->anim_start_y;
                double target_cx = view->x + view->width / 2.0;
                double target_cy = view->y + view->height / 2.0;

                double cur_cx = start_cx + (target_cx - start_cx) * p_clamp;
                double cur_cy = start_cy + (target_cy - start_cy) * p_clamp;

                double cur_w = view->width * scale;
                double cur_h = view->height * scale;

                int render_x = (int)round(cur_cx - cur_w / 2.0);
                int render_y = (int)round(cur_cy - cur_h / 2.0);

                wlr_scene_node_set_position(&view->scene_tree->node, render_x, render_y);
                view_set_scale(view, scale, scale);
                view_set_opacity(view, (float)opacity);

                if (!active) {
                    view_anim_settle(view);
                }
                break;
            }
            case VIEW_ANIM_GEOMETRY: {
                // p: 0.0 -> 1.0 (smooth rectangle interpolation)
                double p_clamp = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
                double cur_x = view->anim_start_x + (view->anim_target_x - view->anim_start_x) * p_clamp;
                double cur_y = view->anim_start_y + (view->anim_target_y - view->anim_start_y) * p_clamp;
                double cur_w = view->anim_start_w + (view->anim_target_w - view->anim_start_w) * p_clamp;
                double cur_h = view->anim_start_h + (view->anim_target_h - view->anim_start_h) * p_clamp;

                if (cur_w < 50.0) cur_w = 50.0;
                if (cur_h < 50.0) cur_h = 50.0;
                if (cur_y < TOPBAR_HEIGHT) cur_y = TOPBAR_HEIGHT;

                double scale_x = cur_w / (double)(view->width > 0 ? view->width : 1);
                double scale_y = cur_h / (double)(view->height > 0 ? view->height : 1);

                wlr_scene_node_set_position(&view->scene_tree->node, (int)round(cur_x), (int)round(cur_y));
                view_set_scale(view, scale_x, scale_y);

                if (!active) {
                    view_anim_settle(view);
                }
                break;
            }
            default:
                break;
        }
    }
}

void view_focus(aurade_view_t *view) {
    if (!view) return;
    struct aurade_server *server = view->server;
    struct wlr_seat *seat = server->seat;

    if (view->xdg_toplevel) {
        if (!view->xdg_toplevel->base->initialized) return;

        aurade_view_t *previous = server->focused_view;
        if (previous && previous != view) {
            if (previous->xdg_toplevel && previous->xdg_toplevel->base->initialized) {
                wlr_xdg_toplevel_set_activated(previous->xdg_toplevel, false);
            } else if (previous->xwayland_surface) {
                wlr_xwayland_surface_activate(previous->xwayland_surface, false);
            }
        }
        server->focused_view = view;
        if (view->demands_attention) {
            view->demands_attention = false;
            dock_refresh(server);
        }

        wlr_scene_node_raise_to_top(&view->scene_tree->node);
        wl_list_remove(&view->link);
        wl_list_insert(&server->views, &view->link);

        wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);

        struct wlr_surface *surface = view->xdg_toplevel->base->surface;
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
        if (keyboard && surface) {
            wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
                                           keyboard->num_keycodes, &keyboard->modifiers);
        }

        const char *app_name = view->xdg_toplevel->title;
        if (!app_name || !*app_name) app_name = view->xdg_toplevel->app_id;
        if (server->topbar) {
            topbar_set_active_app(server->topbar, app_name);
        }
    } else if (view->xwayland_surface) {
        struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
        if (xsurface->override_redirect &&
            !wlr_xwayland_surface_override_redirect_wants_focus(xsurface)) {
            return;
        }

        aurade_view_t *previous = server->focused_view;
        if (previous && previous != view) {
            if (previous->xdg_toplevel && previous->xdg_toplevel->base->initialized) {
                wlr_xdg_toplevel_set_activated(previous->xdg_toplevel, false);
            } else if (previous->xwayland_surface) {
                wlr_xwayland_surface_activate(previous->xwayland_surface, false);
            }
        }
        server->focused_view = view;
        if (view->demands_attention) {
            view->demands_attention = false;
            dock_refresh(server);
        }

        if (view->scene_tree) {
            wlr_scene_node_raise_to_top(&view->scene_tree->node);
        }
        if (!xsurface->override_redirect && !wl_list_empty(&view->link)) {
            wl_list_remove(&view->link);
            wl_list_insert(&server->views, &view->link);
        }

        wlr_xwayland_surface_activate(xsurface, true);
        wlr_xwayland_surface_restack(xsurface, NULL, XCB_STACK_MODE_ABOVE);

        if (xsurface->surface) {
            struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
            if (keyboard) {
                wlr_seat_keyboard_notify_enter(seat, xsurface->surface, keyboard->keycodes,
                                               keyboard->num_keycodes, &keyboard->modifiers);
            }
        }

        const char *app_name = xsurface->title;
        if (!app_name || !*app_name) app_name = xsurface->class;
        if (server->topbar) {
            topbar_set_active_app(server->topbar, app_name ? app_name : "X11 Application");
        }
    }
}

void view_unfocus(aurade_server_t *server) {
    if (!server) return;
    aurade_view_t *view = server->focused_view;
    if (view) {
        if (view->xdg_toplevel && view->xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_activated(view->xdg_toplevel, false);
        } else if (view->xwayland_surface) {
            wlr_xwayland_surface_activate(view->xwayland_surface, false);
        }
        server->focused_view = NULL;
    }
    wlr_seat_keyboard_clear_focus(server->seat);
    wlr_seat_pointer_clear_focus(server->seat);
    if (server->topbar) {
        topbar_set_active_app(server->topbar, "Finder");
    }
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void view_apply_geometry(aurade_view_t *view) {
    if (!view) return;
    // Never hand wlroots a negative or degenerate configure: clients that
    // were restored from an uninitialised (zero) geometry would otherwise
    // assert inside wlr_xdg_toplevel_set_size.
    if (view->width < 240) view->width = 240;
    if (view->height < 180) view->height = 180;
    if (view->y < TOPBAR_HEIGHT) view->y = TOPBAR_HEIGHT;
    if (view->scene_tree) {
        wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    }
    if (view->xdg_toplevel) {
        if (view->xdg_toplevel->base->initialized) {
            view->pending_configure = false;
            view->last_configure_ms = now_ms();
            wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->width, view->height);
        } else {
            // Applied by xdg_toplevel_map once the initial configure is acked.
            view->pending_configure = true;
        }
    } else if (view->xwayland_surface) {
        wlr_xwayland_surface_configure(view->xwayland_surface,
                                       view->x, view->y, view->width, view->height);
    }
}

void view_tile(aurade_view_t *view, bool left_half) {
    if (!view) return;
    int screen_w = view->server->screen_width > 0 ? view->server->screen_width : 1920;
    int screen_h = view->server->screen_height > 0 ? view->server->screen_height : 1080;
    int top_margin = TOPBAR_HEIGHT;
    bool hide_dock = view->server->config.dock_autohide ||
                     view->server->config.dock_intellihide ||
                     !view->server->config.dock_enabled;
    int bottom_margin = hide_dock ? 0 : 96;

    int old_x = view->x;
    int old_y = view->y;
    int old_w = view->width;
    int old_h = view->height;

    int target_x = left_half ? 0 : screen_w / 2;
    int target_y = top_margin;
    int target_w = screen_w / 2;
    int target_h = screen_h - top_margin - bottom_margin;

    view->maximized = false;
    if (view->xdg_toplevel && view->xdg_toplevel->base->initialized) {
        wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, false);
    } else if (view->xwayland_surface) {
        wlr_xwayland_surface_set_maximized(view->xwayland_surface, false, false);
    }
    view->x = target_x;
    view->y = target_y;
    view->width = target_w;
    view->height = target_h;

    view_apply_geometry(view);

    if (old_w > 0 && old_h > 0 && (old_x != target_x || old_y != target_y || old_w != target_w || old_h != target_h)) {
        view->anim_type = VIEW_ANIM_GEOMETRY;
        view->anim_start_x = old_x;
        view->anim_start_y = old_y;
        view->anim_start_w = old_w;
        view->anim_start_h = old_h;
        view->anim_target_x = target_x;
        view->anim_target_y = target_y;
        view->anim_target_w = target_w;
        view->anim_target_h = target_h;

        spring_config_t geom_cfg = {
            .mass = 1.0,
            .stiffness = 340.0,
            .damping = 0.88,
            .velocity_threshold = 0.01,
            .position_threshold = 0.001,
        };
        spring_simulation_start(&view->anim_spring, &geom_cfg, 0.0, 1.0, 0.0);

        wlr_scene_node_set_position(&view->scene_tree->node, old_x, old_y);
        view_set_scale(view, (double)old_w / target_w, (double)old_h / target_h);
        schedule_all_outputs(view->server);
    }

    dock_refresh(view->server);
}

void view_toggle_maximize(aurade_view_t *view) {
    if (!view) return;
    struct aurade_server *server = view->server;
    int screen_w = server->screen_width > 0 ? server->screen_width : 1920;
    int screen_h = server->screen_height > 0 ? server->screen_height : 1080;
    bool hide_dock = server->config.dock_autohide ||
                     server->config.dock_intellihide ||
                     !server->config.dock_enabled;
    int bottom_margin = hide_dock ? 0 : 96;

    int old_x = view->x;
    int old_y = view->y;
    int old_w = view->width;
    int old_h = view->height;

    int target_x, target_y, target_w, target_h;

    if (view->maximized) {
        view->maximized = false;
        view->fullscreen = false;
        if (view->prev_w >= 240 && view->prev_h >= 180) {
            target_x = view->prev_x;
            target_y = view->prev_y;
            target_w = view->prev_w;
            target_h = view->prev_h;
            if (target_y < TOPBAR_HEIGHT) target_y = TOPBAR_HEIGHT;
        } else {
            // The view was maximized before its first configure, so there is
            // no real previous geometry to restore. Use a centered window.
            target_w = screen_w * 3 / 4;
            target_h = (screen_h - TOPBAR_HEIGHT - bottom_margin) * 3 / 4;
            target_x = (screen_w - target_w) / 2;
            target_y = TOPBAR_HEIGHT + ((screen_h - bottom_margin - TOPBAR_HEIGHT) - target_h) / 2;
        }
    } else {
        view->prev_x = view->x;
        view->prev_y = view->y;
        view->prev_w = view->width;
        view->prev_h = view->height;
        view->maximized = true;

        // macOS windows maximise below the menu bar and above the Dock.
        target_x = 0;
        target_y = TOPBAR_HEIGHT;
        target_w = screen_w;
        target_h = screen_h - TOPBAR_HEIGHT - bottom_margin;
    }

    view->x = target_x;
    view->y = target_y;
    view->width = target_w;
    view->height = target_h;

    if (view->xdg_toplevel) {
        if (view->xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, view->maximized);
            wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, view->fullscreen);
        }
    } else if (view->xwayland_surface) {
        wlr_xwayland_surface_set_maximized(view->xwayland_surface, view->maximized, view->maximized);
        wlr_xwayland_surface_set_fullscreen(view->xwayland_surface, view->fullscreen);
    }
    view_apply_geometry(view);

    if (old_w > 0 && old_h > 0 && (old_x != target_x || old_y != target_y || old_w != target_w || old_h != target_h)) {
        view->anim_type = VIEW_ANIM_GEOMETRY;
        view->anim_start_x = old_x;
        view->anim_start_y = old_y;
        view->anim_start_w = old_w;
        view->anim_start_h = old_h;
        view->anim_target_x = target_x;
        view->anim_target_y = target_y;
        view->anim_target_w = target_w;
        view->anim_target_h = target_h;

        spring_config_t geom_cfg = {
            .mass = 1.0,
            .stiffness = 340.0,
            .damping = 0.88,
            .velocity_threshold = 0.01,
            .position_threshold = 0.001,
        };
        spring_simulation_start(&view->anim_spring, &geom_cfg, 0.0, 1.0, 0.0);

        wlr_scene_node_set_position(&view->scene_tree->node, old_x, old_y);
        view_set_scale(view, (double)old_w / target_w, (double)old_h / target_h);
        schedule_all_outputs(server);
    }

    dock_refresh(server);
}

void view_set_fullscreen(aurade_view_t *view, bool fullscreen) {
    if (!view) return;
    if (view->fullscreen == fullscreen) return;
    struct aurade_server *server = view->server;
    int screen_w = server->screen_width > 0 ? server->screen_width : 1920;
    int screen_h = server->screen_height > 0 ? server->screen_height : 1080;
    bool hide_dock = server->config.dock_autohide ||
                     server->config.dock_intellihide ||
                     !server->config.dock_enabled;
    int bottom_margin = hide_dock ? 0 : 96;

    if (fullscreen) {
        if (!view->fullscreen && !view->maximized) {
            view->prev_x = view->x;
            view->prev_y = view->y;
            view->prev_w = view->width;
            view->prev_h = view->height;
        }
        view->fullscreen = true;
        view->maximized = true;

        // "fullscreen size (except topbar, do it like maximized window)"
        view->x = 0;
        view->y = TOPBAR_HEIGHT;
        view->width = screen_w;
        view->height = screen_h - TOPBAR_HEIGHT - bottom_margin;
    } else {
        view->fullscreen = false;
        view->maximized = false;
        if (view->prev_w >= 240 && view->prev_h >= 180) {
            view->x = view->prev_x;
            view->y = view->prev_y;
            if (view->y < TOPBAR_HEIGHT) view->y = TOPBAR_HEIGHT;
            view->width = view->prev_w;
            view->height = view->prev_h;
        } else {
            view->width = screen_w * 3 / 4;
            view->height = (screen_h - TOPBAR_HEIGHT - bottom_margin) * 3 / 4;
            view->x = (screen_w - view->width) / 2;
            view->y = TOPBAR_HEIGHT + ((screen_h - bottom_margin - TOPBAR_HEIGHT) - view->height) / 2;
        }
    }

    if (view->xdg_toplevel) {
        if (view->xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, view->fullscreen);
            wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, view->maximized);
        }
    } else if (view->xwayland_surface) {
        wlr_xwayland_surface_set_fullscreen(view->xwayland_surface, view->fullscreen);
        wlr_xwayland_surface_set_maximized(view->xwayland_surface, view->maximized, view->maximized);
    }
    view_apply_geometry(view);
    dock_refresh(server);
}

void view_minimize(aurade_view_t *view) {
    if (!view || view->minimized) return;

    double dock_cx = 0, dock_cy = 0;
    const char *app_id = view_get_app_id(view);
    dock_get_item_center(view->server->dock, app_id, &dock_cx, &dock_cy);

    view->anim_type = VIEW_ANIM_MINIMIZE;
    view->anim_start_x = view->x;
    view->anim_start_y = view->y;
    view->anim_start_w = view->width;
    view->anim_start_h = view->height;
    view->anim_target_x = dock_cx;
    view->anim_target_y = dock_cy;

    spring_config_t min_cfg = {
        .mass = 1.0,
        .stiffness = 320.0,
        .damping = 0.90,
        .velocity_threshold = 0.01,
        .position_threshold = 0.001,
    };
    spring_simulation_start(&view->anim_spring, &min_cfg, 0.0, 1.0, 0.0);

    if (view->xwayland_surface) {
        wlr_xwayland_surface_set_minimized(view->xwayland_surface, true);
    }
    if (view->server->focused_view == view) {
        // Hand focus to the next visible window on this workspace.
        view->server->focused_view = NULL;
        aurade_view_t *other;
        bool found = false;
        wl_list_for_each(other, &view->server->views, link) {
            if (other != view && !other->minimized &&
                other->anim_type != VIEW_ANIM_MINIMIZE &&
                other->workspace == view->server->current_workspace) {
                view_focus(other);
                found = true;
                break;
            }
        }
        if (!found) {
            wlr_seat_keyboard_clear_focus(view->server->seat);
            if (view->server->topbar) {
                topbar_set_active_app(view->server->topbar, "Finder");
            }
        }
    }
    schedule_all_outputs(view->server);
    dock_refresh(view->server);
}

void view_restore(aurade_view_t *view) {
    if (!view) return;
    view->minimized = false;
    if (view->scene_tree) {
        wlr_scene_node_set_enabled(&view->scene_tree->node, true);
        wlr_scene_node_raise_to_top(&view->scene_tree->node);
    }
    if (view->xwayland_surface) {
        wlr_xwayland_surface_set_minimized(view->xwayland_surface, false);
    }
    if (view->decoration && view->decoration_pending &&
        view->xdg_toplevel && view->xdg_toplevel->base->initialized) {
        view_apply_decoration_request(view);
    }
    if (view->workspace != view->server->current_workspace) {
        server_switch_workspace(view->server, view->workspace, true);
    }

    double dock_cx = 0, dock_cy = 0;
    const char *app_id = view_get_app_id(view);
    dock_get_item_center(view->server->dock, app_id, &dock_cx, &dock_cy);

    view->anim_type = VIEW_ANIM_RESTORE;
    view->anim_start_x = dock_cx;
    view->anim_start_y = dock_cy;
    view->anim_target_x = view->x + view->width / 2.0;
    view->anim_target_y = view->y + view->height / 2.0;

    spring_config_t rest_cfg = {
        .mass = 1.0,
        .stiffness = 320.0,
        .damping = 0.88,
        .velocity_threshold = 0.01,
        .position_threshold = 0.001,
    };
    spring_simulation_start(&view->anim_spring, &rest_cfg, 0.0, 1.0, 0.0);

    double initial_scale = 0.15;
    double cur_w = view->width * initial_scale;
    double cur_h = view->height * initial_scale;
    wlr_scene_node_set_position(&view->scene_tree->node,
                                (int)round(dock_cx - cur_w / 2.0),
                                (int)round(dock_cy - cur_h / 2.0));
    view_set_scale(view, initial_scale, initial_scale);
    view_set_opacity(view, 0.0f);

    view_focus(view);
    schedule_all_outputs(view->server);
    dock_refresh(view->server);
}

void view_close(aurade_view_t *view) {
    if (!view) return;
    if (view->xdg_toplevel) {
        wlr_xdg_toplevel_send_close(view->xdg_toplevel);
    } else if (view->xwayland_surface) {
        wlr_xwayland_surface_close(view->xwayland_surface);
    }
}

static void xdg_surface_commit_debug(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, commit);
    struct wlr_xdg_surface *surface = view->xdg_toplevel->base;
    if (getenv("AURADE_DEBUG")) {
        fprintf(stderr, "[AURADE] commit surface=%p init=%d initial=%d mapped=%d buffer=%d geo=%dx%d view=%dx%d\n",
                (void *)surface, surface->initialized, surface->initial_commit,
                surface->surface->mapped,
                wlr_surface_has_buffer(surface->surface),
                surface->geometry.width, surface->geometry.height,
                view->width, view->height);
    }

    // wlroots 0.20 leaves the very first configure to the compositor: until we
    // send one the client cannot ack it and therefore never maps. Clients such
    // as foot that don't request maximise/activation never trigger a configure
    // on their own, so do it here for every new toplevel.
    if (surface->initial_commit) {
        if (view->maximized && view->width >= 240 && view->height >= 180) {
            wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, true);
            view->last_configure_ms = now_ms();
            wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->width, view->height);
        } else {
            wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
        }
        view->pending_configure = false;
        return;
    }

    if (!surface->initialized || view->pending_configure) return;
    if (!wlr_surface_has_buffer(surface->surface)) return;

    if (view->current_scale > 0.0 && fabs(view->current_scale - 1.0) >= 0.001) {
        view_set_scale(view, view->current_scale, view->current_scale);
    }


    // Ignore the frames a client commits while it is still reacting to a
    // configure we just sent (maximise, tile, restore); those carry the old
    // size and would otherwise be mistaken for a client-driven resize.
    uint64_t now = now_ms();
    if (view->last_configure_ms && now - view->last_configure_ms < 250) return;

    // The client owns its decorations and therefore its size. The window is
    // the xdg geometry (the visible area inside the surface, excluding the
    // client's shadow margins). Fall back to the surface for clients that
    // never send set_window_geometry.
    int width = 0, height = 0;
    struct wlr_box geo = surface->geometry;
    if (geo.width > 0 && geo.height > 0) {
        width = geo.width;
        height = geo.height;
    } else {
        width = surface->surface->current.width;
        height = surface->surface->current.height;
    }
    if (view->content_tree && (view->current_scale <= 0.0 || fabs(view->current_scale - 1.0) < 0.001)) {
        if (view->content_tree->node.x != 0 || view->content_tree->node.y != 0) {
            wlr_scene_node_set_position(&view->content_tree->node, 0, 0);
        }
    }
    if (width <= 0 || height <= 0) return;

    // Shadow margins can change even when the window size does not.

    if (width == view->width && height == view->height) return;

    // A client that takes control of its own size while maximized is no
    // longer maximized.
    if (view->maximized) {
        view->maximized = false;
        wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, false);
    }

    view->width = width;
    view->height = height;
    dock_refresh(view->server);
}

static void xdg_surface_configure_debug(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, configure);
    if (view->decoration && view->decoration_pending &&
        view->xdg_toplevel->base->initialized) {
        view_apply_decoration_request(view);
    }
    if (!getenv("AURADE_DEBUG")) return;
    fprintf(stderr, "[AURADE] configure surface=%p init=%d\n",
            (void *)view->xdg_toplevel->base->surface,
            view->xdg_toplevel->base->initialized);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, map);

    // A mapped toplevel is visible again. This also covers clients that
    // unmap/remap themselves (for example when they withdraw a window).
    view->minimized = false;
    wlr_scene_node_set_enabled(&view->scene_tree->node, true);

    if (view->workspace != view->server->current_workspace) {
        // A client asked to open on another workspace: show it.
        view->workspace = view->server->current_workspace;
        wlr_scene_node_reparent(&view->scene_tree->node,
                                view->server->workspace_trees[view->workspace]);
    }

    // Detect if client requested fullscreen, maximize, or is a picture/media viewer
    bool wants_fullscreen = view->xdg_toplevel->requested.fullscreen;
    bool wants_maximize = view->xdg_toplevel->requested.maximized;
    bool is_media_viewer = false;
    if (view->xdg_toplevel->title && (strcasestr(view->xdg_toplevel->title, "media viewer") ||
                                      strcasestr(view->xdg_toplevel->title, "image viewer") ||
                                      strcasestr(view->xdg_toplevel->title, "photo viewer"))) {
        is_media_viewer = true;
    }
    if (view->xdg_toplevel->app_id && (strcasestr(view->xdg_toplevel->app_id, "loupe") ||
                                       strcasestr(view->xdg_toplevel->app_id, "eog") ||
                                       strcasestr(view->xdg_toplevel->app_id, "gwenview") ||
                                       strcasestr(view->xdg_toplevel->app_id, "feh"))) {
        is_media_viewer = true;
    }

    if (wants_fullscreen) {
        view_set_fullscreen(view, true);
    } else if (is_media_viewer || wants_maximize) {
        if (!view->maximized) {
            view_toggle_maximize(view);
        }
    } else if (!view->maximized && !view->pending_configure) {
        struct wlr_box geobox = view->xdg_toplevel->base->geometry;
        struct wlr_surface *s = view->xdg_toplevel->base->surface;
        if (geobox.width > 0 && geobox.height > 0) {
            view->width = geobox.width;
            view->height = geobox.height;
        } else if (s->current.width > 0 && s->current.height > 0) {
            view->width = s->current.width;
            view->height = s->current.height;
        } else {
            view->width = 700;
            view->height = 500;
        }
        if (view->xdg_toplevel && !view->maximized && !view->fullscreen) {
            wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->width, view->height);
        }

        // Center window in available screen area (between topbar and dock)
        int screen_w = view->server->screen_width > 0 ? view->server->screen_width : 1920;
        int screen_h = view->server->screen_height > 0 ? view->server->screen_height : 1080;
        int top_margin = TOPBAR_HEIGHT;
        bool hide_dock = view->server->config.dock_autohide ||
                         view->server->config.dock_intellihide ||
                         !view->server->config.dock_enabled;
        int bottom_margin = hide_dock ? 0 : 96;

        int center_x = (screen_w - view->width) / 2;
        int center_y = top_margin + ((screen_h - top_margin - bottom_margin) - view->height) / 2;
        if (center_x < 0) center_x = 0;
        if (center_y < top_margin) center_y = top_margin;

        view->x = center_x;
        view->y = center_y;
    }

    if (view->y < TOPBAR_HEIGHT) view->y = TOPBAR_HEIGHT;

    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    view_focus(view);

    // Map pop animation (0.88 -> 1.0 with subtle spring overshoot)
    view->anim_type = VIEW_ANIM_MAP;
    spring_config_t map_cfg = {
        .mass = 1.0,
        .stiffness = 320.0,
        .damping = 0.86,
        .velocity_threshold = 0.01,
        .position_threshold = 0.001,
    };
    spring_simulation_start(&view->anim_spring, &map_cfg, 0.0, 1.0, 0.0);
    double initial_scale = 0.88;
    int render_x = (int)round(view->x + (view->width * (1.0 - initial_scale)) / 2.0);
    int render_y = (int)round(view->y + (view->height * (1.0 - initial_scale)) / 2.0);
    wlr_scene_node_set_position(&view->scene_tree->node, render_x, render_y);
    view_set_scale(view, initial_scale, initial_scale);
    view_set_opacity(view, 0.0f);

    if (view->server->dock) {
        const char *app_id = view_get_app_id(view);
        dock_stop_bounce(view->server->dock, app_id);
    }
    schedule_all_outputs(view->server);

    if (getenv("AURADE_DEBUG")) {
        struct wlr_box geo = view->xdg_toplevel->base->geometry;
        struct wlr_surface *s = view->xdg_toplevel->base->surface;
        fprintf(stderr, "[AURADE] map app_id=%s title=%s geom=%dx%d at %d,%d geo_off=%d,%d surface=%dx%d init=%d max=%d\n",
                view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "?",
                view->xdg_toplevel->title ? view->xdg_toplevel->title : "?",
                view->width, view->height, view->x, view->y,
                geo.x, geo.y, s->current.width, s->current.height,
                view->xdg_toplevel->base->initialized, view->maximized);
    }

    // Flush a maximize/fullscreen/tile requested before the initial configure was acked.
    if (view->fullscreen) {
        wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, true);
    }
    if (view->maximized) {
        wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, true);
    }
    if (view->pending_configure) {
        view_apply_geometry(view);
    }

    dock_refresh(view->server);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, unmap);

    // Unmap is the first half of a close (destroy follows), but some clients
    // withdraw a window and keep the toplevel alive. Hide the whole view so no
    // ghost window or invisible input region is left behind.
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);

    if (view->server->focused_view == view) {
        view->server->focused_view = NULL;
        wlr_seat_keyboard_clear_focus(view->server->seat);
        if (view->server->topbar) {
            topbar_set_active_app(view->server->topbar, "Finder");
        }
    }
    if (view->server->active_constraint &&
        view->xdg_toplevel && view->xdg_toplevel->base &&
        view->server->active_constraint->surface == view->xdg_toplevel->base->surface) {
        server_set_pointer_constraint(view->server, NULL);
    }
    dock_refresh(view->server);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, destroy);

    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->configure.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_minimize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->link);

    if (view->decoration) {
        wl_list_remove(&view->decoration_request_mode.link);
        wl_list_remove(&view->decoration_destroy.link);
        view->decoration = NULL;
    }

    if (view->server->grabbed_view == view) {
        view->server->grabbed_view = NULL;
        view->server->cursor_mode = CURSOR_PASSTHROUGH;
    }

    if (view->server->focused_view == view) {
        view->server->focused_view = NULL;
        wlr_seat_keyboard_clear_focus(view->server->seat);
        if (view->server->topbar) {
            topbar_set_active_app(view->server->topbar, "Finder");
        }
    }
    // Overviews hold raw pointers to windows: drop them before the view dies.
    if (view->server->mission_control) {
        mission_control_forget_view(view->server->mission_control, view);
    }

    // The scene tree is owned by the compositor, not by wlroots. Destroying
    // only the xdg surface leaves the tree alive after the app is gone.
    if (view->scene_tree) {
        wlr_scene_node_destroy(&view->scene_tree->node);
        view->scene_tree = NULL;
    }

    dock_refresh(view->server);
    free(view);
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, request_move);
    if (view->anim_type != VIEW_ANIM_NONE) view_anim_settle(view);
    aurade_server_t *server = view->server;

    if (view->maximized) {
        int screen_w = server->screen_width > 0 ? server->screen_width : 1920;
        int screen_h = server->screen_height > 0 ? server->screen_height : 1080;
        int restored_w = (view->prev_w >= 240 && view->prev_w < screen_w) ? view->prev_w : (screen_w * 3 / 4);
        int restored_h = (view->prev_h >= 180 && view->prev_h < screen_h) ? view->prev_h : ((screen_h - TOPBAR_HEIGHT - 96) * 3 / 4);

        double rel_x = (server->cursor->x - view->x) / (double)(view->width > 0 ? view->width : screen_w);
        if (rel_x < 0.1) rel_x = 0.1;
        if (rel_x > 0.9) rel_x = 0.9;

        int new_x = (int)(server->cursor->x - restored_w * rel_x);
        int new_y = (int)server->cursor->y - 15;
        if (new_y < TOPBAR_HEIGHT) new_y = TOPBAR_HEIGHT;

        view->maximized = false;
        view->fullscreen = false;
        view->x = new_x;
        view->y = new_y;
        view->width = restored_w;
        view->height = restored_h;

        if (view->xdg_toplevel && view->xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, false);
            wlr_xdg_toplevel_set_size(view->xdg_toplevel, restored_w, restored_h);
        } else if (view->xwayland_surface) {
            wlr_xwayland_surface_set_maximized(view->xwayland_surface, false, false);
            wlr_xwayland_surface_configure(view->xwayland_surface, new_x, new_y, restored_w, restored_h);
        }
        wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    }

    server->grabbed_view = view;
    server->cursor_mode = CURSOR_MOVE;
    server->grab_button = 0;
    server->grab_x = server->cursor->x - view->x;
    server->grab_y = server->cursor->y - view->y;
    wlr_seat_pointer_clear_focus(server->seat);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    aurade_view_t *view = wl_container_of(listener, view, request_resize);
    if (view->anim_type != VIEW_ANIM_NONE) view_anim_settle(view);
    aurade_server_t *server = view->server;

    server->grabbed_view = view;
    server->cursor_mode = CURSOR_RESIZE;
    server->grab_button = 0; // any mouse button release ends client resize
    server->resize_edges = event->edges;
    server->grab_x = server->cursor->x;
    server->grab_y = server->cursor->y;
    server->grab_geobox.x = view->x;
    server->grab_geobox.y = view->y;
    server->grab_geobox.width = view->width;
    server->grab_geobox.height = view->height;
    if (view->xdg_toplevel) {
        wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, true);
    }
    wlr_seat_pointer_clear_focus(server->seat);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, request_maximize);

    // wlroots emits the same event for set_maximized and unset_maximized.
    // GTK's own headerbar sends this when the green button is clicked, so
    // honour both directions instead of only ever maximizing.
    bool requested = view->xdg_toplevel->requested.maximized;
    if (requested != view->maximized) {
        view_toggle_maximize(view);
    }
}

static void xdg_toplevel_request_minimize(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, request_minimize);
    view_minimize(view);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, request_fullscreen);
    bool requested = view->xdg_toplevel->requested.fullscreen;
    if (requested != view->fullscreen) {
        view_set_fullscreen(view, requested);
    }
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, set_title);
    if (!view || !view->xdg_toplevel) return;

    if (view->server->focused_view == view && view->server->topbar) {
        const char *app_name = view->xdg_toplevel->title;
        if (!app_name || !*app_name) app_name = view->xdg_toplevel->app_id;
        topbar_set_active_app(view->server->topbar, app_name ? app_name : "Finder");
    }

    if (view->xdg_toplevel->title && (strcasestr(view->xdg_toplevel->title, "media viewer") ||
                                      strcasestr(view->xdg_toplevel->title, "image viewer") ||
                                      strcasestr(view->xdg_toplevel->title, "photo viewer"))) {
        if (!view->maximized && !view->fullscreen) {
            view_toggle_maximize(view);
        }
    }
}

static void view_apply_decoration_request(aurade_view_t *view) {
    if (!view || !view->decoration) return;

    // AuraDE does not draw window decorations: every client is expected to
    // decorate itself. Clients that cannot draw a frame end up undecorated.
    if (!view->xdg_toplevel->base->initialized) {
        view->decoration_pending = true;
        return;
    }
    view->decoration_pending = false;
    wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

static void xdg_decoration_request_mode(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view =
        wl_container_of(listener, view, decoration_request_mode);
    view_apply_decoration_request(view);
}

static void xdg_decoration_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, decoration_destroy);
    wl_list_remove(&view->decoration_request_mode.link);
    wl_list_remove(&view->decoration_destroy.link);
    view->decoration = NULL;
    view->decoration_pending = false;
}

static void view_attach_decoration(aurade_view_t *view,
                                   struct wlr_xdg_toplevel_decoration_v1 *decoration) {
    if (!view || !decoration) return;
    if (view->decoration == decoration) {
        view_apply_decoration_request(view);
        return;
    }

    if (view->decoration) {
        wl_list_remove(&view->decoration_request_mode.link);
        wl_list_remove(&view->decoration_destroy.link);
        view->decoration = NULL;
    }

    view->decoration = decoration;
    view->decoration_request_mode.notify = xdg_decoration_request_mode;
    wl_signal_add(&decoration->events.request_mode,
                  &view->decoration_request_mode);
    view->decoration_destroy.notify = xdg_decoration_destroy;
    wl_signal_add(&decoration->events.destroy, &view->decoration_destroy);

    view_apply_decoration_request(view);
}

static void xdg_decoration_handle_new(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
    aurade_server_t *server =
        wl_container_of(listener, server, new_xdg_decoration);

    aurade_view_t *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->xdg_toplevel == decoration->toplevel) {
            view_attach_decoration(view, decoration);
            return;
        }
    }

    // xdg-decoration objects are normally created after the toplevel, but
    // tolerate the opposite order: server_new_xdg_toplevel() attaches any
    // decoration that already exists for the new toplevel.
    if (getenv("AURADE_DEBUG")) {
        fprintf(stderr, "[AURADE] decoration before view app_id=%s\n",
                decoration->toplevel->app_id ? decoration->toplevel->app_id : "?");
    }
}

// wlroots 0.20 no longer adds xdg-popups to the scene graph from
// wlr_scene_xdg_surface_create(): each popup (context menu, dropdown,
// tooltip, and nested submenus) has to be inserted explicitly. Popups live in
// a dedicated topmost scene layer so a menu is not hidden by another window,
// the menu bar or the Dock. Their position is kept relative to the owning
// window (and to the parent popup for submenus).
typedef struct aurade_popup {
    aurade_server_t *server;
    aurade_view_t *view;
    struct wlr_xdg_popup *xdg_popup;
    struct wlr_scene_tree *container;    // positioned relative to parent
    struct wlr_scene_tree *surface_tree; // xdg surface and its submenus
    struct wl_listener commit;
    struct wl_listener reposition;
    struct wl_listener destroy;
} aurade_popup_t;

static void popup_unconstrain(aurade_popup_t *popup) {
    aurade_view_t *view = popup->view;
    if (!view || !view->xdg_toplevel ||
        !view->xdg_toplevel->base->initialized) {
        return;
    }

    // The output box expressed in the coordinate system of the toplevel
    // surface, so the popup is flipped/shifted to stay on screen.
    int content_x = 0, content_y = 0;
    wlr_scene_node_coords(&view->content_tree->node, &content_x, &content_y);
    struct wlr_xdg_surface *toplevel = view->xdg_toplevel->base;
    int screen_w = view->server->screen_width > 0 ? view->server->screen_width : 1920;
    int screen_h = view->server->screen_height > 0 ? view->server->screen_height : 1080;
    struct wlr_box output_box = {
        .x = -content_x + toplevel->geometry.x,
        .y = -content_y + toplevel->geometry.y,
        .width = screen_w,
        .height = screen_h,
    };
    wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &output_box);
}

static void popup_handle_commit(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_popup_t *popup = wl_container_of(listener, popup, commit);
    // A popup cannot be configured before its first commit. This is where the
    // initial configure (and the on-screen constraint) is sent.
    if (popup->xdg_popup->base->initial_commit) {
        popup_unconstrain(popup);
    }
}

static void popup_handle_reposition(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_popup_t *popup = wl_container_of(listener, popup, reposition);
    popup_unconstrain(popup);
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_popup_t *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->reposition.link);
    wl_list_remove(&popup->destroy.link);
    if (popup->container) {
        wlr_scene_node_destroy(&popup->container->node);
    }
    free(popup);
}

// Close every client popup. Compositor-owned UI (menu bar, Dock, overlays,
// desktop) consumes clicks before they reach the seat, so wlroots never sees
// the outside press that would normally dismiss a popup grab.
static void dismiss_surface_popups(struct wlr_xdg_surface *surface) {
    if (!surface) return;
    struct wlr_xdg_popup *popup, *tmp;
    wl_list_for_each_safe(popup, tmp, &surface->popups, link) {
        if (popup->base) {
            dismiss_surface_popups(popup->base);
        }
        wlr_xdg_popup_destroy(popup);
    }
}

void server_dismiss_popups(aurade_server_t *server) {
    if (!server) return;
    aurade_view_t *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->xdg_toplevel || !view->xdg_toplevel->base) {
            continue;
        }
        dismiss_surface_popups(view->xdg_toplevel->base);
    }
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, new_xdg_popup);
    struct wlr_xdg_popup *xdg_popup = data;

    if ((server->mission_control && server->mission_control->active) ||
        (server->launchpad && server->launchpad->active)) {
        wlr_xdg_popup_destroy(xdg_popup);
        return;
    }

    struct wlr_xdg_surface *parent_xdg = xdg_popup->parent
        ? wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent) : NULL;
    if (!parent_xdg) {
        return;
    }

    aurade_popup_t *parent_popup = NULL;
    aurade_view_t *view = NULL;
    struct wlr_scene_tree *parent_tree = NULL;
    bool is_toplevel_popup = false;

    if (parent_xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        view = parent_xdg->data;
        parent_tree = server->popup_tree;
        is_toplevel_popup = true;
    } else if (parent_xdg->role == WLR_XDG_SURFACE_ROLE_POPUP) {
        parent_popup = parent_xdg->data;
        if (!parent_popup) {
            return;
        }
        view = parent_popup->view;
        parent_tree = parent_popup->surface_tree;
    } else {
        return;
    }

    if (!view || !parent_tree) {
        return;
    }

    aurade_popup_t *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        return;
    }
    popup->server = server;
    popup->view = view;
    popup->xdg_popup = xdg_popup;

    popup->container = wlr_scene_tree_create(parent_tree);
    if (!popup->container) {
        free(popup);
        return;
    }

    if (is_toplevel_popup) {
        int content_x = 0, content_y = 0;
        wlr_scene_node_coords(&view->content_tree->node, &content_x, &content_y);
        wlr_scene_node_set_position(&popup->container->node, content_x, content_y);
    }

    popup->surface_tree =
        wlr_scene_xdg_surface_create(popup->container, xdg_popup->base);
    if (!popup->surface_tree) {
        wlr_scene_node_destroy(&popup->container->node);
        free(popup);
        return;
    }

    // Popups and submenus find their parent through xdg_surface->data; the
    // scene node data lets hit-testing map a popup back to its window.
    xdg_popup->base->data = popup;
    popup->container->node.data = view;
    wlr_scene_node_raise_to_top(&popup->container->node);

    popup->commit.notify = popup_handle_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->reposition.notify = popup_handle_reposition;
    wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);
    popup->destroy.notify = popup_handle_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);

    if (getenv("AURADE_DEBUG")) {
        fprintf(stderr, "[AURADE] popup view=%p container=%p geo=%d,%d+%d,%d\n",
                (void *)view, (void *)popup->container,
                xdg_popup->base->geometry.x, xdg_popup->base->geometry.y,
                xdg_popup->base->geometry.width, xdg_popup->base->geometry.height);
    }
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    aurade_view_t *view = calloc(1, sizeof(*view));
    view->server = server;
    view->id = ++server->next_view_id;
    view->xdg_toplevel = xdg_toplevel;
    view->workspace = server->current_workspace;

    // Create scene tree under current workspace scene tree
    view->scene_tree = wlr_scene_tree_create(server->workspace_trees[view->workspace]);
    xdg_toplevel->base->data = view;

    // The client surface fills the whole window: AuraDE never adds a frame.
    view->content_tree = wlr_scene_xdg_surface_create(view->scene_tree, xdg_toplevel->base);
    wlr_scene_node_set_position(&view->content_tree->node, 0, 0);

    view->map.notify = xdg_toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);

    view->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);

    view->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);

    view->commit.notify = xdg_surface_commit_debug;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);

    view->configure.notify = xdg_surface_configure_debug;
    wl_signal_add(&xdg_toplevel->base->events.configure, &view->configure);

    view->request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);

    view->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);

    view->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &view->request_maximize);

    view->request_minimize.notify = xdg_toplevel_request_minimize;
    wl_signal_add(&xdg_toplevel->events.request_minimize, &view->request_minimize);

    view->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &view->request_fullscreen);

    view->set_title.notify = xdg_toplevel_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);

    wl_list_insert(&server->views, &view->link);

    if (server->xdg_decoration_manager) {
        struct wlr_xdg_toplevel_decoration_v1 *decoration;
        wl_list_for_each(decoration, &server->xdg_decoration_manager->decorations, link) {
            if (decoration->toplevel == xdg_toplevel) {
                view_attach_decoration(view, decoration);
                break;
            }
        }
    }
}

void server_init_xdg_shell(aurade_server_t *server) {
    server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3);
    server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);

    server->new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

    server->xdg_decoration_manager =
        wlr_xdg_decoration_manager_v1_create(server->wl_display);
    if (server->xdg_decoration_manager) {
        server->new_xdg_decoration.notify = xdg_decoration_handle_new;
        wl_signal_add(&server->xdg_decoration_manager->events.new_toplevel_decoration,
                      &server->new_xdg_decoration);
    }
}
