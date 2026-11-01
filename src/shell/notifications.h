#ifndef AURADE_NOTIFICATIONS_H
#define AURADE_NOTIFICATIONS_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <gio/gio.h>
#include "../server.h"
#include "../render/cairo_buffer.h"

#define NOTIF_MAX_VISIBLE 6
#define NOTIF_CARD_W 350.0

typedef struct {
    uint32_t id;
    char app_name[64];
    char app_icon[128];
    char summary[128];
    char body[512];
    cairo_surface_t *icon_surface;

    double slide_progress;   // 0.0 (off-screen right) -> 1.0 (fully visible)
    double alpha;            // 0.0 -> 1.0
    double timer;            // remaining time in seconds
    bool dismissing;

    double card_y;           // layout Y inside buffer
    double card_h;           // computed height
    bool hover;
    bool close_hover;
} notification_item_t;

typedef struct aurade_notifications {
    aurade_server_t *server;
    cairo_buffer_t *buffer;
    struct wlr_scene_buffer *scene_buffer;
    int screen_width, screen_height;

    notification_item_t items[NOTIF_MAX_VISIBLE];
    int count;
    uint32_t next_id;

    // Inter-thread pipe to deliver D-Bus calls into Wayland main loop
    int pipe_read_fd;
    int pipe_write_fd;
    struct wl_event_source *event_source;

    // D-Bus thread and state
    pthread_t dbus_thread;
    GMainLoop *dbus_loop;
    GDBusConnection *dbus_connection;
    guint bus_owner_id;
    bool dbus_running;

    // Mouse tracking
    double mouse_x, mouse_y;
    int hovered_index;
    bool hovered_close;
} aurade_notifications_t;

aurade_notifications_t *notifications_create(aurade_server_t *server);
void notifications_destroy(aurade_notifications_t *notif);
void notifications_resize(aurade_notifications_t *notif, int width, int height);
void notifications_tick(aurade_notifications_t *notif, double dt);
void notifications_render(aurade_notifications_t *notif);
bool notifications_handle_motion(aurade_notifications_t *notif, double mouse_x, double mouse_y);
bool notifications_handle_click(aurade_notifications_t *notif, double mouse_x, double mouse_y);
bool notifications_pointer_owns(aurade_notifications_t *notif, double x, double y);

#endif // AURADE_NOTIFICATIONS_H
