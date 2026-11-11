#include "settings.h"
#include "css.h"

aurade_config_t g_config;
GtkWidget *g_main_window = NULL;
GtkWidget *g_stack = NULL;
GtkWidget *g_status_label = NULL;
GtkWidget *g_sidebar_list = NULL;
GtkCssProvider *g_css_provider = NULL;
guint g_save_timeout_id = 0;
char g_target_page[64] = {0};

typedef struct {
    const char *id;
    const char *title;
    const char *icon;
    const char *badge_class;
    GtkWidget *(*builder)(void);
    GtkWidget *row_widget;
} page_meta_t;

static page_meta_t PAGES[] = {
    { "wifi",        "Wi-Fi",            "network-wireless-symbolic",             "badge-wifi",        build_page_wifi,        NULL },
    { "bluetooth",   "Bluetooth",        "bluetooth-symbolic",                    "badge-bluetooth",   build_page_bluetooth,   NULL },
    { "sound",       "Sound",            "audio-volume-high-symbolic",            "badge-sound",       build_page_sound,       NULL },
    { "display",     "Displays",         "video-display-symbolic",                "badge-display",     build_page_display,     NULL },
    { "workspaces",  "Workspaces",       "view-grid-symbolic",                    "badge-workspaces",  build_page_workspaces,  NULL },
    { "mouse",       "Mouse & Touchpad", "input-mouse-symbolic",                  "badge-mouse",       build_page_mouse,       NULL },
    { "keyboard",    "Keyboard",         "input-keyboard-symbolic",               "badge-keyboard",    build_page_keyboard,    NULL },
    { "appearance",  "Appearance",       "preferences-desktop-wallpaper-symbolic","badge-appearance", build_page_appearance,  NULL },
    { "dock",        "Dock",             "view-paged-symbolic",                   "badge-dock",        build_page_dock,        NULL },
    { "gestures",    "Gestures",         "input-touchpad-symbolic",               "badge-gestures",    build_page_gestures,    NULL },
    { "about",       "About",            "help-about-symbolic",                   "badge-about",       build_page_about,       NULL },
};
static const int PAGES_COUNT = sizeof(PAGES) / sizeof(PAGES[0]);

void apply_theme(bool dark_mode) {
    if (!g_css_provider) return;
    const char *css = settings_get_css(dark_mode);
    gtk_css_provider_load_from_string(g_css_provider, css);
}

void settings_save_now(void) {
    if (config_save(&g_config, NULL)) {
        if (g_status_label) {
            gtk_label_set_text(GTK_LABEL(g_status_label), "Saved");
            gtk_widget_remove_css_class(g_status_label, "disconnected-pill");
            gtk_widget_add_css_class(g_status_label, "connected-pill");
        }
    } else {
        if (g_status_label) {
            gtk_label_set_text(GTK_LABEL(g_status_label), "Save failed");
        }
    }
}

static gboolean on_save_debounce_timeout(gpointer user_data) {
    (void)user_data;
    g_save_timeout_id = 0;
    settings_save_now();
    return G_SOURCE_REMOVE;
}

void settings_schedule_save(void) {
    if (g_status_label) {
        gtk_label_set_text(GTK_LABEL(g_status_label), "Saving…");
        gtk_widget_remove_css_class(g_status_label, "connected-pill");
        gtk_widget_add_css_class(g_status_label, "disconnected-pill");
    }
    if (g_save_timeout_id > 0) {
        g_source_remove(g_save_timeout_id);
    }
    g_save_timeout_id = g_timeout_add(300, on_save_debounce_timeout, NULL);
}

void settings_reset_defaults(void) {
    g_config = config_get_defaults();
    settings_save_now();
    apply_theme(g_config.dark_mode);
}

static void on_sidebar_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "page_idx"));
    if (idx >= 0 && idx < PAGES_COUNT) {
        gtk_stack_set_visible_child_name(GTK_STACK(g_stack), PAGES[idx].id);
    }
}

static GtkWidget *build_sidebar(void) {
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 175, -1);
    gtk_widget_set_hexpand(sidebar, FALSE);

    // Profile Card at top
    GtkWidget *prof_card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(prof_card, "profile-card");

    GtkWidget *avatar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(avatar, "profile-avatar");
    GtkWidget *av_icon = gtk_image_new_from_icon_name("avatar-default-symbolic");
    gtk_widget_set_halign(av_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(av_icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(avatar), av_icon);
    gtk_box_append(GTK_BOX(prof_card), avatar);

    GtkWidget *prof_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(prof_text, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(prof_text, TRUE);

    char username[64] = "User";
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name) {
        strncpy(username, pw->pw_name, sizeof(username) - 1);
    }
    char hostname[64] = "AuraDE";
    gethostname(hostname, sizeof(hostname) - 1);

    GtkWidget *lbl_name = gtk_label_new(username);
    gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(lbl_name), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(lbl_name), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(lbl_name, "profile-name");
    gtk_box_append(GTK_BOX(prof_text), lbl_name);

    GtkWidget *lbl_sub = gtk_label_new(hostname);
    gtk_label_set_xalign(GTK_LABEL(lbl_sub), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(lbl_sub), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(lbl_sub), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(lbl_sub, "profile-sub");
    gtk_box_append(GTK_BOX(prof_text), lbl_sub);

    gtk_box_append(GTK_BOX(prof_card), prof_text);
    gtk_box_append(GTK_BOX(sidebar), prof_card);

    // Category List
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    g_sidebar_list = list;

    for (int i = 0; i < PAGES_COUNT; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        g_object_set_data(G_OBJECT(row), "page_idx", GINT_TO_POINTER(i));
        PAGES[i].row_widget = row;

        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *badge = ui_make_badge(PAGES[i].icon, PAGES[i].badge_class, "sidebar-badge");
        gtk_box_append(GTK_BOX(row_box), badge);

        GtkWidget *lbl = gtk_label_new(PAGES[i].title);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(row_box), lbl);

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
        gtk_list_box_append(GTK_LIST_BOX(list), row);
    }

    g_signal_connect(list, "row-selected", G_CALLBACK(on_sidebar_row_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(sidebar), scroll);

    return sidebar;
}

static void on_app_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    // Load configuration
    if (!config_load(&g_config, NULL)) {
        g_config = config_get_defaults();
    }

    // Main window
    g_main_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_main_window), "Settings");
    gtk_window_set_default_size(GTK_WINDOW(g_main_window), 840, 580);
    gtk_widget_set_size_request(GTK_WIDGET(g_main_window), 360, 240);

    // CSS Provider
    g_css_provider = gtk_css_provider_new();
    apply_theme(g_config.dark_mode);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                              GTK_STYLE_PROVIDER(g_css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Headerbar
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);

    GtkWidget *htitle = gtk_label_new("System Settings");
    gtk_widget_add_css_class(htitle, "header-title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), htitle);

    g_status_label = gtk_label_new("Saved");
    gtk_widget_add_css_class(g_status_label, "connected-pill");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), g_status_label);

    gtk_window_set_titlebar(GTK_WINDOW(g_main_window), header);

    // Main Layout (HBox: Sidebar + Scrolled Stack)
    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    GtkWidget *sidebar = build_sidebar();
    gtk_box_append(GTK_BOX(main_hbox), sidebar);

    // Content Stack
    g_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(g_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(g_stack), 150);
    gtk_stack_set_hhomogeneous(GTK_STACK(g_stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(g_stack), FALSE);

    for (int i = 0; i < PAGES_COUNT; i++) {
        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), FALSE);
        gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), 100);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 100);
        GtkWidget *page_content = PAGES[i].builder();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page_content);
        gtk_stack_add_named(GTK_STACK(g_stack), scroll, PAGES[i].id);
    }

    gtk_widget_set_hexpand(g_stack, TRUE);
    gtk_widget_set_vexpand(g_stack, TRUE);
    gtk_box_append(GTK_BOX(main_hbox), g_stack);

    gtk_window_set_child(GTK_WINDOW(g_main_window), main_hbox);

    // Select target page or default to first
    int target_idx = 0;
    if (g_target_page[0]) {
        for (int i = 0; i < PAGES_COUNT; i++) {
            if (strcmp(PAGES[i].id, g_target_page) == 0 ||
                (strcmp(g_target_page, "displays") == 0 && strcmp(PAGES[i].id, "display") == 0)) {
                target_idx = i;
                break;
            }
        }
    }

    if (PAGES[target_idx].row_widget) {
        gtk_list_box_select_row(GTK_LIST_BOX(g_sidebar_list), GTK_LIST_BOX_ROW(PAGES[target_idx].row_widget));
    }
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), PAGES[target_idx].id);

    gtk_window_present(GTK_WINDOW(g_main_window));
}

int main(int argc, char *argv[]) {
    // Parse target page from command line if provided
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            strncpy(g_target_page, argv[i], sizeof(g_target_page) - 1);
            break;
        }
    }

    GtkApplication *app = gtk_application_new("com.aurade.settings", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    return status;
}
