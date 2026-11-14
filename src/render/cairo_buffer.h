#ifndef AURADE_CAIRO_BUFFER_H
#define AURADE_CAIRO_BUFFER_H

#include <cairo.h>
#include <stdbool.h>
#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <scenefx/types/wlr_scene.h>

typedef struct {
    struct wlr_buffer base;
    cairo_surface_t *surface;
    cairo_t *cr;
    int width;
    int height;
} cairo_buffer_t;

/**
 * Create a new Cairo-backed wlr_buffer with ARGB32 format.
 */
cairo_buffer_t *cairo_buffer_create(int width, int height);

/**
 * Resize existing cairo buffer (recreates surface if dimensions changed).
 */
void cairo_buffer_resize(cairo_buffer_t *buf, int width, int height);

/**
 * Clear the buffer with transparent or solid background.
 */
void cairo_buffer_clear(cairo_buffer_t *buf);

/**
 * Update the scene buffer node with this cairo buffer.
 */
void cairo_buffer_apply_to_scene(cairo_buffer_t *buf, struct wlr_scene_buffer *scene_buf);

/**
 * Draw a smooth rounded rectangle path.
 */
void draw_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r);

/**
 * Register global wallpaper surface for Liquid Glass optical refraction sampling.
 */
void cairo_buffer_set_wallpaper_surface(cairo_surface_t *surf);
cairo_surface_t *cairo_buffer_get_wallpaper_surface(void);

/**
 * Configure global Liquid Glass mode.
 */
void cairo_buffer_set_liquid_glass_enabled(bool enabled);
bool cairo_buffer_get_liquid_glass_enabled(void);

/**
 * Draw a modern fluid frosted glass panel with subtle top-highlight border.
 */
void draw_glass_panel(cairo_t *cr, double x, double y, double w, double h, double r,
                      double r_bg, double g_bg, double b_bg, double a_bg);

void draw_glass_panel_at(cairo_t *cr, double x, double y, double w, double h, double r,
                         double screen_x, double screen_y,
                         double r_bg, double g_bg, double b_bg, double a_bg);

/**
 * Draw Liquid Glass refractive edge cushion (Fresnel lens perimeter glow,
 * corner caustics, specular top crest, and bottom meniscus).
 */
void draw_refractive_glass_rim(cairo_t *cr, double x, double y, double w, double h,
                               double r, double edge_a);

/**
 * Draw Liquid Glass with true optical lens magnification (лупа),
 * Snell's law peripheral barrel refraction (викривлення), chromatic dispersion,
 * soft focus transmission, and specular caustics.
 */
void draw_liquid_glass_lens(cairo_t *target_cr,
                           cairo_surface_t *bg_surf,
                           double panel_x, double panel_y,
                           double panel_w, double panel_h,
                           double panel_r,
                           double screen_x, double screen_y,
                           double mag, double edge_a);

/**
 * Draw a smooth multi-layered drop shadow for windows and floating cards.
 */
void draw_drop_shadow(cairo_t *cr, double x, double y, double w, double h,
                      double r, double blur_size, double max_alpha);

/**
 * Draw styled text using Pango/Cairo.
 */
void draw_text(cairo_t *cr, const char *text, const char *font, double size,
               bool bold, double r, double g, double b, double a,
               double x, double y, bool center_x, bool center_y);

/**
 * Draw styled text with a crisp soft drop shadow in a single layout pass.
 */
void draw_text_shadowed(cairo_t *cr, const char *text, const char *font, double size,
                        bool bold, double r, double g, double b, double a,
                        double shadow_a, double x, double y, bool center_x, bool center_y);

/**
 * Draw styled text with soft drop shadow, wrapped to max_w and limited to max_lines.
 * cx specifies horizontal center; y specifies top offset.
 */
void draw_text_shadowed_wrapped(cairo_t *cr, const char *text, const char *font, double size,
                                bool bold, double r, double g, double b, double a,
                                double shadow_a, double cx, double y,
                                double max_w, int max_lines);

/**
 * Measure a string with the same font/size used by draw_text().
 */
double text_width(cairo_t *cr, const char *text, const char *font, double size, bool bold);

/**
 * Draw single-line text clamped to max_w with an ellipsis (…) at the end using Pango.
 * (x, y) specifies left edge and vertical center line.
 */
void draw_text_clamped(cairo_t *cr, const char *text, const char *font,
                       double size, bool bold, double r, double g, double b, double a,
                       double x, double y, double max_w);

/**
 * Vector status icons. AuraDE must not depend on emoji or private-use
 * glyphs: on most Linux systems those fall back to tofu or a colour emoji font,
 * which instantly breaks the fluid look. These draw crisp scalable shapes.
 */
void draw_sun_logo(cairo_t *cr, double cx, double cy, double size, double a);
void draw_wifi_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_battery_icon(cairo_t *cr, double x, double y, double w, double h,
                       double level, double a, bool charging);
void draw_control_center_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_bluetooth_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_moon_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_half_circle_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_sun_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_speaker_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_search_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_cpu_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_chevron(cairo_t *cr, double cx, double cy, double size, int direction, double a);

/**
 * Tint for subsequently drawn icons (default white). Lets the same glyphs be
 * used on blue toggles, near-white slider fills and light-mode glass.
 */
void draw_icon_tint(double r, double g, double b);

/* Control Center extras: settings, media transport and a note glyph. */
void draw_gear_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_sliders_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_music_note_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_play_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_pause_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_skip_icon(cairo_t *cr, double cx, double cy, double size, int direction, double a);
void draw_airdrop_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_hotspot_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_keyboard_brightness_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_aura_connect_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_screen_mirror_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_screen_keyboard_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_display_badge_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_airplay_audio_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_user_profile_icon(cairo_t *cr, double cx, double cy, double size, double a);
void draw_accessibility_icon(cairo_t *cr, double cx, double cy, double size, double a);

#endif // AURADE_CAIRO_BUFFER_H
