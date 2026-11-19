#ifndef AURADE_EXT_H
#define AURADE_EXT_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>

#define AURADE_EXT_API_VERSION 1

typedef struct aurade_ext_panel aurade_ext_panel_t;

typedef struct {
    const char *name;
    const char *version;
    const char *author;
    const char *description;
} aurade_ext_info_t;

typedef struct {
    void *handle;
    const char *app_id;
    const char *title;
    int x, y, width, height;
    int workspace;
    bool focused;
} aurade_ext_win_t;

typedef void (*aurade_ext_draw_fn)(cairo_t *cr, int w, int h, void *userdata);
typedef bool (*aurade_ext_panel_click_fn)(double lx, double ly, bool pressed, void *userdata);
typedef void (*aurade_ext_tick_fn)(double dt_sec, void *userdata);
typedef bool (*aurade_ext_key_fn)(uint32_t xkb_sym, uint32_t mods, bool pressed, void *userdata);
typedef bool (*aurade_ext_click_fn)(double lx, double ly, uint32_t button, bool pressed, void *userdata);
typedef void (*aurade_ext_topbar_draw_fn)(cairo_t *cr, int bar_w, int bar_h, void *userdata);
typedef void (*aurade_ext_win_fn)(const aurade_ext_win_t *win, void *userdata);

typedef struct {
    uint32_t api_version;

    /* Logging & display info */
    void (*log)(const char *fmt, ...);
    void (*get_screen_size)(int *out_w, int *out_h);
    bool (*is_dark_mode)(void);

    /* Create and manage custom UI overlay elements */
    aurade_ext_panel_t *(*panel_create)(int x, int y, int w, int h, bool blur,
                                        aurade_ext_draw_fn draw_cb,
                                        aurade_ext_panel_click_fn click_cb,
                                        void *userdata);
    void (*panel_set_pos)(aurade_ext_panel_t *panel, int x, int y);
    void (*panel_set_visible)(aurade_ext_panel_t *panel, bool visible);
    void (*panel_redraw)(aurade_ext_panel_t *panel);
    void (*panel_destroy)(aurade_ext_panel_t *panel);

    /* Hook into existing DE behavior */
    void (*hook_tick)(aurade_ext_tick_fn cb, void *userdata);
    void (*hook_key)(aurade_ext_key_fn cb, void *userdata);
    void (*hook_click)(aurade_ext_click_fn cb, void *userdata);
    void (*hook_topbar_draw)(aurade_ext_topbar_draw_fn cb, void *userdata);
    void (*hook_window_open)(aurade_ext_win_fn cb, void *userdata);
    void (*hook_window_close)(aurade_ext_win_fn cb, void *userdata);
    void (*hook_window_focus)(aurade_ext_win_fn cb, void *userdata);

    /* Control existing windows & DE components */
    void (*window_move_resize)(void *win_handle, int x, int y, int w, int h);
    void (*window_focus)(void *win_handle);
    void (*window_close)(void *win_handle);
    void (*exec)(const char *cmd);
    void (*topbar_redraw)(void);
    void (*toggle_control_center)(void);
    void (*toggle_notifications)(void);
    void (*toggle_launchpad)(void);
    void (*lock_screen)(void);
    void (*set_wallpaper)(const char *path);
    void (*osd_show_volume)(double level, bool muted);
    void (*osd_show_brightness)(double level);
} aurade_ext_api_t;

/* Required extension entry point:
 *   bool aurade_ext_init(const aurade_ext_api_t *api);
 * Optional entry points:
 *   const aurade_ext_info_t *aurade_ext_meta(void);
 *   void aurade_ext_deinit(void);
 */

#endif // AURADE_EXT_H
