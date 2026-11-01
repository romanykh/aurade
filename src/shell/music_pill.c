#include "music_pill.h"
#include "topbar.h"
#include "dock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define FONT_UI "Inter, Noto Sans CJK JP, Adwaita Sans, sans-serif"
#define OVERLAY_WIDTH 360.0
#define OVERLAY_HEIGHT 116.0
#define OVERLAY_RADIUS 26.0

static void music_pill_render_overlay(aurade_music_pill_t *pill);
static int music_pill_anim_tick(void *data);

static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

music_pill_pos_t music_pill_get_pos(aurade_music_pill_t *pill) {
    if (!pill || !pill->server || !pill->server->config.music_pill_enabled) {
        return MUSIC_PILL_POS_DISABLED;
    }
    const char *pos = pill->server->config.music_pill_position;
    if (!pos || !*pos || strcmp(pos, "topbar_center") == 0 || strcmp(pos, "topbar") == 0) {
        return MUSIC_PILL_POS_TOPBAR_CENTER;
    }
    if (strcmp(pos, "dock_left") == 0 || strcmp(pos, "left") == 0) {
        return MUSIC_PILL_POS_DOCK_LEFT;
    }
    if (strcmp(pos, "dock_right") == 0 || strcmp(pos, "right") == 0) {
        return MUSIC_PILL_POS_DOCK_RIGHT;
    }
    if (strcmp(pos, "disabled") == 0 || strcmp(pos, "none") == 0 || strcmp(pos, "off") == 0) {
        return MUSIC_PILL_POS_DISABLED;
    }
    return MUSIC_PILL_POS_TOPBAR_CENTER;
}

// Format microseconds to M:SS
static void format_time(int64_t us, char *buf, size_t sz) {
    if (us < 0) us = 0;
    int total_sec = (int)(us / 1000000);
    int m = total_sec / 60;
    int s = total_sec % 60;
    snprintf(buf, sz, "%d:%02d", m, s);
}

// Draw a stylized music note icon
static void draw_music_note(cairo_t *cr, double cx, double cy, double size, double r, double g, double b, double a) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, size * 0.14);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    double n_w = size * 0.65;
    double n_h = size * 0.75;
    double left_x = cx - n_w / 2.0;
    double right_x = cx + n_w / 2.0;
    double top_y = cy - n_h / 2.0;
    double bot_y = cy + n_h / 2.0;
    double head_r = size * 0.17;

    // Two note heads
    cairo_save(cr);
    cairo_translate(cr, left_x, bot_y - head_r);
    cairo_rotate(cr, -0.35);
    cairo_scale(cr, 1.25, 0.85);
    cairo_arc(cr, 0, 0, head_r, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);

    cairo_save(cr);
    cairo_translate(cr, right_x, bot_y - head_r * 1.5);
    cairo_rotate(cr, -0.35);
    cairo_scale(cr, 1.25, 0.85);
    cairo_arc(cr, 0, 0, head_r, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);

    // Stems & Beam
    cairo_move_to(cr, left_x + head_r * 0.75, bot_y - head_r);
    cairo_line_to(cr, left_x + head_r * 0.75, top_y + size * 0.12);
    cairo_line_to(cr, right_x + head_r * 0.75, top_y);
    cairo_line_to(cr, right_x + head_r * 0.75, bot_y - head_r * 1.5);
    cairo_stroke(cr);

    cairo_set_line_width(cr, size * 0.22);
    cairo_move_to(cr, left_x + head_r * 0.75, top_y + size * 0.12);
    cairo_line_to(cr, right_x + head_r * 0.75, top_y);
    cairo_stroke(cr);

    cairo_restore(cr);
}

// Draw live animated equalizer waveform bars (soundbar green sticks)
static void draw_equalizer_waveform(cairo_t *cr, double cx, double cy, double w, double h,
                                    double phase, bool playing) {
    (void)w;
    cairo_save(cr);
    int bars = 5;
    double bar_w = 2.4;
    double gap = 2.2;
    double total_w = bars * bar_w + (bars - 1) * gap;
    double start_x = cx - total_w / 2.0;

    // Frequencies: Bass kick (strongest), low-mid, mid, mid-high, treble
    static const double freqs[5] = { 4.5, 3.2, 5.8, 2.7, 6.4 };
    static const double phase_offsets[5] = { 0.0, 1.3, 2.7, 0.8, 3.5 };
    static const double weights[5] = { 1.0, 0.85, 0.95, 0.80, 0.75 };

    for (int i = 0; i < bars; i++) {
        double bx = start_x + i * (bar_w + gap);
        double bh;
        if (playing) {
            double beat = sin(phase * 4.0);
            if (beat < 0) beat = 0; // kick thump
            double wave = 0.5 + 0.5 * sin(phase * freqs[i] + phase_offsets[i]);
            double norm = 0.4 * beat + 0.6 * wave;
            bh = 3.5 + norm * (h - 4.5) * weights[i];
        } else {
            bh = 2.5; // Smooth idle indicator when paused
        }
        if (bh < 2.5) bh = 2.5;
        if (bh > h) bh = h;
        double by = cy + (h / 2.0) - bh / 2.0;

        // Vivid Apple Music iOS green
        cairo_set_source_rgba(cr, 0.18, 0.88, 0.38, playing ? 1.0 : 0.65);
        draw_rounded_rect(cr, bx, by, bar_w, bh, bar_w / 2.0);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

// Media player control icon buttons (⏮, ▶, ⏸, ⏭)
static void draw_icon_button(cairo_t *cr, double cx, double cy, double r,
                             int icon_type, bool hovered) {
    cairo_save(cr);
    if (hovered) {
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22);
        cairo_fill(cr);
    }

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, hovered ? 1.0 : 0.88);

    if (icon_type == 1) {
        // Previous (⏮)
        double sz = r * 0.72;
        cairo_rectangle(cr, cx - sz * 0.75, cy - sz * 0.65, sz * 0.22, sz * 1.3);
        cairo_fill(cr);
        cairo_move_to(cr, cx + sz * 0.65, cy - sz * 0.65);
        cairo_line_to(cr, cx - sz * 0.35, cy);
        cairo_line_to(cr, cx + sz * 0.65, cy + sz * 0.65);
        cairo_close_path(cr);
        cairo_fill(cr);
    } else if (icon_type == 2) {
        // Play (▶)
        double sz = r * 0.78;
        cairo_move_to(cr, cx - sz * 0.50, cy - sz * 0.65);
        cairo_line_to(cr, cx + sz * 0.65, cy);
        cairo_line_to(cr, cx - sz * 0.50, cy + sz * 0.65);
        cairo_close_path(cr);
        cairo_fill(cr);
    } else if (icon_type == 3) {
        // Pause (⏸)
        double sz = r * 0.72;
        double bw = sz * 0.32;
        double gap = sz * 0.24;
        draw_rounded_rect(cr, cx - gap / 2.0 - bw, cy - sz * 0.65, bw, sz * 1.3, 1.5);
        cairo_fill(cr);
        draw_rounded_rect(cr, cx + gap / 2.0, cy - sz * 0.65, bw, sz * 1.3, 1.5);
        cairo_fill(cr);
    } else if (icon_type == 4) {
        // Next (⏭)
        double sz = r * 0.72;
        cairo_move_to(cr, cx - sz * 0.65, cy - sz * 0.65);
        cairo_line_to(cr, cx + sz * 0.35, cy);
        cairo_line_to(cr, cx - sz * 0.65, cy + sz * 0.65);
        cairo_close_path(cr);
        cairo_fill(cr);
        cairo_rectangle(cr, cx + sz * 0.53, cy - sz * 0.65, sz * 0.22, sz * 1.3);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

// Draw the track progress / sound seek bar
static void draw_seek_bar(cairo_t *cr, double x, double y, double w, double h,
                          double progress, int64_t pos_us, int64_t len_us, bool hovered) {
    (void)h;
    cairo_save(cr);

    // Left elapsed time
    char elapsed_str[32];
    format_time(pos_us, elapsed_str, sizeof(elapsed_str));
    draw_text(cr, elapsed_str, FONT_UI, 10.0, false, 1.0, 1.0, 1.0, 0.75, x, y, false, true);

    // Right remaining time
    char remain_str[32];
    if (len_us > 0) {
        format_time(len_us, remain_str, sizeof(remain_str));
    } else {
        snprintf(remain_str, sizeof(remain_str), "--:--");
    }
    double r_w = text_width(cr, remain_str, FONT_UI, 10.0, false);
    draw_text(cr, remain_str, FONT_UI, 10.0, false, 1.0, 1.0, 1.0, 0.75, x + w - r_w, y, false, true);

    // Middle seek track
    double time_pad = 32.0;
    double track_x = x + time_pad;
    double track_w = w - time_pad * 2.0;
    double track_h = hovered ? 5.0 : 3.5;
    double track_y = y - track_h / 2.0;
    double track_r = track_h / 2.0;

    // Track background
    draw_rounded_rect(cr, track_x, track_y, track_w, track_h, track_r);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
    cairo_fill(cr);

    if (progress < 0.0) progress = 0.0;
    if (progress > 1.0) progress = 1.0;

    // Filled portion
    double fill_w = track_w * progress;
    if (fill_w > 0.0) {
        draw_rounded_rect(cr, track_x, track_y, fill_w, track_h, track_r);
        cairo_pattern_t *pat = cairo_pattern_create_linear(track_x, track_y, track_x + fill_w, track_y);
        cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.18, 0.88, 0.38, 0.95);
        cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.28, 0.95, 0.48, 1.0);
        cairo_set_source(cr, pat);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
    }

    // Scrubber handle dot
    double knob_x = track_x + fill_w;
    double knob_y = y;
    double knob_r = hovered ? 5.5 : 4.0;
    cairo_arc(cr, knob_x, knob_y, knob_r, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_fill(cr);

    cairo_restore(cr);
}

// Convert GdkPixbuf to cairo surface
static cairo_surface_t *pixbuf_to_cairo_surface(GdkPixbuf *pb) {
    if (!pb) return NULL;
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);
    int channels = gdk_pixbuf_get_n_channels(pb);
    guchar *pixels = gdk_pixbuf_get_pixels(pb);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_surface_flush(surf);
    unsigned char *cdata = cairo_image_surface_get_data(surf);
    int cstride = cairo_image_surface_get_stride(surf);

    for (int y = 0; y < h; y++) {
        guchar *src = pixels + y * stride;
        uint32_t *dst = (uint32_t *)(cdata + y * cstride);
        for (int x = 0; x < w; x++) {
            guchar r = src[0];
            guchar g = src[1];
            guchar b = src[2];
            guchar a = (channels == 4) ? src[3] : 255;
            uint32_t pr = (r * a + 127) / 255;
            uint32_t pg = (g * a + 127) / 255;
            uint32_t pb_b = (b * a + 127) / 255;
            dst[x] = ((uint32_t)a << 24) | (pr << 16) | (pg << 8) | pb_b;
            src += channels;
        }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

// Load artwork from URI/path into cairo surface
static void music_pill_load_artwork(aurade_music_pill_t *pill) {
    if (!pill->art_url[0]) {
        if (pill->art_surface) {
            cairo_surface_destroy(pill->art_surface);
            pill->art_surface = NULL;
        }
        pill->loaded_art_path[0] = '\0';
        return;
    }

    const char *filepath = pill->art_url;
    if (strncmp(filepath, "file://", 7) == 0) {
        filepath += 7;
    }

    if (strcmp(pill->loaded_art_path, filepath) == 0 && pill->art_surface) {
        return; // already loaded
    }

    if (pill->art_surface) {
        cairo_surface_destroy(pill->art_surface);
        pill->art_surface = NULL;
    }
    pill->loaded_art_path[0] = '\0';

    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(filepath, 120, 120, TRUE, &err);
    if (!pb) {
        if (err) g_error_free(err);
        return;
    }

    cairo_surface_t *surf = pixbuf_to_cairo_surface(pb);
    g_object_unref(pb);

    pill->art_surface = surf;
    snprintf(pill->loaded_art_path, sizeof(pill->loaded_art_path), "%s", filepath);
}

// Send MPRIS player method call ("PlayPause", "Previous", "Next")
static void music_pill_send_cmd(aurade_music_pill_t *pill, const char *cmd) {
    if (!pill || !pill->dbus_conn || !pill->player_bus_name[0]) return;
    g_dbus_connection_call(pill->dbus_conn,
                           pill->player_bus_name,
                           "/org/mpris/MediaPlayer2",
                           "org.mpris.MediaPlayer2.Player",
                           cmd,
                           NULL,
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           500,
                           NULL,
                           NULL,
                           NULL);

    if (strcmp(cmd, "PlayPause") == 0) {
        pill->playing = !pill->playing;
        pill->last_pos_update_ms = get_time_ms();
    }

    music_pill_update(pill);
    if (pill->anim_timer) {
        wl_event_source_timer_update(pill->anim_timer, 16);
    }
    if (pill->server) {
        if (pill->server->topbar) topbar_render(pill->server->topbar);
        if (pill->server->dock) dock_render(pill->server->dock);
        schedule_all_outputs(pill->server);
    }
}

// Seek to a specific progress (0.0 to 1.0)
static void music_pill_seek_to(aurade_music_pill_t *pill, double progress) {
    if (!pill || !pill->dbus_conn || !pill->player_bus_name[0] || pill->track_length <= 0) return;
    if (progress < 0.0) progress = 0.0;
    if (progress > 1.0) progress = 1.0;

    int64_t target_us = (int64_t)(progress * (double)pill->track_length);
    pill->track_position = target_us;
    pill->last_pos_update_ms = get_time_ms();

    const char *tid = pill->track_id[0] ? pill->track_id : "/org/mpris/MediaPlayer2/track";
    g_dbus_connection_call(pill->dbus_conn,
                           pill->player_bus_name,
                           "/org/mpris/MediaPlayer2",
                           "org.mpris.MediaPlayer2.Player",
                           "SetPosition",
                           g_variant_new("(ox)", tid, target_us),
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           500,
                           NULL,
                           NULL,
                           NULL);

    if (pill->overlay_open) {
        music_pill_render_overlay(pill);
    }
    if (pill->anim_timer) {
        wl_event_source_timer_update(pill->anim_timer, 16);
    }
    if (pill->server) schedule_all_outputs(pill->server);
}

// D-Bus signal handler for instant updates
static void on_mpris_signal(GDBusConnection *conn, const gchar *sender_name,
                            const gchar *object_path, const gchar *interface_name,
                            const gchar *signal_name, GVariant *parameters,
                            gpointer user_data) {
    (void)conn; (void)sender_name; (void)object_path;
    (void)interface_name; (void)signal_name; (void)parameters;
    aurade_music_pill_t *pill = user_data;
    if (!pill) return;
    music_pill_update(pill);
    if (pill->anim_timer) {
        wl_event_source_timer_update(pill->anim_timer, 16);
    }
    if (pill->server) {
        if (pill->server->topbar) topbar_render(pill->server->topbar);
        if (pill->server->dock) dock_render(pill->server->dock);
        schedule_all_outputs(pill->server);
    }
}

// Query single MPRIS player via GetAll on org.mpris.MediaPlayer2.Player
static bool music_pill_query_player(aurade_music_pill_t *pill, const char *name) {
    if (!pill || !pill->dbus_conn || !name || !*name) return false;

    GVariant *res = g_dbus_connection_call_sync(pill->dbus_conn,
        name,
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "GetAll",
        g_variant_new("(s)", "org.mpris.MediaPlayer2.Player"),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        80,
        NULL,
        NULL);
    if (!res) return false;

    GVariant *dict_v = NULL;
    g_variant_get(res, "(@a{sv})", &dict_v);
    if (!dict_v) {
        g_variant_unref(res);
        return false;
    }

    GVariantDict dict;
    g_variant_dict_init(&dict, dict_v);

    GVariant *st_var = g_variant_dict_lookup_value(&dict, "PlaybackStatus", G_VARIANT_TYPE_STRING);
    const char *status = st_var ? g_variant_get_string(st_var, NULL) : "";

    GVariant *pos_var = g_variant_dict_lookup_value(&dict, "Position", G_VARIANT_TYPE_INT64);
    int64_t cur_pos = pos_var ? g_variant_get_int64(pos_var) : 0;

    GVariant *meta_var = g_variant_dict_lookup_value(&dict, "Metadata", G_VARIANT_TYPE("a{sv}"));
    bool success = false;

    if (meta_var) {
        GVariantDict meta_dict;
        g_variant_dict_init(&meta_dict, meta_var);

        GVariant *t = g_variant_dict_lookup_value(&meta_dict, "xesam:title", G_VARIANT_TYPE_STRING);
        GVariant *art = g_variant_dict_lookup_value(&meta_dict, "mpris:artUrl", G_VARIANT_TYPE_STRING);
        GVariant *a = g_variant_dict_lookup_value(&meta_dict, "xesam:artist", G_VARIANT_TYPE("as"));
        GVariant *len = g_variant_dict_lookup_value(&meta_dict, "mpris:length", G_VARIANT_TYPE_INT64);
        GVariant *tid = g_variant_dict_lookup_value(&meta_dict, "mpris:trackid", G_VARIANT_TYPE_OBJECT_PATH);

        const char *title_str = t ? g_variant_get_string(t, NULL) : NULL;
        if (title_str && *title_str) {
            snprintf(pill->player_bus_name, sizeof(pill->player_bus_name), "%s", name);
            snprintf(pill->title, sizeof(pill->title), "%s", title_str);

            pill->artist[0] = '\0';
            if (a) {
                GVariantIter *aiter;
                g_variant_get(a, "as", &aiter);
                char *art_entry;
                if (g_variant_iter_loop(aiter, "s", &art_entry)) {
                    snprintf(pill->artist, sizeof(pill->artist), "%s", art_entry);
                }
                g_variant_iter_free(aiter);
                g_variant_unref(a);
            }

            if (art) {
                snprintf(pill->art_url, sizeof(pill->art_url), "%s", g_variant_get_string(art, NULL));
                g_variant_unref(art);
            } else {
                pill->art_url[0] = '\0';
            }

            if (len) {
                pill->track_length = g_variant_get_int64(len);
                g_variant_unref(len);
            } else {
                pill->track_length = 0;
            }

            bool is_playing = (strcmp(status, "Playing") == 0);
            bool track_changed = (strcmp(pill->title, title_str) != 0);

            if (tid) {
                const char *new_tid = g_variant_get_string(tid, NULL);
                if (strcmp(pill->track_id, new_tid) != 0) {
                    track_changed = true;
                }
                snprintf(pill->track_id, sizeof(pill->track_id), "%s", new_tid);
                g_variant_unref(tid);
            } else {
                pill->track_id[0] = '\0';
            }

            uint64_t now_ms = get_time_ms();
            if (track_changed || !pill->active) {
                pill->track_position = cur_pos;
            } else if (is_playing) {
                pill->track_position = cur_pos;
            } else if (cur_pos < pill->track_position - 1000000) {
                // Only accept backward seeks while paused; never advance forward while paused!
                pill->track_position = cur_pos;
            }

            pill->last_pos_update_ms = now_ms;
            pill->playing = is_playing;
            pill->active = true;
            success = true;

            music_pill_load_artwork(pill);
        }

        if (t) g_variant_unref(t);
        g_variant_dict_clear(&meta_dict);
        g_variant_unref(meta_var);
    }

    if (st_var) g_variant_unref(st_var);
    if (pos_var) g_variant_unref(pos_var);
    g_variant_dict_clear(&dict);
    g_variant_unref(dict_v);
    g_variant_unref(res);

    return success;
}

// Query MPRIS players on session bus
void music_pill_update(aurade_music_pill_t *pill) {
    if (!pill) return;
    if (!pill->dbus_conn) {
        pill->dbus_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        if (!pill->dbus_conn) return;

        // Subscribe to D-Bus signals for instantaneous reactive updates
        pill->prop_change_sub_id = g_dbus_connection_signal_subscribe(pill->dbus_conn,
            NULL,
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            "/org/mpris/MediaPlayer2",
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_mpris_signal,
            pill,
            NULL);

        pill->name_change_sub_id = g_dbus_connection_signal_subscribe(pill->dbus_conn,
            "org.freedesktop.DBus",
            "org.freedesktop.DBus",
            "NameOwnerChanged",
            "/org/freedesktop/DBus",
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_mpris_signal,
            pill,
            NULL);
    }

    // Fast path: if we already have a player_bus_name, query it directly!
    if (pill->player_bus_name[0] != '\0') {
        if (music_pill_query_player(pill, pill->player_bus_name)) {
            return;
        }
        // Player disappeared or closed
        pill->player_bus_name[0] = '\0';
    }

    // Fallback path: ListNames to discover active players
    GVariant *res = g_dbus_connection_call_sync(pill->dbus_conn,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames",
        NULL,
        G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE,
        80,
        NULL,
        NULL);
    if (!res) {
        pill->active = false;
        pill->playing = false;
        return;
    }

    GVariantIter *iter;
    g_variant_get(res, "(as)", &iter);
    char *name;
    bool found_active = false;

    while (g_variant_iter_loop(iter, "s", &name)) {
        if (strncmp(name, "org.mpris.MediaPlayer2.", 23) == 0 &&
            strcmp(name, "org.mpris.MediaPlayer2.playerctld") != 0) {
            if (music_pill_query_player(pill, name)) {
                found_active = true;
                break;
            }
        }
    }

    g_variant_iter_free(iter);
    g_variant_unref(res);

    if (!found_active) {
        pill->active = false;
        pill->playing = false;
        pill->player_bus_name[0] = '\0';
    }
}

// Animation tick for waveform, spring opening morph, and display updates
static int music_pill_anim_tick(void *data) {
    aurade_music_pill_t *pill = data;
    if (!pill) return 0;

    uint64_t now_ms = get_time_ms();
    static uint64_t last_mpris_poll_ms = 0;
    // D-Bus signals (PropertiesChanged, NameOwnerChanged) update state reactively.
    // Periodic poll runs every 3 seconds to catch drift or missed events.
    if (now_ms - last_mpris_poll_ms >= 3000) {
        last_mpris_poll_ms = now_ms;
        music_pill_update(pill);
    }

    // Step spring simulation if opening or closing
    if (pill->is_animating) {
        bool active = spring_simulation_step(&pill->expand_spring, 0.016);
        pill->expand_progress = pill->expand_spring.current_pos;
        if (pill->expand_progress < 0.0) pill->expand_progress = 0.0;
        if (pill->expand_progress > 1.05) pill->expand_progress = 1.05;

        if (!active || pill->expand_spring.settled) {
            pill->is_animating = false;
            if (!pill->expanding && pill->expand_progress <= 0.02) {
                pill->overlay_open = false;
                pill->expand_progress = 0.0;
                wlr_scene_node_set_enabled(&pill->overlay_scene->node, false);
                if (pill->server && pill->server->topbar) {
                    topbar_render(pill->server->topbar);
                }
            } else if (pill->expanding) {
                pill->expand_progress = 1.0;
            }
        }
    }

    // Advance track position smoothly between queries when playing
    if (pill->playing && pill->last_pos_update_ms > 0) {
        uint64_t diff_ms = now_ms - pill->last_pos_update_ms;
        pill->track_position += (int64_t)diff_ms * 1000;
        if (pill->track_length > 0 && pill->track_position > pill->track_length) {
            pill->track_position = pill->track_length;
        }
    }
    pill->last_pos_update_ms = now_ms;

    if (pill->playing || pill->is_animating || pill->overlay_open) {
        pill->waveform_phase += 0.20;
        if (pill->waveform_phase > 2.0 * M_PI * 100.0) {
            pill->waveform_phase = 0.0;
        }

        music_pill_pos_t pos = music_pill_get_pos(pill);
        if (pos == MUSIC_PILL_POS_TOPBAR_CENTER && pill->server && pill->server->topbar) {
            topbar_render(pill->server->topbar);
        } else if ((pos == MUSIC_PILL_POS_DOCK_LEFT || pos == MUSIC_PILL_POS_DOCK_RIGHT) &&
                   pill->server && pill->server->dock) {
            dock_render(pill->server->dock);
        }

        if (pill->overlay_open) {
            music_pill_render_overlay(pill);
        }

        // Schedule hardware redraw on every frame so animation is 60fps!
        if (pill->server) {
            schedule_all_outputs(pill->server);
        }
    }

    int next_delay;
    if (pill->is_animating || pill->playing) {
        next_delay = 16;
    } else if (pill->overlay_open) {
        next_delay = 100;
    } else {
        next_delay = 1000;
    }
    if (pill->anim_timer) {
        wl_event_source_timer_update(pill->anim_timer, next_delay);
    }
    return 0;
}

aurade_music_pill_t *music_pill_create(aurade_server_t *server) {
    aurade_music_pill_t *pill = calloc(1, sizeof(*pill));
    pill->server = server;
    pill->available = true;
    pill->hover_control = 0;
    pill->start_pill_w = 180.0;
    pill->start_pill_h = 21.0;

    // Create overlay buffer & scene on overlay_tree (above desktop & windows)
    pill->overlay_buffer = cairo_buffer_create((int)OVERLAY_WIDTH, (int)OVERLAY_HEIGHT);
    pill->overlay_scene = wlr_scene_buffer_create(server->overlay_tree, &pill->overlay_buffer->base);
    pill->overlay_open = false;
    wlr_scene_node_set_enabled(&pill->overlay_scene->node, false);

    // Initial update
    music_pill_update(pill);

    // Animation timer on Wayland event loop
    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    pill->anim_timer = wl_event_loop_add_timer(loop, music_pill_anim_tick, pill);
    if (pill->anim_timer) {
        wl_event_source_timer_update(pill->anim_timer, 16);
    }

    return pill;
}

void music_pill_destroy(aurade_music_pill_t *pill) {
    if (!pill) return;
    if (pill->anim_timer) wl_event_source_remove(pill->anim_timer);
    if (pill->art_surface) cairo_surface_destroy(pill->art_surface);
    if (pill->overlay_buffer) wlr_buffer_drop(&pill->overlay_buffer->base);
    if (pill->dbus_conn) {
        if (pill->prop_change_sub_id) g_dbus_connection_signal_unsubscribe(pill->dbus_conn, pill->prop_change_sub_id);
        if (pill->name_change_sub_id) g_dbus_connection_signal_unsubscribe(pill->dbus_conn, pill->name_change_sub_id);
        g_object_unref(pill->dbus_conn);
    }
    free(pill);
}

// ---------------------------------------------------------------------------
// TOPBAR RENDERING (Center Dynamic Island)
// ---------------------------------------------------------------------------

void music_pill_render_topbar(aurade_music_pill_t *pill, cairo_t *cr, int topbar_w, int topbar_h) {
    if (!pill || !pill->active) {
        pill->pill_w = 0;
        pill->pill_h = 0;
        return;
    }

    double h = 21.0;
    double y = (topbar_h - h) / 2.0;

    // Compose display string: "Title  •  Artist"
    char label[256];
    if (pill->artist[0]) {
        snprintf(label, sizeof(label), "%s  •  %s", pill->title, pill->artist);
    } else {
        snprintf(label, sizeof(label), "%s", pill->title);
    }

    if (strcmp(pill->cached_label, label) != 0 || pill->cached_label_w <= 0.0) {
        snprintf(pill->cached_label, sizeof(pill->cached_label), "%s", label);
        pill->cached_label_w = text_width(cr, label, FONT_UI, 11.5, true);
    }
    double text_w = pill->cached_label_w;
    if (text_w > 180.0) text_w = 180.0;
    if (text_w < 60.0) text_w = 60.0;

    double icon_sz = 15.0;
    double wave_w = 22.0;
    double pad = 8.0;
    double w = pad + icon_sz + 7.0 + text_w + 9.0 + wave_w + pad;
    double x = (topbar_w - w) / 2.0;

    pill->pill_x = x;
    pill->pill_y = y;
    pill->pill_w = w;
    pill->pill_h = h;
    pill->start_pill_w = w;
    pill->start_pill_h = h;

    // When the Dynamic Island overlay is open (expanding/open/closing),
    // the overlay owns rendering so hide the compact topbar pill!
    if (pill->overlay_open) {
        return;
    }

    // Drop shadow
    draw_drop_shadow(cr, x, y, w, h, h / 2.0, 5.0, 0.40);

    // Jet-black Apple Dynamic Island capsule fill
    draw_rounded_rect(cr, x, y, w, h, h / 2.0);
    if (pill->hover_topbar_pill) {
        cairo_set_source_rgba(cr, 0.08, 0.08, 0.11, 0.98);
    } else {
        cairo_set_source_rgba(cr, 0.04, 0.04, 0.06, 0.94);
    }
    cairo_fill_preserve(cr);

    // Hairline glass border
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, pill->hover_topbar_pill ? 0.28 : 0.16);
    cairo_set_line_width(cr, 0.75);
    cairo_stroke(cr);

    // Left: Artwork thumbnail or colorful gradient music note
    double icon_x = x + pad;
    double icon_y = y + (h - icon_sz) / 2.0;

    if (pill->art_surface) {
        cairo_save(cr);
        draw_rounded_rect(cr, icon_x, icon_y, icon_sz, icon_sz, 3.5);
        cairo_clip(cr);
        double sw = cairo_image_surface_get_width(pill->art_surface);
        double sh = cairo_image_surface_get_height(pill->art_surface);
        cairo_translate(cr, icon_x, icon_y);
        cairo_scale(cr, icon_sz / sw, icon_sz / sh);
        cairo_set_source_surface(cr, pill->art_surface, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_save(cr);
        draw_rounded_rect(cr, icon_x, icon_y, icon_sz, icon_sz, 3.5);
        cairo_pattern_t *pat = cairo_pattern_create_linear(icon_x, icon_y, icon_x + icon_sz, icon_y + icon_sz);
        cairo_pattern_add_color_stop_rgba(pat, 0.0, 1.0, 0.20, 0.45, 1.0);
        cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.95, 0.45, 0.10, 1.0);
        cairo_set_source(cr, pat);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
        draw_music_note(cr, icon_x + icon_sz / 2.0, icon_y + icon_sz / 2.0, icon_sz * 0.75, 1.0, 1.0, 1.0, 0.95);
        cairo_restore(cr);
    }

    // Center: Track title & artist
    double text_x = icon_x + icon_sz + 7.0;
    draw_text_clamped(cr, label, FONT_UI, 11.5, true,
                      1.0, 1.0, 1.0, 0.94, text_x, y + h / 2.0, text_w);

    // Right: Animated live waveform visualizer (soundbar green sticks)
    double wave_x = text_x + text_w + 9.0 + wave_w / 2.0;
    draw_equalizer_waveform(cr, wave_x, y + (h - 11.0) / 2.0, wave_w, 11.0,
                            pill->waveform_phase, pill->playing);
}

// ---------------------------------------------------------------------------
// EXPANDED DYNAMIC ISLAND OVERLAY (Fluid Spring Morph Animation)
// ---------------------------------------------------------------------------

static void music_pill_render_overlay(aurade_music_pill_t *pill) {
    if (!pill || !pill->overlay_buffer || !pill->overlay_buffer->cr) return;
    cairo_t *cr = pill->overlay_buffer->cr;
    cairo_buffer_clear(pill->overlay_buffer);

    double p = pill->expand_progress;
    if (p < 0.0) p = 0.0;
    if (p > 1.05) p = 1.05;

    double start_w = pill->start_pill_w > 50.0 ? pill->start_pill_w : 180.0;
    double start_h = pill->start_pill_h > 15.0 ? pill->start_pill_h : 21.0;

    // Morph dimensions from compact pill -> expanded card
    double curr_w = start_w + (OVERLAY_WIDTH - start_w) * p;
    double curr_h = start_h + (OVERLAY_HEIGHT - start_h) * p;
    double curr_r = (start_h / 2.0) + (OVERLAY_RADIUS - (start_h / 2.0)) * p;

    double screen_w = (pill->server && pill->server->screen_width > 0) ?
                      (double)pill->server->screen_width : 1920.0;
    int scene_x = (int)((screen_w - OVERLAY_WIDTH) / 2.0);
    int scene_y = 3;
    wlr_scene_node_set_position(&pill->overlay_scene->node, scene_x, scene_y);

    // Inside the buffer (width OVERLAY_WIDTH, height OVERLAY_HEIGHT),
    // the capsule is centered horizontally:
    double bx = (OVERLAY_WIDTH - curr_w) / 2.0;
    double by = 0.0;

    // Global screen hit test coordinates for the capsule
    pill->overlay_x = (double)scene_x + bx;
    pill->overlay_y = (double)scene_y + by;
    pill->overlay_w = curr_w;
    pill->overlay_h = curr_h;

    // Drop shadow
    if (p > 0.05) {
        draw_drop_shadow(cr, bx + 2.0, by + 2.0, curr_w - 4.0, curr_h - 4.0, curr_r, 10.0 * p, 0.50 * p);
    }

    // Jet-black Apple Dynamic Island capsule body
    draw_rounded_rect(cr, bx, by, curr_w, curr_h, curr_r);
    cairo_set_source_rgba(cr, 0.04, 0.04, 0.06, 0.96);
    cairo_fill_preserve(cr);

    // Hairline glass border
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18 + 0.10 * p);
    cairo_set_line_width(cr, 0.85);
    cairo_stroke(cr);

    // Smooth crossfade: compact content fades out as p rises
    if (p < 0.35) {
        double compact_alpha = (0.35 - p) / 0.35;
        if (compact_alpha > 1.0) compact_alpha = 1.0;
        if (compact_alpha < 0.0) compact_alpha = 0.0;

        cairo_save(cr);
        cairo_push_group(cr);

        double pad = 8.0;
        double icon_sz = 15.0;
        double icon_x = bx + pad;
        double icon_y = by + (start_h - icon_sz) / 2.0;

        if (pill->art_surface) {
            cairo_save(cr);
            draw_rounded_rect(cr, icon_x, icon_y, icon_sz, icon_sz, 3.5);
            cairo_clip(cr);
            double sw = cairo_image_surface_get_width(pill->art_surface);
            double sh = cairo_image_surface_get_height(pill->art_surface);
            cairo_translate(cr, icon_x, icon_y);
            cairo_scale(cr, icon_sz / sw, icon_sz / sh);
            cairo_set_source_surface(cr, pill->art_surface, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        } else {
            cairo_save(cr);
            draw_rounded_rect(cr, icon_x, icon_y, icon_sz, icon_sz, 3.5);
            cairo_pattern_t *pat = cairo_pattern_create_linear(icon_x, icon_y, icon_x + icon_sz, icon_y + icon_sz);
            cairo_pattern_add_color_stop_rgba(pat, 0.0, 1.0, 0.20, 0.45, 1.0);
            cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.95, 0.45, 0.10, 1.0);
            cairo_set_source(cr, pat);
            cairo_fill(cr);
            cairo_pattern_destroy(pat);
            draw_music_note(cr, icon_x + icon_sz / 2.0, icon_y + icon_sz / 2.0, icon_sz * 0.75, 1.0, 1.0, 1.0, 0.95);
            cairo_restore(cr);
        }

        char label[256];
        if (pill->artist[0]) {
            snprintf(label, sizeof(label), "%s  •  %s", pill->title, pill->artist);
        } else {
            snprintf(label, sizeof(label), "%s", pill->title);
        }
        double text_x = icon_x + icon_sz + 7.0;
        double wave_w = 22.0;
        double text_w = curr_w - (pad * 2.0 + icon_sz + 7.0 + 9.0 + wave_w);
        if (text_w > 0.0) {
            draw_text_clamped(cr, label, FONT_UI, 11.5, true,
                              1.0, 1.0, 1.0, 0.94, text_x, by + start_h / 2.0, text_w);
        }

        double wave_x = bx + curr_w - pad - wave_w / 2.0;
        draw_equalizer_waveform(cr, wave_x, by + (start_h - 11.0) / 2.0, wave_w, 11.0,
                                pill->waveform_phase, pill->playing);

        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, compact_alpha);
        cairo_restore(cr);
    }

    // Expanded card content fades in as p rises
    if (p > 0.20) {
        double content_alpha = (p - 0.20) / 0.80;
        if (content_alpha > 1.0) content_alpha = 1.0;
        if (content_alpha < 0.0) content_alpha = 0.0;

        cairo_save(cr);
        cairo_push_group(cr);

        // Artwork on left (48x48)
        double art_x = bx + 16.0;
        double art_y = by + 14.0;
        double art_sz = 48.0;

        if (pill->art_surface) {
            cairo_save(cr);
            draw_rounded_rect(cr, art_x, art_y, art_sz, art_sz, 10.0);
            cairo_clip(cr);
            double sw = cairo_image_surface_get_width(pill->art_surface);
            double sh = cairo_image_surface_get_height(pill->art_surface);
            cairo_translate(cr, art_x, art_y);
            cairo_scale(cr, art_sz / sw, art_sz / sh);
            cairo_set_source_surface(cr, pill->art_surface, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);

            draw_rounded_rect(cr, art_x, art_y, art_sz, art_sz, 10.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        } else {
            cairo_save(cr);
            draw_rounded_rect(cr, art_x, art_y, art_sz, art_sz, 10.0);
            cairo_pattern_t *pat = cairo_pattern_create_linear(art_x, art_y, art_x + art_sz, art_y + art_sz);
            cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.95, 0.20, 0.45, 1.0);
            cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.98, 0.48, 0.15, 1.0);
            cairo_set_source(cr, pat);
            cairo_fill(cr);
            cairo_pattern_destroy(pat);
            draw_music_note(cr, art_x + art_sz / 2.0, art_y + art_sz / 2.0, art_sz * 0.65, 1.0, 1.0, 1.0, 0.96);
            cairo_restore(cr);
        }

        // Track title & artist
        double text_x = art_x + art_sz + 12.0;
        double max_text_w = 145.0;

        draw_text_clamped(cr, pill->title[0] ? pill->title : "Music", FONT_UI, 13.5, true,
                          1.0, 1.0, 1.0, 0.98, text_x, by + 22.0, max_text_w);

        draw_text_clamped(cr, pill->artist[0] ? pill->artist : "Media Player", FONT_UI, 11.0, false,
                          1.0, 1.0, 1.0, 0.65, text_x, by + 38.0, max_text_w);

        // Status badge ("● Playing" / "Paused")
        double status_y = by + 52.0;
        cairo_arc(cr, text_x + 3.0, status_y, 2.5, 0, 2 * M_PI);
        if (pill->playing) {
            cairo_set_source_rgba(cr, 0.20, 0.88, 0.40, 0.95);
        } else {
            cairo_set_source_rgba(cr, 0.70, 0.70, 0.75, 0.75);
        }
        cairo_fill(cr);

        draw_text_clamped(cr, pill->playing ? "Playing" : "Paused", FONT_UI, 9.5, true,
                          1.0, 1.0, 1.0, 0.60, text_x + 10.0, status_y, 80.0);

        // Playback control buttons on right (stored relative to capsule origin)
        double ctrl_y = 38.0;
        pill->prev_btn_x = curr_w - 110.0;
        pill->prev_btn_y = ctrl_y;
        pill->prev_btn_r = 13.0;

        pill->play_btn_x = curr_w - 74.0;
        pill->play_btn_y = ctrl_y;
        pill->play_btn_r = 16.0;

        pill->next_btn_x = curr_w - 38.0;
        pill->next_btn_y = ctrl_y;
        pill->next_btn_r = 13.0;

        draw_icon_button(cr, bx + pill->prev_btn_x, by + pill->prev_btn_y, pill->prev_btn_r,
                         1, pill->hover_control == 1);
        draw_icon_button(cr, bx + pill->play_btn_x, by + pill->play_btn_y, pill->play_btn_r,
                         pill->playing ? 3 : 2, pill->hover_control == 2);
        draw_icon_button(cr, bx + pill->next_btn_x, by + pill->next_btn_y, pill->next_btn_r,
                         4, pill->hover_control == 3);

        // Animated waveform (green sticks) on top-right
        draw_equalizer_waveform(cr, bx + curr_w - 38.0, by + 16.0, 24.0, 12.0,
                                pill->waveform_phase, pill->playing);

        // Bottom Seek / Progress Sound Bar (stored relative to capsule origin)
        double sb_x = 16.0;
        double sb_y = 80.0;
        double sb_w = curr_w - 32.0;
        double sb_h = 16.0;
        pill->seek_bar_x = sb_x;
        pill->seek_bar_y = sb_y - 6.0;
        pill->seek_bar_w = sb_w;
        pill->seek_bar_h = sb_h;

        double progress = 0.0;
        if (pill->track_length > 0) {
            progress = (double)pill->track_position / (double)pill->track_length;
        }
        draw_seek_bar(cr, bx + sb_x, by + sb_y, sb_w, sb_h, progress,
                      pill->track_position, pill->track_length, pill->hover_seek_bar);

        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, content_alpha);
        cairo_restore(cr);
    }

    cairo_buffer_apply_to_scene(pill->overlay_buffer, pill->overlay_scene);
}

void music_pill_collapse(aurade_music_pill_t *pill) {
    if (!pill || !pill->overlay_open) return;
    spring_config_t spring = {
        .mass = 1.0,
        .stiffness = 360.0,
        .damping = 0.86,
        .velocity_threshold = 0.005,
        .position_threshold = 0.002,
    };
    spring_simulation_start(&pill->expand_spring, &spring, pill->expand_progress, 0.0, 0.0);
    pill->is_animating = true;
    pill->expanding = false;
    pill->hover_control = 0;
    pill->hover_seek_bar = false;
    if (pill->anim_timer) {
        wl_event_source_timer_update(pill->anim_timer, 16);
    }
    if (pill->server) schedule_all_outputs(pill->server);
}

bool music_pill_is_expanded(aurade_music_pill_t *pill) {
    return pill && pill->overlay_open;
}

bool music_pill_overlay_owns(aurade_music_pill_t *pill, double x, double y) {
    if (!pill || !pill->overlay_open) return false;
    return (x >= pill->overlay_x && x <= pill->overlay_x + pill->overlay_w &&
            y >= pill->overlay_y && y <= pill->overlay_y + pill->overlay_h);
}

bool music_pill_handle_overlay_motion(aurade_music_pill_t *pill, double x, double y) {
    if (!pill || !pill->overlay_open) return false;
    double lx = x - pill->overlay_x;
    double ly = y - pill->overlay_y;

    int hover = 0;
    if (hypot(lx - pill->prev_btn_x, ly - pill->prev_btn_y) <= pill->prev_btn_r + 2.0) {
        hover = 1;
    } else if (hypot(lx - pill->play_btn_x, ly - pill->play_btn_y) <= pill->play_btn_r + 2.0) {
        hover = 2;
    } else if (hypot(lx - pill->next_btn_x, ly - pill->next_btn_y) <= pill->next_btn_r + 2.0) {
        hover = 3;
    } else {
        double track_x = pill->seek_bar_x + 36.0;
        double track_w = pill->seek_bar_w - 72.0;
        if (lx >= track_x && lx <= track_x + track_w &&
            ly >= pill->seek_bar_y - 4.0 && ly <= pill->seek_bar_y + pill->seek_bar_h + 4.0) {
            hover = 4;
        } else if (lx >= 0 && lx <= pill->overlay_w && ly >= 0 && ly <= pill->overlay_h) {
            hover = 5;
        }
    }

    bool seek_hover = (hover == 4);
    if (hover != pill->hover_control || seek_hover != pill->hover_seek_bar) {
        pill->hover_control = hover;
        pill->hover_seek_bar = seek_hover;
        music_pill_render_overlay(pill);
        if (pill->server) schedule_all_outputs(pill->server);
    }
    return true;
}

bool music_pill_handle_overlay_click(aurade_music_pill_t *pill, double x, double y) {
    if (!pill || !pill->overlay_open) return false;
    double lx = x - pill->overlay_x;
    double ly = y - pill->overlay_y;

    if (hypot(lx - pill->prev_btn_x, ly - pill->prev_btn_y) <= pill->prev_btn_r + 4.0) {
        music_pill_send_cmd(pill, "Previous");
        return true;
    }
    if (hypot(lx - pill->play_btn_x, ly - pill->play_btn_y) <= pill->play_btn_r + 4.0) {
        music_pill_send_cmd(pill, "PlayPause");
        return true;
    }
    if (hypot(lx - pill->next_btn_x, ly - pill->next_btn_y) <= pill->next_btn_r + 4.0) {
        music_pill_send_cmd(pill, "Next");
        return true;
    }

    // Seeking click on seekbar
    double track_x = pill->seek_bar_x + 36.0;
    double track_w = pill->seek_bar_w - 72.0;
    if (lx >= track_x && lx <= track_x + track_w &&
        ly >= pill->seek_bar_y - 4.0 && ly <= pill->seek_bar_y + pill->seek_bar_h + 4.0) {
        double pr = (lx - track_x) / track_w;
        music_pill_seek_to(pill, pr);
        return true;
    }

    // Clicking elsewhere on the expanded island collapses it
    music_pill_collapse(pill);
    return true;
}

bool music_pill_handle_topbar_motion(aurade_music_pill_t *pill, double x, double y) {
    if (!pill || !pill->active || pill->pill_w <= 0) return false;
    bool inside = (x >= pill->pill_x && x <= pill->pill_x + pill->pill_w &&
                   y >= pill->pill_y && y <= pill->pill_y + pill->pill_h);

    if (inside != pill->hover_topbar_pill) {
        pill->hover_topbar_pill = inside;
        return true;
    }
    return false;
}

bool music_pill_handle_topbar_click(aurade_music_pill_t *pill, double x, double y) {
    if (!pill || !pill->active || pill->pill_w <= 0) return false;
    if (x >= pill->pill_x && x <= pill->pill_x + pill->pill_w &&
        y >= pill->pill_y && y <= pill->pill_y + pill->pill_h) {

        if (pill->overlay_open && !pill->is_animating) {
            music_pill_collapse(pill);
        } else if (!pill->overlay_open) {
            // Trigger bouncy spring opening morph animation!
            spring_config_t spring = {
                .mass = 1.0,
                .stiffness = 340.0,
                .damping = 0.86,
                .velocity_threshold = 0.005,
                .position_threshold = 0.002,
            };
            spring_simulation_start(&pill->expand_spring, &spring, 0.0, 1.0, 0.0);
            pill->is_animating = true;
            pill->expanding = true;
            pill->overlay_open = true;
            pill->expand_progress = 0.0;
            wlr_scene_node_set_enabled(&pill->overlay_scene->node, true);
            music_pill_render_overlay(pill);
            if (pill->anim_timer) {
                wl_event_source_timer_update(pill->anim_timer, 16);
            }
            if (pill->server) schedule_all_outputs(pill->server);
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// DOCK RENDERING (Dock Left / Dock Right)
// ---------------------------------------------------------------------------

void music_pill_render_dock(aurade_music_pill_t *pill, cairo_t *cr,
                            double x, double y, double w, double h) {
    if (!pill || !pill->active) return;

    pill->dock_pill_x = x;
    pill->dock_pill_y = y;
    pill->dock_pill_w = w;
    pill->dock_pill_h = h;

    double r = h / 2.0;
    if (r > 20.0) r = 20.0;

    // Drop shadow
    draw_drop_shadow(cr, x, y, w, h, r, 8.0, 0.45);

    // Jet black glass pill
    draw_rounded_rect(cr, x, y, w, h, r);
    if (pill->hover_dock_pill) {
        cairo_set_source_rgba(cr, 0.08, 0.08, 0.11, 0.98);
    } else {
        cairo_set_source_rgba(cr, 0.04, 0.04, 0.06, 0.92);
    }
    cairo_fill_preserve(cr);

    // Border
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, pill->hover_dock_pill ? 0.26 : 0.16);
    cairo_set_line_width(cr, 0.85);
    cairo_stroke(cr);

    // Left artwork icon
    double art_sz = h - 14.0;
    if (art_sz < 24.0) art_sz = 24.0;
    if (art_sz > 38.0) art_sz = 38.0;
    double art_x = x + 8.0;
    double art_y = y + (h - art_sz) / 2.0;

    if (pill->art_surface) {
        cairo_save(cr);
        draw_rounded_rect(cr, art_x, art_y, art_sz, art_sz, 6.0);
        cairo_clip(cr);
        double sw = cairo_image_surface_get_width(pill->art_surface);
        double sh = cairo_image_surface_get_height(pill->art_surface);
        cairo_translate(cr, art_x, art_y);
        cairo_scale(cr, art_sz / sw, art_sz / sh);
        cairo_set_source_surface(cr, pill->art_surface, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_save(cr);
        draw_rounded_rect(cr, art_x, art_y, art_sz, art_sz, 6.0);
        cairo_pattern_t *pat = cairo_pattern_create_linear(art_x, art_y, art_x + art_sz, art_y + art_sz);
        cairo_pattern_add_color_stop_rgba(pat, 0.0, 1.0, 0.20, 0.45, 1.0);
        cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.95, 0.45, 0.10, 1.0);
        cairo_set_source(cr, pat);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
        draw_music_note(cr, art_x + art_sz / 2.0, art_y + art_sz / 2.0, art_sz * 0.70, 1.0, 1.0, 1.0, 0.95);
        cairo_restore(cr);
    }

    if (pill->hover_dock_pill) {
        // Show interactive controls when hovered
        double btn_y = y + h / 2.0;
        double cx1 = art_x + art_sz + 18.0;
        double cx2 = cx1 + 26.0;
        double cx3 = cx2 + 26.0;

        draw_icon_button(cr, cx1, btn_y, 11.0, 1, pill->dock_hover_control == 1);
        draw_icon_button(cr, cx2, btn_y, 13.0, pill->playing ? 3 : 2, pill->dock_hover_control == 2);
        draw_icon_button(cr, cx3, btn_y, 11.0, 4, pill->dock_hover_control == 3);
    } else {
        // Track title & animated waveform
        double text_x = art_x + art_sz + 8.0;
        double max_t_w = w - (art_sz + 44.0);
        if (max_t_w > 10.0) {
            draw_text_clamped(cr, pill->title, FONT_UI, 11.5, true,
                              1.0, 1.0, 1.0, 0.96, text_x, y + h / 2.0 - 5.0, max_t_w);
            draw_text_clamped(cr, pill->artist[0] ? pill->artist : "Media", FONT_UI, 10.0, false,
                              1.0, 1.0, 1.0, 0.65, text_x, y + h / 2.0 + 8.0, max_t_w);
        }

        // Waveform on right
        double wave_x = x + w - 18.0;
        draw_equalizer_waveform(cr, wave_x, y + (h - 14.0) / 2.0, 16.0, 14.0,
                                pill->waveform_phase, pill->playing);
    }
}

bool music_pill_handle_dock_motion(aurade_music_pill_t *pill, double x, double y) {
    if (!pill || !pill->active || pill->dock_pill_w <= 0) return false;
    bool inside = (x >= pill->dock_pill_x && x <= pill->dock_pill_x + pill->dock_pill_w &&
                   y >= pill->dock_pill_y && y <= pill->dock_pill_y + pill->dock_pill_h);

    int hover_ctrl = 0;
    if (inside) {
        double lx = x - pill->dock_pill_x;
        double art_sz = pill->dock_pill_h - 14.0;
        double cx1 = 8.0 + art_sz + 18.0;
        double cx2 = cx1 + 26.0;
        double cx3 = cx2 + 26.0;
        if (fabs(lx - cx1) <= 12.0) hover_ctrl = 1;
        else if (fabs(lx - cx2) <= 14.0) hover_ctrl = 2;
        else if (fabs(lx - cx3) <= 12.0) hover_ctrl = 3;
    }

    if (inside != pill->hover_dock_pill || hover_ctrl != pill->dock_hover_control) {
        pill->hover_dock_pill = inside;
        pill->dock_hover_control = hover_ctrl;
        return true;
    }
    return false;
}

bool music_pill_handle_dock_click(aurade_music_pill_t *pill, double x, double y) {
    if (!pill || !pill->active || pill->dock_pill_w <= 0) return false;
    if (x >= pill->dock_pill_x && x <= pill->dock_pill_x + pill->dock_pill_w &&
        y >= pill->dock_pill_y && y <= pill->dock_pill_y + pill->dock_pill_h) {

        if (pill->dock_hover_control == 1) {
            music_pill_send_cmd(pill, "Previous");
        } else if (pill->dock_hover_control == 3) {
            music_pill_send_cmd(pill, "Next");
        } else {
            music_pill_send_cmd(pill, "PlayPause");
        }
        return true;
    }
    return false;
}
