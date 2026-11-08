#include "xwayland.h"
#include "../shell/topbar.h"
#include "../shell/dock.h"
#include "../shell/mission_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void xwayland_surface_map(struct wl_listener *listener, void *data);
static void xwayland_surface_unmap(struct wl_listener *listener, void *data);

static inline void safe_remove_listener(struct wl_listener *listener) {
    if (listener->link.prev && listener->link.next) {
        wl_list_remove(&listener->link);
        listener->link.prev = NULL;
        listener->link.next = NULL;
    }
}

static void handle_content_tree_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, content_tree_destroy);
    view->content_tree = NULL;
    safe_remove_listener(&view->content_tree_destroy);
}

static void handle_xwayland_ready(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_server_t *server = wl_container_of(listener, server, xwayland_ready);
    if (!server || !server->xwayland) return;

    setenv("DISPLAY", server->xwayland->display_name, 1);

    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (!wayland_display) wayland_display = "wayland-0";

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "dbus-update-activation-environment --systemd DISPLAY=%s WAYLAND_DISPLAY=%s 2>/dev/null &",
             server->xwayland->display_name, wayland_display);
    system(cmd);

    snprintf(cmd, sizeof(cmd),
             "systemctl --user import-environment DISPLAY WAYLAND_DISPLAY 2>/dev/null &");
    system(cmd);

    fprintf(stderr, "[AURADE] Xwayland ready on DISPLAY=%s (WAYLAND_DISPLAY=%s)\n",
            server->xwayland->display_name, wayland_display);
}

static void xwayland_surface_associate(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, xwayland_associate);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    if (!xsurface || !xsurface->surface) return;

    safe_remove_listener(&view->map);
    safe_remove_listener(&view->unmap);

    view->map.notify = xwayland_surface_map;
    wl_signal_add(&xsurface->surface->events.map, &view->map);

    view->unmap.notify = xwayland_surface_unmap;
    wl_signal_add(&xsurface->surface->events.unmap, &view->unmap);

    if (view->scene_tree && !view->content_tree) {
        view->content_tree = wlr_scene_subsurface_tree_create(view->scene_tree, xsurface->surface);
        if (view->content_tree) {
            safe_remove_listener(&view->content_tree_destroy);
            view->content_tree_destroy.notify = handle_content_tree_destroy;
            wl_signal_add(&view->content_tree->node.events.destroy, &view->content_tree_destroy);
        }
    }
}

static void xwayland_surface_dissociate(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, xwayland_dissociate);
    safe_remove_listener(&view->map);
    safe_remove_listener(&view->unmap);
    if (view->content_tree) {
        safe_remove_listener(&view->content_tree_destroy);
        wlr_scene_node_destroy(&view->content_tree->node);
        view->content_tree = NULL;
    }
}

static void xwayland_surface_map(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, map);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    aurade_server_t *server = view->server;
    if (!xsurface || !server) return;

    if (xsurface->override_redirect) {
        if (view->scene_tree) {
            wlr_scene_node_set_position(&view->scene_tree->node, xsurface->x, xsurface->y);
            wlr_scene_node_set_enabled(&view->scene_tree->node, true);
        }
        if (wlr_xwayland_surface_override_redirect_wants_focus(xsurface)) {
            view_focus(view);
        }
        return;
    }

    int screen_w = server->screen_width > 0 ? server->screen_width : 1920;
    int screen_h = server->screen_height > 0 ? server->screen_height : 1080;
    int top_margin = TOPBAR_HEIGHT;
    bool hide_dock = server->config.dock_autohide ||
                     server->config.dock_intellihide ||
                     !server->config.dock_enabled;
    int bottom_margin = hide_dock ? 0 : 96;

    int w = xsurface->width;
    int h = xsurface->height;
    if (w <= 0 || h <= 0) {
        if (xsurface->surface && xsurface->surface->current.width > 0) {
            w = xsurface->surface->current.width;
            h = xsurface->surface->current.height;
        } else {
            w = 800;
            h = 600;
        }
    }
    if (w < 240) w = 240;
    if (h < 180) h = 180;

    int x = xsurface->x;
    int y = xsurface->y;
    if (x <= 0 && y <= 0) {
        x = (screen_w - w) / 2;
        y = top_margin + ((screen_h - top_margin - bottom_margin) - h) / 2;
        if (y < top_margin) y = top_margin;
        if (x < 0) x = 0;
    }
    if (!xsurface->override_redirect && y < top_margin) {
        y = top_margin;
    }

    view->x = x;
    view->y = y;
    view->width = w;
    view->height = h;

    if (view->scene_tree) {
        wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
        wlr_scene_node_set_enabled(&view->scene_tree->node, true);
    }

    wlr_xwayland_surface_configure(xsurface, view->x, view->y, view->width, view->height);

    if (wl_list_empty(&view->link)) {
        wl_list_insert(&server->views, &view->link);
    }

    view_focus(view);

    if (!xsurface->override_redirect && view->scene_tree) {
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
        if (server->dock) {
            dock_stop_bounce(server->dock, view_get_app_id(view));
        }
        schedule_all_outputs(server);
    }

    dock_refresh(server);
}

static void xwayland_surface_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, unmap);
    struct aurade_server *server = view->server;
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;

    if (view->scene_tree) {
        wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    }

    if (server->grabbed_view == view) {
        server->grabbed_view = NULL;
        server->cursor_mode = CURSOR_PASSTHROUGH;
    }

    if (server->mission_control) {
        mission_control_forget_view(server->mission_control, view);
    }

    if (xsurface && !xsurface->override_redirect) {
        if (!wl_list_empty(&view->link)) {
            wl_list_remove(&view->link);
            wl_list_init(&view->link);
        }
        if (server->focused_view == view) {
            server->focused_view = NULL;
            aurade_view_t *other;
            wl_list_for_each(other, &server->views, link) {
                if (!other->minimized && other->workspace == server->current_workspace) {
                    view_focus(other);
                    break;
                }
            }
            if (!server->focused_view) {
                wlr_seat_keyboard_clear_focus(server->seat);
                if (server->topbar) topbar_set_active_app(server->topbar, "Finder");
            }
        }
        if (server->active_constraint && xsurface &&
            server->active_constraint->surface == xsurface->surface) {
            server_set_pointer_constraint(server, NULL);
        }
        dock_refresh(server);
    }
}

static void xwayland_surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, destroy);
    struct aurade_server *server = view->server;

    if (!wl_list_empty(&view->link)) {
        wl_list_remove(&view->link);
        wl_list_init(&view->link);
    }

    if (view->xwayland_surface) {
        view->xwayland_surface->data = NULL;
    }

    if (server->grabbed_view == view) {
        server->grabbed_view = NULL;
        server->cursor_mode = CURSOR_PASSTHROUGH;
    }

    if (server->mission_control) {
        mission_control_forget_view(server->mission_control, view);
    }

    if (server->active_constraint && view->xwayland_surface &&
        server->active_constraint->surface == view->xwayland_surface->surface) {
        server_set_pointer_constraint(server, NULL);
    }

    safe_remove_listener(&view->map);
    safe_remove_listener(&view->unmap);
    safe_remove_listener(&view->destroy);
    safe_remove_listener(&view->xwayland_associate);
    safe_remove_listener(&view->xwayland_dissociate);
    safe_remove_listener(&view->xwayland_request_configure);
    safe_remove_listener(&view->request_move);
    safe_remove_listener(&view->request_resize);
    safe_remove_listener(&view->request_maximize);
    safe_remove_listener(&view->request_minimize);
    safe_remove_listener(&view->request_fullscreen);
    safe_remove_listener(&view->xwayland_request_activate);
    safe_remove_listener(&view->set_title);
    safe_remove_listener(&view->xwayland_set_class);
    safe_remove_listener(&view->xwayland_set_override_redirect);

    if (view->content_tree) {
        safe_remove_listener(&view->content_tree_destroy);
        wlr_scene_node_destroy(&view->content_tree->node);
        view->content_tree = NULL;
    }
    if (view->scene_tree) {
        wlr_scene_node_destroy(&view->scene_tree->node);
        view->scene_tree = NULL;
    }

    if (server->focused_view == view) {
        server->focused_view = NULL;
        aurade_view_t *other;
        wl_list_for_each(other, &server->views, link) {
            if (!other->minimized && other->workspace == server->current_workspace) {
                view_focus(other);
                break;
            }
        }
        if (!server->focused_view) {
            wlr_seat_keyboard_clear_focus(server->seat);
            if (server->topbar) topbar_set_active_app(server->topbar, "Finder");
        }
    }

    free(view);
    dock_refresh(server);
}

static void xwayland_surface_request_configure(struct wl_listener *listener, void *data) {
    aurade_view_t *view = wl_container_of(listener, view, xwayland_request_configure);
    struct wlr_xwayland_surface_configure_event *ev = data;
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    if (!xsurface) return;

    if (xsurface->override_redirect) {
        wlr_xwayland_surface_configure(xsurface, ev->x, ev->y, ev->width, ev->height);
        if (view->scene_tree) {
            wlr_scene_node_set_position(&view->scene_tree->node, ev->x, ev->y);
        }
        return;
    }

    if (view->maximized || view->fullscreen) {
        wlr_xwayland_surface_configure(xsurface, view->x, view->y, view->width, view->height);
        return;
    }

    if (!xsurface->override_redirect && ev->y < TOPBAR_HEIGHT) {
        ev->y = TOPBAR_HEIGHT;
    }

    view->x = ev->x;
    view->y = ev->y;
    view->width = ev->width;
    view->height = ev->height;
    wlr_xwayland_surface_configure(xsurface, ev->x, ev->y, ev->width, ev->height);
    if (view->scene_tree) {
        wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    }
}

static void xwayland_surface_request_move(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, request_move);
    aurade_server_t *server = view->server;
    if (!server) return;

    if (server->cursor_mode == CURSOR_PASSTHROUGH) {
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

            wlr_xwayland_surface_set_maximized(view->xwayland_surface, false, false);
            wlr_xwayland_surface_configure(view->xwayland_surface, new_x, new_y, restored_w, restored_h);
            wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
        }

        server->grabbed_view = view;
        server->cursor_mode = CURSOR_MOVE;
        server->grab_button = 0;
        server->grab_x = server->cursor->x - view->x;
        server->grab_y = server->cursor->y - view->y;
        wlr_seat_pointer_clear_focus(server->seat);
    }
}

static void xwayland_surface_request_resize(struct wl_listener *listener, void *data) {
    aurade_view_t *view = wl_container_of(listener, view, request_resize);
    struct wlr_xwayland_resize_event *ev = data;
    aurade_server_t *server = view->server;
    if (!server) return;

    if (server->cursor_mode == CURSOR_PASSTHROUGH) {
        server->grabbed_view = view;
        server->cursor_mode = CURSOR_RESIZE;
        server->grab_button = 0;
        server->resize_edges = ev->edges;
        server->grab_x = server->cursor->x;
        server->grab_y = server->cursor->y;
        server->grab_geobox.x = view->x;
        server->grab_geobox.y = view->y;
        server->grab_geobox.width = view->width;
        server->grab_geobox.height = view->height;
        wlr_seat_pointer_clear_focus(server->seat);
    }
}

static void xwayland_surface_request_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, request_maximize);
    view_toggle_maximize(view);
}

static void xwayland_surface_request_minimize(struct wl_listener *listener, void *data) {
    aurade_view_t *view = wl_container_of(listener, view, request_minimize);
    struct wlr_xwayland_minimize_event *ev = data;
    if (ev->minimize) {
        view_minimize(view);
    } else {
        view_restore(view);
    }
}

static void xwayland_surface_request_fullscreen(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, request_fullscreen);
    view_set_fullscreen(view, !view->fullscreen);
}

static void xwayland_surface_request_activate(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, xwayland_request_activate);
    if (view->xwayland_surface && view->xwayland_surface->surface &&
        view->xwayland_surface->surface->mapped) {
        view_focus(view);
    }
}

static void xwayland_surface_set_title(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, set_title);
    if (!view || !view->xwayland_surface) return;
    aurade_server_t *server = view->server;
    if (server && server->focused_view == view && server->topbar) {
        const char *title = view->xwayland_surface->title;
        if (!title || !*title) title = view->xwayland_surface->class;
        if (title) topbar_set_active_app(server->topbar, title);
    }
    dock_refresh(server);
}

static void xwayland_surface_set_class(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, xwayland_set_class);
    if (view && view->server) {
        dock_refresh(view->server);
    }
}

static void xwayland_surface_set_override_redirect(struct wl_listener *listener, void *data) {
    (void)data;
    aurade_view_t *view = wl_container_of(listener, view, xwayland_set_override_redirect);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    if (!xsurface || !view->scene_tree) return;

    if (xsurface->override_redirect) {
        if (!wl_list_empty(&view->link)) {
            wl_list_remove(&view->link);
            wl_list_init(&view->link);
        }
        wlr_scene_node_reparent(&view->scene_tree->node, view->server->popup_tree);
    } else {
        wlr_scene_node_reparent(&view->scene_tree->node,
                                view->server->workspace_trees[view->workspace]);
        if (wl_list_empty(&view->link) && xsurface->surface && xsurface->surface->mapped) {
            wl_list_insert(&view->server->views, &view->link);
        }
    }
}

static void handle_xwayland_new_surface(struct wl_listener *listener, void *data) {
    aurade_server_t *server = wl_container_of(listener, server, xwayland_new_surface);
    struct wlr_xwayland_surface *xsurface = data;
    if (!xsurface) return;

    aurade_view_t *view = calloc(1, sizeof(*view));
    if (!view) return;

    view->server = server;
    view->id = ++server->next_view_id;
    view->xwayland_surface = xsurface;
    xsurface->data = view;
    view->workspace = server->current_workspace;
    view->current_scale = 1.0;
    view->anim_scale = 1.0;
    view->anim_alpha = 1.0;
    wl_list_init(&view->link);

    struct wlr_scene_tree *parent_tree = xsurface->override_redirect ?
        server->popup_tree : server->workspace_trees[view->workspace];
    view->scene_tree = wlr_scene_tree_create(parent_tree);
    view->scene_tree->node.data = view;
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);

    view->xwayland_associate.notify = xwayland_surface_associate;
    wl_signal_add(&xsurface->events.associate, &view->xwayland_associate);

    view->xwayland_dissociate.notify = xwayland_surface_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &view->xwayland_dissociate);

    view->destroy.notify = xwayland_surface_destroy;
    wl_signal_add(&xsurface->events.destroy, &view->destroy);

    view->xwayland_request_configure.notify = xwayland_surface_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &view->xwayland_request_configure);

    view->request_move.notify = xwayland_surface_request_move;
    wl_signal_add(&xsurface->events.request_move, &view->request_move);

    view->request_resize.notify = xwayland_surface_request_resize;
    wl_signal_add(&xsurface->events.request_resize, &view->request_resize);

    view->request_maximize.notify = xwayland_surface_request_maximize;
    wl_signal_add(&xsurface->events.request_maximize, &view->request_maximize);

    view->request_minimize.notify = xwayland_surface_request_minimize;
    wl_signal_add(&xsurface->events.request_minimize, &view->request_minimize);

    view->request_fullscreen.notify = xwayland_surface_request_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen, &view->request_fullscreen);

    view->xwayland_request_activate.notify = xwayland_surface_request_activate;
    wl_signal_add(&xsurface->events.request_activate, &view->xwayland_request_activate);

    view->set_title.notify = xwayland_surface_set_title;
    wl_signal_add(&xsurface->events.set_title, &view->set_title);

    view->xwayland_set_class.notify = xwayland_surface_set_class;
    wl_signal_add(&xsurface->events.set_class, &view->xwayland_set_class);

    view->xwayland_set_override_redirect.notify = xwayland_surface_set_override_redirect;
    wl_signal_add(&xsurface->events.set_override_redirect, &view->xwayland_set_override_redirect);

    if (xsurface->surface) {
        xwayland_surface_associate(&view->xwayland_associate, NULL);
    }
}

void server_init_xwayland(aurade_server_t *server) {
    if (!server || !server->wl_display || !server->compositor) {
        fprintf(stderr, "[AURADE] Cannot initialize Xwayland: missing display or compositor\n");
        return;
    }

    server->xwayland = wlr_xwayland_create(server->wl_display, server->compositor, false);
    if (!server->xwayland) {
        fprintf(stderr, "[AURADE] Failed to create Xwayland server\n");
        return;
    }

    server->xwayland_ready.notify = handle_xwayland_ready;
    wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);

    server->xwayland_new_surface.notify = handle_xwayland_new_surface;
    wl_signal_add(&server->xwayland->events.new_surface, &server->xwayland_new_surface);

    if (server->seat) {
        wlr_xwayland_set_seat(server->xwayland, server->seat);
    }

    fprintf(stderr, "[AURADE] Xwayland server initialized\n");
}

void server_finish_xwayland(aurade_server_t *server) {
    if (!server || !server->xwayland) return;
    wlr_xwayland_destroy(server->xwayland);
    server->xwayland = NULL;
}
