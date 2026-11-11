#include "cairo_buffer.h"
#include <drm_fourcc.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void cairo_buffer_destroy_wlr(struct wlr_buffer *wlr_buffer) {
    cairo_buffer_t *buf = wl_container_of(wlr_buffer, buf, base);
    if (buf->cr) cairo_destroy(buf->cr);
    if (buf->surface) cairo_surface_destroy(buf->surface);
    free(buf);
}

static bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
        uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    (void)flags;
    cairo_buffer_t *buf = wl_container_of(wlr_buffer, buf, base);
    cairo_surface_flush(buf->surface);
    *data = cairo_image_surface_get_data(buf->surface);
    *stride = cairo_image_surface_get_stride(buf->surface);
    *format = DRM_FORMAT_ARGB8888;
    return true;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
    cairo_buffer_t *buf = wl_container_of(wlr_buffer, buf, base);
    cairo_surface_mark_dirty(buf->surface);
}

static const struct wlr_buffer_impl cairo_buffer_impl = {
    .destroy = cairo_buffer_destroy_wlr,
    .begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
    .end_data_ptr_access = cairo_buffer_end_data_ptr_access,
};

cairo_buffer_t *cairo_buffer_create(int width, int height) {
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;

    cairo_buffer_t *buf = calloc(1, sizeof(*buf));
    if (!buf) return NULL;

    buf->width = width;
    buf->height = height;
    buf->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    buf->cr = cairo_create(buf->surface);

    cairo_save(buf->cr);
    cairo_set_operator(buf->cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(buf->cr);
    cairo_restore(buf->cr);

    wlr_buffer_init(&buf->base, &cairo_buffer_impl, width, height);
    return buf;
}

void cairo_buffer_resize(cairo_buffer_t *buf, int width, int height) {
    if (!buf) return;
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;
    if (buf->width == width && buf->height == height) return;

    if (buf->cr) cairo_destroy(buf->cr);
    if (buf->surface) cairo_surface_destroy(buf->surface);

    buf->width = width;
    buf->height = height;
    buf->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    buf->cr = cairo_create(buf->surface);

    cairo_save(buf->cr);
    cairo_set_operator(buf->cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(buf->cr);
    cairo_restore(buf->cr);

    buf->base.width = width;
    buf->base.height = height;
}

void cairo_buffer_clear(cairo_buffer_t *buf) {
    if (!buf || !buf->cr) return;
    cairo_save(buf->cr);
    cairo_set_operator(buf->cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(buf->cr);
    cairo_restore(buf->cr);
}

void cairo_buffer_apply_to_scene(cairo_buffer_t *buf, struct wlr_scene_buffer *scene_buf) {
    if (!buf || !scene_buf) return;
    cairo_surface_flush(buf->surface);
    wlr_scene_buffer_set_buffer_with_damage(scene_buf, &buf->base, NULL);
}

void draw_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

void draw_glass_panel(cairo_t *cr, double x, double y, double w, double h, double r,
                      double r_bg, double g_bg, double b_bg, double a_bg) {
    if (w <= 0.0 || h <= 0.0) return;
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;
    cairo_save(cr);

    double edge_a = a_bg > 1.0 ? 1.0 : (a_bg < 0.0 ? 0.0 : a_bg);

    // 1. Refractive depth gradient body fill (optical density absorption)
    cairo_pattern_t *body_pat = cairo_pattern_create_linear(x, y, x, y + h);
    double top_r = r_bg * 1.15; if (top_r > 1.0) top_r = 1.0;
    double top_g = g_bg * 1.15; if (top_g > 1.0) top_g = 1.0;
    double top_b = b_bg * 1.22; if (top_b > 1.0) top_b = 1.0;
    double bot_r = r_bg * 0.88;
    double bot_g = g_bg * 0.88;
    double bot_b = b_bg * 0.92;
    double top_a = a_bg * 0.90;
    double bot_a = a_bg * 1.06; if (bot_a > 1.0) bot_a = 1.0;

    cairo_pattern_add_color_stop_rgba(body_pat, 0.0, top_r, top_g, top_b, top_a);
    cairo_pattern_add_color_stop_rgba(body_pat, 1.0, bot_r, bot_g, bot_b, bot_a);

    draw_rounded_rect(cr, x, y, w, h, r);
    cairo_set_source(cr, body_pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(body_pat);

    // 2. Specular border gradient (diagonal light strike: top-left bright, bottom-right subtle)
    cairo_pattern_t *border_pat = cairo_pattern_create_linear(x, y, x + w, y + h);
    cairo_pattern_add_color_stop_rgba(border_pat, 0.0, 1.0, 1.0, 1.0, 0.45 * edge_a);
    cairo_pattern_add_color_stop_rgba(border_pat, 0.35, 1.0, 1.0, 1.0, 0.22 * edge_a);
    cairo_pattern_add_color_stop_rgba(border_pat, 0.70, 1.0, 1.0, 1.0, 0.12 * edge_a);
    cairo_pattern_add_color_stop_rgba(border_pat, 1.0, 1.0, 1.0, 1.0, 0.06 * edge_a);

    cairo_set_source(cr, border_pat);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_pattern_destroy(border_pat);

    // 3. Caustic surface sheen wash across the top ~40% of the glass
    if (h > 6.0 && edge_a > 0.05) {
        cairo_save(cr);
        draw_rounded_rect(cr, x, y, w, h, r);
        cairo_clip(cr);

        double sheen_h = h * 0.42;
        if (sheen_h > 48.0) sheen_h = 48.0;
        if (sheen_h < 4.0) sheen_h = 4.0;
        cairo_pattern_t *sheen = cairo_pattern_create_linear(x, y, x, y + sheen_h);
        cairo_pattern_add_color_stop_rgba(sheen, 0.0, 1.0, 1.0, 1.0, 0.14 * edge_a);
        cairo_pattern_add_color_stop_rgba(sheen, 0.5, 1.0, 1.0, 1.0, 0.05 * edge_a);
        cairo_pattern_add_color_stop_rgba(sheen, 1.0, 1.0, 1.0, 1.0, 0.0);
        cairo_rectangle(cr, x, y, w, sheen_h);
        cairo_set_source(cr, sheen);
        cairo_fill(cr);
        cairo_pattern_destroy(sheen);
        cairo_restore(cr);
    }

    // 4. Crisp top-edge specular crest (micro-bevel light reflection)
    if (w > 2.0 * r && edge_a > 0.05) {
        cairo_new_path(cr);
        cairo_move_to(cr, x + r, y + 0.5);
        cairo_line_to(cr, x + w - r, y + 0.5);
        cairo_pattern_t *crest = cairo_pattern_create_linear(x + r, y, x + w - r, y);
        cairo_pattern_add_color_stop_rgba(crest, 0.0, 1.0, 1.0, 1.0, 0.20 * edge_a);
        cairo_pattern_add_color_stop_rgba(crest, 0.5, 1.0, 1.0, 1.0, 0.48 * edge_a);
        cairo_pattern_add_color_stop_rgba(crest, 1.0, 1.0, 1.0, 1.0, 0.20 * edge_a);
        cairo_set_source(cr, crest);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_pattern_destroy(crest);
    }

    // 5. Bottom grounding edge (dark hairline rim for physical plate thickness)
    if (w > 2.0 * r && h > 8.0 && edge_a > 0.10) {
        cairo_new_path(cr);
        cairo_move_to(cr, x + r, y + h - 0.5);
        cairo_line_to(cr, x + w - r, y + h - 0.5);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.28 * edge_a);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

void draw_drop_shadow(cairo_t *cr, double x, double y, double w, double h,
                      double r, double blur_size, double max_alpha) {
    if (blur_size <= 0.0 || max_alpha <= 0.0) return;
    cairo_save(cr);

    int steps = (int)(blur_size / 3.5);
    if (steps < 2) steps = 2;
    if (steps > 5) steps = 5;

    for (int i = steps; i >= 1; i--) {
        double spread = ((double)i / steps) * blur_size;
        double alpha = (max_alpha / (steps * 1.25)) * (1.0 - (double)i / (steps * 1.15));
        if (alpha < 0.0) alpha = 0.0;

        draw_rounded_rect(cr, x - spread + 1.0, y - spread + (spread * 0.5),
                          w + spread * 2.0, h + spread * 2.0, r + spread);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, alpha);
        cairo_fill(cr);
    }

    cairo_restore(cr);
}

static void apply_font_desc(PangoFontDescription *desc, const char *font, double size, bool bold) {
    char fam_buf[256];
    if (font && *font) {
        if (strstr(font, "Noto Sans CJK") == NULL) {
            snprintf(fam_buf, sizeof(fam_buf), "%s, Noto Sans CJK JP, Noto Sans CJK SC, Noto Sans CJK KR, Adwaita Sans, Noto Sans, sans-serif", font);
        } else {
            snprintf(fam_buf, sizeof(fam_buf), "%s", font);
        }
    } else {
        snprintf(fam_buf, sizeof(fam_buf), "Inter, Noto Sans CJK JP, Noto Sans CJK SC, Noto Sans CJK KR, Adwaita Sans, Noto Sans, Helvetica, sans-serif");
    }
    pango_font_description_set_family(desc, fam_buf);
    pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
}

static inline void safe_pango_set_text(PangoLayout *layout, const char *text) {
    if (!layout || !text) return;
    if (g_utf8_validate(text, -1, NULL)) {
        pango_layout_set_text(layout, text, -1);
    } else {
        char *valid = g_utf8_make_valid(text, -1);
        pango_layout_set_text(layout, valid, -1);
        g_free(valid);
    }
}

void draw_text(cairo_t *cr, const char *text, const char *font, double size,
               bool bold, double r, double g, double b, double a,
               double x, double y, bool center_x, bool center_y) {
    if (!text || !*text || a <= 0.0) return;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    apply_font_desc(desc, font, size, bold);

    pango_layout_set_font_description(layout, desc);
    safe_pango_set_text(layout, text);

    int text_w = 0, text_h = 0;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    double draw_x = x;
    double draw_y = y;
    if (center_x) draw_x -= (double)text_w / 2.0;
    if (center_y) draw_y -= (double)text_h / 2.0;

    cairo_save(cr);
    cairo_new_path(cr);
    cairo_move_to(cr, draw_x, draw_y);
    cairo_set_source_rgba(cr, r, g, b, a);
    pango_cairo_show_layout(cr, layout);
    cairo_new_path(cr);
    cairo_restore(cr);
    cairo_new_path(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
}

void draw_text_shadowed(cairo_t *cr, const char *text, const char *font, double size,
                        bool bold, double r, double g, double b, double a,
                        double shadow_a, double x, double y, bool center_x, bool center_y) {
    if (!text || !*text) return;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    apply_font_desc(desc, font, size, bold);

    pango_layout_set_font_description(layout, desc);
    safe_pango_set_text(layout, text);

    int text_w = 0, text_h = 0;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    double draw_x = x;
    double draw_y = y;
    if (center_x) draw_x -= (double)text_w / 2.0;
    if (center_y) draw_y -= (double)text_h / 2.0;

    cairo_save(cr);
    cairo_new_path(cr);
    if (shadow_a > 0.0) {
        cairo_move_to(cr, draw_x, draw_y + 1.0);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, shadow_a);
        pango_cairo_show_layout(cr, layout);
    }
    if (a > 0.0) {
        cairo_new_path(cr);
        cairo_move_to(cr, draw_x, draw_y);
        cairo_set_source_rgba(cr, r, g, b, a);
        pango_cairo_show_layout(cr, layout);
    }
    cairo_new_path(cr);
    cairo_restore(cr);
    cairo_new_path(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
}

void draw_text_shadowed_wrapped(cairo_t *cr, const char *text, const char *font, double size,
                                bool bold, double r, double g, double b, double a,
                                double shadow_a, double cx, double y,
                                double max_w, int max_lines) {
    if (!text || !*text || a <= 0.0) return;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    apply_font_desc(desc, font, size, bold);
    pango_layout_set_font_description(layout, desc);
    safe_pango_set_text(layout, text);

    if (max_w > 0.0) {
        pango_layout_set_width(layout, (int)(max_w * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        if (max_lines > 0) {
            pango_layout_set_height(layout, -max_lines);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        }
    }

    double draw_x = (max_w > 0.0) ? (cx - max_w / 2.0) : cx;
    double draw_y = y;

    cairo_save(cr);
    cairo_new_path(cr);
    if (shadow_a > 0.0) {
        cairo_move_to(cr, draw_x, draw_y + 1.0);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, shadow_a);
        pango_cairo_show_layout(cr, layout);
    }
    if (a > 0.0) {
        cairo_new_path(cr);
        cairo_move_to(cr, draw_x, draw_y);
        cairo_set_source_rgba(cr, r, g, b, a);
        pango_cairo_show_layout(cr, layout);
    }
    cairo_new_path(cr);
    cairo_restore(cr);
    cairo_new_path(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
}

double text_width(cairo_t *cr, const char *text, const char *font, double size, bool bold) {
    if (!text || !*text) return 0.0;
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    apply_font_desc(desc, font, size, bold);
    pango_layout_set_font_description(layout, desc);
    safe_pango_set_text(layout, text);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    pango_font_description_free(desc);
    g_object_unref(layout);
    return (double)w;
}

void draw_text_clamped(cairo_t *cr, const char *text, const char *font,
                       double size, bool bold, double r, double g, double b, double a,
                       double x, double y, double max_w) {
    if (!text || !*text || a <= 0.0) return;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    apply_font_desc(desc, font, size, bold);
    pango_layout_set_font_description(layout, desc);
    safe_pango_set_text(layout, text);
    pango_layout_set_single_paragraph_mode(layout, TRUE);

    if (max_w > 0.0) {
        pango_layout_set_width(layout, (int)(max_w * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    }

    int text_w = 0, text_h = 0;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    cairo_save(cr);
    cairo_new_path(cr);
    cairo_move_to(cr, x, y - (double)text_h / 2.0);
    cairo_set_source_rgba(cr, r, g, b, a);
    pango_cairo_show_layout(cr, layout);
    cairo_new_path(cr);
    cairo_restore(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
}

// -------------------------------------------------------------
// Vector status / UI icons
// -------------------------------------------------------------

// Glyphs are drawn in a single tint colour so the same vector icons work on
// dark glass, on a blue toggle and on a near-white slider fill. Callers set the
// tint immediately before drawing and restore white afterwards.
static double g_icon_rgb[3] = {1.0, 1.0, 1.0};

void draw_icon_tint(double r, double g, double b) {
    g_icon_rgb[0] = r;
    g_icon_rgb[1] = g;
    g_icon_rgb[2] = b;
}

void draw_sun_logo(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);

    // Minimalistic sun core disk
    cairo_arc(cr, cx, cy, s * 0.22, 0, 2 * M_PI);
    cairo_fill(cr);

    // 8 sleek minimalist sun rays
    cairo_set_line_width(cr, s * 0.095);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 8; i++) {
        double ang = (double)i * (M_PI / 4.0);
        cairo_new_path(cr);
        cairo_move_to(cr, cx + cos(ang) * s * 0.32, cy + sin(ang) * s * 0.32);
        cairo_line_to(cr, cx + cos(ang) * s * 0.47, cy + sin(ang) * s * 0.47);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}


void draw_wifi_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_width(cr, s * 0.10);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    double base_y = cy + s * 0.24;
    for (int i = 0; i < 3; i++) {
        double r = s * (0.20 + i * 0.19);
        cairo_new_path(cr);
        cairo_arc(cr, cx, base_y, r, -M_PI * 0.75, -M_PI * 0.25);
        cairo_stroke(cr);
    }
    cairo_arc(cr, cx, base_y, s * 0.075, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_battery_icon(cairo_t *cr, double x, double y, double w, double h,
                       double level, double a, bool charging) {
    if (level < 0.0) level = 0.0;
    if (level > 1.0) level = 1.0;
    cairo_save(cr);
    cairo_set_line_width(cr, 1.1);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], 0.55 * a);
    draw_rounded_rect(cr, x, y, w, h, h * 0.32);
    cairo_stroke(cr);

    // Terminal nub
    draw_rounded_rect(cr, x + w + 1.0, y + h * 0.30, 1.8, h * 0.40, 0.9);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], 0.45 * a);
    cairo_fill(cr);

    double inset = 1.8;
    double fill_w = (w - inset * 2.0) * level;
    if (fill_w > 0.6) {
        draw_rounded_rect(cr, x + inset, y + inset, fill_w, h - inset * 2.0, h * 0.16);
        if (level <= 0.15) cairo_set_source_rgba(cr, 1.0, 0.35, 0.30, 0.95 * a);
        else cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], 0.90 * a);
        cairo_fill(cr);
    }

    if (charging) {
        cairo_new_path(cr);
        cairo_move_to(cr, x + w * 0.52, y + h * 0.16);
        cairo_line_to(cr, x + w * 0.34, y + h * 0.54);
        cairo_line_to(cr, x + w * 0.50, y + h * 0.54);
        cairo_line_to(cr, x + w * 0.42, y + h * 0.86);
        cairo_line_to(cr, x + w * 0.66, y + h * 0.44);
        cairo_line_to(cr, x + w * 0.50, y + h * 0.44);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 0.05, 0.05, 0.06, 0.9 * a);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

void draw_control_center_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);

    // Two toggle switches, like the macOS Control Center menu-bar glyph.
    double w = s * 0.44, h = s * 0.24;
    draw_rounded_rect(cr, cx - s * 0.30, cy - s * 0.30, w, h, h / 2.0);
    cairo_fill(cr);
    cairo_arc(cr, cx - s * 0.30 + w - h / 2.0, cy - s * 0.30 + h / 2.0, h * 0.32, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.10, 0.10, 0.12, a);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    draw_rounded_rect(cr, cx - s * 0.14, cy + s * 0.06, w, h, h / 2.0);
    cairo_fill(cr);
    cairo_arc(cr, cx - s * 0.14 + h / 2.0, cy + s * 0.06 + h / 2.0, h * 0.32, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.10, 0.10, 0.12, a);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_bluetooth_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    double h = s * 0.5;
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_width(cr, s * 0.11);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_new_path(cr);
    cairo_move_to(cr, cx - h * 0.34, cy - h * 0.60);
    cairo_line_to(cr, cx + h * 0.38, cy + h * 0.60);
    cairo_line_to(cr, cx, cy + h * 0.92);
    cairo_line_to(cr, cx, cy - h * 0.92);
    cairo_line_to(cr, cx + h * 0.38, cy - h * 0.60);
    cairo_line_to(cr, cx - h * 0.34, cy + h * 0.60);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void draw_moon_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    // Even-odd fill of two offset circles: a true crescent rather than the
    // thin ring that two concentric arcs produce at small sizes.
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, s * 0.46, 0, 2 * M_PI);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx + s * 0.25, cy - s * 0.10, s * 0.40, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_half_circle_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_arc(cr, cx, cy, s * 0.42, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], 0.20 * a);
    cairo_fill(cr);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, s * 0.42, -M_PI / 2.0, M_PI / 2.0);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_sun_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_width(cr, s * 0.085);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, cx, cy, s * 0.16, 0, 2 * M_PI);
    cairo_fill(cr);
    for (int i = 0; i < 8; i++) {
        double ang = i * M_PI / 4.0;
        cairo_new_path(cr);
        cairo_move_to(cr, cx + cos(ang) * s * 0.28, cy + sin(ang) * s * 0.28);
        cairo_line_to(cr, cx + cos(ang) * s * 0.42, cy + sin(ang) * s * 0.42);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

void draw_speaker_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_new_path(cr);
    cairo_move_to(cr, cx - s * 0.42, cy - s * 0.13);
    cairo_line_to(cr, cx - s * 0.22, cy - s * 0.13);
    cairo_line_to(cr, cx - s * 0.02, cy - s * 0.34);
    cairo_line_to(cr, cx - s * 0.02, cy + s * 0.34);
    cairo_line_to(cr, cx - s * 0.22, cy + s * 0.13);
    cairo_line_to(cr, cx - s * 0.42, cy + s * 0.13);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_line_width(cr, s * 0.075);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_new_path(cr);
    cairo_arc(cr, cx - s * 0.02, cy, s * 0.26, -M_PI * 0.32, M_PI * 0.32);
    cairo_stroke(cr);
    cairo_new_path(cr);
    cairo_arc(cr, cx - s * 0.02, cy, s * 0.44, -M_PI * 0.30, M_PI * 0.30);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void draw_search_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_width(cr, s * 0.10);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, cx - s * 0.08, cy - s * 0.08, s * 0.26, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, cx + s * 0.12, cy + s * 0.12);
    cairo_line_to(cr, cx + s * 0.34, cy + s * 0.34);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void draw_cpu_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    double die_sz = s * 0.64;
    double half = die_sz / 2.0;

    // Outer chip body with rounded corners
    draw_rounded_rect(cr, cx - half, cy - half, die_sz, die_sz, 1.8);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a * 0.90);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // Micro pins (2 pins extending outward from each edge)
    double pin_len = s * 0.16;
    double pin_spacing = die_sz * 0.28;
    cairo_set_line_width(cr, 0.9);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    for (int dir = -1; dir <= 1; dir += 2) {
        // Horizontal pins
        cairo_move_to(cr, cx - half, cy + dir * pin_spacing);
        cairo_line_to(cr, cx - half - pin_len, cy + dir * pin_spacing);
        cairo_move_to(cr, cx + half, cy + dir * pin_spacing);
        cairo_line_to(cr, cx + half + pin_len, cy + dir * pin_spacing);

        // Vertical pins
        cairo_move_to(cr, cx + dir * pin_spacing, cy - half);
        cairo_line_to(cr, cx + dir * pin_spacing, cy - half - pin_len);
        cairo_move_to(cr, cx + dir * pin_spacing, cy + half);
        cairo_line_to(cr, cx + dir * pin_spacing, cy + half + pin_len);
    }
    cairo_stroke(cr);

    // Thermal indicator core dot
    cairo_arc(cr, cx, cy, die_sz * 0.22, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_fill(cr);

    cairo_restore(cr);
}

void draw_chevron(cairo_t *cr, double cx, double cy, double s, int direction, double a) {
    // direction: 0 = left, 1 = right, 2 = up, 3 = down
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_width(cr, s * 0.16);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    double r = s * 0.30;
    cairo_new_path(cr);
    if (direction == 0) {
        cairo_move_to(cr, cx + r * 0.5, cy - r);
        cairo_line_to(cr, cx - r * 0.5, cy);
        cairo_line_to(cr, cx + r * 0.5, cy + r);
    } else if (direction == 1) {
        cairo_move_to(cr, cx - r * 0.5, cy - r);
        cairo_line_to(cr, cx + r * 0.5, cy);
        cairo_line_to(cr, cx - r * 0.5, cy + r);
    } else if (direction == 2) {
        cairo_move_to(cr, cx - r, cy + r * 0.5);
        cairo_line_to(cr, cx, cy - r * 0.5);
        cairo_line_to(cr, cx + r, cy + r * 0.5);
    } else {
        cairo_move_to(cr, cx - r, cy - r * 0.5);
        cairo_line_to(cr, cx, cy + r * 0.5);
        cairo_line_to(cr, cx + r, cy - r * 0.5);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

void draw_gear_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    // Eight teeth around a ring: reads as a gear at 14–20 px, unlike the
    // classic 50-line path which turns to mush at menu-bar sizes.
    cairo_set_line_width(cr, s * 0.14);
    for (int i = 0; i < 8; i++) {
        double ang = i * M_PI / 4.0 + M_PI / 8.0;
        cairo_move_to(cr, cx + cos(ang) * s * 0.28, cy + sin(ang) * s * 0.28);
        cairo_line_to(cr, cx + cos(ang) * s * 0.44, cy + sin(ang) * s * 0.44);
        cairo_stroke(cr);
    }

    cairo_new_path(cr);
    cairo_set_line_width(cr, s * 0.15);
    cairo_arc(cr, cx, cy, s * 0.24, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_new_path(cr);
    cairo_restore(cr);
    cairo_new_path(cr);
}

void draw_sliders_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_width(cr, s * 0.11);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    static const double knob[3] = {0.30, -0.22, 0.10};
    for (int i = 0; i < 3; i++) {
        double y = cy + (i - 1) * s * 0.30;
        cairo_move_to(cr, cx - s * 0.44, y);
        cairo_line_to(cr, cx + s * 0.44, y);
        cairo_stroke(cr);
        cairo_arc(cr, cx + s * knob[i], y, s * 0.11, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.10, 0.10, 0.12, a);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    }
    cairo_restore(cr);
}

void draw_music_note_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    // Stem
    cairo_set_line_width(cr, s * 0.10);
    cairo_move_to(cr, cx + s * 0.16, cy + s * 0.02);
    cairo_line_to(cr, cx + s * 0.16, cy - s * 0.42);
    cairo_stroke(cr);
    // Flag
    cairo_set_line_width(cr, s * 0.11);
    cairo_move_to(cr, cx + s * 0.16, cy - s * 0.42);
    cairo_curve_to(cr, cx + s * 0.38, cy - s * 0.32,
                   cx + s * 0.42, cy - s * 0.16, cx + s * 0.30, cy - s * 0.10);
    cairo_stroke(cr);
    // Note head
    cairo_save(cr);
    cairo_translate(cr, cx - s * 0.06, cy + s * 0.22);
    cairo_rotate(cr, -0.35);
    cairo_scale(cr, 1.0, 0.78);
    cairo_arc(cr, 0, 0, s * 0.21, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_restore(cr);
}

void draw_play_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_move_to(cr, cx - s * 0.22, cy - s * 0.34);
    cairo_line_to(cr, cx + s * 0.34, cy);
    cairo_line_to(cr, cx - s * 0.22, cy + s * 0.34);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_pause_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    draw_rounded_rect(cr, cx - s * 0.26, cy - s * 0.32, s * 0.19, s * 0.64, s * 0.06);
    cairo_fill(cr);
    draw_rounded_rect(cr, cx + s * 0.07, cy - s * 0.32, s * 0.19, s * 0.64, s * 0.06);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_skip_icon(cairo_t *cr, double cx, double cy, double s, int direction, double a) {
    // direction: 1 = next, -1 = previous
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, direction >= 0 ? 1.0 : -1.0, 1.0);
    cairo_move_to(cr, -s * 0.34, -s * 0.30);
    cairo_line_to(cr, s * 0.14, 0);
    cairo_line_to(cr, -s * 0.34, s * 0.30);
    cairo_close_path(cr);
    cairo_fill(cr);
    draw_rounded_rect(cr, s * 0.20, -s * 0.30, s * 0.12, s * 0.60, s * 0.05);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_airdrop_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    double cy_adj = cy - s * 0.04;

    // Upward arrow / chevron at bottom
    cairo_set_line_width(cr, s * 0.10);
    cairo_move_to(cr, cx - s * 0.16, cy_adj + s * 0.30);
    cairo_line_to(cr, cx, cy_adj + s * 0.14);
    cairo_line_to(cr, cx + s * 0.16, cy_adj + s * 0.30);
    cairo_stroke(cr);

    // Inner arc
    cairo_arc(cr, cx, cy_adj, s * 0.22, M_PI * 0.5 + 0.58, M_PI * 2.5 - 0.58);
    cairo_stroke(cr);

    // Outer arc
    cairo_arc(cr, cx, cy_adj, s * 0.38, M_PI * 0.5 + 0.50, M_PI * 2.5 - 0.50);
    cairo_stroke(cr);

    cairo_restore(cr);
}

void draw_hotspot_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_width(cr, s * 0.10);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, -M_PI / 4.0);

    double rw = s * 0.44;
    double rh = s * 0.28;
    double r = rh / 2.0;

    // Loop 1
    draw_rounded_rect(cr, -s * 0.36, -rh / 2.0, rw, rh, r);
    cairo_stroke(cr);

    // Loop 2
    cairo_new_path(cr);
    draw_rounded_rect(cr, -s * 0.08, -rh / 2.0, rw, rh, r);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_restore(cr);
    cairo_new_path(cr);
}

void draw_keyboard_brightness_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    cairo_set_line_width(cr, s * 0.10);
    cairo_move_to(cr, cx - s * 0.38, cy + s * 0.26);
    cairo_line_to(cr, cx + s * 0.38, cy + s * 0.26);
    cairo_stroke(cr);

    cairo_set_line_width(cr, s * 0.095);
    for (int i = 0; i < 5; i++) {
        double ang = -M_PI / 2.0 + (i - 2) * (M_PI / 6.0);
        double r1 = s * 0.14;
        double r2 = s * 0.30;
        cairo_move_to(cr, cx + cos(ang) * r1, cy + s * 0.14 + sin(ang) * r1);
        cairo_line_to(cr, cx + cos(ang) * r2, cy + s * 0.14 + sin(ang) * r2);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

void draw_aura_connect_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    // Monitor on left
    cairo_set_line_width(cr, s * 0.085);
    draw_rounded_rect(cr, cx - s * 0.44, cy - s * 0.32, s * 0.48, s * 0.36, s * 0.06);
    cairo_stroke(cr);

    // Monitor stand neck & base
    cairo_new_path(cr);
    cairo_move_to(cr, cx - s * 0.20, cy + s * 0.04);
    cairo_line_to(cr, cx - s * 0.20, cy + s * 0.14);
    cairo_move_to(cr, cx - s * 0.32, cy + s * 0.14);
    cairo_line_to(cr, cx - s * 0.08, cy + s * 0.14);
    cairo_stroke(cr);

    // Smartphone on right
    cairo_new_path(cr);
    cairo_set_line_width(cr, s * 0.085);
    draw_rounded_rect(cr, cx + s * 0.12, cy - s * 0.14, s * 0.28, s * 0.44, s * 0.07);
    cairo_stroke(cr);

    // Phone home bar
    cairo_new_path(cr);
    cairo_set_line_width(cr, s * 0.07);
    cairo_move_to(cr, cx + s * 0.21, cy + s * 0.22);
    cairo_line_to(cr, cx + s * 0.31, cy + s * 0.22);
    cairo_stroke(cr);

    // Connection wave / signal link between them
    cairo_new_path(cr);
    cairo_set_line_width(cr, s * 0.075);
    cairo_arc(cr, cx + s * 0.12, cy - s * 0.04, s * 0.09, M_PI * 0.75, M_PI * 1.25);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_restore(cr);
    cairo_new_path(cr);
}

void draw_screen_mirror_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_width(cr, s * 0.09);

    draw_rounded_rect(cr, cx - s * 0.44, cy - s * 0.34, s * 0.54, s * 0.40, s * 0.08);
    cairo_stroke(cr);

    draw_rounded_rect(cr, cx - s * 0.16, cy - s * 0.10, s * 0.54, s * 0.40, s * 0.08);
    cairo_stroke(cr);

    cairo_restore(cr);
}

void draw_screen_keyboard_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    // Keyboard frame
    cairo_set_line_width(cr, s * 0.09);
    draw_rounded_rect(cr, cx - s * 0.44, cy - s * 0.30, s * 0.88, s * 0.60, s * 0.08);
    cairo_stroke(cr);

    // Row 1 (keys)
    double kw = s * 0.11;
    double kh = s * 0.08;
    double kr = s * 0.02;
    for (int i = 0; i < 4; i++) {
        double kx = cx - s * 0.31 + i * s * 0.18;
        cairo_new_path(cr);
        draw_rounded_rect(cr, kx, cy - s * 0.16, kw, kh, kr);
        cairo_fill(cr);
    }

    // Row 2 (keys)
    for (int i = 0; i < 4; i++) {
        double kx = cx - s * 0.31 + i * s * 0.18;
        cairo_new_path(cr);
        draw_rounded_rect(cr, kx, cy - s * 0.03, kw, kh, kr);
        cairo_fill(cr);
    }

    // Row 3 (space bar in center)
    cairo_new_path(cr);
    draw_rounded_rect(cr, cx - s * 0.22, cy + s * 0.10, s * 0.44, kh, kr);
    cairo_fill(cr);

    cairo_new_path(cr);
    cairo_restore(cr);
    cairo_new_path(cr);
}

void draw_display_badge_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_width(cr, s * 0.10);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    draw_rounded_rect(cr, cx - s * 0.42, cy - s * 0.36, s * 0.84, s * 0.52, s * 0.10);
    cairo_stroke(cr);

    cairo_move_to(cr, cx, cy + s * 0.16);
    cairo_line_to(cr, cx, cy + s * 0.32);
    cairo_stroke(cr);

    cairo_move_to(cr, cx - s * 0.20, cy + s * 0.32);
    cairo_line_to(cr, cx + s * 0.20, cy + s * 0.32);
    cairo_stroke(cr);

    cairo_restore(cr);
}

void draw_airplay_audio_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    cairo_move_to(cr, cx - s * 0.22, cy + s * 0.32);
    cairo_line_to(cr, cx, cy + s * 0.04);
    cairo_line_to(cr, cx + s * 0.22, cy + s * 0.32);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_line_width(cr, s * 0.095);
    cairo_arc(cr, cx, cy + s * 0.20, s * 0.34, M_PI + 0.60, 2 * M_PI - 0.60);
    cairo_stroke(cr);

    cairo_arc(cr, cx, cy + s * 0.20, s * 0.50, M_PI + 0.70, 2 * M_PI - 0.70);
    cairo_stroke(cr);

    cairo_restore(cr);
}

void draw_user_profile_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);

    cairo_set_line_width(cr, s * 0.085);
    cairo_arc(cr, cx, cy, s * 0.44, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_arc(cr, cx, cy - s * 0.10, s * 0.15, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_save(cr);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, s * 0.42, 0, 2 * M_PI);
    cairo_clip(cr);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy + s * 0.44, s * 0.32, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);

    cairo_new_path(cr);
    cairo_restore(cr);
    cairo_new_path(cr);
}

void draw_accessibility_icon(cairo_t *cr, double cx, double cy, double s, double a) {
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, g_icon_rgb[0], g_icon_rgb[1], g_icon_rgb[2], a);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    cairo_set_line_width(cr, s * 0.085);
    cairo_arc(cr, cx, cy, s * 0.44, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_arc(cr, cx, cy - s * 0.20, s * 0.085, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_new_path(cr);
    cairo_set_line_width(cr, s * 0.085);
    cairo_move_to(cr, cx - s * 0.26, cy - s * 0.04);
    cairo_line_to(cr, cx + s * 0.26, cy - s * 0.04);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, cx, cy - s * 0.04);
    cairo_line_to(cr, cx, cy + s * 0.12);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, cx, cy + s * 0.12);
    cairo_line_to(cr, cx - s * 0.18, cy + s * 0.30);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, cx, cy + s * 0.12);
    cairo_line_to(cr, cx + s * 0.18, cy + s * 0.30);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_restore(cr);
    cairo_new_path(cr);
}
