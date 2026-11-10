#ifndef AURADE_LOCKSCREEN_H
#define AURADE_LOCKSCREEN_H

#include "../server.h"
#include "../render/cairo_buffer.h"
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>
#include <pthread.h>

typedef enum {
    LOCKSCREEN_STATE_UNLOCKED = 0,
    LOCKSCREEN_STATE_LOCKING,
    LOCKSCREEN_STATE_LOCKED,
    LOCKSCREEN_STATE_UNLOCKING,
} lockscreen_state_t;

typedef struct {
    pthread_t thread;
    char username[64];
    char password[256];
    bool done;
    bool success;
} pam_task_t;

typedef struct aurade_lockscreen {
    aurade_server_t *server;
    cairo_buffer_t *buffer;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_buffer *scene_buffer;

    int width, height;
    lockscreen_state_t state;
    double progress; // 0.0 to 1.0 (lock in / unlock out)
    bool needs_render;

    // User details
    char username[64];
    char display_name[128];

    // Password buffer
    char password[256];
    int password_len;
    bool caps_lock;
    bool authenticating;
    double spinner_angle;

    // Error & Shake state
    bool shake_active;
    double shake_time;
    double shake_offset;
    char error_msg[128];

    // PAM worker task
    pam_task_t *pam_task;

    // Hit-testing & hover geometry
    double input_x, input_y, input_w, input_h;
    double submit_x, submit_y, submit_r;
    bool hover_submit;

    // Bottom action buttons (Sleep, Restart, Shut Down)
    double btn_sleep_x, btn_sleep_y, btn_r;
    double btn_restart_x, btn_restart_y;
    double btn_power_x, btn_power_y;
    int hover_action; // 0=none, 1=sleep, 2=restart, 3=shutdown
} aurade_lockscreen_t;

aurade_lockscreen_t *lockscreen_create(aurade_server_t *server);
void lockscreen_destroy(aurade_lockscreen_t *ls);
void lockscreen_resize(aurade_lockscreen_t *ls, int width, int height);

void lockscreen_show(aurade_lockscreen_t *ls);
void lockscreen_unlock(aurade_lockscreen_t *ls);

bool lockscreen_is_active(const aurade_lockscreen_t *ls);
bool lockscreen_is_animating(const aurade_lockscreen_t *ls);

void lockscreen_tick(aurade_lockscreen_t *ls, double dt);
void lockscreen_render(aurade_lockscreen_t *ls);

bool lockscreen_handle_key(aurade_lockscreen_t *ls, xkb_keysym_t sym,
                           struct xkb_state *xkb_state, bool pressed);
void lockscreen_handle_pointer_motion(aurade_lockscreen_t *ls, double x, double y);
bool lockscreen_handle_button(aurade_lockscreen_t *ls, double x, double y,
                              uint32_t button, bool pressed);

#endif // AURADE_LOCKSCREEN_H
