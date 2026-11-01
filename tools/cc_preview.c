/*
 * cc_preview — offline renderer for the AuraDE Control Center.
 *
 * The Control Center is a Cairo scene-graph buffer, so it can be rendered
 * without a running compositor. This tool paints the real control_center_render()
 * output over a mock desktop wallpaper and writes PNGs, which makes design
 * iteration and visual regression checks possible on headless machines.
 *
 *   make preview && ./tools/cc_preview build/preview
 */
#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "render/cairo_buffer.h"
#include "shell/control_center.h"
#include "shell/topbar.h"

/* main.c owns these; the renderer only calls them from dismiss_control_center(). */
void server_handle_gesture_begin(aurade_server_t *server, gesture_action_type_t action) {
    (void)server; (void)action;
}
void server_handle_gesture_update(aurade_server_t *server, double dx, double dy, double s) {
    (void)server; (void)dx; (void)dy; (void)s;
}
void server_handle_gesture_end(aurade_server_t *server, bool c, double vx, double vy) {
    (void)server; (void)c; (void)vx; (void)vy;
}
void schedule_all_outputs(aurade_server_t *server) {
    (void)server;
}
void topbar_render(aurade_topbar_t *bar) {
    (void)bar;
}

static cairo_surface_t *g_background;

static void make_background(int w, int h) {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);

    cairo_pattern_t *lin = cairo_pattern_create_linear(0, 0, w, h);
    cairo_pattern_add_color_stop_rgb(lin, 0.0, 0.10, 0.16, 0.35);
    cairo_pattern_add_color_stop_rgb(lin, 0.45, 0.32, 0.20, 0.52);
    cairo_pattern_add_color_stop_rgb(lin, 1.0, 0.88, 0.47, 0.42);
    cairo_set_source(cr, lin);
    cairo_paint(cr);
    cairo_pattern_destroy(lin);

    cairo_pattern_t *rad = cairo_pattern_create_radial(w * 0.72, h * 0.18, 10,
                                                       w * 0.72, h * 0.18, w * 0.7);
    cairo_pattern_add_color_stop_rgba(rad, 0.0, 1.0, 0.72, 0.55, 0.55);
    cairo_pattern_add_color_stop_rgba(rad, 1.0, 0.0, 0.0, 0.0, 0.0);
    cairo_set_source(cr, rad);
    cairo_paint(cr);
    cairo_pattern_destroy(rad);

    /* A few soft "windows" so the material has something to sit over. */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    draw_rounded_rect(cr, w * 0.06, h * 0.16, w * 0.34, h * 0.30, 18);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.05, 0.05, 0.10, 0.22);
    draw_rounded_rect(cr, w * 0.10, h * 0.52, w * 0.46, h * 0.34, 18);
    cairo_fill(cr);

    cairo_destroy(cr);
    g_background = s;
}

/* Bounding box of everything the panel actually painted (shadow included). */
static void content_bounds(cairo_surface_t *s, int *ox, int *oy, int *ow, int *oh) {
    cairo_surface_flush(s);
    int w = cairo_image_surface_get_width(s);
    int h = cairo_image_surface_get_height(s);
    int stride = cairo_image_surface_get_stride(s);
    unsigned char *data = cairo_image_surface_get_data(s);

    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(data + (size_t)y * stride);
        for (int x = 0; x < w; x++) {
            if ((row[x] >> 24) != 0) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    if (maxx < 0) {
        *ox = 0; *oy = 0; *ow = w; *oh = h;
        return;
    }
    *ox = minx; *oy = miny;
    *ow = maxx - minx + 1;
    *oh = maxy - miny + 1;
}

typedef struct {
    const char *name;
    bool dark;
    bool media;
    int hover;      /* cc_module_t */
    double brightness;
    double volume;
    bool wifi_on;
    bool bt_on;
    bool playing;
    bool dnd;
    bool muted;
    bool unavailable;
    int pressed;
    const char *ssid;
    const char *bt_name;
} preview_state_t;

static cairo_buffer_t *paint_state(preview_state_t *st) {
    aurade_control_center_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.server = NULL;
    cc.active = true;
    cc.progress = 1.0;
    cc.dark_mode = st->dark;
    cc.dark_mode_available = true;
    cc.wifi_available = !st->unavailable;
    cc.wifi_enabled = st->wifi_on;
    snprintf(cc.wifi_ssid, sizeof(cc.wifi_ssid), "%s", st->ssid ? st->ssid : "Aurade-5G");
    cc.bluetooth_available = !st->unavailable;
    cc.bluetooth_enabled = st->bt_on;
    snprintf(cc.bluetooth_device, sizeof(cc.bluetooth_device), "%s",
             st->bt_name ? st->bt_name : "AirPods Pro");
    cc.dnd_available = !st->unavailable;
    cc.dnd_enabled = st->dnd;
    cc.brightness_available = !st->unavailable;
    cc.brightness = st->brightness;
    cc.volume_available = !st->unavailable;
    cc.volume = st->volume;
    cc.volume_muted = st->muted;
    cc.np_available = true;
    cc.np_active = st->media;
    cc.np_playing = st->playing;
    if (st->media) {
        snprintf(cc.np_title, sizeof(cc.np_title), "Nocturne in C-sharp minor");
        snprintf(cc.np_artist, sizeof(cc.np_artist), "Frédéric Chopin");
    }
    cc.battery_available = !st->unavailable;
    cc.battery_percent = 84;
    cc.battery_charging = false;
    cc.hotspot_available = !st->unavailable;
    cc.hotspot_enabled = false;
    cc.aura_connect_enabled = false;
    cc.screen_keyboard_enabled = false;
    cc.hover = st->hover;
    cc.pressed = st->pressed;

    // Allocate exactly what the panel needs, so the preview exercises the same
    // buffer geometry the compositor uses.
    cc.buffer = cairo_buffer_create(control_center_width(&cc),
                                    control_center_height(&cc));
    control_center_render(&cc);
    return cc.buffer;
}

static int write_preview(const char *dir, preview_state_t *st) {
    cairo_buffer_t *buf = paint_state(st);
    if (!buf) return 1;

    int ox, oy, ow, oh;
    content_bounds(buf->surface, &ox, &oy, &ow, &oh);

    int margin = 90;
    int W = ow + margin * 2;
    int H = oh + margin * 2;
    if (!g_background || cairo_image_surface_get_width(g_background) != W ||
        cairo_image_surface_get_height(g_background) != H) {
        if (g_background) cairo_surface_destroy(g_background);
        make_background(W, H);
    }

    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *cr = cairo_create(out);
    cairo_set_source_surface(cr, g_background, 0, 0);
    cairo_paint(cr);

    cairo_save(cr);
    cairo_set_source_surface(cr, buf->surface, margin - ox, margin - oy);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_destroy(cr);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", dir, st->name);
    cairo_surface_write_to_png(out, path);
    cairo_surface_destroy(out);

    wlr_buffer_drop(&buf->base);
    printf("%s  (%dx%d)\n", path, ow, oh);
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/preview";
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    if (system(cmd) != 0) return 1;

    preview_state_t states[] = {
        { .name = "cc-dark",  .dark = true,  .hover = CC_MODULE_NONE, .brightness = 0.82,
          .volume = 0.64, .wifi_on = true, .bt_on = true, .playing = true },
        { .name = "cc-light", .dark = false, .hover = CC_MODULE_NONE, .brightness = 0.82,
          .volume = 0.64, .wifi_on = true, .bt_on = false, .playing = true },
        { .name = "cc-media", .dark = true,  .hover = CC_MODULE_PLAY, .brightness = 0.55,
          .volume = 0.40, .wifi_on = false, .bt_on = true, .media = true, .playing = true },
        { .name = "cc-focus", .dark = true,  .hover = CC_MODULE_SOUND,
          .pressed = CC_MODULE_BLUETOOTH, .brightness = 0.30, .volume = 0.85,
          .wifi_on = true, .bt_on = true, .dnd = true },
        { .name = "cc-unavailable", .dark = false, .hover = CC_MODULE_DISPLAY,
          .brightness = 0.5, .volume = 0.5, .wifi_on = false, .bt_on = false,
          .unavailable = true, .muted = true },
        { .name = "cc-longtext", .dark = true, .hover = CC_MODULE_NONE,
          .brightness = 1.0, .volume = 0.0, .wifi_on = true, .bt_on = true,
          .media = true, .playing = false,
          .ssid = "Some Extremely Long Hotel Wi-Fi Network Name",
          .bt_name = "Sony WH-1000XM5 Wireless Headphones" },
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        rc |= write_preview(dir, &states[i]);
    }
    if (g_background) cairo_surface_destroy(g_background);
    return rc;
}
