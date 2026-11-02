#include <stdio.h>
#include "dock.h"
#include "launchpad.h"
#include "music_pill.h"
#include "wallpaper.h"
#include "../util/apps.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#define DOCK_REVEAL_EDGE 3.0
#define DOCK_HIDE_DELAY_MS 320
#define DOCK_ANIM_TAU 0.055

// -------------------------------------------------------------
// Icon artwork
// -------------------------------------------------------------

typedef struct { double r, g, b; } rgb_t;

static rgb_t icon_color(const char *name) {
    if (strcmp(name, "Finder") == 0)     return (rgb_t){0.16, 0.52, 0.96};
    if (strcmp(name, "Launchpad") == 0)  return (rgb_t){0.42, 0.44, 0.50};
    if (strcmp(name, "Terminal") == 0)   return (rgb_t){0.11, 0.12, 0.16};
    if (strcmp(name, "Browser") == 0)    return (rgb_t){0.10, 0.45, 0.92};
    if (strcmp(name, "Settings") == 0)   return (rgb_t){0.52, 0.55, 0.60};

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
    double half = size / 2.0;
    double corner = size * 0.235;
    rgb_t c = icon_color(name);
    bool finder = (strcmp(name, "Finder") == 0);

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
    } else if (strcmp(name, "Launchpad") == 0) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.94);
        double dot_r = size * 0.045;
        double sp = size * 0.16;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                cairo_arc(cr, cx + dx * sp, cy + dy * sp, dot_r, 0, 2 * M_PI);
                cairo_fill(cr);
            }
        }
    } else if (strcmp(name, "Terminal") == 0) {
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
    } else if (strcmp(name, "Browser") == 0) {
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
    } else if (strcmp(name, "Settings") == 0) {
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
        char letter[2] = { name[0] ? name[0] : 'A', '\0' };
        draw_text(cr, letter, DOCK_FONT, size * 0.46, true,
                  1.0, 1.0, 1.0, 0.96, cx, cy - size * 0.02, true, true);
    }
}

static void draw_dock_item_icon(cairo_t *cr, double cx, double cy, double size, dock_item_t *item) {
    if (item->icon_surface) {
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

/* Distance the dock travels between "fully visible" and "fully hidden": just
 * enough for the pill to clear the bottom edge. */
static double dock_slide_distance(aurade_dock_t *dock) {
    if (!dock->autohide && !dock->intellihide) return 0.0;
    double slide = dock->pill_height + DOCK_BOTTOM_MARGIN;
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
    wlr_scene_node_set_enabled(&dock->scene_buffer->node,
                               dock->enabled && dock->hide_progress < 0.999);
}

static int dock_anim_tick(void *data) {
    aurade_dock_t *dock = data;
    uint64_t now = dock_now_ms();
    double dt = (double)(now - dock->last_tick_ms) / 1000.0;
    dock->last_tick_ms = now;
    if (dt < 0.001) dt = 0.001;
    if (dt > 0.05) dt = 0.05;

    if (dock->hide_target && dock->hide_at_ms) {
        if (now < dock->hide_at_ms) {
            int delay = (int)(dock->hide_at_ms - now);
            if (delay < 16) delay = 16;
            if (dock->anim_timer) wl_event_source_timer_update(dock->anim_timer, delay);
            return 0;
        }
        dock->hide_at_ms = 0;   // grace period over: start sliding away
    }

    double target = (dock->hide_target && dock->hide_at_ms == 0) ? 1.0 : 0.0;
    double k = 1.0 - exp(-dt / DOCK_ANIM_TAU);
    dock->hide_progress += (target - dock->hide_progress) * k;
    if (fabs(target - dock->hide_progress) < 0.003) dock->hide_progress = target;

    dock_apply_slide(dock);
    if (dock->server) {
        schedule_all_outputs(dock->server);
    }

    if (!dock->anim_timer) return 0;
    if (dock->hide_at_ms == 0 && fabs(target - dock->hide_progress) < 0.0005) {
        wl_event_source_timer_update(dock->anim_timer, 0);
        return 0;
    }
    wl_event_source_timer_update(dock->anim_timer, 16);
    return 0;
}

static void dock_arm_animation(aurade_dock_t *dock) {
    if (!dock || !dock->server || !dock->server->wl_display) return;
    if (!dock->anim_timer) {
        struct wl_event_loop *loop = wl_display_get_event_loop(dock->server->wl_display);
        dock->anim_timer = wl_event_loop_add_timer(loop, dock_anim_tick, dock);
    }
    dock->last_tick_ms = dock_now_ms();
    wl_event_source_timer_update(dock->anim_timer, 16);
}

static void dock_request_visible(aurade_dock_t *dock, bool visible) {
    if (!dock->autohide && !dock->intellihide) visible = true;

    if (visible) {
        bool was_away = dock->hide_target || dock->hide_at_ms || dock->hide_progress > 0.001;
        dock->hide_target = false;
        dock->hide_at_ms = 0;
        if (was_away) dock_arm_animation(dock);
    } else if (!dock->hide_target) {
        // Wait a moment before leaving, so moving along the dock does not
        // make it flicker away between icons.
        dock->hide_target = true;
        dock->hide_at_ms = dock_now_ms() + DOCK_HIDE_DELAY_MS;
        dock_arm_animation(dock);
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

    if (dock->server && dock->server->launchpad && dock->server->launchpad->active) {
        dock_request_visible(dock, true);
        return;
    }

    bool over = dock->mouse_over_dock;

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
    return true;
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
    dock->blur_dirty = true;

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

    dock->buffer = cairo_buffer_create(dock->screen_width, dock->buffer_height);
    dock->scene_buffer = wlr_scene_buffer_create(server->dock_tree ? server->dock_tree : server->shell_tree,
                                                 &dock->buffer->base);

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
    if (strcasestr(app_id, name)) return true;
    if (strcasecmp(name, "Steam") == 0 && strcasestr(app_id, "steam")) return true;
    if (strcmp(name, "Finder") == 0 &&
        (strcasestr(app_id, "nautilus") || strcasestr(app_id, "org.gnome.Nautilus") ||
         strcasestr(app_id, "thunar") || strcasestr(app_id, "dolphin"))) return true;
    if (strcmp(name, "Terminal") == 0 &&
        (strcasestr(app_id, "foot") || strcasestr(app_id, "kitty") ||
         strcasestr(app_id, "terminal") || strcasestr(app_id, "alacritty"))) return true;
    if (strcmp(name, "Browser") == 0 &&
        (strcasestr(app_id, "firefox") || strcasestr(app_id, "chrom") ||
         strcasestr(app_id, "epiphany") || strcasestr(app_id, "zen"))) return true;
    if (strcmp(name, "Settings") == 0 &&
        (strcasestr(app_id, "aurade") || strcasestr(app_id, "settings") ||
         strcasestr(app_id, "control-center"))) return true;
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

static aurade_view_t *find_running_view(aurade_dock_t *dock, const char *name) {
    aurade_view_t *view;
    wl_list_for_each(view, &dock->server->views, link) {
        const char *app_id = view_get_app_id(view);
        if (!app_id) continue;
        if (app_id_matches(app_id, name)) return view;
    }
    return NULL;
}

void dock_update_running_state(aurade_dock_t *dock) {
    if (!dock) return;

    int pinned_count = 0;
    for (int i = 0; i < dock->item_count; i++) {
        if (dock->items[i].is_pinned) pinned_count++;
    }

    // Drop dynamic entries and reset running state for pinned ones.
    dock->item_count = pinned_count;
    for (int i = 0; i < pinned_count; i++) {
        dock->items[i].is_running = false;
        dock->items[i].demands_attention = false;
    }

    aurade_view_t *view;
    wl_list_for_each(view, &dock->server->views, link) {
        const char *app_id = view_get_app_id(view);
        if (!app_id) continue;
        bool matched = false;

        for (int i = 0; i < pinned_count; i++) {
            if (app_id_matches(app_id, dock->items[i].name)) {
                dock->items[i].is_running = true;
                if (view->demands_attention) dock->items[i].demands_attention = true;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        // Already represented by another dynamic entry?
        for (int i = pinned_count; i < dock->item_count; i++) {
            if (strcasecmp(dock->items[i].name, app_id) == 0) {
                dock->items[i].is_running = true;
                if (view->demands_attention) dock->items[i].demands_attention = true;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        if (dock->item_count < 24) {
            dock_item_t *dyn = &dock->items[dock->item_count];
            memset(dyn, 0, sizeof(*dyn));
            snprintf(dyn->name, sizeof(dyn->name), "%.*s",
                     (int)sizeof(dyn->name) - 1, app_id);
            dyn->exec[0] = '\0';
            dyn->is_running = true;
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
    server->dock->blur_dirty = true;
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

    double pill_h = base + 18.0;
    double pill_x = (dock->screen_width - used) / 2.0;
    double pill_local_y = dock->buffer_height - pill_h - DOCK_BOTTOM_MARGIN;

    // Screen-space rect for input handling. The scene node is bottom-aligned.
    dock->pill_x = pill_x;
    dock->pill_y = dock->screen_height - pill_h - DOCK_BOTTOM_MARGIN;
    dock->pill_width = used;
    dock->pill_height = pill_h;

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

    // Panel shadow + frosted-glass rounded rectangle (not a full capsule).
    double pill_r = 22.0;
    if (pill_r > pill_h / 2.0) pill_r = pill_h / 2.0;
    draw_drop_shadow(cr, pill_x, pill_local_y, used, pill_h, pill_r, 18.0, 0.5);

    double dock_screen_y = dock->screen_height - dock->buffer_height;
    if (dock->server->config.blur_dock && dock->server->wallpaper) {
        if (!dock->blur_cache || dock->blur_dirty) {
            dock->blur_dirty = false;
            dock->last_blur_y = pill_local_y;
            dock->last_blur_h = pill_h;
            wallpaper_update_dock_blur(dock->server->wallpaper,
                                       &dock->blur_cache,
                                       dock->screen_width,
                                       dock_screen_y,
                                       dock->buffer_height);
        }

        if (dock->blur_cache) {
            int bw = cairo_image_surface_get_width(dock->blur_cache);
            int bh = cairo_image_surface_get_height(dock->blur_cache);

            cairo_save(cr);
            draw_rounded_rect(cr, pill_x, pill_local_y, used, pill_h, pill_r);
            cairo_clip(cr);

            cairo_scale(cr, (double)dock->screen_width / bw, (double)dock->buffer_height / bh);
            cairo_set_source_surface(cr, dock->blur_cache, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
            cairo_paint(cr);
            cairo_restore(cr);

            // Dark frosted glass tint overlay
            cairo_save(cr);
            draw_rounded_rect(cr, pill_x, pill_local_y, used, pill_h, pill_r);
            cairo_set_source_rgba(cr, 0.08, 0.09, 0.12, 0.55);
            cairo_fill(cr);
            cairo_restore(cr);

            // Subtle outer border & specular top highlight
            cairo_save(cr);
            draw_rounded_rect(cr, pill_x, pill_local_y, used, pill_h, pill_r);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);

            cairo_new_path(cr);
            cairo_move_to(cr, pill_x + pill_r, pill_local_y + 0.5);
            cairo_line_to(cr, pill_x + used - pill_r, pill_local_y + 0.5);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.32);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            cairo_restore(cr);
        } else {
            draw_glass_panel(cr, pill_x, pill_local_y, used, pill_h, pill_r,
                             0.10, 0.11, 0.14, 0.60);
        }
    } else {
        draw_glass_panel(cr, pill_x, pill_local_y, used, pill_h, pill_r,
                         0.10, 0.11, 0.14, 0.60);
    }

    // Separators drawn crisply over frosted glass
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
    if (left_sep_x >= 0.0) {
        cairo_rectangle(cr, left_sep_x, pill_local_y + 14.0, 1.0, pill_h - 28.0);
        cairo_fill(cr);
    }
    if (pinned_sep_x >= 0.0) {
        cairo_rectangle(cr, pinned_sep_x, pill_local_y + 14.0, 1.0, pill_h - 28.0);
        cairo_fill(cr);
    }
    if (right_sep_x >= 0.0) {
        cairo_rectangle(cr, right_sep_x, pill_local_y + 14.0, 1.0, pill_h - 28.0);
        cairo_fill(cr);
    }

    if (has_mp_dock && mp_draw_x >= 0.0) {
        double mp_h = pill_h - 12.0;
        double mp_y = pill_local_y + 6.0;
        dock->server->music_pill->dock_pill_x = mp_draw_x;
        dock->server->music_pill->dock_pill_y = dock->pill_y + 6.0;
        dock->server->music_pill->dock_pill_w = mp_w;
        dock->server->music_pill->dock_pill_h = mp_h;
        music_pill_render_dock(dock->server->music_pill, cr, mp_draw_x, mp_y, mp_w, mp_h);
    }

    dock->hovered_index = -1;
    double best_scale = 1.0;

    double icon_bottom = pill_local_y + pill_h - 10.0;
    int drag_draw_idx = -1;

    for (int i = 0; i < count; i++) {
        if (dock->is_dragging && i == dock->drag_index) {
            drag_draw_idx = i;
            continue;
        }
        double w = dock->items[i].width;
        double cx = dock->items[i].center_x;
        double cy = icon_bottom - w / 2.0 + dock->items[i].bounce_offset;

        draw_dock_item_icon(cr, cx, cy, w, &dock->items[i]);

        if (dock->items[i].demands_attention) {
            cairo_arc(cr, cx, pill_local_y + pill_h - 5.0, 3.0, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 0.45, 0.10, 0.98);
            cairo_fill(cr);
        } else if (dock->items[i].is_running) {
            cairo_arc(cr, cx, pill_local_y + pill_h - 5.0, 2.1, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, dock->items[i].is_pinned ? 1.0 : 0.75,
                                  1.0, 1.0, 0.86);
            cairo_fill(cr);
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

    // macOS-style app name tooltip above the hovered icon.
    if (!dock->is_dragging && dock->mouse_over_dock && dock->hovered_index >= 0) {
        draw_tooltip(dock, cr, dock->items[dock->hovered_index].name,
                     dock->items[dock->hovered_index].center_x, pill_local_y);
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
            break;
        }
        case DOCK_MENU_ACT_KEEP_IN_DOCK:
            item->is_pinned = true;
            dock_save_pinned(dock);
            dock_update_running_state(dock);
            dock_render(dock);
            break;
        case DOCK_MENU_ACT_QUIT: {
            char target_name[64];
            snprintf(target_name, sizeof(target_name), "%s", item->name);
            aurade_view_t *view, *tmp;
            wl_list_for_each_safe(view, tmp, &dock->server->views, link) {
                const char *app_id = view_get_app_id(view);
                if (app_id && app_id_matches(app_id, target_name)) {
                    view_close(view);
                }
            }
            dock_update_running_state(dock);
            dock_render(dock);
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

    double pill_top = dock_pill_top(dock);
    bool was_over = dock->mouse_over_dock;
    // When the dock is revealing or open (!dock->hide_target), test against the resting pill position
    // so moving towards icons while sliding up never cancels the reveal!
    double test_top = (!dock->hide_target) ? dock->pill_y : pill_top;
    bool over = (mouse_x >= dock->pill_x - 10.0 &&
                 mouse_x <= dock->pill_x + dock->pill_width + 10.0 &&
                 mouse_y >= test_top - dock_headroom(dock) &&
                 mouse_y <= (double)dock->screen_height);

    // Reaching for the bottom edge is what pulls a hidden dock back up, even
    // while the pointer is over a maximised window.
    bool near_edge = mouse_y >= dock->screen_height - DOCK_REVEAL_EDGE;

    double dx = mouse_x - dock->mouse_x;
    double dy = mouse_y - dock->mouse_y;
    dock->mouse_x = mouse_x;
    dock->mouse_y = mouse_y;
    dock->mouse_over_dock = over;

    if (dock->autohide) {
        dock_request_visible(dock, near_edge || over);
    } else if (dock->intellihide) {
        bool overlaps = dock_intersects_window(dock);
        if (overlaps) {
            dock_request_visible(dock, near_edge || over);
        } else {
            dock_request_visible(dock, true);
        }
    }

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
    // spawning a duplicate window.
    aurade_view_t *view = find_running_view(dock, item->name);
    if (view) {
        if (view->workspace != dock->server->current_workspace) {
            server_switch_workspace(dock->server, view->workspace, true);
        }
        if (view->minimized) view_restore(view);
        view_focus(view);
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
    if (dock->blur_cache) {
        cairo_surface_destroy(dock->blur_cache);
        dock->blur_cache = NULL;
    }
    if (dock->buffer) wlr_buffer_drop(&dock->buffer->base);
    free(dock);
}
