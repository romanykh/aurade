/*
 * AuraDE Control Center — a macOS-style modular drawer.
 *
 * Layout follows the modular grid Apple introduced with Big Sur: a 2×2
 * connectivity group and a Focus tile on the first row, full-width Display and
 * Sound sliders, a dynamic Now Playing tile, and three quiet shortcuts at the
 * bottom. The material adapts to the system appearance (light / dark glass),
 * every interactive region has hover and press feedback, and the drawer scales
 * and fades with the gesture progress instead of sliding as a rigid rectangle.
 */
#include "control_center.h"
#include "topbar.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <json-c/json.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <wayland-server-core.h>
#include <sys/eventfd.h>
#include <dirent.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

#define CC_WIDTH 340.0
#define CC_PAD 14.0
#define CC_RADIUS 24.0
#define CC_GAP 8.0
#define CC_SHADOW_PAD 24.0

#define CC_CONTENT_W (CC_WIDTH - 2.0 * CC_PAD)
#define CC_COL_W ((CC_CONTENT_W - CC_GAP) / 2.0)
#define CC_CARD_RADIUS 16.0
#define CC_FOOT_RADIUS 13.0

// Gap between the menu bar and the top of the panel, and from the screen edge.
#define CC_MARGIN_TOP 8.0
#define CC_MARGIN_RIGHT 10.0

// Maximum height allocated for the scene buffer to accommodate subpages
#define CC_MAX_PANEL_H 540.0

#define CC_FONT "Inter, Adwaita Sans, sans-serif"

#define CC_NP_INTERVAL 2000

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

typedef struct {
    double r, g, b, a;
} cc_color_t;

typedef struct {
    cc_color_t panel_top, panel_bottom, border, highlight;
    cc_color_t card, card_hover, card_pressed, card_border;
    cc_color_t overlay;
    cc_color_t text, text_dim, text_faint, divider;
    cc_color_t track;
    cc_color_t fill, fill_text, fill_muted, badge_off, badge_off_glyph;
    cc_color_t accent, accent_blue, focus_top, focus_bottom;
    bool dark;
    double shadow;
} cc_theme_t;

typedef enum {
    CC_ICON_WIFI,
    CC_ICON_BLUETOOTH,
    CC_ICON_HOTSPOT,
    CC_ICON_MOON,
    CC_ICON_DND,
    CC_ICON_SUN,
    CC_ICON_SPEAKER,
    CC_ICON_AURA_CONNECT,
    CC_ICON_SCREEN_KEYBOARD,
    CC_ICON_DISPLAY_BADGE,
    CC_ICON_AIRPLAY_AUDIO,
    CC_ICON_USER_PROFILE,
    CC_ICON_ACCESSIBILITY,
    CC_ICON_GEAR,
    CC_ICON_SLIDERS,
    CC_ICON_NOTE,
    CC_ICON_PLAY,
    CC_ICON_PAUSE,
    CC_ICON_PREV,
    CC_ICON_NEXT,
    CC_ICON_AIRDROP = CC_ICON_HOTSPOT,
    CC_ICON_KBD_BRIGHTNESS = CC_ICON_AURA_CONNECT,
    CC_ICON_SCREEN_MIRROR = CC_ICON_SCREEN_KEYBOARD,
} cc_icon_kind_t;

static cc_color_t cc_rgba(double r, double g, double b, double a) {
    cc_color_t c = {r, g, b, a};
    return c;
}

static cc_color_t cc_fade(cc_color_t c, double k) {
    c.a *= k;
    return c;
}

static void cc_set(cairo_t *cr, cc_color_t c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

/* "#RRGGBB" (or "#RGB") -> components in [0,1]. */
static bool cc_hex_color(const char *text, double *r, double *g, double *b) {
    if (!text || text[0] != '#') return false;
    unsigned int v = 0;
    size_t len = strlen(text);
    if (len == 7) {
        if (sscanf(text + 1, "%6x", &v) != 1) return false;
        *r = ((v >> 16) & 0xFF) / 255.0;
        *g = ((v >> 8) & 0xFF) / 255.0;
        *b = (v & 0xFF) / 255.0;
        return true;
    }
    if (len == 4) {
        if (sscanf(text + 1, "%3x", &v) != 1) return false;
        *r = (((v >> 8) & 0xF) * 17) / 255.0;
        *g = (((v >> 4) & 0xF) * 17) / 255.0;
        *b = ((v & 0xF) * 17) / 255.0;
        return true;
    }
    return false;
}

static cc_theme_t cc_theme(bool dark, const char *accent) {
    cc_theme_t t = {0};
    double ar = 0.04, ag = 0.52, ab = 1.0;
    cc_hex_color(accent, &ar, &ag, &ab);
    t.accent_blue = cc_rgba(0.0, 0.48, 1.0, 1.0);

    if (dark) {
        t.panel_top = cc_rgba(0.18, 0.20, 0.24, 0.88);
        t.panel_bottom = cc_rgba(0.11, 0.12, 0.15, 0.92);
        t.border = cc_rgba(1.0, 1.0, 1.0, 0.15);
        t.highlight = cc_rgba(1.0, 1.0, 1.0, 0.20);
        t.card = cc_rgba(1.0, 1.0, 1.0, 0.085);
        t.card_hover = cc_rgba(1.0, 1.0, 1.0, 0.14);
        t.card_pressed = cc_rgba(1.0, 1.0, 1.0, 0.055);
        t.card_border = cc_rgba(1.0, 1.0, 1.0, 0.07);
        t.overlay = cc_rgba(1.0, 1.0, 1.0, 0.07);
        t.text = cc_rgba(1.0, 1.0, 1.0, 0.96);
        t.text_dim = cc_rgba(0.85, 0.86, 0.90, 0.65);
        t.text_faint = cc_rgba(0.80, 0.82, 0.88, 0.40);
        t.divider = cc_rgba(1.0, 1.0, 1.0, 0.09);
        t.track = cc_rgba(0.0, 0.0, 0.0, 0.35);
        t.fill = cc_rgba(1.0, 1.0, 1.0, 1.0);
        t.fill_text = cc_rgba(0.10, 0.10, 0.13, 0.90);
        t.fill_muted = cc_rgba(0.82, 0.83, 0.88, 0.30);
        t.badge_off = cc_rgba(1.0, 1.0, 1.0, 0.12);
        t.badge_off_glyph = cc_rgba(1.0, 1.0, 1.0, 0.88);
        t.accent = cc_rgba(ar, ag, ab, 0.96);
        t.focus_top = cc_rgba(0.45, 0.38, 0.95, 0.96);
        t.focus_bottom = cc_rgba(0.33, 0.29, 0.82, 0.96);
        t.dark = true;
        t.shadow = 0.55;
    } else {
        t.panel_top = cc_rgba(0.93, 0.94, 0.97, 0.88);
        t.panel_bottom = cc_rgba(0.89, 0.91, 0.95, 0.92);
        t.border = cc_rgba(1.0, 1.0, 1.0, 0.80);
        t.highlight = cc_rgba(1.0, 1.0, 1.0, 0.95);
        t.card = cc_rgba(1.0, 1.0, 1.0, 0.60);
        t.card_hover = cc_rgba(1.0, 1.0, 1.0, 0.78);
        t.card_pressed = cc_rgba(1.0, 1.0, 1.0, 0.45);
        t.card_border = cc_rgba(0.0, 0.0, 0.0, 0.06);
        t.overlay = cc_rgba(0.0, 0.0, 0.0, 0.05);
        t.text = cc_rgba(0.08, 0.09, 0.12, 0.92);
        t.text_dim = cc_rgba(0.24, 0.26, 0.32, 0.65);
        t.text_faint = cc_rgba(0.30, 0.32, 0.38, 0.40);
        t.divider = cc_rgba(0.0, 0.0, 0.0, 0.07);
        t.track = cc_rgba(0.0, 0.0, 0.0, 0.08);
        t.fill = cc_rgba(1.0, 1.0, 1.0, 1.0);
        t.fill_text = cc_rgba(0.09, 0.09, 0.12, 0.90);
        t.fill_muted = cc_rgba(0.35, 0.37, 0.42, 0.22);
        t.badge_off = cc_rgba(0.0, 0.0, 0.0, 0.08);
        t.badge_off_glyph = cc_rgba(0.12, 0.13, 0.16, 0.82);
        t.accent = cc_rgba(ar, ag, ab, 0.95);
        t.focus_top = cc_rgba(0.45, 0.38, 0.95, 0.95);
        t.focus_bottom = cc_rgba(0.33, 0.29, 0.82, 0.95);
        t.dark = false;
        t.shadow = 0.32;
    }
    return t;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

typedef struct {
    double x, y, w, h;
} cc_rect_t;

typedef struct {
    bool show_now_playing;
    // Connectivity (left column)
    cc_rect_t conn;
    cc_rect_t wifi_row, wifi_badge;
    cc_rect_t bt_row, bt_badge;
    cc_rect_t hotspot_row, hotspot_badge;
    cc_rect_t airdrop_row, airdrop_badge;

    // Focus / DND (right column top)
    cc_rect_t focus, focus_badge;

    // Action tiles (right column bottom)
    cc_rect_t aura_connect_tile, aura_connect_badge;
    cc_rect_t screen_kbd_tile, screen_kbd_badge;
    cc_rect_t kbd_tile, kbd_badge;
    cc_rect_t mirror_tile, mirror_badge;

    // Display
    cc_rect_t display, display_track, display_detail;

    // Sound
    cc_rect_t sound, sound_track, sound_detail;

    // Now Playing
    cc_rect_t np;
    cc_rect_t np_prev, np_play, np_next;

    // Footer
    cc_rect_t footer;
    cc_rect_t footer_battery;
    cc_rect_t footer_user;
    cc_rect_t footer_accessibility;
    cc_rect_t footer_settings;

    // Legacy fields for compatibility
    cc_rect_t footer_appearance, footer_system;

    double panel_h;
} cc_layout_t;

static bool cc_is_dark(const aurade_control_center_t *cc) {
    return cc->dark_mode_available ? cc->dark_mode : true;
}

static uint64_t cc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

static cc_layout_t cc_layout_for_page(const aurade_control_center_t *cc, cc_page_t page) {
    cc_layout_t L = {0};
    if (page == CC_PAGE_WIFI) {
        double y = CC_PAD + 38.0;
        if (!cc->wifi_enabled) {
            y += 74.0;
        } else {
            if (cc->password_prompt_open) y += 126.0;
            y += 24.0;
            int max_show = cc->password_prompt_open ? 5 : 8;
            int count = cc->wifi_count > 0 ? (cc->wifi_count > max_show ? max_show : cc->wifi_count) : 1;
            y += count * 38.0;
            y += 44.0;
        }
        y += CC_PAD;
        if (y < 360.0) y = 360.0;
        if (y > CC_MAX_PANEL_H) y = CC_MAX_PANEL_H;
        L.panel_h = y;
        return L;
    } else if (page == CC_PAGE_BLUETOOTH) {
        double y = CC_PAD + 38.0;
        if (!cc->bluetooth_enabled) {
            y += 74.0;
        } else {
            if (cc->bt_status_msg[0]) y += 26.0;
            y += 24.0;
            int count = cc->bt_count > 0 ? (cc->bt_count > 6 ? 6 : cc->bt_count) : 1;
            y += count * 42.0;
            y += 44.0;
        }
        y += CC_PAD;
        if (y < 360.0) y = 360.0;
        if (y > CC_MAX_PANEL_H) y = CC_MAX_PANEL_H;
        L.panel_h = y;
        return L;
    } else if (page == CC_PAGE_SOUND) {
        double y = CC_PAD + 36.0;
        y += 56.0;
        y += 22.0 + (cc->audio_output_count > 0 ? (cc->audio_output_count > 4 ? 4 : cc->audio_output_count) * 32.0 : 32.0);
        y += 22.0 + (cc->audio_input_count > 0 ? (cc->audio_input_count > 3 ? 3 : cc->audio_input_count) * 32.0 : 32.0);
        y += 42.0;
        y += CC_PAD;
        if (y < 380.0) y = 380.0;
        if (y > CC_MAX_PANEL_H) y = CC_MAX_PANEL_H;
        L.panel_h = y;
        return L;
    }

    double y = CC_PAD;

    // Left column: Connectivity card (Wi-Fi, Bluetooth, AirDrop)
    L.conn = (cc_rect_t){CC_PAD, y, CC_COL_W, 152.0};
    double row_h = 152.0 / 3.0;
    L.wifi_row = (cc_rect_t){CC_PAD, y, CC_COL_W, row_h};
    L.wifi_badge = (cc_rect_t){CC_PAD + 10.0, y + (row_h - 30.0) / 2.0, 30.0, 30.0};

    L.bt_row = (cc_rect_t){CC_PAD, y + row_h, CC_COL_W, row_h};
    L.bt_badge = (cc_rect_t){CC_PAD + 10.0, y + row_h + (row_h - 30.0) / 2.0, 30.0, 30.0};

    L.hotspot_row = (cc_rect_t){CC_PAD, y + 2.0 * row_h, CC_COL_W, 152.0 - 2.0 * row_h};
    L.hotspot_badge = (cc_rect_t){CC_PAD + 10.0, y + 2.0 * row_h + (152.0 - 2.0 * row_h - 30.0) / 2.0, 30.0, 30.0};
    L.airdrop_row = L.hotspot_row;
    L.airdrop_badge = L.hotspot_badge;

    // Right column: Focus / DND on top, Action tiles below
    double rx = CC_PAD + CC_COL_W + CC_GAP;
    double dnd_h = 56.0;
    L.focus = (cc_rect_t){rx, y, CC_COL_W, dnd_h};
    L.focus_badge = (cc_rect_t){rx + 10.0, y + (dnd_h - 30.0) / 2.0, 30.0, 30.0};

    double tile_y = y + dnd_h + CC_GAP;
    double tile_w = (CC_COL_W - CC_GAP) / 2.0;
    double tile_h = 152.0 - dnd_h - CC_GAP;
    L.aura_connect_tile = (cc_rect_t){rx, tile_y, tile_w, tile_h};
    L.aura_connect_badge = (cc_rect_t){rx + (tile_w - 32.0) / 2.0, tile_y + 12.0, 32.0, 32.0};
    L.kbd_tile = L.aura_connect_tile;
    L.kbd_badge = L.aura_connect_badge;

    L.screen_kbd_tile = (cc_rect_t){rx + tile_w + CC_GAP, tile_y, tile_w, tile_h};
    L.screen_kbd_badge = (cc_rect_t){rx + tile_w + CC_GAP + (tile_w - 32.0) / 2.0, tile_y + 12.0, 32.0, 32.0};
    L.mirror_tile = L.screen_kbd_tile;
    L.mirror_badge = L.screen_kbd_badge;

    y += 152.0 + CC_GAP;

    // Display Card
    double slider_card_h = 68.0;
    L.display = (cc_rect_t){CC_PAD, y, CC_CONTENT_W, slider_card_h};
    double track_h = 28.0;
    double det_size = 28.0;
    double track_w = CC_CONTENT_W - 24.0 - det_size - 8.0;
    L.display_track = (cc_rect_t){CC_PAD + 12.0, y + 28.0, track_w, track_h};
    L.display_detail = (cc_rect_t){CC_PAD + CC_CONTENT_W - 12.0 - det_size, y + 28.0, det_size, det_size};

    y += slider_card_h + CC_GAP;

    // Sound Card
    L.sound = (cc_rect_t){CC_PAD, y, CC_CONTENT_W, slider_card_h};
    L.sound_track = (cc_rect_t){CC_PAD + 12.0, y + 28.0, track_w, track_h};
    L.sound_detail = (cc_rect_t){CC_PAD + CC_CONTENT_W - 12.0 - det_size, y + 28.0, det_size, det_size};

    y += slider_card_h + CC_GAP;

    // Now Playing Card
    L.show_now_playing = cc->np_available && cc->np_active;
    if (L.show_now_playing) {
        double np_h = 68.0;
        L.np = (cc_rect_t){CC_PAD, y, CC_CONTENT_W, np_h};
        double cy = L.np.y + np_h / 2.0;
        L.np_next = (cc_rect_t){L.np.x + L.np.w - 38.0, cy - 14.0, 28.0, 28.0};
        L.np_play = (cc_rect_t){L.np.x + L.np.w - 70.0, cy - 15.0, 30.0, 30.0};
        L.np_prev = (cc_rect_t){L.np.x + L.np.w - 100.0, cy - 14.0, 28.0, 28.0};
        y += np_h + CC_GAP;
    }

    // Footer
    double footer_h = 28.0;
    L.footer = (cc_rect_t){CC_PAD, y, CC_CONTENT_W, footer_h};
    L.footer_battery = (cc_rect_t){CC_PAD + 4.0, y, 110.0, footer_h};
    L.footer_settings = (cc_rect_t){CC_PAD + CC_CONTENT_W - 28.0, y, 28.0, 28.0};
    L.footer_accessibility = (cc_rect_t){L.footer_settings.x - 32.0, y, 28.0, 28.0};
    L.footer_user = (cc_rect_t){L.footer_accessibility.x - 32.0, y, 28.0, 28.0};

    // Legacy aliases
    L.footer_appearance = L.footer_accessibility;
    L.footer_system = L.footer_settings;

    y += footer_h + CC_PAD;
    L.panel_h = y;
    return L;
}

static cc_layout_t cc_layout(const aurade_control_center_t *cc) {
    return cc_layout_for_page(cc, cc->current_page);
}

// ---------------------------------------------------------------------------
// Small drawing helpers
// ---------------------------------------------------------------------------

static bool cc_hit(double x, double y, cc_rect_t r) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

static inline bool cc_sub_hit(double x, double y, cc_sub_rect_t r) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

static void draw_circle(cairo_t *cr, double cx, double cy, double r) {
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2.0 * M_PI);
}

static void cc_card(cairo_t *cr, cc_rect_t r, cc_color_t fill, cc_color_t border) {
    draw_rounded_rect(cr, r.x, r.y, r.w, r.h, CC_CARD_RADIUS);
    cc_set(cr, fill);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cc_set(cr, border);
    cairo_stroke(cr);
}

static void __attribute__((unused)) cc_card_gradient(cairo_t *cr, cc_rect_t r, cc_color_t top, cc_color_t bottom) {
    draw_rounded_rect(cr, r.x, r.y, r.w, r.h, CC_CARD_RADIUS);
    cairo_pattern_t *g = cairo_pattern_create_linear(r.x, r.y, r.x, r.y + r.h);
    cairo_pattern_add_color_stop_rgba(g, 0.0, top.r, top.g, top.b, top.a);
    cairo_pattern_add_color_stop_rgba(g, 1.0, bottom.r, bottom.g, bottom.b, bottom.a);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

static void cc_text_at(cairo_t *cr, const char *text, double size, bool bold,
                       cc_color_t color, double x, double y, bool center_x,
                       bool center_y) {
    if (!text || !*text) return;
    draw_text(cr, text, CC_FONT, size, bold, color.r, color.g, color.b, color.a,
              x, y, center_x, center_y);
}

static double cc_text_w(cairo_t *cr, const char *text, double size, bool bold) {
    return text_width(cr, text, CC_FONT, size, bold);
}

/* Single-line text truncated with a typographic ellipsis. */
static void cc_text_fit(cairo_t *cr, const char *text, double size, bool bold,
                        cc_color_t color, double x, double y, double max_w) {
    if (!text || !*text) return;
    if (cc_text_w(cr, text, size, bold) <= max_w) {
        cc_text_at(cr, text, size, bold, color, x, y, false, true);
        return;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", text);
    size_t len = strlen(buf);
    while (len > 0) {
        do {
            len--;
        } while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80);
        buf[len] = '\0';

        char candidate[260];
        snprintf(candidate, sizeof(candidate), "%s…", buf);
        if (cc_text_w(cr, candidate, size, bold) <= max_w) {
            cc_text_at(cr, candidate, size, bold, color, x, y, false, true);
            return;
        }
    }
}

static void cc_icon(cairo_t *cr, cc_icon_kind_t kind, double cx, double cy, double size,
                    cc_color_t color) {
    cairo_new_path(cr);
    draw_icon_tint(color.r, color.g, color.b);
    switch (kind) {
        case CC_ICON_WIFI:            draw_wifi_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_BLUETOOTH:       draw_bluetooth_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_HOTSPOT:         draw_hotspot_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_MOON:            draw_moon_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_DND:             draw_half_circle_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_SUN:             draw_sun_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_SPEAKER:         draw_speaker_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_AURA_CONNECT:    draw_aura_connect_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_SCREEN_KEYBOARD: draw_screen_keyboard_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_DISPLAY_BADGE:   draw_display_badge_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_AIRPLAY_AUDIO:  draw_airplay_audio_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_USER_PROFILE:   draw_user_profile_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_ACCESSIBILITY:  draw_accessibility_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_GEAR:           draw_gear_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_SLIDERS:        draw_sliders_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_NOTE:           draw_music_note_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_PLAY:           draw_play_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_PAUSE:          draw_pause_icon(cr, cx, cy, size, color.a); break;
        case CC_ICON_PREV:           draw_skip_icon(cr, cx, cy, size, -1, color.a); break;
        case CC_ICON_NEXT:           draw_skip_icon(cr, cx, cy, size, 1, color.a); break;
    }
    draw_icon_tint(1.0, 1.0, 1.0);
    cairo_new_path(cr);
}

// ---------------------------------------------------------------------------
// Panel chrome
// ---------------------------------------------------------------------------

static void cc_draw_panel(cairo_t *cr, double w, double h, const cc_theme_t *t) {
    cairo_save(cr);

    draw_rounded_rect(cr, 0.0, 0.0, w, h, CC_RADIUS);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgba(g, 0.0, t->panel_top.r, t->panel_top.g,
                                      t->panel_top.b, t->panel_top.a);
    cairo_pattern_add_color_stop_rgba(g, 1.0, t->panel_bottom.r, t->panel_bottom.g,
                                      t->panel_bottom.b, t->panel_bottom.a);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    // Hairline rim plus a brighter top edge: the classic macOS glass cue.
    draw_rounded_rect(cr, 0.5, 0.5, w - 1.0, h - 1.0, CC_RADIUS - 0.5);
    cc_set(cr, t->border);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, CC_RADIUS, 1.0);
    cairo_line_to(cr, w - CC_RADIUS, 1.0);
    cc_set(cr, t->highlight);
    cairo_stroke(cr);

    cairo_restore(cr);
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

static const char *cc_wifi_status(const aurade_control_center_t *cc) {
    if (!cc->wifi_available) return "Unavailable";
    if (!cc->wifi_enabled) return "Off";
    if (cc->wifi_connecting && cc->connecting_ssid[0]) return "Connecting…";
    if (cc->wifi_ssid[0]) return cc->wifi_ssid;
    return "Not Connected";
}

static const char *cc_bluetooth_status(const aurade_control_center_t *cc) {
    if (!cc->bluetooth_available) return "Unavailable";
    if (!cc->bluetooth_enabled) return "Off";
    if (cc->bt_connecting) return cc->bt_disconnecting ? "Disconnecting…" : "Connecting…";
    if (cc->bluetooth_device[0]) return cc->bluetooth_device;
    return "On";
}

static const char *cc_hotspot_status(const aurade_control_center_t *cc) {
    if (!cc->hotspot_available) return "Unavailable";
    if (cc->hotspot_enabled) return "On";
    return "Off";
}

static void cc_draw_spinner(cairo_t *cr, double cx, double cy, double r,
                            double phase, cc_color_t col) {
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 1.6);
    const int spokes = 8;
    for (int i = 0; i < spokes; i++) {
        double angle = (2.0 * M_PI * i) / spokes + phase;
        double fade = (double)((i + 1)) / (double)spokes;
        double alpha = col.a * (0.18 + 0.82 * fade * fade);
        cairo_set_source_rgba(cr, col.r, col.g, col.b, alpha);
        double r0 = r * 0.44;
        double r1 = r * 0.96;
        cairo_move_to(cr, cx + cos(angle) * r0, cy + sin(angle) * r0);
        cairo_line_to(cr, cx + cos(angle) * r1, cy + sin(angle) * r1);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void cc_draw_chevron(cairo_t *cr, double cx, double cy, cc_color_t color) {
    cairo_save(cr);
    cc_set(cr, color);
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - 2.5, cy - 4.5);
    cairo_line_to(cr, cx + 2.0, cy);
    cairo_line_to(cr, cx - 2.5, cy + 4.5);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void cc_draw_connectivity_item(cairo_t *cr, cc_rect_t row, cc_rect_t badge,
                                      const cc_theme_t *t, cc_icon_kind_t icon,
                                      const char *title, const char *status,
                                      bool enabled, bool available, bool busy, double spinner_phase,
                                      bool row_hover, bool badge_hover, bool pressed) {
    (void)badge_hover;
    if (row_hover || pressed) {
        cairo_save(cr);
        draw_rounded_rect(cr, row.x + 3.0, row.y + 2.0, row.w - 6.0, row.h - 4.0, 10.0);
        cc_set(cr, pressed ? t->card_pressed : t->card_hover);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    // Badge circle
    draw_circle(cr, badge.x + badge.w / 2.0, badge.y + badge.h / 2.0, badge.w / 2.0);
    if (!available) {
        cc_set(cr, cc_fade(t->badge_off, 0.55));
    } else if (enabled) {
        cc_set(cr, t->accent_blue);
    } else {
        cc_set(cr, t->badge_off);
    }
    cairo_fill(cr);

    // Badge glyph
    cc_color_t glyph;
    if (!available) {
        glyph = cc_fade(t->text_faint, 0.8);
    } else if (enabled) {
        glyph = cc_rgba(1.0, 1.0, 1.0, 1.0);
    } else {
        glyph = t->badge_off_glyph;
    }
    cc_icon(cr, icon, badge.x + badge.w / 2.0, badge.y + badge.h / 2.0, 15.0, glyph);

    // Text labels
    double tx = badge.x + badge.w + 10.0;
    double max_w = row.x + row.w - tx - 18.0;
    cc_color_t title_color = available ? t->text : cc_fade(t->text, 0.45);
    cc_color_t status_color = busy ? t->accent : (available ? t->text_dim : cc_fade(t->text_dim, 0.55));

    cc_text_fit(cr, title, 12.0, true, title_color, tx, row.y + row.h / 2.0 - 7.0, max_w);
    cc_text_fit(cr, status, 10.0, false, status_color, tx, row.y + row.h / 2.0 + 8.0, max_w);

    // Spinner when connecting/disconnecting, otherwise chevron disclosure indicator
    if (busy) {
        cc_draw_spinner(cr, row.x + row.w - 13.0, row.y + row.h / 2.0, 6.0, spinner_phase, t->accent);
    } else {
        cc_draw_chevron(cr, row.x + row.w - 12.0, row.y + row.h / 2.0, cc_fade(t->text_dim, 0.7));
    }
}

static void cc_draw_connectivity(cairo_t *cr, const cc_layout_t *L, const cc_theme_t *t,
                                 const aurade_control_center_t *cc) {
    cc_rect_t r = L->conn;
    cc_card(cr, r, t->card, t->card_border);

    // Wi-Fi
    cc_draw_connectivity_item(cr, L->wifi_row, L->wifi_badge, t, CC_ICON_WIFI,
                              "Wi-Fi", cc_wifi_status(cc),
                              cc->wifi_enabled, cc->wifi_available,
                              cc->wifi_connecting, cc->spinner_phase,
                              cc->hover == CC_MODULE_WIFI,
                              cc->hover == CC_MODULE_WIFI_TOGGLE,
                              cc->pressed == CC_MODULE_WIFI || cc->pressed == CC_MODULE_WIFI_TOGGLE);

    // Bluetooth
    cc_draw_connectivity_item(cr, L->bt_row, L->bt_badge, t, CC_ICON_BLUETOOTH,
                              "Bluetooth", cc_bluetooth_status(cc),
                              cc->bluetooth_enabled, cc->bluetooth_available,
                              cc->bt_connecting, cc->spinner_phase,
                              cc->hover == CC_MODULE_BLUETOOTH,
                              cc->hover == CC_MODULE_BLUETOOTH_TOGGLE,
                              cc->pressed == CC_MODULE_BLUETOOTH || cc->pressed == CC_MODULE_BLUETOOTH_TOGGLE);

    // Hotspot
    cc_draw_connectivity_item(cr, L->hotspot_row, L->hotspot_badge, t, CC_ICON_HOTSPOT,
                              "Hotspot", cc_hotspot_status(cc),
                              cc->hotspot_enabled, cc->hotspot_available,
                              false, cc->spinner_phase,
                              cc->hover == CC_MODULE_HOTSPOT,
                              cc->hover == CC_MODULE_HOTSPOT,
                              cc->pressed == CC_MODULE_HOTSPOT);

    // Dividers
    cairo_set_line_width(cr, 1.0);
    cc_set(cr, t->divider);
    cairo_move_to(cr, r.x + 12.0, L->bt_row.y);
    cairo_line_to(cr, r.x + r.w - 12.0, L->bt_row.y);
    cairo_stroke(cr);

    cairo_move_to(cr, r.x + 12.0, L->hotspot_row.y);
    cairo_line_to(cr, r.x + r.w - 12.0, L->hotspot_row.y);
    cairo_stroke(cr);
}

static void cc_draw_focus(cairo_t *cr, const cc_layout_t *L, const cc_theme_t *t,
                          const aurade_control_center_t *cc) {
    cc_rect_t r = L->focus;
    bool on = cc->dnd_enabled && cc->dnd_available;
    bool hovered = cc->hover == CC_MODULE_FOCUS;
    bool pressed = cc->pressed == CC_MODULE_FOCUS;

    cc_card(cr, r, hovered ? t->card_hover : (pressed ? t->card_pressed : t->card), t->card_border);

    // Badge circle
    cc_rect_t b = L->focus_badge;
    draw_circle(cr, b.x + b.w / 2.0, b.y + b.h / 2.0, b.w / 2.0);
    if (on) {
        cc_set(cr, t->accent_blue);
    } else {
        cc_set(cr, t->badge_off);
    }
    cairo_fill(cr);

    // Badge glyph
    cc_color_t glyph = on ? cc_rgba(1.0, 1.0, 1.0, 1.0) : t->badge_off_glyph;
    cc_icon(cr, CC_ICON_MOON, b.x + b.w / 2.0, b.y + b.h / 2.0, 15.0, glyph);

    // Text labels
    double tx = b.x + b.w + 10.0;
    double max_w = r.x + r.w - tx - 12.0;
    cc_text_fit(cr, "Do Not Disturb", 12.0, true, t->text, tx, r.y + r.h / 2.0 - 7.0, max_w);
    cc_text_fit(cr, on ? "On" : "Off", 10.0, false, t->text_dim, tx, r.y + r.h / 2.0 + 8.0, max_w);
}

static void cc_draw_action_tiles(cairo_t *cr, const cc_layout_t *L, const cc_theme_t *t,
                                 const aurade_control_center_t *cc) {
    // 1. Aura Connect tile (dummy)
    cc_rect_t act = L->aura_connect_tile;
    bool ac_hov = cc->hover == CC_MODULE_AURA_CONNECT;
    bool ac_pre = cc->pressed == CC_MODULE_AURA_CONNECT;
    cc_card(cr, act, ac_hov ? t->card_hover : (ac_pre ? t->card_pressed : t->card), t->card_border);

    // Circular badge on top
    cc_rect_t ac_b = L->aura_connect_badge;
    draw_circle(cr, ac_b.x + ac_b.w / 2.0, ac_b.y + ac_b.h / 2.0, ac_b.w / 2.0);
    if (cc->aura_connect_enabled) {
        cc_set(cr, t->accent_blue);
    } else {
        cc_set(cr, t->badge_off);
    }
    cairo_fill(cr);

    cc_color_t ac_glyph = cc->aura_connect_enabled ? cc_rgba(1.0, 1.0, 1.0, 1.0) : t->badge_off_glyph;
    cc_icon(cr, CC_ICON_AURA_CONNECT, ac_b.x + ac_b.w / 2.0, ac_b.y + ac_b.h / 2.0, 16.0, ac_glyph);

    // Label below
    cc_text_at(cr, "Aura", 10.0, true, t->text, act.x + act.w / 2.0, act.y + 56.0, true, true);
    cc_text_at(cr, "Connect", 10.0, true, t->text, act.x + act.w / 2.0, act.y + 70.0, true, true);

    // 2. Screen Keyboard tile (dummy)
    cc_rect_t skt = L->screen_kbd_tile;
    bool sk_hov = cc->hover == CC_MODULE_SCREEN_KEYBOARD;
    bool sk_pre = cc->pressed == CC_MODULE_SCREEN_KEYBOARD;
    cc_card(cr, skt, sk_hov ? t->card_hover : (sk_pre ? t->card_pressed : t->card), t->card_border);

    // Circular badge on top
    cc_rect_t sk_b = L->screen_kbd_badge;
    draw_circle(cr, sk_b.x + sk_b.w / 2.0, sk_b.y + sk_b.h / 2.0, sk_b.w / 2.0);
    if (cc->screen_keyboard_enabled) {
        cc_set(cr, t->accent_blue);
    } else {
        cc_set(cr, t->badge_off);
    }
    cairo_fill(cr);

    cc_color_t sk_glyph = cc->screen_keyboard_enabled ? cc_rgba(1.0, 1.0, 1.0, 1.0) : t->badge_off_glyph;
    cc_icon(cr, CC_ICON_SCREEN_KEYBOARD, sk_b.x + sk_b.w / 2.0, sk_b.y + sk_b.h / 2.0, 16.0, sk_glyph);

    // Label below
    cc_text_at(cr, "Screen", 10.0, true, t->text, skt.x + skt.w / 2.0, skt.y + 56.0, true, true);
    cc_text_at(cr, "Keyboard", 10.0, true, t->text, skt.x + skt.w / 2.0, skt.y + 70.0, true, true);
}

static void cc_draw_slider_card(cairo_t *cr, cc_rect_t r, cc_rect_t tr, cc_rect_t det,
                                const cc_theme_t *t, cc_icon_kind_t slider_icon,
                                cc_icon_kind_t detail_icon, const char *title,
                                double value, bool muted, bool available,
                                bool slider_hov, bool slider_pre,
                                bool det_hov, bool det_pre) {
    (void)slider_hov;
    (void)slider_pre;
    cc_card(cr, r, t->card, t->card_border);

    // Title at top left
    cc_text_at(cr, title, 12.0, true, t->text, r.x + 14.0, r.y + 14.0, false, true);

    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;

    // Track capsule
    double cap_r = tr.h / 2.0;
    draw_rounded_rect(cr, tr.x, tr.y, tr.w, tr.h, cap_r);
    cc_set(cr, t->track);
    cairo_fill(cr);

    // Draggable thumb knob coordinates
    double knob_cx = tr.x + cap_r + (tr.w - tr.h) * value;
    double knob_cy = tr.y + cap_r;
    double knob_r = cap_r - 1.5;

    // Fill up to knob center
    if (available && value > 0.001) {
        cairo_save(cr);
        draw_rounded_rect(cr, tr.x, tr.y, tr.w, tr.h, cap_r);
        cairo_clip(cr);
        cairo_rectangle(cr, tr.x, tr.y, knob_cx - tr.x, tr.h);
        cc_set(cr, muted ? t->fill_muted : t->fill);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    // Draggable thumb knob with soft drop shadow
    if (available) {
        // Drop shadow
        cairo_save(cr);
        cairo_arc(cr, knob_cx, knob_cy + 1.0, knob_r, 0, 2.0 * M_PI);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.16);
        cairo_fill(cr);

        // Knob circle
        cairo_arc(cr, knob_cx, knob_cy, knob_r, 0, 2.0 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 0.8);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.08);
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    // Inner icon inside capsule on left
    double icon_cx = tr.x + 14.0;
    double fill_right = knob_cx;
    bool icon_over_fill = available && (fill_right > icon_cx + 9.0) && !muted;
    cc_color_t icon_color;
    if (!available) {
        icon_color = cc_fade(t->text_faint, 0.8);
    } else if (muted) {
        icon_color = cc_fade(t->text_dim, 0.6);
    } else if (icon_over_fill) {
        icon_color = t->fill_text;
    } else {
        icon_color = t->badge_off_glyph;
    }
    cc_icon(cr, slider_icon, icon_cx, tr.y + cap_r, 16.0, icon_color);

    // Detail button on right
    draw_rounded_rect(cr, det.x, det.y, det.w, det.h, 7.0);
    if (det_hov || det_pre) {
        cc_set(cr, det_pre ? t->card_pressed : t->card_hover);
        cairo_fill_preserve(cr);
    } else {
        cc_set(cr, t->badge_off);
        cairo_fill_preserve(cr);
    }
    cairo_set_line_width(cr, 1.0);
    cc_set(cr, t->card_border);
    cairo_stroke(cr);

    cc_icon(cr, detail_icon, det.x + det.w / 2.0, det.y + det.h / 2.0, 15.0,
            det_hov ? t->text : t->badge_off_glyph);
}

static void cc_draw_media_button(cairo_t *cr, cc_rect_t r, const cc_theme_t *t,
                                 cc_icon_kind_t icon, bool hovered, bool pressed,
                                 double icon_size) {
    if (hovered || pressed) {
        cairo_arc(cr, r.x + r.w / 2.0, r.y + r.h / 2.0, r.w / 2.0, 0, 2 * M_PI);
        cc_set(cr, pressed ? t->card_pressed : t->overlay);
        cairo_fill(cr);
    }
    cc_icon(cr, icon, r.x + r.w / 2.0, r.y + r.h / 2.0, icon_size,
            cc_fade(t->text, hovered ? 1.0 : 0.88));
}

static void cc_draw_album_art(cairo_t *cr, cc_rect_t r, const char *seed) {
    unsigned hash = 2166136261u;
    for (const char *p = seed; p && *p; p++) {
        hash = (hash ^ (unsigned char)*p) * 16777619u;
    }
    static const double pairs[5][6] = {
        {0.99, 0.45, 0.42, 0.58, 0.24, 0.72},
        {0.30, 0.62, 0.99, 0.12, 0.30, 0.62},
        {0.99, 0.72, 0.30, 0.86, 0.32, 0.36},
        {0.45, 0.86, 0.66, 0.13, 0.52, 0.55},
        {0.72, 0.52, 0.98, 0.32, 0.22, 0.62},
    };
    const double *p = pairs[hash % 5];

    cairo_save(cr);
    draw_rounded_rect(cr, r.x, r.y, r.w, r.h, 9.0);
    cairo_clip(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(r.x, r.y, r.x + r.w, r.y + r.h);
    cairo_pattern_add_color_stop_rgb(g, 0.0, p[0], p[1], p[2]);
    cairo_pattern_add_color_stop_rgb(g, 1.0, p[3], p[4], p[5]);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    cairo_pattern_t *sheen = cairo_pattern_create_radial(
        r.x + r.w * 0.25, r.y + r.h * 0.2, 2, r.x + r.w * 0.25, r.y + r.h * 0.2, r.w);
    cairo_pattern_add_color_stop_rgba(sheen, 0.0, 1.0, 1.0, 1.0, 0.28);
    cairo_pattern_add_color_stop_rgba(sheen, 1.0, 1.0, 1.0, 1.0, 0.0);
    cairo_set_source(cr, sheen);
    cairo_paint(cr);
    cairo_pattern_destroy(sheen);
    cairo_restore(cr);

    cc_icon(cr, CC_ICON_NOTE, r.x + r.w / 2.0, r.y + r.h / 2.0, 18.0,
            cc_rgba(1.0, 1.0, 1.0, 0.95));
}

static void cc_draw_now_playing(cairo_t *cr, const cc_layout_t *L, const cc_theme_t *t,
                                const aurade_control_center_t *cc) {
    cc_rect_t r = L->np;
    cc_card(cr, r, t->card, t->card_border);

    cc_rect_t art = {r.x + 12.0, r.y + (r.h - 44.0) / 2.0, 44.0, 44.0};
    cc_draw_album_art(cr, art, cc->np_title[0] ? cc->np_title : "AuraDE");

    double tx = art.x + art.w + 10.0;
    double title_max = L->np_prev.x - tx - 8.0;
    if (title_max < 40.0) title_max = 40.0;

    cc_text_fit(cr, cc->np_title[0] ? cc->np_title : "Nothing Playing", 12.0, true,
                t->text, tx, r.y + r.h / 2.0 - 7.0, title_max);
    cc_text_fit(cr, cc->np_artist[0] ? cc->np_artist : (cc->np_playing ? "" : "Paused"),
                10.0, false, t->text_dim, tx, r.y + r.h / 2.0 + 8.0, title_max);

    cc_draw_media_button(cr, L->np_prev, t, CC_ICON_PREV,
                         cc->hover == CC_MODULE_PREV, cc->pressed == CC_MODULE_PREV, 13.0);
    cc_draw_media_button(cr, L->np_play, t, cc->np_playing ? CC_ICON_PAUSE : CC_ICON_PLAY,
                         cc->hover == CC_MODULE_PLAY, cc->pressed == CC_MODULE_PLAY, 15.0);
    cc_draw_media_button(cr, L->np_next, t, CC_ICON_NEXT,
                         cc->hover == CC_MODULE_NEXT, cc->pressed == CC_MODULE_NEXT, 13.0);
}

static void cc_draw_footer(cairo_t *cr, const cc_layout_t *L, const cc_theme_t *t,
                           const aurade_control_center_t *cc) {
    cairo_new_path(cr);

    // Battery on left
    cc_rect_t bat = L->footer_battery;
    double bat_w = 22.0;
    double bat_h = 11.0;
    double bat_x = bat.x + 2.0;
    double bat_y = bat.y + (bat.h - bat_h) / 2.0;

    double level = cc->battery_available ? (cc->battery_percent / 100.0) : 1.0;
    bool charging = cc->battery_available && cc->battery_charging;
    cairo_new_path(cr);
    draw_battery_icon(cr, bat_x, bat_y, bat_w, bat_h, level, t->text.a, charging);
    cairo_new_path(cr);

    char bat_str[32];
    if (cc->battery_available) {
        if (charging) {
            snprintf(bat_str, sizeof(bat_str), "⚡ %d%%", cc->battery_percent);
        } else {
            snprintf(bat_str, sizeof(bat_str), "%d%%", cc->battery_percent);
        }
    } else {
        snprintf(bat_str, sizeof(bat_str), "100%%");
    }
    cc_text_at(cr, bat_str, 11.5, true, t->text, bat_x + bat_w + 8.0, bat.y + bat.h / 2.0, false, true);
    cairo_new_path(cr);

    // Quick action buttons on right: User, Accessibility, Settings
    cc_rect_t u_btn = L->footer_user;
    if (cc->hover == CC_MODULE_USER) {
        draw_circle(cr, u_btn.x + u_btn.w / 2.0, u_btn.y + u_btn.h / 2.0, u_btn.w / 2.0);
        cc_set(cr, t->card_hover);
        cairo_fill(cr);
    }
    cc_icon(cr, CC_ICON_USER_PROFILE, u_btn.x + u_btn.w / 2.0, u_btn.y + u_btn.h / 2.0, 14.0, t->text);
    cairo_new_path(cr);

    cc_rect_t a_btn = L->footer_accessibility;
    if (cc->hover == CC_MODULE_ACCESSIBILITY) {
        draw_circle(cr, a_btn.x + a_btn.w / 2.0, a_btn.y + a_btn.h / 2.0, a_btn.w / 2.0);
        cc_set(cr, t->card_hover);
        cairo_fill(cr);
    }
    cc_icon(cr, CC_ICON_ACCESSIBILITY, a_btn.x + a_btn.w / 2.0, a_btn.y + a_btn.h / 2.0, 14.0, t->text);
    cairo_new_path(cr);

    cc_rect_t s_btn = L->footer_settings;
    if (cc->hover == CC_MODULE_SETTINGS) {
        draw_circle(cr, s_btn.x + s_btn.w / 2.0, s_btn.y + s_btn.h / 2.0, s_btn.w / 2.0);
        cc_set(cr, t->card_hover);
        cairo_fill(cr);
    }
    cc_icon(cr, CC_ICON_GEAR, s_btn.x + s_btn.w / 2.0, s_btn.y + s_btn.h / 2.0, 14.0, t->text);
    cairo_new_path(cr);
}

// ---------------------------------------------------------------------------
// System state probes
// ---------------------------------------------------------------------------

static void run_cmd_async(const char *cmd) {
    if (!cmd || !*cmd) return;
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static int run_cmd_capture(const char *cmd, char *out, size_t max_size) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    size_t n = fread(out, 1, max_size - 1, fp);
    out[n] = '\0';
    pclose(fp);
    return (int)n;
}

static bool capture_command(const char *command, char *out, size_t size) {
    if (!out || size < 2) return false;
    out[0] = '\0';

    FILE *pipe = popen(command, "r");
    if (!pipe) return false;

    size_t n = fread(out, 1, size - 1, pipe);
    out[n] = '\0';
    int status = pclose(pipe);

    if (status != 0 || n == 0) return false;

    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' ' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
    return true;
}

static int percent_before(const char *value, const char *end) {
    const char *p = end;
    while (p > value && (isdigit((unsigned char)p[-1]) || p[-1] == ' ')) p--;
    return atoi(p);
}

static void unquote(char *text) {
    if (!text) return;
    size_t len = strlen(text);
    if (len >= 2 && text[0] == '\'' && text[len - 1] == '\'') {
        memmove(text, text + 1, len - 2);
        text[len - 2] = '\0';
    }
}

static bool read_sysfs_brightness(double *out_br) {
    DIR *dir = opendir("/sys/class/backlight");
    if (!dir) return false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char b_path[512], m_path[512];
        snprintf(b_path, sizeof(b_path), "/sys/class/backlight/%s/brightness", entry->d_name);
        snprintf(m_path, sizeof(m_path), "/sys/class/backlight/%s/max_brightness", entry->d_name);
        FILE *fb = fopen(b_path, "r");
        FILE *fm = fopen(m_path, "r");
        if (fb && fm) {
            long cur = 0, max = 0;
            if (fscanf(fb, "%ld", &cur) == 1 && fscanf(fm, "%ld", &max) == 1 && max > 0) {
                fclose(fb);
                fclose(fm);
                closedir(dir);
                double val = (double)cur / (double)max;
                if (val < 0.01) val = 0.01;
                if (val > 1.0) val = 1.0;
                if (out_br) *out_br = val;
                return true;
            }
        }
        if (fb) fclose(fb);
        if (fm) fclose(fm);
    }
    closedir(dir);
    return false;
}

static void probe_worker_wifi(cc_probe_snapshot_t *snap, bool scan_details, bool force_rescan) {
    char out[1024];
    if (!capture_command("nmcli radio wifi 2>/dev/null", out, sizeof(out))) {
        snap->wifi_available = false;
        snap->wifi_enabled = false;
        snap->wifi_ssid[0] = '\0';
        return;
    }
    snap->wifi_available = true;
    snap->wifi_enabled = strncmp(out, "enabled", 7) == 0;
    snap->wifi_ssid[0] = '\0';
    if (!snap->wifi_enabled) return;

    // Fast active SSID check without scanning: nmcli -t -f TYPE,STATE,CONNECTION dev
    char dev_out[1024];
    if (capture_command("nmcli -t -f TYPE,STATE,CONNECTION dev 2>/dev/null", dev_out, sizeof(dev_out))) {
        char *line = dev_out;
        while (line && *line) {
            char *next = strchr(line, '\n');
            if (next) *next = '\0';
            if (strncmp(line, "wifi:connected:", 15) == 0) {
                snprintf(snap->wifi_ssid, sizeof(snap->wifi_ssid), "%s", line + 15);
                break;
            }
            line = next ? next + 1 : NULL;
        }
    }

    if (!scan_details) return;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "nmcli -t -f ACTIVE,SSID,SIGNAL,SECURITY dev wifi list %s 2>/dev/null",
             force_rescan ? "" : "--rescan no");
    char list_buf[16384] = {0};
    if (run_cmd_capture(cmd, list_buf, sizeof(list_buf)) <= 0) return;

    char *line = list_buf;
    while (line && *line && snap->wifi_count < 24) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';
        while (*line == ' ' || *line == '\t') line++;
        if (*line) {
            bool active = false;
            char *p = line;
            if (strncmp(p, "yes:", 4) == 0) {
                active = true;
                p += 4;
            } else if (strncmp(p, "no:", 3) == 0) {
                active = false;
                p += 3;
            } else {
                line = next ? next + 1 : NULL;
                continue;
            }

            char ssid[64] = {0};
            int s_idx = 0;
            while (*p && s_idx < (int)sizeof(ssid) - 1) {
                if (*p == '\\' && *(p + 1) == ':') {
                    p++;
                    ssid[s_idx++] = *p++;
                } else if (*p == ':') {
                    break;
                } else {
                    ssid[s_idx++] = *p++;
                }
            }
            ssid[s_idx] = '\0';

            if (ssid[0] != '\0' && strcmp(ssid, "--") != 0) {
                int sig = 0;
                bool sec_flag = false;
                if (*p == ':') {
                    p++;
                    sig = atoi(p);
                    char *colon_sec = strchr(p, ':');
                    if (colon_sec) {
                        char *security = colon_sec + 1;
                        sec_flag = (strstr(security, "WPA") != NULL ||
                                    strstr(security, "WEP") != NULL ||
                                    strstr(security, "802.1X") != NULL);
                    }
                }

                int found = -1;
                for (int i = 0; i < snap->wifi_count; i++) {
                    if (strcmp(snap->wifi_items[i].ssid, ssid) == 0) {
                        found = i;
                        break;
                    }
                }
                if (found >= 0) {
                    if (active) snap->wifi_items[found].active = true;
                    if (sig > snap->wifi_items[found].signal) snap->wifi_items[found].signal = sig;
                } else {
                    snprintf(snap->wifi_items[snap->wifi_count].ssid,
                             sizeof(snap->wifi_items[snap->wifi_count].ssid), "%s", ssid);
                    snap->wifi_items[snap->wifi_count].signal = sig;
                    snap->wifi_items[snap->wifi_count].secure = sec_flag;
                    snap->wifi_items[snap->wifi_count].active = active;
                    snap->wifi_count++;
                }
                if (active && snap->wifi_ssid[0] == '\0') {
                    snprintf(snap->wifi_ssid, sizeof(snap->wifi_ssid), "%s", ssid);
                }
            }
        }
        line = next ? next + 1 : NULL;
    }

    for (int i = 0; i < snap->wifi_count; i++) {
        if (snap->wifi_items[i].active) {
            if (i > 0) {
                cc_wifi_item_t tmp = snap->wifi_items[i];
                for (int j = i; j > 0; j--) snap->wifi_items[j] = snap->wifi_items[j - 1];
                snap->wifi_items[0] = tmp;
            }
            break;
        }
    }

    // Sort remaining non-active networks by signal descending
    int start_idx = (snap->wifi_count > 0 && snap->wifi_items[0].active) ? 1 : 0;
    for (int i = start_idx; i < snap->wifi_count - 1; i++) {
        for (int j = i + 1; j < snap->wifi_count; j++) {
            if (snap->wifi_items[j].signal > snap->wifi_items[i].signal) {
                cc_wifi_item_t tmp = snap->wifi_items[i];
                snap->wifi_items[i] = snap->wifi_items[j];
                snap->wifi_items[j] = tmp;
            }
        }
    }
}

static bool is_unnamed_bt_device(const char *name) {
    if (!name || !*name) return true;
    size_t len = strlen(name);
    if (len == 17) {
        bool all_mac = true;
        for (size_t i = 0; i < 17; i++) {
            if (i % 3 == 2) {
                if (name[i] != ':' && name[i] != '-') {
                    all_mac = false;
                    break;
                }
            } else if (!isxdigit((unsigned char)name[i])) {
                all_mac = false;
                break;
            }
        }
        if (all_mac) return true;
    }
    return false;
}

static void add_bt_lines_to_snap(cc_probe_snapshot_t *snap, char *buf,
                                 const char *conn_buf, bool skip_unnamed) {
    char *line = buf;
    while (line && *line && snap->bt_count < 15) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';
        if (strncmp(line, "Device ", 7) == 0) {
            char *mac = line + 7;
            char *space = strchr(mac, ' ');
            if (space) {
                *space = '\0';
                char *name = space + 1;
                while (*name == ' ') name++;
                if (!skip_unnamed || !is_unnamed_bt_device(name)) {
                    bool exists = false;
                    for (int i = 0; i < snap->bt_count; i++) {
                        if (strcmp(snap->bt_items[i].mac, mac) == 0) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        snprintf(snap->bt_items[snap->bt_count].mac,
                                 sizeof(snap->bt_items[snap->bt_count].mac), "%s", mac);
                        snprintf(snap->bt_items[snap->bt_count].name,
                                 sizeof(snap->bt_items[snap->bt_count].name), "%s", name);
                        snap->bt_items[snap->bt_count].connected = (strstr(conn_buf, mac) != NULL);
                        snap->bt_count++;
                    }
                }
            }
        }
        line = next ? next + 1 : NULL;
    }
}

static void probe_worker_bluetooth(cc_probe_snapshot_t *snap, bool list_devices) {
    char pwr_buf[1024] = {0};
    int n = run_cmd_capture("bluetoothctl show 2>/dev/null", pwr_buf, sizeof(pwr_buf));
    if (n > 0) {
        snap->bluetooth_available = true;
        snap->bluetooth_enabled = (strstr(pwr_buf, "Powered: yes") != NULL);
    } else {
        char rf_buf[256] = {0};
        if (run_cmd_capture("rfkill list bluetooth 2>/dev/null", rf_buf, sizeof(rf_buf)) > 0) {
            snap->bluetooth_available = true;
            snap->bluetooth_enabled = (strstr(rf_buf, "Soft blocked: yes") == NULL);
        } else {
            snap->bluetooth_available = false;
            snap->bluetooth_enabled = false;
            snap->bluetooth_device[0] = '\0';
            return;
        }
    }
    snap->bluetooth_device[0] = '\0';
    if (!snap->bluetooth_enabled) return;

    char conn_buf[1024] = {0};
    if (run_cmd_capture("bluetoothctl devices Connected 2>/dev/null", conn_buf, sizeof(conn_buf)) > 0) {
        char *device = strstr(conn_buf, "Device ");
        if (device) {
            device += 7;
            char *space = strchr(device, ' ');
            if (space && space[1]) {
                char *name = space + 1;
                while (*name == ' ') name++;
                char *nl = strchr(name, '\n');
                if (nl) *nl = '\0';
                snprintf(snap->bluetooth_device, sizeof(snap->bluetooth_device), "%s", name);
            }
        }
    }

    if (!list_devices) return;

    // 1. Always add Paired devices first so known headphones/mice/keyboards appear at the top
    char paired_buf[4096] = {0};
    if (run_cmd_capture("bluetoothctl devices Paired 2>/dev/null", paired_buf, sizeof(paired_buf)) > 0) {
        add_bt_lines_to_snap(snap, paired_buf, conn_buf, true);
    }

    // 2. Add any other discovered devices, filtering out unnamed MAC-only BLE beacons
    char dev_buf[8192] = {0};
    if (run_cmd_capture("bluetoothctl devices 2>/dev/null", dev_buf, sizeof(dev_buf)) > 0) {
        add_bt_lines_to_snap(snap, dev_buf, conn_buf, true);
    }

    for (int i = 0; i < snap->bt_count; i++) {
        if (snap->bt_items[i].connected) {
            if (i > 0) {
                cc_bt_item_t tmp = snap->bt_items[i];
                for (int j = i; j > 0; j--) snap->bt_items[j] = snap->bt_items[j - 1];
                snap->bt_items[0] = tmp;
            }
            break;
        }
    }
}

static void probe_worker_dark_mode(cc_probe_snapshot_t *snap) {
    char out[256];
    if (capture_command("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", out, sizeof(out))) {
        snap->dark_mode_available = true;
        snap->dark_mode = strstr(out, "dark") != NULL || strstr(out, "Dark") != NULL;
        return;
    }
    if (capture_command("gsettings get org.gnome.desktop.interface gtk-theme 2>/dev/null", out, sizeof(out))) {
        snap->dark_mode_available = true;
        snap->dark_mode = strstr(out, "Dark") != NULL || strstr(out, "dark") != NULL;
        return;
    }
    snap->dark_mode_available = false;
}

static void probe_worker_dnd(cc_probe_snapshot_t *snap) {
    char out[128];
    if (!capture_command("gsettings get org.gnome.desktop.notifications show-banners 2>/dev/null", out, sizeof(out))) {
        snap->dnd_available = false;
        return;
    }
    snap->dnd_available = true;
    snap->dnd_enabled = strstr(out, "false") != NULL;
}

static void probe_worker_brightness(cc_probe_snapshot_t *snap) {
    if (read_sysfs_brightness(&snap->brightness)) {
        snap->brightness_available = true;
        return;
    }
    char out[256];
    if (capture_command("brightnessctl -m 2>/dev/null", out, sizeof(out))) {
        char *pct = strchr(out, '%');
        if (pct) {
            int p = percent_before(out, pct);
            if (p < 1) p = 1;
            if (p > 100) p = 100;
            snap->brightness = p / 100.0;
            snap->brightness_available = true;
            return;
        }
    }
    snap->brightness_available = false;
}

static void probe_worker_volume(cc_probe_snapshot_t *snap, bool list_devices) {
    char out[512];
    if (capture_command("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", out, sizeof(out))) {
        double value = 0.0;
        if (sscanf(out, "Volume: %lf", &value) == 1) {
            if (value < 0.0) value = 0.0;
            if (value > 1.0) value = 1.0;
            snap->volume = value;
            snap->volume_muted = strstr(out, "[MUTED]") != NULL;
            snap->volume_uses_pactl = false;
            snap->volume_available = true;
            goto list_sound;
        }
    }

    if (capture_command("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", out, sizeof(out))) {
        char *pct = strchr(out, '%');
        if (pct) {
            int p = percent_before(out, pct);
            if (p < 0) p = 0;
            if (p > 100) p = 100;
            snap->volume = p / 100.0;
            snap->volume_uses_pactl = true;
            snap->volume_available = true;
            char mute_buf[128];
            if (capture_command("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null", mute_buf, sizeof(mute_buf))) {
                snap->volume_muted = strstr(mute_buf, "yes") != NULL;
            } else {
                snap->volume_muted = false;
            }
            goto list_sound;
        }
    }

    snap->volume_available = false;
    snap->volume_muted = false;

list_sound:
    if (!list_devices) return;

    char def_sink[128] = {0};
    char def_source[128] = {0};
    run_cmd_capture("pactl get-default-sink 2>/dev/null", def_sink, sizeof(def_sink));
    run_cmd_capture("pactl get-default-source 2>/dev/null", def_source, sizeof(def_source));
    char *nl = strchr(def_sink, '\n'); if (nl) *nl = '\0';
    nl = strchr(def_source, '\n'); if (nl) *nl = '\0';

    char sinks_buf[16384] = {0};
    if (run_cmd_capture("pactl -f json list sinks 2>/dev/null", sinks_buf, sizeof(sinks_buf)) > 0) {
        struct json_object *root = json_tokener_parse(sinks_buf);
        if (root && json_object_is_type(root, json_type_array)) {
            int len = json_object_array_length(root);
            for (int i = 0; i < len && snap->audio_output_count < 10; i++) {
                struct json_object *obj = json_object_array_get_idx(root, i);
                struct json_object *jname = NULL, *jdesc = NULL;
                if (json_object_object_get_ex(obj, "name", &jname) &&
                    json_object_object_get_ex(obj, "description", &jdesc)) {
                    const char *name = json_object_get_string(jname);
                    const char *desc = json_object_get_string(jdesc);
                    if (name && strstr(name, ".monitor") == NULL) {
                        snprintf(snap->audio_outputs[snap->audio_output_count].name,
                                 sizeof(snap->audio_outputs[snap->audio_output_count].name), "%s", name);
                        snprintf(snap->audio_outputs[snap->audio_output_count].description,
                                 sizeof(snap->audio_outputs[snap->audio_output_count].description), "%s", desc ? desc : name);
                        snap->audio_outputs[snap->audio_output_count].is_default = (strcmp(name, def_sink) == 0);
                        snap->audio_output_count++;
                    }
                }
            }
            json_object_put(root);
        }
    }

    char sources_buf[16384] = {0};
    if (run_cmd_capture("pactl -f json list sources 2>/dev/null", sources_buf, sizeof(sources_buf)) > 0) {
        struct json_object *root = json_tokener_parse(sources_buf);
        if (root && json_object_is_type(root, json_type_array)) {
            int len = json_object_array_length(root);
            for (int i = 0; i < len && snap->audio_input_count < 10; i++) {
                struct json_object *obj = json_object_array_get_idx(root, i);
                struct json_object *jname = NULL, *jdesc = NULL, *jmon = NULL;
                if (json_object_object_get_ex(obj, "name", &jname) &&
                    json_object_object_get_ex(obj, "description", &jdesc)) {
                    const char *name = json_object_get_string(jname);
                    const char *desc = json_object_get_string(jdesc);
                    const char *mon = json_object_object_get_ex(obj, "monitor_source", &jmon)
                                      ? json_object_get_string(jmon) : NULL;

                    if (name && strstr(name, ".monitor") == NULL && (!mon || !*mon)) {
                        snprintf(snap->audio_inputs[snap->audio_input_count].name,
                                 sizeof(snap->audio_inputs[snap->audio_input_count].name), "%s", name);
                        snprintf(snap->audio_inputs[snap->audio_input_count].description,
                                 sizeof(snap->audio_inputs[snap->audio_input_count].description), "%s", desc ? desc : name);
                        snap->audio_inputs[snap->audio_input_count].is_default = (strcmp(name, def_source) == 0);
                        snap->audio_input_count++;
                    }
                }
            }
            json_object_put(root);
        }
    }
}

static void probe_worker_now_playing(cc_probe_snapshot_t *snap) {
    char out[1024];
    if (!capture_command("playerctl metadata --format "
                         "'{{status}}||{{artist}}||{{title}}' 2>/dev/null",
                         out, sizeof(out))) {
        snap->np_available = true;
        snap->np_active = false;
        snap->np_playing = false;
        snap->np_title[0] = '\0';
        snap->np_artist[0] = '\0';
        return;
    }
    char *nl = strchr(out, '\n'); if (nl) *nl = '\0';
    char *sep1 = strstr(out, "||");
    if (!sep1) return;
    *sep1 = '\0';
    char *artist = sep1 + 2;
    char *sep2 = strstr(artist, "||");
    if (!sep2) return;
    *sep2 = '\0';
    char *title = sep2 + 2;

    snap->np_available = true;
    snap->np_active = true;
    snap->np_playing = strcmp(out, "Playing") == 0;
    snprintf(snap->np_title, sizeof(snap->np_title), "%s", title);
    snprintf(snap->np_artist, sizeof(snap->np_artist), "%s", artist);
}

static void probe_worker_battery(cc_probe_snapshot_t *snap) {
    DIR *dir = opendir("/sys/class/power_supply");
    if (!dir) {
        snap->battery_available = false;
        return;
    }
    struct dirent *entry;
    bool found = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "BAT", 3) != 0) continue;
        char cap_path[512], stat_path[512];
        snprintf(cap_path, sizeof(cap_path), "/sys/class/power_supply/%s/capacity", entry->d_name);
        snprintf(stat_path, sizeof(stat_path), "/sys/class/power_supply/%s/status", entry->d_name);
        FILE *fc = fopen(cap_path, "r");
        if (fc) {
            int cap = 0;
            if (fscanf(fc, "%d", &cap) == 1) {
                if (cap < 0) cap = 0;
                if (cap > 100) cap = 100;
                snap->battery_percent = cap;
                snap->battery_available = true;
                found = true;
            }
            fclose(fc);
        }
        FILE *fs = fopen(stat_path, "r");
        if (fs) {
            char stat[32] = {0};
            if (fscanf(fs, "%31s", stat) == 1) {
                snap->battery_charging = (strcmp(stat, "Charging") == 0);
            }
            fclose(fs);
        }
        if (found) break;
    }
    closedir(dir);
    if (!found) {
        snap->battery_available = false;
    }
}

static void probe_worker_hotspot(cc_probe_snapshot_t *snap) {
    snap->hotspot_available = snap->wifi_available;
    char out[1024];
    if (capture_command("nmcli -t -f NAME,TYPE connection show --active 2>/dev/null", out, sizeof(out))) {
        char *line = out;
        while (line && *line) {
            char *next = strchr(line, '\n');
            if (next) *next = '\0';
            if (strcasestr(line, "hotspot") != NULL) {
                snap->hotspot_enabled = true;
                break;
            }
            line = next ? next + 1 : NULL;
        }
    }
}

typedef struct {
    aurade_control_center_t *cc;
    cc_page_t page;
    bool scan_wifi;
} cc_probe_worker_args_t;

static void *cc_probe_worker_thread(void *data) {
    cc_probe_worker_args_t *args = (cc_probe_worker_args_t *)data;
    aurade_control_center_t *cc = args->cc;
    cc_page_t page = args->page;
    bool scan_wifi = args->scan_wifi;
    free(args);

    cc_probe_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));

    // Always pre-fetch Wi-Fi networks (--rescan no takes ~15ms) and Bluetooth devices (~15ms)
    // so opening Control Center or subpages is 100% instant with 0 latency.
    probe_worker_wifi(&snap, true, scan_wifi);
    probe_worker_bluetooth(&snap, true);
    probe_worker_dark_mode(&snap);
    probe_worker_dnd(&snap);
    probe_worker_brightness(&snap);
    probe_worker_volume(&snap, page == CC_PAGE_SOUND);
    probe_worker_now_playing(&snap);
    probe_worker_battery(&snap);
    probe_worker_hotspot(&snap);

    pthread_mutex_lock(&cc->probe_mutex);
    cc->probe_result = snap;
    cc->probe_has_result = true;
    pthread_mutex_unlock(&cc->probe_mutex);

    uint64_t val = 1;
    ssize_t w = write(cc->probe_event_fd, &val, sizeof(val));
    (void)w;
    return NULL;
}

static void cc_trigger_async_refresh(aurade_control_center_t *cc, bool scan_wifi) {
    if (!cc || cc->probe_event_fd < 0) return;

    pthread_mutex_lock(&cc->probe_mutex);
    if (cc->probe_in_progress) {
        if (scan_wifi) cc->probe_wifi_scan_requested = true;
        pthread_mutex_unlock(&cc->probe_mutex);
        return;
    }
    cc->probe_in_progress = true;
    cc_probe_worker_args_t *args = malloc(sizeof(*args));
    if (!args) {
        cc->probe_in_progress = false;
        pthread_mutex_unlock(&cc->probe_mutex);
        return;
    }
    args->cc = cc;
    args->page = cc->current_page;
    args->scan_wifi = scan_wifi || cc->probe_wifi_scan_requested;
    cc->probe_wifi_scan_requested = false;
    pthread_mutex_unlock(&cc->probe_mutex);

    pthread_t th;
    if (pthread_create(&th, NULL, cc_probe_worker_thread, args) == 0) {
        pthread_detach(th);
    } else {
        free(args);
        pthread_mutex_lock(&cc->probe_mutex);
        cc->probe_in_progress = false;
        pthread_mutex_unlock(&cc->probe_mutex);
    }
}

static void cc_arm_anim(aurade_control_center_t *cc);

static int cc_probe_event_handler(int fd, uint32_t mask, void *data) {
    (void)mask;
    aurade_control_center_t *cc = (aurade_control_center_t *)data;
    uint64_t val = 0;
    ssize_t r = read(fd, &val, sizeof(val));
    (void)r;

    if (!cc) return 0;

    cc_probe_snapshot_t snap;
    bool has_result = false;
    bool request_another = false;

    pthread_mutex_lock(&cc->probe_mutex);
    if (cc->probe_has_result) {
        snap = cc->probe_result;
        has_result = true;
        cc->probe_has_result = false;
    }
    cc->probe_in_progress = false;
    if (cc->probe_wifi_scan_requested) {
        request_another = true;
    }
    pthread_mutex_unlock(&cc->probe_mutex);

    if (has_result) {
        cc->wifi_enabled = snap.wifi_enabled;
        cc->wifi_available = snap.wifi_available;
        snprintf(cc->wifi_ssid, sizeof(cc->wifi_ssid), "%s",
                 snap.wifi_enabled ? snap.wifi_ssid : "");

        cc->bluetooth_enabled = snap.bluetooth_enabled;
        cc->bluetooth_available = snap.bluetooth_available;
        snprintf(cc->bluetooth_device, sizeof(cc->bluetooth_device), "%s",
                 snap.bluetooth_enabled ? snap.bluetooth_device : "");

        if (snap.dark_mode_available) {
            cc->dark_mode = snap.dark_mode;
            cc->dark_mode_available = true;
        }
        if (snap.dnd_available) {
            cc->dnd_enabled = snap.dnd_enabled;
            cc->dnd_available = true;
        }

        // Only update sliders if user is not actively dragging them
        if (cc->dragging != 1 && snap.brightness_available) {
            cc->brightness = snap.brightness;
            cc->brightness_available = true;
        }
        if (cc->dragging != 2 && snap.volume_available) {
            cc->volume = snap.volume;
            cc->volume_muted = snap.volume_muted;
            cc->volume_uses_pactl = snap.volume_uses_pactl;
            cc->volume_available = true;
        }

        cc->np_available = snap.np_available;
        cc->np_active = snap.np_active;
        cc->np_playing = snap.np_playing;
        snprintf(cc->np_title, sizeof(cc->np_title), "%s", snap.np_title);
        snprintf(cc->np_artist, sizeof(cc->np_artist), "%s", snap.np_artist);

        cc->battery_percent = snap.battery_percent;
        cc->battery_charging = snap.battery_charging;
        cc->battery_available = snap.battery_available;
        cc->hotspot_enabled = snap.hotspot_enabled;
        cc->hotspot_available = snap.hotspot_available;

        // Always update cached Wi-Fi and Bluetooth lists so subpages open in 0ms
        if (!snap.wifi_enabled) {
            cc->wifi_count = 0;
        } else if (snap.wifi_count > 0 || cc->wifi_count == 0) {
            cc->wifi_count = snap.wifi_count;
            memcpy(cc->wifi_items, snap.wifi_items, sizeof(cc->wifi_items));
        }

        if (!snap.bluetooth_enabled) {
            cc->bt_count = 0;
        } else {
            cc->bt_count = snap.bt_count;
            memcpy(cc->bt_items, snap.bt_items, sizeof(cc->bt_items));
        }

        if (cc->current_page == CC_PAGE_SOUND || snap.audio_output_count > 0) {
            cc->audio_output_count = snap.audio_output_count;
            memcpy(cc->audio_outputs, snap.audio_outputs, sizeof(cc->audio_outputs));
            cc->audio_input_count = snap.audio_input_count;
            memcpy(cc->audio_inputs, snap.audio_inputs, sizeof(cc->audio_inputs));
        }

        cc_arm_anim(cc);
        control_center_render(cc);
        if (cc->server) schedule_all_outputs(cc->server);
    }

    if (request_another) {
        cc_trigger_async_refresh(cc, true);
    }
    return 0;
}

static void refresh_wifi(aurade_control_center_t *cc) {
    cc_trigger_async_refresh(cc, false);
}

static void refresh_bluetooth(aurade_control_center_t *cc) {
    cc_trigger_async_refresh(cc, false);
}

static void refresh_sound(aurade_control_center_t *cc) {
    cc_trigger_async_refresh(cc, false);
}

static void cc_schedule_refresh(aurade_control_center_t *cc, int delay_ms);
static void cc_schedule_np(aurade_control_center_t *cc, int delay_ms);

static bool is_wifi_saved(const char *ssid) {
    if (!ssid || !*ssid) return false;
    char buf[4096] = {0};
    if (run_cmd_capture("nmcli -t -f NAME connection show 2>/dev/null", buf, sizeof(buf)) <= 0) {
        return false;
    }
    char *line = buf;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';
        if (strcmp(line, ssid) == 0) return true;
        line = next ? next + 1 : NULL;
    }
    return false;
}

static void connect_to_wifi(aurade_control_center_t *cc, const char *ssid, const char *password) {
    if (!ssid || !*ssid) return;

    unlink("/tmp/aurade_wifi_status");
    unlink("/tmp/aurade_wifi_result");

    char cmd[512];
    if (password && *password) {
        char esc_pass[256] = {0};
        char esc_ssid[128] = {0};
        for (int i = 0, j = 0; password[i] && j < 250; i++) {
            if (password[i] == '"' || password[i] == '\\' || password[i] == '$') esc_pass[j++] = '\\';
            esc_pass[j++] = password[i];
        }
        for (int i = 0, j = 0; ssid[i] && j < 120; i++) {
            if (ssid[i] == '"' || ssid[i] == '\\' || ssid[i] == '$') esc_ssid[j++] = '\\';
            esc_ssid[j++] = ssid[i];
        }
        snprintf(cmd, sizeof(cmd),
            "(nmcli dev wifi connect \"%s\" password \"%s\" >/tmp/aurade_wifi_result 2>&1; echo $? >/tmp/aurade_wifi_status) &",
            esc_ssid, esc_pass);
    } else {
        char esc_ssid[128] = {0};
        for (int i = 0, j = 0; ssid[i] && j < 120; i++) {
            if (ssid[i] == '"' || ssid[i] == '\\' || ssid[i] == '$') esc_ssid[j++] = '\\';
            esc_ssid[j++] = ssid[i];
        }
        snprintf(cmd, sizeof(cmd),
            "(nmcli dev wifi connect \"%s\" >/tmp/aurade_wifi_result 2>&1; echo $? >/tmp/aurade_wifi_status) &",
            esc_ssid);
    }

    cc->wifi_connecting = true;
    snprintf(cc->connecting_ssid, sizeof(cc->connecting_ssid), "%s", ssid);
    snprintf(cc->wifi_status_msg, sizeof(cc->wifi_status_msg), "Connecting to %s…", ssid);
    run_cmd_async(cmd);
    cc_arm_anim(cc);
    control_center_render(cc);
    cc_schedule_refresh(cc, 350);
}

static void connect_to_bluetooth(aurade_control_center_t *cc, const char *mac,
                                 const char *name, bool disconnect) {
    if (!cc || !mac || !*mac) return;

    unlink("/tmp/aurade_bt_status");
    unlink("/tmp/aurade_bt_result");

    char cmd[640];
    if (disconnect) {
        snprintf(cmd, sizeof(cmd),
            "(bluetoothctl disconnect %s >/tmp/aurade_bt_result 2>&1; echo $? >/tmp/aurade_bt_status) &",
            mac);
    } else {
        snprintf(cmd, sizeof(cmd),
            "(bluetoothctl trust %s >/dev/null 2>&1; "
            "bluetoothctl connect %s >/tmp/aurade_bt_result 2>&1 || "
            "(bluetoothctl pair %s >/dev/null 2>&1 && bluetoothctl connect %s >/tmp/aurade_bt_result 2>&1); "
            "echo $? >/tmp/aurade_bt_status) &",
            mac, mac, mac, mac);
    }

    cc->bt_connecting = true;
    cc->bt_disconnecting = disconnect;
    snprintf(cc->bt_connecting_mac, sizeof(cc->bt_connecting_mac), "%s", mac);
    snprintf(cc->bt_status_msg, sizeof(cc->bt_status_msg), "%s %s…",
             disconnect ? "Disconnecting" : "Connecting to",
             (name && *name) ? name : mac);

    run_cmd_async(cmd);
    cc_arm_anim(cc);
    control_center_render(cc);
    cc_schedule_refresh(cc, 350);
}

static void cc_refresh_all(aurade_control_center_t *cc) {
    if (!cc) return;
    cc_trigger_async_refresh(cc, false);
}

static int cc_refresh_timer_cb(void *data) {
    aurade_control_center_t *cc = data;

    if (cc->wifi_connecting) {
        FILE *fp = fopen("/tmp/aurade_wifi_status", "r");
        if (fp) {
            int exit_code = -1;
            if (fscanf(fp, "%d", &exit_code) == 1) {
                cc->wifi_connecting = false;
                if (exit_code == 0) {
                    cc->password_prompt_open = false;
                    if (cc->connecting_ssid[0]) {
                        snprintf(cc->wifi_ssid, sizeof(cc->wifi_ssid), "%s", cc->connecting_ssid);
                        for (int i = 0; i < cc->wifi_count; i++) {
                            cc->wifi_items[i].active = (strcmp(cc->wifi_items[i].ssid, cc->connecting_ssid) == 0);
                        }
                    }
                    cc->connecting_ssid[0] = '\0';
                    snprintf(cc->wifi_status_msg, sizeof(cc->wifi_status_msg), "Connected!");
                    cc_trigger_async_refresh(cc, false);
                } else {
                    char err[128] = {0};
                    FILE *rfp = fopen("/tmp/aurade_wifi_result", "r");
                    if (rfp) {
                        if (fgets(err, sizeof(err), rfp)) {
                            char *nl = strchr(err, '\n'); if (nl) *nl = '\0';
                        }
                        fclose(rfp);
                    }
                    if (strstr(err, "Secrets were required") != NULL ||
                        strstr(err, "secret") != NULL ||
                        strstr(err, "password") != NULL) {
                        if (!cc->password_prompt_open && cc->connecting_ssid[0]) {
                            cc->password_prompt_open = true;
                            cc->wifi_password[0] = '\0';
                            snprintf(cc->selected_ssid, sizeof(cc->selected_ssid), "%s", cc->connecting_ssid);
                            snprintf(cc->wifi_status_msg, sizeof(cc->wifi_status_msg), "Password required");
                        } else {
                            snprintf(cc->wifi_status_msg, sizeof(cc->wifi_status_msg), "Incorrect password");
                        }
                    } else if (err[0] != '\0') {
                        snprintf(cc->wifi_status_msg, sizeof(cc->wifi_status_msg), "%s", err);
                    } else {
                        snprintf(cc->wifi_status_msg, sizeof(cc->wifi_status_msg), "Connection failed");
                    }
                    cc->connecting_ssid[0] = '\0';
                }
            }
            fclose(fp);
            unlink("/tmp/aurade_wifi_status");
            unlink("/tmp/aurade_wifi_result");
            cc_arm_anim(cc);
            control_center_render(cc);
        }
    }

    if (cc->bt_connecting) {
        FILE *fp = fopen("/tmp/aurade_bt_status", "r");
        if (fp) {
            int exit_code = -1;
            if (fscanf(fp, "%d", &exit_code) == 1) {
                cc->bt_connecting = false;
                if (exit_code == 0) {
                    for (int i = 0; i < cc->bt_count; i++) {
                        if (strcmp(cc->bt_items[i].mac, cc->bt_connecting_mac) == 0) {
                            cc->bt_items[i].connected = !cc->bt_disconnecting;
                            if (!cc->bt_disconnecting) {
                                memcpy(cc->bluetooth_device, cc->bt_items[i].name, sizeof(cc->bluetooth_device));
                                cc->bluetooth_device[sizeof(cc->bluetooth_device) - 1] = '\0';
                            } else if (strcmp(cc->bluetooth_device, cc->bt_items[i].name) == 0) {
                                cc->bluetooth_device[0] = '\0';
                            }
                            break;
                        }
                    }
                    cc->bt_status_msg[0] = '\0';
                    cc_trigger_async_refresh(cc, false);
                } else {
                    snprintf(cc->bt_status_msg, sizeof(cc->bt_status_msg),
                             cc->bt_disconnecting ? "Failed to disconnect" : "Connection failed — ensure device is in pairing mode");
                }
                cc->bt_connecting_mac[0] = '\0';
            }
            fclose(fp);
            unlink("/tmp/aurade_bt_status");
            unlink("/tmp/aurade_bt_result");
            cc_arm_anim(cc);
            control_center_render(cc);
        }
    }

    cc_trigger_async_refresh(cc, false);

    if (cc->wifi_connecting || cc->bt_connecting || cc->wifi_scanning || cc->bt_scanning) {
        cc_schedule_refresh(cc, 400);
    } else if (cc->active) {
        cc_schedule_refresh(cc, 2000);
    } else {
        // Control Center is closed: keep background cache warm with low overhead
        cc_schedule_refresh(cc, 30000);
    }
    return 0;
}

static void cc_schedule_refresh(aurade_control_center_t *cc, int delay_ms) {
    if (!cc || !cc->server || !cc->server->wl_display) return;

    if (!cc->refresh_timer) {
        struct wl_event_loop *loop = wl_display_get_event_loop(cc->server->wl_display);
        cc->refresh_timer = wl_event_loop_add_timer(loop, cc_refresh_timer_cb, cc);
    }
    if (cc->refresh_timer) {
        wl_event_source_timer_update(cc->refresh_timer, delay_ms);
    }
}

static int cc_np_timer_cb(void *data) {
    aurade_control_center_t *cc = data;
    cc_trigger_async_refresh(cc, false);
    if (cc->active && cc->np_timer) {
        wl_event_source_timer_update(cc->np_timer, CC_NP_INTERVAL);
    }
    return 0;
}

static void cc_schedule_np(aurade_control_center_t *cc, int delay_ms) {
    if (!cc || !cc->np_available || !cc->server || !cc->server->wl_display) return;

    if (!cc->np_timer) {
        struct wl_event_loop *loop = wl_display_get_event_loop(cc->server->wl_display);
        cc->np_timer = wl_event_loop_add_timer(loop, cc_np_timer_cb, cc);
    }
    if (cc->np_timer) {
        wl_event_source_timer_update(cc->np_timer, delay_ms);
    }
}

static void apply_dark_mode(aurade_control_center_t *cc, bool dark) {
    if (!cc->dark_mode_available) return;

    char theme[128] = {0};
    char target[160] = {0};

    if (capture_command("gsettings get org.gnome.desktop.interface gtk-theme 2>/dev/null",
                        theme, sizeof(theme))) {
        unquote(theme);
        const char *needle = dark ? "-Light" : "-Dark";
        const char *replacement = dark ? "-Dark" : "-Light";
        char *pos = strstr(theme, needle);
        if (pos) {
            snprintf(target, sizeof(target), "%.*s%s%s",
                     (int)(pos - theme), theme, replacement, pos + strlen(needle));
        }
    }

    app_launch_command(dark
        ? "gsettings set org.gnome.desktop.interface color-scheme 'prefer-dark'"
        : "gsettings set org.gnome.desktop.interface color-scheme 'prefer-light'");

    bool safe = target[0] != '\0';
    for (const char *p = target; safe && *p; p++) {
        if (!isalnum((unsigned char)*p) && strchr("._+-", *p) == NULL) safe = false;
    }
    if (safe) {
        char theme_path[256];
        snprintf(theme_path, sizeof(theme_path), "/usr/share/themes/%s", target);
        if (access(theme_path, R_OK) == 0) {
            char command[384];
            snprintf(command, sizeof(command),
                     "gsettings set org.gnome.desktop.interface gtk-theme '%s'", target);
            app_launch_command(command);
        }
    }

    cc_schedule_refresh(cc, 250);
}

static void apply_dnd(aurade_control_center_t *cc, bool enabled) {
    if (!cc->dnd_available) return;
    app_launch_command(enabled
        ? "gsettings set org.gnome.desktop.notifications show-banners false"
        : "gsettings set org.gnome.desktop.notifications show-banners true");
    cc_schedule_refresh(cc, 200);
}

static void apply_wifi(aurade_control_center_t *cc, bool enabled) {
    if (!cc->wifi_available) return;
    app_launch_command(enabled ? "nmcli radio wifi on" : "nmcli radio wifi off");
    cc_schedule_refresh(cc, 700);
}

static void apply_bluetooth(aurade_control_center_t *cc, bool enabled) {
    if (!cc->bluetooth_available) return;
    app_launch_command(enabled ? "bluetoothctl power on" : "bluetoothctl power off");
    cc_schedule_refresh(cc, 700);
}

static void apply_brightness(aurade_control_center_t *cc) {
    if (!cc->brightness_available) return;
    int pct = (int)lround(cc->brightness * 100.0);
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    char command[128];
    snprintf(command, sizeof(command), "brightnessctl set %d%%", pct);
    app_launch_command(command);
    if (cc->server) {
        session_state_set_brightness(&cc->server->state, cc->brightness);
    }
    cc_schedule_refresh(cc, 300);
}

static void apply_volume(aurade_control_center_t *cc) {
    if (!cc->volume_available) return;
    int pct = (int)lround(cc->volume * 100.0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    char command[320];
    if (cc->volume_uses_pactl) {
        snprintf(command, sizeof(command),
                 "pactl set-sink-mute @DEFAULT_SINK@ 0; "
                 "pactl set-sink-volume @DEFAULT_SINK@ %d%%", pct);
    } else {
        snprintf(command, sizeof(command),
                 "wpctl set-mute @DEFAULT_AUDIO_SINK@ 0; "
                 "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%", pct);
    }
    cc->volume_muted = false;
    app_launch_command(command);
    if (cc->server) {
        session_state_set_sink_volume(&cc->server->state, cc->server->current_audio_sink, cc->volume, false);
    }
    cc_schedule_refresh(cc, 220);
}

static void toggle_mute(aurade_control_center_t *cc) {
    if (!cc->volume_available) return;

    if (cc->volume_muted) {
        if (cc->volume < 0.02) cc->volume = 0.30;
        apply_volume(cc);
    } else {
        cc->volume_muted = true;
        if (cc->volume_uses_pactl) {
            app_launch_command("pactl set-sink-mute @DEFAULT_SINK@ 1");
        } else {
            app_launch_command("wpctl set-mute @DEFAULT_AUDIO_SINK@ 1");
        }
        if (cc->server) {
            session_state_set_sink_volume(&cc->server->state, cc->server->current_audio_sink, cc->volume, true);
        }
        cc_schedule_refresh(cc, 220);
    }
}

static void set_slider_from_x(aurade_control_center_t *cc, double panel_x, int slider) {
    cc_layout_t L = cc_layout(cc);
    cc_rect_t tr = slider == 1 ? L.display_track : L.sound_track;
    if (tr.w <= 0.0) return;

    double value = (panel_x - tr.x) / tr.w;
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;

    if (slider == 1) {
        cc->brightness = value;
    } else {
        cc->volume = value;
    }
}

static void dismiss_control_center(aurade_control_center_t *cc) {
    if (!cc || !cc->server) return;
    server_handle_gesture_begin(cc->server, ACTION_CONTROL_CENTER);
    server_handle_gesture_update(cc->server, 350.0, 0, 1.0);
    server_handle_gesture_end(cc->server, false, 500.0, 0);
}

static void media_command(aurade_control_center_t *cc, const char *command) {
    app_launch_command(command);
    cc_schedule_np(cc, 250);
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

static int cc_module_at(const cc_layout_t *L, double x, double y) {
    if (x < 0 || y < 0 || x > CC_WIDTH || y > L->panel_h) return CC_MODULE_NONE;

    if (cc_hit(x, y, L->conn)) {
        if (cc_hit(x, y, L->wifi_badge)) return CC_MODULE_WIFI_TOGGLE;
        if (cc_hit(x, y, L->wifi_row)) return CC_MODULE_WIFI;
        if (cc_hit(x, y, L->bt_badge)) return CC_MODULE_BLUETOOTH_TOGGLE;
        if (cc_hit(x, y, L->bt_row)) return CC_MODULE_BLUETOOTH;
        if (cc_hit(x, y, L->hotspot_badge) || cc_hit(x, y, L->hotspot_row)) return CC_MODULE_HOTSPOT;
        return CC_MODULE_NONE;
    }

    if (cc_hit(x, y, L->focus)) return CC_MODULE_FOCUS;

    if (cc_hit(x, y, L->aura_connect_tile)) return CC_MODULE_AURA_CONNECT;
    if (cc_hit(x, y, L->screen_kbd_tile)) return CC_MODULE_SCREEN_KEYBOARD;

    if (cc_hit(x, y, L->display_detail)) return CC_MODULE_DISPLAY_DETAIL;
    if (cc_hit(x, y, L->display_track) || cc_hit(x, y, L->display)) return CC_MODULE_DISPLAY;

    if (cc_hit(x, y, L->sound_detail)) return CC_MODULE_SOUND_DETAIL;
    if (cc_hit(x, y, L->sound_track) || cc_hit(x, y, L->sound)) return CC_MODULE_SOUND;

    if (L->show_now_playing && cc_hit(x, y, L->np)) {
        if (cc_hit(x, y, L->np_prev)) return CC_MODULE_PREV;
        if (cc_hit(x, y, L->np_play)) return CC_MODULE_PLAY;
        if (cc_hit(x, y, L->np_next)) return CC_MODULE_NEXT;
        return CC_MODULE_NP_CARD;
    }

    if (cc_hit(x, y, L->footer_battery)) return CC_MODULE_BATTERY;
    if (cc_hit(x, y, L->footer_user)) return CC_MODULE_USER;
    if (cc_hit(x, y, L->footer_accessibility)) return CC_MODULE_ACCESSIBILITY;
    if (cc_hit(x, y, L->footer_settings)) return CC_MODULE_SETTINGS;

    return CC_MODULE_NONE;
}

static void cc_to_panel_coords(const aurade_control_center_t *cc, double x, double y,
                               double *px, double *py) {
    double top = TOPBAR_HEIGHT + CC_MARGIN_TOP;
    if (px) *px = x - ((cc->screen_width - CC_MARGIN_RIGHT) - CC_WIDTH);
    if (py) *py = y - top;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool control_center_is_open(const aurade_control_center_t *cc) {
    return cc && cc->active;
}

int control_center_width(const aurade_control_center_t *cc) {
    (void)cc;
    return (int)lround(CC_WIDTH + 2.0 * CC_SHADOW_PAD);
}

int control_center_height(const aurade_control_center_t *cc) {
    if (!cc) return (int)lround(CC_MAX_PANEL_H + 2.0 * CC_SHADOW_PAD);
    if (cc->anim_panel_h > 100.0) {
        return (int)lround(cc->anim_panel_h + 2.0 * CC_SHADOW_PAD);
    }
    cc_layout_t L = cc_layout(cc);
    return (int)lround(L.panel_h + 2.0 * CC_SHADOW_PAD);
}

static bool playerctl_available(void) {
    return (access("/usr/bin/playerctl", X_OK) == 0 ||
            access("/bin/playerctl", X_OK) == 0 ||
            access("/usr/local/bin/playerctl", X_OK) == 0);
}

static int cc_anim_tick(void *data) {
    aurade_control_center_t *cc = data;
    if (!cc) return 0;

    bool more = false;
    uint64_t now = cc_now_ms();

    if (cc->wifi_scanning && now >= cc->wifi_scan_until_ms) {
        cc->wifi_scanning = false;
        cc_trigger_async_refresh(cc, false);
    }
    if (cc->bt_scanning && now >= cc->bt_scan_until_ms) {
        cc->bt_scanning = false;
        cc_trigger_async_refresh(cc, false);
    }

    // 1. Page transition animation
    if (cc->page_anim < 1.0) {
        cc->page_anim += 0.14;
        if (cc->page_anim >= 1.0) {
            cc->page_anim = 1.0;
            cc->prev_page = cc->current_page;
        } else {
            more = true;
        }
    }

    // 2. Smooth panel height morphing
    double target_h = cc_layout(cc).panel_h;
    if (cc->anim_panel_h <= 0.0) {
        cc->anim_panel_h = target_h;
    } else if (fabs(cc->anim_panel_h - target_h) > 0.6) {
        cc->anim_panel_h += (target_h - cc->anim_panel_h) * 0.30;
        more = true;
    } else {
        cc->anim_panel_h = target_h;
    }

    // 3. Toggle switch thumb animations
    double target_wifi_tog = cc->wifi_enabled ? 1.0 : 0.0;
    if (fabs(cc->wifi_toggle_pos - target_wifi_tog) > 0.01) {
        cc->wifi_toggle_pos += (target_wifi_tog - cc->wifi_toggle_pos) * 0.30;
        more = true;
    } else {
        cc->wifi_toggle_pos = target_wifi_tog;
    }

    double target_bt_tog = cc->bluetooth_enabled ? 1.0 : 0.0;
    if (fabs(cc->bt_toggle_pos - target_bt_tog) > 0.01) {
        cc->bt_toggle_pos += (target_bt_tog - cc->bt_toggle_pos) * 0.30;
        more = true;
    } else {
        cc->bt_toggle_pos = target_bt_tog;
    }

    // 4. Row hover fade animations
    for (int i = 0; i < 24; i++) {
        double target_a = (cc->sub_hover_row == i) ? 1.0 : 0.0;
        if (fabs(cc->row_hover_alpha[i] - target_a) > 0.02) {
            cc->row_hover_alpha[i] += (target_a - cc->row_hover_alpha[i]) * 0.34;
            more = true;
        } else {
            cc->row_hover_alpha[i] = target_a;
        }
    }

    // 5. Activity spinner rotation
    if (cc->wifi_connecting || cc->bt_connecting || cc->wifi_scanning || cc->bt_scanning) {
        cc->spinner_phase += 0.36;
        if (cc->spinner_phase > 2.0 * M_PI) cc->spinner_phase -= 2.0 * M_PI;
        more = true;
    }

    if (cc->active) {
        control_center_render(cc);
        if (cc->server) schedule_all_outputs(cc->server);
    }

    if (more && cc->anim_timer) {
        wl_event_source_timer_update(cc->anim_timer, 16);
    }
    return 0;
}

static void cc_arm_anim(aurade_control_center_t *cc) {
    if (!cc || !cc->server || !cc->server->wl_display) return;
    if (!cc->anim_timer) {
        struct wl_event_loop *loop = wl_display_get_event_loop(cc->server->wl_display);
        cc->anim_timer = wl_event_loop_add_timer(loop, cc_anim_tick, cc);
    }
    if (cc->anim_timer) {
        wl_event_source_timer_update(cc->anim_timer, 16);
    }
}

static void cc_navigate_to(aurade_control_center_t *cc, cc_page_t page) {
    if (!cc || cc->current_page == page) return;
    cc->prev_page = cc->current_page;
    cc->current_page = page;
    cc->page_anim = 0.0;
    cc->page_anim_dir = (page == CC_PAGE_MAIN) ? -1 : 1;
    cc->sub_hover_row = -1;
    cc->sub_hover_back = false;
    cc->sub_hover_toggle = false;
    cc->sub_hover_scan = false;
    cc->sub_hover_settings = false;
    for (int i = 0; i < 24; i++) cc->row_hover_alpha[i] = 0.0;
    cc_arm_anim(cc);
}

aurade_control_center_t *control_center_create(aurade_server_t *server) {
    aurade_control_center_t *cc = calloc(1, sizeof(*cc));
    if (!cc) return NULL;
    cc->server = server;
    cc->screen_width = server->screen_width > 0 ? server->screen_width : 1920;
    cc->screen_height = server->screen_height > 0 ? server->screen_height : 1080;
    cc->progress = 0.0;
    cc->active = false;
    cc->hover = CC_MODULE_NONE;
    cc->pressed = CC_MODULE_NONE;
    cc->sub_hover_row = -1;

    pthread_mutex_init(&cc->probe_mutex, NULL);
    cc->probe_event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (cc->probe_event_fd >= 0 && server && server->wl_display) {
        struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
        cc->probe_event_source = wl_event_loop_add_fd(loop, cc->probe_event_fd, WL_EVENT_READABLE,
                                                      cc_probe_event_handler, cc);
    }

    cc->wifi_available = true;
    cc->bluetooth_available = true;
    cc->dark_mode_available = true;
    cc->dnd_available = true;
    cc->brightness_available = true;
    cc->volume_available = true;
    cc->wifi_enabled = true;
    cc->bluetooth_enabled = false;
    cc->dark_mode = true;
    cc->dnd_enabled = false;
    cc->brightness = 0.85;
    cc->volume = 0.70;
    cc->battery_percent = 100;
    cc->battery_charging = false;
    cc->battery_available = true;
    cc->hotspot_available = true;
    cc->hotspot_enabled = false;
    cc->aura_connect_enabled = true;
    cc->screen_keyboard_enabled = false;
    cc->wifi_ssid[0] = '\0';
    cc->bluetooth_device[0] = '\0';
    cc->np_available = playerctl_available();

    // Fast direct sysfs read for initial brightness
    read_sysfs_brightness(&cc->brightness);

    // Populate initial Wi-Fi & Bluetooth state immediately (~25ms total) so Control Center
    // never shows placeholder text and has all connected devices & lists ready from frame 0.
    cc_probe_snapshot_t init_snap;
    memset(&init_snap, 0, sizeof(init_snap));
    probe_worker_wifi(&init_snap, true, false);
    probe_worker_bluetooth(&init_snap, true);
    probe_worker_battery(&init_snap);
    cc->wifi_available = init_snap.wifi_available;
    cc->wifi_enabled = init_snap.wifi_enabled;
    snprintf(cc->wifi_ssid, sizeof(cc->wifi_ssid), "%s", init_snap.wifi_ssid);
    cc->wifi_count = init_snap.wifi_count;
    memcpy(cc->wifi_items, init_snap.wifi_items, sizeof(cc->wifi_items));

    cc->bluetooth_available = init_snap.bluetooth_available;
    cc->bluetooth_enabled = init_snap.bluetooth_enabled;
    snprintf(cc->bluetooth_device, sizeof(cc->bluetooth_device), "%s", init_snap.bluetooth_device);
    cc->bt_count = init_snap.bt_count;
    memcpy(cc->bt_items, init_snap.bt_items, sizeof(cc->bt_items));

    if (init_snap.battery_available) {
        cc->battery_available = true;
        cc->battery_percent = init_snap.battery_percent;
        cc->battery_charging = init_snap.battery_charging;
    }

    cc->page_anim = 1.0;
    cc->prev_page = CC_PAGE_MAIN;
    cc->wifi_toggle_pos = cc->wifi_enabled ? 1.0 : 0.0;
    cc->bt_toggle_pos = cc->bluetooth_enabled ? 1.0 : 0.0;
    cc->anim_panel_h = cc_layout(cc).panel_h;

    cc->buffer = cairo_buffer_create(control_center_width(cc),
                                     (int)lround(CC_MAX_PANEL_H + 2.0 * CC_SHADOW_PAD));
    cc->scene_buffer = wlr_scene_buffer_create(server->overlay_tree, &cc->buffer->base);
    wlr_scene_node_set_enabled(&cc->scene_buffer->node, false);

    control_center_render(cc);
    cc_schedule_refresh(cc, 100);
    return cc;
}

void control_center_resize(aurade_control_center_t *cc, int width, int height) {
    if (!cc) return;
    cc->screen_width = width;
    cc->screen_height = height;
    if (cc->active) control_center_update_progress(cc, cc->progress);
}

static void cc_apply_transform(aurade_control_center_t *cc, double p);

void control_center_begin(aurade_control_center_t *cc) {
    if (!cc) return;
    cc->active = true;
    cc->hover = CC_MODULE_NONE;
    cc->pressed = CC_MODULE_NONE;
    cc->anim_panel_h = cc_layout(cc).panel_h;
    wlr_scene_node_set_enabled(&cc->scene_buffer->node, true);
    // Place the panel at progress 0 before the first frame so it can never
    // flash at the scene origin when a gesture starts.
    cc_apply_transform(cc, 0.0);
    if (cc->server && cc->server->topbar) topbar_render(cc->server->topbar);

    // Cached state is already rendered immediately; trigger background refresh
    // to catch any changes since the last background poll.
    cc_trigger_async_refresh(cc, false);
    cc_schedule_refresh(cc, 1500);
    cc_schedule_np(cc, 120);
}

/* Scale / fade / anchor the panel for a gesture progress in [0, 1]. macOS drops
 * the drawer in: a slight scale-up anchored at the top-right corner combined
 * with a fade. Both are linear in the finger position, so direct manipulation
 * still feels 1:1. */
static void cc_apply_transform(aurade_control_center_t *cc, double p) {
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;

    double scale = 0.94 + 0.06 * p;
    double opacity = p * 2.0;
    if (opacity > 1.0) opacity = 1.0;

    int bw = control_center_width(cc);
    int bh = (int)lround(CC_MAX_PANEL_H + 2.0 * CC_SHADOW_PAD);
    wlr_scene_buffer_set_dest_size(cc->scene_buffer, (int)lround(bw * scale),
                                   (int)lround(bh * scale));
    wlr_scene_buffer_set_opacity(cc->scene_buffer, (float)opacity);

    double node_x = (cc->screen_width - CC_MARGIN_RIGHT) - scale * (CC_SHADOW_PAD + CC_WIDTH);
    double node_y = (TOPBAR_HEIGHT + CC_MARGIN_TOP) - scale * CC_SHADOW_PAD;
    wlr_scene_node_set_position(&cc->scene_buffer->node, (int)lround(node_x),
                                (int)lround(node_y));
}

void control_center_update_progress(aurade_control_center_t *cc, double progress) {
    if (!cc || !cc->active) return;
    cc->progress = progress;

    if (progress <= 0.001) {
        wlr_scene_node_set_enabled(&cc->scene_buffer->node, false);
        wlr_scene_buffer_set_opacity(cc->scene_buffer, 1.0f);
        cc->active = false;
        cc->hover = CC_MODULE_NONE;
        cc->pressed = CC_MODULE_NONE;
        cc->dragging = 0;
        if (cc->np_timer) wl_event_source_timer_update(cc->np_timer, 0);
        if (cc->server && cc->server->topbar) topbar_render(cc->server->topbar);
        return;
    }

    wlr_scene_node_set_enabled(&cc->scene_buffer->node, true);
    cc_apply_transform(cc, progress);
    control_center_render(cc);
}

void control_center_end(aurade_control_center_t *cc, bool opened) {
    if (!cc) return;
    cc->active = opened;
    cc->progress = opened ? cc->progress : 0.0;

    if (!opened) {
        // Keep background refresh running every 3.5s so state is always instant on open
        cc_schedule_refresh(cc, 3500);
        if (cc->np_timer) wl_event_source_timer_update(cc->np_timer, 0);
        cc->hover = CC_MODULE_NONE;
        cc->pressed = CC_MODULE_NONE;
        cc->dragging = 0;
        cc->current_page = CC_PAGE_MAIN;
        cc->prev_page = CC_PAGE_MAIN;
        cc->page_anim = 1.0;
        cc->password_prompt_open = false;
        cc->wifi_password[0] = '\0';
        cc->wifi_status_msg[0] = '\0';
        cc->bt_status_msg[0] = '\0';
        cc->sub_dragging_slider = false;
        cc->sub_hover_row = -1;
        cc->anim_panel_h = cc_layout(cc).panel_h;
        wlr_scene_node_set_enabled(&cc->scene_buffer->node, false);
        wlr_scene_buffer_set_opacity(cc->scene_buffer, 1.0f);
        if (cc->server && cc->server->topbar) topbar_render(cc->server->topbar);
    } else {
        cc_schedule_refresh(cc, 1500);
        cc_schedule_np(cc, 120);
    }
}

static void cc_draw_checkmark(cairo_t *cr, double cx, double cy, double size, cc_color_t col) {
    cairo_save(cr);
    cc_set(cr, col);
    cairo_set_line_width(cr, 1.8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - size * 0.4, cy);
    cairo_line_to(cr, cx - size * 0.1, cy + size * 0.35);
    cairo_line_to(cr, cx + size * 0.45, cy - size * 0.35);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void cc_draw_lock_icon(cairo_t *cr, double cx, double cy, double size, cc_color_t col) {
    cairo_save(cr);
    cc_set(cr, col);
    double bw = size * 0.75, bh = size * 0.55;
    draw_rounded_rect(cr, cx - bw / 2.0, cy - bh / 2.0 + 2.0, bw, bh, 2.0);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.3);
    cairo_arc(cr, cx, cy - bh / 2.0 + 2.0, size * 0.24, M_PI, 0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void cc_draw_toggle_switch(cairo_t *cr, double x, double y, double w, double h,
                                  double pos, bool hover) {
    cairo_save(cr);
    if (pos < 0.0) pos = 0.0;
    if (pos > 1.0) pos = 1.0;
    double r = h / 2.0;
    draw_rounded_rect(cr, x, y, w, h, r);
    double tr_r = 0.45 * (1.0 - pos) + 0.0 * pos;
    double tr_g = 0.45 * (1.0 - pos) + 0.48 * pos;
    double tr_b = 0.48 * (1.0 - pos) + 1.0 * pos;
    double tr_a = (hover ? 0.75 : 0.60) * (1.0 - pos) + (hover ? 1.0 : 0.94) * pos;
    cairo_set_source_rgba(cr, tr_r, tr_g, tr_b, tr_a);
    cairo_fill(cr);

    double knob_r = r - 2.0;
    double knob_cx = (x + r) + pos * (w - 2.0 * r);
    double knob_cy = y + r;
    cairo_arc(cr, knob_cx, knob_cy, knob_r, 0, 2.0 * M_PI);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void cc_draw_back_button(cairo_t *cr, double x, double y, double w, double h,
                                bool hover, const cc_theme_t *t) {
    cairo_save(cr);
    draw_rounded_rect(cr, x, y, w, h, h / 2.0);
    if (hover) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
    }
    cairo_fill(cr);

    double cx = x + 12.0;
    double cy = y + h / 2.0;
    cairo_set_line_width(cr, 1.8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90);
    cairo_move_to(cr, cx + 2.0, cy - 4.5);
    cairo_line_to(cr, cx - 2.5, cy);
    cairo_line_to(cr, cx + 2.0, cy + 4.5);
    cairo_stroke(cr);

    cc_text_at(cr, "Back", 11.5, true, t->text, cx + 8.0, cy, false, true);
    cairo_restore(cr);
}

static void cc_draw_scan_button(cairo_t *cr, cc_sub_rect_t r, bool scanning,
                                bool hover, double spinner_phase, const cc_theme_t *t) {
    cairo_save(cr);
    draw_rounded_rect(cr, r.x, r.y, r.w, r.h, r.h / 2.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, hover ? 0.18 : 0.08);
    cairo_fill(cr);

    double cx = r.x + r.w / 2.0;
    double cy = r.y + r.h / 2.0;
    if (scanning) {
        cc_draw_spinner(cr, cx, cy, 6.5, spinner_phase, t->accent);
    } else {
        // Draw circular refresh arrow icon
        cc_set(cr, t->text_dim);
        cairo_set_line_width(cr, 1.5);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_arc(cr, cx, cy, 5.2, -0.3 * M_PI, 1.35 * M_PI);
        cairo_stroke(cr);
        double ax = cx + 5.2 * cos(-0.3 * M_PI);
        double ay = cy + 5.2 * sin(-0.3 * M_PI);
        cairo_move_to(cr, ax - 3.0, ay - 1.0);
        cairo_line_to(cr, ax + 0.5, ay + 1.5);
        cairo_line_to(cr, ax + 2.2, ay - 2.2);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void cc_draw_wifi_page(cairo_t *cr, const cc_layout_t *L, const cc_theme_t *t,
                              aurade_control_center_t *cc) {
    (void)L;
    double cur_y = CC_PAD;

    cc->sub_back_rect = (cc_sub_rect_t){CC_PAD, cur_y, 64.0, 26.0};
    cc_draw_back_button(cr, cc->sub_back_rect.x, cc->sub_back_rect.y,
                        cc->sub_back_rect.w, cc->sub_back_rect.h,
                        cc->sub_hover_back, t);

    cc_text_at(cr, "Wi-Fi", 14.0, true, t->text, CC_WIDTH / 2.0, cur_y + 13.0, true, true);

    cc->sub_scan_rect = (cc_sub_rect_t){CC_WIDTH - CC_PAD - 70.0, cur_y + 1.0, 24.0, 24.0};
    if (cc->wifi_enabled) {
        cc_draw_scan_button(cr, cc->sub_scan_rect, cc->wifi_scanning,
                            cc->sub_hover_scan, cc->spinner_phase, t);
    }

    cc->sub_toggle_rect = (cc_sub_rect_t){CC_WIDTH - CC_PAD - 38.0, cur_y + 2.0, 38.0, 22.0};
    cc_draw_toggle_switch(cr, cc->sub_toggle_rect.x, cc->sub_toggle_rect.y,
                          cc->sub_toggle_rect.w, cc->sub_toggle_rect.h,
                          cc->wifi_toggle_pos, cc->sub_hover_toggle);
    cur_y += 38.0;

    if (!cc->wifi_enabled) {
        cc_rect_t off_card = {CC_PAD, cur_y, CC_CONTENT_W, 64.0};
        cc_card(cr, off_card, t->card, t->card_border);
        cc_text_at(cr, "Wi-Fi is turned off", 12.5, false, t->text_dim,
                   CC_WIDTH / 2.0, cur_y + 32.0, true, true);
        return;
    }

    if (cc->password_prompt_open) {
        double pw_card_h = 116.0;
        cc_rect_t pw_card = {CC_PAD, cur_y, CC_CONTENT_W, pw_card_h};
        cc_card(cr, pw_card, t->card_pressed, t->card_border);

        char prompt_title[96];
        snprintf(prompt_title, sizeof(prompt_title), "Enter password for \"%s\"", cc->selected_ssid);
        cc_text_fit(cr, prompt_title, 11.5, true, t->text, CC_PAD + 14.0, cur_y + 16.0, CC_CONTENT_W - 28.0);

        cc->pw_input_rect = (cc_sub_rect_t){CC_PAD + 12.0, cur_y + 30.0, CC_CONTENT_W - 24.0, 32.0};
        draw_rounded_rect(cr, cc->pw_input_rect.x, cc->pw_input_rect.y,
                          cc->pw_input_rect.w, cc->pw_input_rect.h, 8.0);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.85);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);

        if (cc->wifi_password[0] == '\0') {
            cc_text_at(cr, "Password", 12.0, false, cc_fade(t->text, 0.35),
                       cc->pw_input_rect.x + 10.0, cc->pw_input_rect.y + 16.0, false, true);
        } else {
            size_t pass_len = strlen(cc->wifi_password);
            char bullets[128] = {0};
            for (size_t b = 0; b < pass_len && b < 40; b++) {
                strcat(bullets, "•");
            }
            cc_text_at(cr, bullets, 15.0, true, t->text,
                       cc->pw_input_rect.x + 10.0, cc->pw_input_rect.y + 16.0, false, true);
            double bw = cc_text_w(cr, bullets, 15.0, true);
            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.95);
            cairo_set_line_width(cr, 1.8);
            cairo_move_to(cr, cc->pw_input_rect.x + 10.0 + bw + 2.0, cc->pw_input_rect.y + 8.0);
            cairo_line_to(cr, cc->pw_input_rect.x + 10.0 + bw + 2.0, cc->pw_input_rect.y + 24.0);
            cairo_stroke(cr);
        }

        if (cc->wifi_status_msg[0] != '\0') {
            bool is_err = (strstr(cc->wifi_status_msg, "fail") != NULL ||
                           strstr(cc->wifi_status_msg, "Incorrect") != NULL ||
                           strstr(cc->wifi_status_msg, "required") != NULL);
            cc_color_t status_col = is_err ? cc_rgba(1.0, 0.40, 0.40, 0.95) : t->accent;
            cc_text_fit(cr, cc->wifi_status_msg, 10.5, false, status_col,
                        CC_PAD + 14.0, cur_y + 86.0, CC_CONTENT_W - 148.0);
        }

        cc->pw_cancel_rect = (cc_sub_rect_t){CC_PAD + CC_CONTENT_W - 138.0, cur_y + 72.0, 62.0, 26.0};
        draw_rounded_rect(cr, cc->pw_cancel_rect.x, cc->pw_cancel_rect.y,
                          cc->pw_cancel_rect.w, cc->pw_cancel_rect.h, 7.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, cc->sub_hover_cancel ? 0.20 : 0.10);
        cairo_fill(cr);
        cc_text_at(cr, "Cancel", 11.5, false, t->text,
                   cc->pw_cancel_rect.x + cc->pw_cancel_rect.w / 2.0,
                   cc->pw_cancel_rect.y + cc->pw_cancel_rect.h / 2.0, true, true);

        cc->pw_connect_rect = (cc_sub_rect_t){CC_PAD + CC_CONTENT_W - 68.0, cur_y + 72.0, 58.0, 26.0};
        draw_rounded_rect(cr, cc->pw_connect_rect.x, cc->pw_connect_rect.y,
                          cc->pw_connect_rect.w, cc->pw_connect_rect.h, 7.0);
        if (cc->wifi_connecting) {
            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.55);
        } else {
            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, cc->sub_hover_connect ? 1.0 : 0.90);
        }
        cairo_fill(cr);
        if (cc->wifi_connecting) {
            cc_draw_spinner(cr, cc->pw_connect_rect.x + cc->pw_connect_rect.w / 2.0,
                            cc->pw_connect_rect.y + cc->pw_connect_rect.h / 2.0,
                            6.5, cc->spinner_phase, cc_rgba(1.0, 1.0, 1.0, 0.95));
        } else {
            cc_text_at(cr, "Join", 11.5, true, cc_rgba(1.0, 1.0, 1.0, 1.0),
                       cc->pw_connect_rect.x + cc->pw_connect_rect.w / 2.0,
                       cc->pw_connect_rect.y + cc->pw_connect_rect.h / 2.0, true, true);
        }

        cur_y += pw_card_h + 10.0;
    }

    cc_text_at(cr, cc->wifi_items[0].active ? "CONNECTED & AVAILABLE NETWORKS" : "AVAILABLE NETWORKS",
               10.0, true, t->text_faint, CC_PAD + 4.0, cur_y + 8.0, false, true);
    cur_y += 20.0;

    int max_show = cc->password_prompt_open ? 5 : 8;
    for (int i = 0; i < cc->wifi_count && i < max_show; i++) {
        double rh = 38.0;
        cc->wifi_items[i].y = cur_y;
        cc->wifi_items[i].h = rh;

        bool active = cc->wifi_items[i].active;
        bool is_connecting = (cc->wifi_connecting &&
                              strcmp(cc->connecting_ssid, cc->wifi_items[i].ssid) == 0);
        double hov_a = cc->row_hover_alpha[i];
        if (cc->sub_hover_row == i && hov_a < 0.25) hov_a = 0.25;

        if (hov_a > 0.01 || active || is_connecting) {
            draw_rounded_rect(cr, CC_PAD, cur_y + 1.0, CC_CONTENT_W, rh - 2.0, 10.0);
            if (active) {
                cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.14 + 0.08 * hov_a);
            } else {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.13 * hov_a);
            }
            cairo_fill(cr);
        }

        // Circular icon badge (macOS Control Center style)
        double bcx = CC_PAD + 18.0;
        double bcy = cur_y + rh / 2.0;
        draw_circle(cr, bcx, bcy, 12.5);
        if (active || is_connecting) {
            cc_set(cr, t->accent_blue);
        } else {
            cc_set(cr, t->badge_off);
        }
        cairo_fill(cr);
        draw_wifi_icon(cr, bcx, bcy, 13.0, (active || is_connecting) ? 1.0 : 0.88);

        cc_color_t name_color = active ? t->text : t->text;
        cc_text_fit(cr, cc->wifi_items[i].ssid, 12.2, active || is_connecting,
                    name_color, CC_PAD + 38.0, cur_y + (active || is_connecting ? 13.0 : rh / 2.0),
                    CC_CONTENT_W - 108.0);

        if (is_connecting) {
            cc_text_at(cr, "Connecting…", 9.8, false, t->accent,
                       CC_PAD + 38.0, cur_y + 27.0, false, true);
        } else if (active) {
            cc_text_at(cr, "Connected", 9.8, false, t->accent,
                       CC_PAD + 38.0, cur_y + 27.0, false, true);
        }

        if (is_connecting) {
            cc_draw_spinner(cr, CC_PAD + CC_CONTENT_W - 18.0, bcy, 7.0, cc->spinner_phase, t->accent);
        } else if (active) {
            if (cc->wifi_items[i].secure) {
                cc_draw_lock_icon(cr, CC_PAD + CC_CONTENT_W - 36.0, bcy, 11.5, t->text_dim);
            }
            cc_draw_checkmark(cr, CC_PAD + CC_CONTENT_W - 16.0, bcy, 12.0, t->accent);
        } else {
            if (hov_a > 0.05) {
                double pill_w = 44.0, pill_h = 22.0;
                double px = CC_PAD + CC_CONTENT_W - pill_w - 8.0;
                double py = bcy - pill_h / 2.0;
                draw_rounded_rect(cr, px, py, pill_w, pill_h, pill_h / 2.0);
                cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.85 * hov_a);
                cairo_fill(cr);
                cc_text_at(cr, "Join", 10.5, true, cc_rgba(1.0, 1.0, 1.0, hov_a),
                           px + pill_w / 2.0, bcy, true, true);
                if (cc->wifi_items[i].secure) {
                    cc_draw_lock_icon(cr, px - 12.0, bcy, 11.5, t->text_dim);
                }
            } else if (cc->wifi_items[i].secure) {
                cc_draw_lock_icon(cr, CC_PAD + CC_CONTENT_W - 18.0, bcy, 11.5, t->text_dim);
            }
        }

        cur_y += rh;
    }

    if (cc->wifi_count == 0) {
        if (cc->wifi_scanning) {
            cc_draw_spinner(cr, CC_WIDTH / 2.0 - 56.0, cur_y + 18.0, 6.5, cc->spinner_phase, t->accent);
            cc_text_at(cr, "Scanning for networks…", 12.0, false, t->text_dim,
                       CC_WIDTH / 2.0 + 8.0, cur_y + 18.0, true, true);
        } else {
            cc_text_at(cr, "No Wi-Fi Networks Found", 12.0, false, t->text_dim,
                       CC_WIDTH / 2.0, cur_y + 18.0, true, true);
        }
        cur_y += 38.0;
    }

    cur_y += 6.0;
    cc->sub_settings_rect = (cc_sub_rect_t){CC_PAD, cur_y, CC_CONTENT_W, 32.0};
    draw_rounded_rect(cr, cc->sub_settings_rect.x, cc->sub_settings_rect.y,
                      cc->sub_settings_rect.w, cc->sub_settings_rect.h, 8.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, cc->sub_hover_settings ? 0.16 : 0.08);
    cairo_fill(cr);
    cc_text_at(cr, "Wi-Fi Settings…", 11.5, false, t->text,
               cc->sub_settings_rect.x + 12.0,
               cc->sub_settings_rect.y + cc->sub_settings_rect.h / 2.0, false, true);
    cc_draw_chevron(cr, cc->sub_settings_rect.x + cc->sub_settings_rect.w - 14.0,
                    cc->sub_settings_rect.y + cc->sub_settings_rect.h / 2.0, t->text_dim);
}

static void cc_draw_bluetooth_page(cairo_t *cr, const cc_layout_t *L, const cc_theme_t *t,
                                   aurade_control_center_t *cc) {
    (void)L;
    double cur_y = CC_PAD;

    cc->sub_back_rect = (cc_sub_rect_t){CC_PAD, cur_y, 64.0, 26.0};
    cc_draw_back_button(cr, cc->sub_back_rect.x, cc->sub_back_rect.y,
                        cc->sub_back_rect.w, cc->sub_back_rect.h,
                        cc->sub_hover_back, t);

    cc_text_at(cr, "Bluetooth", 14.0, true, t->text, CC_WIDTH / 2.0, cur_y + 13.0, true, true);

    cc->sub_scan_rect = (cc_sub_rect_t){CC_WIDTH - CC_PAD - 70.0, cur_y + 1.0, 24.0, 24.0};
    if (cc->bluetooth_enabled) {
        cc_draw_scan_button(cr, cc->sub_scan_rect, cc->bt_scanning,
                            cc->sub_hover_scan, cc->spinner_phase, t);
    }

    cc->sub_toggle_rect = (cc_sub_rect_t){CC_WIDTH - CC_PAD - 38.0, cur_y + 2.0, 38.0, 22.0};
    cc_draw_toggle_switch(cr, cc->sub_toggle_rect.x, cc->sub_toggle_rect.y,
                          cc->sub_toggle_rect.w, cc->sub_toggle_rect.h,
                          cc->bt_toggle_pos, cc->sub_hover_toggle);
    cur_y += 38.0;

    if (!cc->bluetooth_enabled) {
        cc_rect_t off_card = {CC_PAD, cur_y, CC_CONTENT_W, 64.0};
        cc_card(cr, off_card, t->card, t->card_border);
        cc_text_at(cr, "Bluetooth is turned off", 12.5, false, t->text_dim,
                   CC_WIDTH / 2.0, cur_y + 32.0, true, true);
        return;
    }

    if (cc->bt_status_msg[0]) {
        bool is_err = (strstr(cc->bt_status_msg, "fail") != NULL ||
                       strstr(cc->bt_status_msg, "Failed") != NULL);
        cc_color_t scol = is_err ? cc_rgba(1.0, 0.42, 0.42, 0.95) : t->accent;
        cc_text_fit(cr, cc->bt_status_msg, 10.5, false, scol,
                    CC_PAD + 4.0, cur_y + 10.0, CC_CONTENT_W - 8.0);
        cur_y += 24.0;
    }

    cc_text_at(cr, cc->bt_scanning ? "DEVICES (SCANNING…)" : "DEVICES",
               10.0, true, t->text_faint, CC_PAD + 4.0, cur_y + 8.0, false, true);
    cur_y += 20.0;

    for (int i = 0; i < cc->bt_count && i < 6; i++) {
        double rh = 42.0;
        cc->bt_items[i].y = cur_y;
        cc->bt_items[i].h = rh;

        bool connected = cc->bt_items[i].connected;
        bool is_busy = (cc->bt_connecting &&
                        strcmp(cc->bt_connecting_mac, cc->bt_items[i].mac) == 0);
        double hov_a = cc->row_hover_alpha[i];
        if (cc->sub_hover_row == i && hov_a < 0.25) hov_a = 0.25;

        if (hov_a > 0.01 || connected || is_busy) {
            draw_rounded_rect(cr, CC_PAD, cur_y + 1.0, CC_CONTENT_W, rh - 2.0, 10.0);
            if (connected) {
                cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.14 + 0.08 * hov_a);
            } else {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.13 * hov_a);
            }
            cairo_fill(cr);
        }

        double bcx = CC_PAD + 18.0;
        double bcy = cur_y + rh / 2.0;
        draw_circle(cr, bcx, bcy, 13.0);
        if (connected || is_busy) {
            cc_set(cr, t->accent_blue);
        } else {
            cc_set(cr, t->badge_off);
        }
        cairo_fill(cr);
        draw_bluetooth_icon(cr, bcx, bcy, 14.0, (connected || is_busy) ? 1.0 : 0.88);

        cc_text_fit(cr, cc->bt_items[i].name, 12.2, connected || is_busy,
                    t->text, CC_PAD + 38.0, cur_y + 14.0, CC_CONTENT_W - 116.0);

        const char *sub_txt = connected ? "Connected" : "Not Connected";
        cc_color_t sub_col = connected ? t->accent : t->text_dim;
        if (is_busy) {
            sub_txt = cc->bt_disconnecting ? "Disconnecting…" : "Connecting…";
            sub_col = t->accent;
        }
        cc_text_at(cr, sub_txt, 10.0, false, sub_col, CC_PAD + 38.0, cur_y + 29.0, false, true);

        if (is_busy) {
            cc_draw_spinner(cr, CC_PAD + CC_CONTENT_W - 18.0, bcy, 7.5, cc->spinner_phase, t->accent);
        } else if (hov_a > 0.05) {
            const char *btn_lbl = connected ? "Disconnect" : "Connect";
            double pill_w = connected ? 70.0 : 58.0;
            double pill_h = 22.0;
            double px = CC_PAD + CC_CONTENT_W - pill_w - 8.0;
            double py = bcy - pill_h / 2.0;
            draw_rounded_rect(cr, px, py, pill_w, pill_h, pill_h / 2.0);
            if (connected) {
                cairo_set_source_rgba(cr, 1.0, 0.35, 0.35, 0.25 * hov_a);
            } else {
                cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.85 * hov_a);
            }
            cairo_fill(cr);
            cc_color_t lbl_col = connected ? cc_rgba(1.0, 0.55, 0.55, hov_a) : cc_rgba(1.0, 1.0, 1.0, hov_a);
            cc_text_at(cr, btn_lbl, 10.2, true, lbl_col, px + pill_w / 2.0, bcy, true, true);
        } else if (connected) {
            cc_draw_checkmark(cr, CC_PAD + CC_CONTENT_W - 16.0, bcy, 12.0, t->accent);
        }

        cur_y += rh;
    }

    if (cc->bt_count == 0) {
        if (cc->bt_scanning) {
            cc_draw_spinner(cr, CC_WIDTH / 2.0 - 60.0, cur_y + 18.0, 6.5, cc->spinner_phase, t->accent);
            cc_text_at(cr, "Searching for devices…", 12.0, false, t->text_dim,
                       CC_WIDTH / 2.0 + 8.0, cur_y + 18.0, true, true);
        } else {
            cc_text_at(cr, "No Bluetooth Devices Found", 12.0, false, t->text_dim,
                       CC_WIDTH / 2.0, cur_y + 18.0, true, true);
        }
        cur_y += 38.0;
    }

    cur_y += 6.0;
    cc->sub_settings_rect = (cc_sub_rect_t){CC_PAD, cur_y, CC_CONTENT_W, 32.0};
    draw_rounded_rect(cr, cc->sub_settings_rect.x, cc->sub_settings_rect.y,
                      cc->sub_settings_rect.w, cc->sub_settings_rect.h, 8.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, cc->sub_hover_settings ? 0.16 : 0.08);
    cairo_fill(cr);
    cc_text_at(cr, "Bluetooth Settings…", 11.5, false, t->text,
               cc->sub_settings_rect.x + 12.0,
               cc->sub_settings_rect.y + cc->sub_settings_rect.h / 2.0, false, true);
    cc_draw_chevron(cr, cc->sub_settings_rect.x + cc->sub_settings_rect.w - 14.0,
                    cc->sub_settings_rect.y + cc->sub_settings_rect.h / 2.0, t->text_dim);
}

static void cc_draw_sound_page(cairo_t *cr, const cc_layout_t *L, const cc_theme_t *t,
                               aurade_control_center_t *cc) {
    (void)L;
    double cur_y = CC_PAD;

    cc->sub_back_rect = (cc_sub_rect_t){CC_PAD, cur_y, 64.0, 26.0};
    cc_draw_back_button(cr, cc->sub_back_rect.x, cc->sub_back_rect.y,
                        cc->sub_back_rect.w, cc->sub_back_rect.h,
                        cc->sub_hover_back, t);

    cc_text_at(cr, "Sound", 14.0, true, t->text, CC_WIDTH / 2.0, cur_y + 13.0, true, true);
    cur_y += 36.0;

    double card_h = 56.0;
    cc_rect_t scard = {CC_PAD, cur_y, CC_CONTENT_W, card_h};
    cc_card(cr, scard, t->card, t->card_border);

    cc->sound_sub_mute_rect = (cc_sub_rect_t){CC_PAD + 10.0, cur_y + (card_h - 28.0) / 2.0, 28.0, 28.0};
    if (cc->sub_hover_mute) {
        draw_rounded_rect(cr, cc->sound_sub_mute_rect.x, cc->sound_sub_mute_rect.y, 28.0, 28.0, 6.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14);
        cairo_fill(cr);
    }
    draw_speaker_icon(cr, cc->sound_sub_mute_rect.x + 14.0, cc->sound_sub_mute_rect.y + 14.0, 16.0,
                      cc->volume_muted ? 0.45 : 0.95);

    cc->sound_sub_slider_rect = (cc_sub_rect_t){CC_PAD + 44.0, cur_y + (card_h - 18.0) / 2.0,
                                                CC_CONTENT_W - 90.0, 18.0};
    draw_rounded_rect(cr, cc->sound_sub_slider_rect.x, cc->sound_sub_slider_rect.y,
                      cc->sound_sub_slider_rect.w, cc->sound_sub_slider_rect.h, 9.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
    cairo_fill(cr);

    double fill_p = cc->volume_muted ? 0.0 : cc->volume;
    if (fill_p < 0.0) fill_p = 0.0;
    if (fill_p > 1.0) fill_p = 1.0;
    if (fill_p > 0.01) {
        draw_rounded_rect(cr, cc->sound_sub_slider_rect.x, cc->sound_sub_slider_rect.y,
                          cc->sound_sub_slider_rect.w * fill_p, cc->sound_sub_slider_rect.h, 9.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90);
        cairo_fill(cr);
    }

    char vol_str[16];
    if (cc->volume_muted) snprintf(vol_str, sizeof(vol_str), "Muted");
    else snprintf(vol_str, sizeof(vol_str), "%d%%", (int)lround(cc->volume * 100.0));
    cc_text_at(cr, vol_str, 11.0, true, t->text, CC_PAD + CC_CONTENT_W - 14.0,
               cur_y + card_h / 2.0, true, true);

    cur_y += card_h + 10.0;

    cc_text_at(cr, "OUTPUT", 10.0, true, t->text_faint, CC_PAD + 4.0, cur_y + 8.0, false, true);
    cur_y += 18.0;

    for (int i = 0; i < cc->audio_output_count && i < 4; i++) {
        cc->audio_outputs[i].y = cur_y;
        cc->audio_outputs[i].h = 32.0;

        bool row_hover = (cc->sub_hover_row == i);
        if (row_hover) {
            draw_rounded_rect(cr, CC_PAD, cur_y, CC_CONTENT_W, 32.0, 8.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
            cairo_fill(cr);
        }

        draw_speaker_icon(cr, CC_PAD + 16.0, cur_y + 16.0, 13.0, 0.90);

        cc_color_t name_col = cc->audio_outputs[i].is_default ? t->accent : t->text;
        cc_text_fit(cr, cc->audio_outputs[i].description, 12.0, cc->audio_outputs[i].is_default,
                    name_col, CC_PAD + 34.0, cur_y + 16.0, CC_CONTENT_W - 60.0);

        if (cc->audio_outputs[i].is_default) {
            cc_draw_checkmark(cr, CC_PAD + CC_CONTENT_W - 16.0, cur_y + 16.0, 12.0, t->accent);
        }

        cur_y += 32.0;
    }

    cur_y += 6.0;

    cc_text_at(cr, "INPUT", 10.0, true, t->text_faint, CC_PAD + 4.0, cur_y + 8.0, false, true);
    cur_y += 18.0;

    for (int i = 0; i < cc->audio_input_count && i < 3; i++) {
        cc->audio_inputs[i].y = cur_y;
        cc->audio_inputs[i].h = 32.0;

        bool row_hover = (cc->sub_hover_row == 100 + i);
        if (row_hover) {
            draw_rounded_rect(cr, CC_PAD, cur_y, CC_CONTENT_W, 32.0, 8.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
            cairo_fill(cr);
        }

        cc_icon(cr, CC_ICON_MOON, CC_PAD + 16.0, cur_y + 16.0, 13.0, cc_fade(t->text, 0.85));

        cc_color_t name_col = cc->audio_inputs[i].is_default ? t->accent : t->text;
        cc_text_fit(cr, cc->audio_inputs[i].description, 12.0, cc->audio_inputs[i].is_default,
                    name_col, CC_PAD + 34.0, cur_y + 16.0, CC_CONTENT_W - 60.0);

        if (cc->audio_inputs[i].is_default) {
            cc_draw_checkmark(cr, CC_PAD + CC_CONTENT_W - 16.0, cur_y + 16.0, 12.0, t->accent);
        }

        cur_y += 32.0;
    }

    cur_y += 6.0;
    cc->sub_settings_rect = (cc_sub_rect_t){CC_PAD, cur_y, CC_CONTENT_W, 32.0};
    draw_rounded_rect(cr, cc->sub_settings_rect.x, cc->sub_settings_rect.y,
                      cc->sub_settings_rect.w, cc->sub_settings_rect.h, 8.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, cc->sub_hover_settings ? 0.16 : 0.08);
    cairo_fill(cr);
    cc_text_at(cr, "Sound Settings…", 11.5, false, t->text,
               cc->sub_settings_rect.x + 12.0,
               cc->sub_settings_rect.y + cc->sub_settings_rect.h / 2.0, false, true);
}

static void cc_draw_page_content(cairo_t *cr, cc_page_t page, const cc_theme_t *theme,
                                 aurade_control_center_t *cc) {
    cc_layout_t L = cc_layout_for_page(cc, page);
    if (page == CC_PAGE_WIFI) {
        cc_draw_wifi_page(cr, &L, theme, cc);
    } else if (page == CC_PAGE_BLUETOOTH) {
        cc_draw_bluetooth_page(cr, &L, theme, cc);
    } else if (page == CC_PAGE_SOUND) {
        cc_draw_sound_page(cr, &L, theme, cc);
    } else {
        cc_draw_connectivity(cr, &L, theme, cc);
        cc_draw_focus(cr, &L, theme, cc);
        cc_draw_action_tiles(cr, &L, theme, cc);

        cc_draw_slider_card(cr, L.display, L.display_track, L.display_detail, theme,
                            CC_ICON_SUN, CC_ICON_DISPLAY_BADGE, "Display",
                            cc->brightness, false, cc->brightness_available,
                            cc->hover == CC_MODULE_DISPLAY, cc->pressed == CC_MODULE_DISPLAY,
                            cc->hover == CC_MODULE_DISPLAY_DETAIL, cc->pressed == CC_MODULE_DISPLAY_DETAIL);

        cc_draw_slider_card(cr, L.sound, L.sound_track, L.sound_detail, theme,
                            CC_ICON_SPEAKER, CC_ICON_AIRPLAY_AUDIO, "Sound",
                            cc->volume, cc->volume_muted, cc->volume_available,
                            cc->hover == CC_MODULE_SOUND, cc->pressed == CC_MODULE_SOUND,
                            cc->hover == CC_MODULE_SOUND_DETAIL, cc->pressed == CC_MODULE_SOUND_DETAIL);

        if (L.show_now_playing) cc_draw_now_playing(cr, &L, theme, cc);

        cc_draw_footer(cr, &L, theme, cc);
    }
}

void control_center_render(aurade_control_center_t *cc) {
    if (!cc || !cc->buffer || !cc->buffer->cr) return;
    cairo_t *cr = cc->buffer->cr;
    cairo_buffer_clear(cc->buffer);

    cc_layout_t L = cc_layout(cc);
    double panel_h = (cc->anim_panel_h > 100.0) ? cc->anim_panel_h : L.panel_h;
    cc_theme_t theme = cc_theme(cc_is_dark(cc),
                                cc->server ? cc->server->config.accent_color : NULL);

    draw_drop_shadow(cr, CC_SHADOW_PAD, CC_SHADOW_PAD, CC_WIDTH, panel_h,
                     CC_RADIUS, 20.0, theme.shadow);

    cairo_save(cr);
    cairo_translate(cr, CC_SHADOW_PAD, CC_SHADOW_PAD);

    cc_draw_panel(cr, CC_WIDTH, panel_h, &theme);

    // Clip content to rounded panel bounds for smooth page slide/height transitions
    draw_rounded_rect(cr, 0.0, 0.0, CC_WIDTH, panel_h, CC_RADIUS);
    cairo_clip(cr);

    if (cc->page_anim < 0.999 && cc->prev_page != cc->current_page) {
        double p = cc->page_anim;
        double ease = 1.0 - pow(1.0 - p, 3.0);
        double shift = 36.0 * (double)cc->page_anim_dir;

        // Outgoing page
        cairo_push_group(cr);
        cairo_translate(cr, -shift * ease, 0.0);
        cc_draw_page_content(cr, cc->prev_page, &theme, cc);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 1.0 - ease);

        // Incoming page
        cairo_push_group(cr);
        cairo_translate(cr, shift * (1.0 - ease), 0.0);
        cc_draw_page_content(cr, cc->current_page, &theme, cc);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, ease);
    } else {
        cc_draw_page_content(cr, cc->current_page, &theme, cc);
    }

    cairo_restore(cr);

    if (cc->scene_buffer) cairo_buffer_apply_to_scene(cc->buffer, cc->scene_buffer);
}

static bool cc_handle_subpage_click(aurade_control_center_t *cc, double px, double py) {
    if (!cc) return false;

    if (cc_sub_hit(px, py, cc->sub_back_rect)) {
        cc->password_prompt_open = false;
        cc->wifi_status_msg[0] = '\0';
        cc->bt_status_msg[0] = '\0';
        cc_navigate_to(cc, CC_PAGE_MAIN);
        cc_refresh_all(cc);
        control_center_render(cc);
        return true;
    }

    if (cc_sub_hit(px, py, cc->sub_scan_rect)) {
        if (cc->current_page == CC_PAGE_WIFI && cc->wifi_enabled) {
            cc->wifi_scanning = true;
            cc->wifi_scan_until_ms = cc_now_ms() + 3000;
            run_cmd_async("nmcli dev wifi rescan 2>/dev/null");
            cc_trigger_async_refresh(cc, true);
            cc_arm_anim(cc);
            cc_schedule_refresh(cc, 600);
            control_center_render(cc);
            return true;
        } else if (cc->current_page == CC_PAGE_BLUETOOTH && cc->bluetooth_enabled) {
            cc->bt_scanning = true;
            cc->bt_scan_until_ms = cc_now_ms() + 8000;
            run_cmd_async("bluetoothctl --timeout 8 scan on >/dev/null 2>&1");
            cc_trigger_async_refresh(cc, false);
            cc_arm_anim(cc);
            cc_schedule_refresh(cc, 600);
            control_center_render(cc);
            return true;
        }
    }

    if (cc_sub_hit(px, py, cc->sub_toggle_rect)) {
        if (cc->current_page == CC_PAGE_WIFI) {
            cc->wifi_enabled = !cc->wifi_enabled;
            if (!cc->wifi_enabled) cc->wifi_ssid[0] = '\0';
            apply_wifi(cc, cc->wifi_enabled);
            refresh_wifi(cc);
            cc_arm_anim(cc);
        } else if (cc->current_page == CC_PAGE_BLUETOOTH) {
            cc->bluetooth_enabled = !cc->bluetooth_enabled;
            if (!cc->bluetooth_enabled) cc->bluetooth_device[0] = '\0';
            apply_bluetooth(cc, cc->bluetooth_enabled);
            refresh_bluetooth(cc);
            cc_arm_anim(cc);
        }
        control_center_render(cc);
        return true;
    }

    if (cc_sub_hit(px, py, cc->sub_settings_rect)) {
        if (cc->current_page == CC_PAGE_WIFI) {
            app_launch_settings("wifi");
        } else if (cc->current_page == CC_PAGE_BLUETOOTH) {
            app_launch_settings("bluetooth");
        } else if (cc->current_page == CC_PAGE_SOUND) {
            app_launch_settings("sound");
        }
        dismiss_control_center(cc);
        return true;
    }

    if (cc->current_page == CC_PAGE_WIFI) {
        if (cc->password_prompt_open) {
            if (cc_sub_hit(px, py, cc->pw_cancel_rect)) {
                cc->password_prompt_open = false;
                cc->wifi_status_msg[0] = '\0';
                cc_arm_anim(cc);
                control_center_render(cc);
                return true;
            }
            if (cc_sub_hit(px, py, cc->pw_connect_rect)) {
                connect_to_wifi(cc, cc->selected_ssid, cc->wifi_password);
                return true;
            }
            if (cc_sub_hit(px, py, cc->pw_input_rect)) {
                return true;
            }
        }

        int max_show = cc->password_prompt_open ? 5 : 8;
        for (int i = 0; i < cc->wifi_count && i < max_show; i++) {
            if (py >= cc->wifi_items[i].y && py <= cc->wifi_items[i].y + cc->wifi_items[i].h) {
                const char *ssid = cc->wifi_items[i].ssid;
                if (cc->wifi_items[i].active) {
                    return true;
                }
                if (!cc->wifi_items[i].secure || is_wifi_saved(ssid)) {
                    connect_to_wifi(cc, ssid, NULL);
                    return true;
                } else {
                    cc->password_prompt_open = true;
                    cc->wifi_password[0] = '\0';
                    snprintf(cc->selected_ssid, sizeof(cc->selected_ssid), "%s", ssid);
                    cc->wifi_status_msg[0] = '\0';
                    cc_arm_anim(cc);
                    control_center_render(cc);
                    return true;
                }
            }
        }
    }

    if (cc->current_page == CC_PAGE_BLUETOOTH) {
        for (int i = 0; i < cc->bt_count && i < 6; i++) {
            if (py >= cc->bt_items[i].y && py <= cc->bt_items[i].y + cc->bt_items[i].h) {
                connect_to_bluetooth(cc, cc->bt_items[i].mac, cc->bt_items[i].name,
                                     cc->bt_items[i].connected);
                return true;
            }
        }
    }

    if (cc->current_page == CC_PAGE_SOUND) {
        if (cc_sub_hit(px, py, cc->sound_sub_mute_rect)) {
            toggle_mute(cc);
            control_center_render(cc);
            return true;
        }
        if (cc_sub_hit(px, py, cc->sound_sub_slider_rect)) {
            cc->sub_dragging_slider = true;
            double p = (px - cc->sound_sub_slider_rect.x) / cc->sound_sub_slider_rect.w;
            if (p < 0.0) p = 0.0;
            if (p > 1.0) p = 1.0;
            cc->volume = p;
            apply_volume(cc);
            control_center_render(cc);
            return true;
        }
        for (int i = 0; i < cc->audio_output_count && i < 4; i++) {
            if (py >= cc->audio_outputs[i].y && py <= cc->audio_outputs[i].y + cc->audio_outputs[i].h) {
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "pactl set-default-sink \"%s\"", cc->audio_outputs[i].name);
                run_cmd_async(cmd);
                refresh_sound(cc);
                control_center_render(cc);
                return true;
            }
        }
        for (int i = 0; i < cc->audio_input_count && i < 3; i++) {
            if (py >= cc->audio_inputs[i].y && py <= cc->audio_inputs[i].y + cc->audio_inputs[i].h) {
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "pactl set-default-source \"%s\"", cc->audio_inputs[i].name);
                run_cmd_async(cmd);
                refresh_sound(cc);
                control_center_render(cc);
                return true;
            }
        }
    }

    return true;
}

static void cc_handle_subpage_motion(aurade_control_center_t *cc, double px, double py) {
    if (!cc) return;

    if (cc->sub_dragging_slider && cc->current_page == CC_PAGE_SOUND) {
        double p = (px - cc->sound_sub_slider_rect.x) / cc->sound_sub_slider_rect.w;
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
        cc->volume = p;
        apply_volume(cc);
        control_center_render(cc);
        return;
    }

    int old_row = cc->sub_hover_row;
    bool old_back = cc->sub_hover_back;
    bool old_toggle = cc->sub_hover_toggle;
    bool old_scan = cc->sub_hover_scan;
    bool old_settings = cc->sub_hover_settings;
    bool old_cancel = cc->sub_hover_cancel;
    bool old_connect = cc->sub_hover_connect;
    bool old_mute = cc->sub_hover_mute;

    cc->sub_hover_row = -1;
    cc->sub_hover_back = cc_sub_hit(px, py, cc->sub_back_rect);
    cc->sub_hover_toggle = cc_sub_hit(px, py, cc->sub_toggle_rect);
    cc->sub_hover_scan = cc_sub_hit(px, py, cc->sub_scan_rect);
    cc->sub_hover_settings = cc_sub_hit(px, py, cc->sub_settings_rect);
    cc->sub_hover_cancel = false;
    cc->sub_hover_connect = false;
    cc->sub_hover_mute = false;

    if (cc->current_page == CC_PAGE_WIFI) {
        if (cc->password_prompt_open) {
            cc->sub_hover_cancel = cc_sub_hit(px, py, cc->pw_cancel_rect);
            cc->sub_hover_connect = cc_sub_hit(px, py, cc->pw_connect_rect);
        }
        int max_show = cc->password_prompt_open ? 5 : 8;
        for (int i = 0; i < cc->wifi_count && i < max_show; i++) {
            if (py >= cc->wifi_items[i].y && py <= cc->wifi_items[i].y + cc->wifi_items[i].h) {
                cc->sub_hover_row = i;
                break;
            }
        }
    } else if (cc->current_page == CC_PAGE_BLUETOOTH) {
        for (int i = 0; i < cc->bt_count && i < 6; i++) {
            if (py >= cc->bt_items[i].y && py <= cc->bt_items[i].y + cc->bt_items[i].h) {
                cc->sub_hover_row = i;
                break;
            }
        }
    } else if (cc->current_page == CC_PAGE_SOUND) {
        cc->sub_hover_mute = cc_sub_hit(px, py, cc->sound_sub_mute_rect);
        for (int i = 0; i < cc->audio_output_count && i < 4; i++) {
            if (py >= cc->audio_outputs[i].y && py <= cc->audio_outputs[i].y + cc->audio_outputs[i].h) {
                cc->sub_hover_row = i;
                break;
            }
        }
        if (cc->sub_hover_row == -1) {
            for (int i = 0; i < cc->audio_input_count && i < 3; i++) {
                if (py >= cc->audio_inputs[i].y && py <= cc->audio_inputs[i].y + cc->audio_inputs[i].h) {
                    cc->sub_hover_row = 100 + i;
                    break;
                }
            }
        }
    }

    if (old_row != cc->sub_hover_row || old_back != cc->sub_hover_back ||
        old_toggle != cc->sub_hover_toggle || old_scan != cc->sub_hover_scan ||
        old_settings != cc->sub_hover_settings || old_cancel != cc->sub_hover_cancel ||
        old_connect != cc->sub_hover_connect || old_mute != cc->sub_hover_mute) {
        cc_arm_anim(cc);
        control_center_render(cc);
    }
}

bool control_center_handle_click(aurade_control_center_t *cc, double x, double y) {
    if (!cc || !cc->active || cc->progress < 0.5) return false;

    cc_layout_t L = cc_layout(cc);
    double panel_h = (cc->anim_panel_h > 100.0) ? cc->anim_panel_h : L.panel_h;
    double px = 0.0, py = 0.0;
    cc_to_panel_coords(cc, x, y, &px, &py);

    if (px < 0.0 || py < 0.0 || px > CC_WIDTH || py > panel_h) {
        dismiss_control_center(cc);
        return true;
    }

    if (cc->current_page != CC_PAGE_MAIN) {
        return cc_handle_subpage_click(cc, px, py);
    }

    int module = cc_module_at(&L, px, py);
    cc->pressed = module;
    cc->hover = module;

    switch (module) {
        case CC_MODULE_WIFI_TOGGLE:
            if (cc->wifi_available) {
                cc->wifi_enabled = !cc->wifi_enabled;
                if (!cc->wifi_enabled) cc->wifi_ssid[0] = '\0';
                apply_wifi(cc, cc->wifi_enabled);
                cc_arm_anim(cc);
            }
            break;
        case CC_MODULE_WIFI:
            cc->password_prompt_open = false;
            cc->wifi_status_msg[0] = '\0';
            cc_navigate_to(cc, CC_PAGE_WIFI);
            // Cached networks render in 0ms; start background rescan silently
            run_cmd_async("nmcli dev wifi rescan 2>/dev/null");
            refresh_wifi(cc);
            break;
        case CC_MODULE_BLUETOOTH_TOGGLE:
            if (cc->bluetooth_available) {
                cc->bluetooth_enabled = !cc->bluetooth_enabled;
                if (!cc->bluetooth_enabled) cc->bluetooth_device[0] = '\0';
                apply_bluetooth(cc, cc->bluetooth_enabled);
                cc_arm_anim(cc);
            }
            break;
        case CC_MODULE_BLUETOOTH:
            cc->bt_status_msg[0] = '\0';
            cc_navigate_to(cc, CC_PAGE_BLUETOOTH);
            refresh_bluetooth(cc);
            break;
        case CC_MODULE_HOTSPOT:
            if (!cc->hotspot_enabled) {
                run_cmd_async("nmcli dev wifi hotspot 2>/dev/null");
                cc->hotspot_enabled = true;
            } else {
                run_cmd_async("nmcli con down Hotspot 2>/dev/null || nmcli connection down id Hotspot 2>/dev/null");
                cc->hotspot_enabled = false;
            }
            break;
        case CC_MODULE_FOCUS:
            if (cc->dnd_available) {
                cc->dnd_enabled = !cc->dnd_enabled;
                apply_dnd(cc, cc->dnd_enabled);
            }
            break;
        case CC_MODULE_AURA_CONNECT:
            cc->aura_connect_enabled = true;
            app_launch_aura_connect();
            dismiss_control_center(cc);
            return true;
        case CC_MODULE_SCREEN_KEYBOARD:
            cc->screen_keyboard_enabled = !cc->screen_keyboard_enabled;
            if (cc->screen_keyboard_enabled) {
                run_cmd_async("wvkbd-mobintl 2>/dev/null || onboard 2>/dev/null");
            } else {
                run_cmd_async("pkill -x wvkbd-mobintl 2>/dev/null || pkill -x onboard 2>/dev/null");
            }
            break;
        case CC_MODULE_DISPLAY_DETAIL:
            app_launch_settings("displays");
            dismiss_control_center(cc);
            return true;
        case CC_MODULE_DISPLAY:
            if (cc->brightness_available) {
                cc->dragging = 1;
                set_slider_from_x(cc, px, 1);
            }
            break;
        case CC_MODULE_SOUND_DETAIL:
            cc_navigate_to(cc, CC_PAGE_SOUND);
            refresh_sound(cc);
            break;
        case CC_MODULE_SOUND:
            if (cc->volume_available) {
                cc->dragging = 2;
                set_slider_from_x(cc, px, 2);
            }
            break;
        case CC_MODULE_PREV:
            media_command(cc, "playerctl previous");
            break;
        case CC_MODULE_PLAY:
            media_command(cc, "playerctl play-pause");
            break;
        case CC_MODULE_NEXT:
            media_command(cc, "playerctl next");
            break;
        case CC_MODULE_BATTERY:
            app_launch_settings("power");
            dismiss_control_center(cc);
            return true;
        case CC_MODULE_USER:
            app_launch_settings("user");
            dismiss_control_center(cc);
            return true;
        case CC_MODULE_ACCESSIBILITY:
            if (cc->dark_mode_available) {
                cc->dark_mode = !cc->dark_mode;
                apply_dark_mode(cc, cc->dark_mode);
            }
            break;
        case CC_MODULE_SETTINGS:
            app_launch_settings(NULL);
            dismiss_control_center(cc);
            return true;
        default:
            break;
    }

    control_center_render(cc);
    return true;
}

void control_center_handle_motion(aurade_control_center_t *cc, double x, double y) {
    if (!cc || !cc->active) return;

    if (cc->current_page != CC_PAGE_MAIN) {
        double px = 0.0, py = 0.0;
        cc_to_panel_coords(cc, x, y, &px, &py);
        cc_handle_subpage_motion(cc, px, py);
        return;
    }

    if (cc->dragging != 0) {
        double px = 0.0;
        cc_to_panel_coords(cc, x, y, &px, NULL);
        set_slider_from_x(cc, px, cc->dragging);
        control_center_render(cc);
        return;
    }

    cc_layout_t L = cc_layout(cc);
    double px = 0.0, py = 0.0;
    cc_to_panel_coords(cc, x, y, &px, &py);
    int module = cc_module_at(&L, px, py);

    if (module != cc->hover) {
        cc->hover = module;
        control_center_render(cc);
    }
}

void control_center_handle_release(aurade_control_center_t *cc, double x, double y) {
    if (!cc) return;
    cc->pressed = CC_MODULE_NONE;

    if (cc->sub_dragging_slider) {
        cc->sub_dragging_slider = false;
        apply_volume(cc);
        control_center_render(cc);
        return;
    }

    if (cc->dragging == 1) {
        double px = 0.0;
        cc_to_panel_coords(cc, x, y, &px, NULL);
        set_slider_from_x(cc, px, 1);
        apply_brightness(cc);
    } else if (cc->dragging == 2) {
        double px = 0.0;
        cc_to_panel_coords(cc, x, y, &px, NULL);
        set_slider_from_x(cc, px, 2);
        apply_volume(cc);
    }
    cc->dragging = 0;
    control_center_render(cc);
}

bool control_center_handle_key(aurade_control_center_t *cc, uint32_t keyval, bool pressed) {
    if (!cc || !cc->active || !pressed) return false;

    if (keyval == XKB_KEY_Escape) {
        if (cc->password_prompt_open) {
            cc->password_prompt_open = false;
            cc->wifi_status_msg[0] = '\0';
            cc_arm_anim(cc);
            control_center_render(cc);
            return true;
        }
        if (cc->current_page != CC_PAGE_MAIN) {
            cc_navigate_to(cc, CC_PAGE_MAIN);
            cc_refresh_all(cc);
            control_center_render(cc);
            return true;
        }
        dismiss_control_center(cc);
        return true;
    }

    if (cc->current_page == CC_PAGE_WIFI && cc->password_prompt_open) {
        if (keyval == XKB_KEY_Return || keyval == XKB_KEY_KP_Enter) {
            connect_to_wifi(cc, cc->selected_ssid, cc->wifi_password);
            return true;
        }
        if (keyval == XKB_KEY_BackSpace) {
            size_t len = strlen(cc->wifi_password);
            if (len > 0) {
                cc->wifi_password[len - 1] = '\0';
                control_center_render(cc);
            }
            return true;
        }

        char buf[8] = {0};
        int n = xkb_keysym_to_utf8(keyval, buf, sizeof(buf));
        if (n > 0 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] != 127) {
            size_t len = strlen(cc->wifi_password);
            if (len + (size_t)n < sizeof(cc->wifi_password) - 1) {
                strcat(cc->wifi_password, buf);
                control_center_render(cc);
            }
            return true;
        }
        return true;
    }

    return false;
}

const char *control_center_cursor(aurade_control_center_t *cc, double x, double y) {
    if (!cc || !cc->active) return "default";
    cc_layout_t L = cc_layout(cc);
    double px = 0.0, py = 0.0;
    cc_to_panel_coords(cc, x, y, &px, &py);

    if (cc->current_page != CC_PAGE_MAIN) {
        if (cc->password_prompt_open && cc_sub_hit(px, py, cc->pw_input_rect)) {
            return "text";
        }
        if (cc_sub_hit(px, py, cc->sub_back_rect) ||
            cc_sub_hit(px, py, cc->sub_toggle_rect) ||
            cc_sub_hit(px, py, cc->sub_scan_rect) ||
            cc_sub_hit(px, py, cc->sub_settings_rect) ||
            cc->sub_hover_row != -1 ||
            (cc->password_prompt_open && (cc_sub_hit(px, py, cc->pw_cancel_rect) || cc_sub_hit(px, py, cc->pw_connect_rect))) ||
            (cc->current_page == CC_PAGE_SOUND && cc_sub_hit(px, py, cc->sound_sub_mute_rect))) {
            return "pointer";
        }
        return "default";
    }

    return cc_module_at(&L, px, py) != CC_MODULE_NONE ? "pointer" : "default";
}

void control_center_destroy(aurade_control_center_t *cc) {
    if (!cc) return;
    if (cc->anim_timer) {
        wl_event_source_remove(cc->anim_timer);
        cc->anim_timer = NULL;
    }
    if (cc->refresh_timer) {
        wl_event_source_remove(cc->refresh_timer);
        cc->refresh_timer = NULL;
    }
    if (cc->np_timer) {
        wl_event_source_remove(cc->np_timer);
        cc->np_timer = NULL;
    }
    if (cc->probe_event_source) {
        wl_event_source_remove(cc->probe_event_source);
        cc->probe_event_source = NULL;
    }
    if (cc->probe_event_fd >= 0) {
        close(cc->probe_event_fd);
        cc->probe_event_fd = -1;
    }
    pthread_mutex_destroy(&cc->probe_mutex);
    if (cc->buffer) wlr_buffer_drop(&cc->buffer->base);
    free(cc);
}

void control_center_sync_audio(aurade_control_center_t *cc, double volume, bool muted) {
    if (!cc) return;
    cc->volume = volume;
    cc->volume_muted = muted;
    cc->volume_available = true;
    if (control_center_is_open(cc)) {
        control_center_render(cc);
    }
}

void control_center_sync_brightness(aurade_control_center_t *cc, double brightness) {
    if (!cc) return;
    cc->brightness = brightness;
    cc->brightness_available = true;
    if (control_center_is_open(cc)) {
        control_center_render(cc);
    }
}
