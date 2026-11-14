#ifndef AURADE_CONFIG_H
#define AURADE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <xkbcommon/xkbcommon.h>
#include "../gestures/physics.h"

#define MAX_CUSTOM_SHORTCUTS 32

typedef struct {
    char name[64];
    char combo[64];
    char command[256];
} custom_shortcut_t;

typedef enum {
    ACTION_NONE = 0,
    ACTION_MISSION_CONTROL,
    ACTION_APP_EXPOSE,
    ACTION_WORKSPACE_NEXT,
    ACTION_WORKSPACE_PREV,
    ACTION_LAUNCHPAD,
    ACTION_SHOW_DESKTOP,
    ACTION_CONTROL_CENTER,
    ACTION_CUSTOM,
} gesture_action_type_t;

typedef struct {
    gesture_action_type_t type;
    char custom_cmd[256];
} gesture_action_t;

typedef struct {
    double opacity;
    double blur;
    bool liquid_glass;
} glass_element_t;

typedef struct {
    // 3-finger gestures
    gesture_action_t swipe_up_3;
    gesture_action_t swipe_down_3;
    gesture_action_t swipe_left_3;
    gesture_action_t swipe_right_3;
    gesture_action_t pinch_in_3;
    gesture_action_t pinch_out_3;

    // 4-finger gestures
    gesture_action_t swipe_up_4;
    gesture_action_t swipe_down_4;
    gesture_action_t swipe_left_4;
    gesture_action_t swipe_right_4;
    gesture_action_t pinch_in_4;
    gesture_action_t pinch_out_4;

    // Edge gestures
    gesture_action_t edge_swipe_right;
    bool notif_center_left;         // false = right edge swipe/slide, true = left edge swipe/slide

    // Physics
    spring_config_t spring;
    double sensitivity;
    double snap_threshold;          // e.g. 0.35 (35% displacement to complete)
    double flick_velocity_threshold;// e.g. 350.0 px/sec
    double rubber_band_limit;       // e.g. 180.0 px
    double deadzone_px;             // e.g. 8.0 px
    bool natural_scroll;            // true for fluid direction (touchpad)
    bool mouse_natural_scroll;      // true for fluid direction (physical mouse)
    bool disable_while_typing;      // true to lock touchpad while typing; false for gaming
    bool smooth_wheel_scroll;       // true for kinetic mouse wheel scrolling

    // Keyboard. Empty strings mean "auto-detect from XKB env or
    // /etc/vconsole.conf", which keeps AuraDE in sync with the system layout.
    char kb_layout[128];            // e.g. "us,ua"
    char kb_variant[128];           // e.g. ",winkeysenhanced"
    char kb_model[64];              // e.g. "pc105"
    char kb_options[256];           // e.g. "grp:alt_shift_toggle"

#define DOCK_THEME_MODERN "modern"
#define DOCK_THEME_TABLE_3D "table_3d"
#define DOCK_THEME_PLANK "plank"

    // Dock
    bool dock_enabled;
    bool dock_autohide;             // hidden until the pointer reaches the bottom edge
    bool dock_intellihide;          // hidden only when an active window overlaps the dock
    int dock_icon_size;
    bool dock_magnification;
    double dock_max_scale;
    double dock_mag_radius;
    char dock_theme[32];            // "modern", "table_3d", "plank"
    double dock_opacity;            // 0.0 (transparent) to 1.0 (solid)
    int dock_pinned_count;
    struct {
        char name[64];
        char exec[256];
    } dock_pinned[24];

    // Appearance
    bool dark_mode;
    char wallpaper_path[512];
    char accent_color[32];
    double window_opacity;          // 0.1 to 1.0 (default 1.0)
    int window_rules_count;
    struct {
        char app_id[128];
        char title[256];
        double opacity;
    } window_rules[64];

    // Music Pill / Dynamic Island
    bool music_pill_enabled;
    char music_pill_position[32]; // "topbar_center", "dock_left", "dock_right", "disabled"

    // Workspaces
    int num_workspaces;
    bool workspace_auto_focus;
    bool tiling_enabled;            // Hyprland-style dynamic dwindle tiling (off by default)
    int tiling_gap;                 // Inner/outer gap in pixels for tiled windows (default 10)

    // Blur effects (legacy/global)
    bool blur_dock;
    bool blur_control_center;
    bool blur_power_menu;
    bool blur_notifications;
    bool blur_lockscreen;
    bool blur_topbar;
    bool blur_launchpad;
    bool blur_workspace_overview;
    bool blur_windows;
    double blur_strength;
    int blur_radius;
    int blur_passes;
    double blur_saturation;
    double blur_brightness;
    double blur_contrast;
    double blur_noise;
    bool liquid_glass;

    // Per-element Blur, Liquid Glass & Opacity
    glass_element_t glass_windows;
    glass_element_t glass_dock;
    glass_element_t glass_topbar;
    glass_element_t glass_control_center;
    glass_element_t glass_power_menu;
    glass_element_t glass_notifications;
    glass_element_t glass_notification_center;
    glass_element_t glass_osd;
    glass_element_t glass_music_pill;
    glass_element_t glass_lockscreen;
    glass_element_t glass_launchpad;
    glass_element_t glass_workspace_overview;

    // Topbar Widgets
    bool topbar_cpu_temp;

    // Custom Keyboard Shortcuts
    int custom_shortcuts_count;
    custom_shortcut_t custom_shortcuts[MAX_CUSTOM_SHORTCUTS];
} aurade_config_t;

/**
 * Check if a key event matches a shortcut combination string (e.g. "Super+Return", "Ctrl+Alt+T").
 */
bool shortcut_matches_event(const char *combo,
                            bool super, bool ctrl, bool alt, bool shift,
                            xkb_keysym_t sym, xkb_keysym_t latin_sym);

/**
 * Get default fluid gesture configuration.
 */
aurade_config_t config_get_defaults(void);

/**
 * Load configuration from path (typically ~/.config/aurade/config.json).
 * Creates with defaults if not found.
 */
bool config_load(aurade_config_t *cfg, const char *path);

/**
 * Save configuration to path.
 */
bool config_save(const aurade_config_t *cfg, const char *path);

/**
 * Helper to parse action string (e.g. "mission_control", "custom:foot").
 */
gesture_action_t action_from_string(const char *str);

/**
 * Helper to convert action to string for JSON saving.
 */
const char *action_to_string(const gesture_action_t *action, char *buf, size_t buf_size);

/**
 * Get default config file path in ~/.config/aurade/config.json.
 */
const char *config_get_default_path(void);

/*
 * Session Hardware State (Volume, Brightness & Per-Device Earbuds Memory)
 */
typedef struct {
    double volume;           // Last active volume (0.01 - 1.0, default 0.30)
    bool volume_muted;       // Mute state
    double brightness;       // Brightness (0.01 - 1.0, default 0.60)
    int sink_count;
    struct {
        char name[128];
        double volume;
        bool muted;
    } sinks[32];
} aurade_session_state_t;

const char *config_get_state_path(void);
void session_state_load(aurade_session_state_t *st);
void session_state_save(const aurade_session_state_t *st);
double session_state_get_sink_volume(const aurade_session_state_t *st, const char *sink_name, double fallback);
void session_state_set_sink_volume(aurade_session_state_t *st, const char *sink_name, double volume, bool muted);
void session_state_set_brightness(aurade_session_state_t *st, double brightness);

#endif // AURADE_CONFIG_H
