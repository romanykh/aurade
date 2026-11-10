#ifndef AURADE_MISSION_CONTROL_H
#define AURADE_MISSION_CONTROL_H

#include "../server.h"
#include "../render/cairo_buffer.h"

#define MC_MAX_SLOTS 128

typedef struct {
    aurade_view_t *view;
    int workspace;
    double orig_x, orig_y;
    double orig_w, orig_h;
    double target_x, target_y;
    double target_w, target_h;
    double scale;
    char display_title[160];
    double title_width;
    double badge_w;
    desktop_app_t *app;

    // Relayout reflow animation
    bool is_relayout;
    double relayout_progress;
    double from_x, from_y;
    double from_scale;

    // Close window animation
    bool is_closing;
    double close_progress;
    double close_start_x, close_start_y;
    double close_start_scale;
} mc_window_slot_t;

typedef struct {
    bool active;
    bool is_return;
    aurade_view_t *view;
    int from_workspace;
    int to_workspace;
    double start_x, start_y;
    double target_x, target_y;
    double start_scale, target_scale;
    double progress;
} mc_window_drop_anim_t;

typedef enum {
    WS_BAR_ANIM_NONE = 0,
    WS_BAR_ANIM_ADD,
    WS_BAR_ANIM_DELETE,
} mc_ws_bar_anim_type_t;

typedef struct {
    bool active;
    mc_ws_bar_anim_type_t type;
    int index;
    double progress;
} mc_ws_bar_anim_t;

typedef struct aurade_mission_control {
    aurade_server_t *server;
    cairo_buffer_t *buffer;
    struct wlr_scene_buffer *scene_buffer;

    int screen_width;
    int screen_height;
    bool active;
    double progress;
    bool needs_render;

    cairo_surface_t *wallpaper_thumb;
    cairo_surface_t *spaces_bar_surf;
    bool spaces_bar_dirty;

    mc_window_slot_t slots[MC_MAX_SLOTS];
    int slot_count;

    aurade_view_t *hovered_view;
    aurade_view_t *hovered_close_view;
    int hovered_workspace;
    int hovered_ws_close;
    bool hovered_plus;

    bool mouse_pressed;
    bool is_dragging;
    int drag_slot;
    aurade_view_t *dragged_view;
    double drag_start_x, drag_start_y;
    double drag_offset_x, drag_offset_y;
    int drag_target_workspace;

    bool is_dragging_ws;
    int ws_drag_candidate;
    int ws_close_candidate;
    bool plus_candidate;
    int drag_ws;
    int ws_drag_target;
    double ws_drag_start_x, ws_drag_start_y;
    double ws_drag_current_x, ws_drag_current_y;

    mc_window_drop_anim_t drop_anim;
    mc_ws_bar_anim_t ws_bar_anim;

    bool ws_animating;
    bool ws_gesture_active;
    double ws_gesture_base_offset;
    spring_simulation_t ws_spring;
    double ws_slide_offset;
    double scroll_accum_x;
    uint64_t last_scroll_time_ms;
} aurade_mission_control_t;

aurade_mission_control_t *mission_control_create(aurade_server_t *server);
void mission_control_resize(aurade_mission_control_t *mc, int width, int height);
bool mission_control_is_animating(const aurade_mission_control_t *mc);
void mission_control_begin(aurade_mission_control_t *mc);
void mission_control_update_progress(aurade_mission_control_t *mc, double progress);
void mission_control_end(aurade_mission_control_t *mc, bool opened);
bool mission_control_handle_click(aurade_mission_control_t *mc, double x, double y);
bool mission_control_handle_press(aurade_mission_control_t *mc, double x, double y);
bool mission_control_handle_release(aurade_mission_control_t *mc, double x, double y);
void mission_control_handle_motion(aurade_mission_control_t *mc, double x, double y);
void mission_control_switch_workspace(aurade_mission_control_t *mc, int new_workspace);
void mission_control_ws_gesture_begin(aurade_mission_control_t *mc);
void mission_control_ws_gesture_update(aurade_mission_control_t *mc, double delta_x);
void mission_control_ws_gesture_end(aurade_mission_control_t *mc, bool cancelled, double velocity_x);
void mission_control_handle_axis(aurade_mission_control_t *mc, enum wl_pointer_axis orientation,
                                 double delta, int32_t delta_discrete);
void mission_control_tick(aurade_mission_control_t *mc, double dt);
void mission_control_render(aurade_mission_control_t *mc);
void mission_control_forget_view(aurade_mission_control_t *mc, aurade_view_t *view);
void mission_control_destroy(aurade_mission_control_t *mc);

#endif // AURADE_MISSION_CONTROL_H
