#ifndef AURADE_WORKSPACE_H
#define AURADE_WORKSPACE_H

#include "../server.h"

void server_init_workspaces(aurade_server_t *server);
void server_set_num_workspaces(aurade_server_t *server, int count);
void server_delete_workspace(aurade_server_t *server, int ws_to_delete);
void server_reorder_workspace(aurade_server_t *server, int from_ws, int to_ws);
void server_switch_workspace(aurade_server_t *server, int target_workspace, bool animate);
void server_update_workspace_positions(aurade_server_t *server);
void server_workspace_auto_focus(aurade_server_t *server);
void server_step_workspace_spring(aurade_server_t *server, double dt);

#endif // AURADE_WORKSPACE_H
