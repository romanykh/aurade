#define _GNU_SOURCE
#include "../src/ext/aurade_ext.h"

#include <cairo.h>
#include <math.h>
#include <pango/pangocairo.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define CLIP_MAX_ITEMS 24
#define CLIP_MAX_DATA  8192
#define CLIP_PANEL_W   360
#define CLIP_PANEL_H   440
#define CLIP_BTN_W     28.0
#define CLIP_BTN_H     17.0
#define CLIP_ROW_H     54.0
#define CLIP_LIST_TOP  54.0

typedef struct {
    bool is_files;
    char mime[64];
    char data[CLIP_MAX_DATA];
    size_t len;
    char title[160];
    char subtitle[160];
    char time_str[16];
} clip_item_t;

static const aurade_ext_api_t *g_api = NULL;
static aurade_ext_panel_t *g_panel = NULL;
static bool g_open = false;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static volatile bool g_running = false;
static bool g_dirty = false;

static clip_item_t g_items[CLIP_MAX_ITEMS];
static int g_count = 0;
static int g_scroll = 0;

static double g_btn_x = 0.0, g_btn_y = 0.0;
static int g_panel_x = 0, g_panel_y = 38;

/* ---- Helpers ------------------------------------------------------- */

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

static void draw_text_pango(cairo_t *cr, const char *txt, double size, bool bold,
                            double r, double g, double b, double a,
                            double x, double y, double max_w, bool center_x, bool center_y) {
    if (!txt || !*txt) return;
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, "Inter, Noto Sans, sans-serif");
    pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_layout_set_text(l, txt, -1);
    if (max_w > 0) {
        pango_layout_set_width(l, (int)(max_w * PANGO_SCALE));
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    }
    int pw, ph;
    pango_layout_get_pixel_size(l, &pw, &ph);
    double dx = center_x ? (x - pw / 2.0) : x;
    double dy = center_y ? (y - ph / 2.0) : y;
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, dx, dy);
    pango_cairo_show_layout(cr, l);
    pango_font_description_free(fd);
    g_object_unref(l);
}

static void draw_clipboard_glyph(cairo_t *cr, double cx, double cy, double scale, double a) {
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, scale, scale);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, a);
    cairo_set_line_width(cr, 1.35);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    // Board body
    rrect(cr, -4.5, -4.5, 9.0, 10.5, 1.8);
    cairo_stroke(cr);
    // Top clip
    rrect(cr, -2.2, -6.0, 4.4, 2.4, 0.9);
    cairo_fill(cr);
    // Lines
    cairo_move_to(cr, -2.2, -0.3);
    cairo_line_to(cr, 2.2, -0.3);
    cairo_move_to(cr, -2.2, 2.5);
    cairo_line_to(cr, 1.4, 2.5);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* ---- Clipboard parsing & polling ----------------------------------- */

static void format_preview(clip_item_t *it) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(it->time_str, sizeof(it->time_str), "%H:%M", &tm);

    if (it->is_files) {
        int file_count = 0;
        char first_name[128] = {0};
        char first_dir[128] = {0};

        const char *p = it->data;
        const char *end = it->data + it->len;
        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
            while (line_len > 0 && (p[line_len - 1] == '\r' || p[line_len - 1] == '\n')) line_len--;
            if (line_len > 7 && strncmp(p, "file://", 7) == 0) {
                file_count++;
                if (!first_name[0]) {
                    char path[512];
                    size_t plen = line_len - 7;
                    if (plen >= sizeof(path)) plen = sizeof(path) - 1;
                    memcpy(path, p + 7, plen);
                    path[plen] = '\0';
                    char *slash = strrchr(path, '/');
                    if (slash && slash[1]) {
                        snprintf(first_name, sizeof(first_name), "%s", slash + 1);
                        *slash = '\0';
                        snprintf(first_dir, sizeof(first_dir), "%s", path[0] ? path : "/");
                    } else {
                        snprintf(first_name, sizeof(first_name), "%s", path);
                    }
                }
            }
            if (!nl) break;
            p = nl + 1;
        }
        if (file_count <= 0) {
            snprintf(it->title, sizeof(it->title), "Copied Files");
            snprintf(it->subtitle, sizeof(it->subtitle), "%s", it->mime);
        } else if (file_count == 1) {
            snprintf(it->title, sizeof(it->title), "%s", first_name);
            snprintf(it->subtitle, sizeof(it->subtitle), "File • %s", first_dir[0] ? first_dir : "/");
        } else {
            snprintf(it->title, sizeof(it->title), "%s (+%d more)", first_name, file_count - 1);
            snprintf(it->subtitle, sizeof(it->subtitle), "%d files • %s", file_count, first_dir[0] ? first_dir : "/");
        }
    } else {
        size_t o = 0;
        for (size_t i = 0; i < it->len && o < sizeof(it->title) - 1; i++) {
            unsigned char c = (unsigned char)it->data[i];
            if (c == '\n' || c == '\r' || c == '\t') {
                if (o > 0 && it->title[o - 1] != ' ') it->title[o++] = ' ';
            } else if (c >= 32) {
                it->title[o++] = (char)c;
            }
        }
        it->title[o] = '\0';
        if (!it->title[0]) snprintf(it->title, sizeof(it->title), "(whitespace)");
        snprintf(it->subtitle, sizeof(it->subtitle), "Text • %zu chars", it->len);
    }
}

static bool read_cmd_buf(const char *cmd, char *buf, size_t cap, size_t *out_len) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    size_t total = 0;
    while (total < cap - 1) {
        size_t n = fread(buf + total, 1, cap - 1 - total, fp);
        if (n == 0) break;
        total += n;
    }
    int st = pclose(fp);
    buf[total] = '\0';
    if (out_len) *out_len = total;
    return st == 0 && total > 0;
}

static void push_item(const clip_item_t *it) {
    pthread_mutex_lock(&g_lock);
    if (g_count > 0 && g_items[0].len == it->len &&
        g_items[0].is_files == it->is_files &&
        memcmp(g_items[0].data, it->data, it->len) == 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    // Remove duplicate further down if present
    for (int i = 1; i < g_count; i++) {
        if (g_items[i].len == it->len &&
            g_items[i].is_files == it->is_files &&
            memcmp(g_items[i].data, it->data, it->len) == 0) {
            memmove(&g_items[i], &g_items[i + 1], (size_t)(g_count - i - 1) * sizeof(clip_item_t));
            g_count--;
            break;
        }
    }
    int move_n = g_count < CLIP_MAX_ITEMS ? g_count : CLIP_MAX_ITEMS - 1;
    if (move_n > 0) memmove(&g_items[1], &g_items[0], (size_t)move_n * sizeof(clip_item_t));
    g_items[0] = *it;
    if (g_count < CLIP_MAX_ITEMS) g_count++;
    g_dirty = true;
    pthread_mutex_unlock(&g_lock);
}

static void *clipboard_worker(void *arg) {
    (void)arg;
    char types[512];
    char buf[CLIP_MAX_DATA];
    while (g_running) {
        size_t tlen = 0;
        if (read_cmd_buf("wl-paste --list-types 2>/dev/null", types, sizeof(types), &tlen)) {
            clip_item_t it;
            memset(&it, 0, sizeof(it));
            if (strstr(types, "x-special/gnome-copied-files")) {
                if (read_cmd_buf("wl-paste -t x-special/gnome-copied-files 2>/dev/null", buf, sizeof(buf), &it.len)) {
                    it.is_files = true;
                    snprintf(it.mime, sizeof(it.mime), "x-special/gnome-copied-files");
                    memcpy(it.data, buf, it.len);
                    format_preview(&it);
                    push_item(&it);
                }
            } else if (strstr(types, "text/uri-list")) {
                if (read_cmd_buf("wl-paste -t text/uri-list 2>/dev/null", buf, sizeof(buf), &it.len)) {
                    it.is_files = true;
                    snprintf(it.mime, sizeof(it.mime), "text/uri-list");
                    memcpy(it.data, buf, it.len);
                    format_preview(&it);
                    push_item(&it);
                }
            } else if (strstr(types, "text/plain") || strstr(types, "UTF8_STRING") || strstr(types, "STRING")) {
                if (read_cmd_buf("wl-paste --no-newline 2>/dev/null", buf, sizeof(buf), &it.len)) {
                    it.is_files = false;
                    snprintf(it.mime, sizeof(it.mime), "text/plain");
                    memcpy(it.data, buf, it.len);
                    format_preview(&it);
                    push_item(&it);
                }
            }
        }
        usleep(450000);
    }
    return NULL;
}

static void restore_item_to_clipboard(const clip_item_t *it) {
    FILE *fp = fopen("/tmp/aurade-clip-restore.bin", "wb");
    if (!fp) return;
    fwrite(it->data, 1, it->len, fp);
    fclose(fp);

    char cmd[256];
    if (it->is_files && it->mime[0]) {
        snprintf(cmd, sizeof(cmd), "wl-copy -t '%s' < /tmp/aurade-clip-restore.bin", it->mime);
    } else {
        snprintf(cmd, sizeof(cmd), "wl-copy < /tmp/aurade-clip-restore.bin");
    }
    g_api->exec(cmd);
}

/* ---- UI Rendering & Interaction ------------------------------------ */

static void position_panel(void) {
    int sw = 1920, sh = 1080;
    g_api->get_screen_size(&sw, &sh);
    g_panel_x = sw - CLIP_PANEL_W - 14;
    if (g_panel_x < 12) g_panel_x = 12;
    g_panel_y = 38;
    if (g_panel) g_api->panel_set_pos(g_panel, g_panel_x, g_panel_y);
}

static void toggle_panel(void) {
    g_open = !g_open;
    if (g_open) {
        g_scroll = 0;
        position_panel();
        g_api->panel_redraw(g_panel);
    }
    g_api->panel_set_visible(g_panel, g_open);
    g_api->topbar_redraw();
}

static void on_panel_draw(cairo_t *cr, int w, int h, void *ud) {
    (void)ud;
    // Translucent dark liquid glass panel
    rrect(cr, 0, 0, w, h, 18.0);
    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.16, 0.16, 0.24, 0.84);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.08, 0.08, 0.14, 0.90);
    cairo_set_source(cr, bg);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(bg);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    pthread_mutex_lock(&g_lock);
    int cnt = g_count;

    // Header
    draw_text_pango(cr, "Clipboard", 15.0, true, 1, 1, 1, 0.95, 18.0, 26.0, 160, false, true);

    if (cnt > 0) {
        char cbuf[16];
        snprintf(cbuf, sizeof(cbuf), "%d", cnt);
        draw_text_pango(cr, cbuf, 11.5, true, 0.65, 0.78, 1.0, 0.85, 96.0, 26.5, 40, false, true);

        // Clear button
        rrect(cr, w - 76.0, 14.0, 60.0, 24.0, 12.0);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
        cairo_fill(cr);
        draw_text_pango(cr, "Clear", 11.0, true, 1, 1, 1, 0.90, w - 46.0, 26.0, 50, true, true);
    }

    // Divider
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 16.0, 48.0);
    cairo_line_to(cr, w - 16.0, 48.0);
    cairo_stroke(cr);

    if (cnt == 0) {
        draw_clipboard_glyph(cr, w / 2.0, h * 0.42, 2.2, 0.42);
        draw_text_pango(cr, "Clipboard is Empty", 13.5, true, 1, 1, 1, 0.65, w / 2.0, h * 0.52, w - 40, true, true);
        draw_text_pango(cr, "Copy text or files (Super+V to toggle)", 11.0, false, 1, 1, 1, 0.38, w / 2.0, h * 0.58, w - 40, true, true);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    int max_vis = (int)((h - CLIP_LIST_TOP - 12.0) / (CLIP_ROW_H + 6.0));
    if (g_scroll > cnt - max_vis) g_scroll = cnt - max_vis;
    if (g_scroll < 0) g_scroll = 0;

    for (int i = g_scroll; i < cnt && (i - g_scroll) < max_vis; i++) {
        const clip_item_t *it = &g_items[i];
        double rx = 14.0;
        double ry = CLIP_LIST_TOP + (i - g_scroll) * (CLIP_ROW_H + 6.0);
        double rw = w - 28.0;
        double rh = CLIP_ROW_H;

        rrect(cr, rx, ry, rw, rh, 12.0);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Type badge
        rrect(cr, rx + 10.0, ry + 11.0, 32.0, 32.0, 9.0);
        if (it->is_files) {
            cairo_set_source_rgba(cr, 0.25, 0.56, 1.0, 0.25);
        } else {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
        }
        cairo_fill(cr);
        draw_text_pango(cr, it->is_files ? "FILE" : "TXT", 9.5, true,
                        it->is_files ? 0.55 : 0.85,
                        it->is_files ? 0.80 : 0.85,
                        1.0, 0.95, rx + 26.0, ry + 27.0, 30.0, true, true);

        draw_text_pango(cr, it->title, 12.5, true, 1, 1, 1, 0.94, rx + 50.0, ry + 18.0, rw - 92.0, false, true);
        draw_text_pango(cr, it->subtitle, 10.5, false, 1, 1, 1, 0.55, rx + 50.0, ry + 36.0, rw - 110.0, false, true);
        draw_text_pango(cr, it->time_str, 10.0, false, 1, 1, 1, 0.42, rx + rw - 52.0, ry + 36.0, 40.0, false, true);

        // Delete (x) button
        cairo_arc(cr, rx + rw - 16.0, ry + 16.0, 8.5, 0, 2.0 * M_PI);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.72);
        cairo_set_line_width(cr, 1.2);
        cairo_move_to(cr, rx + rw - 19.0, ry + 13.0);
        cairo_line_to(cr, rx + rw - 13.0, ry + 19.0);
        cairo_move_to(cr, rx + rw - 13.0, ry + 13.0);
        cairo_line_to(cr, rx + rw - 19.0, ry + 19.0);
        cairo_stroke(cr);
    }

    pthread_mutex_unlock(&g_lock);
}

static bool on_panel_click(double lx, double ly, bool pressed, void *ud) {
    (void)ud;
    if (!pressed || !g_open) return g_open;

    pthread_mutex_lock(&g_lock);
    // Clear button
    if (g_count > 0 && lx >= CLIP_PANEL_W - 76.0 && lx <= CLIP_PANEL_W - 16.0 &&
        ly >= 14.0 && ly <= 38.0) {
        g_count = 0;
        g_scroll = 0;
        pthread_mutex_unlock(&g_lock);
        g_api->panel_redraw(g_panel);
        g_api->topbar_redraw();
        return true;
    }

    int max_vis = (int)((CLIP_PANEL_H - CLIP_LIST_TOP - 12.0) / (CLIP_ROW_H + 6.0));
    for (int i = g_scroll; i < g_count && (i - g_scroll) < max_vis; i++) {
        double rx = 14.0;
        double ry = CLIP_LIST_TOP + (i - g_scroll) * (CLIP_ROW_H + 6.0);
        double rw = CLIP_PANEL_W - 28.0;
        double rh = CLIP_ROW_H;
        if (lx >= rx && lx <= rx + rw && ly >= ry && ly <= ry + rh) {
            // Check delete (x)
            double dx = lx - (rx + rw - 16.0);
            double dy = ly - (ry + 16.0);
            if (dx * dx + dy * dy <= 12.0 * 12.0) {
                memmove(&g_items[i], &g_items[i + 1], (size_t)(g_count - i - 1) * sizeof(clip_item_t));
                g_count--;
                pthread_mutex_unlock(&g_lock);
                g_api->panel_redraw(g_panel);
                g_api->topbar_redraw();
                return true;
            }
            clip_item_t chosen = g_items[i];
            pthread_mutex_unlock(&g_lock);
            restore_item_to_clipboard(&chosen);
            toggle_panel();
            return true;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return true;
}

static void on_topbar_draw(cairo_t *cr, int bar_w, int bar_h, void *ud) {
    (void)ud;
    g_btn_x = bar_w - 392.0;
    if (g_btn_x < 220.0) g_btn_x = 220.0;
    g_btn_y = (bar_h - CLIP_BTN_H) / 2.0;

    rrect(cr, g_btn_x, g_btn_y, CLIP_BTN_W, CLIP_BTN_H, CLIP_BTN_H / 2.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, g_open ? 0.24 : 0.12);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    draw_clipboard_glyph(cr, g_btn_x + CLIP_BTN_W / 2.0, bar_h / 2.0, 0.90, g_open ? 1.0 : 0.88);

    pthread_mutex_lock(&g_lock);
    int cnt = g_count;
    pthread_mutex_unlock(&g_lock);
    if (cnt > 0 && !g_open) {
        cairo_arc(cr, g_btn_x + CLIP_BTN_W - 4.0, g_btn_y + 4.0, 2.2, 0, 2.0 * M_PI);
        cairo_set_source_rgba(cr, 0.30, 0.64, 1.0, 0.95);
        cairo_fill(cr);
    }
}

static bool on_click(double lx, double ly, uint32_t button, bool pressed, void *ud) {
    (void)ud;
    if (button != 0x110 /* BTN_LEFT */) return false;

    // Topbar button click
    if (ly <= 32.0 && lx >= g_btn_x - 2.0 && lx <= g_btn_x + CLIP_BTN_W + 2.0 &&
        ly >= g_btn_y - 2.0 && ly <= g_btn_y + CLIP_BTN_H + 2.0) {
        if (pressed) toggle_panel();
        return true;
    }

    // Click outside open panel dismisses it
    if (g_open && pressed) {
        if (lx < g_panel_x || lx > g_panel_x + CLIP_PANEL_W ||
            ly < g_panel_y || ly > g_panel_y + CLIP_PANEL_H) {
            toggle_panel();
            return true;
        }
    }
    return false;
}

static bool on_key(uint32_t sym, uint32_t mods, bool pressed, void *ud) {
    (void)ud;
    if (!pressed) return false;
    // Super+V
    if ((mods & 1u) && (sym == XKB_KEY_v || sym == XKB_KEY_V)) {
        toggle_panel();
        return true;
    }
    if (g_open && sym == XKB_KEY_Escape) {
        toggle_panel();
        return true;
    }
    return false;
}

static void on_tick(double dt, void *ud) {
    (void)dt; (void)ud;
    bool need = false;
    pthread_mutex_lock(&g_lock);
    if (g_dirty) { g_dirty = false; need = true; }
    pthread_mutex_unlock(&g_lock);

    if (need) {
        if (g_open) g_api->panel_redraw(g_panel);
        g_api->topbar_redraw();
    }
}

/* ---- Extension Entry Points ---------------------------------------- */

static const aurade_ext_info_t EXT_INFO = {
    .name = "Clipboard Buffer",
    .version = "1.0",
    .author = "AuraDE",
    .description = "Clipboard & copied files history (Super+V or topbar button)"
};

const aurade_ext_info_t *aurade_ext_meta(void) {
    return &EXT_INFO;
}

bool aurade_ext_init(const aurade_ext_api_t *api) {
    if (!api || api->api_version != AURADE_EXT_API_VERSION) return false;
    g_api = api;

    g_panel = api->panel_create(100, 38, CLIP_PANEL_W, CLIP_PANEL_H, true,
                                on_panel_draw, on_panel_click, NULL);
    if (!g_panel) return false;
    api->panel_set_visible(g_panel, false);
    position_panel();

    api->hook_topbar_draw(on_topbar_draw, NULL);
    api->hook_click(on_click, NULL);
    api->hook_key(on_key, NULL);
    api->hook_tick(on_tick, NULL);

    g_running = true;
    pthread_create(&g_thread, NULL, clipboard_worker, NULL);
    api->topbar_redraw();
    return true;
}

void aurade_ext_deinit(void) {
    g_running = false;
    if (g_thread) pthread_join(g_thread, NULL);
    if (g_panel && g_api) {
        g_api->panel_destroy(g_panel);
        g_panel = NULL;
    }
}
