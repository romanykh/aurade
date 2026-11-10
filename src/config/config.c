#include "config.h"
#include <json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

gesture_action_t action_from_string(const char *str) {
    gesture_action_t a = { .type = ACTION_NONE, .custom_cmd = "" };
    if (!str || !*str || strcmp(str, "none") == 0) return a;
    if (strcmp(str, "mission_control") == 0 || strcmp(str, "app_expose") == 0) a.type = ACTION_MISSION_CONTROL;
    else if (strcmp(str, "workspace_next") == 0 || strcmp(str, "next_space") == 0) a.type = ACTION_WORKSPACE_NEXT;
    else if (strcmp(str, "workspace_prev") == 0 || strcmp(str, "prev_space") == 0) a.type = ACTION_WORKSPACE_PREV;
    else if (strcmp(str, "launchpad") == 0) a.type = ACTION_LAUNCHPAD;
    else if (strcmp(str, "show_desktop") == 0) a.type = ACTION_SHOW_DESKTOP;
    else if (strcmp(str, "control_center") == 0) a.type = ACTION_CONTROL_CENTER;
    else if (strncmp(str, "custom:", 7) == 0) {
        a.type = ACTION_CUSTOM;
        strncpy(a.custom_cmd, str + 7, sizeof(a.custom_cmd) - 1);
    }
    return a;
}

const char *action_to_string(const gesture_action_t *action, char *buf, size_t buf_size) {
    if (!action) {
        snprintf(buf, buf_size, "none");
        return buf;
    }
    switch (action->type) {
        case ACTION_MISSION_CONTROL: snprintf(buf, buf_size, "mission_control"); break;
        case ACTION_APP_EXPOSE: snprintf(buf, buf_size, "app_expose"); break;
        case ACTION_WORKSPACE_NEXT: snprintf(buf, buf_size, "workspace_next"); break;
        case ACTION_WORKSPACE_PREV: snprintf(buf, buf_size, "workspace_prev"); break;
        case ACTION_LAUNCHPAD: snprintf(buf, buf_size, "launchpad"); break;
        case ACTION_SHOW_DESKTOP: snprintf(buf, buf_size, "show_desktop"); break;
        case ACTION_CONTROL_CENTER: snprintf(buf, buf_size, "control_center"); break;
        case ACTION_CUSTOM: snprintf(buf, buf_size, "custom:%s", action->custom_cmd); break;
        default: snprintf(buf, buf_size, "none"); break;
    }
    return buf;
}

aurade_config_t config_get_defaults(void) {
    aurade_config_t cfg = {
        // 3-finger gestures
        .swipe_up_3 = { .type = ACTION_MISSION_CONTROL },
        .swipe_down_3 = { .type = ACTION_MISSION_CONTROL },
        .swipe_left_3 = { .type = ACTION_WORKSPACE_NEXT },
        .swipe_right_3 = { .type = ACTION_WORKSPACE_PREV },
        .pinch_in_3 = { .type = ACTION_LAUNCHPAD },
        .pinch_out_3 = { .type = ACTION_SHOW_DESKTOP },

        // 4-finger gestures
        .swipe_up_4 = { .type = ACTION_MISSION_CONTROL },
        .swipe_down_4 = { .type = ACTION_MISSION_CONTROL },
        .swipe_left_4 = { .type = ACTION_WORKSPACE_NEXT },
        .swipe_right_4 = { .type = ACTION_WORKSPACE_PREV },
        .pinch_in_4 = { .type = ACTION_LAUNCHPAD },   // 4-finger pinch-in = Launchpad
        .pinch_out_4 = { .type = ACTION_LAUNCHPAD },  // 4-finger pinch-out = close Launchpad

        // Edge gestures
        .edge_swipe_right = { .type = ACTION_CONTROL_CENTER },

        // Physics
        .spring = spring_config_default(),
        .sensitivity = 1.0,
        .snap_threshold = 0.35,
        .flick_velocity_threshold = 700.0,
        .rubber_band_limit = 180.0,
        .deadzone_px = 40.0,
        .natural_scroll = true,
        .disable_while_typing = false,

        // Keyboard: auto-detect (see config.h)
        .kb_layout = "",
        .kb_variant = "",
        .kb_model = "",
        .kb_options = "",

        // Dock
        .dock_enabled = true,
        .dock_autohide = false,
        .dock_intellihide = true,
        .dock_icon_size = 52,
        .dock_magnification = true,
        .dock_max_scale = 1.5,
        .dock_mag_radius = 130.0,
        .dock_theme = DOCK_THEME_MODERN,
        .dock_opacity = 0.85,
        .dock_pinned_count = 1,
        .dock_pinned = {
            { "Launchpad", "" },
        },

        // Appearance
        .dark_mode = true,
        .wallpaper_path = "",
        .accent_color = "#007AFF",
        .window_opacity = 1.0,
        .window_rules_count = 0,

        // Music Pill / Dynamic Island
        .music_pill_enabled = true,
        .music_pill_position = "topbar_center",

        // Workspaces
        .num_workspaces = 4,
        .workspace_auto_focus = false,
        .tiling_enabled = false,
        .tiling_gap = 10,

        // Blur effects
        .blur_dock = true,
        .blur_topbar = true,
        .blur_launchpad = true,
        .blur_workspace_overview = true,
        .blur_windows = false,
        .blur_strength = 1.0,

        // Topbar
        .topbar_cpu_temp = true,
    };
    return cfg;
}

const char *config_get_default_path(void) {
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/home/user";
    snprintf(path, sizeof(path), "%s/.config/aurade/config.json", home);
    return path;
}

static void ensure_parent_dir(const char *file_path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", file_path);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(dir, 0755);
    }
}

static void read_action(struct json_object *obj, const char *key, gesture_action_t *out) {
    struct json_object *val = NULL;
    if (json_object_object_get_ex(obj, key, &val)) {
        *out = action_from_string(json_object_get_string(val));
    }
}

static void read_string(struct json_object *obj, const char *key, char *out, size_t size) {
    struct json_object *val = NULL;
    if (json_object_object_get_ex(obj, key, &val)) {
        const char *s = json_object_get_string(val);
        if (s) snprintf(out, size, "%s", s);
    }
}

bool config_load(aurade_config_t *cfg, const char *path) {
    if (!cfg) return false;
    *cfg = config_get_defaults();

    if (!path) path = config_get_default_path();

    struct json_object *root = json_object_from_file(path);
    if (!root) {
        // A file that exists but does not parse is almost always a settings app
        // caught mid-write. Keep the current configuration instead of
        // overwriting the user's file with defaults.
        if (access(path, F_OK) == 0) return false;

        // First run: save a default config so the user can inspect it.
        config_save(cfg, path);
        return true;
    }

    struct json_object *gestures = NULL;
    if (json_object_object_get_ex(root, "gestures", &gestures)) {
        read_action(gestures, "swipe_up_3", &cfg->swipe_up_3);
        read_action(gestures, "swipe_down_3", &cfg->swipe_down_3);
        read_action(gestures, "swipe_left_3", &cfg->swipe_left_3);
        read_action(gestures, "swipe_right_3", &cfg->swipe_right_3);
        read_action(gestures, "pinch_in_3", &cfg->pinch_in_3);
        read_action(gestures, "pinch_out_3", &cfg->pinch_out_3);

        read_action(gestures, "swipe_up_4", &cfg->swipe_up_4);
        read_action(gestures, "swipe_down_4", &cfg->swipe_down_4);
        read_action(gestures, "swipe_left_4", &cfg->swipe_left_4);
        read_action(gestures, "swipe_right_4", &cfg->swipe_right_4);
        read_action(gestures, "pinch_in_4", &cfg->pinch_in_4);
        read_action(gestures, "pinch_out_4", &cfg->pinch_out_4);

        read_action(gestures, "edge_swipe_right", &cfg->edge_swipe_right);
    }

    struct json_object *physics = NULL;
    if (json_object_object_get_ex(root, "physics", &physics)) {
        struct json_object *v;
        if (json_object_object_get_ex(physics, "stiffness", &v)) cfg->spring.stiffness = json_object_get_double(v);
        if (json_object_object_get_ex(physics, "damping", &v)) cfg->spring.damping = json_object_get_double(v);
        if (json_object_object_get_ex(physics, "mass", &v)) cfg->spring.mass = json_object_get_double(v);
        if (json_object_object_get_ex(physics, "sensitivity", &v)) cfg->sensitivity = json_object_get_double(v);
        if (json_object_object_get_ex(physics, "snap_threshold", &v)) cfg->snap_threshold = json_object_get_double(v);
        if (json_object_object_get_ex(physics, "flick_velocity_threshold", &v)) cfg->flick_velocity_threshold = json_object_get_double(v);
        if (json_object_object_get_ex(physics, "rubber_band_limit", &v)) cfg->rubber_band_limit = json_object_get_double(v);
        if (json_object_object_get_ex(physics, "deadzone_px", &v)) cfg->deadzone_px = json_object_get_double(v);
        if (json_object_object_get_ex(physics, "natural_scroll", &v)) cfg->natural_scroll = json_object_get_boolean(v);
        if (json_object_object_get_ex(physics, "disable_while_typing", &v)) cfg->disable_while_typing = json_object_get_boolean(v);
    }

    struct json_object *touchpad = NULL;
    if (json_object_object_get_ex(root, "touchpad", &touchpad)) {
        struct json_object *v;
        if (json_object_object_get_ex(touchpad, "disable_while_typing", &v)) cfg->disable_while_typing = json_object_get_boolean(v);
        if (json_object_object_get_ex(touchpad, "natural_scroll", &v)) cfg->natural_scroll = json_object_get_boolean(v);
    }

    struct json_object *keyboard = NULL;
    if (json_object_object_get_ex(root, "keyboard", &keyboard)) {
        read_string(keyboard, "layout", cfg->kb_layout, sizeof(cfg->kb_layout));
        read_string(keyboard, "variant", cfg->kb_variant, sizeof(cfg->kb_variant));
        read_string(keyboard, "model", cfg->kb_model, sizeof(cfg->kb_model));
        read_string(keyboard, "options", cfg->kb_options, sizeof(cfg->kb_options));
    }

    struct json_object *dock = NULL;
    if (json_object_object_get_ex(root, "dock", &dock)) {
        struct json_object *v;
        if (json_object_object_get_ex(dock, "enabled", &v)) cfg->dock_enabled = json_object_get_boolean(v);
        if (json_object_object_get_ex(dock, "autohide", &v)) cfg->dock_autohide = json_object_get_boolean(v);
        if (json_object_object_get_ex(dock, "intellihide", &v)) cfg->dock_intellihide = json_object_get_boolean(v);
        if (json_object_object_get_ex(dock, "icon_size", &v)) cfg->dock_icon_size = json_object_get_int(v);
        if (json_object_object_get_ex(dock, "magnification", &v)) cfg->dock_magnification = json_object_get_boolean(v);
        if (json_object_object_get_ex(dock, "max_scale", &v)) cfg->dock_max_scale = json_object_get_double(v);
        if (json_object_object_get_ex(dock, "mag_radius", &v)) cfg->dock_mag_radius = json_object_get_double(v);
        if (json_object_object_get_ex(dock, "theme", &v)) {
            snprintf(cfg->dock_theme, sizeof(cfg->dock_theme), "%s", json_object_get_string(v));
        }
        if (json_object_object_get_ex(dock, "opacity", &v)) {
            double op = json_object_get_double(v);
            if (op < 0.0) op = 0.0;
            if (op > 1.0) op = 1.0;
            cfg->dock_opacity = op;
        }

        struct json_object *pinned = NULL;
        if (json_object_object_get_ex(dock, "pinned", &pinned) &&
            json_object_is_type(pinned, json_type_array)) {
            size_t n = json_object_array_length(pinned);
            if (n > 24) n = 24;
            cfg->dock_pinned_count = 0;
            for (size_t i = 0; i < n; i++) {
                struct json_object *entry = json_object_array_get_idx(pinned, i);
                if (json_object_is_type(entry, json_type_object)) {
                    struct json_object *n_obj = NULL, *e_obj = NULL;
                    json_object_object_get_ex(entry, "name", &n_obj);
                    json_object_object_get_ex(entry, "exec", &e_obj);
                    const char *n_str = n_obj ? json_object_get_string(n_obj) : "";
                    const char *e_str = e_obj ? json_object_get_string(e_obj) : "";
                    if (n_str && *n_str) {
                        snprintf(cfg->dock_pinned[cfg->dock_pinned_count].name,
                                 sizeof(cfg->dock_pinned[0].name), "%s", n_str);
                        snprintf(cfg->dock_pinned[cfg->dock_pinned_count].exec,
                                 sizeof(cfg->dock_pinned[0].exec), "%s", e_str ? e_str : "");
                        cfg->dock_pinned_count++;
                    }
                }
            }
        }
    }

    struct json_object *appr = NULL;
    if (json_object_object_get_ex(root, "appearance", &appr)) {
        struct json_object *v;
        if (json_object_object_get_ex(appr, "dark_mode", &v)) cfg->dark_mode = json_object_get_boolean(v);
        if (json_object_object_get_ex(appr, "wallpaper_path", &v)) strncpy(cfg->wallpaper_path, json_object_get_string(v), sizeof(cfg->wallpaper_path) - 1);
        if (json_object_object_get_ex(appr, "accent_color", &v)) strncpy(cfg->accent_color, json_object_get_string(v), sizeof(cfg->accent_color) - 1);
        if (json_object_object_get_ex(appr, "window_opacity", &v)) {
            double op = json_object_get_double(v);
            if (op < 0.1) op = 0.1;
            if (op > 1.0) op = 1.0;
            cfg->window_opacity = op;
        } else if (json_object_object_get_ex(appr, "opacity", &v)) {
            double op = json_object_get_double(v);
            if (op < 0.1) op = 0.1;
            if (op > 1.0) op = 1.0;
            cfg->window_opacity = op;
        }

        cfg->window_rules_count = 0;
        struct json_object *wr = NULL;
        if (json_object_object_get_ex(appr, "window_rules", &wr) && json_object_is_type(wr, json_type_array)) {
            size_t n = json_object_array_length(wr);
            if (n > 64) n = 64;
            for (size_t i = 0; i < n; i++) {
                struct json_object *item = json_object_array_get_idx(wr, i);
                if (!json_object_is_type(item, json_type_object)) continue;
                struct json_object *a_obj = NULL, *t_obj = NULL, *o_obj = NULL;
                json_object_object_get_ex(item, "app_id", &a_obj);
                json_object_object_get_ex(item, "title", &t_obj);
                json_object_object_get_ex(item, "opacity", &o_obj);
                const char *a_str = a_obj ? json_object_get_string(a_obj) : "";
                const char *t_str = t_obj ? json_object_get_string(t_obj) : "";
                double op = o_obj ? json_object_get_double(o_obj) : 1.0;
                if (op < 0.10) op = 0.10;
                if (op > 1.0) op = 1.0;

                snprintf(cfg->window_rules[cfg->window_rules_count].app_id,
                         sizeof(cfg->window_rules[0].app_id), "%s", a_str ? a_str : "");
                snprintf(cfg->window_rules[cfg->window_rules_count].title,
                         sizeof(cfg->window_rules[0].title), "%s", t_str ? t_str : "");
                cfg->window_rules[cfg->window_rules_count].opacity = op;
                cfg->window_rules_count++;
            }
        }
        struct json_object *wo = NULL;
        if (json_object_object_get_ex(appr, "window_opacities", &wo) && json_object_is_type(wo, json_type_object)) {
            json_object_object_foreach(wo, key, val) {
                if (cfg->window_rules_count >= 64) break;
                double op = json_object_get_double(val);
                if (op < 0.10) op = 0.10;
                if (op > 1.0) op = 1.0;
                bool found = false;
                for (int i = 0; i < cfg->window_rules_count; i++) {
                    if (strcmp(cfg->window_rules[i].app_id, key) == 0 && cfg->window_rules[i].title[0] == '\0') {
                        cfg->window_rules[i].opacity = op;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    snprintf(cfg->window_rules[cfg->window_rules_count].app_id,
                             sizeof(cfg->window_rules[0].app_id), "%s", key);
                    cfg->window_rules[cfg->window_rules_count].title[0] = '\0';
                    cfg->window_rules[cfg->window_rules_count].opacity = op;
                    cfg->window_rules_count++;
                }
            }
        }
    }

    struct json_object *ws = NULL;
    if (json_object_object_get_ex(root, "workspaces", &ws)) {
        struct json_object *v;
        if (json_object_object_get_ex(ws, "count", &v)) {
            int c = json_object_get_int(v);
            if (c >= 1 && c <= 16) cfg->num_workspaces = c;
        }
        if (json_object_object_get_ex(ws, "auto_focus", &v)) {
            cfg->workspace_auto_focus = json_object_get_boolean(v);
        }
        if (json_object_object_get_ex(ws, "tiling", &v)) {
            cfg->tiling_enabled = json_object_get_boolean(v);
        }
        if (json_object_object_get_ex(ws, "tiling_gap", &v)) {
            int g = json_object_get_int(v);
            if (g < 0) g = 0;
            if (g > 48) g = 48;
            cfg->tiling_gap = g;
        }
    }

    struct json_object *tiling_obj = NULL;
    if (json_object_object_get_ex(root, "tiling", &tiling_obj)) {
        struct json_object *v;
        if (json_object_object_get_ex(tiling_obj, "enabled", &v)) {
            cfg->tiling_enabled = json_object_get_boolean(v);
        }
        if (json_object_object_get_ex(tiling_obj, "gap", &v)) {
            int g = json_object_get_int(v);
            if (g < 0) g = 0;
            if (g > 48) g = 48;
            cfg->tiling_gap = g;
        }
    }

    struct json_object *mp = NULL;
    if (json_object_object_get_ex(root, "music_pill", &mp)) {
        struct json_object *v;
        if (json_object_object_get_ex(mp, "enabled", &v)) cfg->music_pill_enabled = json_object_get_boolean(v);
        read_string(mp, "position", cfg->music_pill_position, sizeof(cfg->music_pill_position));
    }

    struct json_object *blur = NULL;
    if (json_object_object_get_ex(root, "blur", &blur)) {
        struct json_object *v;
        if (json_object_object_get_ex(blur, "dock", &v)) cfg->blur_dock = json_object_get_boolean(v);
        if (json_object_object_get_ex(blur, "topbar", &v)) cfg->blur_topbar = json_object_get_boolean(v);
        if (json_object_object_get_ex(blur, "launchpad", &v)) cfg->blur_launchpad = json_object_get_boolean(v);
        if (json_object_object_get_ex(blur, "workspace_overview", &v)) cfg->blur_workspace_overview = json_object_get_boolean(v);
        if (json_object_object_get_ex(blur, "windows", &v)) cfg->blur_windows = json_object_get_boolean(v);
        else if (json_object_object_get_ex(blur, "window", &v)) cfg->blur_windows = json_object_get_boolean(v);
        if (json_object_object_get_ex(blur, "strength", &v)) {
            double s = json_object_get_double(v);
            if (s >= 0.1 && s <= 3.0) cfg->blur_strength = s;
        } else {
            cfg->blur_strength = 1.0;
        }
    }

    struct json_object *tb = NULL;
    if (json_object_object_get_ex(root, "topbar", &tb)) {
        struct json_object *v;
        if (json_object_object_get_ex(tb, "cpu_temp", &v)) cfg->topbar_cpu_temp = json_object_get_boolean(v);
    }

    json_object_put(root);
    return true;
}

static void add_action(struct json_object *obj, const char *key, const gesture_action_t *action) {
    char buf[320];
    json_object_object_add(obj, key, json_object_new_string(action_to_string(action, buf, sizeof(buf))));
}

bool config_save(const aurade_config_t *cfg, const char *path) {
    if (!cfg) return false;
    if (!path) path = config_get_default_path();
    ensure_parent_dir(path);

    struct json_object *root = json_object_new_object();

    struct json_object *gestures = json_object_new_object();
    add_action(gestures, "swipe_up_3", &cfg->swipe_up_3);
    add_action(gestures, "swipe_down_3", &cfg->swipe_down_3);
    add_action(gestures, "swipe_left_3", &cfg->swipe_left_3);
    add_action(gestures, "swipe_right_3", &cfg->swipe_right_3);
    add_action(gestures, "pinch_in_3", &cfg->pinch_in_3);
    add_action(gestures, "pinch_out_3", &cfg->pinch_out_3);

    add_action(gestures, "swipe_up_4", &cfg->swipe_up_4);
    add_action(gestures, "swipe_down_4", &cfg->swipe_down_4);
    add_action(gestures, "swipe_left_4", &cfg->swipe_left_4);
    add_action(gestures, "swipe_right_4", &cfg->swipe_right_4);
    add_action(gestures, "pinch_in_4", &cfg->pinch_in_4);
    add_action(gestures, "pinch_out_4", &cfg->pinch_out_4);

    add_action(gestures, "edge_swipe_right", &cfg->edge_swipe_right);
    json_object_object_add(root, "gestures", gestures);

    struct json_object *physics = json_object_new_object();
    json_object_object_add(physics, "stiffness", json_object_new_double(cfg->spring.stiffness));
    json_object_object_add(physics, "damping", json_object_new_double(cfg->spring.damping));
    json_object_object_add(physics, "mass", json_object_new_double(cfg->spring.mass));
    json_object_object_add(physics, "sensitivity", json_object_new_double(cfg->sensitivity));
    json_object_object_add(physics, "snap_threshold", json_object_new_double(cfg->snap_threshold));
    json_object_object_add(physics, "flick_velocity_threshold", json_object_new_double(cfg->flick_velocity_threshold));
    json_object_object_add(physics, "rubber_band_limit", json_object_new_double(cfg->rubber_band_limit));
    json_object_object_add(physics, "deadzone_px", json_object_new_double(cfg->deadzone_px));
    json_object_object_add(physics, "natural_scroll", json_object_new_boolean(cfg->natural_scroll));
    json_object_object_add(physics, "disable_while_typing", json_object_new_boolean(cfg->disable_while_typing));
    json_object_object_add(root, "physics", physics);

    struct json_object *keyboard = json_object_new_object();
    json_object_object_add(keyboard, "layout", json_object_new_string(cfg->kb_layout));
    json_object_object_add(keyboard, "variant", json_object_new_string(cfg->kb_variant));
    json_object_object_add(keyboard, "model", json_object_new_string(cfg->kb_model));
    json_object_object_add(keyboard, "options", json_object_new_string(cfg->kb_options));
    json_object_object_add(root, "keyboard", keyboard);

    struct json_object *dock = json_object_new_object();
    json_object_object_add(dock, "enabled", json_object_new_boolean(cfg->dock_enabled));
    json_object_object_add(dock, "autohide", json_object_new_boolean(cfg->dock_autohide));
    json_object_object_add(dock, "intellihide", json_object_new_boolean(cfg->dock_intellihide));
    json_object_object_add(dock, "icon_size", json_object_new_int(cfg->dock_icon_size));
    json_object_object_add(dock, "magnification", json_object_new_boolean(cfg->dock_magnification));
    json_object_object_add(dock, "max_scale", json_object_new_double(cfg->dock_max_scale));
    json_object_object_add(dock, "mag_radius", json_object_new_double(cfg->dock_mag_radius));
    json_object_object_add(dock, "theme", json_object_new_string(cfg->dock_theme));
    json_object_object_add(dock, "opacity", json_object_new_double(cfg->dock_opacity));
    if (cfg->dock_pinned_count > 0) {
        struct json_object *pinned = json_object_new_array();
        for (int i = 0; i < cfg->dock_pinned_count; i++) {
            struct json_object *entry = json_object_new_object();
            json_object_object_add(entry, "name", json_object_new_string(cfg->dock_pinned[i].name));
            json_object_object_add(entry, "exec", json_object_new_string(cfg->dock_pinned[i].exec));
            json_object_array_add(pinned, entry);
        }
        json_object_object_add(dock, "pinned", pinned);
    }
    json_object_object_add(root, "dock", dock);

    struct json_object *appr = json_object_new_object();
    json_object_object_add(appr, "dark_mode", json_object_new_boolean(cfg->dark_mode));
    json_object_object_add(appr, "wallpaper_path", json_object_new_string(cfg->wallpaper_path));
    json_object_object_add(appr, "accent_color", json_object_new_string(cfg->accent_color));
    json_object_object_add(appr, "window_opacity", json_object_new_double(cfg->window_opacity));
    if (cfg->window_rules_count > 0) {
        struct json_object *rules_arr = json_object_new_array();
        struct json_object *op_obj = json_object_new_object();
        for (int i = 0; i < cfg->window_rules_count; i++) {
            struct json_object *rentry = json_object_new_object();
            json_object_object_add(rentry, "app_id", json_object_new_string(cfg->window_rules[i].app_id));
            if (cfg->window_rules[i].title[0]) {
                json_object_object_add(rentry, "title", json_object_new_string(cfg->window_rules[i].title));
            }
            json_object_object_add(rentry, "opacity", json_object_new_double(cfg->window_rules[i].opacity));
            json_object_array_add(rules_arr, rentry);

            if (cfg->window_rules[i].app_id[0] && !cfg->window_rules[i].title[0]) {
                json_object_object_add(op_obj, cfg->window_rules[i].app_id,
                                       json_object_new_double(cfg->window_rules[i].opacity));
            }
        }
        json_object_object_add(appr, "window_rules", rules_arr);
        json_object_object_add(appr, "window_opacities", op_obj);
    }
    json_object_object_add(root, "appearance", appr);

    struct json_object *ws = json_object_new_object();
    json_object_object_add(ws, "count", json_object_new_int(cfg->num_workspaces > 0 ? cfg->num_workspaces : 4));
    json_object_object_add(ws, "auto_focus", json_object_new_boolean(cfg->workspace_auto_focus));
    json_object_object_add(ws, "tiling", json_object_new_boolean(cfg->tiling_enabled));
    json_object_object_add(ws, "tiling_gap", json_object_new_int(cfg->tiling_gap));
    json_object_object_add(root, "workspaces", ws);

    struct json_object *mp = json_object_new_object();
    json_object_object_add(mp, "enabled", json_object_new_boolean(cfg->music_pill_enabled));
    json_object_object_add(mp, "position", json_object_new_string(cfg->music_pill_position[0] ? cfg->music_pill_position : "topbar_center"));
    json_object_object_add(root, "music_pill", mp);

    struct json_object *blur = json_object_new_object();
    json_object_object_add(blur, "dock", json_object_new_boolean(cfg->blur_dock));
    json_object_object_add(blur, "topbar", json_object_new_boolean(cfg->blur_topbar));
    json_object_object_add(blur, "launchpad", json_object_new_boolean(cfg->blur_launchpad));
    json_object_object_add(blur, "workspace_overview", json_object_new_boolean(cfg->blur_workspace_overview));
    json_object_object_add(blur, "windows", json_object_new_boolean(cfg->blur_windows));
    json_object_object_add(blur, "strength", json_object_new_double(cfg->blur_strength > 0.05 ? cfg->blur_strength : 1.0));
    json_object_object_add(root, "blur", blur);

    struct json_object *tb = json_object_new_object();
    json_object_object_add(tb, "cpu_temp", json_object_new_boolean(cfg->topbar_cpu_temp));
    json_object_object_add(root, "topbar", tb);

    int ret = json_object_to_file_ext(path, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);
    return ret == 0;
}

const char *config_get_state_path(void) {
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/home/user";
    snprintf(path, sizeof(path), "%s/.config/aurade/state.json", home);
    return path;
}

void session_state_load(aurade_session_state_t *st) {
    if (!st) return;
    st->volume = 0.30;
    st->volume_muted = false;
    st->brightness = 0.60;
    st->sink_count = 0;

    const char *path = config_get_state_path();
    struct json_object *root = json_object_from_file(path);
    if (!root) return;

    struct json_object *val = NULL;
    if (json_object_object_get_ex(root, "volume", &val)) {
        double v = json_object_get_double(val);
        if (v >= 0.01 && v <= 1.0) st->volume = v;
    }
    if (json_object_object_get_ex(root, "volume_muted", &val)) {
        st->volume_muted = json_object_get_boolean(val);
    }
    if (json_object_object_get_ex(root, "brightness", &val)) {
        double b = json_object_get_double(val);
        if (b >= 0.01 && b <= 1.0) st->brightness = b;
    }

    struct json_object *sinks_obj = NULL;
    if (json_object_object_get_ex(root, "sinks", &sinks_obj) &&
        json_object_get_type(sinks_obj) == json_type_object) {
        json_object_object_foreach(sinks_obj, key, sink_val) {
            if (st->sink_count >= 32) break;
            if (!key || !*key) continue;
            strncpy(st->sinks[st->sink_count].name, key, sizeof(st->sinks[st->sink_count].name) - 1);
            if (json_object_get_type(sink_val) == json_type_double || json_object_get_type(sink_val) == json_type_int) {
                double sv = json_object_get_double(sink_val);
                if (sv < 0.01) sv = 0.01;
                if (sv > 1.0) sv = 1.0;
                st->sinks[st->sink_count].volume = sv;
                st->sinks[st->sink_count].muted = false;
                st->sink_count++;
            } else if (json_object_get_type(sink_val) == json_type_object) {
                struct json_object *sv_obj = NULL;
                double sv = 0.30;
                bool sm = false;
                if (json_object_object_get_ex(sink_val, "volume", &sv_obj)) {
                    sv = json_object_get_double(sv_obj);
                }
                if (json_object_object_get_ex(sink_val, "muted", &sv_obj)) {
                    sm = json_object_get_boolean(sv_obj);
                }
                if (sv < 0.01) sv = 0.01;
                if (sv > 1.0) sv = 1.0;
                st->sinks[st->sink_count].volume = sv;
                st->sinks[st->sink_count].muted = sm;
                st->sink_count++;
            }
        }
    }

    json_object_put(root);
}

void session_state_save(const aurade_session_state_t *st) {
    if (!st) return;
    const char *path = config_get_state_path();
    ensure_parent_dir(path);

    struct json_object *root = json_object_new_object();
    json_object_object_add(root, "volume", json_object_new_double(st->volume));
    json_object_object_add(root, "volume_muted", json_object_new_boolean(st->volume_muted));
    json_object_object_add(root, "brightness", json_object_new_double(st->brightness));

    struct json_object *sinks_obj = json_object_new_object();
    for (int i = 0; i < st->sink_count; i++) {
        struct json_object *s_entry = json_object_new_object();
        json_object_object_add(s_entry, "volume", json_object_new_double(st->sinks[i].volume));
        json_object_object_add(s_entry, "muted", json_object_new_boolean(st->sinks[i].muted));
        json_object_object_add(sinks_obj, st->sinks[i].name, s_entry);
    }
    json_object_object_add(root, "sinks", sinks_obj);

    char tmp_path[540];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (json_object_to_file_ext(tmp_path, root, JSON_C_TO_STRING_PRETTY) == 0) {
        rename(tmp_path, path);
    }
    json_object_put(root);
}

double session_state_get_sink_volume(const aurade_session_state_t *st, const char *sink_name, double fallback) {
    if (!st || !sink_name || !*sink_name) return fallback;
    for (int i = 0; i < st->sink_count; i++) {
        if (strcmp(st->sinks[i].name, sink_name) == 0) {
            return st->sinks[i].volume;
        }
    }
    return fallback;
}

void session_state_set_sink_volume(aurade_session_state_t *st, const char *sink_name, double volume, bool muted) {
    if (!st) return;
    if (volume < 0.01) volume = 0.01;
    if (volume > 1.0) volume = 1.0;
    st->volume = volume;
    st->volume_muted = muted;

    if (sink_name && *sink_name) {
        int found = -1;
        for (int i = 0; i < st->sink_count; i++) {
            if (strcmp(st->sinks[i].name, sink_name) == 0) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            st->sinks[found].volume = volume;
            st->sinks[found].muted = muted;
        } else if (st->sink_count < 32) {
            strncpy(st->sinks[st->sink_count].name, sink_name, sizeof(st->sinks[st->sink_count].name) - 1);
            st->sinks[st->sink_count].volume = volume;
            st->sinks[st->sink_count].muted = muted;
            st->sink_count++;
        }
    }
    session_state_save(st);
}

void session_state_set_brightness(aurade_session_state_t *st, double brightness) {
    if (!st) return;
    if (brightness < 0.01) brightness = 0.01;
    if (brightness > 1.0) brightness = 1.0;
    st->brightness = brightness;
    session_state_save(st);
}

