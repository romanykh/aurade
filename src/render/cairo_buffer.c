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

void draw_refractive_glass_rim(cairo_t *cr, double x, double y, double w, double h,
                               double r, double edge_a) {
    if (w <= 4.0 || h <= 4.0 || edge_a <= 0.01) return;
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;

    cairo_save(cr);

    // --- 1. Soft Inner Refractive Lens Cushion (Side light refraction / Fresnel glow) ---
    // Clip strictly to the rounded rect interior so the lens cushion spreads inward:
    draw_rounded_rect(cr, x, y, w, h, r);
    cairo_clip(cr);

    // Deep refractive halo (width 5.0px) - reduced 80%
    cairo_pattern_t *inner_cushion = cairo_pattern_create_linear(x, y, x + w, y + h);
    cairo_pattern_add_color_stop_rgba(inner_cushion, 0.0, 1.0, 1.0, 1.0, 0.08 * edge_a);
    cairo_pattern_add_color_stop_rgba(inner_cushion, 0.35, 1.0, 1.0, 1.0, 0.05 * edge_a);
    cairo_pattern_add_color_stop_rgba(inner_cushion, 0.70, 1.0, 1.0, 1.0, 0.03 * edge_a);
    cairo_pattern_add_color_stop_rgba(inner_cushion, 1.0, 1.0, 1.0, 1.0, 0.015 * edge_a);

    draw_rounded_rect(cr, x, y, w, h, r);
    cairo_set_source(cr, inner_cushion);
    cairo_set_line_width(cr, 5.0);
    cairo_stroke(cr);
    cairo_pattern_destroy(inner_cushion);

    // Secondary tighter rim (width 2.0px) - reduced 80%
    cairo_pattern_t *inner_bright = cairo_pattern_create_linear(x, y, x + w, y + h);
    cairo_pattern_add_color_stop_rgba(inner_bright, 0.0, 1.0, 1.0, 1.0, 0.11 * edge_a);
    cairo_pattern_add_color_stop_rgba(inner_bright, 0.35, 1.0, 1.0, 1.0, 0.06 * edge_a);
    cairo_pattern_add_color_stop_rgba(inner_bright, 0.70, 1.0, 1.0, 1.0, 0.04 * edge_a);
    cairo_pattern_add_color_stop_rgba(inner_bright, 1.0, 1.0, 1.0, 1.0, 0.02 * edge_a);

    draw_rounded_rect(cr, x, y, w, h, r);
    cairo_set_source(cr, inner_bright);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);
    cairo_pattern_destroy(inner_bright);

    cairo_reset_clip(cr);

    // --- 2. Crisp Outer Specular Boundary Hairline (1px on boundary) - reduced 80% ---
    cairo_pattern_t *outer_rim = cairo_pattern_create_linear(x, y, x + w, y + h);
    cairo_pattern_add_color_stop_rgba(outer_rim, 0.0, 1.0, 1.0, 1.0, 0.14 * edge_a);
    cairo_pattern_add_color_stop_rgba(outer_rim, 0.35, 1.0, 1.0, 1.0, 0.07 * edge_a);
    cairo_pattern_add_color_stop_rgba(outer_rim, 0.70, 1.0, 1.0, 1.0, 0.04 * edge_a);
    cairo_pattern_add_color_stop_rgba(outer_rim, 1.0, 1.0, 1.0, 1.0, 0.02 * edge_a);

    draw_rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, r > 0.5 ? r - 0.5 : r);
    cairo_set_source(cr, outer_rim);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_pattern_destroy(outer_rim);

    // --- 3. Top Specular Crest Arc (sharp micro-bevel catching key light) - reduced 80% ---
    if (w > 2.0 * r) {
        cairo_new_path(cr);
        cairo_move_to(cr, x + r, y + 0.5);
        cairo_line_to(cr, x + w - r, y + 0.5);
        cairo_pattern_t *crest = cairo_pattern_create_linear(x + r, y, x + w - r, y);
        cairo_pattern_add_color_stop_rgba(crest, 0.0, 1.0, 1.0, 1.0, 0.05 * edge_a);
        cairo_pattern_add_color_stop_rgba(crest, 0.5, 1.0, 1.0, 1.0, 0.15 * edge_a);
        cairo_pattern_add_color_stop_rgba(crest, 1.0, 1.0, 1.0, 1.0, 0.05 * edge_a);
        cairo_set_source(cr, crest);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_pattern_destroy(crest);
    }

    // --- 4. Bottom Refraction Meniscus Arc (curved light gathering on bottom edge) - reduced 80% ---
    if (w > 2.0 * r) {
        cairo_new_path(cr);
        cairo_move_to(cr, x + r, y + h - 0.5);
        cairo_line_to(cr, x + w - r, y + h - 0.5);
        cairo_pattern_t *meniscus = cairo_pattern_create_linear(x + r, y + h, x + w - r, y + h);
        cairo_pattern_add_color_stop_rgba(meniscus, 0.0, 1.0, 1.0, 1.0, 0.015 * edge_a);
        cairo_pattern_add_color_stop_rgba(meniscus, 0.5, 1.0, 1.0, 1.0, 0.07 * edge_a);
        cairo_pattern_add_color_stop_rgba(meniscus, 1.0, 1.0, 1.0, 1.0, 0.015 * edge_a);
        cairo_set_source(cr, meniscus);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_pattern_destroy(meniscus);
    }

    // --- 5. Bottom Grounding Shadow Hairline ---
    if (w > 2.0 * r && h > 8.0 && edge_a > 0.10) {
        cairo_new_path(cr);
        cairo_move_to(cr, x + r, y + h - 0.5);
        cairo_line_to(cr, x + w - r, y + h - 0.5);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.25 * edge_a);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

static cairo_surface_t *s_wallpaper_surface = NULL;
static bool s_liquid_glass_enabled = false;

void cairo_buffer_set_wallpaper_surface(cairo_surface_t *surf) {
    s_wallpaper_surface = surf;
}

cairo_surface_t *cairo_buffer_get_wallpaper_surface(void) {
    return s_wallpaper_surface;
}

void cairo_buffer_set_liquid_glass_enabled(bool enabled) {
    s_liquid_glass_enabled = enabled;
}

bool cairo_buffer_get_liquid_glass_enabled(void) {
    return s_liquid_glass_enabled;
}

void draw_liquid_glass_lens(cairo_t *target_cr,
                           cairo_surface_t *bg_surf,
                           double panel_x, double panel_y,
                           double panel_w, double panel_h,
                           double panel_r,
                           double screen_x, double screen_y,
                           double mag, double edge_a) {
    if (!target_cr || !bg_surf || panel_w <= 0.0 || panel_h <= 0.0 || edge_a <= 0.01) return;

    int iw = (int)ceil(panel_w);
    int ih = (int)ceil(panel_h);
    if (iw < 4 || ih < 4) return;

    cairo_surface_flush(bg_surf);
    int bg_w = cairo_image_surface_get_width(bg_surf);
    int bg_h = cairo_image_surface_get_height(bg_surf);
    int bg_stride = cairo_image_surface_get_stride(bg_surf);
    uint32_t *bg_data = (uint32_t *)cairo_image_surface_get_data(bg_surf);
    if (!bg_data || bg_w <= 0 || bg_h <= 0) return;

    cairo_surface_t *lens_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    cairo_surface_flush(lens_surf);
    int lens_stride = cairo_image_surface_get_stride(lens_surf);
    uint32_t *lens_data = (uint32_t *)cairo_image_surface_get_data(lens_surf);
    if (!lens_data) {
        cairo_surface_destroy(lens_surf);
        return;
    }

    float hw = (float)(panel_w * 0.5);
    float hh = (float)(panel_h * 0.5);
    float cx = hw;
    float cy = hh;
    float r = (float)panel_r;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    if (r < 2.0f) r = 2.0f;
    float inv_r = 1.0f / r;

    float safe_mag = mag < 1.05f ? 1.05f : (mag > 2.0f ? 2.0f : (float)mag);
    float bx = hw - r;
    float by = hh - r;
    float mag_pull = 1.0f - (1.0f / safe_mag);
    float max_bg_x = (float)(bg_w - 2);
    float max_bg_y = (float)(bg_h - 2);
    float edge_alpha = (float)edge_a;

    for (int y = 0; y < ih; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)lens_data + y * lens_stride);
        float ly = (y + 0.5f) - cy;
        float abs_ly = fabsf(ly);
        float qy = abs_ly - by;
        float dy = qy > 0.0f ? qy : 0.0f;
        float sign_y = ly >= 0.0f ? 1.0f : -1.0f;

        for (int x = 0; x < iw; x++) {
            float lx = (x + 0.5f) - cx;
            float abs_lx = fabsf(lx);
            float qx = abs_lx - bx;
            float dx = qx > 0.0f ? qx : 0.0f;
            float sign_x = lx >= 0.0f ? 1.0f : -1.0f;

            float dist_outside;
            float nx = 0.0f, ny = 0.0f;

            if (qx > 0.0f && qy > 0.0f) {
                float corner_dist = sqrtf(dx * dx + dy * dy);
                dist_outside = corner_dist - r;
                if (corner_dist > 1e-4f) {
                    float inv_c = 1.0f / corner_dist;
                    nx = (dx * inv_c) * sign_x;
                    ny = (dy * inv_c) * sign_y;
                }
            } else {
                float max_q = qx > qy ? qx : qy;
                dist_outside = max_q - r;
                if (qx > qy) nx = sign_x;
                else ny = sign_y;
            }

            if (dist_outside >= 1.0f) {
                row[x] = 0;
                continue;
            }

            float dist_inside = -dist_outside;
            float alpha = dist_inside > 1.0f ? 1.0f : (dist_inside < 0.0f ? 0.0f : dist_inside);
            float norm_depth = dist_inside * inv_r;
            if (norm_depth > 1.0f) norm_depth = 1.0f;

            // 1. Center Magnification (лупа)
            float pull = 1.0f - mag_pull * norm_depth;
            float sample_lx = lx * pull;
            float sample_ly = ly * pull;

            // 2. Peripheral Curved Refraction (Snell's Law edge warping)
            float one_minus_d = 1.0f - norm_depth;
            float sin_approx = norm_depth * (1.570796f - 0.645f * norm_depth * norm_depth);
            float refract_intensity = 24.0f * one_minus_d * sin_approx;
            sample_lx += nx * refract_intensity;
            sample_ly += ny * refract_intensity;

            float bg_x = (float)screen_x + cx + sample_lx;
            float bg_y = (float)screen_y + cy + sample_ly;
            if (bg_x < 0.0f) bg_x = 0.0f;
            else if (bg_x > max_bg_x) bg_x = max_bg_x;
            if (bg_y < 0.0f) bg_y = 0.0f;
            else if (bg_y > max_bg_y) bg_y = max_bg_y;

            // Bilinear sampling
            int isx = (int)bg_x;
            int isy = (int)bg_y;
            float fx = bg_x - (float)isx;
            float fy = bg_y - (float)isy;

            uint32_t *p_row0 = (uint32_t *)((uint8_t *)bg_data + isy * bg_stride);
            uint32_t *p_row1 = (uint32_t *)((uint8_t *)bg_data + (isy + 1) * bg_stride);
            uint32_t p00 = p_row0[isx];
            uint32_t p10 = p_row0[isx + 1];
            uint32_t p01 = p_row1[isx];
            uint32_t p11 = p_row1[isx + 1];

            float w00 = (1.0f - fx) * (1.0f - fy);
            float w10 = fx * (1.0f - fy);
            float w01 = (1.0f - fx) * fy;
            float w11 = fx * fy;

            float b = w00 * (p00 & 0xFF) + w10 * (p10 & 0xFF) + w01 * (p01 & 0xFF) + w11 * (p11 & 0xFF);
            float g = w00 * ((p00 >> 8) & 0xFF) + w10 * ((p10 >> 8) & 0xFF) + w01 * ((p01 >> 8) & 0xFF) + w11 * ((p11 >> 8) & 0xFF);
            float red = w00 * ((p00 >> 16) & 0xFF) + w10 * ((p10 >> 16) & 0xFF) + w01 * ((p01 >> 16) & 0xFF) + w11 * ((p11 >> 16) & 0xFF);

            // Subtle chromatic dispersion at curved edge
            if (norm_depth < 0.65f) {
                float fringe = (1.0f - norm_depth * 1.538f) * 2.0f;
                int rx = (int)(bg_x + nx * fringe);
                int ry = (int)(bg_y + ny * fringe);
                if (rx >= 0 && rx < bg_w && ry >= 0 && ry < bg_h) {
                    uint32_t pr = *(uint32_t *)((uint8_t *)bg_data + ry * bg_stride + rx * 4);
                    red = red * 0.72f + ((pr >> 16) & 0xFF) * 0.28f;
                }
                int bx_i = (int)(bg_x - nx * fringe);
                int by_i = (int)(bg_y - ny * fringe);
                if (bx_i >= 0 && bx_i < bg_w && by_i >= 0 && by_i < bg_h) {
                    uint32_t pb = *(uint32_t *)((uint8_t *)bg_data + by_i * bg_stride + bx_i * 4);
                    b = b * 0.72f + (pb & 0xFF) * 0.28f;
                }
            }

            // Crystal glass neutral glow
            float fresnel = one_minus_d * one_minus_d * 0.28f * edge_alpha;
            float top_sheen = 0.0f;
            if (ly < 0.0f && norm_depth > 0.10f) {
                float sheen_depth = (-ly) / hh;
                top_sheen = sheen_depth * sheen_depth * 0.10f * edge_alpha;
            }

            float glow = (fresnel + top_sheen) * 255.0f;
            red = red * 1.01f + glow;
            g = g * 1.01f + glow;
            b = b * 1.02f + glow;

            if (red > 255.0f) red = 255.0f;
            if (g > 255.0f) g = 255.0f;
            if (b > 255.0f) b = 255.0f;

            uint32_t a_byte = (uint32_t)lroundf(alpha * 255.0f);
            uint32_t r_byte = (uint32_t)lroundf(red * alpha);
            uint32_t g_byte = (uint32_t)lroundf(g * alpha);
            uint32_t b_byte = (uint32_t)lroundf(b * alpha);

            row[x] = (a_byte << 24) | (r_byte << 16) | (g_byte << 8) | b_byte;
        }
    }

    cairo_surface_mark_dirty(lens_surf);

    cairo_save(target_cr);
    cairo_set_source_surface(target_cr, lens_surf, panel_x, panel_y);
    cairo_paint(target_cr);
    cairo_restore(target_cr);

    cairo_surface_destroy(lens_surf);

    // Apply specular caustics and rim cushion on top
    draw_refractive_glass_rim(target_cr, panel_x, panel_y, panel_w, panel_h, panel_r, edge_a);
}

void draw_glass_panel_at(cairo_t *cr, double x, double y, double w, double h, double r,
                         double screen_x, double screen_y,
                         double r_bg, double g_bg, double b_bg, double a_bg) {
    if (w <= 0.0 || h <= 0.0) return;
    (void)screen_x;
    (void)screen_y;
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;
    cairo_save(cr);

    double edge_a = a_bg > 1.0 ? 1.0 : (a_bg < 0.0 ? 0.0 : a_bg);

    // 1. Refractive depth gradient body fill (translucent, crystal clear glass)
    cairo_pattern_t *body_pat = cairo_pattern_create_linear(x, y, x, y + h);
        double top_r = r_bg * 1.15; if (top_r > 1.0) top_r = 1.0;
        double top_g = g_bg * 1.15; if (top_g > 1.0) top_g = 1.0;
        double top_b = b_bg * 1.22; if (top_b > 1.0) top_b = 1.0;
        double bot_r = r_bg * 0.88;
        double bot_g = g_bg * 0.88;
        double bot_b = b_bg * 0.92;
        double top_a = a_bg * 0.82;
        double bot_a = a_bg * 0.98;

        cairo_pattern_add_color_stop_rgba(body_pat, 0.0, top_r, top_g, top_b, top_a);
        cairo_pattern_add_color_stop_rgba(body_pat, 1.0, bot_r, bot_g, bot_b, bot_a);

        draw_rounded_rect(cr, x, y, w, h, r);
        cairo_set_source(cr, body_pat);
        cairo_fill(cr);
        cairo_pattern_destroy(body_pat);

        // 2. Caustic surface sheen wash across the top ~38% of the glass
        if (h > 6.0 && edge_a > 0.05) {
            cairo_save(cr);
            draw_rounded_rect(cr, x, y, w, h, r);
            cairo_clip(cr);

            double sheen_h = h * 0.38;
            if (sheen_h > 45.0) sheen_h = 45.0;
            if (sheen_h < 4.0) sheen_h = 4.0;
            cairo_pattern_t *sheen = cairo_pattern_create_linear(x, y, x, y + sheen_h);
            cairo_pattern_add_color_stop_rgba(sheen, 0.0, 1.0, 1.0, 1.0, 0.035 * edge_a);
            cairo_pattern_add_color_stop_rgba(sheen, 0.6, 1.0, 1.0, 1.0, 0.01 * edge_a);
            cairo_pattern_add_color_stop_rgba(sheen, 1.0, 1.0, 1.0, 1.0, 0.0);
            cairo_rectangle(cr, x, y, w, sheen_h);
            cairo_set_source(cr, sheen);
            cairo_fill(cr);
            cairo_pattern_destroy(sheen);
            cairo_restore(cr);
        }

        // 3. Liquid Glass Refractive Edge Cushion ("заломлення світла по сторонах")
        draw_refractive_glass_rim(cr, x, y, w, h, r, edge_a);

    cairo_restore(cr);
}

void draw_glass_panel(cairo_t *cr, double x, double y, double w, double h, double r,
                      double r_bg, double g_bg, double b_bg, double a_bg) {
    draw_glass_panel_at(cr, x, y, w, h, r, x, y, r_bg, g_bg, b_bg, a_bg);
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

    // Two toggle switches, like the Control Center menu-bar glyph.
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
