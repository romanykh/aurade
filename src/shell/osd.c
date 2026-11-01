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

    int pos_x = (osd->screen_width - OSD_BUFFER_W) / 2;
    int pos_y = 54;
    wlr_scene_node_set_position(&osd->scene_buffer->node, pos_x, pos_y);
    wlr_scene_node_set_enabled(&osd->scene_buffer->node, false);

    return osd;
}

void osd_destroy(aurade_osd_t *osd) {
    if (!osd) return;
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

    if (osd->scene_buffer) {
        wlr_scene_node_set_enabled(&osd->scene_buffer->node, true);
        wlr_scene_node_raise_to_top(&osd->scene_buffer->node);
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

    if (osd->scene_buffer) {
        wlr_scene_node_set_enabled(&osd->scene_buffer->node, true);
        wlr_scene_node_raise_to_top(&osd->scene_buffer->node);
    }
    osd_render(osd);
    if (osd->server) schedule_all_outputs(osd->server);
}

void osd_tick(aurade_osd_t *osd, double dt) {
    if (!osd || !osd->visible) return;

    if (osd->timer > 0.0) {
        osd->timer -= dt;
    } else {
        osd->alpha -= dt * 3.8; // Smooth fade out in ~0.26s
        if (osd->alpha <= 0.0) {
            osd->alpha = 0.0;
            osd->visible = false;
            if (osd->scene_buffer) {
                wlr_scene_node_set_enabled(&osd->scene_buffer->node, false);
            }
            return;
        }
        osd_render(osd);
    }
}

void osd_render(aurade_osd_t *osd) {
    if (!osd || !osd->buffer || !osd->buffer->cr) return;
    cairo_t *cr = osd->buffer->cr;
    cairo_buffer_clear(osd->buffer);

    double a = osd->alpha;
    if (a <= 0.001) return;
    if (a > 1.0) a = 1.0;

    double px = OSD_PAD_X;
    double py = OSD_PAD_Y;
    double pw = OSD_PILL_W;
    double ph = OSD_PILL_H;
    double pr = OSD_CORNER;

    // Drop shadow
    draw_drop_shadow(cr, px, py, pw, ph, pr, 10.0, 0.42 * a);

    // Frosted glass capsule body
    draw_glass_panel(cr, px, py, pw, ph, pr, 0.11, 0.12, 0.15, 0.86 * a);

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
