#ifndef AURADE_MUSIC_PILL_H
#define AURADE_MUSIC_PILL_H

#include "../server.h"
#include "../render/cairo_buffer.h"
#include "../gestures/physics.h"
#include <gio/gio.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MUSIC_PILL_POS_TOPBAR_CENTER = 0,
    MUSIC_PILL_POS_DOCK_LEFT,
    MUSIC_PILL_POS_DOCK_RIGHT,
    MUSIC_PILL_POS_DISABLED,
} music_pill_pos_t;

typedef struct aurade_music_pill {
    aurade_server_t *server;
    GDBusConnection *dbus_conn;
    guint prop_change_sub_id;
    guint name_change_sub_id;

    // Track metadata
    bool available;
    bool active;
    bool playing;
    char player_bus_name[128];
    char track_id[256];
    char title[128];
    char artist[128];
    char art_url[512];
    cairo_surface_t *art_surface;
    char loaded_art_path[512];
    char cached_label[256];
    double cached_label_w;

    // Playback Timing & Position
    int64_t track_length;   // In microseconds (0 = unknown/streaming)
    int64_t track_position; // In microseconds
    uint64_t last_pos_update_ms;

    // Waveform Animation & Fast Output Scheduling
    double waveform_phase;
    struct wl_event_source *anim_timer;

    // Fluid Spring Opening / Morph Animation
    spring_simulation_t expand_spring;
    bool is_animating;
    bool expanding; // true: opening to 1.0, false: closing to 0.0
    double expand_progress; // 0.0 = compact pill, 1.0 = expanded island
    double start_pill_w;
    double start_pill_h;

    // Expanded state
    bool expanded;
    int hover_control; // 0: none, 1: prev, 2: play_pause, 3: next, 4: seekbar, 5: body
    bool is_seeking;
    double seek_drag_progress; // 0.0 to 1.0
    uint64_t last_hover_ms;

    // Hit-test geometry in topbar
    double pill_x, pill_y, pill_w, pill_h;
    bool hover_topbar_pill;

    // Hit-test geometry in dock
    double dock_pill_x, dock_pill_y, dock_pill_w, dock_pill_h;
    bool hover_dock_pill;
    int dock_hover_control; // 0: none, 1: prev, 2: play_pause, 3: next

    // Dedicated overlay scene buffer for expanded Dynamic Island
    cairo_buffer_t *overlay_buffer;
    struct wlr_scene_buffer *overlay_scene;
    struct wlr_scene_blur *blur_node;
    bool overlay_open;
    double overlay_x, overlay_y, overlay_w, overlay_h;
    double prev_btn_x, prev_btn_y, prev_btn_r;
    double play_btn_x, play_btn_y, play_btn_r;
    double next_btn_x, next_btn_y, next_btn_r;
    double seek_bar_x, seek_bar_y, seek_bar_w, seek_bar_h;
    bool hover_seek_bar;
} aurade_music_pill_t;

music_pill_pos_t music_pill_get_pos(aurade_music_pill_t *pill);
aurade_music_pill_t *music_pill_create(aurade_server_t *server);
void music_pill_destroy(aurade_music_pill_t *pill);
void music_pill_update(aurade_music_pill_t *pill);
void music_pill_render_topbar(aurade_music_pill_t *pill, cairo_t *cr, int topbar_w, int topbar_h);
void music_pill_render_dock(aurade_music_pill_t *pill, cairo_t *cr, double x, double y, double w, double h);
bool music_pill_handle_topbar_motion(aurade_music_pill_t *pill, double x, double y);
bool music_pill_handle_topbar_click(aurade_music_pill_t *pill, double x, double y);
bool music_pill_handle_dock_motion(aurade_music_pill_t *pill, double x, double y);
bool music_pill_handle_dock_click(aurade_music_pill_t *pill, double x, double y);
bool music_pill_handle_overlay_motion(aurade_music_pill_t *pill, double x, double y);
bool music_pill_handle_overlay_click(aurade_music_pill_t *pill, double x, double y);
void music_pill_collapse(aurade_music_pill_t *pill);
bool music_pill_is_expanded(aurade_music_pill_t *pill);
bool music_pill_overlay_owns(aurade_music_pill_t *pill, double x, double y);

#endif // AURADE_MUSIC_PILL_H
