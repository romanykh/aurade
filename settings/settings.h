#ifndef AURADE_SETTINGS_H
#define AURADE_SETTINGS_H

#include <gtk/gtk.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include "config/config.h"

// Global configuration state
extern aurade_config_t g_config;
extern GtkWidget *g_main_window;
extern GtkWidget *g_stack;
extern GtkWidget *g_status_label;
extern GtkCssProvider *g_css_provider;
extern guint g_save_timeout_id;
extern char g_target_page[64];

// Gesture action helper mapping
typedef struct {
    gesture_action_type_t type;
    const char *id;
    const char *label;
} action_item_t;

extern const action_item_t GESTURE_ACTIONS[];
extern const int GESTURE_ACTIONS_COUNT;

// System helpers
bool run_cmd_capture(const char *cmd, char *out, size_t size);
void run_cmd_async(const char *cmd);

// GSettings helpers
bool gsettings_get_bool(const char *schema, const char *key, bool def);
void gsettings_set_bool(const char *schema, const char *key, bool val);
int gsettings_get_int(const char *schema, const char *key, int def);
void gsettings_set_int(const char *schema, const char *key, int val);
double gsettings_get_double(const char *schema, const char *key, double def);
void gsettings_set_double(const char *schema, const char *key, double val);
char *gsettings_get_string(const char *schema, const char *key, const char *def);
void gsettings_set_string(const char *schema, const char *key, const char *val);

// Config save/reload
void settings_schedule_save(void);
void settings_save_now(void);
void settings_reset_defaults(void);
void apply_theme(bool dark_mode);

// UI helper prototypes
GtkWidget *ui_make_badge(const char *icon_name, const char *color_class, const char *size_class);
GtkWidget *ui_page_container(const char *title, const char *lead);
GtkWidget *ui_card(GtkWidget *parent, const char *section_title);
GtkWidget *ui_add_row(GtkWidget *card, const char *title, const char *subtitle,
                      GtkWidget *control, const char *icon_name, const char *badge_class);

GtkWidget *ui_switch(bool initial, void (*callback)(bool active, gpointer user_data), gpointer user_data);
GtkWidget *ui_scale(double val, double min, double max, double step, int digits,
                    void (*callback)(double value, gpointer user_data), gpointer user_data);
GtkWidget *ui_dropdown(const char *const *labels, int count, int selected,
                       void (*callback)(int selected_idx, gpointer user_data), gpointer user_data);
GtkWidget *ui_entry(const char *text, const char *placeholder,
                    void (*callback)(const char *text, gpointer user_data), gpointer user_data);

// Page builder prototypes
GtkWidget *build_page_wifi(void);
GtkWidget *build_page_bluetooth(void);
GtkWidget *build_page_sound(void);
GtkWidget *build_page_display(void);
GtkWidget *build_page_workspaces(void);
GtkWidget *build_page_mouse(void);
GtkWidget *build_page_keyboard(void);
GtkWidget *build_page_appearance(void);
GtkWidget *build_page_dock(void);
GtkWidget *build_page_gestures(void);
GtkWidget *build_page_default_apps(void);
GtkWidget *build_page_about(void);

#endif // AURADE_SETTINGS_H
