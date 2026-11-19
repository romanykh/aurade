#ifndef AURADE_EXTENSION_H
#define AURADE_EXTENSION_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>
#include "../server.h"

typedef struct aurade_ext_manager aurade_ext_manager_t;

aurade_ext_manager_t *ext_manager_create(aurade_server_t *server);
void ext_manager_destroy(aurade_ext_manager_t *mgr);

bool ext_manager_is_safe_mode(const aurade_ext_manager_t *mgr);
int ext_manager_loaded_count(const aurade_ext_manager_t *mgr);

void ext_manager_tick(aurade_ext_manager_t *mgr, double dt_sec);
bool ext_manager_handle_key(aurade_ext_manager_t *mgr, uint32_t sym, uint32_t mods, bool pressed);
bool ext_manager_handle_button(aurade_ext_manager_t *mgr, double lx, double ly, uint32_t button, bool pressed);
void ext_manager_render_topbar(aurade_ext_manager_t *mgr, cairo_t *cr, int bar_w, int bar_h);

void ext_manager_notify_window_open(aurade_ext_manager_t *mgr, aurade_view_t *view);
void ext_manager_notify_window_close(aurade_ext_manager_t *mgr, aurade_view_t *view);
void ext_manager_notify_window_focus(aurade_ext_manager_t *mgr, aurade_view_t *view);

#endif // AURADE_EXTENSION_H
