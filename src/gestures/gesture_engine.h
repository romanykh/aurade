#ifndef AURADE_GESTURE_ENGINE_H
#define AURADE_GESTURE_ENGINE_H

#include "../server.h"
#include <linux/input.h>
#include <wayland-server-core.h>

#define MAX_TOUCH_SLOTS 10

/* Gesture travel is reported in "reference pixels" of a 1920x1080 canvas.
 * Touchpad units are converted with ref/screen-independent factors, so the same
 * finger distance performs the same gesture on a 1080p and a 4K display. */
#define GE_REF_W 1920.0
#define GE_REF_H 1080.0

typedef struct {
    int tracking_id;
    double x, y;          // raw device units
    bool active;
    unsigned char pos_mask; // bit 0: X seen, bit 1: Y seen for this contact
} touch_slot_t;

typedef enum {
    TRACKER_IDLE = 0,
    TRACKER_POSSIBLE,
    TRACKER_ACTIVE,
} tracker_state_t;

typedef struct gesture_engine {
    aurade_server_t *server;

    // Hardware evdev device integrated into main event loop
    int evdev_fd;
    char device_path[256];
    char device_name[256];
    struct wl_event_source *event_source;

    // Raw touch coordinates are in device units. These ranges/scales convert
    // them to reference pixels so gesture thresholds mean the same physical
    // distance on every touchpad and every display.
    int abs_x_min, abs_x_max;
    int abs_y_min, abs_y_max;
    double px_scale_x, px_scale_y;

    // Multi-touch slots (evdev protocol B)
    touch_slot_t slots[MAX_TOUCH_SLOTS];
    int active_slot;
    int finger_count;
    int last_count;

    // Gesture tracking state
    tracker_state_t state;
    gesture_action_type_t current_action;
    int gesture_fingers;         // fingers when the action was recognised
    uint64_t gesture_slots;      // exact contacts that started the gesture
    bool have_last_centroid;
    bool wait_for_release;       // ignore leftover fingers after an end

    double start_cx, start_cy;
    double start_radius;         // 0 = contact spread too small to pinch
    double last_cx, last_cy;
    double last_radius;
    uint64_t last_time_us;
    uint64_t last_motion_us;     // last frame in which the centroid actually moved
    uint64_t count_change_us;    // last time the finger count changed

    double velocity_x, velocity_y;
    double velocity_scale;       // radial velocity for pinch (normalized radius/sec)

    // Latest finger travel since the gesture origin. Used to suppress
    // velocity-based snapping when the fingers barely moved: a noisy 10 px
    // frame can otherwise report thousands of px/s and fling an overlay open.
    double travel_x, travel_y;

    // Two-finger touchpad edge swipe for Notification Center
    bool nc_two_finger_active;
    bool nc_two_finger_triggered;
    bool nc_in_edge;
    double nc_start_ccx, nc_start_ccy;

    // Time source. NULL uses the real clock; the offline gesture simulation
    // replaces it with a deterministic counter.
    uint64_t (*now_fn)(void);

    // IPC Server Socket
    int ipc_fd;
    int ipc_client_fd;
    struct wl_event_source *ipc_event_source;
    int remote_fd;
    struct wl_event_source *remote_event_source;
} gesture_engine_t;

gesture_engine_t *gesture_engine_create(aurade_server_t *server, struct wl_event_loop *loop);
void gesture_engine_destroy(gesture_engine_t *ge);

/* Feed one evdev event into the recogniser. Exposed so the offline simulation
 * (tools/gesture_sim.c) can drive the exact same code path as the kernel. */
void gesture_engine_handle_event(gesture_engine_t *ge, const struct input_event *ev);

// IPC broadcast
void gesture_engine_broadcast_telemetry(gesture_engine_t *ge, const char *status);

#endif // AURADE_GESTURE_ENGINE_H
