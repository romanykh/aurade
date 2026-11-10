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
} mc_window_slot_t;

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
    bool hovered_minus;

    bool mouse_pressed;
    bool is_dragging;
    int drag_slot;
    aurade_view_t *dragged_view;
    double drag_start_x, drag_start_y;
    double drag_offset_x, drag_offset_y;
    int drag_target_workspace;

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
