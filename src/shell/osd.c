#include "osd.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define OSD_BUFFER_W 260
#define OSD_BUFFER_H 72
#define OSD_PILL_W   236.0
#define OSD_PILL_H   46.0
#define OSD_CORNER   23.0
#define OSD_PAD_X    12.0
#define OSD_PAD_Y    13.0

aurade_osd_t *osd_create(aurade_server_t *server) {
    aurade_osd_t *osd = calloc(1, sizeof(*osd));
    if (!osd) return NULL;
    osd->server = server;
    osd->screen_width = server->screen_width > 0 ? server->screen_width : 1920;
    osd->screen_height = server->screen_height > 0 ? server->screen_height : 1080;
    osd->type = OSD_TYPE_NONE;
    osd->level = 0.5;
    osd->muted = false;
    osd->alpha = 0.0;
    osd->timer = 0.0;
    osd->visible = false;

    osd->buffer = cairo_buffer_create(OSD_BUFFER_W, OSD_BUFFER_H);
    osd->scene_buffer = wlr_scene_buffer_create(server->overlay_tree, &osd->buffer->base);
    osd->blur_node = wlr_scene_blur_create(server->overlay_tree, (int)OSD_PILL_W, (int)OSD_PILL_H);

    int pos_x = (osd->screen_width - OSD_BUFFER_W) / 2;
    int pos_y = 54;
    wlr_scene_node_set_position(&osd->scene_buffer->node, pos_x, pos_y);
    wlr_scene_node_set_enabled(&osd->scene_buffer->node, false);

    if (osd->blur_node) {
        wlr_scene_node_place_below(&osd->blur_node->node, &osd->scene_buffer->node);
        wlr_scene_node_set_position(&osd->blur_node->node, pos_x + (int)OSD_PAD_X, pos_y + (int)OSD_PAD_Y);
        wlr_scene_blur_set_corner_radius(osd->blur_node, (int)OSD_CORNER);
        wlr_scene_blur_set_liquid_glass(osd->blur_node, server->config.glass_osd.liquid_glass);
        float str = (float)server->config.glass_osd.blur;
        wlr_scene_blur_set_strength(osd->blur_node, str);
        wlr_scene_node_set_enabled(&osd->blur_node->node, false);
    }

    return osd;
}

static int osd_fade_timer_cb(void *data) {
    aurade_osd_t *osd = data;
    if (!osd || !osd->visible) return 0;
    osd->timer = 0.0;
    if (osd->server) schedule_all_outputs(osd->server);
    return 0;
}

void osd_destroy(aurade_osd_t *osd) {
    if (!osd) return;
    if (osd->fade_timer) {
        wl_event_source_remove(osd->fade_timer);
        osd->fade_timer = NULL;
    }
    if (osd->buffer) {
        wlr_buffer_drop(&osd->buffer->base);
    }
    free(osd);
}

void osd_resize(aurade_osd_t *osd, int width, int height) {
    if (!osd) return;
    osd->screen_width = width;
    osd->screen_height = height;
    int pos_x = (width - OSD_BUFFER_W) / 2;
    int pos_y = 54;
    if (osd->scene_buffer) {
        wlr_scene_node_set_position(&osd->scene_buffer->node, pos_x, pos_y);
    }
    if (osd->blur_node) {
        wlr_scene_node_set_position(&osd->blur_node->node, pos_x + (int)OSD_PAD_X, pos_y + (int)OSD_PAD_Y);
    }
}

static void osd_arm_fade_timer(aurade_osd_t *osd, int delay_ms) {
    if (!osd || !osd->server || !osd->server->wl_display) return;
    if (!osd->fade_timer) {
        struct wl_event_loop *loop = wl_display_get_event_loop(osd->server->wl_display);
        osd->fade_timer = wl_event_loop_add_timer(loop, osd_fade_timer_cb, osd);
    }
    if (osd->fade_timer) {
        wl_event_source_timer_update(osd->fade_timer, delay_ms);
    }
}

void osd_show_volume(aurade_osd_t *osd, double level, bool muted) {
    if (!osd) return;
    if (level < 0.0) level = 0.0;
    if (level > 1.0) level = 1.0;

    osd->type = OSD_TYPE_VOLUME;
    osd->level = level;
    osd->muted = muted;
    osd->alpha = 1.0;
    osd->timer = 1.35;
    osd->visible = true;

    osd_arm_fade_timer(osd, 1350);

    if (osd->scene_buffer) {
        wlr_scene_node_set_enabled(&osd->scene_buffer->node, true);
        wlr_scene_node_raise_to_top(&osd->scene_buffer->node);
        if (osd->blur_node) {
            wlr_scene_node_place_below(&osd->blur_node->node, &osd->scene_buffer->node);
        }
    }
    osd_render(osd);
    if (osd->server) schedule_all_outputs(osd->server);
}

void osd_show_brightness(aurade_osd_t *osd, double level) {
    if (!osd) return;
    if (level < 0.0) level = 0.0;
    if (level > 1.0) level = 1.0;

    osd->type = OSD_TYPE_BRIGHTNESS;
    osd->level = level;
    osd->muted = false;
    osd->alpha = 1.0;
    osd->timer = 1.35;
    osd->visible = true;

    osd_arm_fade_timer(osd, 1350);

    if (osd->scene_buffer) {
        wlr_scene_node_set_enabled(&osd->scene_buffer->node, true);
        wlr_scene_node_raise_to_top(&osd->scene_buffer->node);
        if (osd->blur_node) {
            wlr_scene_node_place_below(&osd->blur_node->node, &osd->scene_buffer->node);
        }
    }
    osd_render(osd);
    if (osd->server) schedule_all_outputs(osd->server);
}

bool osd_is_animating(const aurade_osd_t *osd) {
    if (!osd || !osd->visible) return false;
    return osd->timer <= 0.0; // Only animating when actively fading out
}

void osd_tick(aurade_osd_t *osd, double dt) {
    if (!osd || !osd->visible) return;

    if (osd->timer > 0.0) {
        return; // Handled reactively by fade_timer without burning VBlank frames
    }

    osd->alpha -= dt * 3.8; // Smooth fade out in ~0.26s
    if (osd->alpha <= 0.0) {
        osd->alpha = 0.0;
        osd->visible = false;
        if (osd->scene_buffer) {
            wlr_scene_node_set_enabled(&osd->scene_buffer->node, false);
        }
        if (osd->blur_node) {
            wlr_scene_node_set_enabled(&osd->blur_node->node, false);
        }
        return;
    }
    osd_render(osd);
}

static void osd_draw_liquid_glass_capsule(cairo_t *cr, double x, double y, double w, double h,
                                          double r, double opacity) {
    cairo_save(cr);

    // 1. Refractive depth gradient body tint overlay
    draw_rounded_rect(cr, x, y, w, h, r);
    cairo_pattern_t *bg = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.14, 0.16, 0.22, 0.28 * opacity);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.06, 0.07, 0.10, 0.40 * opacity);
    cairo_set_source(cr, bg);
    cairo_fill(cr);
    cairo_pattern_destroy(bg);

    // 2. Caustic surface sheen wash across top 40%
    cairo_save(cr);
    draw_rounded_rect(cr, x, y, w, h, r);
    cairo_clip(cr);
    double sheen_h = h * 0.40;
    cairo_pattern_t *sheen = cairo_pattern_create_linear(x, y, x, y + sheen_h);
    cairo_pattern_add_color_stop_rgba(sheen, 0.0, 1.0, 1.0, 1.0, 0.04 * opacity);
    cairo_pattern_add_color_stop_rgba(sheen, 0.6, 1.0, 1.0, 1.0, 0.01 * opacity);
    cairo_pattern_add_color_stop_rgba(sheen, 1.0, 1.0, 1.0, 1.0, 0.0);
    cairo_rectangle(cr, x, y, w, sheen_h);
    cairo_set_source(cr, sheen);
    cairo_fill(cr);
    cairo_pattern_destroy(sheen);
    cairo_restore(cr);

    // 3. Refractive edge cushion
    draw_refractive_glass_rim(cr, x, y, w, h, r, 0.90 * opacity);

    cairo_restore(cr);
}

void osd_render(aurade_osd_t *osd) {
    if (!osd || !osd->buffer || !osd->buffer->cr) return;
    cairo_t *cr = osd->buffer->cr;
    cairo_buffer_clear(osd->buffer);

    double a = osd->alpha;
    if (a <= 0.001) {
        if (osd->blur_node) wlr_scene_node_set_enabled(&osd->blur_node->node, false);
        return;
    }
    if (a > 1.0) a = 1.0;

    glass_element_t go = osd->server ? osd->server->config.glass_osd : (glass_element_t){ 0.85, 1.0, true };
    bool liquid = go.liquid_glass;
    bool blur_enabled = (go.blur > 0.01 || liquid);

    if (osd->blur_node) {
        if (blur_enabled && a > 0.05) {
            wlr_scene_blur_set_liquid_glass(osd->blur_node, liquid);
            wlr_scene_blur_set_strength(osd->blur_node, (float)go.blur);
            wlr_scene_blur_set_alpha(osd->blur_node, (float)a);
            wlr_scene_node_set_enabled(&osd->blur_node->node, true);
            wlr_scene_node_place_below(&osd->blur_node->node, &osd->scene_buffer->node);
        } else {
            wlr_scene_node_set_enabled(&osd->blur_node->node, false);
        }
    }

    double px = OSD_PAD_X;
    double py = OSD_PAD_Y;
    double pw = OSD_PILL_W;
    double ph = OSD_PILL_H;
    double pr = OSD_CORNER;

    // Capsule body (Liquid Glass when enabled, frosted glass fallback otherwise)
    if (liquid) {
        osd_draw_liquid_glass_capsule(cr, px, py, pw, ph, pr, a * go.opacity);
    } else {
        draw_glass_panel(cr, px, py, pw, ph, pr, 0.11, 0.12, 0.15, go.opacity * a);
    }

    // Left Icon (Speaker or Sun)
    double icon_cx = px + 25.0;
    double icon_cy = py + ph / 2.0;

    if (osd->type == OSD_TYPE_VOLUME) {
        draw_speaker_icon(cr, icon_cx, icon_cy, 18.0, 0.95 * a);
        if (osd->muted) {
            // Crisp red slash across speaker
            cairo_set_source_rgba(cr, 1.0, 0.28, 0.28, 0.95 * a);
            cairo_set_line_width(cr, 2.0);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_move_to(cr, icon_cx - 9.0, icon_cy + 8.0);
            cairo_line_to(cr, icon_cx + 9.0, icon_cy - 8.0);
            cairo_stroke(cr);
        }
    } else if (osd->type == OSD_TYPE_BRIGHTNESS) {
        draw_sun_icon(cr, icon_cx, icon_cy, 19.0, 0.95 * a);
    }

    // Slider track
    double track_x = px + 48.0;
    double track_h = 7.0;
    double track_y = py + (ph - track_h) / 2.0;
    double track_w = 126.0;
    double track_r = track_h / 2.0;

    // Background track
    draw_rounded_rect(cr, track_x, track_y, track_w, track_h, track_r);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22 * a);
    cairo_fill(cr);

    // Filled bar
    double fill_p = (osd->type == OSD_TYPE_VOLUME && osd->muted) ? 0.0 : osd->level;
    if (fill_p > 1.0) fill_p = 1.0;
    if (fill_p < 0.0) fill_p = 0.0;
    double fill_w = track_w * fill_p;

    if (fill_w > 0.5) {
        if (fill_w < track_h) fill_w = track_h;
        draw_rounded_rect(cr, track_x, track_y, fill_w, track_h, track_r);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * a);
        cairo_fill(cr);
    }

    // Right-hand percentage text
    char str[32];
    if (osd->type == OSD_TYPE_VOLUME && osd->muted) {
        snprintf(str, sizeof(str), "Mute");
    } else {
        snprintf(str, sizeof(str), "%d%%", (int)lround(osd->level * 100.0));
    }

    draw_text(cr, str, NULL, 11.5, true,
              1.0, 1.0, 1.0, 0.92 * a,
              track_x + track_w + 14.0, py + ph / 2.0, false, true);

    cairo_buffer_apply_to_scene(osd->buffer, osd->scene_buffer);
}
