#ifndef AURADE_TOPBAR_H
#define AURADE_TOPBAR_H

#include "../server.h"
#include "../render/cairo_buffer.h"

// Menu-bar height; other shell components (Control Center) hang below it.
#define TOPBAR_HEIGHT 28

typedef enum {
    POWER_MENU_CLOSED = 0,
    POWER_MENU_OPEN,
    POWER_MENU_CONFIRM,
} power_menu_state_t;

typedef enum {
    POWER_ACTION_NONE = 0,
    POWER_ACTION_SETTINGS,
    POWER_ACTION_LOCK,
    POWER_ACTION_SLEEP,
    POWER_ACTION_RESTART,
    POWER_ACTION_SHUTDOWN,
    POWER_ACTION_LOGOUT,
} power_action_t;

typedef struct aurade_topbar {
    aurade_server_t *server;
    cairo_buffer_t *buffer;
    struct wlr_scene_buffer *scene_buffer;
    int width, height;
    char active_app_title[128];

    // Layout rectangles, recomputed on every render and used for hit-testing.
    double apple_x, apple_y, apple_w, apple_h;
    bool hover_apple;
    double cc_x, cc_y, cc_w, cc_h;
    bool hover_cc;
    double rec_x, rec_y, rec_w, rec_h;
    bool hover_rec;

    // CPU Temperature widget
    double cpu_temp_x, cpu_temp_y, cpu_temp_w, cpu_temp_h;
    bool hover_cpu_temp;
    double cached_cpu_avg;
    double cached_cpu_max;
    bool cpu_temp_valid;
    uint64_t last_temp_check_ms;

    // Apple menu (system power menu)
    power_menu_state_t power_menu_state;
    power_action_t power_menu_confirm;
    int power_menu_hover_row;      // -1 = no row hovered
    int power_menu_confirm_hover;  // 0 = none, 1 = cancel, 2 = confirm
    cairo_buffer_t *power_menu_buffer;
    struct wlr_scene_buffer *power_menu_scene;
    struct wlr_scene_blur *power_menu_blur_node;

    bool needs_render;
    struct wlr_scene_blur *blur_node;
} aurade_topbar_t;

aurade_topbar_t *topbar_create(aurade_server_t *server);
void topbar_resize(aurade_topbar_t *bar, int width);
void topbar_set_active_app(aurade_topbar_t *bar, const char *title);
void topbar_render(aurade_topbar_t *bar);
bool topbar_handle_click(aurade_topbar_t *bar, double x, double y);
bool topbar_handle_motion(aurade_topbar_t *bar, double x, double y);
void topbar_destroy(aurade_topbar_t *bar);

// Apple/power menu. Clicks anywhere on screen are consumed while this is open.
bool topbar_power_menu_is_open(aurade_topbar_t *bar);
void topbar_close_power_menu(aurade_topbar_t *bar);
bool topbar_handle_power_menu_click(aurade_topbar_t *bar, double x, double y);

#endif // AURADE_TOPBAR_H
