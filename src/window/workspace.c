#include "workspace.h"
#include "xdg_shell.h"
#include "../shell/mission_control.h"
#include "../shell/dock.h"
#include "../shell/topbar.h"
#include <math.h>

void server_init_workspaces(aurade_server_t *server) {
    server->current_workspace = 0;
    server->workspace_offset = 0.0;
    server->workspace_animating = false;
    server->num_workspaces = server->config.num_workspaces > 0 ?
                             server->config.num_workspaces : DEFAULT_WORKSPACES;
    if (server->num_workspaces < 1) server->num_workspaces = 1;
    if (server->num_workspaces > MAX_WORKSPACES) server->num_workspaces = MAX_WORKSPACES;

    for (int i = 0; i < MAX_WORKSPACES; i++) {
        server->workspace_trees[i] = wlr_scene_tree_create(&server->scene->tree);
        wlr_scene_node_set_enabled(&server->workspace_trees[i]->node, i < server->num_workspaces);
    }
    server_update_workspace_positions(server);
}

void server_set_num_workspaces(aurade_server_t *server, int count) {
    if (count < 1) count = 1;
    if (count > MAX_WORKSPACES) count = MAX_WORKSPACES;
    if (server->num_workspaces == count) return;

    int old_count = server->num_workspaces;
    server->num_workspaces = count;

    // If workspaces were reduced, move windows from deleted workspaces to the last available workspace
    if (count < old_count) {
        int dest_ws = count - 1;
        aurade_view_t *view;
        wl_list_for_each(view, &server->views, link) {
            if (view->workspace >= count) {
                view->workspace = dest_ws;
                if (view->scene_tree && server->workspace_trees[dest_ws]) {
                    wlr_scene_node_reparent(&view->scene_tree->node,
                                            server->workspace_trees[dest_ws]);
                }
            }
        }
        if (server->current_workspace >= count) {
            server->current_workspace = dest_ws;
            server->workspace_offset = 0.0;
            server->workspace_animating = false;
            server_workspace_auto_focus(server);
        }
        for (int i = count; i < old_count; i++) {
            if (server->workspace_trees[i]) {
                wlr_scene_node_set_enabled(&server->workspace_trees[i]->node, false);
            }
        }
    } else {
        // Workspaces were added
        for (int i = old_count; i < count; i++) {
            if (!server->workspace_trees[i]) {
                server->workspace_trees[i] = wlr_scene_tree_create(&server->scene->tree);
            }
        }
    }

    server_update_workspace_positions(server);
    if (server->dock) dock_update_visibility(server->dock);
}

void server_update_workspace_positions(aurade_server_t *server) {
    int screen_w = server->screen_width > 0 ? server->screen_width : 1920;
    int num_ws = server->num_workspaces > 0 ? server->num_workspaces : DEFAULT_WORKSPACES;

    for (int i = 0; i < MAX_WORKSPACES; i++) {
        if (i >= num_ws) {
            if (server->workspace_trees[i]) {
                wlr_scene_node_set_enabled(&server->workspace_trees[i]->node, false);
            }
            continue;
        }
        int diff = i - server->current_workspace;
        double pos_x = (diff * screen_w) + server->workspace_offset;
        int ipx = (int)round(pos_x);

        wlr_scene_node_set_position(&server->workspace_trees[i]->node, ipx, 0);
        // Only enable a workspace tree if its [0, screen_w] zone actually overlaps [0, screen_w] on screen
        bool visible = (fabs(server->workspace_offset) < 0.5)
                       ? (i == server->current_workspace)
                       : (abs(ipx) < screen_w);
        wlr_scene_node_set_enabled(&server->workspace_trees[i]->node, visible);
    }

    if (server->views.next && server->views.prev) {
        aurade_view_t *view;
        wl_list_for_each(view, &server->views, link) {
            if (!view->minimized) {
                view_update_workspace_clip(view);
            }
        }
    }
}

void server_workspace_auto_focus(aurade_server_t *server) {
    if (!server) return;
    if (!server->config.workspace_auto_focus) return;

    aurade_view_t *view;
    aurade_view_t *candidate = NULL;
    wl_list_for_each(view, &server->views, link) {
        if (!view->minimized &&
            view->anim_type != VIEW_ANIM_MINIMIZE &&
            view->workspace == server->current_workspace) {
            candidate = view;
            break;
        }
    }

    if (candidate) {
        view_focus(candidate);
        wlr_seat_pointer_clear_focus(server->seat);
    } else {
        view_unfocus(server);
    }
}

void server_switch_workspace(aurade_server_t *server, int target_workspace, bool animate) {
    if (!server) return;
    server_dismiss_popups(server);
    int num_ws = server->num_workspaces > 0 ? server->num_workspaces : DEFAULT_WORKSPACES;
    if (target_workspace < 0) target_workspace = 0;
    if (target_workspace >= num_ws) target_workspace = num_ws - 1;
    if (target_workspace == server->current_workspace && !animate) return;

    int old_ws = server->current_workspace;

    if (!animate) {
        server->current_workspace = target_workspace;
        server->workspace_offset = 0.0;
        server->workspace_animating = false;
        server_update_workspace_positions(server);
        if (server->dock) {
            dock_update_visibility(server->dock);
        }
        schedule_all_outputs(server);
        if (old_ws != target_workspace) {
            server_workspace_auto_focus(server);
        }
        return;
    }

    int screen_w = server->screen_width > 0 ? server->screen_width : 1920;
    int diff = target_workspace - server->current_workspace;
    double cur_offset = server->workspace_offset + (diff * screen_w);

    server->current_workspace = target_workspace;
    server->workspace_offset = cur_offset;

    spring_config_t ws_cfg = server->config.spring;
    ws_cfg.position_threshold = 0.5;
    ws_cfg.velocity_threshold = 1.0;
    spring_simulation_start(&server->workspace_spring, &ws_cfg,
                            cur_offset, 0.0, 0.0);
    server->workspace_animating = true;
    server_update_workspace_positions(server);
    if (server->dock) {
        dock_update_visibility(server->dock);
    }
    schedule_all_outputs(server);
    if (old_ws != target_workspace) {
        server_workspace_auto_focus(server);
    }
}

void server_step_workspace_spring(aurade_server_t *server, double dt) {
    if (!server->workspace_animating) return;

    bool still_animating = spring_simulation_step(&server->workspace_spring, dt);
    server->workspace_offset = server->workspace_spring.current_pos;

    if (!still_animating) {
        server->workspace_animating = false;
        server->workspace_offset = 0.0;
    }

    server_update_workspace_positions(server);
}
