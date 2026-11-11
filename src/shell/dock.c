#include <stdio.h>
#include "dock.h"
#include "launchpad.h"
#include "music_pill.h"
#include "wallpaper.h"
#include "control_center.h"
#include "../util/apps.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DOCK_FONT "Inter"
#define DOCK_PAD 10.0
#define DOCK_GAP 8.0
#define DOCK_BOTTOM_MARGIN 12.0

// How close to the bottom edge the pointer must be for the hidden dock to
// slide back in, and how long it waits before sliding away again.
#define DOCK_REVEAL_EDGE 1.5
#define DOCK_HIDE_DELAY_MS 320
#define DOCK_ANIM_TAU 0.055

// -------------------------------------------------------------
// Icon artwork
// -------------------------------------------------------------

typedef struct { double r, g, b; } rgb_t;

static rgb_t icon_color(const char *name) {
    if (!name || !*name) return (rgb_t){0.42, 0.44, 0.50};
    if (strcasestr(name, "finder") || strcasestr(name, "nautilus") || strcasestr(name, "thunar")) return (rgb_t){0.16, 0.52, 0.96};
    if (strcasestr(name, "launchpad")) return (rgb_t){0.42, 0.44, 0.50};
    if (strcasestr(name, "terminal") || strcasestr(name, "alacritty") || strcasestr(name, "kitty") || strcasestr(name, "foot")) return (rgb_t){0.11, 0.12, 0.16};
    if (strcasestr(name, "browser") || strcasestr(name, "firefox") || strcasestr(name, "chrome")) return (rgb_t){0.10, 0.45, 0.92};
    if (strcasestr(name, "settings") || strcasestr(name, "preferences") || strcasestr(name, "control-center")) return (rgb_t){0.52, 0.55, 0.60};

    // Stable, pleasant hue for unknown apps.
    unsigned hash = 2166136261u;
    for (const char *p = name; *p; p++) {
        hash ^= (unsigned char)*p;
        hash *= 16777619u;
    }
    double h = (hash % 360) / 360.0;
    double r = 0.35 + 0.42 * fabs(sin(h * M_PI));
    double g = 0.35 + 0.42 * fabs(sin((h + 0.33) * M_PI));
    double b = 0.45 + 0.40 * fabs(sin((h + 0.66) * M_PI));
    return (rgb_t){r, g, b};
}

static void draw_squircle_base(cairo_t *cr, double cx, double cy, double size,
                               double corner, rgb_t c, bool two_tone) {
    double half = size / 2.0;
    draw_rounded_rect(cr, cx - half, cy - half, size, size, corner);

    if (two_tone) {
        // Finder's split blue/white face
        cairo_save(cr);
        draw_rounded_rect(cr, cx - half, cy - half, size, size, corner);
        cairo_clip(cr);
        cairo_set_source_rgb(cr, 0.93, 0.96, 1.0);
        cairo_rectangle(cr, cx - half, cy - half, half, size);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.10, 0.46, 0.94);
        cairo_rectangle(cr, cx, cy - half, half, size);
        cairo_fill(cr);
        cairo_restore(cr);
        draw_rounded_rect(cr, cx - half, cy - half, size, size, corner);
    } else {
        cairo_pattern_t *pat = cairo_pattern_create_linear(cx, cy - half, cx, cy + half);
        cairo_pattern_add_color_stop_rgb(pat, 0.0,
                                         fmin(1.0, c.r * 1.18 + 0.05),
                                         fmin(1.0, c.g * 1.18 + 0.05),
                                         fmin(1.0, c.b * 1.18 + 0.05));
        cairo_pattern_add_color_stop_rgb(pat, 1.0, c.r * 0.78, c.g * 0.78, c.b * 0.78);
        cairo_set_source(cr, pat);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(pat);
    }

    // Inner rim + top specular highlight, like macOS app icons.
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.26);
    cairo_set_line_width(cr, fmax(1.0, size * 0.018));
    cairo_stroke(cr);

    cairo_save(cr);
    draw_rounded_rect(cr, cx - half + 1.0, cy - half + 1.0, size - 2.0, size * 0.42, corner * 0.8);
    cairo_pattern_t *gloss = cairo_pattern_create_linear(cx, cy - half, cx, cy);
    cairo_pattern_add_color_stop_rgba(gloss, 0.0, 1.0, 1.0, 1.0, 0.18);
    cairo_pattern_add_color_stop_rgba(gloss, 1.0, 1.0, 1.0, 1.0, 0.0);
    cairo_set_source(cr, gloss);
    cairo_fill(cr);
    cairo_pattern_destroy(gloss);
    cairo_restore(cr);
}

static void draw_dock_icon(cairo_t *cr, double cx, double cy, double size, const char *name) {
    // Standard app icon SVG/PNG themes (such as WhiteSur) have built-in padding for
    // drop shadows, so their visible squircle is ~88% of the icon slot (e.g. 60px for a 68px slot).
    // Scale procedurally drawn icons (like Launchpad) to match standard app icon size.
    size = round(size * 0.88);

    double half = size / 2.0;
    double corner = size * 0.235;
    rgb_t c = icon_color(name);
    bool finder = (strcasecmp(name, "Finder") == 0 || strcasestr(name, "finder") != NULL ||
                   strcasestr(name, "nautilus") != NULL || strcasestr(name, "thunar") != NULL);

    draw_drop_shadow(cr, cx - half, cy - half, size, size, corner, size * 0.10, 0.42);
    draw_squircle_base(cr, cx, cy, size, corner, c, finder);

    if (finder) {
        // Finder's navy smiley
        cairo_set_source_rgba(cr, 0.02, 0.14, 0.34, 0.95);
        cairo_set_line_width(cr, size * 0.055);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_arc(cr, cx, cy + size * 0.03, size * 0.26, 0.18 * M_PI, 0.82 * M_PI);
        cairo_stroke(cr);
        cairo_arc(cr, cx - size * 0.17, cy - size * 0.12, size * 0.037, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_arc(cr, cx + size * 0.17, cy - size * 0.12, size * 0.037, 0, 2 * M_PI);
        cairo_fill(cr);
    } else if (strcasecmp(name, "Launchpad") == 0 || strcasestr(name, "launchpad") != NULL) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.94);
        double dot_r = size * 0.045;
        double sp = size * 0.16;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                cairo_arc(cr, cx + dx * sp, cy + dy * sp, dot_r, 0, 2 * M_PI);
                cairo_fill(cr);
            }
        }
    } else if (strcasecmp(name, "Terminal") == 0 || strcasestr(name, "terminal") != NULL ||
               strcasestr(name, "alacritty") != NULL || strcasestr(name, "kitty") != NULL ||
               strcasestr(name, "foot") != NULL || strcasestr(name, "xterm") != NULL) {
        cairo_set_source_rgba(cr, 0.25, 0.95, 0.45, 0.96);
        cairo_set_line_width(cr, size * 0.065);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_move_to(cr, cx - size * 0.26, cy - size * 0.18);
        cairo_line_to(cr, cx - size * 0.05, cy - size * 0.01);
        cairo_line_to(cr, cx - size * 0.26, cy + size * 0.16);
        cairo_stroke(cr);
        cairo_move_to(cr, cx + size * 0.02, cy + size * 0.16);
        cairo_line_to(cr, cx + size * 0.25, cy + size * 0.16);
        cairo_stroke(cr);
    } else if (strcasecmp(name, "Browser") == 0 || strcasestr(name, "browser") != NULL ||
               strcasestr(name, "firefox") != NULL || strcasestr(name, "chrome") != NULL ||
               strcasestr(name, "zen") != NULL) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
        cairo_set_line_width(cr, size * 0.045);
        cairo_arc(cr, cx, cy, size * 0.27, 0, 2 * M_PI);
        cairo_stroke(cr);
        // Compass needle
        cairo_move_to(cr, cx - size * 0.16, cy + size * 0.16);
        cairo_line_to(cr, cx + size * 0.17, cy - size * 0.17);
        cairo_set_source_rgba(cr, 1.0, 0.25, 0.25, 0.95);
        cairo_set_line_width(cr, size * 0.075);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
        cairo_set_line_width(cr, size * 0.045);
        cairo_arc(cr, cx, cy, size * 0.05, 0, 2 * M_PI);
        cairo_fill(cr);
    } else if (strcasecmp(name, "Settings") == 0 || strcasestr(name, "settings") != NULL ||
               strcasestr(name, "preferences") != NULL || strcasestr(name, "control-center") != NULL) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
        cairo_set_line_width(cr, size * 0.055);
        cairo_arc(cr, cx, cy, size * 0.15, 0, 2 * M_PI);
        cairo_stroke(cr);
        for (int i = 0; i < 8; i++) {
            double a = i * (M_PI / 4.0);
            cairo_move_to(cr, cx + cos(a) * size * 0.17, cy + sin(a) * size * 0.17);
            cairo_line_to(cr, cx + cos(a) * size * 0.28, cy + sin(a) * size * 0.28);
            cairo_stroke(cr);
        }
    } else {
        char letter[2] = { name[0] ? (char)toupper((unsigned char)name[0]) : 'A', '\0' };
        draw_text(cr, letter, DOCK_FONT, size * 0.46, true,
                  1.0, 1.0, 1.0, 0.96, cx, cy - size * 0.02, true, true);
    }
}

static void draw_stacked_icon_card(cairo_t *cr, double cx, double cy, double size, dock_item_t *item,
                                   double shadow_alpha, double overlay_dim, double border_alpha) {
    double half = size / 2.0;
    double corner = size * 0.235;
    draw_drop_shadow(cr, cx - half, cy - half, size, size, corner, size * 0.09, shadow_alpha);

    cairo_save(cr);
    draw_rounded_rect(cr, cx - half, cy - half, size, size, corner);
    cairo_clip(cr);

    if (item->icon_surface) {
        int iw = cairo_image_surface_get_width(item->icon_surface);
        int ih = cairo_image_surface_get_height(item->icon_surface);
        if (iw > 0 && ih > 0) {
            cairo_save(cr);
            cairo_translate(cr, cx - half, cy - half);
            cairo_scale(cr, size / iw, size / ih);
            cairo_set_source_surface(cr, item->icon_surface, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);

            if (overlay_dim > 0.0) {
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, overlay_dim);
                cairo_paint(cr);
            }
        }
    } else {
        cairo_set_source_rgba(cr, 0.22, 0.24, 0.30, 0.92);
        cairo_paint(cr);
    }
    cairo_restore(cr);

    // Specular border highlight around the stacked layer
    cairo_save(cr);
    draw_rounded_rect(cr, cx - half + 0.5, cy - half + 0.5, size - 1.0, size - 1.0, corner);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, border_alpha);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void draw_dock_item_icon(cairo_t *cr, double cx, double cy, double size, dock_item_t *item) {
    if (item->window_count >= 2) {
        double sc = item->current_scale > 0.1 ? item->current_scale : 1.0;
        if (item->window_count >= 3) {
            double s3 = size * 0.93;
            double dx3 = 4.2 * sc;
            double dy3 = -4.5 * sc;
            draw_stacked_icon_card(cr, cx + dx3, cy + dy3, s3, item, 0.32, 0.32, 0.26);
        }
        double s2 = size * 0.965;
        double dx2 = -3.5 * sc;
        double dy2 = -2.8 * sc;
        draw_stacked_icon_card(cr, cx + dx2, cy + dy2, s2, item, 0.38, 0.18, 0.34);
    }

    if (item->icon_surface && cairo_surface_status(item->icon_surface) == CAIRO_STATUS_SUCCESS) {
        int isize = (int)lround(size);
        int iw = cairo_image_surface_get_width(item->icon_surface);
        int ih = cairo_image_surface_get_height(item->icon_surface);
        if (iw > 0 && ih > 0) {
            double half = size / 2.0;
            double corner = size * 0.235;

            // Fast-path: if item scale is near 1.0 (unmagnified), use cached pre-scaled & rounded surface
            if (fabs(item->current_scale - 1.0) < 0.02) {
                if (!item->cached_base_surface || item->cached_base_size != isize) {
                    if (item->cached_base_surface) {
                        cairo_surface_destroy(item->cached_base_surface);
                        item->cached_base_surface = NULL;
                    }
                    item->cached_base_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, isize, isize);
                    cairo_t *ccr = cairo_create(item->cached_base_surface);
                    draw_rounded_rect(ccr, 0, 0, isize, isize, corner);
                    cairo_clip(ccr);
                    cairo_scale(ccr, (double)isize / iw, (double)isize / ih);
                    cairo_set_source_surface(ccr, item->icon_surface, 0, 0);
                    cairo_pattern_set_filter(cairo_get_source(ccr), CAIRO_FILTER_BILINEAR);
                    cairo_paint(ccr);
                    cairo_destroy(ccr);
                    item->cached_base_size = isize;
                }

                draw_drop_shadow(cr, cx - half, cy - half, size, size, corner, size * 0.10, 0.42);
                cairo_set_source_surface(cr, item->cached_base_surface, cx - half, cy - half);
                cairo_paint(cr);
                return;
            }

            // Magnified icon path
            draw_drop_shadow(cr, cx - half, cy - half, size, size, corner, size * 0.10, 0.42);
            cairo_save(cr);
            draw_rounded_rect(cr, cx - half, cy - half, size, size, corner);
            cairo_clip(cr);
            cairo_translate(cr, cx - half, cy - half);
            cairo_scale(cr, size / iw, size / ih);
            cairo_set_source_surface(cr, item->icon_surface, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
            return;
        }
    }
    draw_dock_icon(cr, cx, cy, size, item->name);
}

// -------------------------------------------------------------
// Dock lifecycle
// -------------------------------------------------------------


static uint64_t dock_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static inline bool dock_is_plank(aurade_dock_t *dock) {
    if (!dock || !dock->server) return false;
    const char *t = dock->server->config.dock_theme;
    return (strcmp(t, DOCK_THEME_PLANK) == 0 || strcmp(t, "matte") == 0);
}

static inline bool dock_is_3d(aurade_dock_t *dock) {
    if (!dock || !dock->server) return false;
    return (strcmp(dock->server->config.dock_theme, DOCK_THEME_TABLE_3D) == 0);
}

/* Distance the dock travels between "fully visible" and "fully hidden": just
 * enough for the pill to clear the bottom edge. */
static double dock_slide_distance(aurade_dock_t *dock) {
    if (!dock->autohide && !dock->intellihide) return 0.0;
    bool is_3d = dock_is_3d(dock);
    bool is_plank = dock_is_plank(dock);
    double margin = (is_3d || is_plank) ? 0.0 : DOCK_BOTTOM_MARGIN;
    double slide = dock->pill_height + margin;
    return slide > 1.0 ? slide : 1.0;
}

/* Screen-space top of the pill, accounting for the slide offset. */
static double dock_pill_top(aurade_dock_t *dock) {
    return dock->pill_y + dock->hide_progress * dock_slide_distance(dock);
}

/* Extra space above the pill occupied by magnified icons and the tooltip. */
static double dock_headroom(aurade_dock_t *dock) {
    if (!dock->server || !dock->server->config.dock_magnification) return 0.0;
    double max_scale = dock->server->config.dock_max_scale;
    if (max_scale < 1.0) max_scale = 1.0;
    double base = dock->base_icon_size > 0 ? dock->base_icon_size : 52.0;
    return base * (max_scale - 1.0) + 12.0;
}

/* Bottom edge of the icons (icons are square and sit on this baseline). */
static double dock_icon_bottom(aurade_dock_t *dock) {
    if (dock_is_3d(dock)) {
        double front_rim_h = 5.0;
        double off = front_rim_h + 4.0;
        return dock_pill_top(dock) + dock->pill_height - off;
    }
    if (dock_is_plank(dock)) {
        double bar_h = 10.0;
        return dock_pill_top(dock) + dock->pill_height - bar_h + 1.0;
    }
    return dock_pill_top(dock) + dock->pill_height - 10.0;
}

/* Whether the point is over something the dock actually paints: the pill or
 * one of the icons (which may be magnified above the pill). Everything else in
 * the oversized scene buffer is transparent and must not capture input. */
bool dock_pointer_owns(aurade_dock_t *dock, double x, double y) {
    if (!dock || !dock->enabled) return false;

    if (dock->menu_open) {
        if (x >= dock->menu_x && x <= dock->menu_x + dock->menu_w &&
            y >= dock->menu_y && y <= dock->menu_y + dock->menu_h) {
            return true;
        }
    }

    if (dock->hide_progress > 0.8 || (dock->hide_target && dock->hide_progress > 0.05)) return false;

    double top = dock_pill_top(dock);
    if (x >= dock->pill_x && x <= dock->pill_x + dock->pill_width &&
        y >= top && y <= top + dock->pill_height) {
        return true;
    }

    double icon_bottom = dock_icon_bottom(dock);
    for (int i = 0; i < dock->item_count; i++) {
        double half = dock->items[i].width / 2.0;
        if (half <= 0.0) continue;
        if (fabs(x - dock->items[i].center_x) <= half + 1.0 &&
            y <= icon_bottom + 4.0 &&
            y >= icon_bottom - dock->items[i].width - 2.0) {
            return true;
        }
    }
    return false;
}

void dock_apply_slide(aurade_dock_t *dock) {
    if (!dock || !dock->scene_buffer) return;
    double offset = dock->hide_progress * dock_slide_distance(dock);
    int y = (int)lround(dock->screen_height - dock->buffer_height + offset);
    wlr_scene_node_set_position(&dock->scene_buffer->node, 0, y);
    // A hidden or disabled dock must not take part in scene hit-testing.
    bool visible = dock->enabled && dock->hide_progress < 0.999;
    wlr_scene_node_set_enabled(&dock->scene_buffer->node, visible);
    if (dock->blur_node) {
        int bx = (int)round(dock->pill_x);
        int by = y + (int)round(dock->pill_local_y);
        wlr_scene_node_set_position(&dock->blur_node->node, bx, by);
        if (dock->server && dock->server->config.liquid_glass) {
            wlr_scene_blur_set_liquid_glass(dock->blur_node, true);
        } else {
            wlr_scene_blur_set_liquid_glass(dock->blur_node, false);
        }
        bool pause_blur = dock->server && (
            dock->server->gesture_mode != GESTURE_MODE_IDLE ||
            dock->server->gesture_in_progress ||
            dock->server->gesture_animating ||
            dock->server->workspace_animating
        );
        double dock_op = dock->server ? dock->server->config.dock_opacity : 0.85;
        bool dock_glass = dock->server && (dock->server->config.blur_dock || dock->server->config.liquid_glass);
        float dock_str = (dock->server && dock->server->config.blur_dock) ? (float)dock->server->config.blur_strength : 0.0f;
        wlr_scene_blur_set_strength(dock->blur_node, dock_str);
        wlr_scene_node_set_enabled(&dock->blur_node->node,
                                   visible &&
                                   dock_glass &&
                                   (dock_op > 0.05) && !pause_blur);
    }
}

bool dock_is_sliding(const aurade_dock_t *dock) {
    if (!dock || (!dock->autohide && !dock->intellihide)) return false;
    if (dock->hide_target && dock->hide_at_ms != 0) return false;
    double target = dock->hide_target ? 1.0 : 0.0;
    return fabs(target - dock->hide_progress) >= 0.0005;
}

void dock_tick_slide(aurade_dock_t *dock, double dt) {
    if (!dock || (!dock->autohide && !dock->intellihide)) return;
    if (dock->hide_target && dock->hide_at_ms != 0) return;

    if (dt < 0.001) dt = 0.001;
    if (dt > 0.05) dt = 0.05;

    double target = dock->hide_target ? 1.0 : 0.0;
    if (fabs(target - dock->hide_progress) < 0.0005) {
        if (dock->hide_progress != target) {
            dock->hide_progress = target;
            dock_apply_slide(dock);
        }
        return;
    }

    double k = 1.0 - exp(-dt / DOCK_ANIM_TAU);
    dock->hide_progress += (target - dock->hide_progress) * k;
    if (fabs(target - dock->hide_progress) < 0.003) {
        dock->hide_progress = target;
    }
    dock_apply_slide(dock);
}

static int dock_anim_tick(void *data) {
    aurade_dock_t *dock = data;
    if (!dock) return 0;
    uint64_t now = dock_now_ms();

    if (dock->hide_target && dock->hide_at_ms) {
        if (now < dock->hide_at_ms) {
            int delay = (int)(dock->hide_at_ms - now);
            if (delay < 16) delay = 16;
            if (dock->anim_timer) wl_event_source_timer_update(dock->anim_timer, delay);
            return 0;
        }
        dock->hide_at_ms = 0;   // grace period over: start sliding away
    }

    if (dock->anim_timer) {
        wl_event_source_timer_update(dock->anim_timer, 0);
    }

    if (dock->server) {
        schedule_all_outputs(dock->server);
    }
    return 0;
}

static void dock_arm_animation(aurade_dock_t *dock) {
    if (!dock || !dock->server || !dock->server->wl_display) return;
    if (!dock->anim_timer) {
        struct wl_event_loop *loop = wl_display_get_event_loop(dock->server->wl_display);
        dock->anim_timer = wl_event_loop_add_timer(loop, dock_anim_tick, dock);
    }
    if (dock->hide_target && dock->hide_at_ms) {
        uint64_t now = dock_now_ms();
        int delay = (dock->hide_at_ms > now) ? (int)(dock->hide_at_ms - now) : 16;
        if (delay < 16) delay = 16;
        wl_event_source_timer_update(dock->anim_timer, delay);
    } else {
        if (dock->anim_timer) wl_event_source_timer_update(dock->anim_timer, 0);
    }
    schedule_all_outputs(dock->server);
}

static void dock_request_visible(aurade_dock_t *dock, bool visible) {
    if (dock->server && dock->server->launchpad &&
        (dock->server->launchpad->active || dock->server->gesture_mode == GESTURE_MODE_LAUNCHPAD)) {
        visible = true;
    } else if (!dock->autohide && !dock->intellihide) {
        visible = true;
    }

    if (visible) {
        if (dock->hide_target || dock->hide_at_ms != 0) {
            dock->hide_target = false;
            dock->hide_at_ms = 0;
            if (dock->anim_timer) {
                wl_event_source_timer_update(dock->anim_timer, 0);
            }
            if (dock->hide_progress > 0.0005) {
                if (dock->server) schedule_all_outputs(dock->server);
            }
        }
    } else {
        if (!dock->hide_target) {
            dock->hide_target = true;
            dock->hide_at_ms = dock_now_ms() + DOCK_HIDE_DELAY_MS;
            dock_arm_animation(dock);
        }
    }
}

bool dock_intersects_window(aurade_dock_t *dock) {
    if (!dock || !dock->server) return false;
    aurade_server_t *server = dock->server;

    double pill_x = dock->pill_x;
    double pill_y = dock->pill_y;
    double pill_w = dock->pill_width;
    double pill_h = dock->pill_height;
    if (pill_w <= 0.0 || pill_h <= 0.0) {
        int base = server->config.dock_icon_size > 0 ? server->config.dock_icon_size : 52;
        pill_h = base + 18.0;
        pill_w = (dock->item_count > 0 ? dock->item_count : 5) * (base + 8.0) + 20.0;
        pill_x = (dock->screen_width - pill_w) / 2.0;
        pill_y = dock->screen_height - pill_h - DOCK_BOTTOM_MARGIN;
    }

    double dock_left = pill_x - 6.0;
    double dock_right = pill_x + pill_w + 6.0;
    double dock_top = pill_y - 6.0;
    double dock_bottom = (double)dock->screen_height;

    aurade_view_t *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->xdg_toplevel) {
            if (!view->xdg_toplevel->base || !view->xdg_toplevel->base->surface ||
                !view->xdg_toplevel->base->surface->mapped) continue;
        } else if (view->xwayland_surface) {
            if (!view->xwayland_surface->surface ||
                !view->xwayland_surface->surface->mapped) continue;
        } else {
            continue;
        }
        if (!view->scene_tree || !view->scene_tree->node.enabled) continue;
        if (view->minimized) continue;
        if (view->workspace != server->current_workspace) continue;
        if (view->width <= 0 || view->height <= 0) continue;

        double v_left = view->x;
        double v_right = view->x + view->width;
        double v_top = view->y;
        double v_bottom = view->y + view->height;

        if (v_right > dock_left && v_left < dock_right &&
            v_bottom > dock_top && v_top < dock_bottom) {
            return true;
        }
    }
    return false;
}

void dock_update_visibility(aurade_dock_t *dock) {
    if (!dock || !dock->enabled) return;

    if (dock->server && dock->server->launchpad &&
        (dock->server->launchpad->active || dock->server->gesture_mode == GESTURE_MODE_LAUNCHPAD)) {
        dock_request_visible(dock, true);
        return;
    }

    bool over = !dock->hide_target && dock->mouse_over_dock;

    if (dock->autohide) {
        bool near_edge = dock->mouse_y >= dock->screen_height - DOCK_REVEAL_EDGE;
        dock_request_visible(dock, near_edge || over);
    } else if (dock->intellihide) {
        bool overlaps = dock_intersects_window(dock);
        if (overlaps) {
            bool dragging = (dock->server && (dock->server->cursor_mode == CURSOR_MOVE ||
                                              dock->server->cursor_mode == CURSOR_RESIZE));
            bool near_edge = !dragging && (dock->mouse_y >= dock->screen_height - DOCK_REVEAL_EDGE);
            dock_request_visible(dock, near_edge || (over && !dragging));
        } else {
            dock_request_visible(dock, true);
        }
    } else {
        dock_request_visible(dock, true);
    }
}

bool dock_contains_point(aurade_dock_t *dock, double x, double y) {
    if (!dock || !dock->enabled) return false;
    double top = dock_pill_top(dock) - dock_headroom(dock) - 20.0;
    double bottom = (double)dock->screen_height;
    double left = dock->pill_x - 30.0;
    double right = dock->pill_x + dock->pill_width + 30.0;
    return (x >= left && x <= right && y >= top && y <= bottom);
}

void dock_save_pinned(aurade_dock_t *dock) {
    if (!dock || !dock->server) return;
    int count = 0;
    for (int i = 0; i < dock->item_count && count < 24; i++) {
        if (dock->items[i].is_pinned) {
            snprintf(dock->server->config.dock_pinned[count].name,
                     sizeof(dock->server->config.dock_pinned[0].name), "%s", dock->items[i].name);
            snprintf(dock->server->config.dock_pinned[count].exec,
                     sizeof(dock->server->config.dock_pinned[0].exec), "%s", dock->items[i].exec);
            count++;
        }
    }
    dock->server->config.dock_pinned_count = count;
    config_save(&dock->server->config, NULL);
}

bool dock_pin_app(aurade_dock_t *dock, const char *name, const char *exec,
                  const char *icon_name, cairo_surface_t *icon_surface) {
    if (!dock || !name || !*name) return false;

    // Check if already pinned
    for (int i = 0; i < dock->item_count; i++) {
        if (dock->items[i].is_pinned &&
            (strcasecmp(dock->items[i].name, name) == 0 ||
             (dock->items[i].exec[0] && exec && strcmp(dock->items[i].exec, exec) == 0))) {
            return true;
        }
    }

    // Check if currently present as an unpinned running item
    for (int i = 0; i < dock->item_count; i++) {
        if (!dock->items[i].is_pinned && strcasecmp(dock->items[i].name, name) == 0) {
            dock->items[i].is_pinned = true;
            if (exec && *exec) snprintf(dock->items[i].exec, sizeof(dock->items[i].exec), "%s", exec);
            if (icon_surface && !dock->items[i].icon_surface) {
                dock->items[i].icon_surface = cairo_surface_reference(icon_surface);
            }
            dock_update_running_state(dock);
            dock_render(dock);
            dock_save_pinned(dock);
            if (dock->server && dock->server->launchpad) {
                launchpad_refresh(dock->server->launchpad);
            }
            return true;
        }
    }

    int pinned_count = 0;
    for (int i = 0; i < dock->item_count; i++) {
        if (dock->items[i].is_pinned) pinned_count++;
    }
    if (dock->item_count >= 24) return false;

    // Shift running items to insert new pinned item right after existing pinned items
    for (int i = dock->item_count; i > pinned_count; i--) {
        dock->items[i] = dock->items[i - 1];
    }
    dock_item_t *item = &dock->items[pinned_count];
    memset(item, 0, sizeof(*item));
    snprintf(item->name, sizeof(item->name), "%s", name);
    if (exec) snprintf(item->exec, sizeof(item->exec), "%s", exec);
    if (icon_name) snprintf(item->icon_name, sizeof(item->icon_name), "%s", icon_name);
    if (icon_surface) {
        item->icon_surface = cairo_surface_reference(icon_surface);
    }
    item->current_scale = 1.0;
    item->is_pinned = true;
    item->is_running = false;
    dock->item_count++;

    dock_update_running_state(dock);
    dock_render(dock);
    dock_save_pinned(dock);
    if (dock->server && dock->server->launchpad) {
        launchpad_refresh(dock->server->launchpad);
    }
    return true;
}

static void extract_exec_binary(const char *exec, char *out, size_t out_sz) {
    if (!exec || !*exec || out_sz < 2) return;
    out[0] = '\0';
    const char *cmd_flag = strstr(exec, "--command=");
    if (cmd_flag) {
        cmd_flag += 10;
        size_t len = 0;
        while (cmd_flag[len] && !isspace((unsigned char)cmd_flag[len])) len++;
        if (len > 0 && len < out_sz) {
            strncpy(out, cmd_flag, len);
            out[len] = '\0';
            return;
        }
    }
    while (*exec && isspace((unsigned char)*exec)) exec++;
    const char *end = exec;
    while (*end && !isspace((unsigned char)*end)) end++;
    const char *slash = NULL;
    for (const char *p = exec; p < end; p++) {
        if (*p == '/') slash = p;
    }
    const char *start = slash ? slash + 1 : exec;
    size_t len = end - start;
    if (len >= out_sz) len = out_sz - 1;
    strncpy(out, start, len);
    out[len] = '\0';
}

bool dock_has_app(aurade_dock_t *dock, const desktop_app_t *app) {
    if (!dock || !app) return false;
    for (int i = 0; i < dock->item_count; i++) {
        dock_item_t *item = &dock->items[i];
        if (strcasecmp(item->name, "Launchpad") == 0) continue;

        // 1. Exact match on name
        if (strcasecmp(item->name, app->name) == 0) return true;

        // 2. Exact match on exec
        if (item->exec[0] && app->exec[0] && strcasecmp(item->exec, app->exec) == 0) return true;

        // 3. Exec binary match (e.g. "nautilus --new-window" vs "nautilus %U")
        if (item->exec[0] && app->exec[0]) {
            char bin1[64] = {0}, bin2[64] = {0};
            extract_exec_binary(item->exec, bin1, sizeof(bin1));
            extract_exec_binary(app->exec, bin2, sizeof(bin2));
            if (bin1[0] && bin2[0] && strcasecmp(bin1, bin2) == 0) return true;
        }

        // 4. Exact match on icon name
        if (item->icon_name[0] && app->icon_name[0] &&
            strcasecmp(item->icon_name, app->icon_name) == 0) {
            return true;
        }

        // 5. Dock item name matches binary
        if (item->name[0] && app->exec[0]) {
            char bin[64] = {0};
            extract_exec_binary(app->exec, bin, sizeof(bin));
            if (bin[0] && strcasecmp(item->name, bin) == 0) return true;
        }
    }
    return false;
}

aurade_dock_t *dock_create(aurade_server_t *server) {
    aurade_dock_t *dock = calloc(1, sizeof(*dock));
    dock->server = server;
    dock->screen_width = server->screen_width > 0 ? server->screen_width : 1920;
    dock->screen_height = server->screen_height > 0 ? server->screen_height : 1080;
    dock->buffer_height = 180;
    dock->hovered_index = -1;
    dock->enabled = server->config.dock_enabled;
    dock->autohide = server->config.dock_autohide && dock->enabled;
    dock->intellihide = server->config.dock_intellihide && dock->enabled;
    bool should_hide = dock->autohide || (dock->intellihide && dock_intersects_window(dock));
    dock->hide_progress = should_hide ? 1.0 : 0.0;
    dock->hide_target = should_hide;

    dock->item_count = 0;
    if (server->config.dock_pinned_count > 0) {
        for (int i = 0; i < server->config.dock_pinned_count && dock->item_count < 24; i++) {
            dock->items[i] = (dock_item_t){
                .current_scale = 1.0,
                .is_pinned = true,
            };
            snprintf(dock->items[i].name, sizeof(dock->items[i].name), "%s", server->config.dock_pinned[i].name);
            snprintf(dock->items[i].exec, sizeof(dock->items[i].exec), "%s", server->config.dock_pinned[i].exec);
            if (server->apps) {
                desktop_app_t *app = apps_find(server->apps, dock->items[i].name);
                if (app && app->icon_surface) {
                    dock->items[i].icon_surface = cairo_surface_reference(app->icon_surface);
                }
            }
            dock->item_count++;
        }
    } else {
        dock->item_count = 1;
        dock->items[0] = (dock_item_t){ .name = "Launchpad", .exec = "", .current_scale = 1.0, .is_pinned = true };
    }

    struct wlr_scene_tree *dock_tree = server->dock_tree ? server->dock_tree : server->shell_tree;
    dock->buffer = cairo_buffer_create(dock->screen_width, dock->buffer_height);
    dock->scene_buffer = wlr_scene_buffer_create(dock_tree, &dock->buffer->base);

    dock->blur_node = wlr_scene_blur_create(dock_tree, 200, 60);
    if (dock->blur_node) {
        wlr_scene_node_place_below(&dock->blur_node->node, &dock->scene_buffer->node);
        wlr_scene_node_set_enabled(&dock->blur_node->node, false);
    }

    dock->drag_candidate_index = -1;
    dock->drag_index = -1;
    dock->is_dragging = false;

    dock->menu_buffer = cairo_buffer_create(240, 220);
    dock->menu_scene_buffer = wlr_scene_buffer_create(server->popup_tree ? server->popup_tree :
                                                      (server->dock_tree ? server->dock_tree : server->shell_tree),
                                                      &dock->menu_buffer->base);
    wlr_scene_node_set_enabled(&dock->menu_scene_buffer->node, false);
    dock->menu_open = false;
    dock->menu_item_index = -1;
    dock->menu_hover_row = -1;

    dock_resize(dock, dock->screen_width, dock->screen_height);
    return dock;
}

void dock_resize(aurade_dock_t *dock, int screen_w, int screen_h) {
    if (!dock) return;
    dock->screen_width = screen_w;
    dock->screen_height = screen_h;

    cairo_buffer_resize(dock->buffer, screen_w, dock->buffer_height);
    dock_apply_slide(dock);
    dock_render(dock);
}

static bool app_id_matches(const char *app_id, const char *name) {
    if (!app_id || !*app_id || !name || !*name) return false;
    if (strcasecmp(app_id, name) == 0) return true;
    if (strcasestr(app_id, name) != NULL || strcasestr(name, app_id) != NULL) return true;

    if (strcasecmp(name, "Steam") == 0 && strcasestr(app_id, "steam")) return true;

    // File managers (Files, Finder, Nautilus, Nemo, Thunar, Dolphin, Caja, etc.)
    if ((strcasecmp(name, "Files") == 0 || strcasecmp(name, "Finder") == 0 ||
         strcasestr(name, "nautilus") != NULL || strcasestr(name, "file") != NULL) &&
        (strcasestr(app_id, "nautilus") != NULL || strcasestr(app_id, "org.gnome.Nautilus") != NULL ||
         strcasestr(app_id, "thunar") != NULL || strcasestr(app_id, "dolphin") != NULL ||
         strcasestr(app_id, "pcmanfm") != NULL || strcasestr(app_id, "nemo") != NULL ||
         strcasestr(app_id, "caja") != NULL || strcasestr(app_id, "files") != NULL)) {
        return true;
    }

    // Terminals (Terminal, Foot, Kitty, Alacritty, WezTerm, Konsole, etc.)
    if ((strcasecmp(name, "Terminal") == 0 || strcasestr(name, "terminal") != NULL ||
         strcasecmp(name, "Console") == 0) &&
        (strcasestr(app_id, "foot") != NULL || strcasestr(app_id, "kitty") != NULL ||
         strcasestr(app_id, "terminal") != NULL || strcasestr(app_id, "alacritty") != NULL ||
         strcasestr(app_id, "wezterm") != NULL || strcasestr(app_id, "konsole") != NULL ||
         strcasestr(app_id, "xterm") != NULL)) {
        return true;
    }

    // Browsers (Browser, Web, Firefox, Chrome, Chromium, Brave, Zen, Epiphany, etc.)
    if ((strcasecmp(name, "Browser") == 0 || strcasecmp(name, "Web") == 0 ||
         strcasecmp(name, "Safari") == 0 || strcasestr(name, "browser") != NULL) &&
        (strcasestr(app_id, "firefox") != NULL || strcasestr(app_id, "chrom") != NULL ||
         strcasestr(app_id, "brave") != NULL || strcasestr(app_id, "epiphany") != NULL ||
         strcasestr(app_id, "zen") != NULL || strcasestr(app_id, "opera") != NULL ||
         strcasestr(app_id, "edge") != NULL)) {
        return true;
    }

    // Settings (Settings, Preferences, Control Center, Aurade)
    if ((strcasecmp(name, "Settings") == 0 || strcasecmp(name, "Preferences") == 0 ||
         strcasestr(name, "settings") != NULL || strcasestr(name, "control-center") != NULL) &&
        (strcasestr(app_id, "aurade") != NULL || strcasestr(app_id, "settings") != NULL ||
         strcasestr(app_id, "control-center") != NULL || strcasestr(app_id, "preferences") != NULL)) {
        return true;
    }

    // Text editors / IDEs
    if ((strcasecmp(name, "Editor") == 0 || strcasecmp(name, "Text") == 0 ||
         strcasestr(name, "editor") != NULL) &&
        (strcasestr(app_id, "gedit") != NULL || strcasestr(app_id, "kate") != NULL ||
         strcasestr(app_id, "code") != NULL || strcasestr(app_id, "texteditor") != NULL ||
         strcasestr(app_id, "sublime") != NULL || strcasestr(app_id, "cursor") != NULL)) {
        return true;
    }

    return false;
}

const char *view_get_app_id(aurade_view_t *view) {
    if (!view) return NULL;
    if (view->xdg_toplevel && view->xdg_toplevel->app_id) {
        return view->xdg_toplevel->app_id;
    }
    if (view->xwayland_surface) {
        if (view->xwayland_surface->class && *view->xwayland_surface->class) {
            return view->xwayland_surface->class;
        }
        if (view->xwayland_surface->instance && *view->xwayland_surface->instance) {
            return view->xwayland_surface->instance;
        }
    }
    return NULL;
}

const char *view_get_title(aurade_view_t *view) {
    if (!view) return NULL;
    if (view->xdg_toplevel && view->xdg_toplevel->title) {
        return view->xdg_toplevel->title;
    }
    if (view->xwayland_surface && view->xwayland_surface->title) {
        return view->xwayland_surface->title;
    }
    return NULL;
}

static bool view_matches_dock_item(aurade_view_t *view, dock_item_t *item) {
    if (!view || !item) return false;
    const char *app_id = view_get_app_id(view);
    if (!app_id || !*app_id) return false;

    // 1. Direct match on item name
    if (app_id_matches(app_id, item->name)) return true;

    // 2. Direct match on item icon_name
    if (item->icon_name[0]) {
        if (strcasecmp(app_id, item->icon_name) == 0) return true;
        if (strcasestr(app_id, item->icon_name) != NULL || strcasestr(item->icon_name, app_id) != NULL) return true;
        if (app_id_matches(app_id, item->icon_name)) return true;
    }

    // 3. Match against item exec binary
    if (item->exec[0]) {
        char bin[64] = {0};
        extract_exec_binary(item->exec, bin, sizeof(bin));
        if (bin[0]) {
            if (strcasecmp(app_id, bin) == 0) return true;
            if (strcasestr(app_id, bin) != NULL || strcasestr(bin, app_id) != NULL) return true;
            if (app_id_matches(app_id, bin)) return true;
        }
    }

    return false;
}

static int find_running_views(aurade_dock_t *dock, dock_item_t *item, aurade_view_t *out_views[], int max_views) {
    if (!dock || !item || !dock->server || max_views <= 0) return 0;
    int count = 0;
    aurade_view_t *view;
    wl_list_for_each(view, &dock->server->views, link) {
        if (view->xdg_toplevel) {
            if (!view->xdg_toplevel->base || !view->xdg_toplevel->base->surface ||
                !view->xdg_toplevel->base->surface->mapped) continue;
        } else if (view->xwayland_surface) {
            if (!view->xwayland_surface->surface ||
                !view->xwayland_surface->surface->mapped) continue;
        } else {
            continue;
        }
        if (view_matches_dock_item(view, item)) {
            if (count < max_views) {
                out_views[count++] = view;
            }
        }
    }
    return count;
}


void dock_update_running_state(aurade_dock_t *dock) {
    if (!dock) return;

    // Cleanly separate and compact pinned items to [0 .. pinned_count - 1]
    int pinned_count = 0;
    dock_item_t pinned_items[24];
    for (int i = 0; i < dock->item_count; i++) {
        if (dock->items[i].is_pinned) {
            pinned_items[pinned_count++] = dock->items[i];
        } else {
            if (dock->items[i].icon_surface) {
                cairo_surface_destroy(dock->items[i].icon_surface);
                dock->items[i].icon_surface = NULL;
            }
            if (dock->items[i].cached_base_surface) {
                cairo_surface_destroy(dock->items[i].cached_base_surface);
                dock->items[i].cached_base_surface = NULL;
            }
        }
    }

    for (int i = 0; i < pinned_count; i++) {
        dock->items[i] = pinned_items[i];
        dock->items[i].is_running = false;
        dock->items[i].window_count = 0;
        dock->items[i].demands_attention = false;
    }
    dock->item_count = pinned_count;

    aurade_view_t *view;
    wl_list_for_each(view, &dock->server->views, link) {
        if (view->xdg_toplevel) {
            if (!view->xdg_toplevel->base || !view->xdg_toplevel->base->surface ||
                !view->xdg_toplevel->base->surface->mapped) continue;
        } else if (view->xwayland_surface) {
            if (!view->xwayland_surface->surface ||
                !view->xwayland_surface->surface->mapped) continue;
        } else {
            continue;
        }

        const char *app_id = view_get_app_id(view);
        if (!app_id) continue;
        bool matched = false;

        // 1. Check pinned items
        for (int i = 0; i < pinned_count; i++) {
            if (view_matches_dock_item(view, &dock->items[i])) {
                dock->items[i].is_running = true;
                dock->items[i].window_count++;
                if (view->demands_attention) dock->items[i].demands_attention = true;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        // 2. Already represented by an unpinned dynamic item?
        for (int i = pinned_count; i < dock->item_count; i++) {
            if (view_matches_dock_item(view, &dock->items[i])) {
                dock->items[i].is_running = true;
                dock->items[i].window_count++;
                if (view->demands_attention) dock->items[i].demands_attention = true;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        // 3. New dynamic item
        if (dock->item_count < 24) {
            dock_item_t *dyn = &dock->items[dock->item_count];
            memset(dyn, 0, sizeof(*dyn));

            desktop_app_t *app = NULL;
            if (dock->server && dock->server->apps) {
                app = apps_find(dock->server->apps, app_id);
                if (!app && strchr(app_id, '.')) {
                    const char *dot = strrchr(app_id, '.');
                    if (dot && *(dot + 1)) {
                        app = apps_find(dock->server->apps, dot + 1);
                    }
                }
                if (!app) {
                    for (int a = 0; a < dock->server->apps->count; a++) {
                        desktop_app_t *cand = &dock->server->apps->apps[a];
                        if (strcasecmp(cand->name, app_id) == 0 ||
                            strcasestr(app_id, cand->name) != NULL ||
                            (cand->exec[0] && strcasestr(app_id, cand->exec) != NULL)) {
                            app = cand;
                            break;
                        }
                    }
                }
            }

            if (app) {
                snprintf(dyn->name, sizeof(dyn->name), "%.*s",
                         (int)sizeof(dyn->name) - 1, app->name);
                snprintf(dyn->exec, sizeof(dyn->exec), "%.*s",
                         (int)sizeof(dyn->exec) - 1, app->exec);
                snprintf(dyn->icon_name, sizeof(dyn->icon_name), "%.*s",
                         (int)sizeof(dyn->icon_name) - 1, app->icon_name);
                if (app->icon_surface) {
                    dyn->icon_surface = cairo_surface_reference(app->icon_surface);
                }
            } else {
                snprintf(dyn->name, sizeof(dyn->name), "%.*s",
                         (int)sizeof(dyn->name) - 1, app_id);
            }

            if (!dyn->icon_surface) {
                if (dyn->icon_name[0]) {
                    dyn->icon_surface = apps_load_icon(dyn->icon_name);
                }
                if (!dyn->icon_surface) {
                    dyn->icon_surface = apps_load_icon(app_id);
                }
                if (!dyn->icon_surface && strchr(app_id, '.')) {
                    const char *dot = strrchr(app_id, '.');
                    if (dot && *(dot + 1)) {
                        dyn->icon_surface = apps_load_icon(dot + 1);
                    }
                }
            }

            dyn->is_running = true;
            dyn->window_count = 1;
            dyn->demands_attention = view->demands_attention;
            dyn->is_pinned = false;
            dyn->current_scale = 1.0;
            dock->item_count++;
        }
    }
}

bool dock_get_item_center(aurade_dock_t *dock, const char *name, double *out_x, double *out_y) {
    if (!dock) return false;
    double icon_bottom = dock_icon_bottom(dock);
    if (name && name[0]) {
        for (int i = 0; i < dock->item_count; i++) {
            if (app_id_matches(name, dock->items[i].name) ||
                strcasecmp(dock->items[i].name, name) == 0 ||
                (dock->items[i].exec[0] && strstr(name, dock->items[i].name) != NULL)) {
                if (out_x) *out_x = dock->items[i].center_x;
                if (out_y) *out_y = icon_bottom - dock->items[i].width / 2.0;
                return true;
            }
        }
    }
    if (out_x) *out_x = dock->pill_x + dock->pill_width / 2.0;
    if (out_y) *out_y = icon_bottom - (dock->base_icon_size > 0 ? dock->base_icon_size : 48) / 2.0;
    return true;
}

void dock_start_bounce(aurade_dock_t *dock, const char *name) {
    if (!dock || !name) return;
    for (int i = 0; i < dock->item_count; i++) {
        if (strcasecmp(dock->items[i].name, name) == 0 ||
            app_id_matches(name, dock->items[i].name) ||
            strstr(dock->items[i].exec, name) != NULL ||
            strstr(name, dock->items[i].name) != NULL) {
            dock->items[i].is_bouncing = true;
            dock->items[i].bounce_offset = 0.0;
            dock->items[i].bounce_vel = -420.0;
            dock->items[i].bounce_count = 0;
            if (dock->server) schedule_all_outputs(dock->server);
            break;
        }
    }
}

void dock_stop_bounce(aurade_dock_t *dock, const char *name) {
    if (!dock) return;
    for (int i = 0; i < dock->item_count; i++) {
        if (!name || strcasecmp(dock->items[i].name, name) == 0 ||
            app_id_matches(name, dock->items[i].name)) {
            dock->items[i].is_bouncing = false;
        }
    }
}

bool dock_is_bouncing(aurade_dock_t *dock) {
    if (!dock) return false;
    for (int i = 0; i < dock->item_count; i++) {
        if (dock->items[i].is_bouncing || dock->items[i].bounce_offset < -0.2) {
            return true;
        }
    }
    return false;
}

void dock_tick_bounce(aurade_dock_t *dock, double dt) {
    if (!dock) return;
    bool any_changed = false;
    const double g = 2600.0; // px/s^2

    for (int i = 0; i < dock->item_count; i++) {
        dock_item_t *item = &dock->items[i];
        if (item->is_bouncing || item->bounce_offset < -0.1 || fabs(item->bounce_vel) > 1.0) {
            any_changed = true;
            item->bounce_vel += g * dt;
            item->bounce_offset += item->bounce_vel * dt;

            if (item->bounce_offset >= 0.0) {
                item->bounce_offset = 0.0;
                if (item->is_bouncing) {
                    item->bounce_vel = -390.0; // Hop up again!
                    item->bounce_count++;
                    if (item->bounce_count >= 14) {
                        item->is_bouncing = false;
                        item->bounce_vel = 0.0;
                    }
                } else {
                    item->bounce_vel = 0.0;
                }
            }
        }
    }

    if (any_changed) {
        dock_render(dock);
    }
}

void dock_refresh(aurade_server_t *server) {
    if (!server || !server->dock) return;
    dock_update_running_state(server->dock);
    dock_render(server->dock);
    dock_update_visibility(server->dock);
    schedule_all_outputs(server);
}

static void draw_tooltip(aurade_dock_t *dock, cairo_t *cr, const char *label,
                         double cx, double pill_top) {
    if (!label || !*label) return;
    double tw = text_width(cr, label, DOCK_FONT, 12.0, true);
    double bw = tw + 20.0;
    double bh = 24.0;
    double bx = cx - bw / 2.0;
    if (bx < 8.0) bx = 8.0;
    if (bx + bw > dock->screen_width - 8.0) bx = dock->screen_width - 8.0 - bw;
    double by = pill_top - bh - 8.0;
    if (by < 4.0) by = 4.0;

    draw_drop_shadow(cr, bx, by, bw, bh, bh / 2.0, 8.0, 0.35);
    draw_glass_panel(cr, bx, by, bw, bh, bh / 2.0, 0.10, 0.11, 0.14, 0.82);

    // Callout tip pointing to the icon
    cairo_move_to(cr, cx - 4.5, by + bh);
    cairo_line_to(cr, cx, by + bh + 4.0);
    cairo_line_to(cr, cx + 4.5, by + bh);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.10, 0.11, 0.14, 0.82);
    cairo_fill(cr);

    draw_text(cr, label, DOCK_FONT, 12.0, true,
              1.0, 1.0, 1.0, 0.96, bx + bw / 2.0, by + bh / 2.0, true, true);
}

void dock_render(aurade_dock_t *dock) {
    if (!dock || !dock->buffer || !dock->buffer->cr) return;
    dock->needs_render = false;
    cairo_t *cr = dock->buffer->cr;
    cairo_buffer_clear(dock->buffer);

    // Running state is refreshed by the view map/unmap hooks and the config
    // reload path; scanning every view on each pointer-move render was wasteful.

    // Config may have changed at runtime (the reload watcher calls us).
    dock->enabled = dock->server->config.dock_enabled;
    dock->autohide = dock->server->config.dock_autohide && dock->enabled;
    dock->intellihide = dock->server->config.dock_intellihide && dock->enabled;
    if (!dock->enabled) {
        dock->hide_target = true;
        dock->hide_at_ms = 0;
        dock->hide_progress = 1.0;
    }
    if (!dock->autohide && !dock->intellihide) {
        dock->hide_target = false;
        dock->hide_at_ms = 0;
        dock->hide_progress = 0.0;
    }

    int count = dock->item_count;
    if (count <= 0) {
        cairo_buffer_apply_to_scene(dock->buffer, dock->scene_buffer);
        return;
    }

    int base = dock->server->config.dock_icon_size;
    if (base < 32) base = 32;
    if (base > 96) base = 96;
    dock->base_icon_size = base;

    bool magnify = dock->server->config.dock_magnification && dock->mouse_over_dock && !dock->menu_open && !dock->is_dragging;
    double max_scale = dock->server->config.dock_max_scale;
    if (max_scale < 1.0) max_scale = 1.0;
    if (max_scale > 2.2) max_scale = 2.2;
    double radius = dock->server->config.dock_mag_radius;
    if (radius < 40.0) radius = 40.0;

    // --- pass 1: widths from current centre guesses
    double width[24];
    double used = 0.0;
    int pinned_count = 0;
    for (int i = 0; i < count; i++) {
        if (dock->items[i].is_pinned) pinned_count++;
    }
    bool has_separator = (pinned_count > 0 && pinned_count < count);

    for (int i = 0; i < count; i++) {
        double scale = 1.0;
        if (magnify) {
            double d = fabs(dock->mouse_x - dock->items[i].center_x);
            if (d < radius) {
                double c = cos((M_PI * d) / (2.0 * radius));
                scale = 1.0 + (max_scale - 1.0) * c * c;
            }
        }
        dock->items[i].current_scale = scale;
        width[i] = base * scale;
        used += width[i];
    }
    used += (count - 1) * DOCK_GAP + DOCK_PAD * 2.0;
    if (has_separator) used += 13.0;

    music_pill_pos_t mp_pos = dock->server->music_pill ?
        music_pill_get_pos(dock->server->music_pill) : MUSIC_PILL_POS_DISABLED;
    bool has_mp_dock = dock->server->music_pill && dock->server->music_pill->active &&
        (mp_pos == MUSIC_PILL_POS_DOCK_LEFT || mp_pos == MUSIC_PILL_POS_DOCK_RIGHT);
    double mp_w = has_mp_dock ? 150.0 : 0.0;
    if (has_mp_dock) {
        used += mp_w + DOCK_GAP + 12.0;
    }

    bool is_3d = dock_is_3d(dock);
    bool is_plank = dock_is_plank(dock);
    bool dark = !dock->server || dock->server->config.dark_mode;
    if (dock->server && dock->server->control_center && dock->server->control_center->dark_mode_available) {
        dark = dock->server->control_center->dark_mode;
    }
    double front_rim_h = 5.0;
    double shelf_depth = 14.0;
    double table_h = front_rim_h + shelf_depth;
    double plank_bar_h = 10.0;

    double pill_h = is_3d ? (base + table_h + 2.0) : (is_plank ? (base + plank_bar_h + 4.0) : (base + 18.0));
    double pill_x = (dock->screen_width - used) / 2.0;
    double bottom_margin = (is_3d || is_plank) ? 0.0 : DOCK_BOTTOM_MARGIN;
    double pill_local_y = dock->buffer_height - pill_h - bottom_margin;
    double bar_local_y = dock->buffer_height - plank_bar_h;

    double rim_overhang = 44.0;
    double table_bottom_w = used + rim_overhang * 2.0;
    double table_bottom_x = pill_x - rim_overhang;
    double perspective_slant = 34.0;
    double table_top_x = table_bottom_x + perspective_slant;
    double table_top_w = table_bottom_w - perspective_slant * 2.0;
    double shelf_top_y = pill_local_y + pill_h - front_rim_h - shelf_depth;
    double shelf_rim_y = pill_local_y + pill_h - front_rim_h;

    // Screen-space rect for input handling. The scene node is bottom-aligned.
    if (is_3d) {
        dock->pill_x = table_bottom_x;
        dock->pill_y = dock->screen_height - pill_h - bottom_margin;
        dock->pill_width = table_bottom_w;
        dock->pill_height = pill_h;
    } else {
        dock->pill_x = pill_x;
        dock->pill_y = dock->screen_height - pill_h - bottom_margin;
        dock->pill_width = used;
        dock->pill_height = pill_h;
    }
    dock->pill_local_y = pill_local_y;

    // Recompute the final icon centres. (Magnification widths intentionally use
    // the previous frame's centres, exactly like the classic macOS dock hump.)
    double cur_x = pill_x + DOCK_PAD;
    double mp_draw_x = -1.0;
    double left_sep_x = -1.0;
    if (has_mp_dock && mp_pos == MUSIC_PILL_POS_DOCK_LEFT) {
        mp_draw_x = cur_x;
        cur_x += mp_w + DOCK_GAP;
        left_sep_x = cur_x - DOCK_GAP / 2.0;
        cur_x += 8.0;
    }

    double pinned_sep_x = -1.0;
    for (int i = 0; i < count; i++) {
        if (has_separator && i == pinned_count) {
            pinned_sep_x = cur_x + 5.0;
            cur_x += 13.0;
        }
        double w = width[i];
        double cx = cur_x + w / 2.0;
        dock->items[i].center_x = cx;
        dock->items[i].width = w;
        cur_x += w + DOCK_GAP;
    }

    double right_sep_x = -1.0;
    if (has_mp_dock && mp_pos == MUSIC_PILL_POS_DOCK_RIGHT) {
        right_sep_x = cur_x + 2.0;
        cur_x += 10.0;
        mp_draw_x = cur_x;
    }

    double dock_op = dock->server ? dock->server->config.dock_opacity : 0.85;
    if (dock_op < 0.0) dock_op = 0.0;
    if (dock_op > 1.0) dock_op = 1.0;

    if (dock->blur_node) {
        double offset = dock->hide_progress * dock_slide_distance(dock);
        int y = (int)lround(dock->screen_height - dock->buffer_height + offset);
        int bx = is_3d ? (int)round(table_bottom_x) : (int)round(pill_x);
        int by = is_3d ? (y + (int)round(shelf_top_y)) : (is_plank ? (y + (int)round(bar_local_y)) : (y + (int)round(pill_local_y)));
        int bw = is_3d ? (int)round(table_bottom_w) : (int)round(used);
        int bh = is_3d ? (int)round(table_h) : (is_plank ? (int)round(plank_bar_h) : (int)round(pill_h));
        int br = is_3d ? 6 : (is_plank ? 3 : 22);
        wlr_scene_blur_set_size(dock->blur_node, bw, bh);
        wlr_scene_blur_set_corner_radius(dock->blur_node, br);
        wlr_scene_node_set_position(&dock->blur_node->node, bx, by);
        if (dock->server && dock->server->config.liquid_glass) {
            wlr_scene_blur_set_liquid_glass(dock->blur_node, true);
        } else {
            wlr_scene_blur_set_liquid_glass(dock->blur_node, false);
        }
        bool dock_glass = dock->server && (dock->server->config.blur_dock || dock->server->config.liquid_glass);
        float dock_str = (dock->server && dock->server->config.blur_dock) ? (float)dock->server->config.blur_strength : 0.0f;
        wlr_scene_blur_set_strength(dock->blur_node, dock_str);
        bool pause_blur = dock->server && (
            dock->server->gesture_mode != GESTURE_MODE_IDLE ||
            dock->server->gesture_in_progress ||
            dock->server->gesture_animating ||
            dock->server->workspace_animating
        );
        wlr_scene_node_set_enabled(&dock->blur_node->node,
                                   dock->enabled && dock->hide_progress < 0.999 &&
                                   dock_glass &&
                                   (dock_op > 0.05) &&
                                   !pause_blur);
    }

    if (is_3d) {
        // --- CLASSIC OS X 3D GLASS TABLE / SHELF ---

        double r_top = 5.0;
        double r_btm = 3.0;
        double r_rim_btm = 2.5;

        // Slanted edge unit vectors
        double dx_l = table_top_x - table_bottom_x;
        double dy_l = shelf_top_y - shelf_rim_y;
        double len_l = hypot(dx_l, dy_l);
        double ux_l = dx_l / (len_l > 0.0 ? len_l : 1.0);
        double uy_l = dy_l / (len_l > 0.0 ? len_l : 1.0);

        double dx_r = (table_bottom_x + table_bottom_w) - (table_top_x + table_top_w);
        double dy_r = shelf_rim_y - shelf_top_y;
        double len_r = hypot(dx_r, dy_r);
        double ux_r = dx_r / (len_r > 0.0 ? len_r : 1.0);
        double uy_r = dy_r / (len_r > 0.0 ? len_r : 1.0);

        // Top-left corner control points
        double tl_in_x = table_top_x - r_top * ux_l;
        double tl_in_y = shelf_top_y - r_top * uy_l;
        double tl_out_x = table_top_x + r_top;
        double tl_out_y = shelf_top_y;
        double tl_cp1_x = (tl_in_x + 2.0 * table_top_x) / 3.0;
        double tl_cp1_y = (tl_in_y + 2.0 * shelf_top_y) / 3.0;
        double tl_cp2_x = (tl_out_x + 2.0 * table_top_x) / 3.0;
        double tl_cp2_y = (tl_out_y + 2.0 * shelf_top_y) / 3.0;

        // Top-right corner control points
        double tr_in_x = table_top_x + table_top_w - r_top;
        double tr_in_y = shelf_top_y;
        double tr_out_x = table_top_x + table_top_w + r_top * ux_r;
        double tr_out_y = shelf_top_y + r_top * uy_r;
        double tr_cp1_x = (tr_in_x + 2.0 * (table_top_x + table_top_w)) / 3.0;
        double tr_cp1_y = (tr_in_y + 2.0 * shelf_top_y) / 3.0;
        double tr_cp2_x = (tr_out_x + 2.0 * (table_top_x + table_top_w)) / 3.0;
        double tr_cp2_y = (tr_out_y + 2.0 * shelf_top_y) / 3.0;

        // Bottom-right corner control points
        double br_in_x = (table_bottom_x + table_bottom_w) - r_btm * ux_r;
        double br_in_y = shelf_rim_y - r_btm * uy_r;
        double br_out_x = (table_bottom_x + table_bottom_w) - r_btm;
        double br_out_y = shelf_rim_y;
        double br_cp1_x = (br_in_x + 2.0 * (table_bottom_x + table_bottom_w)) / 3.0;
        double br_cp1_y = (br_in_y + 2.0 * shelf_rim_y) / 3.0;
        double br_cp2_x = (br_out_x + 2.0 * (table_bottom_x + table_bottom_w)) / 3.0;
        double br_cp2_y = (br_out_y + 2.0 * shelf_rim_y) / 3.0;

        // Bottom-left corner control points
        double bl_in_x = table_bottom_x + r_btm;
        double bl_in_y = shelf_rim_y;
        double bl_out_x = table_bottom_x + r_btm * ux_l;
        double bl_out_y = shelf_rim_y + r_btm * uy_l;
        double bl_cp1_x = (bl_in_x + 2.0 * table_bottom_x) / 3.0;
        double bl_cp1_y = (bl_in_y + 2.0 * shelf_rim_y) / 3.0;
        double bl_cp2_x = (bl_out_x + 2.0 * table_bottom_x) / 3.0;
        double bl_cp2_y = (bl_out_y + 2.0 * shelf_rim_y) / 3.0;

        // 1. Drop shadow beneath the shelf
        cairo_save(cr);
        cairo_pattern_t *sh = cairo_pattern_create_linear(0, shelf_rim_y, 0, shelf_rim_y + front_rim_h + 8.0);
        cairo_pattern_add_color_stop_rgba(sh, 0.0, 0, 0, 0, (dark ? 0.35 : 0.20) * dock_op);
        cairo_pattern_add_color_stop_rgba(sh, 1.0, 0, 0, 0, 0.0);
        draw_rounded_rect(cr, table_bottom_x - 6.0, shelf_rim_y, table_bottom_w + 12.0, front_rim_h + 8.0, 4.0);
        cairo_set_source(cr, sh);
        cairo_fill(cr);
        cairo_pattern_destroy(sh);
        cairo_restore(cr);

        // 2. The 3D Slanted Shelf Tabletop (Frosted Smoked / Light Glass with rounded corners)
        cairo_save(cr);
        cairo_new_path(cr);
        cairo_move_to(cr, tl_out_x, tl_out_y);
        cairo_line_to(cr, tr_in_x, tr_in_y);
        cairo_curve_to(cr, tr_cp1_x, tr_cp1_y, tr_cp2_x, tr_cp2_y, tr_out_x, tr_out_y);
        cairo_line_to(cr, br_in_x, br_in_y);
        cairo_curve_to(cr, br_cp1_x, br_cp1_y, br_cp2_x, br_cp2_y, br_out_x, br_out_y);
        cairo_line_to(cr, bl_in_x, bl_in_y);
        cairo_curve_to(cr, bl_cp1_x, bl_cp1_y, bl_cp2_x, bl_cp2_y, bl_out_x, bl_out_y);
        cairo_line_to(cr, tl_in_x, tl_in_y);
        cairo_curve_to(cr, tl_cp1_x, tl_cp1_y, tl_cp2_x, tl_cp2_y, tl_out_x, tl_out_y);
        cairo_close_path(cr);

        cairo_pattern_t *pat = cairo_pattern_create_linear(0, shelf_top_y, 0, shelf_rim_y);
        if (dark) {
            cairo_pattern_add_color_stop_rgba(pat, 0.0,  0.22, 0.25, 0.30, 0.52 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 0.35, 0.14, 0.16, 0.20, 0.70 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 0.75, 0.09, 0.10, 0.14, 0.82 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 1.0,  0.06, 0.07, 0.10, 0.90 * dock_op);
        } else {
            cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.88, 0.92, 0.98, 0.52 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 0.35, 0.76, 0.80, 0.88, 0.60 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 0.80, 0.65, 0.70, 0.78, 0.68 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.58, 0.63, 0.72, 0.78 * dock_op);
        }
        cairo_set_source(cr, pat);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(pat);

        // Top back edge highlight: follows the top curve and top edge smoothly
        cairo_new_path(cr);
        cairo_move_to(cr, tl_in_x, tl_in_y + 0.5);
        cairo_curve_to(cr, tl_cp1_x, tl_cp1_y + 0.5, tl_cp2_x, tl_cp2_y + 0.5, tl_out_x, tl_out_y + 0.5);
        cairo_line_to(cr, tr_in_x, tr_in_y + 0.5);
        cairo_curve_to(cr, tr_cp1_x, tr_cp1_y + 0.5, tr_cp2_x, tr_cp2_y + 0.5, tr_out_x, tr_out_y + 0.5);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (dark ? 0.38 : 0.90) * dock_op);
        cairo_set_line_width(cr, dark ? 1.0 : 1.2);
        cairo_stroke(cr);

        // Angled side bevels
        cairo_new_path(cr);
        cairo_move_to(cr, tl_in_x, tl_in_y);
        cairo_line_to(cr, bl_out_x, bl_out_y);
        cairo_move_to(cr, tr_out_x, tr_out_y);
        cairo_line_to(cr, br_in_x, br_in_y);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (dark ? 0.22 : 0.55) * dock_op);
        cairo_set_line_width(cr, dark ? 0.9 : 1.2);
        cairo_stroke(cr);
        cairo_restore(cr);

        // 3. The Front Vertical Rim / Lip (with rounded bottom corners)
        cairo_save(cr);
        cairo_new_path(cr);
        cairo_move_to(cr, table_bottom_x, shelf_rim_y);
        cairo_line_to(cr, table_bottom_x + table_bottom_w, shelf_rim_y);
        cairo_line_to(cr, table_bottom_x + table_bottom_w, shelf_rim_y + front_rim_h - r_rim_btm);
        cairo_curve_to(cr, table_bottom_x + table_bottom_w, shelf_rim_y + front_rim_h,
                           table_bottom_x + table_bottom_w, shelf_rim_y + front_rim_h,
                           table_bottom_x + table_bottom_w - r_rim_btm, shelf_rim_y + front_rim_h);
        cairo_line_to(cr, table_bottom_x + r_rim_btm, shelf_rim_y + front_rim_h);
        cairo_curve_to(cr, table_bottom_x, shelf_rim_y + front_rim_h,
                           table_bottom_x, shelf_rim_y + front_rim_h,
                           table_bottom_x, shelf_rim_y + front_rim_h - r_rim_btm);
        cairo_close_path(cr);

        cairo_pattern_t *rim_pat = cairo_pattern_create_linear(0, shelf_rim_y, 0, shelf_rim_y + front_rim_h);
        if (dark) {
            cairo_pattern_add_color_stop_rgba(rim_pat, 0.0,  0.18, 0.20, 0.25, 0.92 * dock_op);
            cairo_pattern_add_color_stop_rgba(rim_pat, 0.40, 0.11, 0.12, 0.16, 0.96 * dock_op);
            cairo_pattern_add_color_stop_rgba(rim_pat, 1.0,  0.05, 0.06, 0.08, 0.98 * dock_op);
        } else {
            cairo_pattern_add_color_stop_rgba(rim_pat, 0.0, 0.42, 0.46, 0.52, 0.85 * dock_op);
            cairo_pattern_add_color_stop_rgba(rim_pat, 0.25, 0.25, 0.28, 0.33, 0.92 * dock_op);
            cairo_pattern_add_color_stop_rgba(rim_pat, 1.0, 0.14, 0.16, 0.20, 0.98 * dock_op);
        }
        cairo_set_source(cr, rim_pat);
        cairo_fill(cr);
        cairo_pattern_destroy(rim_pat);

        // Shelf rim highlight (bright line separating tabletop and front rim)
        cairo_new_path(cr);
        cairo_move_to(cr, table_bottom_x + 1.0, shelf_rim_y + 0.5);
        cairo_line_to(cr, table_bottom_x + table_bottom_w - 1.0, shelf_rim_y + 0.5);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (dark ? 0.45 : 0.95) * dock_op);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Bottom dark edge of front rim, following the rounded bottom corners
        cairo_new_path(cr);
        cairo_move_to(cr, table_bottom_x + 0.5, shelf_rim_y + front_rim_h - r_rim_btm);
        cairo_curve_to(cr, table_bottom_x + 0.5, shelf_rim_y + front_rim_h - 0.5,
                           table_bottom_x + 0.5, shelf_rim_y + front_rim_h - 0.5,
                           table_bottom_x + r_rim_btm, shelf_rim_y + front_rim_h - 0.5);
        cairo_line_to(cr, table_bottom_x + table_bottom_w - r_rim_btm, shelf_rim_y + front_rim_h - 0.5);
        cairo_curve_to(cr, table_bottom_x + table_bottom_w - 0.5, shelf_rim_y + front_rim_h - 0.5,
                           table_bottom_x + table_bottom_w - 0.5, shelf_rim_y + front_rim_h - 0.5,
                           table_bottom_x + table_bottom_w - 0.5, shelf_rim_y + front_rim_h - r_rim_btm);
        cairo_set_source_rgba(cr, 0.02, 0.03, 0.04, 0.90 * dock_op);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Outer side edges of front rim
        cairo_new_path(cr);
        cairo_move_to(cr, table_bottom_x + 0.5, shelf_rim_y);
        cairo_line_to(cr, table_bottom_x + 0.5, shelf_rim_y + front_rim_h - r_rim_btm);
        cairo_move_to(cr, table_bottom_x + table_bottom_w - 0.5, shelf_rim_y);
        cairo_line_to(cr, table_bottom_x + table_bottom_w - 0.5, shelf_rim_y + front_rim_h - r_rim_btm);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (dark ? 0.20 : 0.40) * dock_op);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_restore(cr);

        // 4. 3D Separator (Slanted glass divider plaque on shelf)
        if (pinned_sep_x >= 0.0) {
            cairo_save(cr);
            double sep_ratio = (pinned_sep_x - table_bottom_x) / table_bottom_w;
            double top_sep_x = table_top_x + sep_ratio * table_top_w;
            double btm_sep_x = pinned_sep_x;

            cairo_move_to(cr, btm_sep_x - 1.2, shelf_rim_y - 0.5);
            cairo_line_to(cr, top_sep_x - 1.2, shelf_top_y + 1.5);
            cairo_line_to(cr, top_sep_x + 1.2, shelf_top_y + 1.5);
            cairo_line_to(cr, btm_sep_x + 1.2, shelf_rim_y - 0.5);
            cairo_close_path(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, dark ? 0.08 : 0.25);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, dark ? 0.28 : 0.65);
            cairo_set_line_width(cr, 0.8);
            cairo_stroke(cr);

            // Front rim notch for separator
            cairo_move_to(cr, btm_sep_x, shelf_rim_y);
            cairo_line_to(cr, btm_sep_x, shelf_rim_y + front_rim_h);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, dark ? 0.15 : 0.35);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    } else if (is_plank) {
        // --- PLANK MATTE / GLASS DOCK THEME ---
        double bar_r = 3.5;
        double bar_x = pill_x;
        double bar_w = used;
        double bar_y = bar_local_y;
        double bar_h = plank_bar_h;

        // 1. Subtle top drop shadow just above the bar
        cairo_save(cr);
        cairo_pattern_t *sh = cairo_pattern_create_linear(0, bar_y - 4.0, 0, bar_y);
        cairo_pattern_add_color_stop_rgba(sh, 0.0, 0, 0, 0, 0.0);
        cairo_pattern_add_color_stop_rgba(sh, 1.0, 0, 0, 0, (dark ? 0.35 : 0.18) * dock_op);
        cairo_rectangle(cr, bar_x - 4.0, bar_y - 4.0, bar_w + 8.0, 4.0);
        cairo_set_source(cr, sh);
        cairo_fill(cr);
        cairo_pattern_destroy(sh);
        cairo_restore(cr);

        // 2. Bar shape path with rounded top corners and flat bottom
        cairo_save(cr);
        cairo_new_path(cr);
        cairo_move_to(cr, bar_x, bar_y + bar_h);
        cairo_line_to(cr, bar_x, bar_y + bar_r);
        cairo_arc(cr, bar_x + bar_r, bar_y + bar_r, bar_r, M_PI, 1.5 * M_PI);
        cairo_line_to(cr, bar_x + bar_w - bar_r, bar_y);
        cairo_arc(cr, bar_x + bar_w - bar_r, bar_y + bar_r, bar_r, 1.5 * M_PI, 2.0 * M_PI);
        cairo_line_to(cr, bar_x + bar_w, bar_y + bar_h);
        cairo_close_path(cr);

        // Gradient body fill: darker at the top, slightly lighter at bottom (inverted inset bevel)
        cairo_pattern_t *pat = cairo_pattern_create_linear(0, bar_y, 0, bar_y + bar_h);
        if (dark) {
            cairo_pattern_add_color_stop_rgba(pat, 0.0,  0.23, 0.23, 0.25, 0.88 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 0.35, 0.22, 0.22, 0.24, 0.90 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 0.75, 0.27, 0.27, 0.29, 0.92 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 1.0,  0.31, 0.31, 0.33, 0.95 * dock_op);
        } else {
            cairo_pattern_add_color_stop_rgba(pat, 0.0,  0.88, 0.88, 0.90, 0.88 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 0.35, 0.85, 0.85, 0.87, 0.90 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 0.75, 0.90, 0.90, 0.92, 0.92 * dock_op);
            cairo_pattern_add_color_stop_rgba(pat, 1.0,  0.95, 0.95, 0.97, 0.95 * dock_op);
        }
        cairo_set_source(cr, pat);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(pat);

        // Outer crisp rim/border around the shape
        cairo_set_source_rgba(cr, 0.08, 0.09, 0.11, (dark ? 0.85 : 0.40) * dock_op);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Top edge bright highlight line (specular bevel)
        cairo_new_path(cr);
        cairo_move_to(cr, bar_x + bar_r, bar_y + 0.5);
        cairo_line_to(cr, bar_x + bar_w - bar_r, bar_y + 0.5);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (dark ? 0.42 : 0.75) * dock_op);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Bottom dark grounding line
        cairo_new_path(cr);
        cairo_move_to(cr, bar_x, bar_y + bar_h - 0.5);
        cairo_line_to(cr, bar_x + bar_w, bar_y + bar_h - 0.5);
        cairo_set_source_rgba(cr, 0.05, 0.06, 0.08, 0.95 * dock_op);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Separators in Plank theme
        if (left_sep_x >= 0.0) {
            cairo_rectangle(cr, left_sep_x, bar_y + 2.0, 1.0, bar_h - 3.5);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22 * dock_op);
            cairo_fill(cr);
        }
        if (pinned_sep_x >= 0.0) {
            cairo_rectangle(cr, pinned_sep_x, bar_y + 2.0, 1.0, bar_h - 3.5);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22 * dock_op);
            cairo_fill(cr);
        }
        if (right_sep_x >= 0.0) {
            cairo_rectangle(cr, right_sep_x, bar_y + 2.0, 1.0, bar_h - 3.5);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22 * dock_op);
            cairo_fill(cr);
        }
        cairo_restore(cr);
    } else {
        // --- MODERN FLOATING GLASS PILL ---
        double pill_r = 22.0;
        if (pill_r > pill_h / 2.0) pill_r = pill_h / 2.0;
        dock->pill_r = pill_r;
        draw_drop_shadow(cr, pill_x, pill_local_y, used, pill_h, pill_r, 20.0, 0.55 * dock_op);

        cairo_surface_t *bg_surf = cairo_buffer_get_wallpaper_surface();
        if (!bg_surf && dock->server && dock->server->wallpaper && dock->server->wallpaper->buffer) {
            bg_surf = dock->server->wallpaper->buffer->surface;
        }
        double scr_x = pill_x;
        double scr_y = (dock->screen_height - dock->buffer_height) + pill_local_y;

        if (dock->server && (dock->server->config.blur_dock || dock->server->config.liquid_glass)) {
            cairo_save(cr);

            // 1. Refractive depth gradient body tint overlay (translucent)
            draw_rounded_rect(cr, pill_x, pill_local_y, used, pill_h, pill_r);
            cairo_pattern_t *bg = cairo_pattern_create_linear(pill_x, pill_local_y, pill_x, pill_local_y + pill_h);
            cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.14, 0.16, 0.22, 0.28 * dock_op);
            cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.06, 0.07, 0.10, 0.40 * dock_op);
            cairo_set_source(cr, bg);
            cairo_fill(cr);
            cairo_pattern_destroy(bg);

            // 2. Caustic surface sheen wash across top 40%
            cairo_save(cr);
            draw_rounded_rect(cr, pill_x, pill_local_y, used, pill_h, pill_r);
            cairo_clip(cr);
            double sheen_h = pill_h * 0.40;
            cairo_pattern_t *sheen = cairo_pattern_create_linear(pill_x, pill_local_y, pill_x, pill_local_y + sheen_h);
            cairo_pattern_add_color_stop_rgba(sheen, 0.0, 1.0, 1.0, 1.0, 0.035 * dock_op);
            cairo_pattern_add_color_stop_rgba(sheen, 0.6, 1.0, 1.0, 1.0, 0.01 * dock_op);
            cairo_pattern_add_color_stop_rgba(sheen, 1.0, 1.0, 1.0, 1.0, 0.0);
            cairo_rectangle(cr, pill_x, pill_local_y, used, sheen_h);
            cairo_set_source(cr, sheen);
            cairo_fill(cr);
            cairo_pattern_destroy(sheen);
            cairo_restore(cr);

            // 3. Apple Liquid Glass Refractive Edge Cushion ("заломлення світла по сторонах")
            draw_refractive_glass_rim(cr, pill_x, pill_local_y, used, pill_h, pill_r, 0.90 * dock_op);

            cairo_restore(cr);
        } else {
            draw_glass_panel_at(cr, pill_x, pill_local_y, used, pill_h, pill_r,
                                scr_x, scr_y, 0.10, 0.11, 0.14, 0.78 * dock_op);
        }

        // Separators drawn crisply with physical cut-glass groove
        if (left_sep_x >= 0.0) {
            cairo_rectangle(cr, left_sep_x, pill_local_y + 14.0, 1.0, pill_h - 28.0);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.22 * dock_op);
            cairo_fill(cr);
            cairo_rectangle(cr, left_sep_x + 1.0, pill_local_y + 14.0, 1.0, pill_h - 28.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.24 * dock_op);
            cairo_fill(cr);
        }
        if (pinned_sep_x >= 0.0) {
            cairo_rectangle(cr, pinned_sep_x, pill_local_y + 14.0, 1.0, pill_h - 28.0);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.22 * dock_op);
            cairo_fill(cr);
            cairo_rectangle(cr, pinned_sep_x + 1.0, pill_local_y + 14.0, 1.0, pill_h - 28.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.24 * dock_op);
            cairo_fill(cr);
        }
        if (right_sep_x >= 0.0) {
            cairo_rectangle(cr, right_sep_x, pill_local_y + 14.0, 1.0, pill_h - 28.0);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.22 * dock_op);
            cairo_fill(cr);
            cairo_rectangle(cr, right_sep_x + 1.0, pill_local_y + 14.0, 1.0, pill_h - 28.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.24 * dock_op);
            cairo_fill(cr);
        }
    }

    if (has_mp_dock && mp_draw_x >= 0.0) {
        double mp_h = is_3d ? (base * 0.72) : (is_plank ? (base * 0.75) : (pill_h - 12.0));
        double mp_y = is_3d ? (shelf_rim_y - mp_h - 4.0) : (is_plank ? (bar_local_y - mp_h - 2.0) : (pill_local_y + 6.0));
        dock->server->music_pill->dock_pill_x = mp_draw_x;
        dock->server->music_pill->dock_pill_y = dock->pill_y + (is_3d ? (shelf_rim_y - pill_local_y - mp_h - 4.0) : (is_plank ? (bar_local_y - pill_local_y - mp_h - 2.0) : 6.0));
        dock->server->music_pill->dock_pill_w = mp_w;
        dock->server->music_pill->dock_pill_h = mp_h;
        music_pill_render_dock(dock->server->music_pill, cr, mp_draw_x, mp_y, mp_w, mp_h);
    }

    dock->hovered_index = -1;
    double best_scale = 1.0;

    double icon_bottom = is_3d ? (shelf_rim_y - 1.0) : (is_plank ? (bar_local_y + 1.0) : (pill_local_y + pill_h - 10.0));
    int drag_draw_idx = -1;

    for (int i = 0; i < count; i++) {
        if (dock->is_dragging && i == dock->drag_index) {
            drag_draw_idx = i;
            continue;
        }
        double w = dock->items[i].width;
        double cx = dock->items[i].center_x;
        double cy = icon_bottom - w / 2.0 + dock->items[i].bounce_offset;

        // In 3D Table mode, render subtle icon reflection on the glass tabletop
        if (is_3d && dock->items[i].icon_surface) {
            cairo_surface_t *surf = dock->items[i].cached_base_surface ?
                                    dock->items[i].cached_base_surface : dock->items[i].icon_surface;
            int iw = cairo_image_surface_get_width(surf);
            int ih = cairo_image_surface_get_height(surf);
            if (iw > 0 && ih > 0) {
                cairo_save(cr);
                cairo_rectangle(cr, cx - w / 2.0 - 4.0, icon_bottom, w + 8.0, shelf_depth + front_rim_h + 4.0);
                cairo_clip(cr);
                cairo_translate(cr, cx - w / 2.0, icon_bottom);
                cairo_scale(cr, w / (double)iw, -0.22 * (w / (double)ih));
                cairo_set_source_surface(cr, surf, 0, -ih);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                cairo_paint_with_alpha(cr, dark ? 0.18 : 0.28);
                cairo_restore(cr);
            }
        }

        draw_dock_item_icon(cr, cx, cy, w, &dock->items[i]);

        if (is_3d) {
            double led_y = shelf_rim_y + front_rim_h / 2.0;
            if (dock->items[i].demands_attention) {
                cairo_pattern_t *glow = cairo_pattern_create_radial(cx, led_y, 0.3, cx, led_y, 3.5);
                cairo_pattern_add_color_stop_rgba(glow, 0.0, 1.0, 0.7, 0.2, 0.95);
                cairo_pattern_add_color_stop_rgba(glow, 0.5, 1.0, 0.45, 0.1, 0.50);
                cairo_pattern_add_color_stop_rgba(glow, 1.0, 1.0, 0.3, 0.0, 0.0);
                cairo_arc(cr, cx, led_y, 3.5, 0, 2 * M_PI);
                cairo_set_source(cr, glow);
                cairo_fill(cr);
                cairo_pattern_destroy(glow);

                cairo_arc(cr, cx, led_y, 1.3, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 0.95, 0.85, 0.98);
                cairo_fill(cr);
            } else if (dock->items[i].is_running) {
                int wcount = dock->items[i].window_count;
                if (wcount <= 1) {
                    cairo_pattern_t *glow = cairo_pattern_create_radial(cx, led_y, 0.3, cx, led_y, 3.5);
                    cairo_pattern_add_color_stop_rgba(glow, 0.0, 0.4, 0.85, 1.0, 0.95);
                    cairo_pattern_add_color_stop_rgba(glow, 0.5, 0.15, 0.55, 0.95, 0.45);
                    cairo_pattern_add_color_stop_rgba(glow, 1.0, 0.0, 0.3, 0.8, 0.0);
                    cairo_arc(cr, cx, led_y, 3.5, 0, 2 * M_PI);
                    cairo_set_source(cr, glow);
                    cairo_fill(cr);
                    cairo_pattern_destroy(glow);

                    cairo_arc(cr, cx, led_y, 1.3, 0, 2 * M_PI);
                    cairo_set_source_rgba(cr, 0.92, 0.97, 1.0, 0.98);
                    cairo_fill(cr);
                } else if (wcount == 2) {
                    double dots[2] = { cx - 3.5, cx + 3.5 };
                    for (int d = 0; d < 2; d++) {
                        cairo_pattern_t *glow = cairo_pattern_create_radial(dots[d], led_y, 0.2, dots[d], led_y, 2.8);
                        cairo_pattern_add_color_stop_rgba(glow, 0.0, 0.4, 0.85, 1.0, 0.95);
                        cairo_pattern_add_color_stop_rgba(glow, 0.5, 0.15, 0.55, 0.95, 0.45);
                        cairo_pattern_add_color_stop_rgba(glow, 1.0, 0.0, 0.3, 0.8, 0.0);
                        cairo_arc(cr, dots[d], led_y, 2.8, 0, 2 * M_PI);
                        cairo_set_source(cr, glow);
                        cairo_fill(cr);
                        cairo_pattern_destroy(glow);

                        cairo_arc(cr, dots[d], led_y, 1.1, 0, 2 * M_PI);
                        cairo_set_source_rgba(cr, 0.92, 0.97, 1.0, 0.98);
                        cairo_fill(cr);
                    }
                } else {
                    double dots[3] = { cx - 5.5, cx, cx + 5.5 };
                    for (int d = 0; d < 3; d++) {
                        cairo_pattern_t *glow = cairo_pattern_create_radial(dots[d], led_y, 0.2, dots[d], led_y, 2.5);
                        cairo_pattern_add_color_stop_rgba(glow, 0.0, 0.4, 0.85, 1.0, 0.95);
                        cairo_pattern_add_color_stop_rgba(glow, 0.5, 0.15, 0.55, 0.95, 0.45);
                        cairo_pattern_add_color_stop_rgba(glow, 1.0, 0.0, 0.3, 0.8, 0.0);
                        cairo_arc(cr, dots[d], led_y, 2.5, 0, 2 * M_PI);
                        cairo_set_source(cr, glow);
                        cairo_fill(cr);
                        cairo_pattern_destroy(glow);

                        cairo_arc(cr, dots[d], led_y, 1.0, 0, 2 * M_PI);
                        cairo_set_source_rgba(cr, 0.92, 0.97, 1.0, 0.98);
                        cairo_fill(cr);
                    }
                }
            }
        } else if (is_plank) {
            // Plank amber/orange glowing LED indicator
            double led_y = bar_local_y + plank_bar_h / 2.0 + 0.8;
            if (dock->items[i].demands_attention) {
                cairo_pattern_t *glow = cairo_pattern_create_radial(cx, led_y, 0.3, cx, led_y, 4.0);
                cairo_pattern_add_color_stop_rgba(glow, 0.0, 1.0, 0.60, 0.15, 0.98);
                cairo_pattern_add_color_stop_rgba(glow, 0.5, 1.0, 0.30, 0.05, 0.55);
                cairo_pattern_add_color_stop_rgba(glow, 1.0, 0.9, 0.20, 0.00, 0.0);
                cairo_arc(cr, cx, led_y, 4.0, 0, 2 * M_PI);
                cairo_set_source(cr, glow);
                cairo_fill(cr);
                cairo_pattern_destroy(glow);

                cairo_arc(cr, cx, led_y, 1.4, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 0.92, 0.80, 0.98);
                cairo_fill(cr);
            } else if (dock->items[i].is_running) {
                int wcount = dock->items[i].window_count;
                if (wcount <= 1) {
                    cairo_pattern_t *glow = cairo_pattern_create_radial(cx, led_y, 0.2, cx, led_y, 3.8);
                    cairo_pattern_add_color_stop_rgba(glow, 0.0, 0.96, 0.58, 0.22, 0.95);
                    cairo_pattern_add_color_stop_rgba(glow, 0.45, 0.92, 0.36, 0.10, 0.50);
                    cairo_pattern_add_color_stop_rgba(glow, 1.0, 0.85, 0.25, 0.05, 0.0);
                    cairo_arc(cr, cx, led_y, 3.8, 0, 2 * M_PI);
                    cairo_set_source(cr, glow);
                    cairo_fill(cr);
                    cairo_pattern_destroy(glow);

                    cairo_arc(cr, cx, led_y, 1.3, 0, 2 * M_PI);
                    cairo_set_source_rgba(cr, 1.0, 0.88, 0.72, 0.98);
                    cairo_fill(cr);
                } else if (wcount == 2) {
                    double dots[2] = { cx - 3.5, cx + 3.5 };
                    for (int d = 0; d < 2; d++) {
                        cairo_pattern_t *glow = cairo_pattern_create_radial(dots[d], led_y, 0.2, dots[d], led_y, 3.0);
                        cairo_pattern_add_color_stop_rgba(glow, 0.0, 0.96, 0.58, 0.22, 0.95);
                        cairo_pattern_add_color_stop_rgba(glow, 0.45, 0.92, 0.36, 0.10, 0.50);
                        cairo_pattern_add_color_stop_rgba(glow, 1.0, 0.85, 0.25, 0.05, 0.0);
                        cairo_arc(cr, dots[d], led_y, 3.0, 0, 2 * M_PI);
                        cairo_set_source(cr, glow);
                        cairo_fill(cr);
                        cairo_pattern_destroy(glow);

                        cairo_arc(cr, dots[d], led_y, 1.1, 0, 2 * M_PI);
                        cairo_set_source_rgba(cr, 1.0, 0.88, 0.72, 0.98);
                        cairo_fill(cr);
                    }
                } else {
                    double dots[3] = { cx - 5.5, cx, cx + 5.5 };
                    for (int d = 0; d < 3; d++) {
                        cairo_pattern_t *glow = cairo_pattern_create_radial(dots[d], led_y, 0.2, dots[d], led_y, 2.6);
                        cairo_pattern_add_color_stop_rgba(glow, 0.0, 0.96, 0.58, 0.22, 0.95);
                        cairo_pattern_add_color_stop_rgba(glow, 0.45, 0.92, 0.36, 0.10, 0.50);
                        cairo_pattern_add_color_stop_rgba(glow, 1.0, 0.85, 0.25, 0.05, 0.0);
                        cairo_arc(cr, dots[d], led_y, 2.6, 0, 2 * M_PI);
                        cairo_set_source(cr, glow);
                        cairo_fill(cr);
                        cairo_pattern_destroy(glow);

                        cairo_arc(cr, dots[d], led_y, 1.0, 0, 2 * M_PI);
                        cairo_set_source_rgba(cr, 1.0, 0.88, 0.72, 0.98);
                        cairo_fill(cr);
                    }
                }
            }
        } else {
            if (dock->items[i].demands_attention) {
                cairo_arc(cr, cx, pill_local_y + pill_h - 5.0, 3.0, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 0.45, 0.10, 0.98);
                cairo_fill(cr);
            } else if (dock->items[i].is_running) {
                int wcount = dock->items[i].window_count;
                double dot_y = pill_local_y + pill_h - 5.0;
                double alpha = dock->items[i].is_pinned ? 1.0 : 0.75;
                if (wcount <= 1) {
                    cairo_arc(cr, cx, dot_y, 2.1, 0, 2 * M_PI);
                    cairo_set_source_rgba(cr, alpha, 1.0, 1.0, 0.86);
                    cairo_fill(cr);
                } else if (wcount == 2) {
                    cairo_arc(cr, cx - 3.5, dot_y, 1.8, 0, 2 * M_PI);
                    cairo_set_source_rgba(cr, alpha, 1.0, 1.0, 0.86);
                    cairo_fill(cr);
                    cairo_arc(cr, cx + 3.5, dot_y, 1.8, 0, 2 * M_PI);
                    cairo_set_source_rgba(cr, alpha, 1.0, 1.0, 0.86);
                    cairo_fill(cr);
                } else {
                    double dots[3] = { cx - 5.5, cx, cx + 5.5 };
                    for (int d = 0; d < 3; d++) {
                        cairo_arc(cr, dots[d], dot_y, 1.6, 0, 2 * M_PI);
                        cairo_set_source_rgba(cr, alpha, 1.0, 1.0, 0.86);
                        cairo_fill(cr);
                    }
                }
            }
        }

        if (!dock->is_dragging && dock->mouse_over_dock && dock->items[i].current_scale > best_scale) {
            best_scale = dock->items[i].current_scale;
            dock->hovered_index = i;
        }
    }

    if (drag_draw_idx >= 0) {
        double w = dock->items[drag_draw_idx].width * 1.06;
        double cx = dock->drag_current_x;
        if (cx < pill_x + DOCK_PAD) cx = pill_x + DOCK_PAD;
        if (cx > pill_x + used - DOCK_PAD) cx = pill_x + used - DOCK_PAD;
        double cy = icon_bottom - w / 2.0 - 8.0;

        draw_drop_shadow(cr, cx - w / 2.0, cy - w / 2.0, w, w, w * 0.235, 12.0, 0.55);
        draw_dock_item_icon(cr, cx, cy, w, &dock->items[drag_draw_idx]);

        if (is_3d) {
            double led_y = shelf_rim_y + front_rim_h / 2.0;
            if (dock->items[drag_draw_idx].demands_attention) {
                cairo_arc(cr, cx, led_y, 1.4, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 0.45, 0.10, 0.98);
                cairo_fill(cr);
            } else if (dock->items[drag_draw_idx].is_running) {
                cairo_arc(cr, cx, led_y, 1.4, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 0.7, 0.9, 1.0, 0.95);
                cairo_fill(cr);
            }
        } else if (is_plank) {
            double led_y = bar_local_y + plank_bar_h / 2.0 + 0.8;
            if (dock->items[drag_draw_idx].demands_attention) {
                cairo_arc(cr, cx, led_y, 1.4, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 0.55, 0.15, 0.98);
                cairo_fill(cr);
            } else if (dock->items[drag_draw_idx].is_running) {
                cairo_arc(cr, cx, led_y, 1.3, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 0.88, 0.72, 0.98);
                cairo_fill(cr);
            }
        } else {
            if (dock->items[drag_draw_idx].demands_attention) {
                cairo_arc(cr, cx, pill_local_y + pill_h - 5.0, 3.0, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 0.45, 0.10, 0.98);
                cairo_fill(cr);
            } else if (dock->items[drag_draw_idx].is_running) {
                cairo_arc(cr, cx, pill_local_y + pill_h - 5.0, 2.1, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, dock->items[drag_draw_idx].is_pinned ? 1.0 : 0.75,
                                      1.0, 1.0, 0.86);
                cairo_fill(cr);
            }
        }
    }

    // macOS-style app name tooltip above the hovered icon.
    if (!dock->is_dragging && dock->mouse_over_dock && dock->hovered_index >= 0) {
        dock_item_t *hitem = &dock->items[dock->hovered_index];
        char tip[96];
        if (hitem->window_count >= 2) {
            snprintf(tip, sizeof(tip), "%s (%d)", hitem->name, hitem->window_count);
        } else {
            snprintf(tip, sizeof(tip), "%s", hitem->name);
        }
        draw_tooltip(dock, cr, tip,
                     hitem->center_x,
                     is_3d ? (shelf_top_y - 2.0) : (is_plank ? (icon_bottom - hitem->width - 8.0) : pill_local_y));
    }

    cairo_buffer_apply_to_scene(dock->buffer, dock->scene_buffer);

    // The slide offset depends on the pill height, which is only known after
    // layout; re-apply it every render so a hidden dock stays off-screen.
    dock_apply_slide(dock);
}

static void dock_render_context_menu(aurade_dock_t *dock) {
    if (!dock || !dock->menu_buffer || !dock->menu_buffer->cr) return;
    cairo_t *cr = dock->menu_buffer->cr;
    cairo_buffer_clear(dock->menu_buffer);

    if (!dock->menu_open || dock->menu_row_count <= 0) {
        cairo_buffer_apply_to_scene(dock->menu_buffer, dock->menu_scene_buffer);
        return;
    }

    double mw = dock->menu_w;
    double mh = dock->menu_h;
    double off = 10.0;

    // Soft drop shadow
    draw_drop_shadow(cr, off, off, mw, mh, 10.0, 10.0, 0.40);

    // Frosted glass background
    draw_glass_panel(cr, off, off, mw, mh, 10.0, 0.12, 0.13, 0.16, 0.88);

    double curr_y = 6.0;
    for (int r = 0; r < dock->menu_row_count; r++) {
        if (dock->menu_rows[r].is_separator) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
            cairo_rectangle(cr, off + 8.0, off + curr_y + 3.0, mw - 16.0, 1.0);
            cairo_fill(cr);
            curr_y += 7.0;
        } else {
            if (r == dock->menu_hover_row) {
                draw_rounded_rect(cr, off + 5.0, off + curr_y, mw - 10.0, 24.0, 6.0);
                cairo_set_source_rgba(cr, 0.18, 0.48, 0.94, 0.90);
                cairo_fill(cr);
            }
            draw_text(cr, dock->menu_rows[r].label, DOCK_FONT, 12.0, false,
                      1.0, 1.0, 1.0, 0.96, off + 14.0, off + curr_y + 12.0, false, true);
            curr_y += 26.0;
        }
    }

    cairo_buffer_apply_to_scene(dock->menu_buffer, dock->menu_scene_buffer);
}

bool dock_context_menu_is_open(aurade_dock_t *dock) {
    return dock && dock->menu_open;
}

void dock_close_context_menu(aurade_dock_t *dock) {
    if (!dock || !dock->menu_open) return;
    dock->menu_open = false;
    dock->menu_item_index = -1;
    dock->menu_hover_row = -1;
    if (dock->menu_scene_buffer) {
        wlr_scene_node_set_enabled(&dock->menu_scene_buffer->node, false);
    }
}

void dock_open_context_menu(aurade_dock_t *dock, int item_index) {
    if (!dock || item_index < 0 || item_index >= dock->item_count) return;

    dock_item_t *item = &dock->items[item_index];
    dock->menu_item_index = item_index;
    dock->menu_hover_row = -1;

    int rows = 0;
    if (strcmp(item->name, "Launchpad") == 0) {
        dock->menu_rows[rows].action = DOCK_MENU_ACT_LAUNCHPAD;
        snprintf(dock->menu_rows[rows].label, sizeof(dock->menu_rows[rows].label), "Open Launchpad");
        dock->menu_rows[rows].is_separator = false;
        rows++;
    } else {
        dock->menu_rows[rows].action = DOCK_MENU_ACT_NEW_WINDOW;
        snprintf(dock->menu_rows[rows].label, sizeof(dock->menu_rows[rows].label), "%s",
                 item->is_running ? "New Window" : "Open");
        dock->menu_rows[rows].is_separator = false;
        rows++;

        dock->menu_rows[rows].action = DOCK_MENU_ACT_NONE;
        dock->menu_rows[rows].label[0] = '\0';
        dock->menu_rows[rows].is_separator = true;
        rows++;

        if (item->is_pinned) {
            dock->menu_rows[rows].action = DOCK_MENU_ACT_UNPIN;
            snprintf(dock->menu_rows[rows].label, sizeof(dock->menu_rows[rows].label), "Unpin from Dock");
            dock->menu_rows[rows].is_separator = false;
            rows++;
        } else {
            dock->menu_rows[rows].action = DOCK_MENU_ACT_KEEP_IN_DOCK;
            snprintf(dock->menu_rows[rows].label, sizeof(dock->menu_rows[rows].label), "Keep in Dock");
            dock->menu_rows[rows].is_separator = false;
            rows++;
        }

        if (item->is_running) {
            dock->menu_rows[rows].action = DOCK_MENU_ACT_NONE;
            dock->menu_rows[rows].label[0] = '\0';
            dock->menu_rows[rows].is_separator = true;
            rows++;

            dock->menu_rows[rows].action = DOCK_MENU_ACT_QUIT;
            snprintf(dock->menu_rows[rows].label, sizeof(dock->menu_rows[rows].label), "Quit");
            dock->menu_rows[rows].is_separator = false;
            rows++;
        }
    }

    dock->menu_rows[rows].action = DOCK_MENU_ACT_NONE;
    dock->menu_rows[rows].label[0] = '\0';
    dock->menu_rows[rows].is_separator = true;
    rows++;

    dock->menu_rows[rows].action = DOCK_MENU_ACT_CYCLE_THEME;
    const char *tname = "Modern";
    if (dock_is_3d(dock)) tname = "3D Glass";
    else if (dock_is_plank(dock)) tname = "Plank";
    snprintf(dock->menu_rows[rows].label, sizeof(dock->menu_rows[rows].label), "Theme: %s", tname);
    dock->menu_rows[rows].is_separator = false;
    rows++;

    dock->menu_row_count = rows;

    double mw = 170.0;
    double total_h = 12.0; // pad_v * 2 (6.0 + 6.0)
    for (int r = 0; r < rows; r++) {
        total_h += dock->menu_rows[r].is_separator ? 7.0 : 26.0;
    }

    dock->menu_w = mw;
    dock->menu_h = total_h;

    double off = 10.0;
    cairo_buffer_resize(dock->menu_buffer, (int)ceil(mw + off * 2.0), (int)ceil(total_h + off * 2.0));

    double mx = item->center_x - mw / 2.0;
    if (mx < 12.0) mx = 12.0;
    if (mx + mw > dock->screen_width - 12.0) mx = dock->screen_width - 12.0 - mw;

    double pill_top = dock_pill_top(dock);
    double my = pill_top - total_h - 10.0;
    if (my < 35.0) my = 35.0;

    dock->menu_x = mx;
    dock->menu_y = my;

    wlr_scene_node_set_position(&dock->menu_scene_buffer->node, (int)(mx - off), (int)(my - off));
    wlr_scene_node_set_enabled(&dock->menu_scene_buffer->node, true);
    dock->menu_open = true;

    dock_render_context_menu(dock);
}

bool dock_handle_context_click(aurade_dock_t *dock, double x, double y) {
    if (!dock || !dock->enabled) return false;
    if (dock->hide_progress > 0.8 || (dock->hide_target && dock->hide_progress > 0.05)) return false;

    dock_handle_motion(dock, x, y);

    double icon_bottom = dock_icon_bottom(dock);
    for (int i = 0; i < dock->item_count; i++) {
        double half = dock->items[i].width / 2.0;
        if (half <= 0.0) continue;
        if (fabs(x - dock->items[i].center_x) <= half + 1.0 &&
            y <= icon_bottom + 4.0 &&
            y >= icon_bottom - dock->items[i].width - 2.0) {
            dock_open_context_menu(dock, i);
            return true;
        }
    }

    if (dock_pointer_owns(dock, x, y) && dock->item_count > 0) {
        dock_open_context_menu(dock, 0);
        return true;
    }

    if (dock->menu_open) {
        dock_close_context_menu(dock);
    }
    return false;
}

bool dock_handle_context_menu_click(aurade_dock_t *dock, double x, double y) {
    if (!dock || !dock->menu_open) return false;
    if (x < dock->menu_x || x > dock->menu_x + dock->menu_w ||
        y < dock->menu_y || y > dock->menu_y + dock->menu_h) {
        return false;
    }

    double local_y = y - dock->menu_y - 6.0;
    double curr_y = 0.0;
    int clicked_row = -1;
    for (int r = 0; r < dock->menu_row_count; r++) {
        double row_h = dock->menu_rows[r].is_separator ? 7.0 : 26.0;
        if (local_y >= curr_y && local_y < curr_y + row_h) {
            if (!dock->menu_rows[r].is_separator) {
                clicked_row = r;
            }
            break;
        }
        curr_y += row_h;
    }

    if (clicked_row < 0 || dock->menu_item_index < 0 || dock->menu_item_index >= dock->item_count) {
        dock_close_context_menu(dock);
        return true;
    }

    dock_menu_action_t act = dock->menu_rows[clicked_row].action;
    int idx = dock->menu_item_index;
    dock_item_t *item = &dock->items[idx];

    switch (act) {
        case DOCK_MENU_ACT_NEW_WINDOW:
            if (item->exec[0] != '\0') {
                app_launch_command(item->exec);
            }
            break;
        case DOCK_MENU_ACT_LAUNCHPAD:
            if (dock->server && dock->server->launchpad && dock->server->launchpad->active) {
                server_close_overlay(dock->server);
                break;
            }
            server_handle_gesture_begin(dock->server, ACTION_LAUNCHPAD);
            dock->server->gesture_progress = 0.001;
            dock->server->gesture_in_progress = false;
            dock->server->gesture_animating = true;
            dock->server->gesture_is_closing = false;
            spring_simulation_start(&dock->server->gesture_spring, &dock->server->config.spring,
                                    0.0, 1.0, 0.0);
            launchpad_update_progress(dock->server->launchpad, 0.001);
            schedule_all_outputs(dock->server);
            break;
        case DOCK_MENU_ACT_UNPIN: {
            if (item->is_running) {
                item->is_pinned = false;
            } else {
                if (item->cached_base_surface) {
                    cairo_surface_destroy(item->cached_base_surface);
                    item->cached_base_surface = NULL;
                }
                if (item->icon_surface) {
                    cairo_surface_destroy(item->icon_surface);
                    item->icon_surface = NULL;
                }
                for (int j = idx; j < dock->item_count - 1; j++) {
                    dock->items[j] = dock->items[j + 1];
                }
                dock->item_count--;
            }
            dock_save_pinned(dock);
            dock_update_running_state(dock);
            dock_render(dock);
            if (dock->server && dock->server->launchpad) {
                launchpad_refresh(dock->server->launchpad);
            }
            break;
        }
        case DOCK_MENU_ACT_KEEP_IN_DOCK:
            item->is_pinned = true;
            dock_save_pinned(dock);
            dock_update_running_state(dock);
            dock_render(dock);
            if (dock->server && dock->server->launchpad) {
                launchpad_refresh(dock->server->launchpad);
            }
            break;
        case DOCK_MENU_ACT_QUIT: {
            aurade_view_t *view, *tmp;
            wl_list_for_each_safe(view, tmp, &dock->server->views, link) {
                if (view_matches_dock_item(view, item)) {
                    view_close(view);
                }
            }
            dock_update_running_state(dock);
            dock_render(dock);
            break;
        }
        case DOCK_MENU_ACT_CYCLE_THEME: {
            if (dock->server) {
                if (dock_is_3d(dock)) {
                    snprintf(dock->server->config.dock_theme, sizeof(dock->server->config.dock_theme), "%s", DOCK_THEME_PLANK);
                } else if (dock_is_plank(dock)) {
                    snprintf(dock->server->config.dock_theme, sizeof(dock->server->config.dock_theme), "%s", DOCK_THEME_MODERN);
                } else {
                    snprintf(dock->server->config.dock_theme, sizeof(dock->server->config.dock_theme), "%s", DOCK_THEME_TABLE_3D);
                }
                config_save(&dock->server->config, NULL);
                dock_apply_slide(dock);
                dock_render(dock);
                if (dock->server) schedule_all_outputs(dock->server);
            }
            break;
        }
        default:
            break;
    }

    dock_close_context_menu(dock);
    return true;
}

void dock_handle_motion(aurade_dock_t *dock, double mouse_x, double mouse_y) {
    if (!dock) return;
    if (!dock->enabled) {
        dock->mouse_over_dock = false;
        return;
    }

    if (dock->menu_open) {
        int prev_hover = dock->menu_hover_row;
        int new_hover = -1;
        if (mouse_x >= dock->menu_x && mouse_x <= dock->menu_x + dock->menu_w &&
            mouse_y >= dock->menu_y && mouse_y <= dock->menu_y + dock->menu_h) {
            double local_y = mouse_y - dock->menu_y - 6.0;
            double curr_y = 0.0;
            for (int r = 0; r < dock->menu_row_count; r++) {
                double row_h = dock->menu_rows[r].is_separator ? 7.0 : 26.0;
                if (local_y >= curr_y && local_y < curr_y + row_h) {
                    if (!dock->menu_rows[r].is_separator) {
                        new_hover = r;
                    }
                    break;
                }
                curr_y += row_h;
            }
        }
        if (new_hover != prev_hover) {
            dock->menu_hover_row = new_hover;
            dock_render_context_menu(dock);
        }
    }

    if (dock->drag_candidate_index >= 0) {
        double dist = hypot(mouse_x - dock->drag_start_x, mouse_y - dock->drag_start_y);
        if (!dock->is_dragging && dist > 6.0) {
            dock->is_dragging = true;
            dock->drag_index = dock->drag_candidate_index;
        }
        if (dock->is_dragging && dock->drag_index >= 0 && dock->drag_index < dock->item_count) {
            dock->drag_current_x = mouse_x;
            int idx = dock->drag_index;
            bool pinned = dock->items[idx].is_pinned;

            if (pinned) {
                while (idx > 0 && dock->items[idx - 1].is_pinned && mouse_x < dock->items[idx - 1].center_x) {
                    dock_item_t tmp = dock->items[idx];
                    dock->items[idx] = dock->items[idx - 1];
                    dock->items[idx - 1] = tmp;
                    idx--;
                    dock->drag_index = idx;
                    dock->drag_candidate_index = idx;
                }
                while (idx < dock->item_count - 1 && dock->items[idx + 1].is_pinned && mouse_x > dock->items[idx + 1].center_x) {
                    dock_item_t tmp = dock->items[idx];
                    dock->items[idx] = dock->items[idx + 1];
                    dock->items[idx + 1] = tmp;
                    idx++;
                    dock->drag_index = idx;
                    dock->drag_candidate_index = idx;
                }
            } else {
                while (idx > 0 && !dock->items[idx - 1].is_pinned && mouse_x < dock->items[idx - 1].center_x) {
                    dock_item_t tmp = dock->items[idx];
                    dock->items[idx] = dock->items[idx - 1];
                    dock->items[idx - 1] = tmp;
                    idx--;
                    dock->drag_index = idx;
                    dock->drag_candidate_index = idx;
                }
                while (idx < dock->item_count - 1 && !dock->items[idx + 1].is_pinned && mouse_x > dock->items[idx + 1].center_x) {
                    dock_item_t tmp = dock->items[idx];
                    dock->items[idx] = dock->items[idx + 1];
                    dock->items[idx + 1] = tmp;
                    idx++;
                    dock->drag_index = idx;
                    dock->drag_candidate_index = idx;
                }
            }
        }
    }

    bool was_over = dock->mouse_over_dock;
    // Reaching the very bottom edge is the only trigger that pulls a hidden dock back up.
    bool near_edge = mouse_y >= dock->screen_height - DOCK_REVEAL_EDGE;

    // Only consider the pointer "over the dock" when the dock is open/revealing (!dock->hide_target),
    // and only include magnification headroom once the pointer is already inside the dock.
    double extra_top = was_over ? dock_headroom(dock) : 0.0;
    bool in_pill_bounds = (mouse_x >= dock->pill_x - 10.0 &&
                           mouse_x <= dock->pill_x + dock->pill_width + 10.0 &&
                           mouse_y >= dock->pill_y - extra_top &&
                           mouse_y <= (double)dock->screen_height);
    bool over = !dock->hide_target && in_pill_bounds;

    double dx = mouse_x - dock->mouse_x;
    double dy = mouse_y - dock->mouse_y;
    dock->mouse_x = mouse_x;
    dock->mouse_y = mouse_y;

    if (dock->server && dock->server->launchpad &&
        (dock->server->launchpad->active || dock->server->gesture_mode == GESTURE_MODE_LAUNCHPAD)) {
        dock_request_visible(dock, true);
    } else if (dock->autohide) {
        dock_request_visible(dock, near_edge || over);
    } else if (dock->intellihide) {
        bool overlaps = dock_intersects_window(dock);
        if (overlaps) {
            dock_request_visible(dock, near_edge || over);
        } else {
            dock_request_visible(dock, true);
        }
    }

    if (!dock->hide_target && in_pill_bounds) {
        over = true;
    }
    dock->mouse_over_dock = over;

    if (over || was_over || dock->is_dragging) {
        if (fabs(dx) > 0.35 || fabs(dy) > 0.35 || over != was_over || dock->is_dragging) {
            dock->needs_render = true;
            if (dock->server) schedule_all_outputs(dock->server);
        }
    }

    if (dock->server && dock->server->music_pill) {
        if (music_pill_handle_dock_motion(dock->server->music_pill, mouse_x, mouse_y)) {
            dock->needs_render = true;
            if (dock->server) schedule_all_outputs(dock->server);
        }
    }
}

static int cmp_views_by_id(const void *a, const void *b) {
    const aurade_view_t *va = *(const aurade_view_t **)a;
    const aurade_view_t *vb = *(const aurade_view_t **)b;
    if (va->id != vb->id) {
        return (va->id < vb->id) ? -1 : 1;
    }
    return (va < vb) ? -1 : (va > vb ? 1 : 0);
}

static void activate_item(aurade_dock_t *dock, dock_item_t *item) {
    if (strcmp(item->name, "Launchpad") == 0) {
        if (dock->server && dock->server->launchpad && dock->server->launchpad->active) {
            server_close_overlay(dock->server);
            return;
        }
        server_handle_gesture_begin(dock->server, ACTION_LAUNCHPAD);
        dock->server->gesture_progress = 0.001;
        dock->server->gesture_in_progress = false;
        dock->server->gesture_animating = true;
        dock->server->gesture_is_closing = false;
        spring_simulation_start(&dock->server->gesture_spring, &dock->server->config.spring,
                                0.0, 1.0, 0.0);
        launchpad_update_progress(dock->server->launchpad, 0.001);
        schedule_all_outputs(dock->server);
        return;
    }

    // Clicking a running app's icon focuses (or unminimises) it instead of
    // spawning a duplicate window. If multiple windows exist, cycle through all of them.
    aurade_view_t *views[24];
    int count = find_running_views(dock, item, views, 24);
    if (count > 0) {
        // Sort views into a stable order by ID so cycling visits every window sequentially.
        qsort(views, count, sizeof(aurade_view_t *), cmp_views_by_id);

        aurade_view_t *current_focused = dock->server->focused_view;
        int focused_idx = -1;
        for (int v = 0; v < count; v++) {
            if (views[v] == current_focused) {
                focused_idx = v;
                break;
            }
        }

        int target_idx = 0;
        if (focused_idx >= 0 && count > 1) {
            // Already focused on one of this app's windows! Cycle to the next window!
            target_idx = (focused_idx + 1) % count;
        } else {
            // If none of this app's windows are currently focused, pick the most recently
            // active one among them
            aurade_view_t *mru_view = NULL;
            aurade_view_t *v;
            wl_list_for_each(v, &dock->server->views, link) {
                for (int i = 0; i < count; i++) {
                    if (views[i] == v) {
                        mru_view = v;
                        break;
                    }
                }
                if (mru_view) break;
            }
            if (mru_view) {
                for (int i = 0; i < count; i++) {
                    if (views[i] == mru_view) {
                        target_idx = i;
                        break;
                    }
                }
            }
        }

        aurade_view_t *target = views[target_idx];
        if (target->workspace != dock->server->current_workspace) {
            server_switch_workspace(dock->server, target->workspace, true);
        }
        if (target->minimized) {
            view_restore(target);
        }
        view_focus(target);
        if (dock->server) schedule_all_outputs(dock->server);
        return;
    }

    if (item->exec[0] != '\0') {
        item->is_bouncing = true;
        item->bounce_offset = 0.0;
        item->bounce_vel = -420.0;
        item->bounce_count = 0;
        if (dock->server) schedule_all_outputs(dock->server);
        app_launch_command(item->exec);
    }
}

bool dock_handle_press(aurade_dock_t *dock, double x, double y) {
    if (!dock || !dock->enabled) return false;

    if (dock->menu_open) {
        if (dock_handle_context_menu_click(dock, x, y)) {
            return true;
        }
        dock_close_context_menu(dock);
    }

    if (dock->hide_progress > 0.8 || (dock->hide_target && dock->hide_progress > 0.05)) return false;

    dock_handle_motion(dock, x, y);
    if (!dock->enabled) return false;

    if (dock->server && dock->server->music_pill) {
        if (music_pill_handle_dock_click(dock->server->music_pill, x, y)) {
            dock_render(dock);
            return true;
        }
    }

    dock->drag_candidate_index = -1;
    dock->drag_index = -1;
    dock->is_dragging = false;

    double icon_bottom = dock_icon_bottom(dock);
    for (int i = 0; i < dock->item_count; i++) {
        double half = dock->items[i].width / 2.0;
        if (half <= 0.0) continue;
        if (fabs(x - dock->items[i].center_x) <= half + 1.0 &&
            y <= icon_bottom + 4.0 &&
            y >= icon_bottom - dock->items[i].width - 2.0) {
            dock->drag_candidate_index = i;
            dock->drag_start_x = x;
            dock->drag_start_y = y;
            dock->drag_current_x = x;
            return true;
        }
    }

    double pill_top = dock_pill_top(dock);
    if (x >= dock->pill_x && x <= dock->pill_x + dock->pill_width &&
        y >= pill_top && y <= pill_top + dock->pill_height) {
        return true;
    }
    return false;
}

bool dock_handle_release(aurade_dock_t *dock, double x, double y) {
    if (!dock || !dock->enabled) return false;

    if (dock->is_dragging) {
        dock->is_dragging = false;
        dock->drag_candidate_index = -1;
        dock->drag_index = -1;
        dock_save_pinned(dock);
        dock_render(dock);
        return true;
    }

    if (dock->hide_progress > 0.8 || (dock->hide_target && dock->hide_progress > 0.05)) return false;

    if (dock->drag_candidate_index >= 0) {
        int idx = dock->drag_candidate_index;
        dock->drag_candidate_index = -1;
        dock->drag_index = -1;
        dock->is_dragging = false;
        if (idx < dock->item_count) {
            activate_item(dock, &dock->items[idx]);
            dock_render(dock);
        }
        return true;
    }

    double pill_top = dock_pill_top(dock);
    if (x >= dock->pill_x && x <= dock->pill_x + dock->pill_width &&
        y >= pill_top && y <= pill_top + dock->pill_height) {
        return true;
    }
    return false;
}

bool dock_handle_click(aurade_dock_t *dock, double x, double y) {
    if (!dock || !dock->enabled) return false;
    if (dock->hide_progress > 0.8 || (dock->hide_target && dock->hide_progress > 0.05)) return false;
    if (dock_handle_press(dock, x, y)) {
        return dock_handle_release(dock, x, y);
    }
    return false;
}

void dock_destroy(aurade_dock_t *dock) {
    if (!dock) return;
    if (dock->anim_timer) {
        wl_event_source_remove(dock->anim_timer);
        dock->anim_timer = NULL;
    }
    for (int i = 0; i < dock->item_count; i++) {
        if (dock->items[i].cached_base_surface) {
            cairo_surface_destroy(dock->items[i].cached_base_surface);
            dock->items[i].cached_base_surface = NULL;
        }
        if (dock->items[i].icon_surface) {
            cairo_surface_destroy(dock->items[i].icon_surface);
            dock->items[i].icon_surface = NULL;
        }
    }
    if (dock->menu_buffer) {
        wlr_buffer_drop(&dock->menu_buffer->base);
        dock->menu_buffer = NULL;
    }
    if (dock->blur_node) {
        wlr_scene_node_destroy(&dock->blur_node->node);
        dock->blur_node = NULL;
    }
    if (dock->buffer) wlr_buffer_drop(&dock->buffer->base);
    free(dock);
}
