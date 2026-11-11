#include "wallpaper.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Paint a soft, blurred-looking colour blob.
static void draw_orb(cairo_t *cr, double cx, double cy, double radius,
                     double r, double g, double b, double a) {
    cairo_pattern_t *pat = cairo_pattern_create_radial(cx, cy, radius * 0.02,
                                                       cx, cy, radius);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, r, g, b, a);
    cairo_pattern_add_color_stop_rgba(pat, 0.45, r, g, b, a * 0.55);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, r, g, b, 0.0);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);
}

static cairo_surface_t *pixbuf_to_cairo_surface(GdkPixbuf *pb) {
    if (!pb) return NULL;
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);
    int channels = gdk_pixbuf_get_n_channels(pb);
    guchar *pixels = gdk_pixbuf_get_pixels(pb);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_surface_flush(surf);
    unsigned char *cdata = cairo_image_surface_get_data(surf);
    int cstride = cairo_image_surface_get_stride(surf);

    for (int y = 0; y < h; y++) {
        guchar *src = pixels + y * stride;
        uint32_t *dst = (uint32_t *)(cdata + y * cstride);
        for (int x = 0; x < w; x++) {
            guchar r = src[0];
            guchar g = src[1];
            guchar b = src[2];
            guchar a = (channels == 4) ? src[3] : 255;
            uint32_t pr = (r * a + 127) / 255;
            uint32_t pg = (g * a + 127) / 255;
            uint32_t pb_b = (b * a + 127) / 255;
            dst[x] = ((uint32_t)a << 24) | (pr << 16) | (pg << 8) | pb_b;
            src += channels;
        }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

static bool load_custom_wallpaper(aurade_wallpaper_t *wp) {
    const char *path = wp->server->config.wallpaper_path;
    if (!path || !*path) return false;

    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
    cairo_surface_t *img = NULL;
    if (pb) {
        img = pixbuf_to_cairo_surface(pb);
        g_object_unref(pb);
    } else {
        if (err) g_error_free(err);
        img = cairo_image_surface_create_from_png(path);
    }

    if (!img || cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        if (img) cairo_surface_destroy(img);
        return false;
    }

    int img_w = cairo_image_surface_get_width(img);
    int img_h = cairo_image_surface_get_height(img);
    if (img_w <= 0 || img_h <= 0) {
        cairo_surface_destroy(img);
        return false;
    }

    cairo_t *cr = wp->buffer->cr;
    int w = wp->width;
    int h = wp->height;

    // Fill with a dark base so letterboxing is never visible, then cover-scale
    // (preserve aspect ratio, centre-crop) like macOS does.
    cairo_set_source_rgb(cr, 0.02, 0.02, 0.03);
    cairo_paint(cr);

    double scale = fmax((double)w / img_w, (double)h / img_h);
    double draw_w = img_w * scale;
    double draw_h = img_h * scale;
    cairo_save(cr);
    cairo_translate(cr, (w - draw_w) / 2.0, (h - draw_h) / 2.0);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_surface_destroy(img);

    // Gentle vignette keeps white text readable.
    cairo_pattern_t *vig = cairo_pattern_create_radial(w * 0.5, h * 0.5, h * 0.25,
                                                       w * 0.5, h * 0.5, h * 0.85);
    cairo_pattern_add_color_stop_rgba(vig, 0.0, 0.0, 0.0, 0.0, 0.0);
    cairo_pattern_add_color_stop_rgba(vig, 1.0, 0.0, 0.0, 0.0, 0.28);
    cairo_set_source(cr, vig);
    cairo_paint(cr);
    cairo_pattern_destroy(vig);
    return true;
}

static void render_default_wallpaper(aurade_wallpaper_t *wp) {
    cairo_t *cr = wp->buffer->cr;
    int w = wp->width;
    int h = wp->height;

    // macOS Sequoia-inspired dusk gradient.
    cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, w * 0.9, h);
    cairo_pattern_add_color_stop_rgb(pat, 0.00, 0.035, 0.055, 0.125);
    cairo_pattern_add_color_stop_rgb(pat, 0.30, 0.105, 0.130, 0.290);
    cairo_pattern_add_color_stop_rgb(pat, 0.58, 0.280, 0.105, 0.360);
    cairo_pattern_add_color_stop_rgb(pat, 0.80, 0.180, 0.230, 0.500);
    cairo_pattern_add_color_stop_rgb(pat, 1.00, 0.050, 0.330, 0.430);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);

    // Layered soft-light orbs give the flowing wallpaper depth without banding.
    draw_orb(cr, w * 0.78, h * 0.22, w * 0.52, 0.98, 0.36, 0.58, 0.30);
    draw_orb(cr, w * 0.18, h * 0.78, w * 0.50, 0.12, 0.60, 0.98, 0.26);
    draw_orb(cr, w * 0.48, h * 0.52, w * 0.62, 0.42, 0.18, 0.72, 0.18);
    draw_orb(cr, w * 0.92, h * 0.88, w * 0.34, 0.10, 0.85, 0.72, 0.14);

    // Fine diagonal sheen, a subtle nod to Apple's abstract wallpapers.
    cairo_save(cr);
    cairo_pattern_t *sheen = cairo_pattern_create_linear(0, 0, w, h);
    cairo_pattern_add_color_stop_rgba(sheen, 0.0, 1.0, 1.0, 1.0, 0.0);
    cairo_pattern_add_color_stop_rgba(sheen, 0.47, 1.0, 1.0, 1.0, 0.035);
    cairo_pattern_add_color_stop_rgba(sheen, 0.53, 1.0, 1.0, 1.0, 0.045);
    cairo_pattern_add_color_stop_rgba(sheen, 1.0, 1.0, 1.0, 1.0, 0.0);
    cairo_set_source(cr, sheen);
    cairo_paint(cr);
    cairo_pattern_destroy(sheen);
    cairo_restore(cr);

    // Vignette
    cairo_pattern_t *vig = cairo_pattern_create_radial(w * 0.5, h * 0.45, h * 0.20,
                                                       w * 0.5, h * 0.5, h * 0.95);
    cairo_pattern_add_color_stop_rgba(vig, 0.0, 0.0, 0.0, 0.0, 0.0);
    cairo_pattern_add_color_stop_rgba(vig, 1.0, 0.0, 0.0, 0.0, 0.34);
    cairo_set_source(cr, vig);
    cairo_paint(cr);
    cairo_pattern_destroy(vig);
}

static uint32_t *blur_scratch = NULL;
static size_t blur_scratch_cap = 0;

static uint32_t *get_blur_scratch(size_t needed) {
    if (needed > blur_scratch_cap) {
        size_t new_cap = needed * 2;
        if (new_cap < 65536) new_cap = 65536;
        uint32_t *p = realloc(blur_scratch, new_cap * sizeof(uint32_t));
        if (!p) return NULL;
        blur_scratch = p;
        blur_scratch_cap = new_cap;
    }
    return blur_scratch;
}

static int *col_sum_scratch = NULL;
static size_t col_sum_scratch_cap = 0;

static int *get_col_sum_scratch(size_t w) {
    size_t needed = w * 4;
    if (needed > col_sum_scratch_cap) {
        size_t new_cap = needed * 2;
        if (new_cap < 4096) new_cap = 4096;
        int *p = realloc(col_sum_scratch, new_cap * sizeof(int));
        if (!p) return NULL;
        col_sum_scratch = p;
        col_sum_scratch_cap = new_cap;
    }
    return col_sum_scratch;
}

static void box_blur_surface(cairo_surface_t *surface, int radius) {
    if (!surface || radius < 1) return;
    cairo_surface_flush(surface);
    int w = cairo_image_surface_get_width(surface);
    int h = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    uint32_t *data = (uint32_t *)cairo_image_surface_get_data(surface);
    if (!data || w <= 0 || h <= 0) return;

    size_t needed = (size_t)w * h;
    uint32_t *temp = get_blur_scratch(needed);
    if (!temp) return;

    int *col_sums = get_col_sum_scratch(w);
    if (!col_sums) return;

    int *sum_a = col_sums;
    int *sum_r = col_sums + w;
    int *sum_g = col_sums + w * 2;
    int *sum_b = col_sums + w * 3;

    int div = 2 * radius + 1;
    uint32_t inv_div = (uint32_t)(((uint64_t)1 << 24) / div);
    int stride_words = stride / 4;

    // Pass 1: Horizontal blur (row-major data -> row-major temp: sequential memory writes)
    for (int y = 0; y < h; y++) {
        const uint32_t *row_in = data + y * stride_words;
        uint32_t *row_out = temp + y * w;
        int a_sum = 0, r_sum = 0, g_sum = 0, b_sum = 0;
        for (int i = -radius; i <= radius; i++) {
            int cx = i < 0 ? 0 : (i >= w ? w - 1 : i);
            uint32_t pix = row_in[cx];
            a_sum += (pix >> 24) & 0xff;
            r_sum += (pix >> 16) & 0xff;
            g_sum += (pix >> 8) & 0xff;
            b_sum += pix & 0xff;
        }

        for (int x = 0; x < w; x++) {
            row_out[x] = ((((uint32_t)a_sum * inv_div) >> 24) << 24) |
                         ((((uint32_t)r_sum * inv_div) >> 24) << 16) |
                         ((((uint32_t)g_sum * inv_div) >> 24) << 8)  |
                         (((uint32_t)b_sum * inv_div) >> 24);
            int left_idx = x - radius;
            int right_idx = x + radius + 1;
            uint32_t left_pix = row_in[left_idx < 0 ? 0 : left_idx];
            uint32_t right_pix = row_in[right_idx >= w ? w - 1 : right_idx];

            a_sum += ((right_pix >> 24) & 0xff) - ((left_pix >> 24) & 0xff);
            r_sum += ((right_pix >> 16) & 0xff) - ((left_pix >> 16) & 0xff);
            g_sum += ((right_pix >> 8) & 0xff) - ((left_pix >> 8) & 0xff);
            b_sum += (right_pix & 0xff) - (left_pix & 0xff);
        }
    }

    // Pass 2: Vertical blur (row-major temp -> row-major data: L1 cache resident column sums)
    for (int x = 0; x < w; x++) {
        sum_a[x] = 0; sum_r[x] = 0; sum_g[x] = 0; sum_b[x] = 0;
        for (int i = -radius; i <= radius; i++) {
            int cy = i < 0 ? 0 : (i >= h ? h - 1 : i);
            uint32_t pix = temp[cy * w + x];
            sum_a[x] += (pix >> 24) & 0xff;
            sum_r[x] += (pix >> 16) & 0xff;
            sum_g[x] += (pix >> 8) & 0xff;
            sum_b[x] += pix & 0xff;
        }
    }

    for (int y = 0; y < h; y++) {
        uint32_t *row_out = data + y * stride_words;
        int top_y = y - radius;
        int bot_y = y + radius + 1;
        const uint32_t *top_row = temp + (top_y < 0 ? 0 : top_y) * w;
        const uint32_t *bot_row = temp + (bot_y >= h ? h - 1 : bot_y) * w;

        for (int x = 0; x < w; x++) {
            row_out[x] = ((((uint32_t)sum_a[x] * inv_div) >> 24) << 24) |
                         ((((uint32_t)sum_r[x] * inv_div) >> 24) << 16) |
                         ((((uint32_t)sum_g[x] * inv_div) >> 24) << 8)  |
                         (((uint32_t)sum_b[x] * inv_div) >> 24);

            uint32_t tpix = top_row[x];
            uint32_t bpix = bot_row[x];
            sum_a[x] += ((bpix >> 24) & 0xff) - ((tpix >> 24) & 0xff);
            sum_r[x] += ((bpix >> 16) & 0xff) - ((tpix >> 16) & 0xff);
            sum_g[x] += ((bpix >> 8) & 0xff) - ((tpix >> 8) & 0xff);
            sum_b[x] += (bpix & 0xff) - (tpix & 0xff);
        }
    }

    cairo_surface_mark_dirty(surface);
}

static void rebuild_blurred_copy(aurade_wallpaper_t *wp) {
    if (wp->blurred) {
        cairo_surface_destroy(wp->blurred);
        wp->blurred = NULL;
    }
    if (!wp->buffer || !wp->buffer->surface) return;

    // ~1/20th resolution combined with multi-pass box blur gives a lush, true Gaussian blur
    int bw = wp->width / 20;
    int bh = wp->height / 20;
    if (bw < 32) bw = 32;
    if (bh < 32) bh = 32;

    cairo_surface_flush(wp->buffer->surface);
    wp->blurred = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
    cairo_t *bcr = cairo_create(wp->blurred);
    cairo_scale(bcr, (double)bw / wp->width, (double)bh / wp->height);
    cairo_set_source_surface(bcr, wp->buffer->surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(bcr), CAIRO_FILTER_GOOD);
    cairo_pattern_set_extend(cairo_get_source(bcr), CAIRO_EXTEND_PAD);
    cairo_paint(bcr);
    cairo_destroy(bcr);

    // Multi-pass blur scaled by blur_strength
    double str = (wp->server && wp->server->config.blur_strength > 0.05) ?
                 wp->server->config.blur_strength : 1.0;
    int r1 = (int)round(2.5 * str);
    int r2 = (int)round(1.5 * str);
    if (r1 < 1) r1 = 1;
    box_blur_surface(wp->blurred, r1);
    if (r2 >= 1) box_blur_surface(wp->blurred, r2);
}

static void update_blurred_scene_buffer(aurade_wallpaper_t *wp) {
    if (!wp || !wp->blurred_buffer || !wp->blurred) return;
    cairo_t *bcr = wp->blurred_buffer->cr;
    cairo_buffer_clear(wp->blurred_buffer);
    int bw = cairo_image_surface_get_width(wp->blurred);
    int bh = cairo_image_surface_get_height(wp->blurred);
    if (bw > 0 && bh > 0) {
        cairo_save(bcr);
        cairo_scale(bcr, (double)wp->width / bw, (double)wp->height / bh);
        cairo_set_source_surface(bcr, wp->blurred, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(bcr), CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(cairo_get_source(bcr), CAIRO_EXTEND_PAD);
        cairo_paint(bcr);
        cairo_restore(bcr);

        cairo_set_source_rgba(bcr, 0.02, 0.03, 0.06, 0.62);
        cairo_paint(bcr);
    }
    cairo_buffer_apply_to_scene(wp->blurred_buffer, wp->blurred_scene_buffer);
}

static void render_wallpaper_graphics(aurade_wallpaper_t *wp) {
    if (!wp || !wp->buffer || !wp->buffer->cr) return;

    cairo_buffer_clear(wp->buffer);
    wp->custom_image = load_custom_wallpaper(wp);
    if (!wp->custom_image) {
        render_default_wallpaper(wp);
    }

    cairo_buffer_apply_to_scene(wp->buffer, wp->scene_buffer);
    cairo_buffer_set_wallpaper_surface(wp->buffer ? wp->buffer->surface : NULL);
    rebuild_blurred_copy(wp);
    update_blurred_scene_buffer(wp);
}

aurade_wallpaper_t *wallpaper_create(aurade_server_t *server) {
    aurade_wallpaper_t *wp = calloc(1, sizeof(*wp));
    wp->server = server;
    wp->width = server->screen_width > 0 ? server->screen_width : 1920;
    wp->height = server->screen_height > 0 ? server->screen_height : 1080;

    wp->buffer = cairo_buffer_create(wp->width, wp->height);
    wp->scene_buffer = wlr_scene_buffer_create(server->wallpaper_tree, &wp->buffer->base);

    wp->blurred_buffer = cairo_buffer_create(wp->width, wp->height);
    wp->blurred_scene_buffer = wlr_scene_buffer_create(server->wallpaper_tree, &wp->blurred_buffer->base);
    wlr_scene_node_set_enabled(&wp->blurred_scene_buffer->node, false);

    render_wallpaper_graphics(wp);
    return wp;
}

void wallpaper_resize(aurade_wallpaper_t *wp, int width, int height) {
    if (!wp || (wp->width == width && wp->height == height)) return;
    wp->width = width;
    wp->height = height;
    cairo_buffer_resize(wp->buffer, width, height);
    if (wp->blurred_buffer) cairo_buffer_resize(wp->blurred_buffer, width, height);
    render_wallpaper_graphics(wp);
}

void wallpaper_reload(aurade_wallpaper_t *wp) {
    if (!wp) return;
    render_wallpaper_graphics(wp);
}

void wallpaper_set_overview_blur(aurade_wallpaper_t *wp, double progress) {
    if (!wp || !wp->blurred_scene_buffer) return;
    if (progress <= 0.001) {
        wlr_scene_node_set_enabled(&wp->blurred_scene_buffer->node, false);
        return;
    }
    float alpha = (float)fmin(1.0, fmax(0.0, progress));
    wlr_scene_buffer_set_opacity(wp->blurred_scene_buffer, alpha);
    wlr_scene_node_set_enabled(&wp->blurred_scene_buffer->node, true);
}

void wallpaper_draw_backdrop(aurade_wallpaper_t *wp, cairo_t *cr,
                             int width, int height, double dim, double alpha) {
    if (!wp || !wp->blurred || alpha <= 0.0) return;
    int bw = cairo_image_surface_get_width(wp->blurred);
    int bh = cairo_image_surface_get_height(wp->blurred);
    if (bw <= 0 || bh <= 0) return;

    cairo_save(cr);
    cairo_scale(cr, (double)width / bw, (double)height / bh);
    cairo_set_source_surface(cr, wp->blurred, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);

    if (dim > 0.0) {
        cairo_set_source_rgba(cr, 0.02, 0.03, 0.06, dim * alpha);
        cairo_paint(cr);
    }
}

void wallpaper_destroy(aurade_wallpaper_t *wp) {
    if (!wp) return;
    cairo_buffer_set_wallpaper_surface(NULL);
    if (wp->blurred) cairo_surface_destroy(wp->blurred);
    if (wp->blurred_scene_buffer) wlr_scene_node_destroy(&wp->blurred_scene_buffer->node);
    if (wp->blurred_buffer) wlr_buffer_drop(&wp->blurred_buffer->base);
    if (wp->buffer) wlr_buffer_drop(&wp->buffer->base);
    free(wp);
    if (blur_scratch) {
        free(blur_scratch);
        blur_scratch = NULL;
        blur_scratch_cap = 0;
    }
    if (col_sum_scratch) {
        free(col_sum_scratch);
        col_sum_scratch = NULL;
        col_sum_scratch_cap = 0;
    }
}
