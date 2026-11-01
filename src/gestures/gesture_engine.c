#include "gesture_engine.h"
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

// A swipe must travel this far before its release velocity is allowed to fling
// an overlay open or switch a workspace. Without it, a 10 px movement followed
// by a finger lift reports a huge instantaneous velocity and triggers actions
// the user never intended.
#define FLICK_MIN_TRAVEL_PX 90.0

// Reject near-diagonal swipes: without an axis lock a 45-degree movement is
// ambiguous and used to flip between horizontal and vertical actions.
#define AXIS_DOMINANCE 1.45

// Recognition slop. Below this the fingers are considered to be resting.
// Roughly 4 mm on a typical pad: anything smaller made three-finger gestures
// fire while a hand was merely settling on the touchpad.
#define DEADZONE_MIN_PX 80.0

// Pinch recognition needs a meaningful initial spread, otherwise the radius
// ratio is dominated by contact jitter. All values are in reference pixels.
#define PINCH_MIN_RADIUS_PX 40.0
#define PINCH_SCALE_THRESHOLD 0.15
#define PINCH_MIN_RADIUS_DELTA_PX 30.0

// A pinch must move the fingers apart/together more than the whole hand
// travels. Without this, the natural splay of a four-finger swipe reads as a
// pinch and Launchpad opens instead of the workspace switching.
#define PINCH_DOMINANCE 1.25

// Firmware "touch jumps" (this Dell pad reports them) would otherwise teleport
// the gesture. Clamp a single frame to a physically plausible speed instead of
// rejecting it, so the gesture catches up instead of desynchronising.
#define JUMP_SPEED_FACTOR 1.6
#define JUMP_MIN_STEP_PX 90.0

// Wait for the finger count to settle before locking a gesture family. Placing
// three fingers and then a fourth a frame later must resolve as a four-finger
// gesture, not as a three-finger one.
#define COUNT_SETTLE_US 35000ULL

// Velocity is smoothed with a wall-clock time constant rather than per-frame
// weight, so the feel does not change between 60 Hz and 125 Hz touchpads.
#define VELOCITY_TAU_SEC 0.045
#define MAX_VELOCITY_PX_S 9000.0

// If the fingers rest for this long, move the gesture origin to where they are
// now. Slow drift while resting otherwise accumulates into a phantom swipe.
#define REST_REBASE_US 350000ULL

// Release velocity is stale after a pause: decay it with this time constant so
// "swipe, stop, lift" animates back instead of flicking.
#define VELOCITY_STALE_TAU_SEC 0.05

// More contacts than this is a palm, not a gesture.
#define MAX_GESTURE_FINGERS 5

static uint64_t get_time_us(void) {
    // Monotonic: an NTP step or manual clock change must not turn into a
    // multi-second velocity spike and fling the user into another workspace.
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint64_t ge_now(const gesture_engine_t *ge) {
    return ge->now_fn ? ge->now_fn() : get_time_us();
}

static bool test_bit(const unsigned long *bits, unsigned bit) {
    return (bits[bit / (8 * sizeof(unsigned long))] &
            (1UL << (bit % (8 * sizeof(unsigned long))))) != 0;
}

// Detect the touchpad by its capabilities instead of trusting a fixed
// /dev/input/eventN number. Event numbers are assigned in probe order and
// change between boots (and when USB devices are attached).
static char *find_touchpad_device(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return NULL;

    struct dirent *ent;
    char path[512];
    char *best = NULL;
    int best_score = -1;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        unsigned long abs_bits[8] = {0};
        bool usable = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0 &&
                      test_bit(abs_bits, ABS_MT_SLOT) &&
                      test_bit(abs_bits, ABS_MT_POSITION_X) &&
                      test_bit(abs_bits, ABS_MT_POSITION_Y);

        if (usable) {
            struct input_absinfo xi, yi;
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &xi) < 0 ||
                ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &yi) < 0 ||
                xi.maximum <= xi.minimum || yi.maximum <= yi.minimum) {
                usable = false;
            }
        }

        int score = -1;
        if (usable) {
            char name[256] = {0};
            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) name[0] = '\0';

            score = 1;
            if (strcasestr(name, "touchpad") || strcasestr(name, "trackpad")) {
                score += 10;
            }
            // The mouse emulation node of a touchpad does not expose
            // ABS_MT_SLOT, but still guard against a real mouse that does.
            if (strcasestr(name, "mouse")) {
                score -= 5;
            }
        }

        close(fd);

        if (score > best_score) {
            free(best);
            best = strdup(path);
            best_score = score;
        }
    }
    closedir(dir);
    return best;
}

static void setup_ipc_socket(gesture_engine_t *ge) {
    unlink("/tmp/aurade-gesture.sock");
    ge->ipc_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ge->ipc_fd < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/aurade-gesture.sock", sizeof(addr.sun_path) - 1);

    bind(ge->ipc_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ge->ipc_fd, 4);
    ge->ipc_client_fd = -1;
}

void gesture_engine_broadcast_telemetry(gesture_engine_t *ge, const char *status) {
    if (!ge || ge->ipc_fd < 0) return;

    if (ge->ipc_client_fd < 0) {
        int client = accept(ge->ipc_fd, NULL, NULL);
        if (client >= 0) {
            fcntl(client, F_SETFL, O_NONBLOCK);
            ge->ipc_client_fd = client;
        }
    }

    if (ge->ipc_client_fd >= 0) {
        char json[1024];
        int count = ge->finger_count;
        int len = snprintf(json, sizeof(json),
                           "{\"fingers\":%d,\"gesture\":\"%s\",\"progress\":%.2f,"
                           "\"action\":%d,\"vx\":%.1f,\"vy\":%.1f,\"slots\":[",
                           count, status ? status : "IDLE", ge->server->gesture_progress,
                           ge->current_action, ge->velocity_x, ge->velocity_y);

        for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
            if (ge->slots[i].active) {
                len += snprintf(json + len, sizeof(json) - len,
                                "{\"slot\":%d,\"x\":%.1f,\"y\":%.1f},",
                                i, ge->slots[i].x, ge->slots[i].y);
            }
        }
        if (json[len - 1] == ',') json[len - 1] = ']';
        else len += snprintf(json + len, sizeof(json) - len, "]");

        len += snprintf(json + len, sizeof(json) - len, "}\n");
        ssize_t ret = send(ge->ipc_client_fd, json, len, MSG_NOSIGNAL);
        if (ret < 0) {
            close(ge->ipc_client_fd);
            ge->ipc_client_fd = -1;
        }
    }
}

static const char *action_name(gesture_action_type_t action) {
    switch (action) {
        case ACTION_MISSION_CONTROL:
        case ACTION_APP_EXPOSE:      return "mission_control";
        case ACTION_WORKSPACE_NEXT:  return "workspace_next";
        case ACTION_WORKSPACE_PREV:  return "workspace_prev";
        case ACTION_LAUNCHPAD:       return "launchpad";
        case ACTION_SHOW_DESKTOP:    return "show_desktop";
        case ACTION_CONTROL_CENTER:  return "control_center";
        case ACTION_CUSTOM:          return "custom";
        case ACTION_NONE:
        default:                     return "none";
    }
}

static gesture_action_type_t resolve_action(aurade_server_t *server, int fingers,
                                            int dir_x, int dir_y, int pinch) {
    aurade_config_t *cfg = &server->config;

    // Two-finger drags are scrolling, not gestures.
    if (fingers < 3) {
        return ACTION_NONE;
    }
    // Any number of fingers above four still uses the four-finger bindings.
    int family = (fingers == 3) ? 3 : 4;

    if (pinch < 0) {
        return (family == 3) ? cfg->pinch_in_3.type : cfg->pinch_in_4.type;
    } else if (pinch > 0) {
        return (family == 3) ? cfg->pinch_out_3.type : cfg->pinch_out_4.type;
    }

    if (dir_x != 0) {
        if (dir_x > 0) {
            return (family == 3) ? cfg->swipe_right_3.type : cfg->swipe_right_4.type;
        } else {
            return (family == 3) ? cfg->swipe_left_3.type : cfg->swipe_left_4.type;
        }
    }

    if (dir_y != 0) {
        if (dir_y < 0) {
            return (family == 3) ? cfg->swipe_up_3.type : cfg->swipe_up_4.type;
        } else {
            return (family == 3) ? cfg->swipe_down_3.type : cfg->swipe_down_4.type;
        }
    }

    return ACTION_NONE;
}

// A pinch ratio is only meaningful with a wide enough initial spread.
static double pinch_base(double radius) {
    return radius >= PINCH_MIN_RADIUS_PX ? radius : 0.0;
}

static void reset_tracking(gesture_engine_t *ge) {
    ge->state = TRACKER_IDLE;
    ge->current_action = ACTION_NONE;
    ge->gesture_fingers = 0;
    ge->gesture_slots = 0;
    ge->have_last_centroid = false;
    ge->last_count = 0;
    ge->start_radius = 0.0;
    ge->last_radius = 0.0;
    ge->travel_x = 0.0;
    ge->travel_y = 0.0;
    ge->velocity_x = 0.0;
    ge->velocity_y = 0.0;
    ge->velocity_scale = 0.0;
}

// Finish the active gesture and ignore the remaining fingers until they are
// all lifted. Without this guard, lifting one finger out of four would end the
// gesture and the three still-down fingers would immediately start a new one.
static void finish_gesture(gesture_engine_t *ge, bool cancelled) {
    if (ge->state != TRACKER_ACTIVE) {
        reset_tracking(ge);
        ge->wait_for_release = true;
        return;
    }

    double travel = hypot(ge->travel_x, ge->travel_y);
    uint64_t now = ge_now(ge);
    double idle = now > ge->last_motion_us
                  ? (double)(now - ge->last_motion_us) / 1000000.0 : 0.0;

    // Swipe, pause, lift must not fling: the fingers have not moved for a
    // while, so the stored velocity is stale.
    double decay = exp(-idle / VELOCITY_STALE_TAU_SEC);
    double vx = ge->velocity_x * decay;
    double vy = ge->velocity_y * decay;

    // Velocity only means "the user flicked" after the fingers have moved far
    // enough to make that plausible. Tiny motions still animate back.
    if (travel < FLICK_MIN_TRAVEL_PX || cancelled) {
        vx = 0.0;
        vy = 0.0;
    }

    fprintf(stderr, "[AuraDE] Gesture: END fingers=%d dist=%.0f vx=%.0f vy=%.0f%s\n",
            ge->gesture_fingers, travel, vx, vy, cancelled ? " (cancelled)" : "");
    server_handle_gesture_end(ge->server, cancelled, vx, vy);
    gesture_engine_broadcast_telemetry(ge, "END");

    reset_tracking(ge);
    ge->wait_for_release = true;
}

static void update_pixel_scale(gesture_engine_t *ge) {
    int xr = ge->abs_x_max - ge->abs_x_min;
    int yr = ge->abs_y_max - ge->abs_y_min;

    // Reference pixels, not screen pixels: a gesture must need the same finger
    // travel on a 1080p laptop panel and on a 4K monitor, and it must not
    // change when the user changes the output resolution. 1920x1080 keeps the
    // historical 1:1 behaviour on the most common display size.
    ge->px_scale_x = xr > 0 ? GE_REF_W / (double)xr : 1.0;
    ge->px_scale_y = yr > 0 ? GE_REF_H / (double)yr : 1.0;
}

static void start_possible(gesture_engine_t *ge, double ccx, double ccy,
                           double radius, int count, uint64_t now) {
    ge->state = TRACKER_POSSIBLE;
    ge->start_cx = ccx;
    ge->start_cy = ccy;
    ge->start_radius = pinch_base(radius);
    ge->last_cx = ccx;
    ge->last_cy = ccy;
    ge->last_radius = radius;
    ge->last_time_us = now;
    ge->last_motion_us = now;
    ge->count_change_us = now;
    ge->velocity_x = 0.0;
    ge->velocity_y = 0.0;
    ge->velocity_scale = 0.0;
    ge->last_count = count;
    ge->have_last_centroid = true;
    ge->travel_x = 0.0;
    ge->travel_y = 0.0;
}

static void rebase_origin(gesture_engine_t *ge, double ccx, double ccy,
                          double radius, uint64_t now) {
    ge->start_cx = ccx;
    ge->start_cy = ccy;
    ge->start_radius = pinch_base(radius);
    ge->last_cx = ccx;
    ge->last_cy = ccy;
    ge->last_radius = radius;
    ge->last_time_us = now;
    ge->velocity_x = 0.0;
    ge->velocity_y = 0.0;
    ge->velocity_scale = 0.0;
    ge->travel_x = 0.0;
    ge->travel_y = 0.0;
}

static uint64_t active_slot_mask(const gesture_engine_t *ge) {
    uint64_t mask = 0;
    for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
        if (ge->slots[i].active && ge->slots[i].pos_mask == 0x3) {
            mask |= 1ULL << i;
        }
    }
    return mask;
}

static int mask_popcount(uint64_t mask) {
    int n = 0;
    while (mask) {
        mask &= mask - 1;
        n++;
    }
    return n;
}

static void process_multitouch_frame(gesture_engine_t *ge) {
    uint64_t active = active_slot_mask(ge);
    int count = mask_popcount(active);
    ge->finger_count = count;

    if (ge->wait_for_release) {
        if (count < 2) ge->wait_for_release = false;
        return;
    }

    if (count < 2) {
        if (ge->state == TRACKER_ACTIVE) finish_gesture(ge, false);
        reset_tracking(ge);
        return;
    }

    // A palm or a whole resting hand: never a gesture. Cancel anything that is
    // already running rather than dragging an overlay around.
    if (count > MAX_GESTURE_FINGERS) {
        if (ge->state == TRACKER_ACTIVE) finish_gesture(ge, true);
        reset_tracking(ge);
        ge->wait_for_release = true;
        return;
    }

    // Once a gesture is running, keep tracking exactly the contacts that
    // started it. A fourth finger brushing the pad (or one finger of a pinch
    // drifting) must not shift the centroid and make the overlay jump.
    uint64_t tracking = active;
    if (ge->state == TRACKER_ACTIVE) {
        if ((active & ge->gesture_slots) != ge->gesture_slots) {
            // One of the fingers that started the gesture was lifted.
            finish_gesture(ge, false);
            return;
        }
        tracking = ge->gesture_slots;
    }

    update_pixel_scale(ge);

    double sum_x = 0.0, sum_y = 0.0;
    for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
        if (tracking & (1ULL << i)) {
            sum_x += ge->slots[i].x;
            sum_y += ge->slots[i].y;
        }
    }
    int tracked_count = mask_popcount(tracking);

    double cx = sum_x / tracked_count;
    double cy = sum_y / tracked_count;
    double ccx = (cx - ge->abs_x_min) * ge->px_scale_x;
    double ccy = (cy - ge->abs_y_min) * ge->px_scale_y;

    double sum_dist = 0.0;
    for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
        if (tracking & (1ULL << i)) {
            double dx = (ge->slots[i].x - cx) * ge->px_scale_x;
            double dy = (ge->slots[i].y - cy) * ge->px_scale_y;
            sum_dist += hypot(dx, dy);
        }
    }
    double radius = sum_dist / tracked_count;

    uint64_t now = ge_now(ge);

    // Firmware touch jumps: clamp the per-frame centroid movement to a
    // plausible physical speed. The frame is still used, just with a bounded
    // step, and its velocity is not trusted.
    bool jumped = false;
    if (ge->have_last_centroid && ge->state != TRACKER_IDLE && ge->last_time_us) {
        double jump_dt = (double)(now - ge->last_time_us) / 1000000.0;
        double step_limit = MAX_VELOCITY_PX_S * JUMP_SPEED_FACTOR * jump_dt;
        if (step_limit < JUMP_MIN_STEP_PX) step_limit = JUMP_MIN_STEP_PX;

        double sx = ccx - ge->last_cx;
        double sy = ccy - ge->last_cy;
        double step = hypot(sx, sy);
        if (step > step_limit) {
            double k = step_limit / step;
            ccx = ge->last_cx + sx * k;
            ccy = ge->last_cy + sy * k;
            jumped = true;
        }
    }

    if (ge->state == TRACKER_IDLE) {
        start_possible(ge, ccx, ccy, radius, tracked_count, now);
        return;
    }

    bool rebased = false;
    if (tracked_count != ge->last_count) {
        // Fingers landed/lifted while the gesture was still being recognised.
        // Shift the origin by the centroid jump so the translation stays
        // continuous instead of snapping by hundreds of pixels.
        rebase_origin(ge, ccx, ccy, radius, now);
        ge->count_change_us = now;
        ge->last_count = tracked_count;
        rebased = true;
    }

    double moved = ge->have_last_centroid
                   ? hypot(ccx - ge->last_cx, ccy - ge->last_cy) : 0.0;
    bool rested = now > ge->last_motion_us &&
                  (now - ge->last_motion_us) > REST_REBASE_US;

    if (!rebased && !jumped) {
        double dt = (double)(now - ge->last_time_us) / 1000000.0;
        if (dt > 0.0005) {
            double inst_vx = (ccx - ge->last_cx) / dt;
            double inst_vy = (ccy - ge->last_cy) / dt;
            double inst_vs = (ge->last_radius > 1.0)
                             ? (radius - ge->last_radius) / (ge->last_radius * dt)
                             : 0.0;

            // Clamp single-frame outliers (event coalescing, timestamp jitter)
            // before they poison the smoothed value.
            if (inst_vx > MAX_VELOCITY_PX_S) inst_vx = MAX_VELOCITY_PX_S;
            if (inst_vx < -MAX_VELOCITY_PX_S) inst_vx = -MAX_VELOCITY_PX_S;
            if (inst_vy > MAX_VELOCITY_PX_S) inst_vy = MAX_VELOCITY_PX_S;
            if (inst_vy < -MAX_VELOCITY_PX_S) inst_vy = -MAX_VELOCITY_PX_S;

            double alpha = 1.0 - exp(-dt / VELOCITY_TAU_SEC);
            ge->velocity_x += (inst_vx - ge->velocity_x) * alpha;
            ge->velocity_y += (inst_vy - ge->velocity_y) * alpha;
            ge->velocity_scale += (inst_vs - ge->velocity_scale) * alpha;
        }
        ge->last_cx = ccx;
        ge->last_cy = ccy;
        ge->last_radius = radius;
        ge->last_time_us = now;
    }

    if (moved > 0.35) ge->last_motion_us = now;

    // Fingers have been resting long enough that accumulated drift is noise:
    // re-anchor the origin so a hand resting on the pad cannot "swipe".
    if (ge->state == TRACKER_POSSIBLE && rested) {
        rebase_origin(ge, ccx, ccy, radius, now);
        ge->last_motion_us = now;
    }

    double delta_x = ccx - ge->start_cx;
    double delta_y = ccy - ge->start_cy;
    double scale = ge->start_radius > 1.0 ? radius / ge->start_radius : 1.0;
    if (scale < 0.2) scale = 0.2;
    if (scale > 5.0) scale = 5.0;

    ge->travel_x = delta_x;
    ge->travel_y = delta_y;

    // Gesture direction follows the physical finger movement. It must never be
    // affected by the user's two-finger "natural scrolling" preference.
    if (ge->state == TRACKER_POSSIBLE) {
        // Let a fourth finger land before locking the three-finger bindings.
        if (now - ge->count_change_us < COUNT_SETTLE_US) return;

        double deadzone = ge->server->config.deadzone_px;
        if (deadzone < DEADZONE_MIN_PX) deadzone = DEADZONE_MIN_PX;

        double adx = fabs(delta_x);
        double ady = fabs(delta_y);
        double translation = hypot(delta_x, delta_y);
        double radial = fabs(radius - ge->start_radius);
        bool pinch_ready = ge->start_radius > 0.0 &&
                           fabs(scale - 1.0) >= PINCH_SCALE_THRESHOLD &&
                           radial >= PINCH_MIN_RADIUS_DELTA_PX &&
                           radial >= translation * PINCH_DOMINANCE;

        int dir_x = 0, dir_y = 0, pinch = 0;
        bool swipe_ready = false;

        if (adx >= ady) {
            swipe_ready = adx > deadzone && adx > ady * AXIS_DOMINANCE;
            if (swipe_ready) dir_x = (delta_x > 0) ? 1 : -1;
        } else {
            swipe_ready = ady > deadzone && ady > adx * AXIS_DOMINANCE;
            if (swipe_ready) dir_y = (delta_y > 0) ? 1 : -1;
        }

        if (pinch_ready) {
            pinch = (scale < 1.0) ? -1 : 1;
        }

        // If neither a pinch nor a dominant axis is clear yet, keep
        // accumulating movement instead of guessing an action.
        if (pinch_ready || swipe_ready) {
            ge->current_action = resolve_action(ge->server, tracked_count, dir_x, dir_y, pinch);
            if (ge->current_action != ACTION_NONE) {
                ge->state = TRACKER_ACTIVE;
                ge->gesture_fingers = tracked_count;
                ge->gesture_slots = tracking;
                fprintf(stderr,
                        "[AuraDE] Gesture: %d fingers dx=%.0f dy=%.0f scale=%.2f -> %s\n",
                        tracked_count, delta_x, delta_y, scale,
                        action_name(ge->current_action));
                server_handle_gesture_begin(ge->server, ge->current_action);

                // Recognition slop is consumed here. Rebase the origin so the
                // first tracked frame starts at zero instead of leaping by the
                // 40 px (or pinch) threshold. This removes the visible jump
                // when a gesture activates, on both the workspace carousel and
                // the full-screen overlays.
                rebase_origin(ge, ccx, ccy, radius, now);
                delta_x = 0.0;
                delta_y = 0.0;
                scale = 1.0;
            }
        }
    }

    if (ge->state == TRACKER_ACTIVE) {
        server_handle_gesture_update(ge->server, delta_x, delta_y, scale);
        gesture_engine_broadcast_telemetry(ge, "TRACKING");
    }
}

static void handle_syn_dropped(gesture_engine_t *ge) {
    // The kernel buffer overflowed; slot contents are no longer trustworthy.
    for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
        ge->slots[i].active = false;
        ge->slots[i].pos_mask = 0;
        ge->slots[i].tracking_id = -1;
    }
    if (ge->state == TRACKER_ACTIVE) finish_gesture(ge, true);
    reset_tracking(ge);
    ge->wait_for_release = true;
    fprintf(stderr, "[AuraDE] Gesture Engine: SYN_DROPPED, state reset\n");
}

void gesture_engine_handle_event(gesture_engine_t *ge, const struct input_event *ev) {
    if (!ge || !ev) return;

    switch (ev->type) {
        case EV_ABS:
            switch (ev->code) {
                case ABS_MT_SLOT:
                    if (ev->value >= 0 && ev->value < MAX_TOUCH_SLOTS) {
                        ge->active_slot = ev->value;
                    }
                    break;

                case ABS_MT_TRACKING_ID:
                    if (ge->active_slot >= 0 && ge->active_slot < MAX_TOUCH_SLOTS) {
                        touch_slot_t *slot = &ge->slots[ge->active_slot];
                        if (ev->value >= 0) {
                            slot->active = true;
                            slot->tracking_id = ev->value;
                            // A reused slot may still hold the previous
                            // finger's coordinates; do not count it until this
                            // contact has reported both axes.
                            slot->pos_mask = 0;
                        } else {
                            slot->active = false;
                            slot->tracking_id = -1;
                            slot->pos_mask = 0;
                        }
                    }
                    break;

                case ABS_MT_POSITION_X:
                    if (ge->active_slot >= 0 && ge->active_slot < MAX_TOUCH_SLOTS) {
                        touch_slot_t *slot = &ge->slots[ge->active_slot];
                        slot->x = ev->value;
                        slot->pos_mask |= 0x1;
                    }
                    break;

                case ABS_MT_POSITION_Y:
                    if (ge->active_slot >= 0 && ge->active_slot < MAX_TOUCH_SLOTS) {
                        touch_slot_t *slot = &ge->slots[ge->active_slot];
                        slot->y = ev->value;
                        slot->pos_mask |= 0x2;
                    }
                    break;

                default:
                    break;
            }
            break;

        case EV_SYN:
            if (ev->code == SYN_REPORT) {
                process_multitouch_frame(ge);
            } else if (ev->code == SYN_DROPPED) {
                handle_syn_dropped(ge);
            }
            break;

        default:
            break;
    }
}

static void device_lost(gesture_engine_t *ge) {
    if (ge->state == TRACKER_ACTIVE) finish_gesture(ge, true);
    reset_tracking(ge);

    if (ge->event_source) {
        wl_event_source_remove(ge->event_source);
        ge->event_source = NULL;
    }
    if (ge->evdev_fd >= 0) {
        close(ge->evdev_fd);
        ge->evdev_fd = -1;
    }
    fprintf(stderr, "[AuraDE] Gesture Engine: touchpad disconnected, "
                    "gesture control disabled until restart\n");
}

static int evdev_event_callback(int fd, uint32_t mask, void *data) {
    (void)mask;
    gesture_engine_t *ge = (gesture_engine_t *)data;
    struct input_event evs[64];

    // Drain the device: a single 3-finger frame plus pointer traffic can easily
    // exceed 64 events, and leaving them queued adds a full poll cycle of
    // latency to every gesture.
    for (;;) {
        ssize_t n = read(fd, evs, sizeof(evs));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            device_lost(ge);
            break;
        }

        int count = (int)(n / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < count; i++) {
            gesture_engine_handle_event(ge, &evs[i]);
        }
        if ((size_t)n < sizeof(evs)) break;
    }
    return 1;
}

gesture_engine_t *gesture_engine_create(aurade_server_t *server, struct wl_event_loop *loop) {
    gesture_engine_t *ge = calloc(1, sizeof(*ge));
    if (!ge) return NULL;
    ge->server = server;
    ge->evdev_fd = -1;
    ge->ipc_fd = -1;
    ge->ipc_client_fd = -1;
    ge->active_slot = 0;

    setup_ipc_socket(ge);

    // A nested AuraDE (test_nested.sh, or running inside another desktop)
    // shares the physical touchpad with the host compositor. Reading it there
    // makes every swipe act twice, in two different shells. Leave gestures to
    // the host unless explicitly forced.
    const char *backends = getenv("WLR_BACKENDS");
    bool nested_session = backends && strstr(backends, "wayland") != NULL;
    bool forced = getenv("AURADE_ENABLE_GESTURES") != NULL;
    if ((nested_session && !forced) || getenv("AURADE_NO_GESTURES")) {
        printf("[AuraDE] Gesture Engine: %s; gestures handled by the host compositor\n",
               nested_session ? "nested session" : "disabled by AURADE_NO_GESTURES");
        fflush(stdout);
        return ge;
    }

    char *path = find_touchpad_device();
    if (path) {
        strncpy(ge->device_path, path, sizeof(ge->device_path) - 1);
        free(path);
        ge->evdev_fd = open(ge->device_path, O_RDONLY | O_NONBLOCK);
        if (ge->evdev_fd < 0) {
            fprintf(stderr,
                    "[AuraDE] Gesture Engine: cannot open %s (%s). Add this user to "
                    "the 'input' group to enable touchpad gestures.\n",
                    ge->device_path, strerror(errno));
        } else {
            struct input_absinfo xi = {0}, yi = {0};
            if (ioctl(ge->evdev_fd, EVIOCGABS(ABS_MT_POSITION_X), &xi) == 0 &&
                ioctl(ge->evdev_fd, EVIOCGABS(ABS_MT_POSITION_Y), &yi) == 0) {
                ge->abs_x_min = xi.minimum;
                ge->abs_x_max = xi.maximum;
                ge->abs_y_min = yi.minimum;
                ge->abs_y_max = yi.maximum;
            } else {
                ge->abs_x_min = ge->abs_y_min = 0;
                ge->abs_x_max = GE_REF_W;
                ge->abs_y_max = GE_REF_H;
            }

            char name[256] = {0};
            if (ioctl(ge->evdev_fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
                name[0] = '\0';
            }
            strncpy(ge->device_name, name, sizeof(ge->device_name) - 1);
            ge->device_name[sizeof(ge->device_name) - 1] = '\0';
            update_pixel_scale(ge);

            // Register on main Wayland event loop - strictly single-threaded & thread-safe!
            ge->event_source = wl_event_loop_add_fd(loop, ge->evdev_fd, WL_EVENT_READABLE,
                                                    evdev_event_callback, ge);
            printf("[AuraDE] Gesture Engine: Connected to \"%s\" (%s) abs %d-%d x %d-%d\n",
                   ge->device_name[0] ? ge->device_name : "touchpad",
                   ge->device_path, ge->abs_x_min, ge->abs_x_max,
                   ge->abs_y_min, ge->abs_y_max);
            fflush(stdout);
        }
    } else {
        fprintf(stderr, "[AuraDE] Gesture Engine: no multitouch touchpad found\n");
    }

    return ge;
}

void gesture_engine_destroy(gesture_engine_t *ge) {
    if (!ge) return;
    if (ge->event_source) {
        wl_event_source_remove(ge->event_source);
    }
    if (ge->evdev_fd >= 0) {
        close(ge->evdev_fd);
    }
    if (ge->ipc_client_fd >= 0) close(ge->ipc_client_fd);
    if (ge->ipc_fd >= 0) close(ge->ipc_fd);
    unlink("/tmp/aurade-gesture.sock");
    free(ge);
}
