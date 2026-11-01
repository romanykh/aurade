#include "apps.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <math.h>
#include <ctype.h>
#include <librsvg/rsvg.h>

static void clean_exec_field(char *exec) {
    // Remove %f, %F, %u, %U, etc.
    char *p = exec;
    while (*p) {
        if (*p == '%' && *(p + 1) != '\0') {
            *p = ' ';
            *(p + 1) = ' ';
        }
        p++;
    }
    // Trim trailing spaces
    int len = strlen(exec);
    while (len > 0 && (exec[len - 1] == ' ' || exec[len - 1] == '\n' || exec[len - 1] == '\r')) {
        exec[len - 1] = '\0';
        len--;
    }
}

static cairo_surface_t *load_svg_surface(const char *filepath, int size) {
    if (!filepath || access(filepath, R_OK) != 0) return NULL;
    GError *err = NULL;
    RsvgHandle *h = rsvg_handle_new_from_file(filepath, &err);
    if (!h) return NULL;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        g_object_unref(h);
        return NULL;
    }

    cairo_t *cr = cairo_create(surf);
    RsvgRectangle vp = { .x = 0, .y = 0, .width = (double)size, .height = (double)size };
    if (!rsvg_handle_render_document(h, cr, &vp, &err)) {
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        g_object_unref(h);
        return NULL;
    }
    cairo_destroy(cr);
    g_object_unref(h);
    return surf;
}

static cairo_surface_t *load_png_surface(const char *filepath) {
    if (!filepath || access(filepath, R_OK) != 0) return NULL;
    cairo_surface_t *s = cairo_image_surface_create_from_png(filepath);
    if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) return s;
    cairo_surface_destroy(s);
    return NULL;
}

static cairo_surface_t *try_file(const char *path) {
    if (access(path, R_OK) != 0) return NULL;
    if (strstr(path, ".svg") || strstr(path, ".SVG")) {
        return load_svg_surface(path, 96);
    }
    if (strstr(path, ".png") || strstr(path, ".PNG")) {
        return load_png_surface(path);
    }
    return NULL;
}

static cairo_surface_t *search_icon_name(const char *name) {
    if (!name || !*name) return NULL;

    // Direct absolute path?
    if (name[0] == '/') {
        cairo_surface_t *s = try_file(name);
        if (s) return s;
        char p[512];
        snprintf(p, sizeof(p), "%s.svg", name);
        s = try_file(p);
        if (s) return s;
        snprintf(p, sizeof(p), "%s.png", name);
        s = try_file(p);
        if (s) return s;
        return NULL;
    }

    const char *home = getenv("HOME");
    char user_local_icons[512] = {0};
    if (home) {
        snprintf(user_local_icons, sizeof(user_local_icons), "%s/.local/share/icons", home);
    }

    // High-priority icon directories (WhiteSur active macOS theme, Papirus, hicolor, Adwaita, pixmaps)
    const char *search_dirs[] = {
        // WhiteSur (active macOS theme)
        "/usr/share/icons/WhiteSur-dark/apps/scalable",
        "/usr/share/icons/WhiteSur/apps/scalable",
        "/usr/share/icons/WhiteSur/apps/128x128",
        "/usr/share/icons/WhiteSur/apps/64x64",
        "/usr/share/icons/WhiteSur/apps/48x48",
        "/usr/share/icons/WhiteSur/apps/32x32",
        "/usr/share/icons/WhiteSur/categories/32",
        "/usr/share/icons/WhiteSur/preferences/scalable",
        "/usr/share/icons/WhiteSur/devices/scalable",

        // Papirus
        "/usr/share/icons/Papirus-Dark/64x64/apps",
        "/usr/share/icons/Papirus-Dark/48x48/apps",
        "/usr/share/icons/Papirus/64x64/apps",
        "/usr/share/icons/Papirus/48x48/apps",

        // Hicolor (standard Freedesktop system repository)
        "/usr/share/icons/hicolor/scalable/apps",
        "/usr/share/icons/hicolor/512x512/apps",
        "/usr/share/icons/hicolor/256x256/apps",
        "/usr/share/icons/hicolor/128x128/apps",
        "/usr/share/icons/hicolor/64x64/apps",
        "/usr/share/icons/hicolor/48x48/apps",
        "/usr/share/icons/hicolor/32x32/apps",

        // Flatpak icons
        "/var/lib/flatpak/exports/share/icons/hicolor/scalable/apps",
        "/var/lib/flatpak/exports/share/icons/hicolor/128x128/apps",
        "/var/lib/flatpak/exports/share/icons/hicolor/64x64/apps",

        // Adwaita
        "/usr/share/icons/Adwaita/48x48/apps",
        "/usr/share/icons/Adwaita/scalable/apps",
        "/usr/share/icons/Adwaita/scalable/ui",

        // Pixmaps legacy
        "/usr/share/pixmaps",
    };

    char full_path[512];

    // If icon name has extension already:
    if (strstr(name, ".png") || strstr(name, ".svg")) {
        for (size_t i = 0; i < sizeof(search_dirs) / sizeof(search_dirs[0]); i++) {
            snprintf(full_path, sizeof(full_path), "%s/%s", search_dirs[i], name);
            cairo_surface_t *s = try_file(full_path);
            if (s) return s;
        }
        if (home && user_local_icons[0]) {
            snprintf(full_path, sizeof(full_path), "%s/hicolor/scalable/apps/%s", user_local_icons, name);
            cairo_surface_t *s = try_file(full_path);
            if (s) return s;
            snprintf(full_path, sizeof(full_path), "%s/hicolor/128x128/apps/%s", user_local_icons, name);
            s = try_file(full_path);
            if (s) return s;
        }
    }

    // Try .svg first (high resolution scalable vector graphics), then .png
    for (size_t i = 0; i < sizeof(search_dirs) / sizeof(search_dirs[0]); i++) {
        snprintf(full_path, sizeof(full_path), "%s/%s.svg", search_dirs[i], name);
        cairo_surface_t *s = try_file(full_path);
        if (s) return s;

        snprintf(full_path, sizeof(full_path), "%s/%s.png", search_dirs[i], name);
        s = try_file(full_path);
        if (s) return s;
    }

    if (home && user_local_icons[0]) {
        snprintf(full_path, sizeof(full_path), "%s/hicolor/scalable/apps/%s.svg", user_local_icons, name);
        cairo_surface_t *s = try_file(full_path);
        if (s) return s;
        snprintf(full_path, sizeof(full_path), "%s/hicolor/128x128/apps/%s.png", user_local_icons, name);
        s = try_file(full_path);
        if (s) return s;
    }

    return NULL;
}

static cairo_surface_t *try_load_icon(const char *icon_name) {
    if (!icon_name || !*icon_name) return NULL;

    cairo_surface_t *s = search_icon_name(icon_name);
    if (s) return s;

    // Lowercase fallback (e.g. Firefox -> firefox, Foot -> foot)
    char lower[128];
    strncpy(lower, icon_name, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (strcmp(lower, icon_name) != 0) {
        s = search_icon_name(lower);
        if (s) return s;
    }

    // GNOME prefix stripped fallback (e.g. org.gnome.Terminal -> gnome-terminal or Terminal)
    if (strncmp(icon_name, "org.gnome.", 10) == 0) {
        const char *short_name = icon_name + 10;
        s = search_icon_name(short_name);
        if (s) return s;

        char prefixed[256];
        snprintf(prefixed, sizeof(prefixed), "gnome-%s", short_name);
        for (char *p = prefixed; *p; p++) *p = (char)tolower((unsigned char)*p);
        s = search_icon_name(prefixed);
        if (s) return s;
    }

    return NULL;
}

static void parse_desktop_file(const char *filepath, desktop_app_t *app) {
    FILE *f = fopen(filepath, "r");
    if (!f) return;

    char line[512];
    bool in_desktop_entry = false;
    bool no_display = false;
    bool is_type_application = false;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '[') {
            if (strncmp(line, "[Desktop Entry]", 15) == 0) in_desktop_entry = true;
            else in_desktop_entry = false;
            continue;
        }
        if (!in_desktop_entry) continue;

        if (strncmp(line, "Type=", 5) == 0) {
            if (strstr(line, "Application")) is_type_application = true;
        } else if (strncmp(line, "NoDisplay=true", 14) == 0) {
            no_display = true;
        } else if (strncmp(line, "Name=", 5) == 0 && app->name[0] == '\0') {
            snprintf(app->name, sizeof(app->name), "%.*s", (int)(sizeof(app->name) - 1), line + 5);
            char *nl = strchr(app->name, '\n'); if (nl) *nl = '\0';
        } else if (strncmp(line, "Exec=", 5) == 0 && app->exec[0] == '\0') {
            snprintf(app->exec, sizeof(app->exec), "%.*s", (int)(sizeof(app->exec) - 1), line + 5);
            clean_exec_field(app->exec);
        } else if (strncmp(line, "Icon=", 5) == 0 && app->icon_name[0] == '\0') {
            snprintf(app->icon_name, sizeof(app->icon_name), "%.*s", (int)(sizeof(app->icon_name) - 1), line + 5);
            char *nl = strchr(app->icon_name, '\n'); if (nl) *nl = '\0';
        } else if (strncmp(line, "GenericName=", 12) == 0 && app->generic_name[0] == '\0') {
            snprintf(app->generic_name, sizeof(app->generic_name), "%.*s", (int)(sizeof(app->generic_name) - 1), line + 12);
            char *nl = strchr(app->generic_name, '\n'); if (nl) *nl = '\0';
        }
    }
    fclose(f);

    if (no_display || !is_type_application || app->name[0] == '\0' || app->exec[0] == '\0') {
        app->name[0] = '\0';
        return;
    }

    app->icon_surface = try_load_icon(app->icon_name);
}

desktop_apps_t *apps_scanner_load(void) {
    desktop_apps_t *list = calloc(1, sizeof(*list));
    if (!list) return NULL;

    char user_app_dir[512] = {0};
    const char *home = getenv("HOME");
    if (home) {
        snprintf(user_app_dir, sizeof(user_app_dir), "%s/.local/share/applications", home);
    }

    const char *dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        "/var/lib/flatpak/exports/share/applications",
        user_app_dir[0] ? user_app_dir : NULL,
    };

    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        if (!dirs[d]) continue;
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strstr(entry->d_name, ".desktop")) {
                if (list->count >= MAX_APPS) break;
                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s", dirs[d], entry->d_name);

                desktop_app_t app = {0};
                parse_desktop_file(full_path, &app);
                if (app.name[0] != '\0') {
                    // Check duplicate name or exec
                    bool dup = false;
                    for (int i = 0; i < list->count; i++) {
                        if (strcmp(list->apps[i].name, app.name) == 0 ||
                            strcmp(list->apps[i].exec, app.exec) == 0) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        list->apps[list->count++] = app;
                    } else if (app.icon_surface) {
                        cairo_surface_destroy(app.icon_surface);
                    }
                }
            }
        }
        closedir(dir);
    }

    return list;
}

void apps_scanner_free(desktop_apps_t *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        if (list->apps[i].icon_surface) {
            cairo_surface_destroy(list->apps[i].icon_surface);
        }
    }
    free(list);
}

bool app_launch_command(const char *command) {
    if (!command || !*command) return false;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // Child process
        setsid();
        setenv("XCURSOR_THEME", "Aura-Circle", 1);
        setenv("XCURSOR_SIZE", "24", 1);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    return true;
}

desktop_app_t *apps_find(desktop_apps_t *list, const char *name) {
    if (!list || !name || !*name) return NULL;
    for (int i = 0; i < list->count; i++) {
        if (strcasestr(list->apps[i].name, name) || strcasestr(list->apps[i].exec, name)) {
            return &list->apps[i];
        }
    }
    return NULL;
}

cairo_surface_t *apps_load_icon(const char *name) {
    return try_load_icon(name);
}

