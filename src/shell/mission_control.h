#ifndef AURADE_MISSION_CONTROL_H
#define AURADE_MISSION_CONTROL_H

#include "../server.h"
#include "../render/cairo_buffer.h"

typedef struct {
    aurade_view_t *view;
    double orig_x, orig_y, orig_w, orig_h;
    double target_x, target_y, target_w, target_h;
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

    int screen_width, screen_height;
    bool active;
    double progress; // 0.0 (closed) to 1.0 (fully open)
    bool needs_render;
    cairo_surface_t *wallpaper_thumb;

    mc_window_slot_t slots[32];
    int slot_count;
    aurade_view_t *hovered_view;
    aurade_view_t *hovered_close_view;
    int hovered_workspace; // -1 if none, 0..NUM_WORKSPACES-1

    // Drag-and-drop to workspace state
    bool is_dragging;
    int drag_slot;
    aurade_view_t *dragged_view;
    double drag_start_x, drag_start_y;
    double drag_offset_x, drag_offset_y;
    int drag_target_workspace; // -1 if not hovering workspace thumbnail, 0..NUM_WORKSPACES-1
    bool mouse_pressed;
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
void mission_control_render(aurade_mission_control_t *mc);
void mission_control_forget_view(aurade_mission_control_t *mc, aurade_view_t *view);
void mission_control_destroy(aurade_mission_control_t *mc);

#endif // AURADE_MISSION_CONTROL_H
