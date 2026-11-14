#include "settings.h"
#include "css.h"

aurade_config_t g_config;
GtkWidget *g_main_window = NULL;
GtkWidget *g_stack = NULL;
GtkWidget *g_status_label = NULL;
GtkWidget *g_sidebar_list = NULL;
static GtkWidget *g_frame_bg = NULL;
static GtkWidget *g_sidebar_widget = NULL;
GtkCssProvider *g_css_provider = NULL;
guint g_save_timeout_id = 0;
char g_target_page[64] = {0};

static void cairo_rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI_2, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, G_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI_2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

static void draw_frame_bg(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)user_data;
    if (width <= 0 || height <= 0) return;

    cairo_save(cr);
    // Solid GTK gray outer frame with rounded bottom corners (14px) matching headerbar
    double r = 14.0;
    cairo_new_sub_path(cr);
    cairo_move_to(cr, 0.0, 0.0);
    cairo_line_to(cr, width, 0.0);
    cairo_line_to(cr, width, height - r);
    cairo_arc(cr, width - r, height - r, r, 0.0, G_PI_2);
    cairo_line_to(cr, r, height);
    cairo_arc(cr, r, height - r, r, G_PI_2, G_PI);
    cairo_close_path(cr);

    if (g_config.dark_mode) {
        cairo_set_source_rgba(cr, 0.165, 0.165, 0.180, 1.0); // #2a2a2e
    } else {
        cairo_set_source_rgba(cr, 0.910, 0.910, 0.925, 1.0); // #e8e8ec
    }
    cairo_fill(cr);

    // Punch transparent cutouts ONLY for the Liquid Glass panels (.sidebar and .content-stack)
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);

    graphene_rect_t bounds;
    graphene_rect_t sb_win = {0}, ct_win = {0};
    bool has_sb = false, has_ct = false;
    if (g_sidebar_widget && gtk_widget_compute_bounds(g_sidebar_widget, GTK_WIDGET(area), &bounds)) {
        cairo_rounded_rect_path(cr, bounds.origin.x + 1.0, bounds.origin.y + 1.0,
                                bounds.size.width - 2.0, bounds.size.height - 2.0, 15.0);
        cairo_fill(cr);
        has_sb = gtk_widget_compute_bounds(g_sidebar_widget, g_main_window, &sb_win);
    }
    if (g_stack && gtk_widget_compute_bounds(g_stack, GTK_WIDGET(area), &bounds)) {
        cairo_rounded_rect_path(cr, bounds.origin.x + 1.0, bounds.origin.y + 1.0,
                                bounds.size.width - 2.0, bounds.size.height - 2.0, 15.0);
        cairo_fill(cr);
        has_ct = gtk_widget_compute_bounds(g_stack, g_main_window, &ct_win);
    }
    if (has_sb && has_ct) {
        static int prev[8] = {0};
        int cur[8] = {
            (int)roundf(sb_win.origin.x), (int)roundf(sb_win.origin.y),
            (int)roundf(sb_win.size.width), (int)roundf(sb_win.size.height),
            (int)roundf(ct_win.origin.x), (int)roundf(ct_win.origin.y),
            (int)roundf(ct_win.size.width), (int)roundf(ct_win.size.height)
        };
        if (memcmp(prev, cur, sizeof(cur)) != 0) {
            memcpy(prev, cur, sizeof(cur));
            FILE *fp = fopen("/tmp/aurade-settings-panels.rect", "w");
            if (fp) {
                fprintf(fp, "%d %d %d %d %d %d %d %d\n",
                        cur[0], cur[1], cur[2], cur[3],
                        cur[4], cur[5], cur[6], cur[7]);
                fclose(fp);
            }
        }
    }
    cairo_restore(cr);
}

typedef struct {
    const char *id;
    const char *title;
    const char *icon;
    const char *badge_class;
    GtkWidget *(*builder)(void);
    GtkWidget *row_widget;
    GtkWidget *scroll_widget;
    bool built;
} page_meta_t;

static page_meta_t PAGES[] = {
    { "wifi",        "Wi-Fi",            "network-wireless-symbolic",             "badge-wifi",        build_page_wifi,        NULL, NULL, false },
    { "bluetooth",   "Bluetooth",        "bluetooth-symbolic",                    "badge-bluetooth",   build_page_bluetooth,   NULL, NULL, false },
    { "sound",       "Sound",            "audio-volume-high-symbolic",            "badge-sound",       build_page_sound,       NULL, NULL, false },
    { "display",     "Displays",         "video-display-symbolic",                "badge-display",     build_page_display,     NULL, NULL, false },
    { "workspaces",  "Workspaces",       "view-grid-symbolic",                    "badge-workspaces",  build_page_workspaces,  NULL, NULL, false },
    { "mouse",       "Mouse & Touchpad", "input-mouse-symbolic",                  "badge-mouse",       build_page_mouse,       NULL, NULL, false },
    { "keyboard",    "Keyboard",         "input-keyboard-symbolic",               "badge-keyboard",    build_page_keyboard,    NULL, NULL, false },
    { "appearance",  "Appearance",       "preferences-desktop-wallpaper-symbolic","badge-appearance", build_page_appearance,  NULL, NULL, false },
    { "blur-glass",  "Blur & Glass",     "applications-graphics-symbolic",        "badge-appearance", build_page_blur_glass,  NULL, NULL, false },
    { "dock",        "Dock",             "view-paged-symbolic",                   "badge-dock",        build_page_dock,        NULL, NULL, false },
    { "gestures",    "Gestures",         "input-touchpad-symbolic",               "badge-gestures",    build_page_gestures,    NULL, NULL, false },
    { "default-apps","Default Apps",     "preferences-desktop-apps-symbolic",     "badge-appearance",  build_page_default_apps,NULL, NULL, false },
    { "about",       "About",            "help-about-symbolic",                   "badge-about",       build_page_about,       NULL, NULL, false },
};
static const int PAGES_COUNT = sizeof(PAGES) / sizeof(PAGES[0]);

void apply_theme(bool dark_mode) {
    if (!g_css_provider) return;
    const char *css = settings_get_css(dark_mode);
    gtk_css_provider_load_from_string(g_css_provider, css);
    if (g_frame_bg) gtk_widget_queue_draw(g_frame_bg);
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

static void ensure_page_built(int idx) {
    if (idx < 0 || idx >= PAGES_COUNT) return;
    if (PAGES[idx].built) return;

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), 100);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 100);

    GtkWidget *page_content = PAGES[idx].builder();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page_content);
    gtk_stack_add_named(GTK_STACK(g_stack), scroll, PAGES[idx].id);

    PAGES[idx].scroll_widget = scroll;
    PAGES[idx].built = true;
}

static void select_page_by_index(int idx) {
    if (idx < 0 || idx >= PAGES_COUNT) return;
    ensure_page_built(idx);
    if (g_sidebar_list && PAGES[idx].row_widget) {
        gtk_list_box_select_row(GTK_LIST_BOX(g_sidebar_list), GTK_LIST_BOX_ROW(PAGES[idx].row_widget));
    }
    if (g_stack) {
        gtk_stack_set_visible_child_name(GTK_STACK(g_stack), PAGES[idx].id);
    }
}

static void select_page_by_id(const char *page_id) {
    if (!page_id || !*page_id) return;
    for (int i = 0; i < PAGES_COUNT; i++) {
        if (strcmp(PAGES[i].id, page_id) == 0 ||
            (strcmp(page_id, "displays") == 0 && strcmp(PAGES[i].id, "display") == 0)) {
            select_page_by_index(i);
            return;
        }
    }
}

static void on_sidebar_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "page_idx"));
    if (idx >= 0 && idx < PAGES_COUNT) {
        select_page_by_index(idx);
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

    // If already running, switch to target page and present window
    if (g_main_window) {
        if (g_target_page[0]) {
            select_page_by_id(g_target_page);
        }
        gtk_window_present(GTK_WINDOW(g_main_window));
        return;
    }

    // Load configuration
    if (!config_load(&g_config, NULL)) {
        g_config = config_get_defaults();
    }

    // Main window
    g_main_window = gtk_application_window_new(app);
    gtk_widget_add_css_class(g_main_window, "settings-main-window");
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
    gtk_widget_add_css_class(main_hbox, "main-hbox");

    g_sidebar_widget = build_sidebar();
    gtk_box_append(GTK_BOX(main_hbox), g_sidebar_widget);

    // Content Stack
    g_stack = gtk_stack_new();
    gtk_widget_add_css_class(g_stack, "content-stack");
    gtk_stack_set_transition_type(GTK_STACK(g_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(g_stack), 150);
    gtk_stack_set_hhomogeneous(GTK_STACK(g_stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(g_stack), FALSE);

    gtk_widget_set_hexpand(g_stack, TRUE);
    gtk_widget_set_vexpand(g_stack, TRUE);
    gtk_box_append(GTK_BOX(main_hbox), g_stack);

    GtkWidget *overlay = gtk_overlay_new();
    g_frame_bg = gtk_drawing_area_new();
    gtk_widget_set_hexpand(g_frame_bg, TRUE);
    gtk_widget_set_vexpand(g_frame_bg, TRUE);
    gtk_widget_set_can_target(g_frame_bg, FALSE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_frame_bg), draw_frame_bg, NULL, NULL);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), g_frame_bg);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), main_hbox);

    gtk_window_set_child(GTK_WINDOW(g_main_window), overlay);

    // Select target page or default to first (lazy builds that page only)
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

    select_page_by_index(target_idx);

    gtk_window_present(GTK_WINDOW(g_main_window));
}

static int on_app_command_line(GApplication *app, GApplicationCommandLine *cmdline, gpointer user_data) {
    (void)user_data;
    int argc = 0;
    char **argv = g_application_command_line_get_arguments(cmdline, &argc);
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            target = argv[i];
            break;
        }
    }
    if (target && *target) {
        strncpy(g_target_page, target, sizeof(g_target_page) - 1);
        g_target_page[sizeof(g_target_page) - 1] = '\0';
    }
    g_strfreev(argv);

    g_application_activate(app);
    return 0;
}

int main(int argc, char *argv[]) {
    // Parse target page from command line if provided
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            strncpy(g_target_page, argv[i], sizeof(g_target_page) - 1);
            break;
        }
    }

    GtkApplication *app = gtk_application_new("com.aurade.settings", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(on_app_command_line), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
