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
#define MC_RADIUS 12.0

static uint64_t mc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

typedef struct {
    double r, g, b, a;
} mc_color_t;

typedef struct {
    mc_color_t panel_top, panel_bottom, border, highlight;
    mc_color_t card, card_hover, card_pressed, card_border;
    mc_color_t text, text_dim, text_faint;
    mc_color_t accent_blue;
    bool dark;
} mc_theme_t;

static mc_theme_t mc_get_theme(aurade_mission_control_t *mc) {
    bool dark = mc && mc->server ? mc->server->config.dark_mode : true;
    mc_theme_t t = {0};
    t.dark = dark;
    t.accent_blue = (mc_color_t){0.0, 0.48, 1.0, 1.0};

    if (dark) {
        t.panel_top = (mc_color_t){0.18, 0.20, 0.24, 0.88};
        t.panel_bottom = (mc_color_t){0.11, 0.12, 0.15, 0.94};
        t.border = (mc_color_t){1.0, 1.0, 1.0, 0.15};
        t.highlight = (mc_color_t){1.0, 1.0, 1.0, 0.22};
        t.card = (mc_color_t){1.0, 1.0, 1.0, 0.085};
        t.card_hover = (mc_color_t){1.0, 1.0, 1.0, 0.14};
        t.card_pressed = (mc_color_t){1.0, 1.0, 1.0, 0.055};
        t.card_border = (mc_color_t){1.0, 1.0, 1.0, 0.07};
        t.text = (mc_color_t){1.0, 1.0, 1.0, 0.96};
        t.text_dim = (mc_color_t){0.85, 0.86, 0.90, 0.65};
        t.text_faint = (mc_color_t){0.80, 0.82, 0.88, 0.40};
    } else {
        t.panel_top = (mc_color_t){0.96, 0.96, 0.97, 0.88};
        t.panel_bottom = (mc_color_t){0.88, 0.89, 0.92, 0.94};
        t.border = (mc_color_t){1.0, 1.0, 1.0, 0.65};
        t.highlight = (mc_color_t){1.0, 1.0, 1.0, 0.90};
        t.card = (mc_color_t){1.0, 1.0, 1.0, 0.55};
        t.card_hover = (mc_color_t){1.0, 1.0, 1.0, 0.75};
        t.card_pressed = (mc_color_t){1.0, 1.0, 1.0, 0.45};
        t.card_border = (mc_color_t){0.0, 0.0, 0.0, 0.06};
        t.text = (mc_color_t){0.10, 0.10, 0.12, 0.94};
        t.text_dim = (mc_color_t){0.35, 0.36, 0.40, 0.75};
        t.text_faint = (mc_color_t){0.50, 0.52, 0.56, 0.55};
    }
    return t;
}

typedef struct {
    double bar_x, bar_y, bar_w, bar_h;
    double thumb_w, thumb_h, gap;
    double pad_x, pad_top, pad_bottom;
    double label_gap, label_h;
} mc_bar_t;

static void mc_bar_layout(aurade_mission_control_t *mc, mc_bar_t *b, double p) {
    int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
    double gap = 14.0;
    double pad_x = 18.0;
    double pad_top = 14.0;
    double pad_bottom = 14.0;
    double label_gap = 8.0;
    double label_h = 22.0;

    double aspect = (mc->screen_height > 0 && mc->screen_width > 0)
        ? ((double)mc->screen_height / (double)mc->screen_width) : 0.5625;

    double thumb_w = 152.0;
    double thumb_h = round(thumb_w * aspect);

    // Make sure all workspaces fit comfortably within screen width
    double max_avail_w = mc->screen_width - 120.0;
    if (thumb_w * num_ws + gap * (num_ws - 1) + 2.0 * pad_x > max_avail_w) {
        thumb_w = (max_avail_w - 2.0 * pad_x - gap * (num_ws - 1)) / (double)num_ws;
        if (thumb_w < 80.0) thumb_w = 80.0;
        thumb_h = round(thumb_w * aspect);
    }

    double total_thumbs = thumb_w * num_ws + gap * (num_ws - 1);
    b->thumb_w = thumb_w;
    b->thumb_h = thumb_h;
    b->gap = gap;
    b->pad_x = pad_x;
    b->pad_top = pad_top;
    b->pad_bottom = pad_bottom;
    b->label_gap = label_gap;
    b->label_h = label_h;
    b->bar_w = total_thumbs + 2.0 * pad_x;
    b->bar_h = pad_top + thumb_h + label_gap + label_h + pad_bottom;
    b->bar_x = (mc->screen_width - b->bar_w) / 2.0;
    // Slide down smoothly from above as gesture opens
    b->bar_y = -b->bar_h - 12.0 + p * (38.0 + b->bar_h + 12.0);
}

static void paint_wallpaper_thumb(cairo_t *cr, aurade_mission_control_t *mc,
                                  double x, double y, double w, double h, double alpha) {
    if (mc && mc->wallpaper_thumb) {
        int bw = cairo_image_surface_get_width(mc->wallpaper_thumb);
        int bh = cairo_image_surface_get_height(mc->wallpaper_thumb);
        if (bw > 0 && bh > 0) {
            cairo_save(cr);
            draw_rounded_rect(cr, x, y, w, h, 11.0);
            cairo_clip(cr);
            cairo_translate(cr, x, y);
            cairo_scale(cr, w / (double)bw, h / (double)bh);
            cairo_set_source_surface(cr, mc->wallpaper_thumb, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
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


static void draw_mc_app_icon(cairo_t *cr, double cx, double cy, double size,
                             const char *name, cairo_surface_t *surface, double alpha) {
    if (surface) {
        int iw = cairo_image_surface_get_width(surface);
        int ih = cairo_image_surface_get_height(surface);
        if (iw > 0 && ih > 0) {
            cairo_save(cr);
            cairo_translate(cr, cx - size / 2.0, cy - size / 2.0);
            cairo_scale(cr, size / iw, size / ih);
            cairo_set_source_surface(cr, surface, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint_with_alpha(cr, alpha);
            cairo_restore(cr);
            return;
        }
    }

    // Glossy squircle icon from hash
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
    cairo_pattern_add_color_stop_rgba(pat, 0.0, fmin(1, r + 0.18), fmin(1, g + 0.18), fmin(1, b + 0.18), alpha);
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

void mission_control_forget_view(aurade_mission_control_t *mc, aurade_view_t *view) {
    if (!mc || !view) return;
    for (int i = 0; i < mc->slot_count; i++) {
        if (mc->slots[i].view != view) continue;
        for (int j = i; j < mc->slot_count - 1; j++) {
            mc->slots[j] = mc->slots[j + 1];
        }
        mc->slot_count--;
        if (mc->hovered_view == view) mc->hovered_view = NULL;
        if (mc->hovered_close_view == view) mc->hovered_close_view = NULL;
        if (mc->dragged_view == view) mc->dragged_view = NULL;
        i--;
    }
    mc->spaces_bar_dirty = true;
}

static void mission_control_compute_layout(aurade_mission_control_t *mc) {
    if (!mc || mc->slot_count == 0) return;

    mc_bar_t bar;
    mc_bar_layout(mc, &bar, 1.0);

    double avail_x = mc->screen_width * 0.08;
    double avail_w = mc->screen_width * 0.84;
    double avail_y = bar.bar_y + bar.bar_h + 32.0;
    double avail_h = mc->screen_height - avail_y - 96.0;

    if (mc->slot_count == 1) {
        // Single window: comfortable GNOME-style thumbnail, max 58% screen width
        double max_w = avail_w * 0.58;
        double max_h = avail_h * 0.68;
        double orig_w = mc->slots[0].orig_w > 0 ? mc->slots[0].orig_w : 800.0;
        double orig_h = mc->slots[0].orig_h > 0 ? mc->slots[0].orig_h : 600.0;
        double scale = fmin(max_w / orig_w, max_h / orig_h);
        if (scale > 0.62) scale = 0.62;
        double tw = round(orig_w * scale);
        double th = round(orig_h * scale);
        if (tw < 260.0) tw = 260.0;
        if (th < 180.0) th = 180.0;

        mc->slots[0].scale = scale;
        mc->slots[0].target_w = tw;
        mc->slots[0].target_h = th;
        mc->slots[0].target_x = round(avail_x + (avail_w - tw) / 2.0);
        mc->slots[0].target_y = round(avail_y + (avail_h - th) / 2.0);
    } else if (mc->slot_count == 2) {
        // Two windows: side-by-side with nice gap, centered horizontally & vertically
        double gap = 36.0;
        double max_cell_w = (avail_w - gap) / 2.0;
        double max_cell_h = avail_h * 0.72;
        double total_w = 0.0;

        for (int i = 0; i < 2; i++) {
            double orig_w = mc->slots[i].orig_w > 0 ? mc->slots[i].orig_w : 800.0;
            double orig_h = mc->slots[i].orig_h > 0 ? mc->slots[i].orig_h : 600.0;
            double scale = fmin(max_cell_w / orig_w, max_cell_h / orig_h);
            if (scale > 0.55) scale = 0.55;
            double tw = round(orig_w * scale);
            double th = round(orig_h * scale);
            if (tw < 260.0) tw = 260.0;
            if (th < 180.0) th = 180.0;
            mc->slots[i].scale = scale;
            mc->slots[i].target_w = tw;
            mc->slots[i].target_h = th;
            total_w += tw;
        }
        total_w += gap;
        double cur_x = avail_x + (avail_w - total_w) / 2.0;
        for (int i = 0; i < 2; i++) {
            mc->slots[i].target_x = round(cur_x);
            mc->slots[i].target_y = round(avail_y + (avail_h - mc->slots[i].target_h) / 2.0);
            cur_x += mc->slots[i].target_w + gap;
        }
    } else {
        // Grid: 2, 3 or 4 columns
        int cols = 2;
        if (mc->slot_count >= 5) cols = 3;
        if (mc->slot_count >= 10) cols = 4;
        int rows = (mc->slot_count + cols - 1) / cols;

        double gap_x = 28.0;
        double gap_y = 32.0;
        double cell_w = (avail_w - gap_x * (cols - 1)) / cols;
        double cell_h = (avail_h - gap_y * (rows - 1)) / rows;

        for (int r = 0; r < rows; r++) {
            int in_this_row = (r == rows - 1) ? (mc->slot_count - r * cols) : cols;
            double row_w = 0.0;
            for (int c = 0; c < in_this_row; c++) {
                int idx = r * cols + c;
                double orig_w = mc->slots[idx].orig_w > 0 ? mc->slots[idx].orig_w : 800.0;
                double orig_h = mc->slots[idx].orig_h > 0 ? mc->slots[idx].orig_h : 600.0;
                double scale = fmin((cell_w * 0.92) / orig_w, (cell_h * 0.90) / orig_h);
                if (scale > 0.52) scale = 0.52;
                double tw = round(orig_w * scale);
                double th = round(orig_h * scale);
                if (tw < 260.0) tw = 260.0;
                if (th < 180.0) th = 180.0;
                mc->slots[idx].scale = scale;
                mc->slots[idx].target_w = tw;
                mc->slots[idx].target_h = th;
                row_w += tw;
            }
            row_w += gap_x * (in_this_row - 1);
            double row_start_x = avail_x + (avail_w - row_w) / 2.0;
            double cur_x = row_start_x;

            for (int c = 0; c < in_this_row; c++) {
                int idx = r * cols + c;
                mc->slots[idx].target_x = round(cur_x);
                mc->slots[idx].target_y = round(avail_y + r * (cell_h + gap_y) + (cell_h - mc->slots[idx].target_h) / 2.0);
                cur_x += mc->slots[idx].target_w + gap_x;
            }
        }
    }
}

static void mission_control_init_slot_metadata(aurade_mission_control_t *mc) {
    if (!mc) return;
    cairo_t *init_cr = mc->buffer ? mc->buffer->cr : NULL;
    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *v = mc->slots[i].view;
        if (!v) continue;
        const char *app_id = (v->xdg_toplevel && v->xdg_toplevel->app_id) ? v->xdg_toplevel->app_id :
            (v->xwayland_surface ? (v->xwayland_surface->class ? v->xwayland_surface->class : v->xwayland_surface->instance) : NULL);
        const char *title = (v->xdg_toplevel && v->xdg_toplevel->title) ? v->xdg_toplevel->title :
            (v->xwayland_surface && v->xwayland_surface->title ? v->xwayland_surface->title : app_id);
        if (!title || !*title) title = "Window";

        snprintf(mc->slots[i].display_title, sizeof(mc->slots[i].display_title), "%s", title);
        mc->slots[i].app = apps_find(mc->server->apps, app_id);
        double tw = init_cr ? text_width(init_cr, mc->slots[i].display_title, MC_FONT, 10.5, true) :
                              (strlen(mc->slots[i].display_title) * 7.0);
        mc->slots[i].title_width = tw;
        double bw = tw + 24.0;
        if (bw < 60.0) bw = 60.0;
        mc->slots[i].badge_w = bw;
    }
}

aurade_mission_control_t *mission_control_create(aurade_server_t *server) {
    aurade_mission_control_t *mc = calloc(1, sizeof(*mc));
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

void mission_control_begin(aurade_mission_control_t *mc) {
    if (!mc) return;

    // Settle any active workspace spring animation immediately so windows are
    // cleanly positioned at (0, 0) for the active workspace.
    if (mc->server->workspace_animating || mc->server->workspace_offset != 0.0) {
        int screen_w = mc->server->screen_width > 0 ? mc->server->screen_width : 1920;
        double p = mc->server->current_workspace - mc->server->workspace_offset / (double)screen_w;
        int target = (int)round(p);
        if (target < 0) target = 0;
        int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
        if (target >= num_ws) target = num_ws - 1;

        mc->server->current_workspace = target;
        mc->server->workspace_animating = false;
        mc->server->workspace_offset = 0.0;
        server_update_workspace_positions(mc->server);
    }

    mc->active = true;
    mc->progress = 0.0;
    mc->slot_count = 0;
    mc->hovered_view = NULL;
    mc->hovered_workspace = -1;
    mc->is_dragging = false;
    mc->mouse_pressed = false;
    mc->dragged_view = NULL;
    mc->drag_target_workspace = -1;
    mc->spaces_bar_dirty = true;
    mc->ws_animating = false;
    mc->ws_slide_offset = 0.0;
    mc->scroll_accum_x = 0.0;
    mc->last_scroll_time_ms = 0;

    // Dismiss any open client popups and disable popup tree while overview is active
    if (mc->server) {
        server_dismiss_popups(mc->server);
        if (mc->server->dock && dock_context_menu_is_open(mc->server->dock)) {
            dock_close_context_menu(mc->server->dock);
        }
        if (mc->server->popup_tree) {
            wlr_scene_node_set_enabled(&mc->server->popup_tree->node, false);
        }
    }

    // Pause dock and topbar blur shaders while Mission Control overlays the screen
    if (mc->server && mc->server->topbar && mc->server->topbar->blur_node) {
        wlr_scene_node_set_enabled(&mc->server->topbar->blur_node->node, false);
    }
    if (mc->server && mc->server->dock && mc->server->dock->blur_node) {
        wlr_scene_node_set_enabled(&mc->server->dock->blur_node->node, false);
    }

    wlr_scene_node_set_enabled(&mc->scene_buffer->node, true);

    // Pre-cache wallpaper thumbnail for Spaces bar (avoids CPU downscaling every frame)
    if (mc->wallpaper_thumb) {
        cairo_surface_destroy(mc->wallpaper_thumb);
        mc->wallpaper_thumb = NULL;
    }
    mc_bar_t bar;
    mc_bar_layout(mc, &bar, 1.0);
    int tw = (int)ceil(bar.thumb_w);
    int th = (int)ceil(bar.thumb_h);
    if (tw > 0 && th > 0 && mc->server->wallpaper) {
        aurade_wallpaper_t *wp = mc->server->wallpaper;
        cairo_surface_t *source_surf = (wp->buffer && wp->buffer->surface) ? wp->buffer->surface : wp->blurred;
        if (source_surf) {
            int sw = cairo_image_surface_get_width(source_surf);
            int sh = cairo_image_surface_get_height(source_surf);
            if (sw > 0 && sh > 0) {
                mc->wallpaper_thumb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
                cairo_t *tcr = cairo_create(mc->wallpaper_thumb);
                cairo_scale(tcr, (double)tw / sw, (double)th / sh);
                cairo_set_source_surface(tcr, source_surf, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(tcr), CAIRO_FILTER_GOOD);
                cairo_paint(tcr);
                cairo_destroy(tcr);
            }
        }
    }

    // Collect visible views in current workspace only
    aurade_view_t *view;
    wl_list_for_each(view, &mc->server->views, link) {
        if (view->workspace != mc->server->current_workspace || view->minimized) continue;
        if (view->width < 60 || view->height <= 0) continue;
        if (mc->slot_count >= 32) break;

        mc->slots[mc->slot_count].view = view;
        mc->slots[mc->slot_count].orig_x = view->x;
        mc->slots[mc->slot_count].orig_y = view->y;
        mc->slots[mc->slot_count].orig_w = view->width;
        mc->slots[mc->slot_count].orig_h = view->height;
        mc->slot_count++;
    }

    if (mc->slot_count == 0) {
        mission_control_render(mc);
        return;
    }

    mission_control_compute_layout(mc);
    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *v = mc->slots[i].view;
        if (!v) continue;
        if (v->scene_tree) {
            view_set_scale(v, 1.0, 1.0);
            wlr_scene_node_set_position(&v->scene_tree->node,
                                        (int)mc->slots[i].orig_x, (int)mc->slots[i].orig_y);
        }
    }
    mission_control_init_slot_metadata(mc);
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
        // Fully closed — restore all windows to original positions and 1.0 scale
        for (int i = 0; i < mc->slot_count; i++) {
            aurade_view_t *view = mc->slots[i].view;
            if (!view || !view->scene_tree) continue;
            view_set_scale(view, 1.0, 1.0);
            wlr_scene_node_set_position(&view->scene_tree->node,
                                        (int)mc->slots[i].orig_x, (int)mc->slots[i].orig_y);
            view->x = (int)mc->slots[i].orig_x;
            view->y = (int)mc->slots[i].orig_y;
            view->width = (int)mc->slots[i].orig_w;
            view->height = (int)mc->slots[i].orig_h;
        }
        wlr_scene_node_set_enabled(&mc->scene_buffer->node, false);
        mc->needs_render = false;
        server_update_workspace_positions(mc->server);
        return;
    }

    wlr_scene_node_set_enabled(&mc->scene_buffer->node, true);
    if (mc->server && mc->server->wallpaper && mc->server->config.blur_workspace_overview) {
        wallpaper_set_overview_blur(mc->server->wallpaper, progress);
    }

    double p = progress;
    if (p > 1.0) p = 1.0;
    if (p < 0.0) p = 0.0;
    double ep = 1.0 - pow(1.0 - p, 3.0);

    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *view = mc->slots[i].view;
        if (!view || !view->scene_tree) continue;

        if (mc->is_dragging && mc->dragged_view == view) continue;

        double cur_scale = (1.0 - ep) * 1.0 + ep * mc->slots[i].scale;
        double cur_x = (1.0 - ep) * mc->slots[i].orig_x + ep * (mc->slots[i].target_x + mc->ws_slide_offset);
        double cur_y = (1.0 - ep) * mc->slots[i].orig_y + ep * mc->slots[i].target_y;

        view->x = (int)cur_x;
        view->y = (int)cur_y;
        wlr_scene_node_set_position(&view->scene_tree->node, (int)round(cur_x), (int)round(cur_y));
        view_set_scale(view, cur_scale, cur_scale);
    }

    mc->needs_render = true;
}

void mission_control_end(aurade_mission_control_t *mc, bool opened) {
    if (!mc) return;
    mc->active = opened;
    if (!opened) {
        mc->ws_animating = false;
        mc->ws_slide_offset = 0.0;
        mc->scroll_accum_x = 0.0;
        if (mc->is_dragging && mc->dragged_view) {
            wlr_scene_node_reparent(&mc->dragged_view->scene_tree->node,
                                    mc->server->workspace_trees[mc->dragged_view->workspace]);
            mc->is_dragging = false;
            mc->dragged_view = NULL;
        }
        if (mc->server && mc->server->topbar && mc->server->topbar->blur_node) {
            wlr_scene_node_set_enabled(&mc->server->topbar->blur_node->node, mc->server->config.blur_topbar);
        }
        if (mc->server && mc->server->dock && mc->server->dock->blur_node) {
            wlr_scene_node_set_enabled(&mc->server->dock->blur_node->node, mc->server->config.blur_dock);
        }
        if (mc->server && mc->server->popup_tree) {
            wlr_scene_node_set_enabled(&mc->server->popup_tree->node, true);
        }
        mc->progress = 0.0;
        mission_control_update_progress(mc, 0.0);
    }
}

bool mission_control_handle_press(aurade_mission_control_t *mc, double x, double y) {
    if (!mc || !mc->active || mc->progress < 0.4) return false;
    mc->mouse_pressed = true;
    mc->is_dragging = false;
    mc->dragged_view = NULL;
    mc->drag_target_workspace = -1;
    mc->drag_start_x = x;
    mc->drag_start_y = y;

    // Check if clicked close button on any window slot
    for (int i = 0; i < mc->slot_count; i++) {
        double slot_x = mc->slots[i].target_x + mc->ws_slide_offset;
        double btn_x = slot_x + 16.0;
        double btn_y = mc->slots[i].target_y + 16.0;
        if (hypot(x - btn_x, y - btn_y) <= 16.0) {
            return true;
        }
    }

    // Check if clicked inside a window slot
    for (int i = 0; i < mc->slot_count; i++) {
        double slot_x = mc->slots[i].target_x + mc->ws_slide_offset;
        if (x >= slot_x && x <= slot_x + mc->slots[i].target_w &&
            y >= mc->slots[i].target_y && y <= mc->slots[i].target_y + mc->slots[i].target_h) {
            mc->drag_slot = i;
            mc->dragged_view = mc->slots[i].view;
            mc->drag_offset_x = x - slot_x;
            mc->drag_offset_y = y - mc->slots[i].target_y;
            return true;
        }
    }

    return true;
}

void mission_control_handle_motion(aurade_mission_control_t *mc, double x, double y) {
    if (!mc || !mc->active || mc->progress < 0.4) return;

    if (mc->mouse_pressed && mc->dragged_view) {
        if (!mc->is_dragging) {
            if (hypot(x - mc->drag_start_x, y - mc->drag_start_y) > 6.0) {
                mc->is_dragging = true;
                wlr_scene_node_reparent(&mc->dragged_view->scene_tree->node, mc->server->dock_tree);
                wlr_scene_node_raise_to_top(&mc->dragged_view->scene_tree->node);
                mission_control_render(mc);
            }
        }

        if (mc->is_dragging) {
            double win_x = x - mc->drag_offset_x;
            double win_y = y - mc->drag_offset_y;
            mc->dragged_view->x = (int)win_x;
            mc->dragged_view->y = (int)win_y;
            wlr_scene_node_set_position(&mc->dragged_view->scene_tree->node, (int)win_x, (int)win_y);

            // Check if hovering over any workspace thumbnail
            mc_bar_t bar;
            mc_bar_layout(mc, &bar, 1.0);
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

    // Check workspace thumbnail hover in Spaces bar
    int prev_ws_hover = mc->hovered_workspace;
    mc->hovered_workspace = -1;
    mc_bar_t bar;
    mc_bar_layout(mc, &bar, 1.0);
    if (y >= bar.bar_y && y <= bar.bar_y + bar.bar_h) {
        int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
        for (int i = 0; i < num_ws; i++) {
            double sx = bar.bar_x + bar.pad_x + i * (bar.thumb_w + bar.gap);
            if (x >= sx - 4.0 && x <= sx + bar.thumb_w + 4.0) {
                mc->hovered_workspace = i;
                break;
            }
        }
    }

    aurade_view_t *prev_close = mc->hovered_close_view;
    mc->hovered_close_view = NULL;
    for (int i = 0; i < mc->slot_count; i++) {
        double slot_x = mc->slots[i].target_x + mc->ws_slide_offset;
        double btn_x = slot_x + 16.0;
        double btn_y = mc->slots[i].target_y + 16.0;
        if (hypot(x - btn_x, y - btn_y) <= 16.0) {
            mc->hovered_close_view = mc->slots[i].view;
            break;
        }
    }

    aurade_view_t *prev_hover = mc->hovered_view;
    mc->hovered_view = NULL;

    for (int i = 0; i < mc->slot_count; i++) {
        double slot_x = mc->slots[i].target_x + mc->ws_slide_offset;
        if (x >= slot_x && x <= slot_x + mc->slots[i].target_w &&
            y >= mc->slots[i].target_y && y <= mc->slots[i].target_y + mc->slots[i].target_h) {
            mc->hovered_view = mc->slots[i].view;
            break;
        }
    }

    if (mc->hovered_workspace != prev_ws_hover) {
        mc->spaces_bar_dirty = true;
    }
    if (mc->hovered_view != prev_hover || mc->hovered_close_view != prev_close ||
        mc->hovered_workspace != prev_ws_hover) {
        mission_control_render(mc);
    }
}

bool mission_control_handle_release(aurade_mission_control_t *mc, double x, double y) {
    if (!mc || !mc->active || mc->progress < 0.4) return false;
    if (!mc->mouse_pressed) return false;
    bool was_dragging = mc->is_dragging;
    aurade_view_t *dragged = mc->dragged_view;
    int target_ws = mc->drag_target_workspace;

    mc->mouse_pressed = false;
    mc->is_dragging = false;
    mc->dragged_view = NULL;
    mc->drag_target_workspace = -1;

    if (was_dragging && dragged) {
        if (target_ws >= 0 && target_ws != dragged->workspace) {
            // Move window to target workspace!
            view_set_scale(dragged, 1.0, 1.0);
            dragged->workspace = target_ws;
            wlr_scene_node_reparent(&dragged->scene_tree->node,
                                    mc->server->workspace_trees[target_ws]);
            dragged->x = (mc->screen_width - dragged->width) / 2;
            dragged->y = (mc->screen_height - dragged->height) / 2;
            if (dragged->x < 0) dragged->x = 0;
            if (dragged->y < TOPBAR_HEIGHT) dragged->y = TOPBAR_HEIGHT;
            wlr_scene_node_set_position(&dragged->scene_tree->node, dragged->x, dragged->y);

            mission_control_forget_view(mc, dragged);
            mission_control_compute_layout(mc);
            mission_control_init_slot_metadata(mc);
            mission_control_update_progress(mc, mc->progress);
            mission_control_render(mc);
            schedule_all_outputs(mc->server);
            return true;
        } else {
            // Snapped back to slot
            wlr_scene_node_reparent(&dragged->scene_tree->node,
                                    mc->server->workspace_trees[dragged->workspace]);
            double tx = mc->slots[mc->drag_slot].target_x + mc->ws_slide_offset;
            double ty = mc->slots[mc->drag_slot].target_y;
            dragged->x = (int)tx;
            dragged->y = (int)ty;
            wlr_scene_node_set_position(&dragged->scene_tree->node, (int)tx, (int)ty);
            view_set_scale(dragged, mc->slots[mc->drag_slot].scale, mc->slots[mc->drag_slot].scale);
            mission_control_render(mc);
            schedule_all_outputs(mc->server);
            return true;
        }
    }

    // Single click (not dragging):
    // 0. Close button on top-left of window slot
    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *view = mc->slots[i].view;
        if (!view) continue;
        double slot_x = mc->slots[i].target_x + mc->ws_slide_offset;
        double btn_x = slot_x + 16.0;
        double btn_y = mc->slots[i].target_y + 16.0;
        if (hypot(x - btn_x, y - btn_y) <= 16.0) {
            view_close(view);
            mission_control_forget_view(mc, view);
            if (mc->slot_count > 0) {
                mission_control_compute_layout(mc);
                mission_control_init_slot_metadata(mc);
                mission_control_update_progress(mc, mc->progress);
            }
            mission_control_render(mc);
            schedule_all_outputs(mc->server);
            return true;
        }
    }

    // 1. Workspace thumbnails in Spaces bar
    mc_bar_t bar;
    mc_bar_layout(mc, &bar, 1.0);
    if (y >= bar.bar_y && y <= bar.bar_y + bar.bar_h) {
        int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
        for (int i = 0; i < num_ws; i++) {
            double sx = bar.bar_x + bar.pad_x + i * (bar.thumb_w + bar.gap);
            if (x >= sx - 4.0 && x <= sx + bar.thumb_w + 4.0) {
                if (i != mc->server->current_workspace) {
                    mission_control_switch_workspace(mc, i);
                } else {
                    server_close_overlay(mc->server);
                }
                return true;
            }
        }
    }

    // 2. Clicked inside a window slot: focus it and exit
    for (int i = 0; i < mc->slot_count; i++) {
        double slot_x = mc->slots[i].target_x + mc->ws_slide_offset;
        if (x >= slot_x && x <= slot_x + mc->slots[i].target_w &&
            y >= mc->slots[i].target_y && y <= mc->slots[i].target_y + mc->slots[i].target_h) {
            aurade_view_t *clicked_view = mc->slots[i].view;
            int target_ws = clicked_view->workspace;
            if (target_ws != mc->server->current_workspace) {
                server_switch_workspace(mc->server, target_ws, false);
            }
            view_focus(clicked_view);
            server_close_overlay(mc->server);
            return true;
        }
    }

    // 3. Horizontal background swipe gesture (mouse drag / touch swipe on empty background)
    double dx = x - mc->drag_start_x;
    double dy = y - mc->drag_start_y;
    if (fabs(dx) > 50.0 && fabs(dx) > fabs(dy) * 1.5) {
        if (dx < 0) {
            mission_control_switch_workspace(mc, mc->server->current_workspace + 1);
        } else {
            mission_control_switch_workspace(mc, mc->server->current_workspace - 1);
        }
        return true;
    }

    // 4. Clicked empty background: exit Mission Control
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
    if (new_workspace == mc->server->current_workspace) return;

    int cur_ws = mc->server->current_workspace;
    int dir = (new_workspace > cur_ws) ? 1 : -1;

    if (mc->is_dragging && mc->dragged_view) {
        wlr_scene_node_reparent(&mc->dragged_view->scene_tree->node,
                                mc->server->workspace_trees[mc->dragged_view->workspace]);
        mc->is_dragging = false;
        mc->dragged_view = NULL;
    }

    // 1. Restore previous workspace's windows to original scale & coordinates
    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *view = mc->slots[i].view;
        if (!view || !view->scene_tree) continue;
        view_set_scale(view, 1.0, 1.0);
        wlr_scene_node_set_position(&view->scene_tree->node,
                                    (int)mc->slots[i].orig_x, (int)mc->slots[i].orig_y);
        view->x = (int)mc->slots[i].orig_x;
        view->y = (int)mc->slots[i].orig_y;
        view->width = (int)mc->slots[i].orig_w;
        view->height = (int)mc->slots[i].orig_h;
    }

    // 2. Switch workspace in server
    server_switch_workspace(mc->server, new_workspace, false);

    // 3. Clear and repopulate slots for new workspace
    mc->slot_count = 0;
    mc->hovered_view = NULL;
    mc->hovered_close_view = NULL;
    mc->hovered_workspace = -1;
    mc->is_dragging = false;
    mc->mouse_pressed = false;
    mc->dragged_view = NULL;
    mc->drag_target_workspace = -1;
    mc->spaces_bar_dirty = true;

    aurade_view_t *view;
    wl_list_for_each(view, &mc->server->views, link) {
        if (view->workspace != mc->server->current_workspace || view->minimized) continue;
        if (view->width < 60 || view->height <= 0) continue;
        if (mc->slot_count >= 32) break;

        mc->slots[mc->slot_count].view = view;
        mc->slots[mc->slot_count].orig_x = view->x;
        mc->slots[mc->slot_count].orig_y = view->y;
        mc->slots[mc->slot_count].orig_w = view->width;
        mc->slots[mc->slot_count].orig_h = view->height;
        mc->slot_count++;
    }

    if (mc->slot_count > 0) {
        mission_control_compute_layout(mc);
        mission_control_init_slot_metadata(mc);
    }

    // 4. Initialize spring sliding animation from swipe direction
    spring_config_t spring = {
        .stiffness = 380.0,
        .damping = 1.05,
        .mass = 1.0,
    };
    if (mc->server && mc->server->config.spring.stiffness > 10.0) {
        spring = mc->server->config.spring;
    }

    spring_simulation_start(&mc->ws_spring, &spring, dir * 320.0, 0.0, 0.0);
    mc->ws_slide_offset = dir * 320.0;
    mc->ws_animating = true;

    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *v = mc->slots[i].view;
        if (!v || !v->scene_tree) continue;
        double cur_x = mc->slots[i].target_x + mc->ws_slide_offset;
        double cur_y = mc->slots[i].target_y;
        v->x = (int)cur_x;
        v->y = (int)cur_y;
        wlr_scene_node_set_position(&v->scene_tree->node, (int)round(cur_x), (int)round(cur_y));
        view_set_scale(v, mc->slots[i].scale, mc->slots[i].scale);
    }

    mission_control_render(mc);
    schedule_all_outputs(mc->server);
}

void mission_control_tick(aurade_mission_control_t *mc, double dt) {
    if (!mc || !mc->active || !mc->ws_animating) return;

    bool active = spring_simulation_step(&mc->ws_spring, dt);
    mc->ws_slide_offset = mc->ws_spring.current_pos;

    if (!active) {
        mc->ws_slide_offset = 0.0;
        mc->ws_animating = false;
    }

    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *view = mc->slots[i].view;
        if (!view || !view->scene_tree) continue;
        if (mc->is_dragging && mc->dragged_view == view) continue;
        double cur_x = mc->slots[i].target_x + mc->ws_slide_offset;
        double cur_y = mc->slots[i].target_y;
        view->x = (int)cur_x;
        view->y = (int)cur_y;
        wlr_scene_node_set_position(&view->scene_tree->node, (int)round(cur_x), (int)round(cur_y));
        view_set_scale(view, mc->slots[i].scale, mc->slots[i].scale);
    }

    mission_control_render(mc);
    schedule_all_outputs(mc->server);
}

void mission_control_handle_axis(aurade_mission_control_t *mc, enum wl_pointer_axis orientation,
                                 double delta, int32_t delta_discrete) {
    if (!mc || !mc->active || mc->progress < 0.5) return;

    if (orientation != WL_POINTER_AXIS_HORIZONTAL_SCROLL) return;

    uint64_t now = mc_now_ms();
    if (delta_discrete != 0) {
        if (now - mc->last_scroll_time_ms < 250) return;
        if (delta_discrete > 0) {
            mission_control_switch_workspace(mc, mc->server->current_workspace + 1);
            mc->last_scroll_time_ms = now;
        } else if (delta_discrete < 0) {
            mission_control_switch_workspace(mc, mc->server->current_workspace - 1);
            mc->last_scroll_time_ms = now;
        }
        return;
    }

    if (now - mc->last_scroll_time_ms > 300) {
        mc->scroll_accum_x = 0;
    }
    mc->last_scroll_time_ms = now;
    mc->scroll_accum_x += delta;

    if (mc->scroll_accum_x > 25.0) {
        mission_control_switch_workspace(mc, mc->server->current_workspace + 1);
        mc->scroll_accum_x = 0;
    } else if (mc->scroll_accum_x < -25.0) {
        mission_control_switch_workspace(mc, mc->server->current_workspace - 1);
        mc->scroll_accum_x = 0;
    }
}

static int workspace_window_count(aurade_server_t *server, int workspace) {
    int count = 0;
    aurade_view_t *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->workspace == workspace && !view->minimized) count++;
    }
    return count;
}

static void draw_workspace_mini_windows(cairo_t *cr, aurade_mission_control_t *mc,
                                        int ws, double sx, double sy,
                                        double thumb_w, double thumb_h,
                                        bool is_dark, double p) {
    if (!mc || !mc->server || mc->screen_width <= 0 || mc->screen_height <= 0) return;

    // Collect visible, unminimized views in this workspace (bottom to top)
    aurade_view_t *ws_views[32];
    int ws_count = 0;
    aurade_view_t *view;
    wl_list_for_each(view, &mc->server->views, link) {
        if (view->workspace != ws || view->minimized) continue;
        if (view->width < 40 || view->height < 40) continue;
        if (ws_count < 32) {
            ws_views[ws_count++] = view;
        }
    }
    if (ws_count == 0) return;

    double scale_x = thumb_w / (double)mc->screen_width;
    double scale_y = thumb_h / (double)mc->screen_height;

    // Render back-to-front so topmost window in z-order is drawn last on top
    for (int i = ws_count - 1; i >= 0; i--) {
        aurade_view_t *v = ws_views[i];

        // For views in current workspace, use unscaled desktop coordinates
        double vx = (double)v->x;
        double vy = (double)v->y;
        double vw = (double)v->width;
        double vh = (double)v->height;
        if (ws == mc->server->current_workspace) {
            for (int k = 0; k < mc->slot_count; k++) {
                if (mc->slots[k].view == v) {
                    vx = mc->slots[k].orig_x;
                    vy = mc->slots[k].orig_y;
                    vw = mc->slots[k].orig_w;
                    vh = mc->slots[k].orig_h;
                    break;
                }
            }
        }

        // Map screen coordinates to thumbnail coordinates
        double wx = sx + vx * scale_x;
        double wy = sy + vy * scale_y;
        double ww = vw * scale_x;
        double wh = vh * scale_y;

        // Ensure reasonable minimum dimensions so tiny windows remain visible
        if (ww < 14.0) ww = 14.0;
        if (wh < 10.0) wh = 10.0;

        double r = fmin(3.5, fmin(ww, wh) * 0.14);
        if (r < 2.0) r = 2.0;

        // Window drop shadow
        draw_rounded_rect(cr, wx, wy + 1.2, ww, wh, r);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45 * p);
        cairo_fill(cr);

        // Window body fill (subtle vertical gradient matching Control Center dark/light themes)
        cairo_pattern_t *pat = cairo_pattern_create_linear(wx, wy, wx, wy + wh);
        if (is_dark) {
            cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.18, 0.20, 0.24, 0.95 * p);
            cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.11, 0.12, 0.15, 0.98 * p);
        } else {
            cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.96, 0.96, 0.98, 0.95 * p);
            cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.88, 0.89, 0.92, 0.98 * p);
        }
        cairo_set_source(cr, pat);
        draw_rounded_rect(cr, wx, wy, ww, wh, r);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);

        // Window header / titlebar stripe
        double tb_h = fmax(3.5, fmin(7.5, wh * 0.22));
        cairo_save(cr);
        draw_rounded_rect(cr, wx, wy, ww, wh, r);
        cairo_clip(cr);
        cairo_rectangle(cr, wx, wy, ww, tb_h);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (is_dark ? 0.07 : 0.05) * p);
        cairo_fill(cr);

        // Hairline separator under titlebar
        cairo_rectangle(cr, wx, wy + tb_h, ww, 0.5);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (is_dark ? 0.08 : 0.06) * p);
        cairo_fill(cr);
        cairo_restore(cr);

        // Traffic light dots if window is wide enough
        if (ww >= 22.0 && wh >= 14.0) {
            double dot_r = fmax(0.7, fmin(1.2, tb_h * 0.20));
            double dot_y = wy + tb_h / 2.0;
            double dot_x = wx + 3.0;
            double dot_spacing = dot_r * 2.0 + 1.8;

            // Red dot
            cairo_arc(cr, dot_x, dot_y, dot_r, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 0.36, 0.33, 0.90 * p);
            cairo_fill(cr);

            // Yellow dot
            cairo_arc(cr, dot_x + dot_spacing, dot_y, dot_r, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 0.74, 0.18, 0.90 * p);
            cairo_fill(cr);

            // Green dot
            cairo_arc(cr, dot_x + dot_spacing * 2.0, dot_y, dot_r, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0.16, 0.80, 0.25, 0.90 * p);
            cairo_fill(cr);
        }

        // App Icon inside the mini window
        const char *app_id = (v->xdg_toplevel && v->xdg_toplevel->app_id) ? v->xdg_toplevel->app_id :
            (v->xwayland_surface ? (v->xwayland_surface->class ? v->xwayland_surface->class : v->xwayland_surface->instance) : NULL);
        desktop_app_t *app = apps_find(mc->server->apps, app_id);

        double icon_sz = fmax(8.0, fmin(18.0, fmin(ww * 0.50, (wh - tb_h) * 0.70)));
        if (icon_sz >= 8.0) {
            double icon_cx = wx + ww / 2.0;
            double icon_cy = wy + tb_h + (wh - tb_h) / 2.0;
            draw_mc_app_icon(cr, icon_cx, icon_cy, icon_sz,
                             app ? app->name : (app_id ? app_id : "App"),
                             app ? app->icon_surface : NULL, p);
        }

        // Specular border around mini window
        draw_rounded_rect(cr, wx + 0.5, wy + 0.5, ww - 1.0, wh - 1.0, r);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (is_dark ? 0.20 : 0.28) * p);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        // Specular highlight on top edge
        cairo_new_path(cr);
        cairo_move_to(cr, wx + r, wy + 0.5);
        cairo_line_to(cr, wx + ww - r, wy + 0.5);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (is_dark ? 0.35 : 0.50) * p);
        cairo_stroke(cr);
    }
}

static void render_spaces_bar_surface(aurade_mission_control_t *mc) {
    if (!mc) return;
    mc_bar_t bar;
    mc_bar_layout(mc, &bar, 1.0);

    double shadow_pad = 24.0;
    int surf_w = (int)ceil(bar.bar_w + shadow_pad * 2.0);
    int surf_h = (int)ceil(bar.bar_h + shadow_pad * 2.0);

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

    double origin_x = shadow_pad;
    double origin_y = shadow_pad;
    mc_theme_t theme = mc_get_theme(mc);

    // 1. Soft ambient drop shadow around the Spaces bar
    draw_drop_shadow(cr, origin_x, origin_y, bar.bar_w, bar.bar_h, 24.0, 20.0, 0.45);

    // 2. Glass panel background (gradient from panel_top to panel_bottom)
    draw_rounded_rect(cr, origin_x, origin_y, bar.bar_w, bar.bar_h, 24.0);
    cairo_pattern_t *bg = cairo_pattern_create_linear(origin_x, origin_y,
                                                      origin_x, origin_y + bar.bar_h);
    cairo_pattern_add_color_stop_rgba(bg, 0.0, theme.panel_top.r, theme.panel_top.g,
                                      theme.panel_top.b, theme.panel_top.a);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, theme.panel_bottom.r, theme.panel_bottom.g,
                                      theme.panel_bottom.b, theme.panel_bottom.a);
    cairo_set_source(cr, bg);
    cairo_fill(cr);
    cairo_pattern_destroy(bg);

    // 3. Specular rim border
    draw_rounded_rect(cr, origin_x + 0.5, origin_y + 0.5, bar.bar_w - 1.0, bar.bar_h - 1.0, 23.5);
    cairo_set_source_rgba(cr, theme.border.r, theme.border.g, theme.border.b, theme.border.a);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // 4. Specular top edge highlight line
    cairo_new_path(cr);
    cairo_move_to(cr, origin_x + 24.0, origin_y + 1.0);
    cairo_line_to(cr, origin_x + bar.bar_w - 24.0, origin_y + 1.0);
    cairo_set_source_rgba(cr, theme.highlight.r, theme.highlight.g, theme.highlight.b, theme.highlight.a);
    cairo_stroke(cr);

    int num_ws = mc->server->num_workspaces > 0 ? mc->server->num_workspaces : DEFAULT_WORKSPACES;
    for (int i = 0; i < num_ws; i++) {
        double sx = origin_x + bar.pad_x + i * (bar.thumb_w + bar.gap);
        double sy = origin_y + bar.pad_top;
        bool is_active = (i == mc->server->current_workspace);
        bool is_drag_target = (mc->is_dragging && mc->drag_target_workspace == i);
        bool is_hover = (mc->hovered_workspace == i && !is_active && !is_drag_target);

        // Inset card drop shadow
        draw_rounded_rect(cr, sx, sy + 1.5, bar.thumb_w, bar.thumb_h, 12.0);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.25);
        cairo_fill(cr);

        // Wallpaper thumbnail + opened windows
        cairo_save(cr);
        draw_rounded_rect(cr, sx, sy, bar.thumb_w, bar.thumb_h, 11.0);
        cairo_clip(cr);

        // Paint wallpaper
        paint_wallpaper_thumb(cr, mc, sx, sy, bar.thumb_w, bar.thumb_h, 0.90);

        // Subtle dimming overlay for inactive workspaces so active workspace pops
        if (!is_active && !is_drag_target) {
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, (is_hover ? 0.12 : 0.25));
            cairo_paint(cr);
        }

        // RENDER OPENED WINDOWS ON THIS WORKSPACE
        draw_workspace_mini_windows(cr, mc, i, sx, sy, bar.thumb_w, bar.thumb_h, theme.dark, 1.0);

        cairo_restore(cr);

        // Highlight border around thumbnail card
        if (is_drag_target) {
            // Drag target: bright cyan ring
            cairo_set_source_rgba(cr, 0.18, 0.78, 1.0, 1.0);
            cairo_set_line_width(cr, 3.0);
            draw_rounded_rect(cr, sx - 1.5, sy - 1.5, bar.thumb_w + 3.0, bar.thumb_h + 3.0, 13.0);
            cairo_stroke(cr);
        } else if (is_active) {
            // Active workspace: Control Center Electric Blue Accent Ring + Outer Soft Glow
            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.35);
            cairo_set_line_width(cr, 5.0);
            draw_rounded_rect(cr, sx - 1.0, sy - 1.0, bar.thumb_w + 2.0, bar.thumb_h + 2.0, 12.5);
            cairo_stroke(cr);

            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 1.0);
            cairo_set_line_width(cr, 2.5);
            draw_rounded_rect(cr, sx - 1.0, sy - 1.0, bar.thumb_w + 2.0, bar.thumb_h + 2.0, 12.5);
            cairo_stroke(cr);
        } else if (is_hover) {
            // Hover highlight
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.45);
            cairo_set_line_width(cr, 1.5);
            draw_rounded_rect(cr, sx - 0.5, sy - 0.5, bar.thumb_w + 1.0, bar.thumb_h + 1.0, 11.5);
            cairo_stroke(cr);
        } else {
            // Inactive subtle border
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16);
            cairo_set_line_width(cr, 1.0);
            draw_rounded_rect(cr, sx + 0.5, sy + 0.5, bar.thumb_w - 1.0, bar.thumb_h - 1.0, 11.0);
            cairo_stroke(cr);
        }

        // Window count badge (top-right of thumbnail)
        int count = workspace_window_count(mc->server, i);
        if (count > 0) {
            char cbuf[16];
            snprintf(cbuf, sizeof(cbuf), "%d", count);
            double badge_h = 16.0;
            double badge_w = 20.0;
            double bx = sx + bar.thumb_w - badge_w - 6.0;
            double by = sy + 6.0;

            // Frosted dark glass badge pill
            draw_rounded_rect(cr, bx, by, badge_w, badge_h, 8.0);
            cairo_set_source_rgba(cr, 0.05, 0.06, 0.08, 0.65);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.20);
            cairo_set_line_width(cr, 0.8);
            cairo_stroke(cr);

            draw_text(cr, cbuf, MC_FONT, 9.5, true, 1.0, 1.0, 1.0, 0.95,
                      bx + badge_w / 2.0, by + badge_h / 2.0, true, true);
        }

        // Desktop Label Pill (cleanly positioned in dedicated label area below thumbnail)
        char label[32];
        if (is_drag_target) {
            snprintf(label, sizeof(label), "Drop to Move");
        } else {
            snprintf(label, sizeof(label), "Desktop %d", i + 1);
        }

        double label_cy = sy + bar.thumb_h + bar.label_gap + bar.label_h / 2.0;
        double tw = text_width(cr, label, MC_FONT, 11.0, (is_active || is_drag_target));
        double pill_w = tw + 20.0;
        double pill_h = bar.label_h;
        double pill_x = sx + (bar.thumb_w - pill_w) / 2.0;
        double pill_y = label_cy - pill_h / 2.0;

        if (is_drag_target) {
            // Bright cyan drop pill
            draw_rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, 11.0);
            cairo_set_source_rgba(cr, 0.18, 0.78, 1.0, 0.90);
            cairo_fill(cr);
            draw_text(cr, label, MC_FONT, 10.5, true, 0.05, 0.08, 0.14, 0.98,
                      sx + bar.thumb_w / 2.0, label_cy, true, true);
        } else if (is_active) {
            // Control Center Electric Blue Accent Pill!
            draw_rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, 11.0);
            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.88);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.25);
            cairo_set_line_width(cr, 1.0);
            draw_rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, 11.0);
            cairo_stroke(cr);
            draw_text(cr, label, MC_FONT, 11.0, true, 1.0, 1.0, 1.0, 0.98,
                      sx + bar.thumb_w / 2.0, label_cy, true, true);
        } else if (is_hover) {
            // Hover card pill
            draw_rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, 11.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
            cairo_fill(cr);
            draw_text(cr, label, MC_FONT, 10.5, true, theme.text.r, theme.text.g, theme.text.b, 0.92,
                      sx + bar.thumb_w / 2.0, label_cy, true, true);
        } else {
            // Subtle calm text
            draw_text(cr, label, MC_FONT, 10.5, false, theme.text_dim.r, theme.text_dim.g, theme.text_dim.b,
                      theme.text_dim.a, sx + bar.thumb_w / 2.0, label_cy, true, true);
        }
    }

    cairo_destroy(cr);
    mc->spaces_bar_dirty = false;
}

void mission_control_render(aurade_mission_control_t *mc) {
    if (!mc || !mc->buffer || !mc->buffer->cr) return;
    mc->needs_render = false;
    cairo_t *cr = mc->buffer->cr;
    cairo_buffer_clear(mc->buffer);

    double p = mc->progress;
    if (p <= 0.0) return;
    if (p > 1.0) p = 1.0;

    mc_theme_t theme = mc_get_theme(mc);

    // If blur is disabled, fallback to a dimming scrim
    if (!mc->server->config.blur_workspace_overview) {
        cairo_set_source_rgba(cr, 0.03, 0.04, 0.07, 0.70 * p);
        cairo_paint(cr);
    }

    // If dragging a view, draw sleek dashed drop-slot placeholder in the original slot
    if (mc->is_dragging && mc->dragged_view) {
        for (int i = 0; i < mc->slot_count; i++) {
            if (mc->slots[i].view == mc->dragged_view) {
                double sx = mc->slots[i].target_x + mc->ws_slide_offset;
                double sy = mc->slots[i].target_y;
                double sw = mc->slots[i].target_w;
                double sh = mc->slots[i].target_h;
                cairo_save(cr);
                draw_rounded_rect(cr, sx, sy, sw, sh, MC_RADIUS);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.06 * p);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.25 * p);
                double dashes[] = { 6.0, 4.0 };
                cairo_set_dash(cr, dashes, 2, 0.0);
                cairo_set_line_width(cr, 1.5);
                cairo_stroke(cr);
                cairo_restore(cr);
                break;
            }
        }
    }

    // Spaces bar at top — Control Center glass panel style
    mc_bar_t bar;
    mc_bar_layout(mc, &bar, p);

    if (mc->spaces_bar_dirty || !mc->spaces_bar_surf) {
        render_spaces_bar_surface(mc);
    }

    if (mc->spaces_bar_surf) {
        cairo_save(cr);
        cairo_set_source_surface(cr, mc->spaces_bar_surf, bar.bar_x - 24.0, bar.bar_y - 24.0);
        cairo_paint_with_alpha(cr, p);
        cairo_restore(cr);
    }


    double ep = 1.0 - pow(1.0 - p, 3.0);

    // Window frames, hover outlines, app icons, and title badges for current workspace
    for (int i = 0; i < mc->slot_count; i++) {
        aurade_view_t *view = mc->slots[i].view;
        if (!view) continue;
        if (mc->is_dragging && mc->dragged_view == view) continue;

        double cur_scale = (1.0 - ep) * 1.0 + ep * mc->slots[i].scale;
        double cx = (1.0 - ep) * mc->slots[i].orig_x + ep * (mc->slots[i].target_x + mc->ws_slide_offset);
        double cy = (1.0 - ep) * mc->slots[i].orig_y + ep * mc->slots[i].target_y;
        double cw = mc->slots[i].orig_w * cur_scale;
        double ch = mc->slots[i].orig_h * cur_scale;

        bool is_hover = (mc->hovered_view == view);

        // Frame border
        if (is_hover) {
            // Control Center electric blue highlight ring with outer soft glow
            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.25 * p);
            cairo_set_line_width(cr, 6.0);
            draw_rounded_rect(cr, cx - 1.0, cy - 1.0, cw + 2.0, ch + 2.0, MC_RADIUS + 1.0);
            cairo_stroke(cr);

            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.90 * p);
            cairo_set_line_width(cr, 3.0);
            draw_rounded_rect(cr, cx, cy, cw, ch, MC_RADIUS);
            cairo_stroke(cr);
        } else {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.20 * p);
            cairo_set_line_width(cr, 1.0);
            draw_rounded_rect(cr, cx, cy, cw, ch, MC_RADIUS);
            cairo_stroke(cr);
        }

        // Top-left close button (macOS-style circular button with 'X')
        double btn_x = cx + 16.0;
        double btn_y = cy + 16.0;
        double btn_r = 12.0;
        bool is_btn_hover = (mc->hovered_close_view == view);

        cairo_save(cr);
        cairo_new_path(cr);
        cairo_arc(cr, btn_x, btn_y, btn_r, 0, 2 * M_PI);
        if (is_btn_hover) {
            cairo_set_source_rgba(cr, 0.95, 0.26, 0.21, 0.35 * p);
            cairo_arc(cr, btn_x, btn_y, btn_r + 3.0, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.95, 0.26, 0.21, 0.98 * p);
        } else {
            cairo_set_source_rgba(cr, 0.16, 0.18, 0.22, 0.88 * p);
        }
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (is_btn_hover ? 0.50 : 0.22) * p);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // 'X' cross
        double arm = 3.8;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (is_btn_hover ? 1.0 : 0.85) * p);
        cairo_set_line_width(cr, 1.5);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_new_path(cr);
        cairo_move_to(cr, btn_x - arm, btn_y - arm);
        cairo_line_to(cr, btn_x + arm, btn_y + arm);
        cairo_move_to(cr, btn_x + arm, btn_y - arm);
        cairo_line_to(cr, btn_x - arm, btn_y + arm);
        cairo_stroke(cr);
        cairo_restore(cr);

        const char *display_title = mc->slots[i].display_title;
        desktop_app_t *app = mc->slots[i].app;
        double icon_sz = 34.0;
        double icon_cx = cx + cw / 2.0;

        double badge_w = mc->slots[i].badge_w;
        if (badge_w > cw + 60.0) badge_w = cw + 60.0;
        double badge_x = cx + (cw - badge_w) / 2.0;

        double icon_cy, badge_y;
        if (cy + ch + 48.0 < mc->screen_height - 65.0) {
            icon_cy = cy + ch + 8.0;
            badge_y = icon_cy + icon_sz / 2.0 + 4.0;
        } else {
            badge_y = cy - 24.0;
            icon_cy = badge_y - icon_sz / 2.0 - 2.0;
        }

        draw_mc_app_icon(cr, icon_cx, icon_cy, icon_sz,
                         app ? app->name : display_title,
                         app ? app->icon_surface : NULL, p);

        // Control Center style glass card title pill
        draw_drop_shadow(cr, badge_x, badge_y, badge_w, 22.0, 11.0, 8.0, 0.35 * p);
        draw_glass_panel(cr, badge_x, badge_y, badge_w, 22.0, 11.0,
                         theme.panel_top.r, theme.panel_top.g, theme.panel_top.b, 0.88 * p);
        draw_text(cr, display_title, MC_FONT, 10.5, true,
                  theme.text.r, theme.text.g, theme.text.b, 0.96 * p,
                  badge_x + badge_w / 2.0, badge_y + 11.0, true, true);
    }

    if (mc->slot_count == 0) {
        double pill_w = 220.0;
        double pill_h = 44.0;
        double pill_x = (mc->screen_width - pill_w) / 2.0;
        double pill_y = (mc->screen_height - pill_h) / 2.0;
        draw_drop_shadow(cr, pill_x, pill_y, pill_w, pill_h, 22.0, 14.0, 0.35 * p);
        draw_glass_panel(cr, pill_x, pill_y, pill_w, pill_h, 22.0,
                         theme.panel_top.r, theme.panel_top.g, theme.panel_top.b, 0.88 * p);
        draw_text(cr, "No Windows Open", MC_FONT, 14.0, true,
                  theme.text.r, theme.text.g, theme.text.b, 0.85 * p,
                  mc->screen_width / 2.0, mc->screen_height / 2.0, true, true);
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
