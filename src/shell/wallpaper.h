#ifndef AURADE_WALLPAPER_H
#define AURADE_WALLPAPER_H

#include "../server.h"
#include "../render/cairo_buffer.h"

typedef struct aurade_wallpaper {
    aurade_server_t *server;
    cairo_buffer_t *buffer;
    struct wlr_scene_buffer *scene_buffer;
    int width, height;

    // Small, heavily downscaled copy of the wallpaper. Painting it back at full
    // size with a good filter gives a cheap, smooth backdrop blur for Launchpad
    // and Mission Control.
    cairo_surface_t *blurred;
    bool custom_image;
} aurade_wallpaper_t;

aurade_wallpaper_t *wallpaper_create(aurade_server_t *server);
void wallpaper_resize(aurade_wallpaper_t *wp, int width, int height);
void wallpaper_reload(aurade_wallpaper_t *wp);
void wallpaper_draw_backdrop(aurade_wallpaper_t *wp, cairo_t *cr,
                             int width, int height, double dim, double alpha);
void wallpaper_draw_blur_rect(aurade_wallpaper_t *wp, cairo_t *cr,
                              double screen_x, double screen_y,
                              double rect_x, double rect_y,
                              double rect_w, double rect_h,
                              double corner_radius,
                              double tint_r, double tint_g, double tint_b, double tint_a);
void wallpaper_draw_realtime_blur_rect(aurade_wallpaper_t *wp, cairo_t *cr,
                                       double screen_x, double screen_y,
                                       double rect_x, double rect_y,
                                       double rect_w, double rect_h,
                                       double corner_radius,
                                       double tint_r, double tint_g, double tint_b, double tint_a);
void wallpaper_update_dock_blur(aurade_wallpaper_t *wp,
                                cairo_surface_t **cache_surf,
                                int screen_width,
                                double global_ry,
                                double pill_h);
cairo_surface_t *wallpaper_capture_workspace_blur(aurade_server_t *server, int target_w, int target_h);
void wallpaper_destroy(aurade_wallpaper_t *wp);

#endif // AURADE_WALLPAPER_H
