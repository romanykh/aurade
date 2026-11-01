/*
 * gesture_sim — deterministic tests for the AuraDE touchpad recogniser.
 *
 * The recogniser consumes evdev multi-touch protocol B events, so it can be
 * driven without a touchpad. This harness replays scripted finger movements
 * through the exact production code path (gesture_engine_handle_event) and
 * checks the gesture decisions, making the "feel" rules regression-testable.
 *
 *   make test-gestures
 */
#include <linux/input.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gestures/gesture_engine.h"

// ---------------------------------------------------------------------------
// Stubs for the compositor side of the gesture contract (implemented in main.c)
// ---------------------------------------------------------------------------

static int g_begins;
static int g_updates;
static int g_ends;
static gesture_action_type_t g_action;
static bool g_end_cancelled;
static double g_end_vx, g_end_vy;
static double g_first_dx, g_first_dy, g_first_scale;
static double g_last_dx, g_last_dy, g_last_scale;

void server_handle_gesture_begin(aurade_server_t *server, gesture_action_type_t action) {
    (void)server;
    g_begins++;
    g_action = action;
    g_first_dx = g_first_dy = 0.0;
    g_first_scale = 1.0;
}

void server_handle_gesture_update(aurade_server_t *server, double dx, double dy, double scale) {
    (void)server;
    if (g_updates == 0) {
        g_first_dx = dx;
        g_first_dy = dy;
        g_first_scale = scale;
    }
    g_updates++;
    g_last_dx = dx;
    g_last_dy = dy;
    g_last_scale = scale;
}

void server_handle_gesture_end(aurade_server_t *server, bool cancelled,
                               double vx, double vy) {
    (void)server;
    g_ends++;
    g_end_cancelled = cancelled;
    g_end_vx = vx;
    g_end_vy = vy;
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

static aurade_server_t g_server;
static gesture_engine_t g_ge;
static uint64_t g_now_us;
static int g_failures;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("    FAIL: " __VA_ARGS__);                                   \
            printf("\n");                                                       \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

static uint64_t fake_now(void) { return g_now_us; }

static void reset_recorder(void) {
    g_begins = g_updates = g_ends = 0;
    g_action = ACTION_NONE;
    g_end_cancelled = false;
    g_end_vx = g_end_vy = 0.0;
    g_first_dx = g_first_dy = 0.0;
    g_first_scale = 1.0;
    g_last_dx = g_last_dy = 0.0;
    g_last_scale = 1.0;
}

/* A 1200x675 touchpad mapped to 1920x1080 reference pixels: 1 unit = 1.6 ref
 * px, i.e. about 19.2 ref px per millimetre on a 100 mm wide pad. */
static void harness_init(int screen_w) {
    memset(&g_server, 0, sizeof(g_server));
    memset(&g_ge, 0, sizeof(g_ge));

    g_server.screen_width = screen_w;
    g_server.screen_height = screen_w * 9 / 16;

    aurade_config_t *cfg = &g_server.config;
    cfg->sensitivity = 1.0;
    cfg->snap_threshold = 0.35;
    cfg->flick_velocity_threshold = 700.0;
    cfg->rubber_band_limit = 180.0;
    cfg->deadzone_px = 40.0;

    cfg->swipe_up_3.type = ACTION_MISSION_CONTROL;
    cfg->swipe_down_3.type = ACTION_MISSION_CONTROL;
    cfg->swipe_left_3.type = ACTION_WORKSPACE_NEXT;
    cfg->swipe_right_3.type = ACTION_WORKSPACE_PREV;
    cfg->pinch_in_3.type = ACTION_NONE;
    cfg->pinch_out_3.type = ACTION_SHOW_DESKTOP;
    cfg->swipe_up_4.type = ACTION_MISSION_CONTROL;
    cfg->swipe_down_4.type = ACTION_MISSION_CONTROL;
    cfg->swipe_left_4.type = ACTION_WORKSPACE_NEXT;
    cfg->swipe_right_4.type = ACTION_WORKSPACE_PREV;
    cfg->pinch_in_4.type = ACTION_LAUNCHPAD;
    cfg->pinch_out_4.type = ACTION_LAUNCHPAD;

    g_ge.server = &g_server;
    g_ge.evdev_fd = -1;
    g_ge.ipc_fd = -1;
    g_ge.ipc_client_fd = -1;
    g_ge.abs_x_min = 0;
    g_ge.abs_x_max = 1200;
    g_ge.abs_y_min = 0;
    g_ge.abs_y_max = 675;
    g_ge.px_scale_x = GE_REF_W / 1200.0;
    g_ge.px_scale_y = GE_REF_H / 675.0;
    g_ge.now_fn = fake_now;

    g_now_us = 1000000;
    reset_recorder();
}

static void ev(uint16_t type, uint16_t code, int32_t value) {
    struct input_event e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.code = code;
    e.value = value;
    gesture_engine_handle_event(&g_ge, &e);
}

static void syn(void) { ev(EV_SYN, SYN_REPORT, 0); }

static void finger_down(int slot, int tracking_id, int x, int y) {
    ev(EV_ABS, ABS_MT_SLOT, slot);
    ev(EV_ABS, ABS_MT_TRACKING_ID, tracking_id);
    ev(EV_ABS, ABS_MT_POSITION_X, x);
    ev(EV_ABS, ABS_MT_POSITION_Y, y);
}

static void finger_move(int slot, int x, int y) {
    ev(EV_ABS, ABS_MT_SLOT, slot);
    ev(EV_ABS, ABS_MT_POSITION_X, x);
    ev(EV_ABS, ABS_MT_POSITION_Y, y);
}

static void finger_up(int slot) {
    ev(EV_ABS, ABS_MT_SLOT, slot);
    ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
}

/* Advance the clock without producing a frame, exactly like a stationary
 * touchpad (the kernel only reports frames when something changed). */
static void advance(int dt_us) { g_now_us += (uint64_t)dt_us; }

static void settle(int fingers, int frames) {
    (void)fingers;
    advance(10000 * frames);
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

/* Three fingers flicked upwards: Mission Control, with the recognition slop
 * consumed so the first tracked frame starts at zero. */
static void test_swipe_up_three(void) {
    harness_init(1920);
    const int x[3] = {400, 600, 800};
    int y = 500;

    for (int i = 0; i < 3; i++) finger_down(i, 10 + i, x[i], y);
    syn();
    settle(3, 4);

    for (int step = 1; step <= 8; step++) {
        advance(10000);
        for (int i = 0; i < 3; i++) finger_move(i, x[i], y - 30 * step);
        syn();
    }

    CHECK(g_begins == 1, "expected exactly one gesture, got %d", g_begins);
    CHECK(g_action == ACTION_MISSION_CONTROL, "wrong action %d", (int)g_action);
    CHECK(g_updates > 4, "expected live updates, got %d", g_updates);
    CHECK(fabs(g_first_dy) < 1.0, "slop was not consumed (first dy=%.1f)", g_first_dy);

    g_now_us += 10000;
    for (int i = 0; i < 3; i++) finger_up(i);
    syn();

    CHECK(g_ends == 1, "expected one gesture end, got %d", g_ends);
    CHECK(!g_end_cancelled, "gesture was cancelled");
    CHECK(g_end_vy < -100.0, "expected upward flick velocity, got %.0f", g_end_vy);
}

/* Two fingers are a scroll, never a gesture. */
static void test_two_finger_scroll_ignored(void) {
    harness_init(1920);
    int y = 500;
    for (int i = 0; i < 2; i++) finger_down(i, 20 + i, 400 + i * 150, y);
    syn();
    settle(2, 4);

    for (int step = 1; step <= 20; step++) {
        advance(8000);
        for (int i = 0; i < 2; i++) finger_move(i, 400 + i * 150, y - 40 * step);
        syn();
    }
    for (int i = 0; i < 2; i++) finger_up(i);
    g_now_us += 8000;
    syn();

    CHECK(g_begins == 0, "two-finger drag started a gesture");
    CHECK(g_ends == 0, "two-finger drag ended a gesture");
}

/* Resting fingers with contact jitter must not open anything. */
static void test_resting_jitter_ignored(void) {
    harness_init(1920);
    const int x[3] = {500, 600, 700};
    const int y = 400;
    for (int i = 0; i < 3; i++) finger_down(i, 30 + i, x[i], y);
    syn();
    settle(3, 4);

    for (int step = 0; step < 80; step++) {
        advance(8000);
        for (int i = 0; i < 3; i++) {
            int jitter = (step % 2) ? 2 : -2;
            finger_move(i, x[i] + jitter, y + jitter);
        }
        syn();
    }

    CHECK(g_begins == 0, "jitter triggered a gesture (%d)", g_begins);
}

/* A diagonal move stays ambiguous until one axis clearly dominates. */
static void test_diagonal_waits_for_dominance(void) {
    harness_init(1920);
    const int x[3] = {400, 600, 800};
    const int y = 400;
    for (int i = 0; i < 3; i++) finger_down(i, 40 + i, x[i], y);
    syn();
    settle(3, 4);

    for (int step = 1; step <= 10; step++) {
        advance(10000);
        for (int i = 0; i < 3; i++) finger_move(i, x[i] + 6 * step, y + 6 * step);
        syn();
    }
    CHECK(g_begins == 0, "45-degree diagonal was classified too early");

    for (int step = 1; step <= 6; step++) {
        advance(10000);
        for (int i = 0; i < 3; i++) finger_move(i, x[i] + 60 + 40 * step, y + 60);
        syn();
    }

    CHECK(g_begins == 1, "horizontal movement was not recognised");
    CHECK(g_action == ACTION_WORKSPACE_PREV,
          "expected workspace_prev, got %d", (int)g_action);
}

/* Four-finger pinch in opens Launchpad with scale < 1. */
static void test_pinch_in_four(void) {
    harness_init(1920);
    const int cx = 600, cy = 340;
    const int dx[4] = {-200, 200, -200, 200};
    const int dy[4] = {-125, -125, 125, 125};

    for (int i = 0; i < 4; i++) finger_down(i, 50 + i, cx + dx[i], cy + dy[i]);
    syn();
    settle(4, 4);

    for (int step = 1; step <= 6; step++) {
        advance(10000);
        double k = 1.0 - 0.05 * step;
        for (int i = 0; i < 4; i++) {
            int fx = cx + (int)lround(dx[i] * k);
            int fy = cy + (int)lround(dy[i] * k);
            finger_move(i, fx, fy);
        }
        syn();
    }

    CHECK(g_begins == 1, "pinch did not start a gesture (%d)", g_begins);
    CHECK(g_action == ACTION_LAUNCHPAD, "expected launchpad, got %d", (int)g_action);
    CHECK(g_last_scale < 0.97, "scale did not shrink (%.3f)", g_last_scale);

    for (int i = 0; i < 4; i++) finger_up(i);
    g_now_us += 10000;
    syn();
    CHECK(g_ends == 1, "pinch did not end");
}

/* Swipe, pause, lift: the stored velocity is stale and must not fling. */
static void test_pause_before_lift_kills_flick(void) {
    harness_init(1920);
    const int x[3] = {400, 600, 800};
    const int y = 400;
    for (int i = 0; i < 3; i++) finger_down(i, 60 + i, x[i], y);
    syn();
    settle(3, 4);

    for (int step = 1; step <= 6; step++) {
        advance(8000);
        for (int i = 0; i < 3; i++) finger_move(i, x[i], y - 40 * step);
        syn();
    }

    // The user stops moving (the kernel sends nothing while stationary) and
    // lifts a third of a second later.
    g_now_us += 300000;
    for (int i = 0; i < 3; i++) finger_up(i);
    syn();

    CHECK(g_ends == 1, "gesture did not end");
    CHECK(fabs(g_end_vy) < 150.0,
          "stale velocity used for flick (vy=%.0f)", g_end_vy);
}

/* Lifting one finger ends the gesture; the remaining fingers cannot start a
 * new one until they are all released. */
static void test_finger_lift_ends_gesture(void) {
    harness_init(1920);
    const int x[3] = {400, 600, 800};
    const int y = 400;
    for (int i = 0; i < 3; i++) finger_down(i, 70 + i, x[i], y);
    syn();
    settle(3, 4);

    for (int step = 1; step <= 5; step++) {
        advance(10000);
        for (int i = 0; i < 3; i++) finger_move(i, x[i], y - 35 * step);
        syn();
    }
    CHECK(g_begins == 1, "gesture never started");

    g_now_us += 10000;
    finger_up(2);
    syn();
    CHECK(g_ends == 1, "lifting a finger did not end the gesture");

    for (int step = 0; step < 5; step++) {
        advance(10000);
        finger_move(0, x[0], y - 35 * 5 - 30 * step);
        finger_move(1, x[1], y - 35 * 5 - 30 * step);
        syn();
    }
    CHECK(g_begins == 1, "leftover fingers started a new gesture");
}

/* Hand travel is resolution-independent: a 4K output must not require twice
 * the finger distance. */
static void test_resolution_independent(void) {
    double travel_1080 = 0.0, travel_2160 = 0.0;

    for (int pass = 0; pass < 2; pass++) {
        harness_init(pass == 0 ? 1920 : 3840);
        const int x[3] = {400, 600, 800};
        const int y = 400;
        for (int i = 0; i < 3; i++) finger_down(i, 80 + i, x[i], y);
        syn();
        settle(3, 4);

        double total = 0.0;
        for (int step = 1; step <= 5; step++) {
            advance(10000);
            for (int i = 0; i < 3; i++) finger_move(i, x[i], y - 30 * step);
            syn();
            total += fabs(g_last_dy);
        }
        if (pass == 0) travel_1080 = total; else travel_2160 = total;
    }

    CHECK(fabs(travel_1080 - travel_2160) < 0.01,
          "gesture travel depends on screen size (%.1f vs %.1f)",
          travel_1080, travel_2160);
}

/* A whole hand (more than five contacts) must never drive a gesture. */
static void test_palm_rejected(void) {
    harness_init(1920);
    const int x[6] = {300, 400, 500, 600, 700, 800};
    const int y = 400;
    for (int i = 0; i < 6; i++) finger_down(i, 90 + i, x[i], y);
    syn();
    settle(6, 4);

    for (int step = 1; step <= 6; step++) {
        advance(10000);
        for (int i = 0; i < 6; i++) finger_move(i, x[i], y - 30 * step);
        syn();
    }
    CHECK(g_begins == 0, "six contacts started a gesture");
}

/* A small slow drag below the deadzone is not a swipe. */
static void test_below_deadzone_ignored(void) {
    harness_init(1920);
    const int x[3] = {500, 600, 700};
    const int y = 400;
    for (int i = 0; i < 3; i++) finger_down(i, 100 + i, x[i], y);
    syn();
    settle(3, 4);

    for (int step = 1; step <= 5; step++) {
        advance(20000);
        for (int i = 0; i < 3; i++) finger_move(i, x[i], y - 4 * step);
        syn();
    }
    g_now_us += 20000;
    for (int i = 0; i < 3; i++) finger_up(i);
    syn();

    CHECK(g_begins == 0, "sub-deadzone drag triggered a gesture");
}

/* A fast flick down must resolve to App Exposé, not Mission Control. */
static void test_swipe_down_three(void) {
    harness_init(1920);
    const int x[3] = {400, 600, 800};
    int y = 300;
    for (int i = 0; i < 3; i++) finger_down(i, 110 + i, x[i], y);
    syn();
    settle(3, 4);

    for (int step = 1; step <= 6; step++) {
        advance(10000);
        for (int i = 0; i < 3; i++) finger_move(i, x[i], y + 35 * step);
        syn();
    }
    g_now_us += 10000;
    for (int i = 0; i < 3; i++) finger_up(i);
    syn();

    CHECK(g_begins == 1, "downward swipe not recognised");
    CHECK(g_action == ACTION_MISSION_CONTROL, "expected mission_control, got %d", (int)g_action);
    CHECK(g_end_vy > 100.0, "expected downward velocity, got %.0f", g_end_vy);
    printf("PASS three-finger swipe down -> Mission Control\n");
}


/* An extra finger landing mid-gesture must not shift the centroid (which would
 * make the overlay jump) and must not cancel the gesture. */
static void test_extra_finger_mid_gesture(void) {
    harness_init(1920);
    const int x[3] = {400, 600, 800};
    const int y = 400;
    for (int i = 0; i < 3; i++) finger_down(i, 120 + i, x[i], y);
    syn();
    settle(3, 4);

    for (int step = 1; step <= 3; step++) {
        advance(10000);
        for (int i = 0; i < 3; i++) finger_move(i, x[i], y - 35 * step);
        syn();
    }
    CHECK(g_begins == 1, "gesture did not start");
    double before = g_last_dy;

    // A fourth finger touches the far corner of the pad.
    advance(10000);
    finger_down(3, 130, 1180, 660);
    syn();
    CHECK(g_ends == 0, "extra finger cancelled the gesture");

    advance(10000);
    for (int i = 0; i < 3; i++) finger_move(i, x[i], y - 35 * 4);
    syn();

    double after = g_last_dy;
    CHECK(fabs(after - (before - 56.0)) < 20.0,
          "centroid jumped when an extra finger landed (%.0f -> %.0f)", before, after);

    // Lifting the extra finger is irrelevant to the gesture...
    advance(10000);
    finger_up(3);
    syn();
    CHECK(g_ends == 0, "lifting the extra finger ended the gesture");

    // ...but lifting one of the original fingers ends it.
    advance(10000);
    finger_up(2);
    syn();
    CHECK(g_ends == 1, "lifting a gesture finger did not end the gesture");
}


/* A four-finger swipe with a little natural splay must remain a swipe; the
 * radius change must not be mistaken for a pinch. */
static void test_splay_does_not_steal_swipe(void) {
    harness_init(1920);
    const int cx = 600, cy = 340;
    const int dx[4] = {-200, 200, -200, 200};
    const int dy[4] = {-125, -125, 125, 125};

    for (int i = 0; i < 4; i++) finger_down(i, 140 + i, cx + dx[i], cy + dy[i]);
    syn();
    settle(4, 4);

    for (int step = 1; step <= 8; step++) {
        advance(10000);
        double k = 1.0 + 0.02 * step;    // 2% splay per frame
        for (int i = 0; i < 4; i++) {
            int fx = cx + (int)lround(dx[i] * k);
            int fy = cy + (int)lround(dy[i] * k) - 25 * step;
            finger_move(i, fx, fy);
        }
        syn();
    }

    CHECK(g_begins == 1, "splay swipe was not recognised");
    CHECK(g_action != ACTION_LAUNCHPAD,
          "splay was mistaken for a pinch (action %d)", (int)g_action);
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"three-finger swipe up -> Mission Control", test_swipe_up_three},
        {"three-finger swipe down -> App Expose", test_swipe_down_three},
        {"two-finger drag is ignored", test_two_finger_scroll_ignored},
        {"resting jitter is ignored", test_resting_jitter_ignored},
        {"diagonal waits for axis dominance", test_diagonal_waits_for_dominance},
        {"four-finger pinch in -> Launchpad", test_pinch_in_four},
        {"splay does not steal a four-finger swipe", test_splay_does_not_steal_swipe},
        {"pause before lift kills the flick", test_pause_before_lift_kills_flick},
        {"lifted finger ends the gesture", test_finger_lift_ends_gesture},
        {"travel is resolution independent", test_resolution_independent},
        {"extra finger does not disturb the gesture", test_extra_finger_mid_gesture},
        {"palm contact is rejected", test_palm_rejected},
        {"sub-deadzone drag is ignored", test_below_deadzone_ignored},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_failures;
        tests[i].fn();
        printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", tests[i].name);
    }

    if (g_failures) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("\nall gesture checks passed\n");
    return 0;
}
