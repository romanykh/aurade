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
    cairo_buffer_t *blurred_buffer;
    struct wlr_scene_buffer *blurred_scene_buffer;
    bool custom_image;
} aurade_wallpaper_t;

aurade_wallpaper_t *wallpaper_create(aurade_server_t *server);
void wallpaper_resize(aurade_wallpaper_t *wp, int width, int height);
void wallpaper_reload(aurade_wallpaper_t *wp);
void wallpaper_draw_backdrop(aurade_wallpaper_t *wp, cairo_t *cr,
                             int width, int height, double dim, double alpha);
void wallpaper_set_overview_blur(aurade_wallpaper_t *wp, double progress);
void wallpaper_destroy(aurade_wallpaper_t *wp);

#endif // AURADE_WALLPAPER_H
