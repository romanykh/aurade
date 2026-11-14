#ifndef AURADE_CONTROL_CENTER_H
#define AURADE_CONTROL_CENTER_H

#include "../server.h"
#include "../render/cairo_buffer.h"
#include <pthread.h>

/* Interactive regions of the panel, used for hover feedback and hit testing. */
typedef enum {
    CC_MODULE_NONE = 0,
    CC_MODULE_WIFI,
    CC_MODULE_WIFI_TOGGLE,
    CC_MODULE_BLUETOOTH,
    CC_MODULE_BLUETOOTH_TOGGLE,
    CC_MODULE_HOTSPOT,
    CC_MODULE_FOCUS,
    CC_MODULE_AURA_CONNECT,
    CC_MODULE_SCREEN_KEYBOARD,
    CC_MODULE_DISPLAY,
    CC_MODULE_DISPLAY_DETAIL,
    CC_MODULE_SOUND,
    CC_MODULE_SOUND_DETAIL,
    CC_MODULE_NP_CARD,
    CC_MODULE_PREV,
    CC_MODULE_PLAY,
    CC_MODULE_NEXT,
    CC_MODULE_BATTERY,
    CC_MODULE_USER,
    CC_MODULE_ACCESSIBILITY,
    CC_MODULE_SETTINGS,
} cc_module_t;

typedef enum {
    CC_PAGE_MAIN = 0,
    CC_PAGE_WIFI,
    CC_PAGE_BLUETOOTH,
    CC_PAGE_SOUND,
} cc_page_t;

typedef struct {
    char ssid[64];
    int signal;
    bool secure;
    bool active;
    double y, h;
} cc_wifi_item_t;

typedef struct {
    char mac[32];
    char name[64];
    bool connected;
    double y, h;
} cc_bt_item_t;

typedef struct {
    char name[128];
    char description[128];
    bool is_default;
    double y, h;
} cc_audio_device_t;

typedef struct {
    double x, y, w, h;
} cc_sub_rect_t;

typedef struct {
    char profile[64];
    char label[32];
    cc_sub_rect_t rect;
} cc_bt_codec_t;

typedef struct {
    bool wifi_enabled;
    bool wifi_available;
    char wifi_ssid[96];

    bool bluetooth_enabled;
    bool bluetooth_available;
    char bluetooth_device[96];

    bool dark_mode;
    bool dark_mode_available;

    bool dnd_enabled;
    bool dnd_available;

    double brightness;
    bool brightness_available;

    double volume;
    bool volume_available;
    bool volume_muted;
    bool volume_uses_pactl;

    bool np_available;
    bool np_active;
    bool np_playing;
    char np_title[160];
    char np_artist[160];

    int battery_percent;
    bool battery_charging;
    bool battery_available;
    bool hotspot_enabled;
    bool hotspot_available;
    bool aura_connect_enabled;
    bool screen_keyboard_enabled;

    cc_wifi_item_t wifi_items[24];
    int wifi_count;

    cc_bt_item_t bt_items[24];
    int bt_count;

    char bt_codec_card[64];
    char bt_codec_dev_name[64];
    cc_bt_codec_t bt_codecs[10];
    int bt_codec_count;
    int bt_codec_active;

    cc_audio_device_t audio_outputs[12];
    int audio_output_count;
    cc_audio_device_t audio_inputs[12];
    int audio_input_count;
} cc_probe_snapshot_t;

typedef struct aurade_control_center {
    aurade_server_t *server;
    cairo_buffer_t *buffer;
    struct wlr_scene_buffer *scene_buffer;
    struct wlr_scene_blur *blur_node;

    int screen_width, screen_height;
    bool active;
    double progress; // 0.0 (hidden) to 1.0 (fully open)

    // Current page inside Control Center
    cc_page_t current_page;

    // Live system state. These are refreshed from NetworkManager, BlueZ,
    // GSettings, brightnessctl and PipeWire/PulseAudio when the drawer opens
    // (and shortly after an action), so the controls are not decorative.
    bool wifi_enabled;
    bool wifi_available;
    char wifi_ssid[96];

    bool bluetooth_enabled;
    bool bluetooth_available;
    char bluetooth_device[96];

    bool dark_mode;
    bool dark_mode_available;

    bool dnd_enabled;
    bool dnd_available;

    double brightness; // 0.0 to 1.0
    bool brightness_available;

    double volume;     // 0.0 to 1.0
    bool volume_available;
    bool volume_muted;
    bool volume_uses_pactl;

    // Subpage Wi-Fi data
    cc_wifi_item_t wifi_items[24];
    int wifi_count;
    char selected_ssid[64];
    char connecting_ssid[64];
    char wifi_password[128];
    bool password_prompt_open;
    bool wifi_connecting;
    bool wifi_scanning;
    uint64_t wifi_scan_until_ms;
    char wifi_status_msg[96];

    // Subpage Bluetooth data
    cc_bt_item_t bt_items[24];
    int bt_count;
    char bt_connecting_mac[32];
    bool bt_connecting;
    bool bt_disconnecting;
    bool bt_scanning;
    uint64_t bt_scan_until_ms;
    char bt_status_msg[96];
    char bt_codec_card[64];
    char bt_codec_dev_name[64];
    cc_bt_codec_t bt_codecs[10];
    int bt_codec_count;
    int bt_codec_active;
    int bt_codec_hover;

    // Subpage Sound data
    cc_audio_device_t audio_outputs[12];
    int audio_output_count;
    cc_audio_device_t audio_inputs[12];
    int audio_input_count;

    // Subpage click & interaction geometry
    cc_sub_rect_t sub_back_rect;
    cc_sub_rect_t sub_toggle_rect;
    cc_sub_rect_t sub_settings_rect;
    cc_sub_rect_t sub_scan_rect;
    cc_sub_rect_t pw_input_rect;
    cc_sub_rect_t pw_connect_rect;
    cc_sub_rect_t pw_cancel_rect;
    cc_sub_rect_t sound_sub_slider_rect;
    cc_sub_rect_t sound_sub_mute_rect;

    // Subpage hover & drag states
    int sub_hover_row;
    bool sub_hover_back;
    bool sub_hover_toggle;
    bool sub_hover_settings;
    bool sub_hover_scan;
    bool sub_hover_connect;
    bool sub_hover_cancel;
    bool sub_hover_mute;
    bool sub_dragging_slider;

    // Subpage & toggle animations (60 FPS)
    struct wl_event_source *anim_timer;
    cc_page_t prev_page;
    double page_anim;       // 0.0 -> 1.0 page slide/fade progress
    int page_anim_dir;      // +1 opening subpage, -1 returning to main
    double anim_panel_h;    // smoothly interpolated panel height
    double wifi_toggle_pos; // 0.0 -> 1.0 toggle switch thumb position
    double bt_toggle_pos;   // 0.0 -> 1.0 toggle switch thumb position
    double spinner_phase;   // radians for connecting/scanning spinner
    double row_hover_alpha[24];

    // Now Playing (MPRIS through playerctl). The module only appears while a
    // player is running, mirroring fluid where the media tile is dynamic.
    bool np_available;  // playerctl exists on this machine
    bool np_active;     // at least one MPRIS player is running
    bool np_playing;
    char np_title[160];
    char np_artist[160];

    int battery_percent;
    bool battery_charging;
    bool battery_available;
    bool hotspot_enabled;
    bool hotspot_available;
    bool aura_connect_enabled;
    bool screen_keyboard_enabled;

    // 0 = none, 1 = brightness, 2 = volume. A slider changes its visual value
    // while dragging and applies the final value on release.
    int dragging;

    // Hover / press feedback, so the panel feels alive like fluid.
    int hover;    // cc_module_t
    int pressed;  // cc_module_t

    struct wl_event_source *refresh_timer;
    struct wl_event_source *np_timer;

    // Async probe worker fields
    pthread_mutex_t probe_mutex;
    bool probe_in_progress;
    bool probe_has_result;
    bool probe_wifi_scan_requested;
    cc_page_t probe_requested_page;
    int probe_event_fd;
    struct wl_event_source *probe_event_source;
    cc_probe_snapshot_t probe_result;
} aurade_control_center_t;

aurade_control_center_t *control_center_create(aurade_server_t *server);
void control_center_resize(aurade_control_center_t *cc, int width, int height);
void control_center_begin(aurade_control_center_t *cc);
void control_center_update_progress(aurade_control_center_t *cc, double progress);
void control_center_end(aurade_control_center_t *cc, bool opened);
bool control_center_handle_click(aurade_control_center_t *cc, double x, double y);
void control_center_handle_motion(aurade_control_center_t *cc, double x, double y);
void control_center_handle_release(aurade_control_center_t *cc, double x, double y);
bool control_center_handle_key(aurade_control_center_t *cc, uint32_t keyval, bool pressed);
void control_center_render(aurade_control_center_t *cc);
void control_center_destroy(aurade_control_center_t *cc);

/* Geometry helpers (also used by tools/cc_preview.c). */
int control_center_width(const aurade_control_center_t *cc);
int control_center_height(const aurade_control_center_t *cc);
const char *control_center_cursor(aurade_control_center_t *cc, double x, double y);
bool control_center_is_open(const aurade_control_center_t *cc);
void control_center_sync_audio(aurade_control_center_t *cc, double volume, bool muted);
void control_center_sync_brightness(aurade_control_center_t *cc, double brightness);

#endif // AURADE_CONTROL_CENTER_H
