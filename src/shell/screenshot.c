#include "screenshot.h"
#include "topbar.h"
#include "dock.h"
#include "../server.h"
#include "../render/cairo_buffer.h"
#include "../util/apps.h"

#include <cairo.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#include <xkbcommon/xkbcommon.h>

#define SHOT_FONT "Inter"

typedef enum {
    HANDLE_NONE = 0,
    HANDLE_NEW,
    HANDLE_MOVE,
    HANDLE_NW,
    HANDLE_NE,
    HANDLE_SW,
    HANDLE_SE,
    HANDLE_N,
    HANDLE_S,
    HANDLE_W,
    HANDLE_E,
} drag_handle_t;

struct aurade_screenshot {
    aurade_server_t *server;
    cairo_buffer_t *buffer;
    struct wlr_scene_buffer *scene_buffer;

    int width, height;
    bool active;

    // Modes & Targets
    shot_mode_t mode;           // SHOT_MODE_STILL or SHOT_MODE_VIDEO
    shot_target_t target;       // SHOT_TARGET_SELECTION, FULLSCREEN
    bool record_audio;          // Include microphone/audio in video recording
    bool direct_crop;           // Direct region capture (Shift+Print): snap on release

    // Selection box geometry (screen coordinates)
    double sel_x, sel_y, sel_w, sel_h;
    bool has_selection;
    bool dragging;
    drag_handle_t active_handle;
    double drag_start_mouse_x, drag_start_mouse_y;
    double drag_start_box_x, drag_start_box_y, drag_start_box_w, drag_start_box_h;
    double cur_x, cur_y;

    // Floating Toolbar geometry & hover states
    double bar_x, bar_y, bar_w, bar_h;
    int hover_button; // 0 = none, 1 = still, 2 = video, 3 = full, 4 = sel, 5 = audio, 6 = action, 7 = close

    // Live Recording State
    bool is_recording;
    pid_t recorder_pid;
    uint64_t recording_start_ms;
    char recording_output_file[1024];
    struct wl_event_source *recording_timer;

    // Asynchronous Reaping of Finalized Recording
    pid_t reaping_pid;
    int reap_ticks;
    char reap_output_file[1024];
    struct wl_event_source *reap_timer;
};

static int clamp_int(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static double clamp_double(double value, double min, double max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_cursor(aurade_screenshot_t *shot, const char *name) {
    if (!shot || !shot->server || !shot->server->cursor) return;
    wlr_cursor_set_xcursor(shot->server->cursor, shot->server->cursor_mgr, name ? name : "default");
}

static void reset_cursor(aurade_screenshot_t *shot) {
    set_cursor(shot, "default");
}

// ---------------------------------------------------------------------------
// VECTOR ICON DRAWING HELPERS
// ---------------------------------------------------------------------------

static void draw_camera_icon(cairo_t *cr, double cx, double cy, double sz,
                             double r, double g, double b, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, 1.5);

    // Body
    draw_rounded_rect(cr, cx - sz * 0.48, cy - sz * 0.28, sz * 0.96, sz * 0.68, 3.0);
    cairo_stroke(cr);

    // Prism top bump
    draw_rounded_rect(cr, cx - sz * 0.20, cy - sz * 0.46, sz * 0.40, sz * 0.20, 1.5);
    cairo_fill(cr);

    // Lens
    cairo_arc(cr, cx, cy + sz * 0.05, sz * 0.20, 0, 2 * M_PI);
    cairo_stroke(cr);

    // Reflection dot
    cairo_arc(cr, cx + sz * 0.07, cy - sz * 0.02, sz * 0.04, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_restore(cr);
}

static void draw_video_camera_icon(cairo_t *cr, double cx, double cy, double sz,
                                   double r, double g, double b, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);

    // Camera body
    draw_rounded_rect(cr, cx - sz * 0.50, cy - sz * 0.32, sz * 0.62, sz * 0.64, 3.0);
    cairo_fill(cr);

    // Projector lens cone
    cairo_move_to(cr, cx + sz * 0.16, cy - sz * 0.12);
    cairo_line_to(cr, cx + sz * 0.48, cy - sz * 0.32);
    cairo_line_to(cr, cx + sz * 0.48, cy + sz * 0.32);
    cairo_line_to(cr, cx + sz * 0.16, cy + sz * 0.12);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_restore(cr);
}

static void draw_monitor_icon(cairo_t *cr, double cx, double cy, double sz,
                              double r, double g, double b, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, 1.4);

    // Screen
    draw_rounded_rect(cr, cx - sz * 0.48, cy - sz * 0.44, sz * 0.96, sz * 0.64, 2.5);
    cairo_stroke(cr);

    // Neck stand
    cairo_rectangle(cr, cx - 1.2, cy + sz * 0.20, 2.4, sz * 0.18);
    cairo_fill(cr);

    // Base
    draw_rounded_rect(cr, cx - sz * 0.28, cy + sz * 0.38, sz * 0.56, 1.8, 0.9);
    cairo_fill(cr);

    cairo_restore(cr);
}

static void draw_crop_icon(cairo_t *cr, double cx, double cy, double sz,
                           double r, double g, double b, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    double off = sz * 0.42;
    double arm = sz * 0.24;

    // Top-left bracket
    cairo_move_to(cr, cx - off, cy - off + arm);
    cairo_line_to(cr, cx - off, cy - off);
    cairo_line_to(cr, cx - off + arm, cy - off);
    cairo_stroke(cr);

    // Top-right bracket
    cairo_move_to(cr, cx + off - arm, cy - off);
    cairo_line_to(cr, cx + off, cy - off);
    cairo_line_to(cr, cx + off, cy - off + arm);
    cairo_stroke(cr);

    // Bottom-left bracket
    cairo_move_to(cr, cx - off, cy + off - arm);
    cairo_line_to(cr, cx - off, cy + off);
    cairo_line_to(cr, cx - off + arm, cy + off);
    cairo_stroke(cr);

    // Bottom-right bracket
    cairo_move_to(cr, cx + off - arm, cy + off);
    cairo_line_to(cr, cx + off, cy + off);
    cairo_line_to(cr, cx + off, cy + off - arm);
    cairo_stroke(cr);

    cairo_restore(cr);
}

static void draw_mic_icon(cairo_t *cr, double cx, double cy, double sz, bool enabled) {
    cairo_save(cr);
    if (enabled) {
        cairo_set_source_rgba(cr, 0.25, 0.95, 0.45, 1.0);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.45);
    }
    cairo_set_line_width(cr, 1.4);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    // Capsule head
    draw_rounded_rect(cr, cx - sz * 0.16, cy - sz * 0.44, sz * 0.32, sz * 0.52, sz * 0.16);
    cairo_fill(cr);

    // Cradle arc
    cairo_arc(cr, cx, cy - sz * 0.12, sz * 0.28, 0, M_PI);
    cairo_stroke(cr);

    // Stem & base
    cairo_move_to(cr, cx, cy + sz * 0.16);
    cairo_line_to(cr, cx, cy + sz * 0.36);
    cairo_stroke(cr);

    cairo_move_to(cr, cx - sz * 0.22, cy + sz * 0.36);
    cairo_line_to(cr, cx + sz * 0.22, cy + sz * 0.36);
    cairo_stroke(cr);

    // Diagonal mute slash if disabled
    if (!enabled) {
        cairo_set_source_rgba(cr, 0.95, 0.25, 0.25, 0.90);
        cairo_set_line_width(cr, 1.6);
        cairo_move_to(cr, cx - sz * 0.38, cy - sz * 0.38);
        cairo_line_to(cr, cx + sz * 0.38, cy + sz * 0.38);
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

static void draw_close_cross(cairo_t *cr, double cx, double cy, double sz, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, a);
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    double off = sz * 0.30;
    cairo_move_to(cr, cx - off, cy - off);
    cairo_line_to(cr, cx + off, cy + off);
    cairo_stroke(cr);

    cairo_move_to(cr, cx + off, cy - off);
    cairo_line_to(cr, cx - off, cy + off);
    cairo_stroke(cr);

    cairo_restore(cr);
}

// ---------------------------------------------------------------------------
// HANDLE & GEOMETRY HELPERS
// ---------------------------------------------------------------------------

static drag_handle_t get_handle_at(aurade_screenshot_t *shot, double x, double y) {
    if (!shot->has_selection || shot->target != SHOT_TARGET_SELECTION ||
        shot->sel_w < 10.0 || shot->sel_h < 10.0) {
        return HANDLE_NONE;
    }

    double sx = shot->sel_x;
    double sy = shot->sel_y;
    double sw = shot->sel_w;
    double sh = shot->sel_h;

    double rad = 12.0;

    // Corner handles
    if (hypot(x - sx, y - sy) <= rad) return HANDLE_NW;
    if (hypot(x - (sx + sw), y - sy) <= rad) return HANDLE_NE;
    if (hypot(x - sx, y - (sy + sh)) <= rad) return HANDLE_SW;
    if (hypot(x - (sx + sw), y - (sy + sh)) <= rad) return HANDLE_SE;

    // Edge handles
    if (fabs(y - sy) <= 8.0 && x >= sx + rad && x <= sx + sw - rad) return HANDLE_N;
    if (fabs(y - (sy + sh)) <= 8.0 && x >= sx + rad && x <= sx + sw - rad) return HANDLE_S;
    if (fabs(x - sx) <= 8.0 && y >= sy + rad && y <= sy + sh - rad) return HANDLE_W;
    if (fabs(x - (sx + sw)) <= 8.0 && y >= sy + rad && y <= sy + sh - rad) return HANDLE_E;

    // Inside box
    if (x >= sx && x <= sx + sw && y >= sy && y <= sy + sh) {
        return HANDLE_MOVE;
    }

    return HANDLE_NONE;
}

static void normalize_selection(aurade_screenshot_t *shot) {
    if (shot->sel_w < 0.0) {
        shot->sel_x += shot->sel_w;
        shot->sel_w = fabs(shot->sel_w);
    }
    if (shot->sel_h < 0.0) {
        shot->sel_y += shot->sel_h;
        shot->sel_h = fabs(shot->sel_h);
    }
    shot->sel_x = clamp_double(shot->sel_x, 0.0, shot->width);
    shot->sel_y = clamp_double(shot->sel_y, 0.0, shot->height);
    if (shot->sel_x + shot->sel_w > shot->width) shot->sel_w = shot->width - shot->sel_x;
    if (shot->sel_y + shot->sel_h > shot->height) shot->sel_h = shot->height - shot->sel_y;
}

// ---------------------------------------------------------------------------
// RENDERING OVERLAY & FLOATING TOOLBAR
// ---------------------------------------------------------------------------

static void render_overlay(aurade_screenshot_t *shot) {
    if (!shot || !shot->buffer) return;
    cairo_t *cr = shot->buffer->cr;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // 1. Dark overlay mask with cutout
    if (shot->target == SHOT_TARGET_SELECTION && shot->has_selection &&
        shot->sel_w >= 2.0 && shot->sel_h >= 2.0) {
        // Dim surrounding screen
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.40);
        cairo_paint(cr);

        // Punch out clear rectangular window
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_rectangle(cr, shot->sel_x, shot->sel_y, shot->sel_w, shot->sel_h);
        cairo_fill(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

        // Outer contrast line
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.50);
        cairo_set_line_width(cr, 2.5);
        cairo_rectangle(cr, shot->sel_x, shot->sel_y, shot->sel_w, shot->sel_h);
        cairo_stroke(cr);

        // Inner crisp white border
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, shot->sel_x, shot->sel_y, shot->sel_w, shot->sel_h);
        cairo_stroke(cr);

        // Resize Handles (Mac style white squares with dark border)
        double h_sz = 8.0;
        double half_h = h_sz / 2.0;
        double corners[4][2] = {
            { shot->sel_x, shot->sel_y },
            { shot->sel_x + shot->sel_w, shot->sel_y },
            { shot->sel_x, shot->sel_y + shot->sel_h },
            { shot->sel_x + shot->sel_w, shot->sel_y + shot->sel_h },
        };
        for (int i = 0; i < 4; i++) {
            draw_rounded_rect(cr, corners[i][0] - half_h, corners[i][1] - half_h, h_sz, h_sz, 2.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.70);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        }

        // Edge midpoint handles
        double edges[4][2] = {
            { shot->sel_x + shot->sel_w / 2.0, shot->sel_y },
            { shot->sel_x + shot->sel_w / 2.0, shot->sel_y + shot->sel_h },
            { shot->sel_x, shot->sel_y + shot->sel_h / 2.0 },
            { shot->sel_x + shot->sel_w, shot->sel_y + shot->sel_h / 2.0 },
        };
        for (int i = 0; i < 4; i++) {
            double ew = (i < 2) ? 14.0 : 6.0;
            double eh = (i < 2) ? 6.0 : 14.0;
            draw_rounded_rect(cr, edges[i][0] - ew / 2.0, edges[i][1] - eh / 2.0, ew, eh, 2.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.70);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        }

        // Dimension Pill Badge (e.g. 1280 × 720)
        char dim_str[64];
        snprintf(dim_str, sizeof(dim_str), "%d × %d", (int)lround(shot->sel_w), (int)lround(shot->sel_h));
        double tw = text_width(cr, dim_str, SHOT_FONT, 11.5, false);
        double tag_w = tw + 18.0;
        double tag_h = 22.0;
        double tag_x = shot->sel_x + 4.0;
        double tag_y = shot->sel_y - tag_h - 6.0;
        if (tag_y < 10.0) tag_y = shot->sel_y + 8.0;
        if (tag_x + tag_w > shot->width - 10.0) tag_x = shot->width - tag_w - 10.0;

        draw_rounded_rect(cr, tag_x, tag_y, tag_w, tag_h, 6.0);
        cairo_set_source_rgba(cr, 0.08, 0.09, 0.12, 0.88);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);
        draw_text(cr, dim_str, SHOT_FONT, 11.5, false, 1.0, 1.0, 1.0, 0.95,
                  tag_x + 9.0, tag_y + tag_h / 2.0, false, true);

    } else if (shot->target == SHOT_TARGET_FULLSCREEN) {
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.20);
        cairo_paint(cr);

        // Radiant display perimeter border
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90);
        cairo_set_line_width(cr, 2.0);
        cairo_rectangle(cr, 1.0, 1.0, shot->width - 2.0, shot->height - 2.0);
        cairo_stroke(cr);
    } else {
        // Drag prompt dim
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
        cairo_paint(cr);

        const char *hint = (shot->mode == SHOT_MODE_VIDEO) ?
            "Drag to select a recording area  •  Esc to cancel" :
            "Drag to select an area  •  Esc to cancel";
        double tw = text_width(cr, hint, SHOT_FONT, 14.5, true);
        double hbx = (shot->width - tw - 36.0) / 2.0;
        double hby = 46.0;
        draw_rounded_rect(cr, hbx, hby, tw + 36.0, 34.0, 10.0);
        cairo_set_source_rgba(cr, 0.07, 0.08, 0.11, 0.85);
        cairo_fill(cr);
        draw_text(cr, hint, SHOT_FONT, 14.5, true, 1.0, 1.0, 1.0, 0.95,
                  shot->width / 2.0, hby + 17.0, true, true);
    }

    // 2. Modern macOS Floating Control Bar (omitted in direct region crop mode)
    if (shot->direct_crop) {
        shot->bar_x = 0;
        shot->bar_y = 0;
        shot->bar_w = 0;
        shot->bar_h = 0;
        cairo_restore(cr);
        cairo_buffer_apply_to_scene(shot->buffer, shot->scene_buffer);
        return;
    }

    double bw = 416.0;
    double bh = 48.0;
    double bx = (shot->width - bw) / 2.0;

    double bottom_margin = 104.0;
    if (shot->server && shot->server->dock && shot->server->dock->enabled && shot->server->dock->pill_height > 0) {
        bottom_margin = shot->server->dock->pill_height + 34.0;
    }
    if (bottom_margin < 100.0) bottom_margin = 100.0;
    double by = shot->height - bh - bottom_margin;

    shot->bar_x = bx;
    shot->bar_y = by;
    shot->bar_w = bw;
    shot->bar_h = bh;

    // Drop shadow
    draw_drop_shadow(cr, bx, by, bw, bh, 24.0, 16.0, 0.50);

    // Glass pill background
    draw_rounded_rect(cr, bx, by, bw, bh, 24.0);
    cairo_set_source_rgba(cr, 0.11, 0.12, 0.16, 0.94);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    double cur_x = bx + 10.0;

    // Segment 1: Mode Switch (Still / Video)
    double mode_seg_w = 84.0;
    double seg_h = 32.0;
    double seg_y = by + (bh - seg_h) / 2.0;

    draw_rounded_rect(cr, cur_x, seg_y, mode_seg_w, seg_h, 16.0);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
    cairo_fill(cr);

    // Active mode pill capsule
    double btn1_x = cur_x + 2.0;
    double btn2_x = cur_x + 42.0;
    double btn_w = 40.0;
    double btn_h = 28.0;
    double btn_y = seg_y + 2.0;

    if (shot->mode == SHOT_MODE_STILL) {
        draw_rounded_rect(cr, btn1_x, btn_y, btn_w, btn_h, 14.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22);
        cairo_fill(cr);
    } else {
        draw_rounded_rect(cr, btn2_x, btn_y, btn_w, btn_h, 14.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22);
        cairo_fill(cr);
    }

    if (shot->hover_button == 1 && shot->mode != SHOT_MODE_STILL) {
        draw_rounded_rect(cr, btn1_x, btn_y, btn_w, btn_h, 14.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        cairo_fill(cr);
    }
    if (shot->hover_button == 2 && shot->mode != SHOT_MODE_VIDEO) {
        draw_rounded_rect(cr, btn2_x, btn_y, btn_w, btn_h, 14.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        cairo_fill(cr);
    }

    // Mode icons: 📷 Photo, 🎥 Video
    draw_camera_icon(cr, btn1_x + btn_w / 2.0, btn_y + btn_h / 2.0, 16.0,
                     1.0, 1.0, 1.0, shot->mode == SHOT_MODE_STILL ? 1.0 : 0.60);
    draw_video_camera_icon(cr, btn2_x + btn_w / 2.0, btn_y + btn_h / 2.0, 16.0,
                           1.0, 1.0, 1.0, shot->mode == SHOT_MODE_VIDEO ? 1.0 : 0.60);

    cur_x += mode_seg_w + 8.0;

    // Divider 1
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14);
    cairo_rectangle(cr, cur_x, by + 12.0, 1.0, 24.0);
    cairo_fill(cr);
    cur_x += 9.0;

    // Segment 2: Target Selector (Full Screen, Selection)
    double tw_btn = 34.0;
    double t_btn_y = seg_y;
    double t1_x = cur_x;
    double t2_x = cur_x + 36.0;

    // Highlights for active target
    double act_tx = (shot->target == SHOT_TARGET_FULLSCREEN) ? t1_x : t2_x;
    draw_rounded_rect(cr, act_tx, t_btn_y, tw_btn, seg_h, 8.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.20);
    cairo_fill(cr);

    // Hover feedback
    if (shot->hover_button == 3 && shot->target != SHOT_TARGET_FULLSCREEN) {
        draw_rounded_rect(cr, t1_x, t_btn_y, tw_btn, seg_h, 8.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        cairo_fill(cr);
    } else if (shot->hover_button == 4 && shot->target != SHOT_TARGET_SELECTION) {
        draw_rounded_rect(cr, t2_x, t_btn_y, tw_btn, seg_h, 8.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        cairo_fill(cr);
    }

    // Target icons: 🖥️ Full, ⬚ Selection
    draw_monitor_icon(cr, t1_x + tw_btn / 2.0, t_btn_y + seg_h / 2.0, 18.0,
                      1.0, 1.0, 1.0, shot->target == SHOT_TARGET_FULLSCREEN ? 1.0 : 0.65);
    draw_crop_icon(cr, t2_x + tw_btn / 2.0, t_btn_y + seg_h / 2.0, 18.0,
                   1.0, 1.0, 1.0, shot->target == SHOT_TARGET_SELECTION ? 1.0 : 0.65);

    cur_x += 70.0 + 8.0;

    // Divider 2
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14);
    cairo_rectangle(cr, cur_x, by + 12.0, 1.0, 24.0);
    cairo_fill(cr);
    cur_x += 9.0;

    // Segment 3: Audio / Mic Toggle (Interactive in Video Mode)
    double mic_btn_w = 34.0;
    double mic_x = cur_x;
    if (shot->hover_button == 5 && shot->mode == SHOT_MODE_VIDEO) {
        draw_rounded_rect(cr, mic_x, seg_y, mic_btn_w, seg_h, 8.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
        cairo_fill(cr);
    }
    if (shot->record_audio && shot->mode == SHOT_MODE_VIDEO) {
        draw_rounded_rect(cr, mic_x, seg_y, mic_btn_w, seg_h, 8.0);
        cairo_set_source_rgba(cr, 0.18, 0.82, 0.40, 0.20);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.28, 0.95, 0.50, 0.40);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }
    draw_mic_icon(cr, mic_x + mic_btn_w / 2.0, seg_y + seg_h / 2.0, 16.0,
                  shot->mode == SHOT_MODE_VIDEO ? shot->record_audio : false);

    cur_x += mic_btn_w + 8.0;

    // Divider 3
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14);
    cairo_rectangle(cr, cur_x, by + 12.0, 1.0, 24.0);
    cairo_fill(cr);
    cur_x += 9.0;

    // Segment 4: Action Button (Capture Still / Record Video)
    double act_w = 104.0;
    double act_x = cur_x;
    bool can_act = (shot->target != SHOT_TARGET_SELECTION || (shot->has_selection && shot->sel_w >= 10.0 && shot->sel_h >= 10.0));
    draw_rounded_rect(cr, act_x, seg_y, act_w, seg_h, 16.0);
    if (shot->mode == SHOT_MODE_STILL) {
        // macOS Glass Blue Capture
        if (!can_act) {
            cairo_set_source_rgba(cr, 0.10, 0.42, 0.92, 0.35);
        } else if (shot->hover_button == 6) {
            cairo_set_source_rgba(cr, 0.18, 0.52, 1.0, 0.95);
        } else {
            cairo_set_source_rgba(cr, 0.10, 0.42, 0.92, 0.88);
        }
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, can_act ? 0.25 : 0.10);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        draw_text(cr, "Capture", SHOT_FONT, 12.5, true,
                  1.0, 1.0, 1.0, can_act ? 0.98 : 0.45, act_x + act_w / 2.0, seg_y + seg_h / 2.0, true, true);
    } else {
        // Radiant Red Record
        if (!can_act) {
            cairo_set_source_rgba(cr, 0.85, 0.14, 0.14, 0.35);
        } else if (shot->hover_button == 6) {
            cairo_set_source_rgba(cr, 0.95, 0.20, 0.20, 0.98);
        } else {
            cairo_set_source_rgba(cr, 0.85, 0.14, 0.14, 0.90);
        }
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 0.40, 0.40, can_act ? 0.40 : 0.15);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Red record circle dot
        cairo_arc(cr, act_x + 24.0, seg_y + seg_h / 2.0, 4.5, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, can_act ? 0.95 : 0.40);
        cairo_fill(cr);

        draw_text(cr, "Record", SHOT_FONT, 12.5, true,
                  1.0, 1.0, 1.0, can_act ? 0.98 : 0.45, act_x + 58.0, seg_y + seg_h / 2.0, true, true);
    }

    cur_x += act_w + 8.0;

    // Segment 5: Close Button (✕)
    double close_cx = cur_x + 14.0;
    double close_cy = seg_y + seg_h / 2.0;
    double close_r = 13.0;
    if (shot->hover_button == 7) {
        cairo_arc(cr, close_cx, close_cy, close_r, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
        cairo_fill(cr);
    }
    draw_close_cross(cr, close_cx, close_cy, 14.0, shot->hover_button == 7 ? 1.0 : 0.70);

    cairo_restore(cr);
    cairo_buffer_apply_to_scene(shot->buffer, shot->scene_buffer);
}

// ---------------------------------------------------------------------------
// CAPTURE & RECORDING EXECUTION
// ---------------------------------------------------------------------------

static void capture_command(bool region, int x, int y, int w, int h) {
    char cmd[3072];
    if (region) {
        snprintf(cmd, sizeof(cmd),
                 "d=\"$HOME/Pictures/Screenshots\"; mkdir -p \"$d\" || exit 1; "
                 "f=\"$d/Screenshot_$(date '+%%Y-%%m-%%d_%%H-%%M-%%S')-$$.png\"; "
                 "sleep 0.08; "
                 "if grim -g '%d,%d %dx%d' \"$f\"; then "
                 "wl-copy --type image/png < \"$f\" 2>/dev/null || true; "
                 "notify-send -a \"Screenshot\" -i \"$f\" \"Screenshot Saved\" \"$f\" 2>/dev/null || true; "
                 "echo \"[AuraDE] Screenshot saved: $f\"; "
                 "fi &",
                 x, y, w, h);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "d=\"$HOME/Pictures/Screenshots\"; mkdir -p \"$d\" || exit 1; "
                 "f=\"$d/Screenshot_$(date '+%%Y-%%m-%%d_%%H-%%M-%%S')-$$.png\"; "
                 "sleep 0.08; "
                 "if grim \"$f\"; then "
                 "wl-copy --type image/png < \"$f\" 2>/dev/null || true; "
                 "notify-send -a \"Screenshot\" -i \"$f\" \"Screenshot Saved\" \"$f\" 2>/dev/null || true; "
                 "echo \"[AuraDE] Screenshot saved: $f\"; "
                 "fi &");
    }
    app_launch_command(cmd);
}

static int reap_timer_cb(void *data) {
    aurade_screenshot_t *shot = data;
    if (!shot) return 0;

    bool done = false;
    if (shot->reaping_pid > 0) {
        int status = 0;
        pid_t r = waitpid(shot->reaping_pid, &status, WNOHANG);
        if (r > 0 || r < 0) {
            // Process finished and has been reaped
            done = true;
        } else {
            shot->reap_ticks++;
            if (shot->reap_ticks >= 60) { // 60 * 50ms = 3.0s timeout
                kill(shot->reaping_pid, SIGKILL);
                waitpid(shot->reaping_pid, NULL, WNOHANG);
                done = true;
            }
        }
    } else {
        done = true;
    }

    if (done) {
        if (shot->reap_timer) {
            wl_event_source_remove(shot->reap_timer);
            shot->reap_timer = NULL;
        }
        shot->reaping_pid = 0;

        // Dispatch notification asynchronously using app_launch_command (NEVER system())
        if (shot->reap_output_file[0] != '\0') {
            char cmd[4096];
            snprintf(cmd, sizeof(cmd),
                     "notify-send -a \"Screen Recorder\" -i \"video-x-generic\" "
                     "\"Screen Recording Saved\" \"Saved to %s\" 2>/dev/null &",
                     shot->reap_output_file);
            app_launch_command(cmd);
            shot->reap_output_file[0] = '\0';
        }
        return 0;
    }

    if (shot->reap_timer) {
        wl_event_source_timer_update(shot->reap_timer, 50);
    }
    return 0;
}

static int recording_timer_cb(void *data) {
    aurade_screenshot_t *shot = data;
    if (!shot || !shot->is_recording) return 0;

    int status = 0;
    pid_t r = waitpid(shot->recorder_pid, &status, WNOHANG);
    if (r > 0) {
        // Process exited unexpectedly or completed on its own
        shot->is_recording = false;
        shot->recorder_pid = 0;
        if (shot->recording_timer) {
            wl_event_source_remove(shot->recording_timer);
            shot->recording_timer = NULL;
        }
        if (shot->server && shot->server->topbar) {
            shot->server->topbar->rec_w = 0;
            shot->server->topbar->rec_h = 0;
            shot->server->topbar->hover_rec = false;
            topbar_render(shot->server->topbar);
        }
        if (shot->server) schedule_all_outputs(shot->server);

        char cmd[4096];
        snprintf(cmd, sizeof(cmd),
                 "notify-send -a \"Screen Recorder\" -i \"video-x-generic\" "
                 "\"Screen Recording Saved\" \"Saved to %s\" 2>/dev/null &",
                 shot->recording_output_file);
        app_launch_command(cmd);
        return 0;
    }

    // Refresh topbar every second to update elapsed time timer
    if (shot->server && shot->server->topbar) {
        topbar_render(shot->server->topbar);
    }
    if (shot->server) {
        schedule_all_outputs(shot->server);
    }

    if (shot->recording_timer) {
        wl_event_source_timer_update(shot->recording_timer, 1000);
    }
    return 0;
}

static void start_wf_recorder(aurade_screenshot_t *shot, int x, int y, int w, int h) {
    char dir[512];
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    snprintf(dir, sizeof(dir), "%s/Videos/Recordings", home);
    mkdir(dir, 0755);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char filename[1024];
    snprintf(filename, sizeof(filename), "%s/Recording_%04d-%02d-%02d_%02d-%02d-%02d.mp4",
             dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    snprintf(shot->recording_output_file, sizeof(shot->recording_output_file), "%s", filename);

    // Geometry string: ensure width and height are divisible by 2 for H.264
    char geom_str[64] = {0};
    if (w > 0 && h > 0 && (w < shot->width || h < shot->height || x > 0 || y > 0)) {
        int ew = (w / 2) * 2;
        int eh = (h / 2) * 2;
        if (ew < 2) ew = 2;
        if (eh < 2) eh = 2;
        snprintf(geom_str, sizeof(geom_str), "%d,%d %dx%d", x, y, ew, eh);
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[AuraDE] Failed to fork wf-recorder\n");
        return;
    }

    if (pid == 0) {
        // In child: redirect stdout/stderr
        int log_fd = open("/tmp/aurade_recorder.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        char *argv[32];
        int argc = 0;
        argv[argc++] = "wf-recorder";
        argv[argc++] = "-f";
        argv[argc++] = filename;
        argv[argc++] = "-c";
        argv[argc++] = "libx264";
        argv[argc++] = "-x";
        argv[argc++] = "yuv420p";
        argv[argc++] = "-p";
        argv[argc++] = "preset=superfast";
        argv[argc++] = "-p";
        argv[argc++] = "crf=22";
        if (geom_str[0] != '\0') {
            argv[argc++] = "-g";
            argv[argc++] = geom_str;
        }
        if (shot->record_audio) {
            argv[argc++] = "-a";
        }
        argv[argc] = NULL;

        execvp("wf-recorder", argv);
        _exit(127);
    }

    shot->recorder_pid = pid;
    shot->is_recording = true;
    shot->recording_start_ms = get_time_ms();

    // Register Wayland timer for 1-second ticks
    if (shot->server && shot->server->wl_display) {
        struct wl_event_loop *loop = wl_display_get_event_loop(shot->server->wl_display);
        shot->recording_timer = wl_event_loop_add_timer(loop, recording_timer_cb, shot);
        if (shot->recording_timer) {
            wl_event_source_timer_update(shot->recording_timer, 1000);
        }
    }

    if (shot->server && shot->server->topbar) {
        topbar_render(shot->server->topbar);
    }
    if (shot->server) {
        schedule_all_outputs(shot->server);
    }

    char notif[1024];
    snprintf(notif, sizeof(notif),
             "notify-send -a \"Screen Recorder\" -i \"media-record\" "
             "\"Recording Started\" \"Click the menu bar Stop icon or press Print to finish\" 2>/dev/null &");
    app_launch_command(notif);
}

void screenshot_stop_recording(aurade_screenshot_t *shot) {
    if (!shot || !shot->is_recording) return;

    pid_t pid = shot->recorder_pid;
    shot->is_recording = false;
    shot->recorder_pid = 0;

    if (shot->recording_timer) {
        wl_event_source_remove(shot->recording_timer);
        shot->recording_timer = NULL;
    }

    if (shot->server && shot->server->topbar) {
        shot->server->topbar->rec_w = 0;
        shot->server->topbar->rec_h = 0;
        shot->server->topbar->hover_rec = false;
        topbar_render(shot->server->topbar);
    }
    if (shot->server) {
        schedule_all_outputs(shot->server);
    }

    if (pid > 0) {
        // Send SIGINT so wf-recorder finalizes the MP4 moov atom
        kill(pid, SIGINT);

        shot->reaping_pid = pid;
        shot->reap_ticks = 0;
        snprintf(shot->reap_output_file, sizeof(shot->reap_output_file), "%s",
                 shot->recording_output_file);

        if (shot->server && shot->server->wl_display) {
            struct wl_event_loop *loop = wl_display_get_event_loop(shot->server->wl_display);
            if (shot->reap_timer) {
                wl_event_source_remove(shot->reap_timer);
                shot->reap_timer = NULL;
            }
            shot->reap_timer = wl_event_loop_add_timer(loop, reap_timer_cb, shot);
            if (shot->reap_timer) {
                wl_event_source_timer_update(shot->reap_timer, 50);
            }
        }
    }
}

bool screenshot_is_recording(aurade_screenshot_t *shot) {
    return shot && shot->is_recording;
}

uint32_t screenshot_get_recording_duration_sec(aurade_screenshot_t *shot) {
    if (!shot || !shot->is_recording) return 0;
    uint64_t now = get_time_ms();
    if (now <= shot->recording_start_ms) return 0;
    return (uint32_t)((now - shot->recording_start_ms) / 1000);
}

void screenshot_start_recording(aurade_screenshot_t *shot) {
    if (!shot) return;
    shot->mode = SHOT_MODE_VIDEO;
    screenshot_begin(shot);
}

static void cancel_region(aurade_screenshot_t *shot) {
    if (!shot) return;
    shot->active = false;
    shot->direct_crop = false;
    shot->dragging = false;
    shot->active_handle = HANDLE_NONE;
    wlr_scene_node_set_enabled(&shot->scene_buffer->node, false);
    reset_cursor(shot);
    if (shot->server) schedule_all_outputs(shot->server);
}

static void screenshot_perform_action(aurade_screenshot_t *shot) {
    if (!shot) return;

    int rx = 0, ry = 0, rw = shot->width, rh = shot->height;

    if (shot->target == SHOT_TARGET_SELECTION) {
        if (!shot->has_selection || shot->sel_w < 10.0 || shot->sel_h < 10.0) {
            return;
        }
        rx = (int)floor(shot->sel_x);
        ry = (int)floor(shot->sel_y);
        rw = (int)ceil(shot->sel_w);
        rh = (int)ceil(shot->sel_h);
    } else { // SHOT_TARGET_FULLSCREEN
        rx = 0;
        ry = 0;
        rw = shot->width;
        rh = shot->height;
    }

    rx = clamp_int(rx, 0, shot->width - 1);
    ry = clamp_int(ry, 0, shot->height - 1);
    if (rx + rw > shot->width) rw = shot->width - rx;
    if (ry + rh > shot->height) rh = shot->height - ry;

    bool is_video = (shot->mode == SHOT_MODE_VIDEO);
    cancel_region(shot);

    if (is_video) {
        start_wf_recorder(shot, rx, ry, rw, rh);
    } else {
        capture_command(shot->target != SHOT_TARGET_FULLSCREEN, rx, ry, rw, rh);
    }
}

// ---------------------------------------------------------------------------
// LIFECYCLE & INVOCATION
// ---------------------------------------------------------------------------

aurade_screenshot_t *screenshot_create(aurade_server_t *server) {
    if (!server) return NULL;

    aurade_screenshot_t *shot = calloc(1, sizeof(*shot));
    if (!shot) return NULL;

    shot->server = server;
    shot->width = 1920;
    shot->height = 1080;
    shot->mode = SHOT_MODE_STILL;
    shot->target = SHOT_TARGET_SELECTION;
    shot->record_audio = true; // Audio enabled by default for video recording

    shot->buffer = cairo_buffer_create(shot->width, shot->height);
    if (!shot->buffer) {
        free(shot);
        return NULL;
    }

    shot->scene_buffer = wlr_scene_buffer_create(server->overlay_tree,
                                                  &shot->buffer->base);
    wlr_scene_node_set_enabled(&shot->scene_buffer->node, false);
    wlr_scene_node_set_position(&shot->scene_buffer->node, 0, 0);

    return shot;
}

void screenshot_destroy(aurade_screenshot_t *shot) {
    if (!shot) return;
    if (shot->is_recording && shot->recorder_pid > 0) {
        kill(shot->recorder_pid, SIGINT);
        waitpid(shot->recorder_pid, NULL, WNOHANG);
    }
    if (shot->reap_timer) {
        wl_event_source_remove(shot->reap_timer);
        shot->reap_timer = NULL;
    }
    if (shot->reaping_pid > 0) {
        kill(shot->reaping_pid, SIGKILL);
        waitpid(shot->reaping_pid, NULL, WNOHANG);
    }
    if (shot->recording_timer) {
        wl_event_source_remove(shot->recording_timer);
    }
    if (shot->scene_buffer) {
        wlr_scene_node_destroy(&shot->scene_buffer->node);
    }
    if (shot->buffer) {
        wlr_buffer_drop(&shot->buffer->base);
    }
    free(shot);
}

void screenshot_resize(aurade_screenshot_t *shot, int width, int height) {
    if (!shot) return;
    if (width <= 0 || height <= 0) return;
    if (shot->active) {
        cancel_region(shot);
    }
    shot->width = width;
    shot->height = height;
    cairo_buffer_resize(shot->buffer, width, height);
    cairo_buffer_apply_to_scene(shot->buffer, shot->scene_buffer);
}

bool screenshot_is_active(aurade_screenshot_t *shot) {
    return shot && shot->active;
}

static void screenshot_open(aurade_screenshot_t *shot, bool direct_crop) {
    if (!shot || shot->active) return;

    shot->active = true;
    shot->direct_crop = direct_crop;
    shot->dragging = false;
    shot->active_handle = HANDLE_NONE;
    shot->hover_button = 0;

    // Reset target and selection box so user can freely drag their desired area
    shot->target = SHOT_TARGET_SELECTION;
    shot->has_selection = false;
    shot->sel_x = 0;
    shot->sel_y = 0;
    shot->sel_w = 0;
    shot->sel_h = 0;

    wlr_scene_node_set_position(&shot->scene_buffer->node, 0, 0);
    wlr_scene_node_set_enabled(&shot->scene_buffer->node, true);
    wlr_scene_node_raise_to_top(&shot->scene_buffer->node);
    set_cursor(shot, "crosshair");
    render_overlay(shot);
    if (shot->server) schedule_all_outputs(shot->server);
}

void screenshot_begin(aurade_screenshot_t *shot) {
    screenshot_open(shot, false);
}

void screenshot_begin_region(aurade_screenshot_t *shot) {
    if (!shot) return;
    shot->mode = SHOT_MODE_STILL;
    shot->target = SHOT_TARGET_SELECTION;
    screenshot_open(shot, true);
}

void screenshot_begin_video(aurade_screenshot_t *shot) {
    if (!shot) return;
    shot->mode = SHOT_MODE_VIDEO;
    shot->target = SHOT_TARGET_SELECTION;
    screenshot_open(shot, false);
}

void screenshot_capture_full(aurade_screenshot_t *shot) {
    if (!shot) return;
    capture_command(false, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// INPUT EVENT HANDLING (MOTION, CLICK, KEYS)
// ---------------------------------------------------------------------------

static int toolbar_button_at(aurade_screenshot_t *shot, double x, double y) {
    if (x < shot->bar_x || x > shot->bar_x + shot->bar_w ||
        y < shot->bar_y || y > shot->bar_y + shot->bar_h) {
        return 0; // outside toolbar
    }

    double cur_x = shot->bar_x + 10.0;

    // Button 1: Still mode
    if (x >= cur_x && x <= cur_x + 40.0) return 1;
    // Button 2: Video mode
    if (x >= cur_x + 42.0 && x <= cur_x + 84.0) return 2;
    cur_x += 84.0 + 17.0;

    // Target 3: Fullscreen
    if (x >= cur_x && x <= cur_x + 34.0) return 3;
    // Target 4: Selection
    if (x >= cur_x + 36.0 && x <= cur_x + 70.0) return 4;
    cur_x += 70.0 + 17.0;

    // Option 5: Audio toggle
    if (x >= cur_x && x <= cur_x + 34.0) return 5;
    cur_x += 34.0 + 17.0;

    // Action 6: Capture / Record
    if (x >= cur_x && x <= cur_x + 104.0) return 6;
    cur_x += 104.0 + 8.0;

    // Button 7: Close
    if (x >= cur_x && x <= shot->bar_x + shot->bar_w - 4.0) return 7;

    return 0;
}

bool screenshot_handle_motion(aurade_screenshot_t *shot, double x, double y) {
    if (!shot || !shot->active) return false;

    shot->cur_x = clamp_double(x, 0.0, shot->width - 1.0);
    shot->cur_y = clamp_double(y, 0.0, shot->height - 1.0);

    // If dragging to create, resize, or move selection box:
    if (shot->dragging) {
        if (shot->active_handle == HANDLE_NEW) {
            shot->sel_x = fmin(shot->drag_start_mouse_x, shot->cur_x);
            shot->sel_y = fmin(shot->drag_start_mouse_y, shot->cur_y);
            shot->sel_w = fabs(shot->cur_x - shot->drag_start_mouse_x);
            shot->sel_h = fabs(shot->cur_y - shot->drag_start_mouse_y);
            shot->has_selection = (shot->sel_w >= 4.0 && shot->sel_h >= 4.0);
        } else {
            double dx = shot->cur_x - shot->drag_start_mouse_x;
            double dy = shot->cur_y - shot->drag_start_mouse_y;

            switch (shot->active_handle) {
                case HANDLE_MOVE: {
                    double new_x = shot->drag_start_box_x + dx;
                    double new_y = shot->drag_start_box_y + dy;
                    shot->sel_x = clamp_double(new_x, 0.0, shot->width - shot->sel_w);
                    shot->sel_y = clamp_double(new_y, 0.0, shot->height - shot->sel_h);
                    break;
                }
                case HANDLE_NW: {
                    double right = shot->drag_start_box_x + shot->drag_start_box_w;
                    double bottom = shot->drag_start_box_y + shot->drag_start_box_h;
                    double new_x = clamp_double(shot->drag_start_box_x + dx, 0.0, right - 10.0);
                    double new_y = clamp_double(shot->drag_start_box_y + dy, 0.0, bottom - 10.0);
                    shot->sel_x = new_x;
                    shot->sel_y = new_y;
                    shot->sel_w = right - new_x;
                    shot->sel_h = bottom - new_y;
                    break;
                }
                case HANDLE_NE: {
                    double left = shot->drag_start_box_x;
                    double bottom = shot->drag_start_box_y + shot->drag_start_box_h;
                    double new_y = clamp_double(shot->drag_start_box_y + dy, 0.0, bottom - 10.0);
                    double new_w = clamp_double(shot->drag_start_box_w + dx, 10.0, shot->width - left);
                    shot->sel_y = new_y;
                    shot->sel_w = new_w;
                    shot->sel_h = bottom - new_y;
                    break;
                }
                case HANDLE_SW: {
                    double right = shot->drag_start_box_x + shot->drag_start_box_w;
                    double top = shot->drag_start_box_y;
                    double new_x = clamp_double(shot->drag_start_box_x + dx, 0.0, right - 10.0);
                    double new_h = clamp_double(shot->drag_start_box_h + dy, 10.0, shot->height - top);
                    shot->sel_x = new_x;
                    shot->sel_w = right - new_x;
                    shot->sel_h = new_h;
                    break;
                }
                case HANDLE_SE: {
                    shot->sel_w = clamp_double(shot->drag_start_box_w + dx, 10.0, shot->width - shot->sel_x);
                    shot->sel_h = clamp_double(shot->drag_start_box_h + dy, 10.0, shot->height - shot->sel_y);
                    break;
                }
                case HANDLE_N: {
                    double bottom = shot->drag_start_box_y + shot->drag_start_box_h;
                    double new_y = clamp_double(shot->drag_start_box_y + dy, 0.0, bottom - 10.0);
                    shot->sel_y = new_y;
                    shot->sel_h = bottom - new_y;
                    break;
                }
                case HANDLE_S: {
                    shot->sel_h = clamp_double(shot->drag_start_box_h + dy, 10.0, shot->height - shot->sel_y);
                    break;
                }
                case HANDLE_W: {
                    double right = shot->drag_start_box_x + shot->drag_start_box_w;
                    double new_x = clamp_double(shot->drag_start_box_x + dx, 0.0, right - 10.0);
                    shot->sel_x = new_x;
                    shot->sel_w = right - new_x;
                    break;
                }
                case HANDLE_E: {
                    shot->sel_w = clamp_double(shot->drag_start_box_w + dx, 10.0, shot->width - shot->sel_x);
                    break;
                }
                default:
                    break;
            }
        }

        render_overlay(shot);
        if (shot->server) schedule_all_outputs(shot->server);
        return true;
    }

    // Check hover over toolbar
    int prev_hover = shot->hover_button;
    shot->hover_button = toolbar_button_at(shot, x, y);

    if (shot->hover_button != 0) {
        set_cursor(shot, "default");
        if (shot->hover_button != prev_hover) {
            render_overlay(shot);
            if (shot->server) schedule_all_outputs(shot->server);
        }
        return true;
    }

    if (prev_hover != 0) {
        render_overlay(shot);
        if (shot->server) schedule_all_outputs(shot->server);
    }

    // Selection mode handle hover
    if (shot->target == SHOT_TARGET_SELECTION) {
        if (shot->has_selection && shot->sel_w >= 10.0 && shot->sel_h >= 10.0) {
            drag_handle_t handle = get_handle_at(shot, x, y);
            switch (handle) {
                case HANDLE_NW:
                case HANDLE_SE:
                case HANDLE_NE:
                case HANDLE_SW:
                case HANDLE_N:
                case HANDLE_S:
                case HANDLE_W:
                case HANDLE_E:
                    set_cursor(shot, "crosshair");
                    break;
                case HANDLE_MOVE:
                    set_cursor(shot, "grab");
                    break;
                default:
                    set_cursor(shot, "crosshair");
                    break;
            }
        } else {
            set_cursor(shot, "crosshair");
        }
        return true;
    }

    set_cursor(shot, "crosshair");
    return true;
}

bool screenshot_handle_button(aurade_screenshot_t *shot, double x, double y,
                              uint32_t button, bool pressed) {
    if (!shot || !shot->active) return false;

    if (button == BTN_RIGHT) {
        cancel_region(shot);
        return true;
    }

    if (button != BTN_LEFT) return true;

    if (pressed) {
        int btn = toolbar_button_at(shot, x, y);
        if (btn != 0) {
            switch (btn) {
                case 1: // Still mode
                    shot->mode = SHOT_MODE_STILL;
                    break;
                case 2: // Video mode
                    shot->mode = SHOT_MODE_VIDEO;
                    break;
                case 3: // Fullscreen target
                    shot->target = SHOT_TARGET_FULLSCREEN;
                    break;
                case 4: // Selection target
                    shot->target = SHOT_TARGET_SELECTION;
                    break;
                case 5: // Audio toggle
                    if (shot->mode == SHOT_MODE_VIDEO) {
                        shot->record_audio = !shot->record_audio;
                    }
                    break;
                case 6: // Action (Capture / Record)
                    if (shot->target == SHOT_TARGET_SELECTION && (!shot->has_selection || shot->sel_w < 10.0 || shot->sel_h < 10.0)) {
                        return true;
                    }
                    screenshot_perform_action(shot);
                    return true;
                case 7: // Close
                    cancel_region(shot);
                    return true;
            }
            render_overlay(shot);
            if (shot->server) schedule_all_outputs(shot->server);
            return true;
        }

        // Clicks outside toolbar
        if (shot->target == SHOT_TARGET_FULLSCREEN) {
            screenshot_perform_action(shot);
            return true;
        }

        if (shot->target == SHOT_TARGET_SELECTION) {
            drag_handle_t handle = get_handle_at(shot, x, y);
            if (handle != HANDLE_NONE) {
                shot->active_handle = handle;
                shot->dragging = true;
                shot->drag_start_mouse_x = x;
                shot->drag_start_mouse_y = y;
                shot->drag_start_box_x = shot->sel_x;
                shot->drag_start_box_y = shot->sel_y;
                shot->drag_start_box_w = shot->sel_w;
                shot->drag_start_box_h = shot->sel_h;
                if (handle == HANDLE_MOVE) set_cursor(shot, "grabbing");
            } else {
                // Dragging in empty space creates a new selection rectangle
                shot->sel_x = x;
                shot->sel_y = y;
                shot->sel_w = 0;
                shot->sel_h = 0;
                shot->has_selection = false;
                shot->active_handle = HANDLE_NEW;
                shot->dragging = true;
                shot->drag_start_mouse_x = x;
                shot->drag_start_mouse_y = y;
                shot->drag_start_box_x = x;
                shot->drag_start_box_y = y;
                shot->drag_start_box_w = 0;
                shot->drag_start_box_h = 0;
                set_cursor(shot, "crosshair");
            }
            render_overlay(shot);
            if (shot->server) schedule_all_outputs(shot->server);
            return true;
        }
    } else {
        // Button released
        if (shot->dragging) {
            shot->dragging = false;
            shot->active_handle = HANDLE_NONE;
            normalize_selection(shot);
            if (shot->sel_w >= 10.0 && shot->sel_h >= 10.0) {
                shot->has_selection = true;
                if (shot->direct_crop) {
                    screenshot_perform_action(shot);
                    return true;
                }
            } else {
                shot->has_selection = false;
                shot->sel_w = 0;
                shot->sel_h = 0;
                if (shot->direct_crop) {
                    cancel_region(shot);
                    return true;
                }
            }
            render_overlay(shot);
            if (shot->server) schedule_all_outputs(shot->server);
            return true;
        }
    }

    return true;
}

bool screenshot_handle_key(aurade_screenshot_t *shot, uint32_t sym, bool pressed) {
    if (!shot || !shot->active) return false;

    if (pressed) {
        if (sym == XKB_KEY_Escape) {
            cancel_region(shot);
            return true;
        }
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) {
            if (shot->target != SHOT_TARGET_SELECTION || (shot->has_selection && shot->sel_w >= 10.0 && shot->sel_h >= 10.0)) {
                screenshot_perform_action(shot);
            }
            return true;
        }
        if (sym == XKB_KEY_1) {
            shot->mode = SHOT_MODE_STILL;
            render_overlay(shot);
            if (shot->server) schedule_all_outputs(shot->server);
            return true;
        }
        if (sym == XKB_KEY_2) {
            shot->mode = SHOT_MODE_VIDEO;
            render_overlay(shot);
            if (shot->server) schedule_all_outputs(shot->server);
            return true;
        }
        if (sym == XKB_KEY_m || sym == XKB_KEY_a) {
            if (shot->mode == SHOT_MODE_VIDEO) {
                shot->record_audio = !shot->record_audio;
                render_overlay(shot);
                if (shot->server) schedule_all_outputs(shot->server);
            }
            return true;
        }
    }

    // Swallow keys while modal overlay is active
    return true;
}
