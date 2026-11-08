#ifndef AURADE_APPS_H
#define AURADE_APPS_H

#include <stdbool.h>
#include <cairo.h>

#define MAX_APPS 512

typedef struct {
    char name[128];
    char exec[256];
    char icon_name[128];
    char generic_name[128];
    cairo_surface_t *icon_surface;
} desktop_app_t;

typedef struct {
    desktop_app_t apps[MAX_APPS];
    int count;
} desktop_apps_t;

/**
 * Scan /usr/share/applications/ and load graphical apps.
 */
desktop_apps_t *apps_scanner_load(void);

/**
 * Free app list and cached icon surfaces.
 */
void apps_scanner_free(desktop_apps_t *list);

/**
 * Launch an app by exec string.
 */
bool app_launch_command(const char *command);

/**
 * Find app by name or command substring.
 */
desktop_app_t *apps_find(desktop_apps_t *list, const char *name);

/**
 * Load an icon surface by icon name or file path.
 */
cairo_surface_t *apps_load_icon(const char *name);

#endif // AURADE_APPS_H
