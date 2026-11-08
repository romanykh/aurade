#ifndef AURADE_SERVER_H
#define AURADE_SERVER_H

#include <wayland-server-core.h>
#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output_layout.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/xwayland.h>

#include "config/config.h"
#include "gestures/physics.h"
#include "util/apps.h"
#include "render/cairo_buffer.h"

#define MAX_WORKSPACES 16
#define DEFAULT_WORKSPACES 4
#define NUM_WORKSPACES DEFAULT_WORKSPACES
typedef enum {
    CURSOR_PASSTHROUGH,
    CURSOR_MOVE,
    CURSOR_RESIZE,
} cursor_mode_t;

typedef enum {
    GESTURE_MODE_IDLE = 0,
    GESTURE_MODE_MISSION_CONTROL,
    GESTURE_MODE_WORKSPACE_CAROUSEL,
    GESTURE_MODE_LAUNCHPAD,
    GESTURE_MODE_SHOW_DESKTOP,
    GESTURE_MODE_CONTROL_CENTER,
} gesture_mode_t;

typedef enum {
    SNAP_NONE = 0,
    SNAP_MAXIMIZE,
    SNAP_LEFT,
    SNAP_RIGHT,
} snap_action_t;

struct aurade_server;

typedef enum {
    VIEW_ANIM_NONE = 0,
    VIEW_ANIM_MAP,
    VIEW_ANIM_MINIMIZE,
    VIEW_ANIM_RESTORE,
    VIEW_ANIM_GEOMETRY,
} view_anim_type_t;

typedef struct aurade_view {
    struct wl_list link;
    struct aurade_server *server;
    uint64_t id;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_xwayland_surface *xwayland_surface;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_tree *content_tree; // xdg surface, aligned to the window rect
    struct wlr_scene_blur *blur_node;
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    int workspace;

    // Normal geometry
    int x, y;
    int width, height;

    // Mission Control & Animations Target / Interpolated geometry
    double anim_x, anim_y;
    double anim_scale;
    double current_scale;
    double anim_alpha;
    double mc_target_x, mc_target_y, mc_target_w, mc_target_h;

    // Window fluid animation physics
    view_anim_type_t anim_type;
    spring_simulation_t anim_spring;
    double anim_start_x, anim_start_y, anim_start_w, anim_start_h;
    double anim_target_x, anim_target_y, anim_target_w, anim_target_h;
    double anim_start_scale, anim_target_scale;
    double anim_start_opacity, anim_target_opacity;

    // State
    bool maximized;
    bool minimized;
    bool fullscreen;
    bool demands_attention;
    bool pending_configure; // geometry changed before the xdg surface was initialized
    bool decoration_pending; // reply to xdg-decoration after initial configure
    uint64_t last_configure_ms; // last time we sent a toplevel configure
    int prev_x, prev_y, prev_w, prev_h;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener commit;
    struct wl_listener configure;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_minimize;
    struct wl_listener request_fullscreen;
    struct wl_listener set_title;
    struct wl_listener decoration_request_mode;
    struct wl_listener decoration_destroy;
    struct wl_listener xwayland_associate;
    struct wl_listener xwayland_dissociate;
    struct wl_listener xwayland_request_configure;
    struct wl_listener xwayland_request_activate;
    struct wl_listener xwayland_set_class;
    struct wl_listener xwayland_set_override_redirect;
    struct wl_listener content_tree_destroy;
} aurade_view_t;

typedef struct aurade_output {
    struct wl_list link;
    struct aurade_server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct wl_listener frame;
    struct wl_listener destroy;
    int width, height;
    struct timespec last_time;
} aurade_output_t;

// Forward declarations of shell components
struct aurade_topbar;
struct aurade_dock;
struct aurade_mission_control;
struct aurade_launchpad;
struct aurade_control_center;
struct aurade_osd;
struct aurade_wallpaper;
struct aurade_screenshot;
struct aurade_notifications;
struct gesture_engine;

typedef struct aurade_server {
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_compositor *compositor;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_xwayland *xwayland;
    struct wl_listener xwayland_ready;
    struct wl_listener xwayland_new_surface;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
    struct wl_listener new_xdg_decoration;

    // Protocols that make AuraDE usable as a daily-driver desktop
    struct wlr_screencopy_manager_v1 *screencopy;
    struct wlr_data_control_manager_v1 *data_control;
    struct wlr_ext_data_control_manager_v1 *ext_data_control;
    struct wlr_primary_selection_v1_device_manager *primary_selection_mgr;
    struct wlr_presentation *presentation;
    struct wlr_xdg_output_manager_v1 *xdg_output_manager;
    struct wlr_xdg_activation_v1 *xdg_activation;
    struct wl_listener request_activate;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    // wlroots gestures listeners
    struct wl_listener swipe_begin;
    struct wl_listener swipe_update;
    struct wl_listener swipe_end;
    struct wl_listener pinch_begin;
    struct wl_listener pinch_update;
    struct wl_listener pinch_end;

    struct wlr_seat *seat;
    struct wl_listener new_input;
    struct wl_listener keyboard_key;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;
    struct wl_listener request_start_drag;
    struct wl_listener start_drag;
    struct wl_listener drag_destroy;
    struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
    struct wl_listener request_set_shape;

    // Relative pointer & Pointer constraints (games, Minecraft, FPS camera control)
    struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_pointer_constraint_v1 *active_constraint;
    struct wl_listener new_pointer_constraint;
    struct wl_listener constraint_destroy;
    struct wl_listener constraint_set_region;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;
    struct wl_list input_devices;

    // Primary output dimensions. Kept in sync so shell geometry and workspace
    // sliding are correct on non-1920x1080 screens.
    int screen_width, screen_height;
    aurade_view_t *focused_view;

    // 1-second clock tick for the menu-bar clock
    struct wl_event_source *clock_timer;

    // Workspaces
    int num_workspaces;
    struct wlr_scene_tree *workspace_trees[MAX_WORKSPACES];
    int current_workspace;
    double workspace_offset;           // 1:1 pixel offset during swipe
    spring_simulation_t workspace_spring;
    bool workspace_animating;

    // Views
    struct wl_list views;
    uint64_t next_view_id;
    aurade_view_t *grabbed_view;
    cursor_mode_t cursor_mode;
    uint32_t pointer_buttons_forwarded;
    uint32_t grab_button;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

    // Shell scene trees
    struct wlr_scene_tree *wallpaper_tree;
    struct wlr_scene_tree *shell_tree;
    struct wlr_scene_tree *overlay_tree;
    struct wlr_scene_tree *dock_tree;
    struct wlr_scene_tree *popup_tree;
    struct wlr_scene_tree *drag_tree;

    // Shell components
    struct aurade_wallpaper *wallpaper;
    struct aurade_topbar *topbar;
    struct aurade_dock *dock;
    struct aurade_mission_control *mission_control;
    struct aurade_launchpad *launchpad;
    struct aurade_control_center *control_center;
    struct aurade_osd *osd;
    struct aurade_screenshot *screenshot;
    struct aurade_notifications *notifications;
    struct aurade_music_pill *music_pill;

    // Gesture State Machine (Direct Manipulation + Springs)
    gesture_mode_t gesture_mode;
    double gesture_progress;           // Direct tracking: 0.0 -> 1.0 (or negative for rubber-band)
    double gesture_base_offset;        // Workspace carousel position when a swipe began
    spring_simulation_t gesture_spring;
    bool gesture_in_progress;
    bool gesture_animating;
    bool gesture_is_closing;
    double gesture_start_progress;

    // Hardware evdev gesture engine
    struct gesture_engine *gesture_engine;

    // Configuration & Applications
    aurade_config_t config;
    desktop_apps_t *apps;

    // Socket IPC for settings visualizer
    int ipc_socket_fd;
    int ipc_client_fd;
    // Config hot‑reload watcher
    int config_inotify_fd;
    struct wl_event_source *config_inotify_source;

    // Hardware Session State (Volume, Brightness & Earbuds Protection)
    aurade_session_state_t state;
    char current_audio_sink[128];
    pid_t audio_subscribe_pid;
    int audio_subscribe_fd;
    struct wl_event_source *audio_subscribe_source;

    // Window snapping & maximize preview
    snap_action_t snap_action;
    cairo_buffer_t *snap_preview_buffer;
    struct wlr_scene_buffer *snap_preview_scene;
} aurade_server_t;

// View operations
void view_focus(aurade_view_t *view);
void view_unfocus(aurade_server_t *server);
void view_tile(aurade_view_t *view, bool left_half);
void view_toggle_maximize(aurade_view_t *view);
void view_set_fullscreen(aurade_view_t *view, bool fullscreen);
void view_minimize(aurade_view_t *view);
void view_restore(aurade_view_t *view);
void view_close(aurade_view_t *view);
void view_set_scale(aurade_view_t *view, double scale_x, double scale_y);
void view_set_opacity(aurade_view_t *view, float opacity);
void view_update_workspace_clip(aurade_view_t *view);
void view_update_blur(aurade_view_t *view);
void view_anim_settle(aurade_view_t *view);
const char *view_get_app_id(aurade_view_t *view);
const char *view_get_title(aurade_view_t *view);
float view_get_target_opacity(aurade_view_t *view);
float view_effective_alpha(aurade_view_t *view);
void server_export_running_windows(aurade_server_t *server);
void server_tick_view_animations(aurade_server_t *server, double dt);
aurade_view_t *server_view_at(aurade_server_t *server, double lx, double ly,
                              struct wlr_surface **surface, double *sx, double *sy);

// Workspace operations
void server_switch_workspace(aurade_server_t *server, int target_workspace, bool animate);
void server_update_workspace_positions(aurade_server_t *server);
void server_workspace_auto_focus(aurade_server_t *server);

// Gesture dispatcher
void server_handle_gesture_begin(aurade_server_t *server, gesture_action_type_t action);
void server_handle_gesture_update(aurade_server_t *server, double delta_x, double delta_y, double scale);
void server_handle_gesture_end(aurade_server_t *server, bool cancelled, double velocity_x, double velocity_y);
void schedule_all_outputs(aurade_server_t *server);
void server_update_blur_config(aurade_server_t *server);
void server_close_overlay(aurade_server_t *server);
void server_set_pointer_constraint(aurade_server_t *server,
                                  struct wlr_pointer_constraint_v1 *constraint);

// Animation tick (called on every frame)
void server_tick_animations(aurade_server_t *server, double dt);

// Hardware media controls
void server_adjust_volume(aurade_server_t *server, double delta);
void server_toggle_mute(aurade_server_t *server);
void server_adjust_brightness(aurade_server_t *server, double delta);
void server_update_touchpad_settings(aurade_server_t *server);
void server_check_audio_sink(aurade_server_t *server);
void server_restore_hardware_state(aurade_server_t *server);

typedef struct aurade_input_device {
    struct wl_list link;
    struct wlr_input_device *wlr_device;
    struct wl_listener destroy;
    aurade_server_t *server;
} aurade_input_device_t;

#endif // AURADE_SERVER_H
