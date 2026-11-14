#ifndef AURADE_OSD_H
#define AURADE_OSD_H

#include <stdbool.h>
#include "../server.h"
#include "../render/cairo_buffer.h"

typedef enum {
    OSD_TYPE_NONE = 0,
    OSD_TYPE_VOLUME,
    OSD_TYPE_BRIGHTNESS,
} osd_type_t;

typedef struct aurade_osd {
    aurade_server_t *server;
    cairo_buffer_t *buffer;
    struct wlr_scene_buffer *scene_buffer;
    struct wlr_scene_blur *blur_node;
    int screen_width, screen_height;

    osd_type_t type;
    double level;       // 0.0 to 1.0
    bool muted;         // for volume

    double alpha;       // 0.0 to 1.0
    double timer;       // countdown in seconds
    bool visible;
    struct wl_event_source *fade_timer;
} aurade_osd_t;

aurade_osd_t *osd_create(aurade_server_t *server);
void osd_destroy(aurade_osd_t *osd);
void osd_resize(aurade_osd_t *osd, int width, int height);
void osd_show_volume(aurade_osd_t *osd, double level, bool muted);
void osd_show_brightness(aurade_osd_t *osd, double level);
void osd_tick(aurade_osd_t *osd, double dt);
void osd_render(aurade_osd_t *osd);
bool osd_is_animating(const aurade_osd_t *osd);

#endif
