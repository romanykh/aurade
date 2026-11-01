#ifndef AURADE_DOCK_H
#define AURADE_DOCK_H

#include "../server.h"
#include "../render/cairo_buffer.h"

typedef struct {
    char name[64];
    char exec[256];
    char icon_name[64];
    cairo_surface_t *icon_surface;
    double current_scale;
    double center_x;
    double width;
    bool is_running;
    bool is_pinned;
    bool demands_attention;
    double bounce_offset;
    double bounce_vel;
    bool is_bouncing;
    int bounce_count;
    cairo_surface_t *cached_base_surface;
    int cached_base_size;
} dock_item_t;

typedef enum {
    DOCK_MENU_ACT_NONE = 0,
    DOCK_MENU_ACT_NEW_WINDOW,
    DOCK_MENU_ACT_UNPIN,
    DOCK_MENU_ACT_KEEP_IN_DOCK,
    DOCK_MENU_ACT_QUIT,
    DOCK_MENU_ACT_LAUNCHPAD,
} dock_menu_action_t;

typedef struct {
    dock_menu_action_t action;
    char label[64];
    bool is_separator;
} dock_menu_row_t;

typedef struct aurade_dock {
    aurade_server_t *server;
    cairo_buffer_t *buffer;
    struct wlr_scene_buffer *scene_buffer;

    int screen_width, screen_height;
    int buffer_height;

    // Dock pill in screen coordinates (for hit-testing).
    double pill_x, pill_y, pill_width, pill_height;

    dock_item_t items[24];
    int item_count;

    double mouse_x, mouse_y;
    bool mouse_over_dock;
    int hovered_index;
    int base_icon_size;

    bool enabled;
    bool needs_render;
    cairo_surface_t *blur_cache;
    bool blur_dirty;
    double last_blur_x, last_blur_y, last_blur_w, last_blur_h;

    // Auto-hide: the dock rests below the screen and slides up when the
    // pointer reaches the bottom edge (or is already over the dock).
    bool autohide;
    bool intellihide;
    double hide_progress;      // 0 = fully visible, 1 = fully hidden
    bool hide_target;
    uint64_t hide_at_ms;       // grace deadline before sliding away
    uint64_t last_tick_ms;
    struct wl_event_source *anim_timer;

    // Drag-to-reorder state
    int drag_candidate_index;
    int drag_index;
    bool is_dragging;
    double drag_start_x, drag_start_y;
    double drag_current_x;

    // Right-click context menu
    cairo_buffer_t *menu_buffer;
    struct wlr_scene_buffer *menu_scene_buffer;
    bool menu_open;
    int menu_item_index;
    double menu_x, menu_y, menu_w, menu_h;
    int menu_hover_row;
    dock_menu_row_t menu_rows[8];
    int menu_row_count;
} aurade_dock_t;

aurade_dock_t *dock_create(aurade_server_t *server);
void dock_resize(aurade_dock_t *dock, int screen_w, int screen_h);
void dock_handle_motion(aurade_dock_t *dock, double mouse_x, double mouse_y);
bool dock_handle_press(aurade_dock_t *dock, double x, double y);
bool dock_handle_release(aurade_dock_t *dock, double x, double y);
bool dock_handle_click(aurade_dock_t *dock, double x, double y);
bool dock_pointer_owns(aurade_dock_t *dock, double x, double y);
void dock_update_running_state(aurade_dock_t *dock);
void dock_refresh(aurade_server_t *server);
void dock_render(aurade_dock_t *dock);
void dock_apply_slide(aurade_dock_t *dock);
bool dock_intersects_window(aurade_dock_t *dock);
void dock_update_visibility(aurade_dock_t *dock);
bool dock_contains_point(aurade_dock_t *dock, double x, double y);
bool dock_get_item_center(aurade_dock_t *dock, const char *name, double *out_x, double *out_y);
void dock_start_bounce(aurade_dock_t *dock, const char *name);
void dock_stop_bounce(aurade_dock_t *dock, const char *name);
bool dock_is_bouncing(aurade_dock_t *dock);
void dock_tick_bounce(aurade_dock_t *dock, double dt);
bool dock_pin_app(aurade_dock_t *dock, const char *name, const char *exec,
                  const char *icon_name, cairo_surface_t *icon_surface);
void dock_save_pinned(aurade_dock_t *dock);
void dock_destroy(aurade_dock_t *dock);

// Context menu
bool dock_context_menu_is_open(aurade_dock_t *dock);
void dock_open_context_menu(aurade_dock_t *dock, int item_index);
void dock_close_context_menu(aurade_dock_t *dock);
bool dock_handle_context_click(aurade_dock_t *dock, double x, double y);
bool dock_handle_context_menu_click(aurade_dock_t *dock, double x, double y);

#endif // AURADE_DOCK_H
