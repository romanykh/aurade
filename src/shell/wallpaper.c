#include "wallpaper.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

static bool load_custom_wallpaper(aurade_wallpaper_t *wp) {
    const char *path = wp->server->config.wallpaper_path;
    if (!path || !*path) return false;

    cairo_surface_t *img = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
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

static void render_wallpaper_graphics(aurade_wallpaper_t *wp) {
    if (!wp || !wp->buffer || !wp->buffer->cr) return;

    cairo_buffer_clear(wp->buffer);
    wp->custom_image = load_custom_wallpaper(wp);
    if (!wp->custom_image) {
        render_default_wallpaper(wp);
    }

    cairo_buffer_apply_to_scene(wp->buffer, wp->scene_buffer);
    rebuild_blurred_copy(wp);
}

aurade_wallpaper_t *wallpaper_create(aurade_server_t *server) {
    aurade_wallpaper_t *wp = calloc(1, sizeof(*wp));
    wp->server = server;
    wp->width = server->screen_width > 0 ? server->screen_width : 1920;
    wp->height = server->screen_height > 0 ? server->screen_height : 1080;

    wp->buffer = cairo_buffer_create(wp->width, wp->height);
    wp->scene_buffer = wlr_scene_buffer_create(server->wallpaper_tree, &wp->buffer->base);

    render_wallpaper_graphics(wp);
    return wp;
}

void wallpaper_resize(aurade_wallpaper_t *wp, int width, int height) {
    if (!wp || (wp->width == width && wp->height == height)) return;
    wp->width = width;
    wp->height = height;
    cairo_buffer_resize(wp->buffer, width, height);
    render_wallpaper_graphics(wp);
}

void wallpaper_reload(aurade_wallpaper_t *wp) {
    if (!wp) return;
    render_wallpaper_graphics(wp);
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

void wallpaper_draw_blur_rect(aurade_wallpaper_t *wp, cairo_t *cr,
                              double screen_x, double screen_y,
                              double rect_x, double rect_y,
                              double rect_w, double rect_h,
                              double corner_radius,
                              double tint_r, double tint_g, double tint_b, double tint_a) {
    if (!wp || !wp->blurred || rect_w <= 0.0 || rect_h <= 0.0) return;
    int bw = cairo_image_surface_get_width(wp->blurred);
    int bh = cairo_image_surface_get_height(wp->blurred);
    if (bw <= 0 || bh <= 0 || wp->width <= 0 || wp->height <= 0) return;

    cairo_save(cr);

    if (corner_radius > 0.0) {
        draw_rounded_rect(cr, rect_x, rect_y, rect_w, rect_h, corner_radius);
    } else {
        cairo_rectangle(cr, rect_x, rect_y, rect_w, rect_h);
    }
    cairo_clip(cr);

    double scale_x = (double)wp->width / bw;
    double scale_y = (double)wp->height / bh;

    cairo_translate(cr, -screen_x, -screen_y);
    cairo_scale(cr, scale_x, scale_y);
    cairo_set_source_surface(cr, wp->blurred, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);

    cairo_restore(cr);

    if (tint_a > 0.0) {
        cairo_save(cr);
        if (corner_radius > 0.0) {
            draw_rounded_rect(cr, rect_x, rect_y, rect_w, rect_h, corner_radius);
        } else {
            cairo_rectangle(cr, rect_x, rect_y, rect_w, rect_h);
        }
        cairo_set_source_rgba(cr, tint_r, tint_g, tint_b, tint_a);
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

// ---------------------------------------------------------------------------
// Real-time window surface capture & blur
// ---------------------------------------------------------------------------

static unsigned char *readback_scratch = NULL;
static size_t readback_scratch_cap = 0;

static unsigned char *get_readback_scratch(size_t needed) {
    if (needed > readback_scratch_cap) {
        size_t new_cap = needed * 2;
        if (new_cap < 262144) new_cap = 262144;
        unsigned char *p = realloc(readback_scratch, new_cap);
        if (!p) return NULL;
        readback_scratch = p;
        readback_scratch_cap = new_cap;
    }
    return readback_scratch;
}

static bool extract_scene_buffer_pixels(struct wlr_scene_buffer *sbuf,
                                        struct wlr_box *src_box,
                                        cairo_surface_t **out_surface) {
    if (!sbuf || !sbuf->buffer) return false;

    int buf_w = sbuf->buffer->width;
    int buf_h = sbuf->buffer->height;
    if (buf_w <= 0 || buf_h <= 0) return false;

    int crop_x = src_box ? src_box->x : 0;
    int crop_y = src_box ? src_box->y : 0;
    int crop_w = src_box ? src_box->width : buf_w;
    int crop_h = src_box ? src_box->height : buf_h;

    if (crop_x < 0) { crop_w += crop_x; crop_x = 0; }
    if (crop_y < 0) { crop_h += crop_y; crop_y = 0; }
    if (crop_x + crop_w > buf_w) crop_w = buf_w - crop_x;
    if (crop_y + crop_h > buf_h) crop_h = buf_h - crop_y;
    if (crop_w <= 0 || crop_h <= 0) return false;

    size_t needed = (size_t)crop_w * crop_h * 4;
    unsigned char *dst_data = get_readback_scratch(needed);
    if (!dst_data) return false;
    int dst_stride = crop_w * 4;

    // 1. Try CPU/SHM pointer access first (fastest path: zero-alloc copy into scratch)
    void *data = NULL;
    uint32_t fmt = 0;
    size_t stride = 0;
    if (wlr_buffer_begin_data_ptr_access(sbuf->buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                         &data, &fmt, &stride)) {
        if (data && stride > 0) {
            const unsigned char *src_data = (const unsigned char *)data + crop_y * stride + crop_x * 4;
            for (int r = 0; r < crop_h; r++) {
                memcpy(dst_data + r * dst_stride, src_data + r * stride, crop_w * 4);
            }
            wlr_buffer_end_data_ptr_access(sbuf->buffer);
            *out_surface = cairo_image_surface_create_for_data(dst_data, CAIRO_FORMAT_ARGB32, crop_w, crop_h, dst_stride);
            return true;
        }
        wlr_buffer_end_data_ptr_access(sbuf->buffer);
    }

    // 2. Try client texture readback (for hardware/GPU textures)
    struct wlr_client_buffer *client_buf = wlr_client_buffer_get(sbuf->buffer);
    if (client_buf && client_buf->texture) {
        struct wlr_texture_read_pixels_options opts = {
            .data = dst_data,
            .format = DRM_FORMAT_ARGB8888,
            .stride = dst_stride,
            .dst_x = 0,
            .dst_y = 0,
            .src_box = {
                .x = crop_x,
                .y = crop_y,
                .width = crop_w,
                .height = crop_h,
            },
        };

        if (wlr_texture_read_pixels(client_buf->texture, &opts)) {
            *out_surface = cairo_image_surface_create_for_data(dst_data, CAIRO_FORMAT_ARGB32, crop_w, crop_h, dst_stride);
            return true;
        }

        opts.format = DRM_FORMAT_XRGB8888;
        if (wlr_texture_read_pixels(client_buf->texture, &opts)) {
            for (int r = 0; r < crop_h; r++) {
                uint32_t *row = (uint32_t *)(dst_data + r * dst_stride);
                for (int c = 0; c < crop_w; c++) {
                    row[c] |= 0xFF000000;
                }
            }
            *out_surface = cairo_image_surface_create_for_data(dst_data, CAIRO_FORMAT_ARGB32, crop_w, crop_h, dst_stride);
            return true;
        }
    }

    return false;
}

struct capture_workspace_data {
    cairo_t *cr;
    double scale_x, scale_y;
    double clip_x, clip_y, clip_w, clip_h;
};

static void capture_workspace_buffer_iterator(struct wlr_scene_buffer *sbuf,
                                              int sx, int sy, void *user_data) {
    (void)sx; (void)sy;
    if (!sbuf || !sbuf->buffer) return;
    int lx = 0, ly = 0;
    if (!wlr_scene_node_coords(&sbuf->node, &lx, &ly)) return;
    if (!sbuf->node.enabled || sbuf->opacity <= 0.001f) return;

    struct capture_workspace_data *data = user_data;

    int bw = sbuf->dst_width > 0 ? sbuf->dst_width : sbuf->buffer->width;
    int bh = sbuf->dst_height > 0 ? sbuf->dst_height : sbuf->buffer->height;
    if (bw <= 0 || bh <= 0) return;

    int ix1 = (int)fmax(lx, data->clip_x);
    int iy1 = (int)fmax(ly, data->clip_y);
    int ix2 = (int)fmin(lx + bw, data->clip_x + data->clip_w);
    int iy2 = (int)fmin(ly + bh, data->clip_y + data->clip_h);
    if (ix2 <= ix1 || iy2 <= iy1) return;

    int iw = ix2 - ix1;
    int ih = iy2 - iy1;

    double scale_u = (double)sbuf->buffer->width / (double)bw;
    double scale_v = (double)sbuf->buffer->height / (double)bh;
    struct wlr_box src_box = {
        .x = (int)floor((ix1 - lx) * scale_u),
        .y = (int)floor((iy1 - ly) * scale_v),
        .width = (int)ceil(iw * scale_u),
        .height = (int)ceil(ih * scale_v),
    };

    cairo_surface_t *surf = NULL;
    if (!extract_scene_buffer_pixels(sbuf, &src_box, &surf) || !surf) return;

    int surf_w = cairo_image_surface_get_width(surf);
    int surf_h = cairo_image_surface_get_height(surf);
    if (surf_w > 0 && surf_h > 0) {
        cairo_save(data->cr);
        cairo_translate(data->cr, (ix1 - data->clip_x) * data->scale_x,
                                  (iy1 - data->clip_y) * data->scale_y);
        cairo_scale(data->cr, (double)iw * data->scale_x / surf_w,
                              (double)ih * data->scale_y / surf_h);
        cairo_set_source_surface(data->cr, surf, 0, 0);
        cairo_paint_with_alpha(data->cr, sbuf->opacity);
        cairo_restore(data->cr);
    }
    cairo_surface_destroy(surf);
}

void wallpaper_draw_realtime_blur_rect(aurade_wallpaper_t *wp, cairo_t *cr,
                                       double screen_x, double screen_y,
                                       double rect_x, double rect_y,
                                       double rect_w, double rect_h,
                                       double corner_radius,
                                       double tint_r, double tint_g, double tint_b, double tint_a) {
    if (!wp || rect_w <= 0.0 || rect_h <= 0.0) return;
    aurade_server_t *server = wp->server;
    if (!server) return;

    double global_rx = screen_x + rect_x;
    double global_ry = screen_y + rect_y;

    // Edge-bleed padding: capture an extra margin beyond the bottom edge so blur smoothly
    // blends across boundaries without harsh cutoff lines or missing edge blur.
    double pad_bottom = 24.0;
    double cap_h = rect_h + pad_bottom;

    // Check if any visible window on the active workspace intersects this padded rect
    bool has_overlapping_window = false;
    aurade_view_t *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->workspace != server->current_workspace) continue;
        if (view->minimized) continue;
        if (!view->scene_tree || !view->scene_tree->node.enabled) continue;
        if (view->width <= 0 || view->height <= 0) continue;
        if (view->x + view->width > global_rx && view->x < global_rx + rect_w &&
            view->y + view->height > global_ry && view->y <= global_ry + cap_h) {
            has_overlapping_window = true;
            break;
        }
    }

    if (!has_overlapping_window || !server->workspace_trees[server->current_workspace]) {
        // Fast path: no windows behind rect, draw directly from static blurred wallpaper (0ms CPU)
        wallpaper_draw_blur_rect(wp, cr, screen_x, screen_y,
                                 rect_x, rect_y, rect_w, rect_h, corner_radius,
                                 tint_r, tint_g, tint_b, tint_a);
        return;
    }

    // Windows intersect: downscaled real-time composite patch with padding
    int bw = (int)round(rect_w / 4.0);
    int bh = (int)round(cap_h / 4.0);
    if (bw < 32) bw = 32;
    if (bh < 8) bh = 8;

    cairo_surface_t *patch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
    cairo_t *pcr = cairo_create(patch);

    double sx = (double)bw / rect_w;
    double sy = (double)bh / cap_h;

    // 1. Paint wallpaper under rect
    if (wp->buffer && wp->buffer->surface) {
        cairo_save(pcr);
        cairo_scale(pcr, sx, sy);
        cairo_set_source_surface(pcr, wp->buffer->surface, -global_rx, -global_ry);
        cairo_pattern_set_filter(cairo_get_source(pcr), CAIRO_FILTER_GOOD);
        cairo_pattern_set_extend(cairo_get_source(pcr), CAIRO_EXTEND_PAD);
        cairo_paint(pcr);
        cairo_restore(pcr);
    }

    // 2. Composite visible window buffers
    struct capture_workspace_data cdata = {
        .cr = pcr,
        .scale_x = sx,
        .scale_y = sy,
        .clip_x = global_rx,
        .clip_y = global_ry,
        .clip_w = rect_w,
        .clip_h = cap_h,
    };
    wlr_scene_node_for_each_buffer(&server->workspace_trees[server->current_workspace]->node,
                                   capture_workspace_buffer_iterator, &cdata);
    cairo_destroy(pcr);

    // 3. Multi-pass box blur according to blur_strength
    double str = server->config.blur_strength > 0.05 ? server->config.blur_strength : 1.0;
    int r1 = (int)round(2.5 * str);
    int r2 = (int)round(1.5 * str);
    if (r1 < 1) r1 = 1;
    box_blur_surface(patch, r1);
    if (r2 >= 1) box_blur_surface(patch, r2);

    // 4. Paint patch clipped to destination rounded rect
    cairo_save(cr);
    if (corner_radius > 0.0) {
        draw_rounded_rect(cr, rect_x, rect_y, rect_w, rect_h, corner_radius);
    } else {
        cairo_rectangle(cr, rect_x, rect_y, rect_w, rect_h);
    }
    cairo_clip(cr);

    cairo_translate(cr, rect_x, rect_y);
    cairo_scale(cr, rect_w / bw, cap_h / bh);
    cairo_set_source_surface(cr, patch, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_surface_destroy(patch);

    if (tint_a > 0.0) {
        cairo_save(cr);
        if (corner_radius > 0.0) {
            draw_rounded_rect(cr, rect_x, rect_y, rect_w, rect_h, corner_radius);
        } else {
            cairo_rectangle(cr, rect_x, rect_y, rect_w, rect_h);
        }
        cairo_set_source_rgba(cr, tint_r, tint_g, tint_b, tint_a);
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

void wallpaper_update_dock_blur(aurade_wallpaper_t *wp,
                                cairo_surface_t **cache_surf,
                                int screen_width,
                                double global_ry,
                                double pill_h) {
    if (!wp || !cache_surf || screen_width <= 0 || pill_h <= 0.0) return;
    aurade_server_t *server = wp->server;
    if (!server) return;

    int bw = screen_width / 4;
    int bh = (int)ceil(pill_h / 4.0);
    if (bw < 32) bw = 32;
    if (bh < 8) bh = 8;

    if (!*cache_surf ||
        cairo_image_surface_get_width(*cache_surf) != bw ||
        cairo_image_surface_get_height(*cache_surf) != bh) {
        if (*cache_surf) cairo_surface_destroy(*cache_surf);
        *cache_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
    }

    cairo_surface_t *cache = *cache_surf;

    // Check if any visible window intersects the bottom dock strip
    bool has_overlapping_window = false;
    aurade_view_t *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->workspace != server->current_workspace || view->minimized) continue;
        if (!view->scene_tree || !view->scene_tree->node.enabled) continue;
        if (view->width <= 0 || view->height <= 0) continue;
        if (view->y + view->height > global_ry && view->y < global_ry + pill_h) {
            has_overlapping_window = true;
            break;
        }
    }

    cairo_t *bcr = cairo_create(cache);

    // Fast path: if no windows are behind the dock, sample directly from pre-blurred wallpaper (0ms CPU)
    if (!has_overlapping_window && wp->blurred) {
        int bw_src = cairo_image_surface_get_width(wp->blurred);
        int bh_src = cairo_image_surface_get_height(wp->blurred);
        cairo_save(bcr);
        double sx = (double)bw / (double)bw_src;
        double sy = (double)bh / ((pill_h / wp->height) * (double)bh_src);
        cairo_scale(bcr, sx, sy);
        double src_y = (global_ry / (double)wp->height) * (double)bh_src;
        cairo_set_source_surface(bcr, wp->blurred, 0.0, -src_y);
        cairo_pattern_set_filter(cairo_get_source(bcr), CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(cairo_get_source(bcr), CAIRO_EXTEND_PAD);
        cairo_paint(bcr);
        cairo_restore(bcr);
        cairo_destroy(bcr);
        return;
    }

    double sx = (double)bw / (double)screen_width;
    double sy = (double)bh / pill_h;

    // 1. Paint wallpaper crop
    if (wp->buffer && wp->buffer->surface) {
        cairo_save(bcr);
        cairo_scale(bcr, sx, sy);
        cairo_set_source_surface(bcr, wp->buffer->surface, 0.0, -global_ry);
        cairo_pattern_set_filter(cairo_get_source(bcr), CAIRO_FILTER_GOOD);
        cairo_pattern_set_extend(cairo_get_source(bcr), CAIRO_EXTEND_PAD);
        cairo_paint(bcr);
        cairo_restore(bcr);
    }

    // 2. Composite visible window buffers if any overlap
    if (has_overlapping_window && server->workspace_trees[server->current_workspace]) {
        struct capture_workspace_data cdata = {
            .cr = bcr,
            .scale_x = sx,
            .scale_y = sy,
            .clip_x = 0.0,
            .clip_y = global_ry,
            .clip_w = screen_width,
            .clip_h = pill_h,
        };
        wlr_scene_node_for_each_buffer(&server->workspace_trees[server->current_workspace]->node,
                                       capture_workspace_buffer_iterator, &cdata);
    }
    cairo_destroy(bcr);

    // 3. Multi-pass box blur
    double str = server->config.blur_strength > 0.05 ? server->config.blur_strength : 1.0;
    int r1 = (int)round(2.5 * str);
    int r2 = (int)round(1.5 * str);
    if (r1 < 1) r1 = 1;
    box_blur_surface(cache, r1);
    if (r2 >= 1) box_blur_surface(cache, r2);
}

cairo_surface_t *wallpaper_capture_workspace_blur(aurade_server_t *server, int target_w, int target_h) {
    if (!server || target_w <= 0 || target_h <= 0) return NULL;
    int sw = server->screen_width > 0 ? server->screen_width : 1920;
    int sh = server->screen_height > 0 ? server->screen_height : 1080;

    // Check if any visible windows exist on current workspace
    bool has_windows = false;
    aurade_view_t *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->workspace == server->current_workspace && !view->minimized &&
            view->scene_tree && view->scene_tree->node.enabled &&
            view->width > 0 && view->height > 0) {
            has_windows = true;
            break;
        }
    }

    // Fast path: if no windows are on current workspace, reuse pre-blurred wallpaper directly (0ms)
    if (!has_windows && server->wallpaper && server->wallpaper->blurred) {
        cairo_surface_t *fast_snap = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, target_w, target_h);
        cairo_t *fcr = cairo_create(fast_snap);
        int bw = cairo_image_surface_get_width(server->wallpaper->blurred);
        int bh = cairo_image_surface_get_height(server->wallpaper->blurred);
        cairo_scale(fcr, (double)target_w / bw, (double)target_h / bh);
        cairo_set_source_surface(fcr, server->wallpaper->blurred, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(fcr), CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(cairo_get_source(fcr), CAIRO_EXTEND_PAD);
        cairo_paint(fcr);
        cairo_destroy(fcr);
        return fast_snap;
    }

    cairo_surface_t *snapshot = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, target_w, target_h);
    cairo_t *scr = cairo_create(snapshot);

    double sx = (double)target_w / sw;
    double sy = (double)target_h / sh;

    // 1. Paint wallpaper
    if (server->wallpaper && server->wallpaper->buffer && server->wallpaper->buffer->surface) {
        cairo_save(scr);
        cairo_scale(scr, (double)target_w / server->wallpaper->width,
                         (double)target_h / server->wallpaper->height);
        cairo_set_source_surface(scr, server->wallpaper->buffer->surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(scr), CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(cairo_get_source(scr), CAIRO_EXTEND_PAD);
        cairo_paint(scr);
        cairo_restore(scr);
    }

    // 2. Composite visible window buffers
    if (server->workspace_trees[server->current_workspace]) {
        struct capture_workspace_data cdata = {
            .cr = scr,
            .scale_x = sx,
            .scale_y = sy,
            .clip_x = 0,
            .clip_y = 0,
            .clip_w = sw,
            .clip_h = sh,
        };
        wlr_scene_node_for_each_buffer(&server->workspace_trees[server->current_workspace]->node,
                                       capture_workspace_buffer_iterator, &cdata);
    }
    cairo_destroy(scr);

    // 3. Multi-pass box blur
    double str = server->config.blur_strength > 0.05 ? server->config.blur_strength : 1.0;
    int r1 = (int)round(3.0 * str);
    int r2 = (int)round(2.0 * str);
    if (r1 < 1) r1 = 1;
    box_blur_surface(snapshot, r1);
    if (r2 >= 1) box_blur_surface(snapshot, r2);
    if (str > 1.3) {
        int r3 = (int)round(2.0 * (str - 0.3));
        if (r3 >= 1) box_blur_surface(snapshot, r3);
    }

    return snapshot;
}

void wallpaper_destroy(aurade_wallpaper_t *wp) {
    if (!wp) return;
    if (wp->blurred) cairo_surface_destroy(wp->blurred);
    if (wp->buffer) wlr_buffer_drop(&wp->buffer->base);
    free(wp);
}
