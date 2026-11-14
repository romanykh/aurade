#include "notifications.h"
#include "../util/apps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <gio/gio.h>
#include <cairo.h>
#include <pango/pangocairo.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NOTIF_BUFFER_W 380
#define NOTIF_BUFFER_H 620
#define NOTIF_PAD_X 15.0
#define NOTIF_CARD_W 350.0
#define NOTIF_GAP 10.0
#define NOTIF_FONT "Inter, Noto Sans CJK JP, Adwaita Sans, sans-serif"

typedef enum {
    NOTIF_CMD_NOTIFY = 1,
    NOTIF_CMD_CLOSE = 2,
} notif_cmd_type_t;

typedef struct {
    notif_cmd_type_t type;
    uint32_t id;
    uint32_t replaces_id;
    int32_t expire_timeout;
    char app_name[64];
    char app_icon[128];
    char summary[128];
    char body[512];
} notif_cmd_t;

static const char introspection_xml[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.freedesktop.Notifications\">\n"
    "    <method name=\"Notify\">\n"
    "      <arg name=\"app_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"replaces_id\" type=\"u\" direction=\"in\"/>\n"
    "      <arg name=\"app_icon\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"summary\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"body\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"actions\" type=\"as\" direction=\"in\"/>\n"
    "      <arg name=\"hints\" type=\"a{sv}\" direction=\"in\"/>\n"
    "      <arg name=\"expire_timeout\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"id\" type=\"u\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"CloseNotification\">\n"
    "      <arg name=\"id\" type=\"u\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"GetCapabilities\">\n"
    "      <arg name=\"capabilities\" type=\"as\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetServerInformation\">\n"
    "      <arg name=\"name\" type=\"s\" direction=\"out\"/>\n"
    "      <arg name=\"vendor\" type=\"s\" direction=\"out\"/>\n"
    "      <arg name=\"version\" type=\"s\" direction=\"out\"/>\n"
    "      <arg name=\"spec_version\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <signal name=\"NotificationClosed\">\n"
    "      <arg name=\"id\" type=\"u\"/>\n"
    "      <arg name=\"reason\" type=\"u\"/>\n"
    "    </signal>\n"
    "    <signal name=\"ActionInvoked\">\n"
    "      <arg name=\"id\" type=\"u\"/>\n"
    "      <arg name=\"action_key\" type=\"s\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "</node>";

static void draw_telegram_paper_plane(cairo_t *cr, double cx, double cy, double size) {
    double half = size / 2.0;
    double corner = size * 0.235;

    draw_drop_shadow(cr, cx - half, cy - half, size, size, corner, size * 0.10, 0.40);

    // Telegram circular squircle in signature sky-blue
    cairo_save(cr);
    draw_rounded_rect(cr, cx - half, cy - half, size, size, corner);
    cairo_pattern_t *pat = cairo_pattern_create_linear(cx, cy - half, cx, cy + half);
    cairo_pattern_add_color_stop_rgb(pat, 0.0, 0.18, 0.70, 0.95);
    cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.11, 0.58, 0.88);
    cairo_set_source(cr, pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(pat);

    // Inner rim highlight
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.28);
    cairo_set_line_width(cr, fmax(1.0, size * 0.02));
    cairo_stroke(cr);
    cairo_restore(cr);

    // Crisp white folded paper airplane
    cairo_save(cr);
    cairo_translate(cr, cx - size * 0.02, cy + size * 0.02);
    double s = size * 0.52;

    // Main wing polygon
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.98);
    cairo_move_to(cr, s * 0.48, -s * 0.44);    // Nose
    cairo_line_to(cr, -s * 0.48, -s * 0.06);   // Left tip
    cairo_line_to(cr, -s * 0.12, s * 0.16);    // Fold
    cairo_line_to(cr, s * 0.48, -s * 0.44);
    cairo_fill(cr);

    // Right wing
    cairo_set_source_rgba(cr, 0.92, 0.95, 0.99, 0.98);
    cairo_move_to(cr, s * 0.48, -s * 0.44);
    cairo_line_to(cr, -s * 0.12, s * 0.16);
    cairo_line_to(cr, s * 0.08, s * 0.46);
    cairo_line_to(cr, s * 0.48, -s * 0.44);
    cairo_fill(cr);

    // Under-wing fold shadow
    cairo_set_source_rgba(cr, 0.72, 0.84, 0.95, 0.95);
    cairo_move_to(cr, -s * 0.12, s * 0.16);
    cairo_line_to(cr, -s * 0.02, s * 0.32);
    cairo_line_to(cr, s * 0.08, s * 0.46);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_restore(cr);
}

static void emit_signal_closed(aurade_notifications_t *notif, uint32_t id, uint32_t reason) {
    if (!notif || !notif->dbus_connection) return;
    g_dbus_connection_emit_signal(
        notif->dbus_connection,
        NULL,
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "NotificationClosed",
        g_variant_new("(uu)", id, reason),
        NULL
    );
}

static void emit_signal_action(aurade_notifications_t *notif, uint32_t id, const char *action_key) {
    if (!notif || !notif->dbus_connection) return;
    g_dbus_connection_emit_signal(
        notif->dbus_connection,
        NULL,
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "ActionInvoked",
        g_variant_new("(us)", id, action_key ? action_key : "default"),
        NULL
    );
}

static void on_method_call(GDBusConnection *connection,
                           const gchar *sender,
                           const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *method_name,
                           GVariant *parameters,
                           GDBusMethodInvocation *invocation,
                           gpointer user_data) {
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)connection;
    aurade_notifications_t *notif = user_data;

    if (g_strcmp0(method_name, "Notify") == 0) {
        const gchar *app_name = NULL;
        guint32 replaces_id = 0;
        const gchar *app_icon = NULL;
        const gchar *summary = NULL;
        const gchar *body = NULL;
        GVariantIter *actions_iter = NULL;
        GVariant *hints_dict = NULL;
        gint32 expire_timeout = -1;

        g_variant_get(parameters, "(&su&s&s&sas@a{sv}i)",
                      &app_name, &replaces_id, &app_icon,
                      &summary, &body, &actions_iter,
                      &hints_dict, &expire_timeout);

        if (actions_iter) g_variant_iter_free(actions_iter);

        uint32_t id = replaces_id > 0 ? replaces_id : ++notif->next_id;
        if (id == 0) id = ++notif->next_id;

        notif_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = NOTIF_CMD_NOTIFY;
        cmd.id = id;
        cmd.replaces_id = replaces_id;
        cmd.expire_timeout = expire_timeout;
        snprintf(cmd.app_name, sizeof(cmd.app_name), "%s", app_name ? app_name : "");
        snprintf(cmd.app_icon, sizeof(cmd.app_icon), "%s", app_icon ? app_icon : "");
        snprintf(cmd.summary, sizeof(cmd.summary), "%s", summary ? summary : "");
        snprintf(cmd.body, sizeof(cmd.body), "%s", body ? body : "");

        if (hints_dict) {
            GVariant *v = g_variant_lookup_value(hints_dict, "image-path", G_VARIANT_TYPE_STRING);
            if (!v) v = g_variant_lookup_value(hints_dict, "image_path", G_VARIANT_TYPE_STRING);
            if (v) {
                const gchar *hint_img = g_variant_get_string(v, NULL);
                if (hint_img && *hint_img) {
                    snprintf(cmd.app_icon, sizeof(cmd.app_icon), "%s", hint_img);
                }
                g_variant_unref(v);
            }
            g_variant_unref(hints_dict);
        }

        printf("[AuraDE Notifications] Received notification id=%u app='%s' icon='%s' summary='%s'\n",
               id, cmd.app_name, cmd.app_icon, cmd.summary);

        if (notif->pipe_write_fd >= 0) {
            ssize_t ret = write(notif->pipe_write_fd, &cmd, sizeof(cmd));
            (void)ret;
        }

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
    } else if (g_strcmp0(method_name, "CloseNotification") == 0) {
        guint32 id = 0;
        g_variant_get(parameters, "(u)", &id);

        notif_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = NOTIF_CMD_CLOSE;
        cmd.id = id;

        if (notif->pipe_write_fd >= 0) {
            ssize_t ret = write(notif->pipe_write_fd, &cmd, sizeof(cmd));
            (void)ret;
        }

        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&builder, "s", "body");
        g_variant_builder_add(&builder, "s", "body-markup");
        g_variant_builder_add(&builder, "s", "actions");
        g_variant_builder_add(&builder, "s", "icon-static");
        g_variant_builder_add(&builder, "s", "persistence");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", &builder));
    } else if (g_strcmp0(method_name, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(ssss)", "AuraDE Notifications", "AuraDE", "1.0", "1.2")
        );
    }
}

static const GDBusInterfaceVTable interface_vtable = {
    .method_call = on_method_call,
    .get_property = NULL,
    .set_property = NULL,
};

static void on_bus_acquired(GDBusConnection *connection,
                            const gchar *name,
                            gpointer user_data) {
    (void)name;
    aurade_notifications_t *notif = user_data;
    notif->dbus_connection = connection;

    GError *error = NULL;
    GDBusNodeInfo *introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (!introspection_data) {
        if (error) {
            fprintf(stderr, "[AuraDE Notifications] Introspection parse error: %s\n", error->message);
            g_error_free(error);
        }
        return;
    }

    g_dbus_connection_register_object(
        connection,
        "/org/freedesktop/Notifications",
        introspection_data->interfaces[0],
        &interface_vtable,
        notif,
        NULL,
        NULL
    );
    g_dbus_node_info_unref(introspection_data);
}

static void on_name_acquired(GDBusConnection *connection,
                             const gchar *name,
                             gpointer user_data) {
    (void)connection;
    (void)name;
    (void)user_data;
    printf("[AuraDE] Acquired D-Bus service org.freedesktop.Notifications\n");
}

static void on_name_lost(GDBusConnection *connection,
                         const gchar *name,
                         gpointer user_data) {
    (void)connection;
    (void)name;
    (void)user_data;
}

static void *notifications_dbus_worker(void *arg) {
    aurade_notifications_t *notif = arg;
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);

    notif->dbus_loop = g_main_loop_new(ctx, FALSE);
    notif->bus_owner_id = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        "org.freedesktop.Notifications",
        G_BUS_NAME_OWNER_FLAGS_REPLACE,
        on_bus_acquired,
        on_name_acquired,
        on_name_lost,
        notif,
        NULL
    );

    notif->dbus_running = true;
    g_main_loop_run(notif->dbus_loop);

    if (notif->bus_owner_id > 0) {
        g_bus_unown_name(notif->bus_owner_id);
        notif->bus_owner_id = 0;
    }

    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    return NULL;
}

static cairo_surface_t *resolve_notification_icon(aurade_notifications_t *notif,
                                                  const char *app_name,
                                                  const char *app_icon) {
    // If a direct file path is given, load it directly
    if (app_icon && (app_icon[0] == '/' || strncmp(app_icon, "file://", 7) == 0)) {
        const char *path = (strncmp(app_icon, "file://", 7) == 0) ? app_icon + 7 : app_icon;
        cairo_surface_t *s = apps_load_icon(path);
        if (s) return s;
    }

    // Check if it's Telegram (or Telegram icon specified)
    if ((app_name && strcasestr(app_name, "telegram")) ||
        (app_icon && strcasestr(app_icon, "telegram"))) {
        cairo_surface_t *s = NULL;
        if (app_icon && *app_icon) s = apps_load_icon(app_icon);
        if (!s && notif->server && notif->server->apps) {
            desktop_app_t *app = apps_find(notif->server->apps, "telegram");
            if (app && app->icon_surface) s = cairo_surface_reference(app->icon_surface);
        }
        return s; // If null, draw_telegram_paper_plane will render vector fallback!
    }

    // Try direct icon name or path
    if (app_icon && *app_icon) {
        cairo_surface_t *s = apps_load_icon(app_icon);
        if (s) return s;
    }

    // Try finding via installed desktop applications
    if (app_name && *app_name && notif->server && notif->server->apps) {
        desktop_app_t *app = apps_find(notif->server->apps, app_name);
        if (app && app->icon_surface) {
            return cairo_surface_reference(app->icon_surface);
        }
    }
    return NULL;
}

static int nc_compute_panel_h(int screen_h) {
    int h = screen_h - 38 - 108;
    if (h < 280) h = 280;
    return h;
}

static void nc_panel_rect(const aurade_notifications_t *notif, double *out_x, double *out_y, double *out_w, double *out_h) {
    double w = NOTIF_CENTER_W;
    double h = notif->nc_panel_h > 0 ? (double)notif->nc_panel_h : 500.0;
    double y = 38.0;
    bool left = notif->server && notif->server->config.notif_center_left;
    double closed_x = left ? (-w - 16.0) : (notif->screen_width + 16.0);
    double open_x = left ? 12.0 : (notif->screen_width - w - 12.0);
    double x = closed_x + (open_x - closed_x) * notif->nc_progress;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

static void nc_draw_liquid_glass_panel(cairo_t *cr, double x, double y, double w, double h,
                                       double r, double opacity) {
    cairo_save(cr);

    draw_rounded_rect(cr, x, y, w, h, r);
    cairo_pattern_t *bg = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.13, 0.14, 0.19, 0.80 * opacity);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.08, 0.09, 0.12, 0.88 * opacity);
    cairo_set_source(cr, bg);
    cairo_fill(cr);
    cairo_pattern_destroy(bg);

    cairo_save(cr);
    draw_rounded_rect(cr, x, y, w, h, r);
    cairo_clip(cr);
    double sheen_h = 80.0;
    cairo_pattern_t *sheen = cairo_pattern_create_linear(x, y, x, y + sheen_h);
    cairo_pattern_add_color_stop_rgba(sheen, 0.0, 1.0, 1.0, 1.0, 0.045 * opacity);
    cairo_pattern_add_color_stop_rgba(sheen, 0.6, 1.0, 1.0, 1.0, 0.01 * opacity);
    cairo_pattern_add_color_stop_rgba(sheen, 1.0, 1.0, 1.0, 1.0, 0.0);
    cairo_rectangle(cr, x, y, w, sheen_h);
    cairo_set_source(cr, sheen);
    cairo_fill(cr);
    cairo_pattern_destroy(sheen);
    cairo_restore(cr);

    draw_refractive_glass_rim(cr, x, y, w, h, r, 0.90 * opacity);
    cairo_restore(cr);
}

static void nc_update_scene_nodes(aurade_notifications_t *notif) {
    if (!notif || !notif->nc_scene_buffer) return;
    if (notif->nc_progress <= 0.002) {
        wlr_scene_node_set_enabled(&notif->nc_scene_buffer->node, false);
        if (notif->nc_blur_node) wlr_scene_node_set_enabled(&notif->nc_blur_node->node, false);
        return;
    }

    double px, py, pw, ph;
    nc_panel_rect(notif, &px, &py, &pw, &ph);
    int ix = (int)lround(px);
    int iy = (int)lround(py);

    wlr_scene_node_set_position(&notif->nc_scene_buffer->node, ix, iy);
    wlr_scene_node_set_enabled(&notif->nc_scene_buffer->node, true);
    wlr_scene_node_raise_to_top(&notif->nc_scene_buffer->node);

    if (notif->nc_blur_node) {
        glass_element_t gnc = notif->server ? notif->server->config.glass_notification_center : (glass_element_t){ 0.84, 1.0, true };
        bool liquid = gnc.liquid_glass;
        bool blur_enabled = (gnc.blur > 0.01 || liquid);
        if (blur_enabled && notif->nc_progress > 0.05) {
            wlr_scene_blur_set_size(notif->nc_blur_node, (int)pw, (int)ph);
            wlr_scene_blur_set_corner_radius(notif->nc_blur_node, 20);
            wlr_scene_node_set_position(&notif->nc_blur_node->node, ix, iy);
            wlr_scene_blur_set_liquid_glass(notif->nc_blur_node, liquid);
            wlr_scene_blur_set_strength(notif->nc_blur_node, (float)gnc.blur);
            wlr_scene_blur_set_alpha(notif->nc_blur_node, (float)notif->nc_progress);
            wlr_scene_node_set_enabled(&notif->nc_blur_node->node, true);
            wlr_scene_node_place_below(&notif->nc_blur_node->node, &notif->nc_scene_buffer->node);
        } else {
            wlr_scene_node_set_enabled(&notif->nc_blur_node->node, false);
        }
    }
}

static void notifications_nc_render(aurade_notifications_t *notif) {
    if (!notif || !notif->nc_buffer || !notif->nc_buffer->cr) return;
    cairo_t *cr = notif->nc_buffer->cr;
    cairo_buffer_clear(notif->nc_buffer);

    if (notif->nc_progress <= 0.002) {
        nc_update_scene_nodes(notif);
        return;
    }

    double w = NOTIF_CENTER_W;
    double h = notif->nc_panel_h > 0 ? (double)notif->nc_panel_h : 500.0;
    double a = notif->nc_progress;
    if (a > 1.0) a = 1.0;

    glass_element_t gnc = notif->server ? notif->server->config.glass_notification_center : (glass_element_t){ 0.84, 1.0, true };
    bool liquid = gnc.liquid_glass;
    double op_scale = gnc.opacity / 0.84;

    if (liquid) {
        nc_draw_liquid_glass_panel(cr, 0.0, 0.0, w, h, 20.0, a * op_scale);
    } else {
        draw_glass_panel(cr, 0.0, 0.0, w, h, 20.0, 0.12, 0.13, 0.17, gnc.opacity * a);
    }

    // Header
    draw_text(cr, "Notifications", NOTIF_FONT, 15.0, true,
              1.0, 1.0, 1.0, 0.95 * a, 18.0, 27.0, false, true);

    if (notif->history_count > 0) {
        double btn_w = 70.0, btn_h = 24.0;
        double btn_x = w - btn_w - 16.0, btn_y = 15.0;
        draw_rounded_rect(cr, btn_x, btn_y, btn_w, btn_h, 12.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (notif->nc_clear_hover ? 0.24 : 0.13) * a);
        cairo_fill(cr);
        draw_text(cr, "Clear All", NOTIF_FONT, 11.0, true,
                  1.0, 1.0, 1.0, 0.90 * a,
                  btn_x + btn_w / 2.0, btn_y + btn_h / 2.0, true, true);
    }

    // Subtle divider
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10 * a);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 16.0, 50.0);
    cairo_line_to(cr, w - 16.0, 50.0);
    cairo_stroke(cr);

    double list_top = 58.0;
    double list_h = h - list_top - 14.0;

    if (notif->history_count == 0) {
        notif->nc_max_scroll = 0.0;
        notif->nc_scroll_y = 0.0;
        draw_text(cr, "No Notifications", NOTIF_FONT, 13.5, false,
                  1.0, 1.0, 1.0, 0.45 * a,
                  w / 2.0, list_top + list_h * 0.42, true, true);
        cairo_buffer_apply_to_scene(notif->nc_buffer, notif->nc_scene_buffer);
        nc_update_scene_nodes(notif);
        return;
    }

    // Compute layout heights and max scroll
    double cur_y = 0.0;
    for (int i = 0; i < notif->history_count; i++) {
        notif_history_item_t *hi = &notif->history[i];
        double ch = hi->body[0] ? (strlen(hi->body) > 60 ? 92.0 : 80.0) : 70.0;
        hi->card_y = cur_y;
        hi->card_h = ch;
        cur_y += ch + 10.0;
    }
    double total_h = cur_y > 0.0 ? cur_y - 10.0 : 0.0;
    notif->nc_max_scroll = total_h > list_h ? (total_h - list_h) : 0.0;
    if (notif->nc_scroll_y < 0.0) notif->nc_scroll_y = 0.0;
    if (notif->nc_scroll_y > notif->nc_max_scroll) notif->nc_scroll_y = notif->nc_max_scroll;

    cairo_save(cr);
    cairo_rectangle(cr, 8.0, list_top, w - 16.0, list_h);
    cairo_clip(cr);

    for (int i = 0; i < notif->history_count; i++) {
        notif_history_item_t *hi = &notif->history[i];
        double cx = 14.0;
        double cy = list_top + hi->card_y - notif->nc_scroll_y;
        double cw = w - 28.0;
        double ch = hi->card_h;

        if (cy + ch < list_top || cy > list_top + list_h) continue;

        // Card container
        draw_rounded_rect(cr, cx, cy, cw, ch, 14.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (hi->hover ? 0.14 : 0.08) * a);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12 * a);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // App icon
        double icon_cx = cx + 20.0;
        double icon_cy = cy + 16.0;
        if (hi->icon_surface) {
            int sw = cairo_image_surface_get_width(hi->icon_surface);
            int sh = cairo_image_surface_get_height(hi->icon_surface);
            if (sw > 0 && sh > 0) {
                cairo_save(cr);
                cairo_translate(cr, icon_cx - 9.0, icon_cy - 9.0);
                cairo_scale(cr, 18.0 / sw, 18.0 / sh);
                cairo_set_source_surface(cr, hi->icon_surface, 0, 0);
                cairo_paint_with_alpha(cr, a);
                cairo_restore(cr);
            }
        } else {
            cairo_arc(cr, icon_cx, icon_cy, 8.0, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0.35, 0.65, 1.0, 0.85 * a);
            cairo_fill(cr);
        }

        // App name + timestamp
        draw_text(cr, hi->app_name, NOTIF_FONT, 10.5, true,
                  1.0, 1.0, 1.0, 0.65 * a, cx + 34.0, icon_cy, false, true);

        if (hi->time_str[0]) {
            draw_text(cr, hi->time_str, NOTIF_FONT, 10.0, false,
                      1.0, 1.0, 1.0, 0.45 * a, cx + cw - 34.0, icon_cy, true, true);
        }

        // Close button (on hover)
        if (hi->hover || hi->close_hover) {
            double bx = cx + cw - 16.0;
            double by = icon_cy;
            cairo_arc(cr, bx, by, 8.5, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (hi->close_hover ? 0.28 : 0.14) * a);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90 * a);
            cairo_set_line_width(cr, 1.3);
            cairo_move_to(cr, bx - 3.0, by - 3.0);
            cairo_line_to(cr, bx + 3.0, by + 3.0);
            cairo_move_to(cr, bx + 3.0, by - 3.0);
            cairo_line_to(cr, bx - 3.0, by + 3.0);
            cairo_stroke(cr);
        }

        // Summary & body
        PangoLayout *layout = pango_cairo_create_layout(cr);
        PangoFontDescription *desc = pango_font_description_from_string(NOTIF_FONT " Bold 11");
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, hi->summary[0] ? hi->summary : hi->app_name, -1);
        pango_layout_set_width(layout, (int)((cw - 32.0) * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96 * a);
        cairo_move_to(cr, cx + 16.0, cy + 29.0);
        pango_cairo_show_layout(cr, layout);
        pango_font_description_free(desc);
        g_object_unref(layout);

        if (hi->body[0]) {
            PangoLayout *bl = pango_cairo_create_layout(cr);
            PangoFontDescription *bdesc = pango_font_description_from_string(NOTIF_FONT " 10");
            pango_layout_set_font_description(bl, bdesc);
            pango_layout_set_text(bl, hi->body, -1);
            pango_layout_set_width(bl, (int)((cw - 32.0) * PANGO_SCALE));
            pango_layout_set_height(bl, (int)((ch - 50.0) * PANGO_SCALE));
            pango_layout_set_wrap(bl, PANGO_WRAP_WORD_CHAR);
            pango_layout_set_ellipsize(bl, PANGO_ELLIPSIZE_END);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.78 * a);
            cairo_move_to(cr, cx + 16.0, cy + 48.0);
            pango_cairo_show_layout(cr, bl);
            pango_font_description_free(bdesc);
            g_object_unref(bl);
        }
    }

    cairo_restore(cr);

    cairo_buffer_apply_to_scene(notif->nc_buffer, notif->nc_scene_buffer);
    nc_update_scene_nodes(notif);
}

static void nc_history_push(aurade_notifications_t *notif, const notification_item_t *item) {
    if (!notif || !item) return;
    if (notif->history_count >= NOTIF_HISTORY_MAX) {
        if (notif->history[NOTIF_HISTORY_MAX - 1].icon_surface) {
            cairo_surface_destroy(notif->history[NOTIF_HISTORY_MAX - 1].icon_surface);
            notif->history[NOTIF_HISTORY_MAX - 1].icon_surface = NULL;
        }
        notif->history_count = NOTIF_HISTORY_MAX - 1;
    }
    for (int i = notif->history_count; i > 0; i--) {
        notif->history[i] = notif->history[i - 1];
    }
    notif->history_count++;

    notif_history_item_t *hi = &notif->history[0];
    memset(hi, 0, sizeof(*hi));
    hi->id = item->id;
    snprintf(hi->app_name, sizeof(hi->app_name), "%s", item->app_name);
    snprintf(hi->app_icon, sizeof(hi->app_icon), "%s", item->app_icon);
    snprintf(hi->summary, sizeof(hi->summary), "%s", item->summary);
    snprintf(hi->body, sizeof(hi->body), "%s", item->body);
    if (item->icon_surface) {
        hi->icon_surface = cairo_surface_reference(item->icon_surface);
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(hi->time_str, sizeof(hi->time_str), "%H:%M", &tm);
}

static void notifications_add(aurade_notifications_t *notif, const notif_cmd_t *cmd) {
    if (!notif) return;

    // Check if replacing an existing notification
    int target_idx = -1;
    if (cmd->replaces_id > 0) {
        for (int i = 0; i < notif->count; i++) {
            if (notif->items[i].id == cmd->replaces_id) {
                target_idx = i;
                break;
            }
        }
    }

    // If not replacing and full, dismiss oldest
    if (target_idx < 0) {
        if (notif->count >= NOTIF_MAX_VISIBLE) {
            emit_signal_closed(notif, notif->items[0].id, 1);
            if (notif->items[0].icon_surface) {
                cairo_surface_destroy(notif->items[0].icon_surface);
                notif->items[0].icon_surface = NULL;
            }
            for (int i = 0; i < notif->count - 1; i++) {
                notif->items[i] = notif->items[i + 1];
            }
            notif->count--;
            memset(&notif->items[notif->count], 0, sizeof(notif->items[notif->count]));
        }
        target_idx = notif->count++;
    }

    notification_item_t *item = &notif->items[target_idx];
    if (item->icon_surface) {
        cairo_surface_destroy(item->icon_surface);
        item->icon_surface = NULL;
    }

    item->id = cmd->id;
    snprintf(item->app_name, sizeof(item->app_name), "%s", cmd->app_name[0] ? cmd->app_name : "Notification");
    snprintf(item->app_icon, sizeof(item->app_icon), "%s", cmd->app_icon);
    snprintf(item->summary, sizeof(item->summary), "%s", cmd->summary);
    snprintf(item->body, sizeof(item->body), "%s", cmd->body);

    item->icon_surface = resolve_notification_icon(notif, cmd->app_name, cmd->app_icon);
    item->slide_progress = 0.0;
    item->alpha = 0.0;
    item->timer = cmd->expire_timeout > 0 ? (cmd->expire_timeout / 1000.0) : 6.0;
    item->dismissing = false;
    item->hover = false;
    item->close_hover = false;

    nc_history_push(notif, item);

    // Play subtle audio alert ping like fluid
    app_launch_command("pw-play /usr/share/sounds/freedesktop/stereo/message.oga >/dev/null 2>&1 &");

    printf("[AuraDE Notifications] Displaying popup notification id=%u app='%s' summary='%s' (timeout=%.1fs)\n",
           item->id, item->app_name, item->summary, item->timer);

    if (notif->scene_buffer) {
        wlr_scene_node_set_enabled(&notif->scene_buffer->node, true);
        wlr_scene_node_raise_to_top(&notif->scene_buffer->node);
    }
    notifications_render(notif);
    if (notif->nc_progress > 0.002) {
        notifications_nc_render(notif);
    }
    if (notif->server) schedule_all_outputs(notif->server);
}

static void notifications_dismiss_id(aurade_notifications_t *notif, uint32_t id, uint32_t reason) {
    if (!notif) return;
    for (int i = 0; i < notif->count; i++) {
        if (notif->items[i].id == id) {
            notif->items[i].dismissing = true;
            emit_signal_closed(notif, id, reason);
            break;
        }
    }
}

static int on_notif_pipe_readable(int fd, uint32_t mask, void *data) {
    (void)mask;
    aurade_notifications_t *notif = data;
    notif_cmd_t cmd;
    while (read(fd, &cmd, sizeof(cmd)) == sizeof(cmd)) {
        if (cmd.type == NOTIF_CMD_NOTIFY) {
            notifications_add(notif, &cmd);
        } else if (cmd.type == NOTIF_CMD_CLOSE) {
            notifications_dismiss_id(notif, cmd.id, 3);
        }
    }
    return 0;
}

aurade_notifications_t *notifications_create(aurade_server_t *server) {
    aurade_notifications_t *notif = calloc(1, sizeof(*notif));
    if (!notif) return NULL;

    notif->server = server;
    notif->screen_width = server->screen_width > 0 ? server->screen_width : 1920;
    notif->screen_height = server->screen_height > 0 ? server->screen_height : 1080;
    notif->next_id = 1;
    notif->hovered_index = -1;

    notif->buffer = cairo_buffer_create(NOTIF_BUFFER_W, NOTIF_BUFFER_H);
    notif->scene_buffer = wlr_scene_buffer_create(server->overlay_tree, &notif->buffer->base);

    int pos_x = notif->screen_width - NOTIF_BUFFER_W - 8;
    int pos_y = 36;
    wlr_scene_node_set_position(&notif->scene_buffer->node, pos_x, pos_y);
    wlr_scene_node_set_enabled(&notif->scene_buffer->node, false);

    for (int i = 0; i < NOTIF_MAX_VISIBLE; i++) {
        notif->blur_nodes[i] = wlr_scene_blur_create(server->overlay_tree, (int)NOTIF_CARD_W, 80);
        if (notif->blur_nodes[i]) {
            wlr_scene_blur_set_corner_radius(notif->blur_nodes[i], 16);
            wlr_scene_node_set_enabled(&notif->blur_nodes[i]->node, false);
        }
    }

    notif->nc_panel_h = nc_compute_panel_h(notif->screen_height);
    notif->nc_buffer = cairo_buffer_create((int)NOTIF_CENTER_W, notif->nc_panel_h);
    notif->nc_scene_buffer = wlr_scene_buffer_create(server->overlay_tree, &notif->nc_buffer->base);
    if (notif->nc_scene_buffer) {
        wlr_scene_node_set_enabled(&notif->nc_scene_buffer->node, false);
    }
    notif->nc_blur_node = wlr_scene_blur_create(server->overlay_tree, (int)NOTIF_CENTER_W, notif->nc_panel_h);
    if (notif->nc_blur_node) {
        wlr_scene_blur_set_corner_radius(notif->nc_blur_node, 20);
        wlr_scene_node_set_enabled(&notif->nc_blur_node->node, false);
    }

    // Setup inter-thread pipe
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
        notif->pipe_read_fd = fds[0];
        notif->pipe_write_fd = fds[1];
        struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
        notif->event_source = wl_event_loop_add_fd(
            loop,
            notif->pipe_read_fd,
            WL_EVENT_READABLE,
            on_notif_pipe_readable,
            notif
        );
    } else {
        notif->pipe_read_fd = -1;
        notif->pipe_write_fd = -1;
    }

    // Launch background D-Bus service worker
    pthread_create(&notif->dbus_thread, NULL, notifications_dbus_worker, notif);

    return notif;
}

void notifications_destroy(aurade_notifications_t *notif) {
    if (!notif) return;

    if (notif->dbus_loop && g_main_loop_is_running(notif->dbus_loop)) {
        g_main_loop_quit(notif->dbus_loop);
        pthread_join(notif->dbus_thread, NULL);
        g_main_loop_unref(notif->dbus_loop);
    }

    if (notif->dismiss_timer) {
        wl_event_source_remove(notif->dismiss_timer);
        notif->dismiss_timer = NULL;
    }
    if (notif->event_source) {
        wl_event_source_remove(notif->event_source);
        notif->event_source = NULL;
    }
    if (notif->pipe_read_fd >= 0) close(notif->pipe_read_fd);
    if (notif->pipe_write_fd >= 0) close(notif->pipe_write_fd);

    for (int i = 0; i < notif->count; i++) {
        if (notif->items[i].icon_surface) {
            cairo_surface_destroy(notif->items[i].icon_surface);
            notif->items[i].icon_surface = NULL;
        }
    }

    for (int i = 0; i < NOTIF_MAX_VISIBLE; i++) {
        if (notif->blur_nodes[i]) {
            wlr_scene_node_destroy(&notif->blur_nodes[i]->node);
            notif->blur_nodes[i] = NULL;
        }
    }

    for (int i = 0; i < notif->history_count; i++) {
        if (notif->history[i].icon_surface) {
            cairo_surface_destroy(notif->history[i].icon_surface);
            notif->history[i].icon_surface = NULL;
        }
    }
    if (notif->nc_blur_node) {
        wlr_scene_node_destroy(&notif->nc_blur_node->node);
        notif->nc_blur_node = NULL;
    }
    if (notif->nc_buffer) {
        wlr_buffer_drop(&notif->nc_buffer->base);
    }

    if (notif->buffer) {
        wlr_buffer_drop(&notif->buffer->base);
    }
    free(notif);
}

void notifications_resize(aurade_notifications_t *notif, int width, int height) {
    if (!notif) return;
    notif->screen_width = width;
    notif->screen_height = height;

    int pos_x = width - NOTIF_BUFFER_W - 8;
    int pos_y = 36;
    if (notif->scene_buffer) {
        wlr_scene_node_set_position(&notif->scene_buffer->node, pos_x, pos_y);
    }

    int new_h = nc_compute_panel_h(height);
    if (new_h != notif->nc_panel_h && notif->nc_scene_buffer) {
        notif->nc_panel_h = new_h;
        cairo_buffer_t *nb = cairo_buffer_create((int)NOTIF_CENTER_W, new_h);
        if (nb) {
            wlr_scene_buffer_set_buffer(notif->nc_scene_buffer, &nb->base);
            if (notif->nc_buffer) wlr_buffer_drop(&notif->nc_buffer->base);
            notif->nc_buffer = nb;
        }
    }
    if (notif->nc_progress > 0.002) {
        notifications_nc_render(notif);
    }
}

bool notifications_is_animating(const aurade_notifications_t *notif) {
    if (!notif) return false;
    if (fabs(notif->nc_progress - notif->nc_target) > 0.002) return true;
    if (notif->count <= 0) return false;
    for (int i = 0; i < notif->count; i++) {
        const notification_item_t *item = &notif->items[i];
        if (item->slide_progress < 1.0 || item->alpha < 1.0) return true;
        if (item->dismissing) return true;
    }
    return false;
}

static int notifications_dismiss_timer_cb(void *data) {
    aurade_notifications_t *notif = data;
    if (!notif || notif->count <= 0) return 0;
    bool any_dismissing = false;
    for (int i = 0; i < notif->count; i++) {
        notification_item_t *item = &notif->items[i];
        if (!item->hover && !item->dismissing) {
            item->dismissing = true;
            any_dismissing = true;
        }
    }
    if (any_dismissing && notif->server) {
        schedule_all_outputs(notif->server);
    }
    return 0;
}

static void notifications_arm_dismiss_timer(aurade_notifications_t *notif) {
    if (!notif || notif->count <= 0 || !notif->server || !notif->server->wl_display) return;
    if (!notif->dismiss_timer) {
        struct wl_event_loop *loop = wl_display_get_event_loop(notif->server->wl_display);
        notif->dismiss_timer = wl_event_loop_add_timer(loop, notifications_dismiss_timer_cb, notif);
    }
    double min_timer = 9999.0;
    for (int i = 0; i < notif->count; i++) {
        if (!notif->items[i].hover && !notif->items[i].dismissing) {
            if (notif->items[i].timer < min_timer) {
                min_timer = notif->items[i].timer;
            }
        }
    }
    if (min_timer < 9000.0) {
        int delay = (int)(min_timer * 1000.0);
        if (delay < 16) delay = 16;
        wl_event_source_timer_update(notif->dismiss_timer, delay);
    } else {
        if (notif->dismiss_timer) wl_event_source_timer_update(notif->dismiss_timer, 0);
    }
}

void notifications_tick(aurade_notifications_t *notif, double dt) {
    if (!notif) return;

    if (fabs(notif->nc_progress - notif->nc_target) > 0.002) {
        double prev = notif->nc_progress;
        notif->nc_progress += (notif->nc_target - notif->nc_progress) * (1.0 - exp(-15.0 * dt));
        if (fabs(notif->nc_progress - notif->nc_target) <= 0.002) {
            notif->nc_progress = notif->nc_target;
        }
        if ((prev <= 0.05 && notif->nc_progress > 0.05) || notif->nc_progress == notif->nc_target || notif->nc_progress > 0.002) {
            notifications_nc_render(notif);
        }
        if (notif->server) schedule_all_outputs(notif->server);
    }

    if (notif->count <= 0) return;

    bool needs_redraw = false;
    for (int i = 0; i < notif->count; i++) {
        notification_item_t *item = &notif->items[i];

        if (!item->dismissing) {
            if (item->slide_progress < 1.0) {
                item->slide_progress += dt * 5.0; // Slide in smoothly in ~0.2s
                if (item->slide_progress > 1.0) item->slide_progress = 1.0;
                needs_redraw = true;
            }
            if (item->alpha < 1.0) {
                item->alpha += dt * 5.0;
                if (item->alpha > 1.0) item->alpha = 1.0;
                needs_redraw = true;
            }

            // Pause countdown when mouse hovers over the notification
            if (!item->hover) {
                item->timer -= dt;
                if (item->timer <= 0.0) {
                    item->dismissing = true;
                }
            }
        } else {
            // Dismiss animation: slide out right & fade
            item->slide_progress -= dt * 4.5;
            item->alpha -= dt * 4.5;
            needs_redraw = true;

            if (item->slide_progress <= 0.0 || item->alpha <= 0.0) {
                emit_signal_closed(notif, item->id, 1);
                if (item->icon_surface) {
                    cairo_surface_destroy(item->icon_surface);
                    item->icon_surface = NULL;
                }
                for (int j = i; j < notif->count - 1; j++) {
                    notif->items[j] = notif->items[j + 1];
                }
                notif->count--;
                memset(&notif->items[notif->count], 0, sizeof(notif->items[notif->count]));
                i--;
                continue;
            }
        }
    }

    if (notif->count > 0) {
        if (needs_redraw) {
            notifications_render(notif);
            if (notif->server) schedule_all_outputs(notif->server);
        }
        if (!notifications_is_animating(notif)) {
            notifications_arm_dismiss_timer(notif);
        }
    } else {
        if (notif->dismiss_timer) {
            wl_event_source_timer_update(notif->dismiss_timer, 0);
        }
        if (notif->scene_buffer) {
            wlr_scene_node_set_enabled(&notif->scene_buffer->node, false);
        }
        for (int i = 0; i < NOTIF_MAX_VISIBLE; i++) {
            if (notif->blur_nodes[i]) {
                wlr_scene_node_set_enabled(&notif->blur_nodes[i]->node, false);
            }
        }
    }
}

void notifications_render(aurade_notifications_t *notif) {
    if (!notif || !notif->buffer || !notif->buffer->cr) return;
    cairo_t *cr = notif->buffer->cr;
    cairo_buffer_clear(notif->buffer);

    if (notif->count <= 0) {
        for (int i = 0; i < NOTIF_MAX_VISIBLE; i++) {
            if (notif->blur_nodes[i]) {
                wlr_scene_node_set_enabled(&notif->blur_nodes[i]->node, false);
            }
        }
        if (notif->scene_buffer) {
            wlr_scene_node_set_enabled(&notif->scene_buffer->node, false);
        }
        cairo_buffer_apply_to_scene(notif->buffer, notif->scene_buffer);
        return;
    }

    int pos_x = notif->screen_width - NOTIF_BUFFER_W - 8;
    int pos_y = 36;
    glass_element_t gn = notif->server ? notif->server->config.glass_notifications : (glass_element_t){ 0.88, 1.0, true };
    bool blur_enabled = (gn.blur > 0.01 || gn.liquid_glass);

    double cur_y = 10.0;
    for (int i = 0; i < notif->count; i++) {
        notification_item_t *item = &notif->items[i];

        double slide_off = (1.0 - item->slide_progress) * 360.0;
        double cx = NOTIF_PAD_X + slide_off;
        double cy = cur_y;
        double cw = NOTIF_CARD_W;

        // Determine height based on body text length
        double ch = 76.0;
        if (item->body[0] != '\0') {
            ch = strlen(item->body) > 60 ? 94.0 : 82.0;
        }
        item->card_y = cy;
        item->card_h = ch;

        double a = item->alpha;
        if (a < 0.0) a = 0.0;
        if (a > 1.0) a = 1.0;

        if (i < NOTIF_MAX_VISIBLE && notif->blur_nodes[i]) {
            if (blur_enabled && a > 0.05 && item->slide_progress > 0.05) {
                int bx = pos_x + (int)lround(cx);
                int by = pos_y + (int)lround(cy);
                int bw = (int)lround(cw);
                int bh = (int)lround(ch);
                wlr_scene_blur_set_size(notif->blur_nodes[i], bw, bh);
                wlr_scene_blur_set_corner_radius(notif->blur_nodes[i], 16);
                wlr_scene_node_set_position(&notif->blur_nodes[i]->node, bx, by);
                wlr_scene_blur_set_alpha(notif->blur_nodes[i], (float)a);
                wlr_scene_blur_set_liquid_glass(notif->blur_nodes[i], gn.liquid_glass);
                wlr_scene_blur_set_strength(notif->blur_nodes[i], (float)gn.blur);
                wlr_scene_node_set_enabled(&notif->blur_nodes[i]->node, true);
                wlr_scene_node_place_below(&notif->blur_nodes[i]->node, &notif->scene_buffer->node);
            } else {
                wlr_scene_node_set_enabled(&notif->blur_nodes[i]->node, false);
            }
        }

        cairo_save(cr);

        // Multi-layered soft drop shadow
        draw_drop_shadow(cr, cx, cy, cw, ch, 16.0, 14.0, 0.45 * a);

        // Frosted glass panel with specular top rim
        draw_glass_panel(cr, cx, cy, cw, ch, 16.0, 0.12, 0.13, 0.17, gn.opacity * a);

        // --- Header row: Small App Icon + App Name + Timestamp + Close button ---
        double header_icon_cx = cx + 22.0;
        double header_icon_cy = cy + 17.0;
        double header_icon_size = 18.0;

        bool is_telegram = strcasestr(item->app_name, "telegram") != NULL ||
                           strcasestr(item->app_icon, "telegram") != NULL;

        if (is_telegram) {
            draw_telegram_paper_plane(cr, header_icon_cx, header_icon_cy, header_icon_size);
        } else if (item->icon_surface) {
            int iw = cairo_image_surface_get_width(item->icon_surface);
            int ih = cairo_image_surface_get_height(item->icon_surface);
            if (iw > 0 && ih > 0) {
                cairo_save(cr);
                draw_rounded_rect(cr, header_icon_cx - header_icon_size / 2.0,
                                  header_icon_cy - header_icon_size / 2.0,
                                  header_icon_size, header_icon_size, 4.0);
                cairo_clip(cr);
                cairo_translate(cr, header_icon_cx - header_icon_size / 2.0,
                                header_icon_cy - header_icon_size / 2.0);
                cairo_scale(cr, header_icon_size / iw, header_icon_size / ih);
                cairo_set_source_surface(cr, item->icon_surface, 0, 0);
                cairo_paint_with_alpha(cr, a);
                cairo_restore(cr);
            }
        } else {
            // Generic squircle app badge
            draw_rounded_rect(cr, header_icon_cx - header_icon_size / 2.0,
                              header_icon_cy - header_icon_size / 2.0,
                              header_icon_size, header_icon_size, 4.5);
            cairo_set_source_rgba(cr, 0.32, 0.48, 0.86, 0.90 * a);
            cairo_fill(cr);
        }

        // App Name (Header)
        draw_text(cr, item->app_name, NOTIF_FONT, 11.5, true,
                  0.88, 0.90, 0.95, 0.92 * a,
                  cx + 38.0, cy + 17.0, false, true);

        // "now" relative time
        draw_text(cr, "now", NOTIF_FONT, 10.5, false,
                  0.65, 0.68, 0.75, 0.75 * a,
                  cx + cw - 36.0, cy + 17.0, true, true);

        // Close button '✕'
        double close_x = cx + cw - 18.0;
        double close_y = cy + 17.0;
        if (item->close_hover) {
            cairo_arc(cr, close_x, close_y, 9.0, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22 * a);
            cairo_fill(cr);
        }
        draw_text(cr, "✕", NOTIF_FONT, 9.0, true,
                  1.0, 1.0, 1.0, (item->close_hover ? 1.0 : 0.65) * a,
                  close_x, close_y, true, true);

        // --- Content Row: Avatar/Large Icon + Summary + Body ---
        double avatar_cx = cx + 32.0;
        double avatar_cy = cy + 48.0;
        double avatar_sz = 34.0;

        if (item->icon_surface) {
            int iw = cairo_image_surface_get_width(item->icon_surface);
            int ih = cairo_image_surface_get_height(item->icon_surface);
            if (iw > 0 && ih > 0) {
                cairo_save(cr);
                draw_rounded_rect(cr, avatar_cx - avatar_sz / 2.0,
                                  avatar_cy - avatar_sz / 2.0,
                                  avatar_sz, avatar_sz, 8.0);
                cairo_clip(cr);
                cairo_translate(cr, avatar_cx - avatar_sz / 2.0,
                                avatar_cy - avatar_sz / 2.0);
                cairo_scale(cr, avatar_sz / iw, avatar_sz / ih);
                cairo_set_source_surface(cr, item->icon_surface, 0, 0);
                cairo_paint_with_alpha(cr, a);
                cairo_restore(cr);
            }
        } else if (is_telegram) {
            draw_telegram_paper_plane(cr, avatar_cx, avatar_cy, avatar_sz);
        } else {
            draw_rounded_rect(cr, avatar_cx - avatar_sz / 2.0,
                              avatar_cy - avatar_sz / 2.0,
                              avatar_sz, avatar_sz, 8.0);
            cairo_set_source_rgba(cr, 0.22, 0.24, 0.32, 0.95 * a);
            cairo_fill(cr);
            char initial[2] = { item->summary[0] ? item->summary[0] : (item->app_name[0] ? item->app_name[0] : 'N'), '\0' };
            draw_text(cr, initial, NOTIF_FONT, 15.0, true,
                      1.0, 1.0, 1.0, 0.95 * a, avatar_cx, avatar_cy, true, true);
        }

        // Summary (Title / Sender)
        double text_x = cx + 58.0;
        draw_text(cr, item->summary[0] ? item->summary : item->app_name,
                  NOTIF_FONT, 13.0, true,
                  1.0, 1.0, 1.0, 0.98 * a,
                  text_x, cy + 39.0, false, true);

        // Body message text
        if (item->body[0] != '\0') {
            PangoLayout *layout = pango_cairo_create_layout(cr);
            PangoFontDescription *desc = pango_font_description_new();
            pango_font_description_set_family(desc, NOTIF_FONT);
            pango_font_description_set_absolute_size(desc, 11.5 * PANGO_SCALE);
            pango_font_description_set_weight(desc, PANGO_WEIGHT_NORMAL);
            pango_layout_set_font_description(layout, desc);
            pango_layout_set_width(layout, (cw - 72.0) * PANGO_SCALE);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
            pango_layout_set_height(layout, 38 * PANGO_SCALE);
            pango_layout_set_text(layout, item->body, -1);

            cairo_set_source_rgba(cr, 0.88, 0.90, 0.94, 0.88 * a);
            cairo_move_to(cr, text_x, cy + 49.0);
            pango_cairo_show_layout(cr, layout);

            pango_font_description_free(desc);
            g_object_unref(layout);
        }

        cairo_restore(cr);

        cur_y += ch + NOTIF_GAP;
        if (cur_y > NOTIF_BUFFER_H - 90.0) break;
    }

    for (int i = notif->count; i < NOTIF_MAX_VISIBLE; i++) {
        if (notif->blur_nodes[i]) {
            wlr_scene_node_set_enabled(&notif->blur_nodes[i]->node, false);
        }
    }

    cairo_buffer_apply_to_scene(notif->buffer, notif->scene_buffer);
}

bool notifications_pointer_owns(aurade_notifications_t *notif, double x, double y) {
    if (!notif) return false;
    if (notifications_nc_pointer_owns(notif, x, y)) return true;
    if (notif->count <= 0) return false;

    int screen_x = notif->screen_width - NOTIF_BUFFER_W - 8;
    int screen_y = 36;

    for (int i = 0; i < notif->count; i++) {
        notification_item_t *item = &notif->items[i];
        if (item->alpha < 0.1) continue;

        double card_sx = screen_x + NOTIF_PAD_X + (1.0 - item->slide_progress) * 360.0;
        double card_sy = screen_y + item->card_y;
        double card_w = NOTIF_CARD_W;
        double card_h = item->card_h > 0 ? item->card_h : 76.0;

        if (x >= card_sx && x <= card_sx + card_w &&
            y >= card_sy && y <= card_sy + card_h) {
            return true;
        }
    }
    return false;
}

static bool nc_handle_motion(aurade_notifications_t *notif, double mouse_x, double mouse_y) {
    if (!notif || notif->nc_progress <= 0.05) return false;

    double px, py, pw, ph;
    nc_panel_rect(notif, &px, &py, &pw, &ph);
    bool over_panel = (mouse_x >= px && mouse_x <= px + pw && mouse_y >= py && mouse_y <= py + ph);
    double lx = mouse_x - px;
    double ly = mouse_y - py;

    bool changed = false;
    bool new_clear_hover = false;
    if (over_panel && notif->history_count > 0) {
        double btn_w = 70.0, btn_h = 24.0;
        double btn_x = pw - btn_w - 16.0, btn_y = 15.0;
        if (lx >= btn_x && lx <= btn_x + btn_w && ly >= btn_y && ly <= btn_y + btn_h) {
            new_clear_hover = true;
        }
    }
    if (new_clear_hover != notif->nc_clear_hover) {
        notif->nc_clear_hover = new_clear_hover;
        changed = true;
    }

    double list_top = 58.0;
    double list_h = ph - list_top - 14.0;
    for (int i = 0; i < notif->history_count; i++) {
        notif_history_item_t *hi = &notif->history[i];
        double cx = 14.0;
        double cy = list_top + hi->card_y - notif->nc_scroll_y;
        double cw = pw - 28.0;
        double ch = hi->card_h;

        bool was_h = hi->hover;
        bool was_ch = hi->close_hover;
        bool in_list = over_panel && ly >= list_top && ly <= list_top + list_h;
        hi->hover = in_list && (lx >= cx && lx <= cx + cw && ly >= cy && ly <= cy + ch);
        double bx = cx + cw - 16.0;
        double by = cy + 16.0;
        hi->close_hover = in_list && (hypot(lx - bx, ly - by) <= 12.0);
        if (hi->hover != was_h || hi->close_hover != was_ch) changed = true;
    }

    if (changed) {
        notifications_nc_render(notif);
        if (notif->server) schedule_all_outputs(notif->server);
    }
    return over_panel;
}

bool notifications_handle_motion(aurade_notifications_t *notif, double mouse_x, double mouse_y) {
    if (!notif) return false;
    bool nc_owned = nc_handle_motion(notif, mouse_x, mouse_y);
    if (notif->count <= 0) return nc_owned;

    notif->mouse_x = mouse_x;
    notif->mouse_y = mouse_y;

    int screen_x = notif->screen_width - NOTIF_BUFFER_W - 8;
    int screen_y = 36;
    bool state_changed = false;

    for (int i = 0; i < notif->count; i++) {
        notification_item_t *item = &notif->items[i];
        double card_sx = screen_x + NOTIF_PAD_X + (1.0 - item->slide_progress) * 360.0;
        double card_sy = screen_y + item->card_y;
        double card_w = NOTIF_CARD_W;
        double card_h = item->card_h > 0 ? item->card_h : 76.0;

        bool was_hover = item->hover;
        bool was_close_hover = item->close_hover;

        bool is_over = (mouse_x >= card_sx && mouse_x <= card_sx + card_w &&
                        mouse_y >= card_sy && mouse_y <= card_sy + card_h);
        item->hover = is_over;

        double close_cx = card_sx + card_w - 18.0;
        double close_cy = card_sy + 17.0;
        item->close_hover = (hypot(mouse_x - close_cx, mouse_y - close_cy) <= 12.0);

        if (item->hover != was_hover || item->close_hover != was_close_hover) {
            state_changed = true;
        }
    }

    if (state_changed) {
        notifications_render(notif);
        if (notif->server) schedule_all_outputs(notif->server);
        notifications_arm_dismiss_timer(notif);
    }
    return nc_owned || notifications_pointer_owns(notif, mouse_x, mouse_y);
}

static bool app_name_matches(const char *app_id, const char *name) {
    if (!app_id || !*app_id || !name || !*name) return false;
    if (strcasestr(app_id, name) || strcasestr(name, app_id)) return true;
    if (strcasestr(name, "telegram") && (strcasestr(app_id, "telegram") || strcasestr(app_id, "org.telegram.desktop"))) return true;
    if (strcasestr(name, "browser") && (strcasestr(app_id, "firefox") || strcasestr(app_id, "chrome"))) return true;
    return false;
}

static void focus_or_launch_app(aurade_notifications_t *notif, const char *app_name) {
    if (!notif || !notif->server || !app_name || !app_name[0]) return;
    bool found = false;
    aurade_view_t *view;
    wl_list_for_each(view, &notif->server->views, link) {
        const char *app_id = view->xdg_toplevel ? view->xdg_toplevel->app_id :
            (view->xwayland_surface ? view->xwayland_surface->class : NULL);
        if (app_id && app_name_matches(app_id, app_name)) {
            view->demands_attention = false;
            view_restore(view);
            found = true;
            break;
        }
    }
    if (!found && notif->server->apps) {
        desktop_app_t *app = apps_find(notif->server->apps, app_name);
        if (app && app->exec[0]) {
            app_launch_command(app->exec);
        }
    }
}

static bool nc_handle_click(aurade_notifications_t *notif, double mouse_x, double mouse_y) {
    if (!notif || notif->nc_progress <= 0.05) return false;
    double px, py, pw, ph;
    nc_panel_rect(notif, &px, &py, &pw, &ph);
    if (mouse_x < px || mouse_x > px + pw || mouse_y < py || mouse_y > py + ph) {
        return false;
    }

    double lx = mouse_x - px;
    double ly = mouse_y - py;

    if (notif->history_count > 0) {
        double btn_w = 70.0, btn_h = 24.0;
        double btn_x = pw - btn_w - 16.0, btn_y = 15.0;
        if (lx >= btn_x && lx <= btn_x + btn_w && ly >= btn_y && ly <= btn_y + btn_h) {
            for (int i = 0; i < notif->history_count; i++) {
                if (notif->history[i].icon_surface) {
                    cairo_surface_destroy(notif->history[i].icon_surface);
                    notif->history[i].icon_surface = NULL;
                }
            }
            notif->history_count = 0;
            notif->nc_scroll_y = 0.0;
            notif->nc_clear_hover = false;
            notifications_nc_render(notif);
            if (notif->server) schedule_all_outputs(notif->server);
            return true;
        }
    }

    double list_top = 58.0;
    double list_h = ph - list_top - 14.0;
    if (ly >= list_top && ly <= list_top + list_h) {
        for (int i = 0; i < notif->history_count; i++) {
            notif_history_item_t *hi = &notif->history[i];
            double cx = 14.0;
            double cy = list_top + hi->card_y - notif->nc_scroll_y;
            double cw = pw - 28.0;
            double ch = hi->card_h;

            if (lx >= cx && lx <= cx + cw && ly >= cy && ly <= cy + ch) {
                double bx = cx + cw - 16.0;
                double by = cy + 16.0;
                if (hypot(lx - bx, ly - by) <= 14.0) {
                    if (hi->icon_surface) {
                        cairo_surface_destroy(hi->icon_surface);
                        hi->icon_surface = NULL;
                    }
                    for (int j = i; j < notif->history_count - 1; j++) {
                        notif->history[j] = notif->history[j + 1];
                    }
                    notif->history_count--;
                    notifications_nc_render(notif);
                    if (notif->server) schedule_all_outputs(notif->server);
                    return true;
                } else {
                    char app_copy[64];
                    snprintf(app_copy, sizeof(app_copy), "%s", hi->app_name);
                    notifications_nc_close(notif);
                    focus_or_launch_app(notif, app_copy);
                    return true;
                }
            }
        }
    }
    return true;
}

bool notifications_handle_click(aurade_notifications_t *notif, double mouse_x, double mouse_y) {
    if (!notif) return false;
    if (nc_handle_click(notif, mouse_x, mouse_y)) return true;
    if (notif->count <= 0) return false;

    int screen_x = notif->screen_width - NOTIF_BUFFER_W - 8;
    int screen_y = 36;

    for (int i = 0; i < notif->count; i++) {
        notification_item_t *item = &notif->items[i];
        if (item->dismissing || item->alpha < 0.1) continue;

        double card_sx = screen_x + NOTIF_PAD_X + (1.0 - item->slide_progress) * 360.0;
        double card_sy = screen_y + item->card_y;
        double card_w = NOTIF_CARD_W;
        double card_h = item->card_h > 0 ? item->card_h : 76.0;

        if (mouse_x >= card_sx && mouse_x <= card_sx + card_w &&
            mouse_y >= card_sy && mouse_y <= card_sy + card_h) {

            double close_cx = card_sx + card_w - 18.0;
            double close_cy = card_sy + 17.0;

            if (hypot(mouse_x - close_cx, mouse_y - close_cy) <= 14.0) {
                // Clicked close button
                item->dismissing = true;
                emit_signal_closed(notif, item->id, 2);
            } else {
                // Clicked notification body: invoke default action and focus/launch app
                emit_signal_action(notif, item->id, "default");
                item->dismissing = true;
                focus_or_launch_app(notif, item->app_name);
            }

            notifications_render(notif);
            if (notif->server) schedule_all_outputs(notif->server);
            return true;
        }
    }
    return false;
}

void notifications_nc_open(aurade_notifications_t *notif) {
    if (!notif) return;
    notif->nc_open = true;
    notif->nc_target = 1.0;
    if (notif->nc_progress <= 0.002) notif->nc_progress = 0.01;
    notifications_nc_render(notif);
    if (notif->server) schedule_all_outputs(notif->server);
}

void notifications_nc_close(aurade_notifications_t *notif) {
    if (!notif) return;
    notif->nc_open = false;
    notif->nc_target = 0.0;
    if (notif->server) schedule_all_outputs(notif->server);
}

void notifications_nc_toggle(aurade_notifications_t *notif) {
    if (!notif) return;
    if (notif->nc_open) notifications_nc_close(notif);
    else notifications_nc_open(notif);
}

bool notifications_nc_is_open(const aurade_notifications_t *notif) {
    return notif && (notif->nc_open || notif->nc_progress > 0.05);
}

bool notifications_nc_pointer_owns(const aurade_notifications_t *notif, double x, double y) {
    if (!notif || notif->nc_progress <= 0.05) return false;
    double px, py, pw, ph;
    nc_panel_rect(notif, &px, &py, &pw, &ph);
    return (x >= px && x <= px + pw && y >= py && y <= py + ph);
}

bool notifications_nc_handle_scroll(aurade_notifications_t *notif, double delta_y) {
    if (!notif || notif->nc_progress <= 0.05) return false;
    notif->nc_scroll_y += delta_y * 1.2;
    if (notif->nc_scroll_y < 0.0) notif->nc_scroll_y = 0.0;
    if (notif->nc_scroll_y > notif->nc_max_scroll) notif->nc_scroll_y = notif->nc_max_scroll;
    notifications_nc_render(notif);
    if (notif->server) schedule_all_outputs(notif->server);
    return true;
}
