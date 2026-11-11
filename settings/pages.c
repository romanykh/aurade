#include "settings.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/utsname.h>

const action_item_t GESTURE_ACTIONS[] = {
    { ACTION_MISSION_CONTROL, "mission_control", "Mission Control" },
    { ACTION_APP_EXPOSE,     "app_expose",      "App Exposé" },
    { ACTION_WORKSPACE_NEXT,  "workspace_next",  "Next Workspace" },
    { ACTION_WORKSPACE_PREV,  "workspace_prev",  "Previous Workspace" },
    { ACTION_LAUNCHPAD,       "launchpad",       "Launchpad" },
    { ACTION_SHOW_DESKTOP,    "show_desktop",    "Show Desktop" },
    { ACTION_CONTROL_CENTER,  "control_center",  "Control Center" },
    { ACTION_NONE,            "none",            "Disabled" },
};
const int GESTURE_ACTIONS_COUNT = sizeof(GESTURE_ACTIONS) / sizeof(GESTURE_ACTIONS[0]);

static const char *const ACTION_LABELS[] = {
    "Mission Control",
    "App Exposé",
    "Next Workspace",
    "Previous Workspace",
    "Launchpad",
    "Show Desktop",
    "Control Center",
    "Disabled",
    NULL
};

static int action_to_dropdown_index(gesture_action_type_t act) {
    for (int i = 0; i < GESTURE_ACTIONS_COUNT; i++) {
        if (GESTURE_ACTIONS[i].type == act) return i;
    }
    return GESTURE_ACTIONS_COUNT - 1; // Disabled
}

static gesture_action_type_t dropdown_index_to_action(int idx) {
    if (idx >= 0 && idx < GESTURE_ACTIONS_COUNT) {
        return GESTURE_ACTIONS[idx].type;
    }
    return ACTION_NONE;
}

// -------------------------------------------------------------
// PAGE: WI-FI
// -------------------------------------------------------------

static void on_wifi_power_toggled(bool active, gpointer user_data) {
    (void)user_data;
    if (active) {
        run_cmd_async("nmcli radio wifi on");
    } else {
        run_cmd_async("nmcli radio wifi off");
    }
}

static void on_wifi_disconnect_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    run_cmd_async("nmcli dev disconnect $(nmcli -t -f DEVICE,TYPE dev 2>/dev/null | grep ':wifi' | cut -d: -f1 | head -n1)");
}

typedef struct {
    char ssid[128];
    GtkWidget *entry;
    GtkWidget *popover;
} wifi_connect_data_t;

static void on_wifi_connect_submit(GtkButton *btn, gpointer user_data) {
    (void)btn;
    wifi_connect_data_t *d = user_data;
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(d->entry));
    char cmd[512];
    if (pw && *pw) {
        snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect \"%s\" password \"%s\" >/dev/null 2>&1 &", d->ssid, pw);
    } else {
        snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect \"%s\" >/dev/null 2>&1 &", d->ssid);
    }
    run_cmd_async(cmd);
    if (d->popover) gtk_popover_popdown(GTK_POPOVER(d->popover));
}

static void on_wifi_connect_clicked(GtkButton *btn, gpointer user_data) {
    const char *ssid = (const char *)user_data;
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);

    char title_buf[196];
    snprintf(title_buf, sizeof(title_buf), "Connect to \"%s\"", ssid);
    GtkWidget *lbl = gtk_label_new(title_buf);
    gtk_widget_add_css_class(lbl, "title");
    gtk_box_append(GTK_BOX(box), lbl);

    GtkWidget *entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
    gtk_box_append(GTK_BOX(box), entry);

    wifi_connect_data_t *d = g_new0(wifi_connect_data_t, 1);
    strncpy(d->ssid, ssid, sizeof(d->ssid) - 1);
    d->entry = entry;
    d->popover = pop;

    GtkWidget *btn_conn = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(btn_conn, "btn-primary");
    g_signal_connect(btn_conn, "clicked", G_CALLBACK(on_wifi_connect_submit), d);
    gtk_box_append(GTK_BOX(box), btn_conn);

    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_widget_set_parent(pop, GTK_WIDGET(btn));
    gtk_popover_popup(GTK_POPOVER(pop));
}

GtkWidget *build_page_wifi(void) {
    GtkWidget *page = ui_page_container("Wi-Fi", "Connect to wireless networks, manage internet connections and security.");

    // 1. Wi-Fi Power Switch Card
    char radio_out[32] = "";
    bool wifi_enabled = true;
    if (run_cmd_capture("nmcli radio wifi 2>/dev/null", radio_out, sizeof(radio_out))) {
        wifi_enabled = (strstr(radio_out, "enabled") != NULL);
    }
    GtkWidget *c_pwr = ui_card(page, NULL);
    GtkWidget *sw_pwr = ui_switch(wifi_enabled, on_wifi_power_toggled, NULL);
    ui_add_row(c_pwr, "Wi-Fi Power", "Turn Wi-Fi wireless networking on or off", sw_pwr, "network-wireless-symbolic", "badge-wifi");

    // 2. Active Connection Card
    char act_buf[256] = "";
    bool has_active = run_cmd_capture("nmcli -t -f TYPE,STATE,CONNECTION dev 2>/dev/null | grep '^wifi:connected' | cut -d: -f3",
                                      act_buf, sizeof(act_buf));
    if (has_active && act_buf[0]) {
        GtkWidget *c_act = ui_card(page, "CURRENT NETWORK");
        GtkWidget *btn_disc = gtk_button_new_with_label("Disconnect");
        gtk_widget_add_css_class(btn_disc, "btn-destructive");
        g_signal_connect(btn_disc, "clicked", G_CALLBACK(on_wifi_disconnect_clicked), NULL);
        ui_add_row(c_act, act_buf, "Connected and active", btn_disc, "network-wireless-symbolic", "badge-wifi");
    }

    // 3. Known / Available Networks Card
    GtkWidget *c_nets = ui_card(page, "AVAILABLE NETWORKS");
    char scan_buf[4096] = "";
    if (run_cmd_capture("nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list 2>/dev/null | head -n 12", scan_buf, sizeof(scan_buf))) {
        char *line = strtok(scan_buf, "\n");
        int count = 0;
        while (line && count < 8) {
            char in_use[8] = "", ssid[128] = "", sig[16] = "", sec[64] = "";
            char *colon1 = strchr(line, ':');
            if (colon1) {
                *colon1 = '\0';
                strncpy(in_use, line, sizeof(in_use) - 1);
                char *colon2 = strchr(colon1 + 1, ':');
                if (colon2) {
                    *colon2 = '\0';
                    strncpy(ssid, colon1 + 1, sizeof(ssid) - 1);
                    char *colon3 = strchr(colon2 + 1, ':');
                    if (colon3) {
                        *colon3 = '\0';
                        strncpy(sig, colon2 + 1, sizeof(sig) - 1);
                        strncpy(sec, colon3 + 1, sizeof(sec) - 1);
                    }
                }
            }
            if (ssid[0] != '\0') {
                char sub_buf[128];
                snprintf(sub_buf, sizeof(sub_buf), "Signal: %s%%  •  Security: %s", sig, sec[0] ? sec : "Open");

                GtkWidget *btn = gtk_button_new_with_label("Connect");
                gtk_widget_add_css_class(btn, "btn-primary");
                g_signal_connect(btn, "clicked", G_CALLBACK(on_wifi_connect_clicked), g_strdup(ssid));

                const char *icon = (strstr(sec, "WPA") || strstr(sec, "WEP")) ? "network-wireless-encrypted-symbolic" : "network-wireless-symbolic";
                ui_add_row(c_nets, ssid, sub_buf, btn, icon, "badge-wifi");
                count++;
            }
            line = strtok(NULL, "\n");
        }
        if (count == 0) {
            ui_add_row(c_nets, "No Wi-Fi Networks Found", "Ensure Wi-Fi is enabled and in range", NULL, "network-wireless-offline-symbolic", "badge-wifi");
        }
    } else {
        ui_add_row(c_nets, "Scanning for Wi-Fi Networks...", "Please wait a moment", NULL, "network-wireless-symbolic", "badge-wifi");
    }

    return page;
}

// -------------------------------------------------------------
// PAGE: BLUETOOTH
// -------------------------------------------------------------

static void on_bt_power_toggled(bool active, gpointer user_data) {
    (void)user_data;
    if (active) run_cmd_async("bluetoothctl power on");
    else run_cmd_async("bluetoothctl power off");
}

static void on_bt_disc_toggled(bool active, gpointer user_data) {
    (void)user_data;
    if (active) run_cmd_async("bluetoothctl discoverable on");
    else run_cmd_async("bluetoothctl discoverable off");
}

static void on_bt_device_action(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *action_and_mac = (const char *)user_data;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "bluetoothctl %s >/dev/null 2>&1 &", action_and_mac);
    run_cmd_async(cmd);
}

GtkWidget *build_page_bluetooth(void) {
    GtkWidget *page = ui_page_container("Bluetooth", "Connect wireless headphones, mice, keyboards and accessories.");

    // Power & Discoverable
    char bt_show[1024] = "";
    bool powered = true, disc = false;
    if (run_cmd_capture("bluetoothctl show 2>/dev/null", bt_show, sizeof(bt_show))) {
        powered = (strstr(bt_show, "Powered: yes") != NULL);
        disc = (strstr(bt_show, "Discoverable: yes") != NULL);
    }

    GtkWidget *c_pwr = ui_card(page, NULL);
    GtkWidget *sw_pwr = ui_switch(powered, on_bt_power_toggled, NULL);
    ui_add_row(c_pwr, "Bluetooth Power", "Enable or disable Bluetooth radio adapter", sw_pwr, "bluetooth-symbolic", "badge-bluetooth");

    GtkWidget *sw_disc = ui_switch(disc, on_bt_disc_toggled, NULL);
    ui_add_row(c_pwr, "Discoverable", "Allow other devices to discover this computer", sw_disc, "bluetooth-active-symbolic", "badge-bluetooth");

    // Paired Devices
    GtkWidget *c_devs = ui_card(page, "MY DEVICES");
    char dev_buf[2048] = "";
    int dev_count = 0;
    if (run_cmd_capture("bluetoothctl devices Paired 2>/dev/null || bluetoothctl devices 2>/dev/null", dev_buf, sizeof(dev_buf))) {
        char *line = strtok(dev_buf, "\n");
        while (line && dev_count < 8) {
            // format: Device 00:11:22:33:44:55 DeviceName
            if (strncmp(line, "Device ", 7) == 0) {
                char mac[32] = {0};
                char name[128] = {0};
                sscanf(line + 7, "%31s %[^\n]", mac, name);
                if (mac[0] && name[0]) {
                    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                    GtkWidget *btn_conn = gtk_button_new_with_label("Connect");
                    gtk_widget_add_css_class(btn_conn, "btn-primary");
                    char *arg_conn = g_strdup_printf("connect %s", mac);
                    g_signal_connect(btn_conn, "clicked", G_CALLBACK(on_bt_device_action), arg_conn);
                    gtk_box_append(GTK_BOX(box), btn_conn);

                    GtkWidget *btn_disc = gtk_button_new_with_label("Disconnect");
                    char *arg_disc = g_strdup_printf("disconnect %s", mac);
                    g_signal_connect(btn_disc, "clicked", G_CALLBACK(on_bt_device_action), arg_disc);
                    gtk_box_append(GTK_BOX(box), btn_disc);

                    GtkWidget *btn_rem = gtk_button_new_with_label("Forget");
                    gtk_widget_add_css_class(btn_rem, "btn-destructive");
                    char *arg_rem = g_strdup_printf("remove %s", mac);
                    g_signal_connect(btn_rem, "clicked", G_CALLBACK(on_bt_device_action), arg_rem);
                    gtk_box_append(GTK_BOX(box), btn_rem);

                    ui_add_row(c_devs, name, mac, box, "audio-headphones-symbolic", "badge-bluetooth");
                    dev_count++;
                }
            }
            line = strtok(NULL, "\n");
        }
    }
    if (dev_count == 0) {
        ui_add_row(c_devs, "No Paired Devices", "Put your Bluetooth device into pairing mode to connect", NULL, "bluetooth-symbolic", "badge-bluetooth");
    }

    return page;
}

// -------------------------------------------------------------
// PAGE: SOUND
// -------------------------------------------------------------

static void on_sound_vol_changed(double val, gpointer user_data) {
    (void)user_data;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ %.2f 2>/dev/null || pactl set-sink-volume @DEFAULT_SINK@ %d%% 2>/dev/null",
             val / 100.0, (int)val);
    run_cmd_async(cmd);
}

static void on_sound_mute_changed(bool active, gpointer user_data) {
    (void)user_data;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wpctl set-mute @DEFAULT_AUDIO_SINK@ %d 2>/dev/null || pactl set-sink-mute @DEFAULT_SINK@ %d 2>/dev/null",
             active ? 1 : 0, active ? 1 : 0);
    run_cmd_async(cmd);
}

static void on_sound_test_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    run_cmd_async("canberra-gtk-play -i bell 2>/dev/null || paplay /usr/share/sounds/freedesktop/stereo/bell.oga 2>/dev/null || speaker-test -t sine -f 440 -l 1 >/dev/null 2>&1 &");
}

GtkWidget *build_page_sound(void) {
    GtkWidget *page = ui_page_container("Sound", "Configure speaker volume, microphones, and audio output devices.");

    double cur_vol = 50.0;
    bool is_muted = false;
    char vol_buf[128] = "";
    if (run_cmd_capture("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", vol_buf, sizeof(vol_buf))) {
        double v = 0.0;
        if (sscanf(vol_buf, "Volume: %lf", &v) == 1) {
            cur_vol = v * 100.0;
            is_muted = (strstr(vol_buf, "[MUTED]") != NULL);
        }
    }

    // Output Card
    GtkWidget *c_out = ui_card(page, "OUTPUT");
    GtkWidget *sc_vol = ui_scale(cur_vol, 0.0, 100.0, 1.0, 0, on_sound_vol_changed, NULL);
    ui_add_row(c_out, "Output Volume", "Master system playback volume", sc_vol, "audio-volume-high-symbolic", "badge-sound");

    GtkWidget *sw_mute = ui_switch(is_muted, on_sound_mute_changed, NULL);
    ui_add_row(c_out, "Mute Audio", "Mute all sound output immediately", sw_mute, "audio-volume-muted-symbolic", "badge-sound");

    GtkWidget *btn_test = gtk_button_new_with_label("Play Alert Sound");
    gtk_widget_add_css_class(btn_test, "btn-primary");
    g_signal_connect(btn_test, "clicked", G_CALLBACK(on_sound_test_clicked), NULL);
    ui_add_row(c_out, "Sound Test", "Verify audio playback and speaker balance", btn_test, "audio-speakers-symbolic", "badge-sound");

    // Input Card
    GtkWidget *c_in = ui_card(page, "INPUT");
    GtkWidget *sc_in = ui_scale(65.0, 0.0, 100.0, 1.0, 0, NULL, NULL);
    ui_add_row(c_in, "Microphone Volume", "Default recording and microphone input level", sc_in, "audio-input-microphone-symbolic", "badge-sound");

    return page;
}

// -------------------------------------------------------------
// PAGE: DISPLAYS
// -------------------------------------------------------------

static void on_brightness_changed(double val, gpointer user_data) {
    (void)user_data;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "brightnessctl set %d%% 2>/dev/null", (int)val);
    run_cmd_async(cmd);
}

static void on_night_light_toggled(bool active, gpointer user_data) {
    (void)user_data;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "gsettings set org.gnome.settings-daemon.plugins.color night-light-enabled %s 2>/dev/null",
             active ? "true" : "false");
    run_cmd_async(cmd);
}

GtkWidget *build_page_display(void) {
    GtkWidget *page = ui_page_container("Displays", "Set display brightness, scaling resolution, and Night Light.");

    double cur_br = 80.0;
    char br_buf[128] = "";
    if (run_cmd_capture("brightnessctl -m 2>/dev/null", br_buf, sizeof(br_buf))) {
        char *pct_str = strrchr(br_buf, ',');
        if (pct_str) {
            int p = atoi(pct_str + 1);
            if (p > 0 && p <= 100) cur_br = p;
        }
    }

    GtkWidget *c_disp = ui_card(page, "DISPLAY BRIGHTNESS");
    GtkWidget *sc_br = ui_scale(cur_br, 1.0, 100.0, 1.0, 0, on_brightness_changed, NULL);
    ui_add_row(c_disp, "Brightness", "Adjust built-in panel backlighting", sc_br, "display-brightness-symbolic", "badge-display");

    GtkWidget *sw_nl = ui_switch(false, on_night_light_toggled, NULL);
    ui_add_row(c_disp, "Night Light", "Shift display colors to warmer tones to reduce eye strain", sw_nl, "weather-clear-night-symbolic", "badge-display");

    static const char *const SCALING_OPTIONS[] = { "100% (Standard)", "125%", "150%", "200% (HiDPI / Retina)", NULL };
    GtkWidget *dd_scale = ui_dropdown(SCALING_OPTIONS, 4, 0, NULL, NULL);
    ui_add_row(c_disp, "Display Scaling", "Scales user interface elements for high-density screens", dd_scale, "video-display-symbolic", "badge-display");

    return page;
}

// -------------------------------------------------------------
// PAGE: WORKSPACES
// -------------------------------------------------------------

static void on_workspace_count_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)user_data;
    int count = gtk_spin_button_get_value_as_int(spin);
    if (count >= 1 && count <= 9) {
        g_config.num_workspaces = count;
        settings_schedule_save();
    }
}

static void on_autofocus_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.workspace_auto_focus = active;
}

static void on_tiling_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.tiling_enabled = active;
}

static void on_tiling_gap_changed(double val, gpointer user_data) {
    (void)user_data;
    g_config.tiling_gap = (int)val;
}

GtkWidget *build_page_workspaces(void) {
    GtkWidget *page = ui_page_container("Workspaces", "Organize windows across virtual desktops and spaces.");

    GtkWidget *c_ws = ui_card(page, "DESKTOPS & SPACES");

    GtkWidget *spin = gtk_spin_button_new_with_range(1, 9, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), g_config.num_workspaces > 0 ? g_config.num_workspaces : 4);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_workspace_count_changed), NULL);
    ui_add_row(c_ws, "Workspace Count", "Number of virtual desktop spaces (1 to 9)", spin, "view-grid-symbolic", "badge-workspaces");

    GtkWidget *sw_af = ui_switch(g_config.workspace_auto_focus, on_autofocus_toggled, NULL);
    ui_add_row(c_ws, "Auto Focus", "Focus active window when switching between spaces", sw_af, "input-mouse-symbolic", "badge-workspaces");

    GtkWidget *c_tile = ui_card(page, "WINDOW TILING");
    GtkWidget *sw_tile = ui_switch(g_config.tiling_enabled, on_tiling_toggled, NULL);
    ui_add_row(c_tile, "Auto-Tiling Mode", "Automatically arrange open windows side-by-side without overlapping", sw_tile, "view-compact-symbolic", "badge-workspaces");

    GtkWidget *sc_gap = ui_scale(g_config.tiling_gap > 0 ? g_config.tiling_gap : 10, 0, 40, 1, 0, on_tiling_gap_changed, NULL);
    ui_add_row(c_tile, "Tiling Gap Size", "Inner and outer margins between tiled windows (pixels)", sc_gap, "view-paged-symbolic", "badge-workspaces");

    return page;
}

// -------------------------------------------------------------
// PAGE: MOUSE & TOUCHPAD
// -------------------------------------------------------------

static void on_natural_scroll_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.natural_scroll = active;
}

static void on_dwt_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.disable_while_typing = active;
}

static void on_sensitivity_changed(double val, gpointer user_data) {
    (void)user_data;
    g_config.sensitivity = val;
}

GtkWidget *build_page_mouse(void) {
    GtkWidget *page = ui_page_container("Mouse & Touchpad", "Configure pointer speed, gestures scrolling direction and typing lock.");

    GtkWidget *c_tp = ui_card(page, "TOUCHPAD & TRACKPAD");

    GtkWidget *sw_nat = ui_switch(g_config.natural_scroll, on_natural_scroll_toggled, NULL);
    ui_add_row(c_tp, "Natural Scrolling", "Content tracks finger movement directly (macOS style)", sw_nat, "input-mouse-symbolic", "badge-mouse");

    GtkWidget *sw_dwt = ui_switch(g_config.disable_while_typing, on_dwt_toggled, NULL);
    ui_add_row(c_tp, "Disable While Typing", "Lock touchpad cursor while typing on keyboard to avoid accidental clicks", sw_dwt, "input-keyboard-symbolic", "badge-mouse");

    GtkWidget *sc_sens = ui_scale(g_config.sensitivity > 0.1 ? g_config.sensitivity : 1.0, 0.4, 3.0, 0.05, 2, on_sensitivity_changed, NULL);
    ui_add_row(c_tp, "Tracking Speed", "Touchpad cursor tracking and motion sensitivity multiplier", sc_sens, "preferences-system-symbolic", "badge-mouse");

    return page;
}

// -------------------------------------------------------------
// PAGE: KEYBOARD
// -------------------------------------------------------------

static void on_layout_changed(const char *text, gpointer user_data) {
    (void)user_data;
    strncpy(g_config.kb_layout, text, sizeof(g_config.kb_layout) - 1);
}

static void on_variant_changed(const char *text, gpointer user_data) {
    (void)user_data;
    strncpy(g_config.kb_variant, text, sizeof(g_config.kb_variant) - 1);
}

GtkWidget *build_page_keyboard(void) {
    GtkWidget *page = ui_page_container("Keyboard", "Configure typing layouts, layout switching shortcuts and repeats.");

    GtkWidget *c_kb = ui_card(page, "INPUT LAYOUTS");
    GtkWidget *en_lay = ui_entry(g_config.kb_layout[0] ? g_config.kb_layout : "us,ua", "e.g. us,ua", on_layout_changed, NULL);
    ui_add_row(c_kb, "Keyboard Layouts", "Comma-separated XKB layout codes (e.g. us, ua, de, fr)", en_lay, "input-keyboard-symbolic", "badge-keyboard");

    GtkWidget *en_var = ui_entry(g_config.kb_variant, "e.g. ,winkeysenhanced", on_variant_changed, NULL);
    ui_add_row(c_kb, "Layout Variant", "Optional XKB variant mappings", en_var, "preferences-system-symbolic", "badge-keyboard");

    GtkWidget *c_sh = ui_card(page, "SHORTCUTS QUICK REFERENCE");
    ui_add_row(c_sh, "Switch Input Language", "Cycle through configured layouts", gtk_label_new("Super + Space"), "input-keyboard-symbolic", "badge-keyboard");
    ui_add_row(c_sh, "Mission Control Overview", "View all open windows across desktops", gtk_label_new("Super + Up  /  3-Finger Swipe Up"), "view-grid-symbolic", "badge-keyboard");
    ui_add_row(c_sh, "Launchpad App Grid", "Browse installed applications", gtk_label_new("Super + P  /  3-Finger Pinch In"), "view-paged-symbolic", "badge-keyboard");
    ui_add_row(c_sh, "Control Center", "Open quick system controls panel", gtk_label_new("Super + C  /  Right Edge Swipe"), "preferences-system-symbolic", "badge-keyboard");
    ui_add_row(c_sh, "Close Active Window", "Dismiss active application window", gtk_label_new("Super + Q"), "window-close-symbolic", "badge-keyboard");

    return page;
}

// -------------------------------------------------------------
// PAGE: APPEARANCE
// -------------------------------------------------------------

static void on_dark_mode_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.dark_mode = active;
    apply_theme(active);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "gsettings set org.gnome.desktop.interface color-scheme '%s' 2>/dev/null",
             active ? "prefer-dark" : "default");
    run_cmd_async(cmd);
}

static void on_wallpaper_file_set(GtkFileDialog *dialog, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, NULL);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            strncpy(g_config.wallpaper_path, path, sizeof(g_config.wallpaper_path) - 1);
            settings_schedule_save();
            g_free(path);
        }
        g_object_unref(file);
    }
}

static void on_browse_wallpaper_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Wallpaper Image");
    gtk_file_dialog_open(dialog, GTK_WINDOW(g_main_window), NULL, (GAsyncReadyCallback)on_wallpaper_file_set, NULL);
}

static void on_accent_color_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *hex = (const char *)user_data;
    strncpy(g_config.accent_color, hex, sizeof(g_config.accent_color) - 1);
    settings_schedule_save();
}

static void on_window_opacity_changed(double val, gpointer user_data) {
    (void)user_data;
    g_config.window_opacity = val;
}

static void on_blur_strength_changed(double val, gpointer user_data) {
    (void)user_data;
    g_config.blur_strength = val;
}

GtkWidget *build_page_appearance(void) {
    GtkWidget *page = ui_page_container("Appearance", "Customize desktop wallpaper, dark mode theme and accent styling.");

    GtkWidget *c_theme = ui_card(page, "THEME & COLORS");
    GtkWidget *sw_dark = ui_switch(g_config.dark_mode, on_dark_mode_toggled, NULL);
    ui_add_row(c_theme, "Dark Mode", "Use darker color palette across system panels and desktop widgets", sw_dark, "weather-clear-night-symbolic", "badge-appearance");

    // Accent Colors
    GtkWidget *box_accents = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    static const struct { const char *hex; const char *name; } ACCENTS[] = {
        {"#007AFF", "Blue"},
        {"#AF52DE", "Purple"},
        {"#FF2D55", "Pink"},
        {"#FF9500", "Orange"},
        {"#34C759", "Green"},
        {"#8E8E93", "Graphite"},
    };
    for (size_t i = 0; i < sizeof(ACCENTS)/sizeof(ACCENTS[0]); i++) {
        GtkWidget *b = gtk_button_new_with_label(ACCENTS[i].name);
        g_signal_connect(b, "clicked", G_CALLBACK(on_accent_color_clicked), (gpointer)ACCENTS[i].hex);
        gtk_box_append(GTK_BOX(box_accents), b);
    }
    ui_add_row(c_theme, "Accent Color", "Highlighted buttons, selections and accents", box_accents, "preferences-color-symbolic", "badge-appearance");

    // Wallpaper
    GtkWidget *c_wall = ui_card(page, "DESKTOP WALLPAPER");
    GtkWidget *btn_wall = gtk_button_new_with_label("Browse Wallpaper…");
    gtk_widget_add_css_class(btn_wall, "btn-primary");
    g_signal_connect(btn_wall, "clicked", G_CALLBACK(on_browse_wallpaper_clicked), NULL);
    const char *wp_sub = g_config.wallpaper_path[0] ? g_config.wallpaper_path : "Default system macOS wallpaper";
    ui_add_row(c_wall, "Background Image", wp_sub, btn_wall, "preferences-desktop-wallpaper-symbolic", "badge-appearance");

    // Blur & Transparency
    GtkWidget *c_blur = ui_card(page, "TRANSPARENCY & BLUR");
    GtkWidget *sc_op = ui_scale(g_config.window_opacity > 0.1 ? g_config.window_opacity : 1.0, 0.4, 1.0, 0.05, 2, on_window_opacity_changed, NULL);
    ui_add_row(c_blur, "Window Opacity", "Base opacity for inactive and transparent windows", sc_op, "view-dual-symbolic", "badge-appearance");

    GtkWidget *sc_blur = ui_scale(g_config.blur_strength > 0.0 ? g_config.blur_strength : 1.0, 0.0, 2.0, 0.1, 1, on_blur_strength_changed, NULL);
    ui_add_row(c_blur, "Blur Strength", "Gaussian blur intensity applied behind translucent UI surfaces", sc_blur, "preferences-desktop-display-symbolic", "badge-appearance");

    return page;
}

// -------------------------------------------------------------
// PAGE: DOCK
// -------------------------------------------------------------

static void on_dock_autohide_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.dock_autohide = active;
}

static void on_dock_intellihide_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.dock_intellihide = active;
}

static void on_dock_magnification_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.dock_magnification = active;
}

static void on_dock_size_changed(double val, gpointer user_data) {
    (void)user_data;
    g_config.dock_icon_size = (int)val;
}

static void on_dock_opacity_changed(double val, gpointer user_data) {
    (void)user_data;
    g_config.dock_opacity = val;
}

static void on_dock_theme_changed(int idx, gpointer user_data) {
    (void)user_data;
    if (idx == 0) strncpy(g_config.dock_theme, DOCK_THEME_MODERN, sizeof(g_config.dock_theme) - 1);
    else if (idx == 1) strncpy(g_config.dock_theme, DOCK_THEME_TABLE_3D, sizeof(g_config.dock_theme) - 1);
    else if (idx == 2) strncpy(g_config.dock_theme, DOCK_THEME_PLANK, sizeof(g_config.dock_theme) - 1);
}

GtkWidget *build_page_dock(void) {
    GtkWidget *page = ui_page_container("Dock", "Position, auto-hide, magnification effects and app launcher icons.");

    GtkWidget *c_behave = ui_card(page, "BEHAVIOR");
    GtkWidget *sw_ah = ui_switch(g_config.dock_autohide, on_dock_autohide_toggled, NULL);
    ui_add_row(c_behave, "Automatically Hide and Show", "Dock stays hidden until cursor moves to the bottom screen edge", sw_ah, "view-paged-symbolic", "badge-dock");

    GtkWidget *sw_ih = ui_switch(g_config.dock_intellihide, on_dock_intellihide_toggled, NULL);
    ui_add_row(c_behave, "Intellihide", "Hide Dock only when an active window overlaps its bounds", sw_ih, "window-maximize-symbolic", "badge-dock");

    GtkWidget *sw_mag = ui_switch(g_config.dock_magnification, on_dock_magnification_toggled, NULL);
    ui_add_row(c_behave, "Magnification Effect", "Smooth fish-eye icon zoom when pointer hovers over the Dock", sw_mag, "zoom-in-symbolic", "badge-dock");

    GtkWidget *c_look = ui_card(page, "APPEARANCE");
    GtkWidget *sc_sz = ui_scale(g_config.dock_icon_size > 20 ? g_config.dock_icon_size : 52, 36, 88, 2, 0, on_dock_size_changed, NULL);
    ui_add_row(c_look, "Icon Size", "Base pixel dimension of Dock application icons", sc_sz, "preferences-system-symbolic", "badge-dock");

    GtkWidget *sc_op = ui_scale(g_config.dock_opacity > 0.05 ? g_config.dock_opacity : 0.85, 0.20, 1.00, 0.05, 2, on_dock_opacity_changed, NULL);
    ui_add_row(c_look, "Dock Opacity", "Frosted glass translucency level", sc_op, "view-dual-symbolic", "badge-dock");

    static const char *const DOCK_THEMES[] = { "Modern Glass (macOS Sonoma)", "3D Glass Table", "Minimal Plank", NULL };
    int sel_theme = 0;
    if (strcmp(g_config.dock_theme, DOCK_THEME_TABLE_3D) == 0) sel_theme = 1;
    else if (strcmp(g_config.dock_theme, DOCK_THEME_PLANK) == 0) sel_theme = 2;
    GtkWidget *dd_th = ui_dropdown(DOCK_THEMES, 3, sel_theme, on_dock_theme_changed, NULL);
    ui_add_row(c_look, "Dock Visual Theme", "Glass shelf styling and lighting mode", dd_th, "preferences-desktop-theme-symbolic", "badge-dock");

    return page;
}

// -------------------------------------------------------------
// PAGE: GESTURES
// -------------------------------------------------------------

static void on_gesture_3_up(int idx, gpointer user_data) { (void)user_data; g_config.swipe_up_3.type = dropdown_index_to_action(idx); }
static void on_gesture_3_down(int idx, gpointer user_data) { (void)user_data; g_config.swipe_down_3.type = dropdown_index_to_action(idx); }
static void on_gesture_3_left(int idx, gpointer user_data) { (void)user_data; g_config.swipe_left_3.type = dropdown_index_to_action(idx); }
static void on_gesture_3_right(int idx, gpointer user_data) { (void)user_data; g_config.swipe_right_3.type = dropdown_index_to_action(idx); }
static void on_gesture_3_pin(int idx, gpointer user_data) { (void)user_data; g_config.pinch_in_3.type = dropdown_index_to_action(idx); }
static void on_gesture_3_pout(int idx, gpointer user_data) { (void)user_data; g_config.pinch_out_3.type = dropdown_index_to_action(idx); }

static void on_spring_stiff_changed(double val, gpointer user_data) { (void)user_data; g_config.spring.stiffness = val; }
static void on_spring_damp_changed(double val, gpointer user_data) { (void)user_data; g_config.spring.damping = val; }
static void on_spring_mass_changed(double val, gpointer user_data) { (void)user_data; g_config.spring.mass = val; }

typedef struct {
    GtkWidget *bar;
    GtkWidget *lbl;
    int fd;
} telemetry_ui_t;

static gboolean on_telemetry_socket_data(GIOChannel *source, GIOCondition cond, gpointer user_data) {
    (void)source;
    telemetry_ui_t *t = user_data;
    if (cond & (G_IO_HUP | G_IO_ERR)) return FALSE;

    char buf[1024];
    ssize_t n = read(t->fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        char *line = strtok(buf, "\n");
        while (line) {
            struct json_object *obj = json_tokener_parse(line);
            if (obj) {
                struct json_object *fingers_obj, *gesture_obj, *prog_obj;
                if (json_object_object_get_ex(obj, "fingers", &fingers_obj) &&
                    json_object_object_get_ex(obj, "gesture", &gesture_obj) &&
                    json_object_object_get_ex(obj, "progress", &prog_obj)) {
                    int fingers = json_object_get_int(fingers_obj);
                    const char *gest = json_object_get_string(gesture_obj);
                    double prog = json_object_get_double(prog_obj);
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(t->bar), prog > 1.0 ? 1.0 : (prog < 0.0 ? 0.0 : prog));
                    char txt[128];
                    snprintf(txt, sizeof(txt), "%d fingers  •  %s  (%.0f%%)", fingers, gest, prog * 100.0);
                    gtk_label_set_text(GTK_LABEL(t->lbl), txt);
                }
                json_object_put(obj);
            }
            line = strtok(NULL, "\n");
        }
    }
    return TRUE;
}

GtkWidget *build_page_gestures(void) {
    GtkWidget *page = ui_page_container("Gestures", "High-frequency touchpad gestures and fluid spring animation physics.");

    // Three-finger gestures
    GtkWidget *c_3f = ui_card(page, "THREE-FINGER GESTURES");
    GtkWidget *d_up = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_up_3.type), on_gesture_3_up, NULL);
    ui_add_row(c_3f, "Swipe Up", "Three fingers sliding upwards", d_up, "go-up-symbolic", "badge-gestures");

    GtkWidget *d_dn = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_down_3.type), on_gesture_3_down, NULL);
    ui_add_row(c_3f, "Swipe Down", "Three fingers sliding downwards", d_dn, "go-down-symbolic", "badge-gestures");

    GtkWidget *d_lf = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_left_3.type), on_gesture_3_left, NULL);
    ui_add_row(c_3f, "Swipe Left", "Three fingers sliding to the left", d_lf, "go-previous-symbolic", "badge-gestures");

    GtkWidget *d_rt = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_right_3.type), on_gesture_3_right, NULL);
    ui_add_row(c_3f, "Swipe Right", "Three fingers sliding to the right", d_rt, "go-next-symbolic", "badge-gestures");

    GtkWidget *d_pi = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.pinch_in_3.type), on_gesture_3_pin, NULL);
    ui_add_row(c_3f, "Pinch In", "Thumb and fingers closing inward", d_pi, "zoom-in-symbolic", "badge-gestures");

    GtkWidget *d_po = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.pinch_out_3.type), on_gesture_3_pout, NULL);
    ui_add_row(c_3f, "Pinch Out / Spread", "Thumb and fingers spreading outward", d_po, "zoom-out-symbolic", "badge-gestures");

    // Physics Tuning
    GtkWidget *c_phys = ui_card(page, "SPRING ANIMATION PHYSICS");
    GtkWidget *sc_st = ui_scale(g_config.spring.stiffness > 10.0 ? g_config.spring.stiffness : 280.0, 80.0, 600.0, 10.0, 0, on_spring_stiff_changed, NULL);
    ui_add_row(c_phys, "Spring Stiffness", "Higher values snap back faster with snappier response", sc_st, "preferences-system-symbolic", "badge-gestures");

    GtkWidget *sc_da = ui_scale(g_config.spring.damping > 0.1 ? g_config.spring.damping : 0.92, 0.40, 1.30, 0.02, 2, on_spring_damp_changed, NULL);
    ui_add_row(c_phys, "Spring Damping", "Ratio of friction. Under 1.0 produces subtle natural overshoot bounce", sc_da, "preferences-system-symbolic", "badge-gestures");

    GtkWidget *sc_ma = ui_scale(g_config.spring.mass > 0.1 ? g_config.spring.mass : 1.0, 0.5, 3.0, 0.1, 1, on_spring_mass_changed, NULL);
    ui_add_row(c_phys, "Virtual Mass", "Apparent weight of desktop windows during spring simulation", sc_ma, "preferences-system-symbolic", "badge-gestures");

    // Live Telemetry Socket Card
    GtkWidget *c_tel = ui_card(page, "LIVE GESTURE TELEMETRY");
    GtkWidget *tel_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *pbar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(pbar), FALSE);
    gtk_box_append(GTK_BOX(tel_box), pbar);

    GtkWidget *lbl_stat = gtk_label_new("IDLE (touch touchpad with 3 fingers)");
    gtk_widget_add_css_class(lbl_stat, "subtitle");
    gtk_box_append(GTK_BOX(tel_box), lbl_stat);

    ui_add_row(c_tel, "Hardware Telemetry Stream", "Real-time recognition feedback from /tmp/aurade-gesture.sock", tel_box, "input-touchpad-symbolic", "badge-gestures");

    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, "/tmp/aurade-gesture.sock", sizeof(addr.sun_path) - 1);
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            telemetry_ui_t *t = g_new0(telemetry_ui_t, 1);
            t->bar = pbar;
            t->lbl = lbl_stat;
            t->fd = sock;
            GIOChannel *chan = g_io_channel_unix_new(sock);
            g_io_add_watch(chan, G_IO_IN | G_IO_HUP, on_telemetry_socket_data, t);
            g_io_channel_unref(chan);
        } else {
            close(sock);
        }
    }

    return page;
}

// -------------------------------------------------------------
// PAGE: ABOUT
// -------------------------------------------------------------

static void on_reset_defaults_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    settings_reset_defaults();
}

GtkWidget *build_page_about(void) {
    GtkWidget *page = ui_page_container("About AuraDE", "Modern macOS-inspired Wayland desktop environment.");

    // Hero Banner
    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(hero, "hero-banner");

    GtkWidget *logo = ui_make_badge("preferences-desktop-display-symbolic", "badge-about", "row-badge");
    gtk_widget_set_size_request(logo, 56, 56);
    gtk_box_append(GTK_BOX(hero), logo);

    GtkWidget *hero_txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *htitle = gtk_label_new("AuraDE Desktop");
    gtk_widget_add_css_class(htitle, "headline");
    gtk_label_set_xalign(GTK_LABEL(htitle), 0.0f);
    gtk_box_append(GTK_BOX(hero_txt), htitle);

    GtkWidget *hsub = gtk_label_new("Fluid Wayland compositor built with SceneFX & wlroots");
    gtk_widget_add_css_class(hsub, "subtitle");
    gtk_label_set_xalign(GTK_LABEL(hsub), 0.0f);
    gtk_box_append(GTK_BOX(hero_txt), hsub);
    gtk_box_append(GTK_BOX(hero), hero_txt);
    gtk_box_append(GTK_BOX(page), hero);

    // System Information
    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    GtkWidget *c_info = ui_card(page, "SYSTEM SPECIFICATIONS");
    ui_add_row(c_info, "Operating System", "Linux", gtk_label_new("AuraOS / Arch Linux"), "computer-symbolic", "badge-about");
    ui_add_row(c_info, "Kernel Version", uts.sysname, gtk_label_new(uts.release), "system-run-symbolic", "badge-about");
    ui_add_row(c_info, "Hardware Architecture", "Host CPU Architecture", gtk_label_new(uts.machine), "applications-system-symbolic", "badge-about");
    ui_add_row(c_info, "Compositor Engine", "Hardware-accelerated rendering pipeline", gtk_label_new("wlroots 0.20 + SceneFX 0.5"), "video-display-symbolic", "badge-about");

    // Config Actions
    GtkWidget *c_cfg = ui_card(page, "CONFIGURATION MANAGEMENT");
    const char *cfg_path = config_get_default_path();
    GtkWidget *lbl_path = gtk_label_new(cfg_path);
    gtk_widget_add_css_class(lbl_path, "mono");
    ui_add_row(c_cfg, "Configuration File", "Active JSON settings file watched via inotify", lbl_path, "document-properties-symbolic", "badge-about");

    GtkWidget *btn_rst = gtk_button_new_with_label("Restore Default Settings");
    gtk_widget_add_css_class(btn_rst, "btn-destructive");
    g_signal_connect(btn_rst, "clicked", G_CALLBACK(on_reset_defaults_clicked), NULL);
    ui_add_row(c_cfg, "Reset to Factory Defaults", "Discard all customizations and reload standard macOS settings", btn_rst, "edit-undo-symbolic", "badge-about");

    return page;
}
