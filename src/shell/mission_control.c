#include "mission_control.h"
#include "topbar.h"
#include "dock.h"
#include "../shell/wallpaper.h"
#include "../util/apps.h"
#include "../window/xdg_shell.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define MC_FONT "Inter, Adwaita Sans, sans-serif"

static double mc_ease_out_cubic(double p) {
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) return 1.0;
    double inv = 1.0 - p;
    return 1.0 - inv * inv * inv;
}

typedef struct {
    double r, g, b, a;
} mc_rgba_t;

typedef struct {
    mc_rgba_t panel_top, panel_bottom;
    mc_rgba_t border, highlight;
    mc_rgba_t text, text_dim;
    mc_rgba_t accent;
    bool dark;
} mc_theme_t;

static mc_theme_t mc_get_theme(const aurade_mission_control_t *mc) {
    bool dark = (mc && mc->server) ? mc->server->config.dark_mode : true;
    mc_theme_t t = {0};
    t.dark = dark;
    t.accent = (mc_rgba_t){0.0, 0.48, 1.0, 1.0};

    if (dark) {
        t.panel_top    = (mc_rgba_t){0.17, 0.19, 0.23, 0.88};
        t.panel_bottom = (mc_rgba_t){0.10, 0.11, 0.14, 0.94};
        t.border       = (mc_rgba_t){1.00, 1.00, 1.00, 0.15};
        t.highlight    = (mc_rgba_t){1.00, 1.00, 1.00, 0.22};
        t.text         = (mc_rgba_t){1.00, 1.00, 1.00, 0.96};
        t.text_dim     = (mc_rgba_t){0.84, 0.86, 0.90, 0.65};
    } else {
        t.panel_top    = (mc_rgba_t){0.96, 0.96, 0.98, 0.88};
        t.panel_bottom = (mc_rgba_t){0.88, 0.89, 0.92, 0.94};
        t.border       = (mc_rgba_t){1.00, 1.00, 1.00, 0.65};
        t.highlight    = (mc_rgba_t){1.00, 1.00, 1.00, 0.90};
        t.text         = (mc_rgba_t){0.10, 0.10, 0.12, 0.94};
        t.text_dim     = (mc_rgba_t){0.34, 0.36, 0.40, 0.75};
    }
    return t;
}

typedef struct {
    double bar_x, bar_y, bar_w, bar_h;
    double thumb_w, thumb_h, gap;
    double pad_x, pad_top, pad_bottom;
    double label_gap, label_h;
    double plus_cx, plus_cy, plus_r;
} mc_bar_geom_t;

static void mc_compute_bar_geom(const aurade_mission_control_t *mc, mc_bar_geom_t *g, double p) {
    int num_ws = (mc && mc->server && mc->server->num_workspaces > 0)
                 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
    int screen_w = (mc && mc->screen_width > 0) ? mc->screen_width : 1920;
    int screen_h = (mc && mc->screen_height > 0) ? mc->screen_height : 1080;

    double gap = 18.0;
    double pad_top = 16.0;
    double pad_bottom = 14.0;
    double label_gap = 8.0;
    double label_h = 18.0;

    double aspect = (double)screen_h / (double)screen_w;
    if (aspect <= 0.1 || aspect > 2.0) aspect = 0.5625;

    double thumb_h = 76.0;
    double thumb_w = round(thumb_h / aspect);

    double max_avail_w = (double)screen_w - 120.0;
    if (thumb_w * num_ws + gap * (num_ws - 1) > max_avail_w) {
        thumb_w = (max_avail_w - gap * (num_ws - 1)) / (double)num_ws;
        if (thumb_w < 60.0) thumb_w = 60.0;
        thumb_h = round(thumb_w * aspect);
    }

    double total_thumbs = thumb_w * num_ws + gap * (num_ws - 1);
    g->thumb_w = thumb_w;
    g->thumb_h = thumb_h;
    g->gap = gap;
    g->pad_x = ((double)screen_w - total_thumbs) / 2.0;
    g->pad_top = pad_top;
    g->pad_bottom = pad_bottom;
    g->label_gap = label_gap;
    g->label_h = label_h;
    g->bar_w = (double)screen_w;
    g->bar_h = pad_top + thumb_h + label_gap + label_h + pad_bottom;
    g->bar_x = 0.0;
    g->bar_y = (p - 1.0) * g->bar_h;

    g->plus_cx = (double)screen_w - 38.0;
    g->plus_cy = pad_top + thumb_h / 2.0;
    g->plus_r = 16.0;
}

/*
 * Query the authoritative visible window geometry (excluding CSD shadow margins).
 */
static void mc_query_view_geometry(aurade_view_t *view, int *out_w, int *out_h) {
    int vw = view ? view->width : 0;
    int vh = view ? view->height : 0;

    if (view && view->xdg_toplevel && view->xdg_toplevel->base) {
        struct wlr_box geo = view->xdg_toplevel->base->geometry;
        if (geo.width > 0 && geo.height > 0) {
            vw = geo.width;
            vh = geo.height;
        } else if (view->xdg_toplevel->base->surface) {
            int sw = view->xdg_toplevel->base->surface->current.width;
            int sh = view->xdg_toplevel->base->surface->current.height;
            if (vw <= 0 && sw > 0) vw = sw;
            if (vh <= 0 && sh > 0) vh = sh;
        }
    } else if (view && view->xwayland_surface) {
        if (view->xwayland_surface->width > 0 && view->xwayland_surface->height > 0) {
            vw = view->xwayland_surface->width;
            vh = view->xwayland_surface->height;
        }
    }

    if (out_w) *out_w = vw;
    if (out_h) *out_h = vh;
}

#include "../window/workspace.h"

static inline double mc_lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

/*
 * Single source of truth for a slot's current integer rectangle and scale.
 * - out_local_x: X relative to the slot's workspace_trees[s->workspace]
 * - out_screen_x: X on screen (including workspace carousel offset + ws_slide_offset)
 */
static void mc_slot_current_rect(aurade_mission_control_t *mc, int idx, double ep,
                                 int *out_local_x, int *out_screen_x, int *out_y,
                                 int *out_w, int *out_h, double *out_scale) {
    mc_window_slot_t *s = &mc->slots[idx];
    if (s->view) {
        int live_w = 0, live_h = 0;
        mc_query_view_geometry(s->view, &live_w, &live_h);
        if (live_w > 0 && live_h > 0 &&
            (fabs(s->orig_w - live_w) > 0.5 || fabs(s->orig_h - live_h) > 0.5)) {
            s->orig_w = (double)live_w;
            s->orig_h = (double)live_h;
            s->target_w = round(s->orig_w * s->scale);
            s->target_h = round(s->orig_h * s->scale);
        }
    }

    int cur_ws = (mc && mc->server) ? mc->server->current_workspace : 0;
    int screen_w = (mc && mc->screen_width > 0) ? mc->screen_width : 1920;
    double ws_dx = (double)(s->workspace - cur_ws) * (double)screen_w + mc->ws_slide_offset;

    double cur_scale = (1.0 - ep) * 1.0 + ep * s->scale;
    double fx_local = (1.0 - ep) * s->orig_x + ep * s->target_x;
    double fy = (1.0 - ep) * s->orig_y + ep * s->target_y;

    if (s->is_closing) {
        double ct = mc_ease_out_cubic(fmin(1.0, s->close_progress));
        cur_scale = s->close_start_scale * (1.0 - 0.40 * ct);
        fx_local = s->close_start_x + (s->orig_w * (s->close_start_scale - cur_scale)) / 2.0;
        fy = s->close_start_y + (s->orig_h * (s->close_start_scale - cur_scale)) / 2.0;
    } else if (s->is_relayout) {
        double rt = mc_ease_out_cubic(fmin(1.0, s->relayout_progress));
        cur_scale = mc_lerp(s->from_scale, s->scale, rt);
        fx_local = mc_lerp(s->from_x, s->target_x, rt);
        fy = mc_lerp(s->from_y, s->target_y, rt);
    }

    int local_rx = (int)round(fx_local);
    int screen_rx = (int)round(fx_local + ws_dx);
    int ry = (int)round(fy);
    int rw = (int)round(s->orig_w * cur_scale);
    int rh = (int)round(s->orig_h * cur_scale);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;

    if (out_local_x) *out_local_x = local_rx;
    if (out_screen_x) *out_screen_x = screen_rx;
    if (out_y) *out_y = ry;
    if (out_w) *out_w = rw;
    if (out_h) *out_h = rh;
    if (out_scale) *out_scale = cur_scale;
}

static const char *mc_view_app_id(const aurade_view_t *v) {
    if (!v) return NULL;
    if (v->xdg_toplevel && v->xdg_toplevel->app_id) {
        return v->xdg_toplevel->app_id;
    }
    if (v->xwayland_surface) {
        return v->xwayland_surface->class ? v->xwayland_surface->class
                                          : v->xwayland_surface->instance;
    }
    return NULL;
}

static const char *mc_view_title(const aurade_view_t *v) {
    if (!v) return "Window";
    if (v->xdg_toplevel && v->xdg_toplevel->title && *v->xdg_toplevel->title) {
        return v->xdg_toplevel->title;
    }
    if (v->xwayland_surface && v->xwayland_surface->title && *v->xwayland_surface->title) {
        return v->xwayland_surface->title;
    }
    const char *app_id = mc_view_app_id(v);
    return (app_id && *app_id) ? app_id : "Window";
}

static void mc_draw_app_icon(cairo_t *cr, double cx, double cy, double size,
                             const char *name, cairo_surface_t *surface, double alpha) {
    if (surface && cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
        int iw = cairo_image_surface_get_width(surface);
        int ih = cairo_image_surface_get_height(surface);
        if (iw > 0 && ih > 0) {
            cairo_save(cr);
            cairo_translate(cr, cx - size / 2.0, cy - size / 2.0);
            cairo_scale(cr, size / (double)iw, size / (double)ih);
            cairo_set_source_surface(cr, surface, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint_with_alpha(cr, alpha);
            cairo_restore(cr);
            return;
        }
    }

    unsigned hash = 2166136261u;
    for (const char *p = name ? name : "App"; *p; p++) {
        hash ^= (unsigned char)*p;
        hash *= 16777619u;
    }
    double h = (hash % 360) / 360.0;
    cairo_save(cr);
    draw_rounded_rect(cr, cx - size / 2.0, cy - size / 2.0, size, size, size * 0.24);

    cairo_pattern_t *pat = cairo_pattern_create_linear(cx, cy - size / 2.0, cx, cy + size / 2.0);
    double r = 0.30 + 0.35 * fabs(sin(h * M_PI));
    double g = 0.30 + 0.35 * fabs(sin((h + 0.33) * M_PI));
    double b = 0.40 + 0.35 * fabs(sin((h + 0.66) * M_PI));
    cairo_pattern_add_color_stop_rgba(pat, 0.0, fmin(1.0, r + 0.18), fmin(1.0, g + 0.18), fmin(1.0, b + 0.18), alpha);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, r * 0.72, g * 0.72, b * 0.72, alpha);
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);

    draw_rounded_rect(cr, cx - size / 2.0 + 1.0, cy - size / 2.0 + 1.0,
                      size - 2.0, size * 0.42, size * 0.20);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18 * alpha);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void mc_paint_wallpaper_thumb(cairo_t *cr, aurade_mission_control_t *mc,
                                     double x, double y, double w, double h, double alpha) {
    if (mc && mc->wallpaper_thumb &&
        cairo_surface_status(mc->wallpaper_thumb) == CAIRO_STATUS_SUCCESS) {
        int bw = cairo_image_surface_get_width(mc->wallpaper_thumb);
        int bh = cairo_image_surface_get_height(mc->wallpaper_thumb);
        if (bw > 0 && bh > 0) {
            cairo_save(cr);
            draw_rounded_rect(cr, x, y, w, h, 11.0);
            cairo_clip(cr);
            cairo_translate(cr, x, y);
            cairo_scale(cr, w / (double)bw, h / (double)bh);
            cairo_set_source_surface(cr, mc->wallpaper_thumb, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
            cairo_paint_with_alpha(cr, alpha);
            cairo_restore(cr);
            return;
        }
    }
    cairo_set_source_rgba(cr, 0.10, 0.12, 0.17, 0.85 * alpha);
    draw_rounded_rect(cr, x, y, w, h, 11.0);
    cairo_fill(cr);
}

/*
 * Compute proportional grid / shelf layout independently for each workspace.
 */
static void mc_compute_layout(aurade_mission_control_t *mc) {
    if (!mc || mc->slot_count <= 0) return;

    mc_bar_geom_t bar;
    mc_compute_bar_geom(mc, &bar, 1.0);

    double avail_x = mc->screen_width * 0.06;
    double avail_w = mc->screen_width * 0.88;
    double avail_y = bar.bar_h + 16.0;
    double avail_h = mc->screen_height - avail_y - 96.0; // Dock clearance at bottom
    if (avail_w < 300.0) avail_w = 300.0;
    if (avail_h < 220.0) avail_h = 220.0;

    int num_ws = (mc->server && mc->server->num_workspaces > 0)
                 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;

    double label_reserve = 44.0;

    for (int ws = 0; ws < num_ws; ws++) {
        int ws_indices[MC_MAX_SLOTS];
        int n = 0;
        for (int i = 0; i < mc->slot_count; i++) {
            if (mc->slots[i].workspace == ws && !mc->slots[i].is_closing) {
                ws_indices[n++] = i;
            }
        }
        if (n <= 0) continue;

        int cols = 1;
        if (n == 2) cols = 2;
        else if (n == 3 || n == 4) cols = 2;
        else if (n <= 9) cols = 3;
        else cols = 4;

        int rows = (n + cols - 1) / cols;

        double gap_x = (n <= 2) ? 44.0 : 32.0;
        double gap_y = (rows <= 1) ? 0.0 : 44.0;

        double cell_w = (avail_w - gap_x * (cols - 1)) / (double)cols;
        double cell_h = (avail_h - gap_y * (rows - 1)) / (double)rows;

        double max_scale_cap = (n == 1) ? 0.72 : (n == 2 ? 0.58 : 0.50);
        double fit_ratio_w = (n == 1) ? 0.65 : (n == 2 ? 0.92 : 0.90);
        double fit_ratio_h = (n == 1) ? 0.70 : (n == 2 ? 0.78 : 0.84);

        double total_grid_h = rows * cell_h + (rows - 1) * gap_y;
        double start_y = avail_y + (avail_h - total_grid_h) / 2.0;

        for (int r = 0; r < rows; r++) {
            int in_row = (r == rows - 1) ? (n - r * cols) : cols;
            double row_w = 0.0;

            for (int c = 0; c < in_row; c++) {
                int idx = ws_indices[r * cols + c];
                mc_window_slot_t *s = &mc->slots[idx];
                double ow = s->orig_w > 0.0 ? s->orig_w : 800.0;
                double oh = s->orig_h > 0.0 ? s->orig_h : 600.0;

                double max_w = (n == 1) ? (avail_w * fit_ratio_w) : (cell_w * fit_ratio_w);
                double max_h = (n == 1) ? ((avail_h - label_reserve) * fit_ratio_h)
                                        : ((cell_h - label_reserve) * fit_ratio_h);

                double sc = fmin(max_w / ow, max_h / oh);
                if (sc > max_scale_cap) sc = max_scale_cap;
                if (sc < 0.12) sc = 0.12;

                double tw = round(ow * sc);
                double th = round(oh * sc);

                s->scale = sc;
                s->target_w = tw;
                s->target_h = th;
                row_w += tw;
            }

            row_w += gap_x * (in_row - 1);
            double cur_x = avail_x + (avail_w - row_w) / 2.0;
            double row_cy = start_y + r * (cell_h + gap_y) + (cell_h - label_reserve) / 2.0;

            for (int c = 0; c < in_row; c++) {
                int idx = ws_indices[r * cols + c];
                mc_window_slot_t *s = &mc->slots[idx];
                s->target_x = round(cur_x);
                s->target_y = round(row_cy - s->target_h / 2.0);
                cur_x += s->target_w + gap_x;
            }
        }
    }
}

static void mc_init_slot_metadata(aurade_mission_control_t *mc) {
    if (!mc) return;

    for (int i = 0; i < mc->slot_count; i++) {
        mc_window_slot_t *s = &mc->slots[i];
        aurade_view_t *v = s->view;
        if (!v) continue;

        const char *app_id = mc_view_app_id(v);
        desktop_app_t *app = apps_find(mc->server->apps, app_id);
        s->app = app;

        const char *title = (app && *app->name) ? app->name : mc_view_title(v);
        snprintf(s->display_title, sizeof(s->display_title), "%s", title ? title : "Window");
        s->title_width = (double)(strlen(s->display_title) * 8.0);
        s->badge_w = s->title_width + 24.0;
    }
}

static void mc_collect_workspace_slots(aurade_mission_control_t *mc) {
    if (!mc || !mc->server) return;
    mc->slot_count = 0;

    aurade_view_t *view;
    wl_list_for_each(view, &mc->server->views, link) {
        if (view->minimized) continue;
        if (mc->slot_count >= MC_MAX_SLOTS) break;

        int vw = 0, vh = 0;
        mc_query_view_geometry(view, &vw, &vh);
        if (vw < 60 || vh <= 0) continue;

        view->width = vw;
        view->height = vh;

        mc_window_slot_t *s = &mc->slots[mc->slot_count++];
        memset(s, 0, sizeof(*s));
        s->view = view;
        s->workspace = view->workspace;
        s->orig_x = (double)view->x;
        s->orig_y = (double)view->y;
        s->orig_w = (double)vw;
        s->orig_h = (double)vh;
        s->scale = 1.0;
    }

    if (mc->slot_count > 0) {
        mc_compute_layout(mc);
        mc_init_slot_metadata(mc);
    }
}

static void mc_start_relayout_animation(aurade_mission_control_t *mc, int workspace) {
    if (!mc) return;
    double ep = mc_ease_out_cubic(mc->progress);
    for (int i = 0; i < mc->slot_count; i++) {
        mc_window_slot_t *s = &mc->slots[i];
        if (s->workspace != workspace || s->is_closing) continue;
        int local_rx = 0, ry = 0;
        double cur_scale = 1.0;
        mc_slot_current_rect(mc, i, ep, &local_rx, NULL, &ry, NULL, NULL, &cur_scale);
        s->from_x = (double)local_rx;
        s->from_y = (double)ry;
        s->from_scale = cur_scale;
        s->is_relayout = true;
        s->relayout_progress = 0.0;
    }
    mc_compute_layout(mc);
    mc_init_slot_metadata(mc);
    mc->spaces_bar_dirty = true;
    schedule_all_outputs(mc->server);
}

static void mc_restore_slots_to_desktop(aurade_mission_control_t *mc) {
    if (!mc) return;
    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *view = mc->slots[i].view;
        if (!view || !view->scene_tree) continue;
        view->x = (int)round(mc->slots[i].orig_x);
        view->y = (int)round(mc->slots[i].orig_y);
        view->width = (int)round(mc->slots[i].orig_w);
        view->height = (int)round(mc->slots[i].orig_h);
        wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
        view_set_scale(view, 1.0, 1.0);
        view_set_opacity(view, 1.0f);
    }
    if (mc->server) {
        mc->server->workspace_offset = 0.0;
        server_update_workspace_positions(mc->server);
    }
}

static void mc_apply_slots_scene_state(aurade_mission_control_t *mc, double ep) {
    if (!mc) return;
    if (mc->server) {
        mc->server->workspace_offset = (ep > 0.05) ? mc->ws_slide_offset : 0.0;
        server_update_workspace_positions(mc->server);
    }
    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *view = mc->slots[i].view;
        if (!view || !view->scene_tree) continue;
        if (mc->is_dragging && mc->dragged_view == view) continue;
        if (mc->drop_anim.active && mc->drop_anim.view == view) continue;

        int local_rx = 0, ry = 0, rw = 0, rh = 0;
        double cur_scale = 1.0;
        mc_slot_current_rect(mc, i, ep, &local_rx, NULL, &ry, &rw, &rh, &cur_scale);

        wlr_scene_node_set_position(&view->scene_tree->node, local_rx, ry);
        view_set_scale(view, cur_scale, cur_scale);

        if (mc->slots[i].is_closing) {
            double ct = mc_ease_out_cubic(fmin(1.0, mc->slots[i].close_progress));
            view_set_opacity(view, (float)fmax(0.0, 1.0 - ct));
        } else {
            view_set_opacity(view, 1.0f);
        }
    }
}

void mission_control_forget_view(aurade_mission_control_t *mc, aurade_view_t *view) {
    if (!mc || !view) return;
    if (mc->drop_anim.active && mc->drop_anim.view == view) {
        mc->drop_anim.active = false;
        mc->drop_anim.view = NULL;
    }
    for (int i = 0; i < mc->slot_count; i++) {
        if (mc->slots[i].view != view) continue;
        for (int j = i; j < mc->slot_count - 1; j++) {
            mc->slots[j] = mc->slots[j + 1];
        }
        mc->slot_count--;
        if (mc->hovered_view == view) mc->hovered_view = NULL;
        if (mc->hovered_close_view == view) mc->hovered_close_view = NULL;
        if (mc->dragged_view == view) {
            mc->dragged_view = NULL;
            mc->is_dragging = false;
        }
        i--;
    }
    mc->spaces_bar_dirty = true;
}

aurade_mission_control_t *mission_control_create(aurade_server_t *server) {
    aurade_mission_control_t *mc = calloc(1, sizeof(*mc));
    if (!mc) return NULL;
    mc->server = server;
    mc->screen_width = 1920;
    mc->screen_height = 1080;
    mc->progress = 0.0;
    mc->active = false;
    mc->drag_target_workspace = -1;
    mc->hovered_workspace = -1;

    mc->buffer = cairo_buffer_create(mc->screen_width, mc->screen_height);
    mc->scene_buffer = wlr_scene_buffer_create(server->overlay_tree, &mc->buffer->base);
    wlr_scene_node_set_enabled(&mc->scene_buffer->node, false);
    mc->hovered_ws_close = -1;
    mc->hovered_plus = false;
    mc->is_dragging_ws = false;
    mc->ws_drag_candidate = -1;
    mc->ws_close_candidate = -1;
    mc->plus_candidate = false;
    mc->drag_ws = -1;
    mc->ws_drag_target = -1;

    return mc;
}

void mission_control_resize(aurade_mission_control_t *mc, int width, int height) {
    if (!mc) return;
    mc->screen_width = width;
    mc->screen_height = height;
    cairo_buffer_resize(mc->buffer, width, height);
    if (mc->spaces_bar_surf) {
        cairo_surface_destroy(mc->spaces_bar_surf);
        mc->spaces_bar_surf = NULL;
    }
    mc->spaces_bar_dirty = true;
}

bool mission_control_is_animating(const aurade_mission_control_t *mc) {
    if (!mc || !mc->active) return false;
    if (mc->ws_animating) return true;
    if (mc->drop_anim.active) return true;
    if (mc->ws_bar_anim.active) return true;
    for (int i = 0; i < mc->slot_count; i++) {
        if (mc->slots[i].is_closing || mc->slots[i].is_relayout) return true;
    }
    return false;
}

void mission_control_begin(aurade_mission_control_t *mc) {
    if (!mc || !mc->server) return;

    // Settle any in-progress workspace transition
    if (mc->server->workspace_animating || mc->server->workspace_offset != 0.0) {
        int screen_w = mc->server->screen_width > 0 ? mc->server->screen_width : 1920;
        double p = mc->server->current_workspace - mc->server->workspace_offset / (double)screen_w;
        int target = (int)round(p);
        int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
        if (target < 0) target = 0;
        if (target >= num_ws) target = num_ws - 1;

        mc->server->current_workspace = target;
        mc->server->workspace_animating = false;
        mc->server->workspace_offset = 0.0;
        server_update_workspace_positions(mc->server);
    }

    // Settle any active window open/minimize/geometry animations across all workspaces
    aurade_view_t *v;
    wl_list_for_each(v, &mc->server->views, link) {
        if (!v->minimized && v->anim_type != VIEW_ANIM_NONE) {
            v->anim_type = VIEW_ANIM_NONE;
            wlr_scene_node_set_position(&v->scene_tree->node, v->x, v->y);
            view_set_scale(v, 1.0, 1.0);
            view_set_opacity(v, 1.0f);
        }
    }

    mc->active = true;
    mc->progress = 0.0;
    mc->slot_count = 0;
    mc->hovered_view = NULL;
    mc->hovered_close_view = NULL;
    mc->hovered_workspace = -1;
    mc->hovered_ws_close = -1;
    mc->hovered_plus = false;
    mc->mouse_pressed = false;
    mc->is_dragging = false;
    mc->dragged_view = NULL;
    mc->drag_target_workspace = -1;
    mc->is_dragging_ws = false;
    mc->ws_drag_candidate = -1;
    mc->ws_close_candidate = -1;
    mc->plus_candidate = false;
    mc->drag_ws = -1;
    mc->ws_drag_target = -1;
    mc->ws_drag_start_x = mc->ws_drag_start_y = 0.0;
    mc->ws_drag_current_x = mc->ws_drag_current_y = 0.0;
    mc->drop_anim.active = false;
    mc->drop_anim.view = NULL;
    mc->ws_bar_anim.active = false;
    mc->ws_bar_anim.type = WS_BAR_ANIM_NONE;
    mc->spaces_bar_dirty = true;
    mc->ws_animating = false;
    mc->ws_gesture_active = false;
    mc->ws_gesture_base_offset = 0.0;
    mc->ws_slide_offset = 0.0;
    mc->scroll_accum_x = 0.0;
    mc->last_scroll_time_ms = 0;

    server_dismiss_popups(mc->server);
    if (mc->server->dock && dock_context_menu_is_open(mc->server->dock)) {
        dock_close_context_menu(mc->server->dock);
    }
    if (mc->server->popup_tree) {
        wlr_scene_node_set_enabled(&mc->server->popup_tree->node, false);
    }
    if (mc->server->topbar) {
        topbar_close_power_menu(mc->server->topbar);
        if (mc->server->topbar->scene_buffer) {
            wlr_scene_node_set_enabled(&mc->server->topbar->scene_buffer->node, false);
        }
        if (mc->server->topbar->blur_node) {
            wlr_scene_node_set_enabled(&mc->server->topbar->blur_node->node, false);
        }
    }
    if (mc->server->dock && mc->server->dock->blur_node) {
        bool dock_glass = dock_uses_pill_blur(mc->server->dock) &&
                          (mc->server->config.blur_dock || mc->server->config.liquid_glass);
        wlr_scene_node_set_enabled(&mc->server->dock->blur_node->node, dock_glass);
    }

    wlr_scene_node_set_enabled(&mc->scene_buffer->node, true);

    // Pre-cache downscaled wallpaper thumbnail for Spaces bar
    if (mc->wallpaper_thumb) {
        cairo_surface_destroy(mc->wallpaper_thumb);
        mc->wallpaper_thumb = NULL;
    }
    mc_bar_geom_t bar;
    mc_compute_bar_geom(mc, &bar, 1.0);
    int tw = (int)ceil(bar.thumb_w);
    int th = (int)ceil(bar.thumb_h);
    if (tw > 0 && th > 0 && mc->server->wallpaper) {
        aurade_wallpaper_t *wp = mc->server->wallpaper;
        cairo_surface_t *src = (wp->buffer && wp->buffer->surface) ? wp->buffer->surface : wp->blurred;
        if (src && cairo_surface_status(src) == CAIRO_STATUS_SUCCESS) {
            int sw = cairo_image_surface_get_width(src);
            int sh = cairo_image_surface_get_height(src);
            if (sw > 0 && sh > 0) {
                mc->wallpaper_thumb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
                cairo_t *tcr = cairo_create(mc->wallpaper_thumb);
                cairo_scale(tcr, (double)tw / sw, (double)th / sh);
                cairo_set_source_surface(tcr, src, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(tcr), CAIRO_FILTER_GOOD);
                cairo_paint(tcr);
                cairo_destroy(tcr);
            }
        }
    }

    mc_collect_workspace_slots(mc);
    mc_apply_slots_scene_state(mc, 0.0);
    mission_control_render(mc);
}

void mission_control_update_progress(aurade_mission_control_t *mc, double progress) {
    if (!mc || !mc->active) return;
    mc->progress = progress;

    if (progress <= 0.001) {
        if (mc->server && mc->server->wallpaper) {
            wallpaper_set_overview_blur(mc->server->wallpaper, 0.0);
        }
        if (mc->server && mc->server->popup_tree) {
            wlr_scene_node_set_enabled(&mc->server->popup_tree->node, true);
        }
        if (mc->server && mc->server->topbar) {
            if (mc->server->topbar->scene_buffer) {
                wlr_scene_node_set_enabled(&mc->server->topbar->scene_buffer->node, true);
            }
            if (mc->server->topbar->blur_node) {
                wlr_scene_node_set_enabled(&mc->server->topbar->blur_node->node, false);
            }
        }
        mc_restore_slots_to_desktop(mc);
        wlr_scene_node_set_enabled(&mc->scene_buffer->node, false);
        mc->needs_render = false;
        server_update_workspace_positions(mc->server);
        if (mc->server && !mc->server->gesture_in_progress && !mc->server->gesture_animating) {
            mc->active = false;
            mc->server->gesture_mode = GESTURE_MODE_IDLE;
        }
        return;
    }

    if (mc->server && mc->server->topbar) {
        if (mc->server->topbar->scene_buffer) {
            wlr_scene_node_set_enabled(&mc->server->topbar->scene_buffer->node, false);
        }
        if (mc->server->topbar->blur_node) {
            wlr_scene_node_set_enabled(&mc->server->topbar->blur_node->node, false);
        }
    }

    wlr_scene_node_set_enabled(&mc->scene_buffer->node, true);
    if (mc->server && mc->server->wallpaper && mc->server->config.blur_workspace_overview) {
        wallpaper_set_overview_blur(mc->server->wallpaper, progress);
    }

    double ep = mc_ease_out_cubic(progress);
    mc_apply_slots_scene_state(mc, ep);
    mc->needs_render = true;
}

void mission_control_end(aurade_mission_control_t *mc, bool opened) {
    if (!mc) return;
    mc->active = opened;
    if (!opened) {
        mc->ws_animating = false;
        mc->ws_gesture_active = false;
        mc->ws_gesture_base_offset = 0.0;
        mc->ws_slide_offset = 0.0;
        mc->scroll_accum_x = 0.0;
        if (mc->is_dragging && mc->dragged_view) {
            wlr_scene_node_reparent(&mc->dragged_view->scene_tree->node,
                                     mc->server->workspace_trees[mc->dragged_view->workspace]);
            mc->is_dragging = false;
            mc->dragged_view = NULL;
        }
        if (mc->drop_anim.active) {
            aurade_view_t *dv = mc->drop_anim.view;
            if (dv && dv->scene_tree) {
                dv->workspace = mc->drop_anim.to_workspace;
                wlr_scene_node_reparent(&dv->scene_tree->node,
                                        mc->server->workspace_trees[dv->workspace]);
                view_set_scale(dv, 1.0, 1.0);
                view_set_opacity(dv, 1.0f);
            }
            mc->drop_anim.active = false;
            mc->drop_anim.view = NULL;
        }
        if (mc->ws_bar_anim.active) {
            if (mc->ws_bar_anim.type == WS_BAR_ANIM_DELETE) {
                server_delete_workspace(mc->server, mc->ws_bar_anim.index);
            }
            mc->ws_bar_anim.active = false;
            mc->ws_bar_anim.type = WS_BAR_ANIM_NONE;
        }
        for (int i = 0; i < mc->slot_count; i++) {
            if (mc->slots[i].is_closing && mc->slots[i].view) {
                view_set_opacity(mc->slots[i].view, 1.0f);
                view_close(mc->slots[i].view);
                mc->slots[i].is_closing = false;
            }
            mc->slots[i].is_relayout = false;
        }
        mc->is_dragging_ws = false;
        mc->drag_ws = -1;
        mc->ws_drag_candidate = -1;
        mc->ws_drag_target = -1;
        mc->ws_close_candidate = -1;
        mc->plus_candidate = false;
        if (mc->server && mc->server->topbar) {
            if (mc->server->topbar->scene_buffer) {
                wlr_scene_node_set_enabled(&mc->server->topbar->scene_buffer->node, true);
            }
            if (mc->server->topbar->blur_node) {
                wlr_scene_node_set_enabled(&mc->server->topbar->blur_node->node, false);
            }
        }
        if (mc->server && mc->server->dock && mc->server->dock->blur_node) {
            wlr_scene_node_set_enabled(&mc->server->dock->blur_node->node,
                                       dock_uses_pill_blur(mc->server->dock) &&
                                       (mc->server->config.blur_dock || mc->server->config.liquid_glass));
        }
        if (mc->server && mc->server->popup_tree) {
            wlr_scene_node_set_enabled(&mc->server->popup_tree->node, true);
        }
        mc->progress = 0.0;
        if (mc->server && mc->server->wallpaper) {
            wallpaper_set_overview_blur(mc->server->wallpaper, 0.0);
        }
        mc_restore_slots_to_desktop(mc);
        wlr_scene_node_set_enabled(&mc->scene_buffer->node, false);
        mc->needs_render = false;
        server_update_workspace_positions(mc->server);
    }
}

bool mission_control_handle_press(aurade_mission_control_t *mc, double x, double y) {
    if (!mc || !mc->active || mc->progress < 0.4) return false;
    mc->mouse_pressed = true;
    mc->is_dragging = false;
    mc->dragged_view = NULL;
    mc->drag_target_workspace = -1;
    mc->is_dragging_ws = false;
    mc->ws_drag_candidate = -1;
    mc->ws_close_candidate = -1;
    mc->plus_candidate = false;
    mc->drag_start_x = x;
    mc->drag_start_y = y;

    double ep = mc_ease_out_cubic(mc->progress);

    // 1. Check close button on any visible slot (only when window is hovered)
    for (int i = 0; i < mc->slot_count; i++) {
        if (mc->slots[i].is_closing) continue;
        if (mc->drop_anim.active && mc->drop_anim.view == mc->slots[i].view) continue;
        if (mc->slots[i].view != mc->hovered_view) continue;
        int rx = 0, ry = 0, rw = 0, rh = 0;
        mc_slot_current_rect(mc, i, ep, NULL, &rx, &ry, &rw, &rh, NULL);
        if (rx + rw < 0 || rx > mc->screen_width) continue;
        double btn_x = rx + 16.0;
        double btn_y = ry + 16.0;
        if (hypot(x - btn_x, y - btn_y) <= 16.0) {
            return true;
        }
    }

    // 2. Check window slot body for drag or click (including app icon & title below)
    for (int i = 0; i < mc->slot_count; i++) {
        if (mc->slots[i].is_closing) continue;
        if (mc->drop_anim.active && mc->drop_anim.view == mc->slots[i].view) continue;
        int rx = 0, ry = 0, rw = 0, rh = 0;
        mc_slot_current_rect(mc, i, ep, NULL, &rx, &ry, &rw, &rh, NULL);
        if (rx + rw < 0 || rx > mc->screen_width) continue;
        if (x >= rx && x <= rx + rw && y >= ry && y <= ry + rh + 42.0) {
            mc->drag_slot = i;
            mc->dragged_view = mc->slots[i].view;
            mc->drag_offset_x = x - rx;
            mc->drag_offset_y = y - ry;
            return true;
        }
    }

    // 3. Spaces bar check
    mc_bar_geom_t bar;
    mc_compute_bar_geom(mc, &bar, 1.0);
    if (y >= bar.bar_y && y <= bar.bar_y + bar.bar_h) {
        if (mc->ws_bar_anim.active) return true;
        int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;

        // Plus button: add desktop
        if (hypot(x - bar.plus_cx, y - (bar.bar_y + bar.plus_cy)) <= bar.plus_r + 6.0) {
            mc->plus_candidate = true;
            return true;
        }

        // Workspace close button '✕' candidate
        if (num_ws > 1) {
            for (int i = 0; i < num_ws; i++) {
                double sx = bar.pad_x + i * (bar.thumb_w + bar.gap);
                double sy = bar.bar_y + bar.pad_top;
                if (hypot(x - (sx + 12.0), y - (sy + 12.0)) <= 14.0) {
                    mc->ws_close_candidate = i;
                    return true;
                }
            }
        }

        // Workspace thumbnail (drag candidate or click candidate)
        for (int i = 0; i < num_ws; i++) {
            double sx = bar.pad_x + i * (bar.thumb_w + bar.gap);
            double sy = bar.bar_y + bar.pad_top;
            if (x >= sx - 4.0 && x <= sx + bar.thumb_w + 4.0 &&
                y >= sy - 4.0 && y <= sy + bar.thumb_h + bar.label_gap + bar.label_h + 6.0) {
                mc->ws_drag_candidate = i;
                mc->ws_drag_start_x = x;
                mc->ws_drag_start_y = y;
                mc->ws_drag_current_x = x;
                mc->ws_drag_current_y = y;
                mc->is_dragging_ws = false;
                mc->drag_ws = i;
                mc->ws_drag_target = i;
                return true;
            }
        }
        return true;
    }

    return true;
}

void mission_control_handle_motion(aurade_mission_control_t *mc, double x, double y) {
    if (!mc || !mc->active || mc->progress < 0.4) return;

    double ep = mc_ease_out_cubic(mc->progress);

    // Dragging a window slot
    if (mc->mouse_pressed && mc->dragged_view) {
        if (!mc->is_dragging && hypot(x - mc->drag_start_x, y - mc->drag_start_y) > 6.0) {
            mc->is_dragging = true;
            wlr_scene_node_reparent(&mc->dragged_view->scene_tree->node, mc->server->dock_tree);
            wlr_scene_node_raise_to_top(&mc->dragged_view->scene_tree->node);
            view_set_opacity(mc->dragged_view, 0.92f);
            mc->needs_render = true;
        }

        if (mc->is_dragging) {
            int win_x = (int)round(x - mc->drag_offset_x);
            int win_y = (int)round(y - mc->drag_offset_y);
            wlr_scene_node_set_position(&mc->dragged_view->scene_tree->node, win_x, win_y);

            mc_bar_geom_t bar;
            mc_compute_bar_geom(mc, &bar, 1.0);
            int prev_target = mc->drag_target_workspace;
            mc->drag_target_workspace = -1;

            if (y >= bar.bar_y && y <= bar.bar_y + bar.bar_h + 12.0) {
                int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
                for (int i = 0; i < num_ws; i++) {
                    double sx = bar.bar_x + bar.pad_x + i * (bar.thumb_w + bar.gap);
                    if (x >= sx - 4.0 && x <= sx + bar.thumb_w + 4.0) {
                        mc->drag_target_workspace = i;
                        break;
                    }
                }
            }

            if (mc->drag_target_workspace != prev_target) {
                mc->spaces_bar_dirty = true;
                mc->needs_render = true;
            }
            schedule_all_outputs(mc->server);
            return;
        }
    }

    // Dragging a desktop thumbnail in Spaces bar
    if (mc->mouse_pressed && mc->ws_drag_candidate >= 0) {
        if (!mc->is_dragging_ws && hypot(x - mc->ws_drag_start_x, y - mc->ws_drag_start_y) > 6.0) {
            mc->is_dragging_ws = true;
            mc->drag_ws = mc->ws_drag_candidate;
        }

        if (mc->is_dragging_ws) {
            mc->ws_drag_current_x = x;
            mc->ws_drag_current_y = y;
            mc_bar_geom_t bar;
            mc_compute_bar_geom(mc, &bar, 1.0);
            int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;

            double step = bar.thumb_w + bar.gap;
            int target = (int)round((x - bar.pad_x - bar.thumb_w / 2.0) / step);
            if (target < 0) target = 0;
            if (target >= num_ws) target = num_ws - 1;

            if (target != mc->ws_drag_target) {
                mc->ws_drag_target = target;
            }
            mc->spaces_bar_dirty = true;
            mission_control_render(mc);
            schedule_all_outputs(mc->server);
            return;
        }
    }

    int prev_ws_hover = mc->hovered_workspace;
    int prev_ws_close = mc->hovered_ws_close;
    bool prev_hover_plus = mc->hovered_plus;

    mc->hovered_workspace = -1;
    mc->hovered_ws_close = -1;
    mc->hovered_plus = false;

    mc_bar_geom_t bar;
    mc_compute_bar_geom(mc, &bar, 1.0);
    int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;

    if (y >= bar.bar_y && y <= bar.bar_y + bar.bar_h) {
        if (hypot(x - bar.plus_cx, y - (bar.bar_y + bar.plus_cy)) <= bar.plus_r + 6.0) {
            mc->hovered_plus = true;
        } else {
            for (int i = 0; i < num_ws; i++) {
                double sx = bar.pad_x + i * (bar.thumb_w + bar.gap);
                double sy = bar.bar_y + bar.pad_top;
                if (num_ws > 1 && hypot(x - (sx + 12.0), y - (sy + 12.0)) <= 14.0) {
                    mc->hovered_ws_close = i;
                    mc->hovered_workspace = i;
                    break;
                }
                if (x >= sx - 4.0 && x <= sx + bar.thumb_w + 4.0 &&
                    y >= sy - 4.0 && y <= sy + bar.thumb_h + bar.label_gap + bar.label_h + 6.0) {
                    mc->hovered_workspace = i;
                    break;
                }
            }
        }
    }

    aurade_view_t *prev_close = mc->hovered_close_view;
    aurade_view_t *prev_hover = mc->hovered_view;
    mc->hovered_close_view = NULL;
    mc->hovered_view = NULL;

    for (int i = 0; i < mc->slot_count; i++) {
        if (mc->slots[i].is_closing) continue;
        if (mc->drop_anim.active && mc->drop_anim.view == mc->slots[i].view) continue;
        int rx = 0, ry = 0, rw = 0, rh = 0;
        mc_slot_current_rect(mc, i, ep, NULL, &rx, &ry, &rw, &rh, NULL);
        if (rx + rw < 0 || rx > mc->screen_width) continue;
        double btn_x = rx + 16.0;
        double btn_y = ry + 16.0;
        if (mc->slots[i].view == prev_hover && hypot(x - btn_x, y - btn_y) <= 16.0) {
            mc->hovered_close_view = mc->slots[i].view;
            mc->hovered_view = mc->slots[i].view;
            break;
        }
        if (x >= rx && x <= rx + rw && y >= ry && y <= ry + rh + 42.0) {
            mc->hovered_view = mc->slots[i].view;
            break;
        }
    }

    if (mc->hovered_workspace != prev_ws_hover ||
        mc->hovered_ws_close != prev_ws_close ||
        mc->hovered_plus != prev_hover_plus) {
        mc->spaces_bar_dirty = true;
    }
    if (mc->hovered_view != prev_hover ||
        mc->hovered_close_view != prev_close ||
        mc->hovered_workspace != prev_ws_hover ||
        mc->hovered_ws_close != prev_ws_close ||
        mc->hovered_plus != prev_hover_plus) {
        mission_control_render(mc);
        schedule_all_outputs(mc->server);
    }
}

bool mission_control_handle_release(aurade_mission_control_t *mc, double x, double y) {
    if (!mc || !mc->active || mc->progress < 0.4) return false;
    if (!mc->mouse_pressed) return false;

    bool was_dragging = mc->is_dragging;
    aurade_view_t *dragged = mc->dragged_view;
    int target_ws = mc->drag_target_workspace;
    int drag_slot = mc->drag_slot;

    mc->mouse_pressed = false;
    mc->is_dragging = false;
    mc->dragged_view = NULL;
    mc->drag_target_workspace = -1;

    double ep = mc_ease_out_cubic(mc->progress);

    // A. Dragging window slot release
    if (was_dragging && dragged) {
        if (target_ws >= 0 && target_ws != dragged->workspace) {
            int old_ws = dragged->workspace;
            mc_bar_geom_t bar;
            mc_compute_bar_geom(mc, &bar, 1.0);
            double sx = bar.bar_x + bar.pad_x + target_ws * (bar.thumb_w + bar.gap);
            double sy = bar.bar_y + bar.pad_top;

            double scr_w = (double)(mc->screen_width > 0 ? mc->screen_width : 1920);
            double thumb_scale = bar.thumb_w / scr_w;
            double win_thumb_w = (double)dragged->width * thumb_scale;
            double win_thumb_h = (double)dragged->height * thumb_scale;

            double s_scale = (dragged->current_scale > 0.01) ? dragged->current_scale : 0.35;
            if (drag_slot >= 0 && drag_slot < mc->slot_count && mc->slots[drag_slot].view == dragged) {
                if (mc->slots[drag_slot].scale > 0.01) s_scale = mc->slots[drag_slot].scale;
            }

            mc->drop_anim.active = true;
            mc->drop_anim.is_return = false;
            mc->drop_anim.view = dragged;
            mc->drop_anim.from_workspace = old_ws;
            mc->drop_anim.to_workspace = target_ws;
            mc->drop_anim.progress = 0.0;
            mc->drop_anim.start_x = (double)round(x - mc->drag_offset_x);
            mc->drop_anim.start_y = (double)round(y - mc->drag_offset_y);
            mc->drop_anim.target_x = sx + (bar.thumb_w - win_thumb_w) / 2.0;
            mc->drop_anim.target_y = sy + (bar.thumb_h - win_thumb_h) / 2.0;
            mc->drop_anim.start_scale = s_scale;
            mc->drop_anim.target_scale = thumb_scale;

            wlr_scene_node_reparent(&dragged->scene_tree->node, mc->server->dock_tree);
            wlr_scene_node_raise_to_top(&dragged->scene_tree->node);

            dragged->workspace = target_ws;
            dragged->tile_order = ++mc->server->next_view_id;
            if (drag_slot >= 0 && drag_slot < mc->slot_count && mc->slots[drag_slot].view == dragged) {
                mc->slots[drag_slot].workspace = target_ws;
            }

            if (mc->server->config.tiling_enabled) {
                workspace_relayout_tiling(mc->server, old_ws);
                for (int i = 0; i < mc->slot_count; i++) {
                    aurade_view_t *sv = mc->slots[i].view;
                    if (!sv || sv->workspace != old_ws) continue;
                    if (sv->anim_type == VIEW_ANIM_GEOMETRY) {
                        view_anim_settle(sv);
                    }
                    mc->slots[i].orig_x = (double)sv->x;
                    mc->slots[i].orig_y = (double)sv->y;
                    mc->slots[i].orig_w = (double)sv->width;
                    mc->slots[i].orig_h = (double)sv->height;
                }
            }
            mc_start_relayout_animation(mc, old_ws);
            mc->needs_render = true;
            schedule_all_outputs(mc->server);
            return true;
        } else {
            // Smooth return flight back to slot!
            int target_rx = 0, target_ry = 0;
            double target_scale = 1.0;
            if (drag_slot >= 0 && drag_slot < mc->slot_count && mc->slots[drag_slot].view == dragged) {
                mc_slot_current_rect(mc, drag_slot, ep, NULL, &target_rx, &target_ry, NULL, NULL, &target_scale);
            } else {
                target_rx = (int)round(dragged->x);
                target_ry = (int)round(dragged->y);
            }

            double s_scale = (dragged->current_scale > 0.01) ? dragged->current_scale : target_scale;

            mc->drop_anim.active = true;
            mc->drop_anim.is_return = true;
            mc->drop_anim.view = dragged;
            mc->drop_anim.from_workspace = dragged->workspace;
            mc->drop_anim.to_workspace = dragged->workspace;
            mc->drop_anim.progress = 0.0;
            mc->drop_anim.start_x = (double)round(x - mc->drag_offset_x);
            mc->drop_anim.start_y = (double)round(y - mc->drag_offset_y);
            mc->drop_anim.target_x = (double)target_rx;
            mc->drop_anim.target_y = (double)target_ry;
            mc->drop_anim.start_scale = s_scale;
            mc->drop_anim.target_scale = target_scale;

            wlr_scene_node_reparent(&dragged->scene_tree->node, mc->server->dock_tree);
            wlr_scene_node_raise_to_top(&dragged->scene_tree->node);

            mc->needs_render = true;
            schedule_all_outputs(mc->server);
            return true;
        }
    }

    // B. Dragging desktop thumbnail release (reorder desktops)
    if (mc->is_dragging_ws) {
        int from_ws = mc->drag_ws;
        int to_ws = mc->ws_drag_target;
        int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;

        mc->is_dragging_ws = false;
        mc->ws_drag_candidate = -1;
        mc->drag_ws = -1;
        mc->ws_drag_target = -1;

        if (from_ws != to_ws && from_ws >= 0 && to_ws >= 0 && from_ws < num_ws && to_ws < num_ws) {
            server_reorder_workspace(mc->server, from_ws, to_ws);
            mc_collect_workspace_slots(mc);
            mc_apply_slots_scene_state(mc, ep);
        }

        mc->spaces_bar_dirty = true;
        mission_control_render(mc);
        schedule_all_outputs(mc->server);
        return true;
    }

    // 1. Close button click on window slot (only when close button was hovered)
    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *view = mc->slots[i].view;
        if (!view || view != mc->hovered_view) continue;
        if (mc->slots[i].is_closing) continue;
        int rx = 0, ry = 0, rw = 0, rh = 0;
        mc_slot_current_rect(mc, i, ep, NULL, &rx, &ry, &rw, &rh, NULL);
        if (rx + rw < 0 || rx > mc->screen_width) continue;
        double btn_x = rx + 16.0;
        double btn_y = ry + 16.0;
        if (hypot(x - btn_x, y - btn_y) <= 16.0) {
            int local_rx = 0, ry_pos = 0;
            double cur_scale = 1.0;
            mc_slot_current_rect(mc, i, ep, &local_rx, NULL, &ry_pos, NULL, NULL, &cur_scale);
            mc->slots[i].close_start_x = (double)local_rx;
            mc->slots[i].close_start_y = (double)ry_pos;
            mc->slots[i].close_start_scale = cur_scale;
            mc->slots[i].is_closing = true;
            mc->slots[i].close_progress = 0.0;
            mc->hovered_view = NULL;
            mc->hovered_close_view = NULL;

            mc_start_relayout_animation(mc, mc->slots[i].workspace);
            mission_control_render(mc);
            schedule_all_outputs(mc->server);
            return true;
        }
    }

    // 2. Spaces bar click (desktop close button, plus button, or desktop thumbnail switch)
    if (mc->ws_close_candidate >= 0) {
        int close_idx = mc->ws_close_candidate;
        mc->ws_close_candidate = -1;
        mc->ws_drag_candidate = -1;
        mc->plus_candidate = false;
        int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
        mc_bar_geom_t bar;
        mc_compute_bar_geom(mc, &bar, 1.0);
        double sx = bar.pad_x + close_idx * (bar.thumb_w + bar.gap);
        double sy = bar.bar_y + bar.pad_top;
        if (hypot(x - (sx + 12.0), y - (sy + 12.0)) <= 16.0 && num_ws > 1 && !mc->ws_bar_anim.active) {
            mc->ws_bar_anim.active = true;
            mc->ws_bar_anim.type = WS_BAR_ANIM_DELETE;
            mc->ws_bar_anim.index = close_idx;
            mc->ws_bar_anim.progress = 0.0;
            mc->hovered_workspace = -1;
            mc->hovered_ws_close = -1;
            mc->spaces_bar_dirty = true;
            mission_control_render(mc);
            schedule_all_outputs(mc->server);
            return true;
        }
    }

    if (mc->plus_candidate) {
        mc->plus_candidate = false;
        mc->ws_drag_candidate = -1;
        mc->ws_close_candidate = -1;
        mc_bar_geom_t bar;
        mc_compute_bar_geom(mc, &bar, 1.0);
        int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
        if (hypot(x - bar.plus_cx, y - (bar.bar_y + bar.plus_cy)) <= bar.plus_r + 6.0 && !mc->ws_bar_anim.active) {
            if (num_ws < MAX_WORKSPACES) {
                int new_idx = num_ws;
                server_set_num_workspaces(mc->server, num_ws + 1);
                mc->ws_bar_anim.active = true;
                mc->ws_bar_anim.type = WS_BAR_ANIM_ADD;
                mc->ws_bar_anim.index = new_idx;
                mc->ws_bar_anim.progress = 0.0;
                mc->spaces_bar_dirty = true;
                mc_collect_workspace_slots(mc);
                mc_apply_slots_scene_state(mc, ep);
                mission_control_render(mc);
                schedule_all_outputs(mc->server);
            }
            return true;
        }
    }

    if (mc->ws_drag_candidate >= 0) {
        int clicked_ws = mc->ws_drag_candidate;
        mc->ws_drag_candidate = -1;
        mc->ws_close_candidate = -1;
        mc->plus_candidate = false;
        if (mc->ws_bar_anim.active) return true;
        int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
        if (clicked_ws >= 0 && clicked_ws < num_ws) {
            if (clicked_ws != mc->server->current_workspace) {
                mission_control_switch_workspace(mc, clicked_ws);
            } else {
                server_close_overlay(mc->server);
            }
            return true;
        }
    }

    // 3. Window thumbnail click (including icon and label) -> focus & exit Mission Control
    for (int i = 0; i < mc->slot_count; i++) {
        if (mc->slots[i].is_closing) continue;
        if (mc->drop_anim.active && mc->drop_anim.view == mc->slots[i].view) continue;
        int rx = 0, ry = 0, rw = 0, rh = 0;
        mc_slot_current_rect(mc, i, ep, NULL, &rx, &ry, &rw, &rh, NULL);
        if (rx + rw < 0 || rx > mc->screen_width) continue;
        if (x >= rx && x <= rx + rw && y >= ry && y <= ry + rh + 42.0) {
            aurade_view_t *clicked = mc->slots[i].view;
            if (clicked) {
                if (clicked->workspace != mc->server->current_workspace) {
                    server_switch_workspace(mc->server, clicked->workspace, false);
                }
                view_focus(clicked);
            }
            server_close_overlay(mc->server);
            return true;
        }
    }

    // 4. Empty background click -> close Mission Control
    server_close_overlay(mc->server);
    return true;
}

bool mission_control_handle_click(aurade_mission_control_t *mc, double x, double y) {
    return mission_control_handle_release(mc, x, y);
}

void mission_control_switch_workspace(aurade_mission_control_t *mc, int new_workspace) {
    if (!mc || !mc->active || mc->progress < 0.4) return;
    int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
    if (new_workspace < 0 || new_workspace >= num_ws) return;
    if (new_workspace == mc->server->current_workspace && fabs(mc->ws_slide_offset) < 0.5) return;

    int cur_ws = mc->server->current_workspace;
    int screen_w = mc->screen_width > 0 ? mc->screen_width : 1920;
    double start_offset = mc->ws_slide_offset + (double)(new_workspace - cur_ws) * (double)screen_w;

    if (mc->is_dragging && mc->dragged_view) {
        wlr_scene_node_reparent(&mc->dragged_view->scene_tree->node,
                                mc->server->workspace_trees[mc->dragged_view->workspace]);
        mc->is_dragging = false;
        mc->dragged_view = NULL;
    }

    mc->server->current_workspace = new_workspace;
    mc->hovered_view = NULL;
    mc->hovered_close_view = NULL;
    mc->hovered_workspace = -1;
    mc->mouse_pressed = false;
    mc->is_dragging = false;
    mc->dragged_view = NULL;
    mc->drag_target_workspace = -1;
    mc->spaces_bar_dirty = true;
    mc->ws_gesture_active = false;

    spring_config_t spring = {
        .stiffness = 380.0,
        .damping = 1.05,
        .mass = 1.0,
        .velocity_threshold = 0.5,
        .position_threshold = 0.2,
    };
    if (mc->server && mc->server->config.spring.stiffness > 10.0) {
        spring = mc->server->config.spring;
        spring.position_threshold = 0.5;
        spring.velocity_threshold = 1.0;
    }

    spring_simulation_start(&mc->ws_spring, &spring, start_offset, 0.0, 0.0);
    mc->ws_slide_offset = start_offset;
    mc->ws_animating = true;

    double ep = mc_ease_out_cubic(mc->progress);
    mc_apply_slots_scene_state(mc, ep);
    mission_control_render(mc);
    schedule_all_outputs(mc->server);
}

void mission_control_ws_gesture_begin(aurade_mission_control_t *mc) {
    if (!mc || !mc->active || !mc->server) return;

    int screen_w = mc->screen_width > 0 ? mc->screen_width : 1920;
    int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;

    if (mc->ws_animating) {
        mc->ws_animating = false;
        double p = mc->server->current_workspace - mc->ws_slide_offset / (double)screen_w;
        int anchor = (int)round(p);
        if (anchor < 0) anchor = 0;
        if (anchor >= num_ws) anchor = num_ws - 1;
        if (anchor != mc->server->current_workspace) {
            mc->ws_slide_offset += (double)(anchor - mc->server->current_workspace) * (double)screen_w;
            mc->server->current_workspace = anchor;
            mc->spaces_bar_dirty = true;
        }
    }

    mc->ws_gesture_active = true;
    mc->ws_gesture_base_offset = mc->ws_slide_offset;
}

void mission_control_ws_gesture_update(aurade_mission_control_t *mc, double delta_x) {
    if (!mc || !mc->active || !mc->ws_gesture_active || !mc->server) return;

    int screen_w = mc->screen_width > 0 ? mc->screen_width : 1920;
    int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
    double sens = mc->server->config.sensitivity > 0.0 ? mc->server->config.sensitivity : 1.0;
    double eff_sens = fmax(sens, 0.85);
    double gain = 2.6;
    double ref_to_screen = (double)screen_w / 1920.0;

    double offset = mc->ws_gesture_base_offset + delta_x * eff_sens * gain * ref_to_screen;

    double max_offset = (double)mc->server->current_workspace * (double)screen_w;
    double min_offset = (double)(mc->server->current_workspace - (num_ws - 1)) * (double)screen_w;
    double limit = mc->server->config.rubber_band_limit > 10.0 ? mc->server->config.rubber_band_limit : 300.0;

    if (offset > max_offset) {
        offset = max_offset + rubber_band_calculate(offset - max_offset, limit);
    } else if (offset < min_offset) {
        offset = min_offset + rubber_band_calculate(offset - min_offset, limit);
    }

    mc->ws_slide_offset = offset;
    double ep = mc_ease_out_cubic(mc->progress);
    mc_apply_slots_scene_state(mc, ep);
    mc->needs_render = true;
    schedule_all_outputs(mc->server);
}

void mission_control_ws_gesture_end(aurade_mission_control_t *mc, bool cancelled, double velocity_x) {
    if (!mc || !mc->active || !mc->server) return;
    mc->ws_gesture_active = false;

    int screen_w = mc->screen_width > 0 ? mc->screen_width : 1920;
    int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
    double sens = mc->server->config.sensitivity > 0.0 ? mc->server->config.sensitivity : 1.0;
    double eff_sens = fmax(sens, 0.85);
    double gain = 2.6;
    double ref_to_screen = (double)screen_w / 1920.0;

    double offset = mc->ws_slide_offset;
    double p = mc->server->current_workspace - offset / (double)screen_w;
    int lower = (int)floor(p);
    int upper = lower + 1;

    double ws_flick_px = 420.0;
    double ws_snap = fmin(mc->server->config.snap_threshold > 0.0 ? mc->server->config.snap_threshold : 0.16, 0.18);

    int target_workspace = mc->server->current_workspace;
    if (cancelled) {
        // Spring back to anchored workspace
    } else if (velocity_x < -ws_flick_px) {
        target_workspace = upper;
    } else if (velocity_x > ws_flick_px) {
        target_workspace = lower;
    } else if (p >= mc->server->current_workspace) {
        if (p - mc->server->current_workspace >= ws_snap) {
            target_workspace = mc->server->current_workspace + 1;
        }
    } else {
        if (mc->server->current_workspace - p >= ws_snap) {
            target_workspace = mc->server->current_workspace - 1;
        }
    }

    if (target_workspace > mc->server->current_workspace + 1) {
        target_workspace = mc->server->current_workspace + 1;
    }
    if (target_workspace < mc->server->current_workspace - 1) {
        target_workspace = mc->server->current_workspace - 1;
    }
    if (target_workspace < 0) target_workspace = 0;
    if (target_workspace >= num_ws) target_workspace = num_ws - 1;

    int diff = target_workspace - mc->server->current_workspace;
    double new_offset = offset + (double)diff * (double)screen_w;
    mc->server->current_workspace = target_workspace;
    mc->ws_slide_offset = new_offset;
    mc->spaces_bar_dirty = true;

    double offset_velocity = velocity_x * eff_sens * gain * ref_to_screen;

    spring_config_t spring = {
        .stiffness = 380.0,
        .damping = 1.05,
        .mass = 1.0,
        .velocity_threshold = 1.0,
        .position_threshold = 0.5,
    };
    if (mc->server->config.spring.stiffness > 10.0) {
        spring = mc->server->config.spring;
        spring.position_threshold = 0.5;
        spring.velocity_threshold = 1.0;
    }

    spring_simulation_start(&mc->ws_spring, &spring, new_offset, 0.0, offset_velocity);
    mc->ws_animating = true;

    double ep = mc_ease_out_cubic(mc->progress);
    mc_apply_slots_scene_state(mc, ep);
    mission_control_render(mc);
    schedule_all_outputs(mc->server);
}

void mission_control_tick(aurade_mission_control_t *mc, double dt) {
    if (!mc || !mc->active) return;
    double ep = mc_ease_out_cubic(mc->progress);

    if (mc->ws_animating) {
        bool active = spring_simulation_step(&mc->ws_spring, dt);
        mc->ws_slide_offset = mc->ws_spring.current_pos;
        if (!active) {
            mc->ws_slide_offset = 0.0;
            mc->ws_animating = false;
        }
    }

    if (mc->ws_bar_anim.active) {
        mc->ws_bar_anim.progress += dt / 0.26;
        mc->spaces_bar_dirty = true;
        if (mc->ws_bar_anim.progress >= 1.0) {
            if (mc->ws_bar_anim.type == WS_BAR_ANIM_DELETE) {
                int del_idx = mc->ws_bar_anim.index;
                mc->ws_bar_anim.active = false;
                mc->ws_bar_anim.type = WS_BAR_ANIM_NONE;
                server_delete_workspace(mc->server, del_idx);
                mc_collect_workspace_slots(mc);
            } else {
                mc->ws_bar_anim.active = false;
                mc->ws_bar_anim.type = WS_BAR_ANIM_NONE;
            }
        }
    }

    if (mc->drop_anim.active) {
        mc->drop_anim.progress += dt / 0.26;
        double t = mc_ease_out_cubic(fmin(1.0, mc->drop_anim.progress));
        aurade_view_t *dv = mc->drop_anim.view;
        if (dv && dv->scene_tree) {
            double cur_x = mc_lerp(mc->drop_anim.start_x, mc->drop_anim.target_x, t);
            double cur_y = mc_lerp(mc->drop_anim.start_y, mc->drop_anim.target_y, t);
            double cur_scale = mc_lerp(mc->drop_anim.start_scale, mc->drop_anim.target_scale, t);
            wlr_scene_node_set_position(&dv->scene_tree->node, (int)round(cur_x), (int)round(cur_y));
            view_set_scale(dv, cur_scale, cur_scale);
            if (!mc->drop_anim.is_return) {
                view_set_opacity(dv, (float)fmax(0.0, 1.0 - t));
            } else {
                view_set_opacity(dv, (float)mc_lerp(0.92, 1.0, t));
            }
        }
        if (mc->drop_anim.progress >= 1.0) {
            int to_ws = mc->drop_anim.to_workspace;
            bool was_return = mc->drop_anim.is_return;
            mc->drop_anim.active = false;
            mc->drop_anim.view = NULL;

            if (dv && dv->scene_tree) {
                wlr_scene_node_reparent(&dv->scene_tree->node,
                                        mc->server->workspace_trees[to_ws]);
                view_set_opacity(dv, 1.0f);
                if (!was_return) {
                    view_set_scale(dv, 1.0, 1.0);
                    wlr_scene_node_set_position(&dv->scene_tree->node, dv->x, dv->y);
                } else {
                    int slot_idx = -1;
                    for (int s = 0; s < mc->slot_count; s++) {
                        if (mc->slots[s].view == dv) {
                            slot_idx = s;
                            break;
                        }
                    }
                    int local_rx = (int)round(dv->x);
                    int local_ry = (int)round(dv->y);
                    double final_scale = mc->drop_anim.target_scale;
                    if (slot_idx >= 0) {
                        mc_slot_current_rect(mc, slot_idx, ep, &local_rx, NULL, &local_ry, NULL, NULL, &final_scale);
                    }
                    wlr_scene_node_set_position(&dv->scene_tree->node, local_rx, local_ry);
                    view_set_scale(dv, final_scale, final_scale);
                }
            }

            if (!was_return) {
                if (mc->server->config.tiling_enabled) {
                    workspace_relayout_tiling(mc->server, to_ws);
                }
                mc_collect_workspace_slots(mc);
                mc->spaces_bar_dirty = true;
                mc_apply_slots_scene_state(mc, ep);
            }
        }
    }

    for (int i = 0; i < mc->slot_count; i++) {
        mc_window_slot_t *s = &mc->slots[i];
        if (s->is_closing) {
            s->close_progress += dt / 0.26;
            if (s->close_progress >= 1.0) {
                aurade_view_t *v = s->view;
                s->is_closing = false;
                view_set_opacity(v, 1.0f);
                view_close(v);
                mission_control_forget_view(mc, v);
                i--;
                continue;
            }
        }
        if (s->is_relayout) {
            s->relayout_progress += dt / 0.26;
            if (s->relayout_progress >= 1.0) {
                s->is_relayout = false;
                s->relayout_progress = 1.0;
            }
        }
    }

    mc_apply_slots_scene_state(mc, ep);
    mc->needs_render = true;
    schedule_all_outputs(mc->server);
}

void mission_control_handle_axis(aurade_mission_control_t *mc, enum wl_pointer_axis orientation,
                                 double delta, int32_t delta_discrete) {
    // Workspace switching only responds to 3-finger horizontal touchpad gestures,
    // never mouse wheel up/down or 2-finger touchpad scrolling.
    (void)mc;
    (void)orientation;
    (void)delta;
    (void)delta_discrete;
}



static void mc_draw_workspace_mini_windows(cairo_t *cr, aurade_mission_control_t *mc,
                                           int ws, double sx, double sy,
                                           double thumb_w, double thumb_h,
                                           bool is_dark, double alpha) {
    if (!mc || !mc->server || mc->screen_width <= 0 || mc->screen_height <= 0) return;

    aurade_view_t *ws_views[MC_MAX_SLOTS];
    int ws_count = 0;
    aurade_view_t *view;
    wl_list_for_each(view, &mc->server->views, link) {
        if (view->workspace != ws || view->minimized) continue;
        int vw = 0, vh = 0;
        mc_query_view_geometry(view, &vw, &vh);
        if (vw < 40 || vh < 40) continue;
        if (ws_count < MC_MAX_SLOTS) {
            ws_views[ws_count++] = view;
        }
    }
    if (ws_count == 0) return;

    double scale_x = thumb_w / (double)mc->screen_width;
    double scale_y = thumb_h / (double)mc->screen_height;

    for (int i = ws_count - 1; i >= 0; i--) {
        aurade_view_t *v = ws_views[i];
        int vw = 0, vh = 0;
        mc_query_view_geometry(v, &vw, &vh);
        double vx = (double)v->x;
        double vy = (double)v->y;

        if (ws == mc->server->current_workspace) {
            for (int k = 0; k < mc->slot_count; k++) {
                if (mc->slots[k].view == v) {
                    vx = mc->slots[k].orig_x;
                    vy = mc->slots[k].orig_y;
                    vw = (int)mc->slots[k].orig_w;
                    vh = (int)mc->slots[k].orig_h;
                    break;
                }
            }
        }

        double wx = sx + vx * scale_x;
        double wy = sy + vy * scale_y;
        double ww = vw * scale_x;
        double wh = vh * scale_y;
        if (ww < 14.0) ww = 14.0;
        if (wh < 10.0) wh = 10.0;

        double r = fmin(3.5, fmin(ww, wh) * 0.14);
        if (r < 2.0) r = 2.0;

        draw_rounded_rect(cr, wx, wy + 1.2, ww, wh, r);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45 * alpha);
        cairo_fill(cr);

        cairo_pattern_t *pat = cairo_pattern_create_linear(wx, wy, wx, wy + wh);
        if (is_dark) {
            cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.18, 0.20, 0.24, 0.95 * alpha);
            cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.11, 0.12, 0.15, 0.98 * alpha);
        } else {
            cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.96, 0.96, 0.98, 0.95 * alpha);
            cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.88, 0.89, 0.92, 0.98 * alpha);
        }
        cairo_set_source(cr, pat);
        draw_rounded_rect(cr, wx, wy, ww, wh, r);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);

        double tb_h = fmax(3.5, fmin(7.5, wh * 0.22));
        cairo_save(cr);
        draw_rounded_rect(cr, wx, wy, ww, wh, r);
        cairo_clip(cr);
        cairo_rectangle(cr, wx, wy, ww, tb_h);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (is_dark ? 0.07 : 0.05) * alpha);
        cairo_fill(cr);
        cairo_restore(cr);

        if (ww >= 22.0 && wh >= 14.0) {
            double dot_r = fmax(0.7, fmin(1.2, tb_h * 0.20));
            double dot_y = wy + tb_h / 2.0;
            double dot_x = wx + 3.0;
            double dot_spacing = dot_r * 2.0 + 1.8;

            cairo_arc(cr, dot_x, dot_y, dot_r, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 0.36, 0.33, 0.90 * alpha);
            cairo_fill(cr);

            cairo_arc(cr, dot_x + dot_spacing, dot_y, dot_r, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 0.74, 0.18, 0.90 * alpha);
            cairo_fill(cr);

            cairo_arc(cr, dot_x + dot_spacing * 2.0, dot_y, dot_r, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0.16, 0.80, 0.25, 0.90 * alpha);
            cairo_fill(cr);
        }

        const char *app_id = mc_view_app_id(v);
        desktop_app_t *app = apps_find(mc->server->apps, app_id);
        double icon_sz = fmax(8.0, fmin(18.0, fmin(ww * 0.50, (wh - tb_h) * 0.70)));
        if (icon_sz >= 8.0) {
            mc_draw_app_icon(cr, wx + ww / 2.0, wy + tb_h + (wh - tb_h) / 2.0, icon_sz,
                             app ? app->name : (app_id ? app_id : "App"),
                             app ? app->icon_surface : NULL, alpha);
        }

        draw_rounded_rect(cr, wx + 0.5, wy + 0.5, ww - 1.0, wh - 1.0, r);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (is_dark ? 0.20 : 0.28) * alpha);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);
    }
}

static void mc_render_spaces_bar_surface(aurade_mission_control_t *mc) {
    if (!mc) return;
    mc_bar_geom_t bar;
    mc_compute_bar_geom(mc, &bar, 1.0);

    int surf_w = (int)ceil(bar.bar_w);
    int surf_h = (int)ceil(bar.bar_h);

    if (!mc->spaces_bar_surf ||
        cairo_image_surface_get_width(mc->spaces_bar_surf) != surf_w ||
        cairo_image_surface_get_height(mc->spaces_bar_surf) != surf_h) {
        if (mc->spaces_bar_surf) cairo_surface_destroy(mc->spaces_bar_surf);
        mc->spaces_bar_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, surf_w, surf_h);
    }

    cairo_t *cr = cairo_create(mc->spaces_bar_surf);
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    // Full-width dark translucent band across top of screen
    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, surf_h);
    cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.10, 0.11, 0.15, 0.88);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.07, 0.08, 0.11, 0.94);
    cairo_rectangle(cr, 0, 0, surf_w, surf_h);
    cairo_set_source(cr, bg);
    cairo_fill(cr);
    cairo_pattern_destroy(bg);

    // Subtle bottom border separating spaces bar from window area
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, surf_h - 0.5);
    cairo_line_to(cr, surf_w, surf_h - 0.5);
    cairo_stroke(cr);

    int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
    for (int i = 0; i < num_ws; i++) {
        double sx = bar.pad_x + i * (bar.thumb_w + bar.gap);
        double sy = bar.pad_top;
        double card_scale = 1.0;
        double card_alpha = 1.0;

        if (mc->ws_bar_anim.active) {
            double t = mc_ease_out_cubic(fmin(1.0, mc->ws_bar_anim.progress));
            if (mc->ws_bar_anim.type == WS_BAR_ANIM_ADD) {
                if (num_ws > 1) {
                    double prev_total = bar.thumb_w * (num_ws - 1) + bar.gap * (num_ws - 2);
                    double prev_pad_x = ((double)mc->screen_width - prev_total) / 2.0;
                    if (i == mc->ws_bar_anim.index) {
                        card_scale = mc_lerp(0.3, 1.0, t);
                        card_alpha = t;
                    } else {
                        double prev_x = prev_pad_x + i * (bar.thumb_w + bar.gap);
                        double target_x = sx;
                        sx = mc_lerp(prev_x, target_x, t);
                    }
                }
            } else if (mc->ws_bar_anim.type == WS_BAR_ANIM_DELETE) {
                if (num_ws > 1) {
                    double next_total = bar.thumb_w * (num_ws - 1) + bar.gap * (num_ws - 2);
                    double next_pad_x = ((double)mc->screen_width - next_total) / 2.0;
                    if (i == mc->ws_bar_anim.index) {
                        card_scale = mc_lerp(1.0, 0.3, t);
                        card_alpha = fmax(0.0, 1.0 - t);
                    } else if (i < mc->ws_bar_anim.index) {
                        double orig_x = sx;
                        double target_x = next_pad_x + i * (bar.thumb_w + bar.gap);
                        sx = mc_lerp(orig_x, target_x, t);
                    } else {
                        double orig_x = sx;
                        double target_x = next_pad_x + (i - 1) * (bar.thumb_w + bar.gap);
                        sx = mc_lerp(orig_x, target_x, t);
                    }
                }
            }
        }

        if (card_alpha <= 0.001) continue;

        bool is_active = (i == mc->server->current_workspace);
        bool is_drag_target = (mc->is_dragging && mc->drag_target_workspace == i);
        bool is_ws_hover = (mc->hovered_workspace == i && !is_drag_target);
        bool is_reorder_source = (mc->is_dragging_ws && mc->drag_ws == i);
        bool is_reorder_target = (mc->is_dragging_ws && mc->ws_drag_target == i);

        if (is_reorder_source) {
            // Draw placeholder ghost slot where the desktop was dragged from
            draw_rounded_rect(cr, sx, sy, bar.thumb_w, bar.thumb_h, 5.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.06);
            cairo_fill_preserve(cr);
            static const double dashes[] = {4.0, 4.0};
            cairo_set_dash(cr, dashes, 2, 0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.28);
            cairo_set_line_width(cr, 1.2);
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);

            char label[32];
            snprintf(label, sizeof(label), "Desktop %d", i + 1);
            double label_cx = sx + bar.thumb_w / 2.0;
            double label_cy = sy + bar.thumb_h + bar.label_gap + bar.label_h / 2.0;
            draw_text(cr, label, MC_FONT, 11.5, false, 0.6, 0.62, 0.68, 0.40,
                      label_cx, label_cy, true, true);
            continue;
        }

        cairo_save(cr);
        if (fabs(card_scale - 1.0) > 0.001) {
            double cx = sx + bar.thumb_w / 2.0;
            double cy = sy + bar.thumb_h / 2.0;
            cairo_translate(cr, cx, cy);
            cairo_scale(cr, card_scale, card_scale);
            cairo_translate(cr, -cx, -cy);
        }
        if (card_alpha < 0.999) {
            cairo_push_group(cr);
        }

        // Thumbnail shadow
        draw_rounded_rect(cr, sx, sy + 1.0, bar.thumb_w, bar.thumb_h, 5.0);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
        cairo_fill(cr);

        // Thumbnail preview: wallpaper + mini windows
        cairo_save(cr);
        draw_rounded_rect(cr, sx, sy, bar.thumb_w, bar.thumb_h, 5.0);
        cairo_clip(cr);

        mc_paint_wallpaper_thumb(cr, mc, sx, sy, bar.thumb_w, bar.thumb_h, 0.95);

        if (!is_active && !is_drag_target && !is_reorder_target) {
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, is_ws_hover ? 0.08 : 0.22);
            cairo_paint(cr);
        }

        mc_draw_workspace_mini_windows(cr, mc, i, sx, sy, bar.thumb_w, bar.thumb_h, true, 1.0);
        cairo_restore(cr);

        // Thumbnail border
        if (is_reorder_target) {
            // Bright drop target border
            cairo_set_source_rgba(cr, 0.18, 0.78, 1.0, 1.0);
            cairo_set_line_width(cr, 3.0);
            draw_rounded_rect(cr, sx - 1.0, sy - 1.0, bar.thumb_w + 2.0, bar.thumb_h + 2.0, 6.0);
            cairo_stroke(cr);
        } else if (is_drag_target) {
            cairo_set_source_rgba(cr, 0.18, 0.78, 1.0, 1.0);
            cairo_set_line_width(cr, 3.0);
            draw_rounded_rect(cr, sx - 1.0, sy - 1.0, bar.thumb_w + 2.0, bar.thumb_h + 2.0, 6.0);
            cairo_stroke(cr);
        } else if (is_active) {
            // Bright blue highlight on active desktop
            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 1.0);
            cairo_set_line_width(cr, 2.5);
            draw_rounded_rect(cr, sx - 1.0, sy - 1.0, bar.thumb_w + 2.0, bar.thumb_h + 2.0, 6.0);
            cairo_stroke(cr);
        } else if (is_ws_hover) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.50);
            cairo_set_line_width(cr, 1.5);
            draw_rounded_rect(cr, sx - 0.5, sy - 0.5, bar.thumb_w + 1.0, bar.thumb_h + 1.0, 5.5);
            cairo_stroke(cr);
        } else {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
            cairo_set_line_width(cr, 1.0);
            draw_rounded_rect(cr, sx + 0.5, sy + 0.5, bar.thumb_w - 1.0, bar.thumb_h - 1.0, 5.0);
            cairo_stroke(cr);
        }

        // Desktop close button '✕' (only if num_ws > 1, not dragging, and hovering this workspace)
        if (num_ws > 1 && !mc->is_dragging_ws && (is_ws_hover || mc->hovered_ws_close == i)) {
            double btn_x = sx + 12.0;
            double btn_y = sy + 12.0;
            double btn_r = 9.0;
            bool is_close_hover = (mc->hovered_ws_close == i);

            cairo_save(cr);
            // Shadow
            cairo_new_path(cr);
            cairo_arc(cr, btn_x, btn_y + 1.0, btn_r, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.40);
            cairo_fill(cr);

            // Circle background
            cairo_new_path(cr);
            cairo_arc(cr, btn_x, btn_y, btn_r, 0, 2 * M_PI);
            if (is_close_hover) {
                cairo_set_source_rgba(cr, 0.95, 0.24, 0.20, 0.98);
            } else {
                cairo_set_source_rgba(cr, 0.12, 0.14, 0.18, 0.88);
            }
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, is_close_hover ? 0.60 : 0.35);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);

            // Cross
            double carm = 3.2;
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            cairo_set_line_width(cr, 1.5);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_new_path(cr);
            cairo_move_to(cr, btn_x - carm, btn_y - carm);
            cairo_line_to(cr, btn_x + carm, btn_y + carm);
            cairo_move_to(cr, btn_x + carm, btn_y - carm);
            cairo_line_to(cr, btn_x - carm, btn_y + carm);
            cairo_stroke(cr);
            cairo_restore(cr);
        }

        // Clean text label underneath: "Desktop %d"
        char label[32];
        if (is_drag_target) {
            snprintf(label, sizeof(label), "Drop to Move");
        } else {
            snprintf(label, sizeof(label), "Desktop %d", i + 1);
        }

        double label_cx = sx + bar.thumb_w / 2.0;
        double label_cy = sy + bar.thumb_h + bar.label_gap + bar.label_h / 2.0;

        // Shadow behind label for contrast
        draw_text(cr, label, MC_FONT, 11.5, is_active, 0.0, 0.0, 0.0, 0.70,
                  label_cx, label_cy + 1.0, true, true);
        if (is_active) {
            draw_text(cr, label, MC_FONT, 11.5, true, 1.0, 1.0, 1.0, 0.98,
                      label_cx, label_cy, true, true);
        } else if (is_ws_hover) {
            draw_text(cr, label, MC_FONT, 11.5, false, 0.95, 0.96, 1.0, 0.95,
                      label_cx, label_cy, true, true);
        } else {
            draw_text(cr, label, MC_FONT, 11.5, false, 0.82, 0.84, 0.88, 0.75,
                      label_cx, label_cy, true, true);
        }

        if (card_alpha < 0.999) {
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, card_alpha);
        }
        cairo_restore(cr);
    }

    // Floating dragged desktop thumbnail card
    if (mc->is_dragging_ws && mc->drag_ws >= 0 && mc->drag_ws < num_ws) {
        double drag_dx = mc->ws_drag_current_x - mc->ws_drag_start_x;
        double orig_sx = bar.pad_x + mc->drag_ws * (bar.thumb_w + bar.gap);
        double drag_sx = orig_sx + drag_dx;
        double min_sx = bar.pad_x - bar.thumb_w * 0.4;
        double max_sx = bar.pad_x + (num_ws - 1) * (bar.thumb_w + bar.gap) + bar.thumb_w * 0.4;
        if (drag_sx < min_sx) drag_sx = min_sx;
        if (drag_sx > max_sx) drag_sx = max_sx;
        double drag_sy = bar.pad_top - 4.0;

        // Elevated drop shadow
        draw_rounded_rect(cr, drag_sx, drag_sy + 4.0, bar.thumb_w, bar.thumb_h, 6.0);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
        cairo_fill(cr);

        // Thumbnail preview
        cairo_save(cr);
        draw_rounded_rect(cr, drag_sx, drag_sy, bar.thumb_w, bar.thumb_h, 5.0);
        cairo_clip(cr);
        mc_paint_wallpaper_thumb(cr, mc, drag_sx, drag_sy, bar.thumb_w, bar.thumb_h, 1.0);
        mc_draw_workspace_mini_windows(cr, mc, mc->drag_ws, drag_sx, drag_sy, bar.thumb_w, bar.thumb_h, true, 1.0);
        cairo_restore(cr);

        // Bright border on dragged card
        cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 1.0);
        cairo_set_line_width(cr, 2.5);
        draw_rounded_rect(cr, drag_sx - 1.0, drag_sy - 1.0, bar.thumb_w + 2.0, bar.thumb_h + 2.0, 6.0);
        cairo_stroke(cr);

        // Dragged label
        char drag_label[32];
        snprintf(drag_label, sizeof(drag_label), "Desktop %d", mc->drag_ws + 1);
        double drag_label_cx = drag_sx + bar.thumb_w / 2.0;
        double drag_label_cy = drag_sy + bar.thumb_h + bar.label_gap + bar.label_h / 2.0;
        draw_text(cr, drag_label, MC_FONT, 11.5, true, 0.0, 0.0, 0.0, 0.70,
                  drag_label_cx, drag_label_cy + 1.0, true, true);
        draw_text(cr, drag_label, MC_FONT, 11.5, true, 1.0, 1.0, 1.0, 0.98,
                  drag_label_cx, drag_label_cy, true, true);
    }

    // Plus (+) button on the right edge
    double arm = 7.0;
    double plus_alpha = mc->hovered_plus ? 0.95 : 0.55;
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, plus_alpha);
    cairo_set_line_width(cr, 2.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_new_path(cr);
    cairo_move_to(cr, bar.plus_cx - arm, bar.plus_cy);
    cairo_line_to(cr, bar.plus_cx + arm, bar.plus_cy);
    cairo_move_to(cr, bar.plus_cx, bar.plus_cy - arm);
    cairo_line_to(cr, bar.plus_cx, bar.plus_cy + arm);
    cairo_stroke(cr);

    cairo_destroy(cr);
    mc->spaces_bar_dirty = false;
}

void mission_control_render(aurade_mission_control_t *mc) {
    if (!mc || !mc->buffer || !mc->buffer->cr) return;
    mc->needs_render = false;
    cairo_t *cr = mc->buffer->cr;
    cairo_buffer_clear(mc->buffer);

    double p = mc->progress;
    if (p <= 0.0) {
        cairo_buffer_apply_to_scene(mc->buffer, mc->scene_buffer);
        return;
    }
    if (p > 1.0) p = 1.0;

    mc_theme_t theme = mc_get_theme(mc);

    if (!mc->server->config.blur_workspace_overview) {
        cairo_set_source_rgba(cr, 0.03, 0.04, 0.07, 0.70 * p);
        cairo_paint(cr);
    }

    double ep = mc_ease_out_cubic(p);

    // Drag placeholder slot
    if (mc->is_dragging && mc->dragged_view) {
        for (int i = 0; i < mc->slot_count; i++) {
            if (mc->slots[i].view == mc->dragged_view) {
                int rx = 0, ry = 0, rw = 0, rh = 0;
                double sc = 1.0;
                mc_slot_current_rect(mc, i, ep, NULL, &rx, &ry, &rw, &rh, &sc);
                double rad = fmax(4.0, round(8.0 * sc));
                cairo_save(cr);
                draw_rounded_rect(cr, rx, ry, rw, rh, rad);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.05 * p);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.24 * p);
                double dashes[] = { 6.0, 4.0 };
                cairo_set_dash(cr, dashes, 2, 0.0);
                cairo_set_line_width(cr, 1.5);
                cairo_stroke(cr);
                cairo_restore(cr);
                break;
            }
        }
    }

    // Spaces bar at top
    mc_bar_geom_t bar;
    mc_compute_bar_geom(mc, &bar, p);
    if (mc->spaces_bar_dirty || !mc->spaces_bar_surf) {
        mc_render_spaces_bar_surface(mc);
    }
    if (mc->spaces_bar_surf) {
        cairo_save(cr);
        cairo_set_source_surface(cr, mc->spaces_bar_surf, 0.0, bar.bar_y);
        cairo_paint_with_alpha(cr, p);
        cairo_restore(cr);
    }

    // Window frames, close buttons, app icons, and title labels
    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *view = mc->slots[i].view;
        if (!view) continue;
        if (mc->is_dragging && mc->dragged_view == view) continue;
        if (mc->drop_anim.active && mc->drop_anim.view == view) continue;

        int rx = 0, ry = 0, rw = 0, rh = 0;
        mc_slot_current_rect(mc, i, ep, NULL, &rx, &ry, &rw, &rh, NULL);
        if (rx + rw < -40 || rx > mc->screen_width + 40) continue;

        double cx = (double)rx;
        double cy = (double)ry;
        double cw = (double)rw;
        double ch = (double)rh;

        double slot_alpha = p;
        if (mc->slots[i].is_closing) {
            double ct = mc_ease_out_cubic(fmin(1.0, mc->slots[i].close_progress));
            slot_alpha = p * fmax(0.0, 1.0 - ct);
        }
        if (slot_alpha <= 0.001) continue;

        bool is_hover = (mc->hovered_view == view && !mc->slots[i].is_closing);
        bool is_focused = (mc->hovered_view == NULL && mc->server && mc->server->focused_view == view && !mc->slots[i].is_closing);

        // Window border outline (blue outline if hovered/focused as in screenshot)
        if (is_hover || is_focused) {
            draw_rounded_rect(cr, cx, cy, cw, ch, 8.0);
            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, (is_hover ? 0.95 : 0.70) * slot_alpha);
            cairo_set_line_width(cr, is_hover ? 2.5 : 2.0);
            cairo_stroke(cr);
        } else {
            draw_rounded_rect(cr, cx, cy, cw, ch, 8.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16 * slot_alpha);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        }

        // Top-left circular close button (only visible when hovering this window)
        if (is_hover) {
            double btn_x = cx + 16.0;
            double btn_y = cy + 16.0;
            double btn_r = 12.0;
            bool is_btn_hover = (mc->hovered_close_view == view);

            cairo_save(cr);
            cairo_new_path(cr);
            cairo_arc(cr, btn_x, btn_y, btn_r, 0, 2 * M_PI);
            if (is_btn_hover) {
                cairo_set_source_rgba(cr, 0.95, 0.26, 0.21, 0.35 * slot_alpha);
                cairo_arc(cr, btn_x, btn_y, btn_r + 3.0, 0, 2 * M_PI);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, 0.95, 0.26, 0.21, 0.98 * slot_alpha);
            } else {
                cairo_set_source_rgba(cr, 0.16, 0.18, 0.22, 0.88 * slot_alpha);
            }
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (is_btn_hover ? 0.50 : 0.22) * slot_alpha);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);

            double arm = 3.8;
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (is_btn_hover ? 1.0 : 0.85) * slot_alpha);
            cairo_set_line_width(cr, 1.5);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_new_path(cr);
            cairo_move_to(cr, btn_x - arm, btn_y - arm);
            cairo_line_to(cr, btn_x + arm, btn_y + arm);
            cairo_move_to(cr, btn_x + arm, btn_y - arm);
            cairo_line_to(cr, btn_x - arm, btn_y + arm);
            cairo_stroke(cr);
            cairo_restore(cr);
        }

        // App icon docked at the bottom border and clean title below
        desktop_app_t *app = mc->slots[i].app;
        const char *display_title = (app && *app->name)
                                    ? app->name : mc->slots[i].display_title;
        double icon_sz = 38.0;
        double icon_cx = cx + cw / 2.0;
        double icon_cy = cy + ch; // Centered on the bottom border of the window

        // Drop shadow for icon
        draw_rounded_rect(cr, icon_cx - icon_sz / 2.0, icon_cy - icon_sz / 2.0 + 2.0,
                          icon_sz, icon_sz, icon_sz * 0.24);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.40 * slot_alpha);
        cairo_fill(cr);

        mc_draw_app_icon(cr, icon_cx, icon_cy, icon_sz,
                         app ? app->name : display_title,
                         app ? app->icon_surface : NULL, slot_alpha);

        // Clean label directly below icon (no background pill)
        double text_y = icon_cy + icon_sz / 2.0 + 12.0;
        draw_text(cr, display_title, MC_FONT, 12.0, true,
                  0.0, 0.0, 0.0, 0.70 * slot_alpha,
                  icon_cx, text_y + 1.0, true, true);
        draw_text(cr, display_title, MC_FONT, 12.0, true,
                  1.0, 1.0, 1.0, 0.98 * slot_alpha,
                  icon_cx, text_y, true, true);
    }

    int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
    for (int ws = 0; ws < num_ws; ws++) {
        bool has_slots = false;
        for (int i = 0; i < mc->slot_count; i++) {
            if (mc->slots[i].workspace == ws) {
                has_slots = true;
                break;
            }
        }
        if (has_slots) continue;

        double ws_cx = mc->screen_width / 2.0 +
                       (double)(ws - mc->server->current_workspace) * mc->screen_width +
                       mc->ws_slide_offset;
        double pill_w = 220.0;
        double pill_h = 44.0;
        double pill_x = ws_cx - pill_w / 2.0;
        double pill_y = (mc->screen_height - pill_h) / 2.0;
        if (pill_x + pill_w < -20.0 || pill_x > mc->screen_width + 20.0) continue;

        draw_drop_shadow(cr, pill_x, pill_y, pill_w, pill_h, 22.0, 14.0, 0.35 * p);
        draw_glass_panel(cr, pill_x, pill_y, pill_w, pill_h, 22.0,
                         theme.panel_top.r, theme.panel_top.g, theme.panel_top.b, 0.88 * p);
        draw_text(cr, "No Windows Open", MC_FONT, 14.0, true,
                  theme.text.r, theme.text.g, theme.text.b, 0.85 * p,
                  ws_cx, mc->screen_height / 2.0, true, true);
    }

    cairo_buffer_apply_to_scene(mc->buffer, mc->scene_buffer);
}

void mission_control_destroy(aurade_mission_control_t *mc) {
    if (!mc) return;
    if (mc->wallpaper_thumb) {
        cairo_surface_destroy(mc->wallpaper_thumb);
        mc->wallpaper_thumb = NULL;
    }
    if (mc->spaces_bar_surf) {
        cairo_surface_destroy(mc->spaces_bar_surf);
        mc->spaces_bar_surf = NULL;
    }
    if (mc->buffer) wlr_buffer_drop(&mc->buffer->base);
    free(mc);
}
