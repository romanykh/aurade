#include "topbar.h"
#include "control_center.h"
#include "music_pill.h"
#include "screenshot.h"
#include "wallpaper.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <glob.h>

#define FONT_UI "Inter"

// Apple menu geometry. The menu lives in its own scene buffer parented to the
// overlay tree so it paints above windows, the dock, and other shell chrome.
#define POWER_MENU_BUFFER_W 288
#define POWER_MENU_BUFFER_H 224
#define POWER_MENU_PANEL_X 10.0
#define POWER_MENU_PANEL_Y 10.0
#define POWER_MENU_PANEL_W 268.0
#define POWER_MENU_PANEL_H 196.0
#define POWER_MENU_SCENE_X 4
#define POWER_MENU_SCENE_Y TOPBAR_HEIGHT
#define POWER_MENU_ROW_H 32.0
#define POWER_MENU_ITEM_COUNT 5
#define POWER_BUTTON_Y 150.0
#define POWER_BUTTON_W 96.0
#define POWER_BUTTON_H 34.0

static const double power_row_y[POWER_MENU_ITEM_COUNT] = { 18.0, 59.0, 91.0, 123.0, 164.0 };
static const power_action_t power_row_action[POWER_MENU_ITEM_COUNT] = {
    POWER_ACTION_SETTINGS, POWER_ACTION_SLEEP, POWER_ACTION_RESTART,
    POWER_ACTION_SHUTDOWN, POWER_ACTION_LOGOUT
};
static const char *const power_row_label[POWER_MENU_ITEM_COUNT] = {
    "System Settings…", "Sleep", "Restart…", "Shut Down…", "Log Out…"
};

static void power_menu_render(aurade_topbar_t *bar);
static void power_menu_perform(aurade_topbar_t *bar, power_action_t action);

static bool read_battery(int *capacity, bool *charging) {
    static uint64_t last_check_ms = 0;
    static bool cached_found = false;
    static int cached_cap = 0;
    static bool cached_charging = false;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    if (last_check_ms > 0 && (now_ms - last_check_ms < 4000)) {
        if (!cached_found) return false;
        *capacity = cached_cap;
        *charging = cached_charging;
        return true;
    }
    last_check_ms = now_ms;

    glob_t g;
    if (glob("/sys/class/power_supply/BAT*", 0, NULL, &g) != 0) {
        cached_found = false;
        return false;
    }

    bool found = false;
    for (size_t i = 0; i < g.gl_pathc && !found; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/capacity", g.gl_pathv[i]);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int cap = -1;
        if (fscanf(f, "%d", &cap) == 1 && cap >= 0 && cap <= 100) {
            *capacity = cap;
            cached_cap = cap;
            found = true;
        }
        fclose(f);

        snprintf(path, sizeof(path), "%s/status", g.gl_pathv[i]);
        f = fopen(path, "r");
        if (f) {
            char status[32] = {0};
            if (fscanf(f, "%31s", status) == 1) {
                *charging = (strcmp(status, "Charging") == 0 ||
                             strcmp(status, "Full") == 0);
                cached_charging = *charging;
            }
            fclose(f);
        }
    }
    globfree(&g);
    cached_found = found;
    return found;
}

aurade_topbar_t *topbar_create(aurade_server_t *server) {
    aurade_topbar_t *bar = calloc(1, sizeof(*bar));
    bar->server = server;
    bar->width = server->screen_width > 0 ? server->screen_width : 1920;
    bar->height = TOPBAR_HEIGHT;
    strncpy(bar->active_app_title, "Finder", sizeof(bar->active_app_title) - 1);

    bar->buffer = cairo_buffer_create(bar->width, bar->height);
    bar->scene_buffer = wlr_scene_buffer_create(server->shell_tree, &bar->buffer->base);
    wlr_scene_node_set_position(&bar->scene_buffer->node, 0, 0);

    bar->power_menu_state = POWER_MENU_CLOSED;
    bar->power_menu_hover_row = -1;
    bar->power_menu_confirm_hover = 0;
    bar->power_menu_buffer = cairo_buffer_create(POWER_MENU_BUFFER_W, POWER_MENU_BUFFER_H);
    bar->power_menu_scene = wlr_scene_buffer_create(server->overlay_tree,
                                                    &bar->power_menu_buffer->base);
    wlr_scene_node_set_position(&bar->power_menu_scene->node,
                                POWER_MENU_SCENE_X, POWER_MENU_SCENE_Y);
    wlr_scene_node_set_enabled(&bar->power_menu_scene->node, false);
    power_menu_render(bar);

    bar->blur_dirty = true;
    topbar_render(bar);
    return bar;
}

void topbar_resize(aurade_topbar_t *bar, int width) {
    if (!bar || bar->width == width) return;
    bar->width = width;
    cairo_buffer_resize(bar->buffer, width, bar->height);
    topbar_render(bar);
}

void topbar_set_active_app(aurade_topbar_t *bar, const char *title) {
    if (!bar) return;
    const char *src = (title && *title) ? title : "Finder";
    strncpy(bar->active_app_title, src, sizeof(bar->active_app_title) - 1);
    bar->active_app_title[sizeof(bar->active_app_title) - 1] = '\0';
    topbar_render(bar);
}

void topbar_render(aurade_topbar_t *bar) {
    if (!bar || !bar->buffer || !bar->buffer->cr) return;
    bar->needs_render = false;
    cairo_t *cr = bar->buffer->cr;
    int w = bar->width;
    int h = bar->height;

    cairo_buffer_clear(bar->buffer);

    if (bar->server->config.blur_topbar && bar->server->wallpaper) {
        bool dims_changed = (bar->blur_cache == NULL ||
                             bar->last_blur_w != w ||
                             bar->last_blur_h != h);
        if (bar->blur_dirty || dims_changed) {
            bar->blur_dirty = false;
            bar->last_blur_w = w;
            bar->last_blur_h = h;
            if (bar->blur_cache) cairo_surface_destroy(bar->blur_cache);
            bar->blur_cache = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            cairo_t *bcr = cairo_create(bar->blur_cache);
            wallpaper_draw_realtime_blur_rect(bar->server->wallpaper, bcr,
                                              0.0, 0.0,
                                              0, 0, w, h, 0.0,
                                              0.10, 0.11, 0.14, 0.50);
            cairo_set_source_rgba(bcr, 1.0, 1.0, 1.0, 0.15);
            cairo_rectangle(bcr, 0, 0, w, 0.5);
            cairo_fill(bcr);
            cairo_set_source_rgba(bcr, 0.0, 0.0, 0.0, 0.28);
            cairo_rectangle(bcr, 0, h - 1.0, w, 1.0);
            cairo_fill(bcr);
            cairo_destroy(bcr);
        }
        cairo_save(cr);
        cairo_set_source_surface(cr, bar->blur_cache, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        // Translucent charcoal "vibrancy" bar with a soft top highlight.
        cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, h);
        cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.10, 0.10, 0.12, 0.66);
        cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.04, 0.04, 0.06, 0.74);
        cairo_set_source(cr, bg);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_fill(cr);
        cairo_pattern_destroy(bg);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        cairo_rectangle(cr, 0, 0, w, 0.5);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.28);
        cairo_rectangle(cr, 0, h - 1.0, w, 1.0);
        cairo_fill(cr);
    }

    // Apple menu glyph. Hover/open uses the same soft capsule as macOS.
    bar->apple_x = 4.0;
    bar->apple_y = 3.0;
    bar->apple_w = 28.0;
    bar->apple_h = h - 6.0;
    if (bar->hover_apple || bar->power_menu_state != POWER_MENU_CLOSED) {
        draw_rounded_rect(cr, bar->apple_x, bar->apple_y, bar->apple_w, bar->apple_h, 5.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16);
        cairo_fill(cr);
    }
    draw_sun_logo(cr, 18.0, h / 2.0 + 1.0, 16.0, 0.94);

    // Active application name
    double app_x = 36.0;
    draw_text(cr, bar->active_app_title, FONT_UI, 13.0, true,
              1.0, 1.0, 1.0, 0.96, app_x, h / 2.0, false, true);

    // Right-hand status area, laid out from the clock inwards.
    time_t now = time(NULL);
    static time_t last_clock_sec = 0;
    static char cached_clock_str[64] = {0};
    static double cached_clock_w = 0.0;
    if (now != last_clock_sec || cached_clock_w <= 0.0) {
        last_clock_sec = now;
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(cached_clock_str, sizeof(cached_clock_str), "%a %b %-d  %-I:%M %p", &tm);
        cached_clock_w = text_width(cr, cached_clock_str, FONT_UI, 12.5, true);
    }

    double clock_x = w - 14.0 - cached_clock_w;
    draw_text(cr, cached_clock_str, FONT_UI, 12.5, true,
              1.0, 1.0, 1.0, 0.92, clock_x, h / 2.0, false, true);

    double cursor_x = clock_x - 14.0;

    int capacity = 0;
    bool charging = false;
    if (read_battery(&capacity, &charging)) {
        double bw = 23.0, bh = 11.0;
        cursor_x -= bw;
        draw_battery_icon(cr, cursor_x, (h - bh) / 2.0, bw, bh,
                          capacity / 100.0, 0.92, charging);
        cursor_x -= 10.0;
    }

    // Control Center pill
    double cc_w = 30.0, cc_h = 17.0;
    cursor_x -= cc_w;
    bar->cc_x = cursor_x;
    bar->cc_y = (h - cc_h) / 2.0;
    bar->cc_w = cc_w;
    bar->cc_h = cc_h;

    draw_rounded_rect(cr, cursor_x, bar->cc_y, cc_w, cc_h, cc_h / 2.0);
    bool cc_open = bar->server && control_center_is_open(bar->server->control_center);
    if (bar->hover_cc || cc_open) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    }
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    draw_icon_tint(1.0, 1.0, 1.0);
    draw_control_center_icon(cr, cursor_x + cc_w / 2.0, h / 2.0, 15.0,
                             (bar->hover_cc || cc_open) ? 1.0 : 0.88);

    cursor_x -= 8.0;

    // Screen recording status pill
    bool is_rec = bar->server && screenshot_is_recording(bar->server->screenshot);
    if (is_rec) {
        uint32_t dur = screenshot_get_recording_duration_sec(bar->server->screenshot);
        uint32_t mins = dur / 60;
        uint32_t secs = dur % 60;
        char dur_str[32];
        snprintf(dur_str, sizeof(dur_str), "%02u:%02u", mins, secs);

        double dur_w = text_width(cr, dur_str, FONT_UI, 11.5, true);
        double rec_w = 8.0 + 8.0 + 5.0 + dur_w + 6.0 + 8.0 + 8.0;
        double rec_h = 20.0;
        cursor_x -= rec_w;
        bar->rec_x = cursor_x;
        bar->rec_y = (h - rec_h) / 2.0;
        bar->rec_w = rec_w;
        bar->rec_h = rec_h;

        // Pill container
        draw_rounded_rect(cr, bar->rec_x, bar->rec_y, bar->rec_w, bar->rec_h, rec_h / 2.0);
        if (bar->hover_rec) {
            cairo_set_source_rgba(cr, 0.90, 0.15, 0.15, 0.38);
        } else {
            cairo_set_source_rgba(cr, 0.85, 0.12, 0.12, 0.22);
        }
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 0.25, 0.25, 0.55);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Pulsing red recording dot
        struct timespec tms;
        clock_gettime(CLOCK_MONOTONIC, &tms);
        double t_sec = tms.tv_sec + tms.tv_nsec / 1e9;
        double pulse = 0.65 + 0.35 * sin(t_sec * 4.0);

        double dot_cx = bar->rec_x + 8.0 + 4.0;
        double dot_cy = h / 2.0;
        cairo_arc(cr, dot_cx, dot_cy, 4.0, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 0.20, 0.20, pulse);
        cairo_fill(cr);

        // Duration string
        double text_x = dot_cx + 4.0 + 5.0;
        draw_text(cr, dur_str, FONT_UI, 11.5, true,
                  1.0, 1.0, 1.0, 0.95, text_x, h / 2.0, false, true);

        // Stop icon
        double stop_x = text_x + dur_w + 6.0;
        double stop_y = (h - 8.0) / 2.0;
        draw_rounded_rect(cr, stop_x, stop_y, 8.0, 8.0, 2.0);
        cairo_set_source_rgba(cr, 1.0, 0.35, 0.35, 0.95);
        cairo_fill(cr);
    } else {
        bar->rec_x = 0;
        bar->rec_y = 0;
        bar->rec_w = 0;
        bar->rec_h = 0;
        bar->hover_rec = false;
    }

    // Dynamic Island / Music Pill in topbar center
    if (bar->server->music_pill &&
        music_pill_get_pos(bar->server->music_pill) == MUSIC_PILL_POS_TOPBAR_CENTER) {
        music_pill_render_topbar(bar->server->music_pill, cr, w, h);
    }

    // Window focus accent: hairline at the top of the bar echoes macOS's
    // active-app cue without stealing attention.
    if (bar->server->focused_view) {
        cairo_pattern_t *accent = cairo_pattern_create_linear(0, 0, w, 0);
        cairo_pattern_add_color_stop_rgba(accent, 0.0, 0.0, 0.48, 1.0, 0.0);
        cairo_pattern_add_color_stop_rgba(accent, 0.5, 0.25, 0.62, 1.0, 0.22);
        cairo_pattern_add_color_stop_rgba(accent, 1.0, 0.0, 0.48, 1.0, 0.0);
        cairo_set_source(cr, accent);
        cairo_rectangle(cr, 0, h - 1.0, w, 1.0);
        cairo_fill(cr);
        cairo_pattern_destroy(accent);
    }

    cairo_buffer_apply_to_scene(bar->buffer, bar->scene_buffer);
}

// -------------------------------------------------------------
// Apple / power menu
// -------------------------------------------------------------

static int power_menu_row_at(double x, double y) {
    double mx = x - POWER_MENU_SCENE_X;
    double my = y - POWER_MENU_SCENE_Y;
    if (mx < POWER_MENU_PANEL_X || mx > POWER_MENU_PANEL_X + POWER_MENU_PANEL_W ||
        my < POWER_MENU_PANEL_Y || my > POWER_MENU_PANEL_Y + POWER_MENU_PANEL_H) {
        return -1;
    }
    for (int i = 0; i < POWER_MENU_ITEM_COUNT; i++) {
        if (my >= power_row_y[i] && my < power_row_y[i] + POWER_MENU_ROW_H) {
            return i;
        }
    }
    return -1;
}

static int power_menu_confirm_button_at(double x, double y) {
    double mx = x - POWER_MENU_SCENE_X;
    double my = y - POWER_MENU_SCENE_Y;
    if (my < POWER_BUTTON_Y || my > POWER_BUTTON_Y + POWER_BUTTON_H) return 0;
    if (mx >= 30.0 && mx <= 30.0 + POWER_BUTTON_W) return 1;
    if (mx >= 162.0 && mx <= 162.0 + POWER_BUTTON_W) return 2;
    return 0;
}

static const char *power_confirm_title(power_action_t action) {
    switch (action) {
        case POWER_ACTION_RESTART:  return "Restart this computer?";
        case POWER_ACTION_SHUTDOWN: return "Shut down this computer?";
        case POWER_ACTION_LOGOUT:   return "Log out of AuraDE?";
        default:                    return "Are you sure?";
    }
}

static const char *power_confirm_subtitle(power_action_t action) {
    switch (action) {
        case POWER_ACTION_RESTART:
        case POWER_ACTION_SHUTDOWN: return "Unsaved work will be lost.";
        case POWER_ACTION_LOGOUT:   return "Your session will end.";
        default:                    return "";
    }
}

static const char *power_confirm_label(power_action_t action) {
    switch (action) {
        case POWER_ACTION_RESTART:  return "Restart";
        case POWER_ACTION_SHUTDOWN: return "Shut Down";
        case POWER_ACTION_LOGOUT:   return "Log Out";
        default:                    return "OK";
    }
}

static void draw_power_button(cairo_t *cr, double x, double y, double w, double h,
                              const char *label, bool primary, bool hover) {
    draw_rounded_rect(cr, x, y, w, h, 8.0);
    if (primary) {
        cairo_set_source_rgba(cr, hover ? 0.94 : 0.80, 0.20, 0.18, 0.96);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, hover ? 0.24 : 0.12);
    }
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    draw_text(cr, label, FONT_UI, 12.5, true, 1.0, 1.0, 1.0, 0.96,
              x + w / 2.0, y + h / 2.0, true, true);
}

static void power_menu_render(aurade_topbar_t *bar) {
    if (!bar || !bar->power_menu_buffer || !bar->power_menu_buffer->cr) return;
    cairo_t *cr = bar->power_menu_buffer->cr;

    cairo_buffer_clear(bar->power_menu_buffer);
    if (bar->power_menu_state == POWER_MENU_CLOSED) {
        cairo_buffer_apply_to_scene(bar->power_menu_buffer, bar->power_menu_scene);
        return;
    }

    draw_drop_shadow(cr, POWER_MENU_PANEL_X, POWER_MENU_PANEL_Y,
                     POWER_MENU_PANEL_W, POWER_MENU_PANEL_H, 12.0, 10.0, 0.55);
    draw_glass_panel(cr, POWER_MENU_PANEL_X, POWER_MENU_PANEL_Y,
                     POWER_MENU_PANEL_W, POWER_MENU_PANEL_H, 12.0,
                     0.09, 0.09, 0.11, 0.95);

    if (bar->power_menu_state == POWER_MENU_OPEN) {
        for (int i = 0; i < POWER_MENU_ITEM_COUNT; i++) {
            double ry = power_row_y[i];

            if (i == bar->power_menu_hover_row) {
                draw_rounded_rect(cr, POWER_MENU_PANEL_X + 6.0, ry + 1.0,
                                  POWER_MENU_PANEL_W - 12.0, POWER_MENU_ROW_H - 2.0, 6.0);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.13);
                cairo_fill(cr);
            }

            draw_text(cr, power_row_label[i], FONT_UI, 13.0, false,
                      1.0, 1.0, 1.0, 0.94,
                      POWER_MENU_PANEL_X + 16.0, ry + POWER_MENU_ROW_H / 2.0,
                      false, true);
        }

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
        cairo_rectangle(cr, POWER_MENU_PANEL_X + 8.0, 50.0,
                        POWER_MENU_PANEL_W - 16.0, 1.0);
        cairo_fill(cr);
        cairo_rectangle(cr, POWER_MENU_PANEL_X + 8.0, 155.0,
                        POWER_MENU_PANEL_W - 16.0, 1.0);
        cairo_fill(cr);
    } else {
        power_action_t action = bar->power_menu_confirm;

        draw_text(cr, power_confirm_title(action), FONT_UI, 14.0, true,
                  1.0, 1.0, 1.0, 0.96,
                  POWER_MENU_PANEL_X + POWER_MENU_PANEL_W / 2.0, 44.0,
                  true, true);
        draw_text(cr, power_confirm_subtitle(action), FONT_UI, 11.5, false,
                  1.0, 1.0, 1.0, 0.62,
                  POWER_MENU_PANEL_X + POWER_MENU_PANEL_W / 2.0, 72.0,
                  true, true);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        cairo_rectangle(cr, POWER_MENU_PANEL_X + 8.0, 116.0,
                        POWER_MENU_PANEL_W - 16.0, 1.0);
        cairo_fill(cr);

        draw_power_button(cr, 30.0, POWER_BUTTON_Y, POWER_BUTTON_W, POWER_BUTTON_H,
                          "Cancel", false, bar->power_menu_confirm_hover == 1);
        draw_power_button(cr, 162.0, POWER_BUTTON_Y, POWER_BUTTON_W, POWER_BUTTON_H,
                          power_confirm_label(action), true,
                          bar->power_menu_confirm_hover == 2);
    }

    cairo_buffer_apply_to_scene(bar->power_menu_buffer, bar->power_menu_scene);
}

static void power_menu_perform(aurade_topbar_t *bar, power_action_t action) {
    switch (action) {
        case POWER_ACTION_SETTINGS:
            app_launch_command("python3 /home/user/AI/de/settings/aurade_settings.py "
                               ">/dev/null 2>&1 &");
            break;
        case POWER_ACTION_SLEEP:
            app_launch_command("systemctl suspend");
            break;
        case POWER_ACTION_RESTART:
            app_launch_command("systemctl reboot");
            break;
        case POWER_ACTION_SHUTDOWN:
            app_launch_command("systemctl poweroff");
            break;
        case POWER_ACTION_LOGOUT: {
            const char *sid = getenv("XDG_SESSION_ID");
            if (sid && *sid) {
                char cmd[96];
                snprintf(cmd, sizeof(cmd), "loginctl terminate-session %s", sid);
                app_launch_command(cmd);
            } else if (bar->server && bar->server->wl_display) {
                // No logind session id (e.g. nested run): exit the compositor.
                wl_display_terminate(bar->server->wl_display);
            }
            break;
        }
        default:
            break;
    }
}

static void topbar_open_power_menu(aurade_topbar_t *bar) {
    bar->power_menu_state = POWER_MENU_OPEN;
    bar->power_menu_confirm = POWER_ACTION_NONE;
    bar->power_menu_hover_row = -1;
    bar->power_menu_confirm_hover = 0;
    bar->hover_apple = true;

    wlr_scene_node_set_enabled(&bar->power_menu_scene->node, true);
    wlr_scene_node_raise_to_top(&bar->power_menu_scene->node);
    power_menu_render(bar);
    topbar_render(bar);
}

bool topbar_power_menu_is_open(aurade_topbar_t *bar) {
    return bar && bar->power_menu_state != POWER_MENU_CLOSED;
}

void topbar_close_power_menu(aurade_topbar_t *bar) {
    if (!bar || bar->power_menu_state == POWER_MENU_CLOSED) return;

    bar->power_menu_state = POWER_MENU_CLOSED;
    bar->power_menu_confirm = POWER_ACTION_NONE;
    bar->power_menu_hover_row = -1;
    bar->power_menu_confirm_hover = 0;
    bar->hover_apple = false;

    if (bar->power_menu_scene) {
        wlr_scene_node_set_enabled(&bar->power_menu_scene->node, false);
    }
    topbar_render(bar);
}

bool topbar_handle_power_menu_click(aurade_topbar_t *bar, double x, double y) {
    if (!bar || bar->power_menu_state == POWER_MENU_CLOSED) return false;

    if (bar->power_menu_state == POWER_MENU_CONFIRM) {
        int button = power_menu_confirm_button_at(x, y);
        if (button == 1) {
            // Back to the menu instead of closing everything.
            bar->power_menu_state = POWER_MENU_OPEN;
            bar->power_menu_confirm = POWER_ACTION_NONE;
            bar->power_menu_confirm_hover = 0;
            bar->power_menu_hover_row = -1;
            power_menu_render(bar);
        } else if (button == 2) {
            power_action_t action = bar->power_menu_confirm;
            topbar_close_power_menu(bar);
            power_menu_perform(bar, action);
        } else {
            topbar_close_power_menu(bar);
        }
        return true;
    }

    int row = power_menu_row_at(x, y);
    if (row < 0) {
        topbar_close_power_menu(bar);
        return true;
    }

    power_action_t action = power_row_action[row];
    if (action == POWER_ACTION_RESTART || action == POWER_ACTION_SHUTDOWN ||
        action == POWER_ACTION_LOGOUT) {
        bar->power_menu_state = POWER_MENU_CONFIRM;
        bar->power_menu_confirm = action;
        bar->power_menu_confirm_hover = 0;
        power_menu_render(bar);
    } else {
        topbar_close_power_menu(bar);
        power_menu_perform(bar, action);
    }
    return true;
}

bool topbar_handle_motion(aurade_topbar_t *bar, double x, double y) {
    if (!bar) return false;

    if (bar->power_menu_state != POWER_MENU_CLOSED) {
        bool changed = false;
        if (bar->power_menu_state == POWER_MENU_OPEN) {
            int row = power_menu_row_at(x, y);
            if (row != bar->power_menu_hover_row) {
                bar->power_menu_hover_row = row;
                changed = true;
            }
        } else {
            int button = power_menu_confirm_button_at(x, y);
            if (button != bar->power_menu_confirm_hover) {
                bar->power_menu_confirm_hover = button;
                changed = true;
            }
        }
        if (changed) power_menu_render(bar);
    }

    if (y > bar->height + 2.0) {
        if (bar->hover_cc || bar->hover_apple || bar->hover_rec) {
            bar->hover_cc = false;
            bar->hover_apple = false;
            bar->hover_rec = false;
            topbar_render(bar);
        }
        return false;
    }

    bool cc = (x >= bar->cc_x && x <= bar->cc_x + bar->cc_w &&
               y >= bar->cc_y && y <= bar->cc_y + bar->cc_h);
    bool apple = (x >= bar->apple_x && x <= bar->apple_x + bar->apple_w &&
                  y >= bar->apple_y && y <= bar->apple_y + bar->apple_h);
    bool rec = (bar->rec_w > 0 &&
                x >= bar->rec_x && x <= bar->rec_x + bar->rec_w &&
                y >= bar->rec_y && y <= bar->rec_y + bar->rec_h);

    if (cc != bar->hover_cc || apple != bar->hover_apple || rec != bar->hover_rec) {
        bar->hover_cc = cc;
        bar->hover_apple = apple;
        bar->hover_rec = rec;
        topbar_render(bar);
        return true;
    }

    if (bar->server->music_pill &&
        music_pill_get_pos(bar->server->music_pill) == MUSIC_PILL_POS_TOPBAR_CENTER) {
        if (music_pill_handle_topbar_motion(bar->server->music_pill, x, y)) {
            topbar_render(bar);
            return true;
        }
    }

    return false;
}

bool topbar_handle_click(aurade_topbar_t *bar, double x, double y) {
    if (!bar || y > bar->height) return false;

    // Music pill in topbar center
    if (bar->server->music_pill &&
        music_pill_get_pos(bar->server->music_pill) == MUSIC_PILL_POS_TOPBAR_CENTER) {
        if (music_pill_handle_topbar_click(bar->server->music_pill, x, y)) {
            topbar_render(bar);
            return true;
        }
    }

    // Screen recording pill click -> Stop recording
    if (bar->rec_w > 0 &&
        x >= bar->rec_x && x <= bar->rec_x + bar->rec_w &&
        y >= bar->rec_y && y <= bar->rec_y + bar->rec_h) {
        if (bar->server && bar->server->screenshot) {
            screenshot_stop_recording(bar->server->screenshot);
        }
        return true;
    }

    // Apple logo toggles the system power menu.
    if (x >= bar->apple_x && x <= bar->apple_x + bar->apple_w &&
        y >= bar->apple_y && y <= bar->apple_y + bar->apple_h) {
        if (bar->power_menu_state == POWER_MENU_CLOSED) {
            topbar_open_power_menu(bar);
        } else {
            topbar_close_power_menu(bar);
        }
        return true;
    }

    // Control Center
    if (x >= bar->cc_x && x <= bar->cc_x + bar->cc_w &&
        y >= bar->cc_y && y <= bar->cc_y + bar->cc_h) {
        // Start a short way in and let the spring finish the drop-in, so the
        // panel scales and fades in instead of appearing fully formed.
        server_handle_gesture_begin(bar->server, ACTION_CONTROL_CENTER);
        server_handle_gesture_update(bar->server, -90.0, 0, 1.0);
        server_handle_gesture_end(bar->server, false, -500.0, 0);
        return true;
    }

    return true; // The menu bar itself never forwards clicks to windows.
}

void topbar_destroy(aurade_topbar_t *bar) {
    if (!bar) return;
    if (bar->blur_cache) {
        cairo_surface_destroy(bar->blur_cache);
        bar->blur_cache = NULL;
    }
    if (bar->power_menu_buffer) wlr_buffer_drop(&bar->power_menu_buffer->base);
    if (bar->buffer) wlr_buffer_drop(&bar->buffer->base);
    free(bar);
}
