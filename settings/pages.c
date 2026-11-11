#include "settings.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <glob.h>

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
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
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

static void on_sound_in_vol_changed(double val, gpointer user_data) {
    (void)user_data;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SOURCE@ %.2f 2>/dev/null || pactl set-source-volume @DEFAULT_SOURCE@ %d%% 2>/dev/null",
             val / 100.0, (int)val);
    run_cmd_async(cmd);
}

static void on_sound_in_mute_changed(bool active, gpointer user_data) {
    (void)user_data;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wpctl set-mute @DEFAULT_AUDIO_SOURCE@ %d 2>/dev/null || pactl set-source-mute @DEFAULT_SOURCE@ %d 2>/dev/null",
             active ? 1 : 0, active ? 1 : 0);
    run_cmd_async(cmd);
}

static void on_sound_test_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    run_cmd_async("canberra-gtk-play -i bell 2>/dev/null || paplay /usr/share/sounds/freedesktop/stereo/bell.oga 2>/dev/null || speaker-test -t sine -f 440 -l 1 >/dev/null 2>&1 &");
}

static void on_set_default_sound_device(GtkButton *btn, gpointer user_data) {
    (void)btn;
    int dev_id = GPOINTER_TO_INT(user_data);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wpctl set-default %d 2>/dev/null", dev_id);
    run_cmd_async(cmd);
    if (g_status_label) {
        gtk_label_set_text(GTK_LABEL(g_status_label), "Audio device set");
    }
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

    double cur_in_vol = 65.0;
    bool is_in_muted = false;
    char in_vol_buf[128] = "";
    if (run_cmd_capture("wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null", in_vol_buf, sizeof(in_vol_buf))) {
        double v = 0.0;
        if (sscanf(in_vol_buf, "Volume: %lf", &v) == 1) {
            cur_in_vol = v * 100.0;
            is_in_muted = (strstr(in_vol_buf, "[MUTED]") != NULL);
        }
    }

    // Output Card
    GtkWidget *c_out = ui_card(page, "OUTPUT & SPEAKERS");
    GtkWidget *sc_vol = ui_scale(cur_vol, 0.0, 100.0, 1.0, 0, on_sound_vol_changed, NULL);
    ui_add_row(c_out, "Output Volume", "Master system playback volume", sc_vol, "audio-volume-high-symbolic", "badge-sound");

    GtkWidget *sw_mute = ui_switch(is_muted, on_sound_mute_changed, NULL);
    ui_add_row(c_out, "Mute Audio", "Mute all sound output immediately", sw_mute, "audio-volume-muted-symbolic", "badge-sound");

    GtkWidget *btn_test = gtk_button_new_with_label("Play Alert Sound");
    gtk_widget_add_css_class(btn_test, "btn-primary");
    g_signal_connect(btn_test, "clicked", G_CALLBACK(on_sound_test_clicked), NULL);
    ui_add_row(c_out, "Sound Test", "Verify audio playback and speaker balance", btn_test, "audio-speakers-symbolic", "badge-sound");

    // Output Devices Card
    GtkWidget *c_sinks = ui_card(page, "OUTPUT DEVICES");

    // Input Card
    GtkWidget *c_in = ui_card(page, "INPUT & MICROPHONE");
    GtkWidget *sc_in = ui_scale(cur_in_vol, 0.0, 100.0, 1.0, 0, on_sound_in_vol_changed, NULL);
    ui_add_row(c_in, "Microphone Volume", "Default recording and microphone input level", sc_in, "audio-input-microphone-symbolic", "badge-sound");

    GtkWidget *sw_in_mute = ui_switch(is_in_muted, on_sound_in_mute_changed, NULL);
    ui_add_row(c_in, "Mute Microphone", "Silence input microphone stream", sw_in_mute, "audio-input-microphone-symbolic", "badge-sound");

    // Input Devices Card
    GtkWidget *c_sources = ui_card(page, "INPUT DEVICES");

    // Parse wpctl status for sinks and sources
    char st_buf[8192] = "";
    if (run_cmd_capture("wpctl status 2>/dev/null", st_buf, sizeof(st_buf))) {
        char *line = strtok(st_buf, "\n");
        int section = 0; // 1 = Sinks, 2 = Sources
        int sink_count = 0, source_count = 0;

        while (line) {
            char clean[256];
            strncpy(clean, line, sizeof(clean) - 1);
            clean[sizeof(clean) - 1] = '\0';

            if (strstr(clean, "Sinks:")) {
                section = 1;
            } else if (strstr(clean, "Sources:")) {
                section = 2;
            } else if (strstr(clean, "Filters:") || strstr(clean, "Streams:") || strstr(clean, "Video")) {
                section = 0;
            } else if (section != 0) {
                // Check if line contains a device: e.g. " │  *   60. Built-in Audio Analog Stereo        [vol: 0.06]"
                char *dot = strchr(clean, '.');
                if (dot && dot > clean) {
                    bool is_def = (strchr(clean, '*') != NULL);
                    // Find digit before dot
                    char *p = dot - 1;
                    while (p >= clean && (*p == ' ' || *p == '*' || *p == '|')) p--;
                    char *num_end = p + 1;
                    while (p >= clean && *p >= '0' && *p <= '9') p--;
                    char *num_start = p + 1;

                    if (num_start < num_end) {
                        int dev_id = atoi(num_start);
                        char *name_start = dot + 1;
                        while (*name_start == ' ') name_start++;
                        char dev_name[128] = "";
                        strncpy(dev_name, name_start, sizeof(dev_name) - 1);
                        char *bracket = strchr(dev_name, '[');
                        if (bracket) *bracket = '\0';
                        // Trim trailing spaces
                        size_t nlen = strlen(dev_name);
                        while (nlen > 0 && dev_name[nlen - 1] == ' ') dev_name[--nlen] = '\0';

                        if (dev_name[0]) {
                            GtkWidget *ctrl;
                            if (is_def) {
                                ctrl = gtk_label_new("Default");
                                gtk_widget_add_css_class(ctrl, "connected-pill");
                            } else {
                                ctrl = gtk_button_new_with_label("Use Device");
                                g_signal_connect(ctrl, "clicked", G_CALLBACK(on_set_default_sound_device), GINT_TO_POINTER(dev_id));
                            }

                            if (section == 1) {
                                ui_add_row(c_sinks, dev_name, "PipeWire Audio Output", ctrl, "audio-speakers-symbolic",
                                           is_def ? "badge-sound" : "badge-about");
                                sink_count++;
                            } else if (section == 2) {
                                ui_add_row(c_sources, dev_name, "PipeWire Audio Input", ctrl, "audio-input-microphone-symbolic",
                                           is_def ? "badge-wifi" : "badge-about");
                                source_count++;
                            }
                        }
                    }
                }
            }
            line = strtok(NULL, "\n");
        }

        if (sink_count == 0) {
            ui_add_row(c_sinks, "Default Output Device", "Active system sink", NULL, "audio-speakers-symbolic", "badge-sound");
        }
        if (source_count == 0) {
            ui_add_row(c_sources, "Default Input Device", "Active system source", NULL, "audio-input-microphone-symbolic", "badge-wifi");
        }
    }

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
    gsettings_set_bool("org.gnome.settings-daemon.plugins.color", "night-light-enabled", active);
}

static void on_night_light_temp_changed(double val, gpointer user_data) {
    (void)user_data;
    gsettings_set_int("org.gnome.settings-daemon.plugins.color", "night-light-temperature", (int)val);
}

static void on_ui_scale_changed(int idx, gpointer user_data) {
    (void)idx; (void)user_data;
    if (g_status_label) {
        static const char *const scales[] = {"100%", "125%", "150%", "200%"};
        if (idx >= 0 && idx < 4) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Display scale set to %s", scales[idx]);
            gtk_label_set_text(GTK_LABEL(g_status_label), msg);
        }
    }
}

GtkWidget *build_page_display(void) {
    GtkWidget *page = ui_page_container("Displays", "Screen resolution, brightness, Night Light and display scaling.");

    double cur_br = 80.0;
    char br_buf[128] = "";
    if (run_cmd_capture("brightnessctl -m 2>/dev/null", br_buf, sizeof(br_buf))) {
        char *pct_str = strrchr(br_buf, ',');
        if (pct_str) {
            int p = atoi(pct_str + 1);
            if (p > 0 && p <= 100) cur_br = p;
        }
    }

    GtkWidget *c_disp = ui_card(page, "SCREEN BRIGHTNESS & RESOLUTION");
    GtkWidget *sc_br = ui_scale(cur_br, 5.0, 100.0, 1.0, 0, on_brightness_changed, NULL);
    ui_add_row(c_disp, "Screen Brightness", "Adjust built-in panel backlighting", sc_br, "display-brightness-symbolic", "badge-display");

    // Dynamic Display Resolution modes
    glob_t globbuf;
    char *modes[32] = {NULL};
    int mode_count = 0;
    if (glob("/sys/class/drm/*/modes", 0, NULL, &globbuf) == 0) {
        for (size_t i = 0; i < globbuf.gl_pathc && mode_count < 30; i++) {
            FILE *f = fopen(globbuf.gl_pathv[i], "r");
            if (f) {
                char mline[64];
                while (fgets(mline, sizeof(mline), f) && mode_count < 30) {
                    char *nl = strchr(mline, '\n');
                    if (nl) *nl = '\0';
                    if (mline[0]) {
                        bool exists = false;
                        for (int k = 0; k < mode_count; k++) {
                            if (strcmp(modes[k], mline) == 0) { exists = true; break; }
                        }
                        if (!exists) {
                            modes[mode_count++] = g_strdup(mline);
                        }
                    }
                }
                fclose(f);
            }
        }
        globfree(&globbuf);
    }
    if (mode_count == 0) {
        modes[0] = g_strdup("1920x1080");
        modes[1] = g_strdup("1366x768");
        modes[2] = g_strdup("1280x720");
        mode_count = 3;
    }
    modes[mode_count] = NULL;

    GtkWidget *mode_dd = ui_dropdown((const char *const *)modes, mode_count, 0, NULL, NULL);
    ui_add_row(c_disp, "Display Resolution", "Active monitor video resolution mode", mode_dd, "video-display-symbolic", "badge-display");

    // Night Light Card
    GtkWidget *c_nl = ui_card(page, "NIGHT LIGHT");
    bool nl_active = gsettings_get_bool("org.gnome.settings-daemon.plugins.color", "night-light-enabled", false);
    GtkWidget *sw_nl = ui_switch(nl_active, on_night_light_toggled, NULL);
    ui_add_row(c_nl, "Night Light", "Makes screen colours warmer to help prevent eye strain", sw_nl, "weather-clear-night-symbolic", "badge-display");

    int cur_temp = gsettings_get_int("org.gnome.settings-daemon.plugins.color", "night-light-temperature", 3500);
    GtkWidget *sc_temp = ui_scale(cur_temp >= 1000 && cur_temp <= 6500 ? cur_temp : 3500, 1700.0, 5000.0, 100.0, 0, on_night_light_temp_changed, NULL);
    ui_add_row(c_nl, "Color Temperature", "Warmer colours reduce blue light emission", sc_temp, "preferences-system-symbolic", "badge-display");

    // Display Scaling Card
    GtkWidget *c_scale = ui_card(page, "DISPLAY SCALING");
    static const char *const SCALING_OPTIONS[] = { "100% (Standard)", "125%", "150%", "200% (HiDPI / Retina)", NULL };
    GtkWidget *dd_scale = ui_dropdown(SCALING_OPTIONS, 4, 0, on_ui_scale_changed, NULL);
    ui_add_row(c_scale, "UI Scale Factor", "Enlarge interface items for higher DPI screens", dd_scale, "zoom-fit-best-symbolic", "badge-display");

    return page;
}

// -------------------------------------------------------------
// PAGE: WORKSPACES
// -------------------------------------------------------------

static GtkWidget *g_ws_cards_box = NULL;
static GtkWidget *g_ws_spin = NULL;

static void refresh_workspace_cards(void);

static void on_ws_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)user_data;
    int count = gtk_spin_button_get_value_as_int(spin);
    if (count < 1) count = 1;
    if (count > 16) count = 16;
    if (count != g_config.num_workspaces) {
        g_config.num_workspaces = count;
        refresh_workspace_cards();
        settings_schedule_save();
    }
}

static void on_delete_space_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (g_config.num_workspaces > 1) {
        g_config.num_workspaces--;
        if (g_ws_spin) gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_ws_spin), g_config.num_workspaces);
        refresh_workspace_cards();
        settings_schedule_save();
    }
}

static void on_add_space_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (g_config.num_workspaces < 16) {
        g_config.num_workspaces++;
        if (g_ws_spin) gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_ws_spin), g_config.num_workspaces);
        refresh_workspace_cards();
        settings_schedule_save();
    }
}

static void refresh_workspace_cards(void) {
    if (!g_ws_cards_box) return;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(g_ws_cards_box)) != NULL) {
        gtk_box_remove(GTK_BOX(g_ws_cards_box), child);
    }

    int count = g_config.num_workspaces;
    if (count < 1) count = 1;
    if (count > 16) count = 16;

    for (int i = 1; i <= count; i++) {
        GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(c, "workspace-card");
        gtk_widget_set_size_request(c, 130, 110);

        GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *icon = gtk_image_new_from_icon_name("video-display-symbolic");
        gtk_box_append(GTK_BOX(top_row), icon);

        char title_buf[32];
        snprintf(title_buf, sizeof(title_buf), "Space %d", i);
        GtkWidget *lbl_title = gtk_label_new(title_buf);
        gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0f);
        gtk_widget_add_css_class(lbl_title, "title");
        gtk_widget_set_hexpand(lbl_title, TRUE);
        gtk_box_append(GTK_BOX(top_row), lbl_title);
        gtk_box_append(GTK_BOX(c), top_row);

        GtkWidget *sub = gtk_label_new(i == 1 ? "Primary Desktop" : "Virtual Desktop");
        gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
        gtk_widget_add_css_class(sub, "subtitle");
        gtk_box_append(GTK_BOX(c), sub);

        GtkWidget *bottom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_vexpand(bottom_box, TRUE);
        gtk_widget_set_valign(bottom_box, GTK_ALIGN_END);

        if (count > 1) {
            GtkWidget *btn_del = gtk_button_new_with_label("Delete");
            gtk_widget_add_css_class(btn_del, "btn-destructive");
            g_signal_connect(btn_del, "clicked", G_CALLBACK(on_delete_space_clicked), NULL);
            gtk_box_append(GTK_BOX(bottom_box), btn_del);
        } else {
            GtkWidget *lbl_min = gtk_label_new("Primary");
            gtk_widget_add_css_class(lbl_min, "subtitle");
            gtk_box_append(GTK_BOX(bottom_box), lbl_min);
        }
        gtk_box_append(GTK_BOX(c), bottom_box);

        gtk_box_append(GTK_BOX(g_ws_cards_box), c);
    }

    if (count < 16) {
        GtkWidget *c_add = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(c_add, "workspace-card-add");
        gtk_widget_set_size_request(c_add, 120, 110);
        gtk_widget_set_valign(c_add, GTK_ALIGN_CENTER);

        GtkWidget *btn_plus = gtk_button_new_with_label("+ Add Space");
        gtk_widget_add_css_class(btn_plus, "btn-primary");
        gtk_widget_set_valign(btn_plus, GTK_ALIGN_CENTER);
        gtk_widget_set_vexpand(btn_plus, TRUE);
        g_signal_connect(btn_plus, "clicked", G_CALLBACK(on_add_space_clicked), NULL);
        gtk_box_append(GTK_BOX(c_add), btn_plus);

        gtk_box_append(GTK_BOX(g_ws_cards_box), c_add);
    }
}

static void on_autofocus_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.workspace_auto_focus = active;
}

static void on_ws_overview_blur_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.blur_workspace_overview = active;
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
    GtkWidget *page = ui_page_container("Workspaces", "Configure virtual workspaces, add or delete spaces, and customize desktop switching.");

    // Workspace Count Card
    GtkWidget *c_cnt = ui_card(page, "WORKSPACE COUNT");
    int cur_ws = g_config.num_workspaces > 0 ? g_config.num_workspaces : 4;
    g_ws_spin = gtk_spin_button_new_with_range(1, 16, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_ws_spin), cur_ws);
    g_signal_connect(g_ws_spin, "value-changed", G_CALLBACK(on_ws_spin_changed), NULL);
    ui_add_row(c_cnt, "Number of Spaces", "Choose between 1 and 16 virtual desktops", g_ws_spin, "view-grid-symbolic", "badge-workspaces");

    // Spaces Overview Card
    GtkWidget *c_visual = ui_card(page, "SPACES OVERVIEW");
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), 100);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 140);

    g_ws_cards_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(g_ws_cards_box, 8);
    gtk_widget_set_margin_bottom(g_ws_cards_box, 12);
    gtk_widget_set_margin_start(g_ws_cards_box, 4);
    gtk_widget_set_margin_end(g_ws_cards_box, 4);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), g_ws_cards_box);
    gtk_box_append(GTK_BOX(c_visual), scroll);

    refresh_workspace_cards();

    // Window Tiling Card
    GtkWidget *c_tile = ui_card(page, "WINDOW TILING (HYPRLAND STYLE)");
    GtkWidget *sw_tile = ui_switch(g_config.tiling_enabled, on_tiling_toggled, NULL);
    ui_add_row(c_tile, "Dynamic Window Tiling",
               "Automatically arrange windows in a Hyprland-style Dwindle spiral layout (off by default). Press Super + V to float/tile, or Super + Drag to swap tiles.",
               sw_tile, "view-grid-symbolic", "badge-workspaces");

    GtkWidget *sc_gap = ui_scale(g_config.tiling_gap >= 0 ? g_config.tiling_gap : 10, 0, 32, 1, 0, on_tiling_gap_changed, NULL);
    ui_add_row(c_tile, "Tiling Window Gaps", "Spacing in pixels between tiled windows and screen edges (0–32 px)",
               sc_gap, "preferences-system-windows-symbolic", "badge-appearance");

    // Focus & Blur Card
    GtkWidget *c_focus = ui_card(page, "WINDOW FOCUS & BLUR");
    GtkWidget *sw_af = ui_switch(g_config.workspace_auto_focus, on_autofocus_toggled, NULL);
    ui_add_row(c_focus, "Auto-Focus on Space Switch", "Automatically focus the frontmost window when switching between workspaces",
               sw_af, "preferences-system-windows-symbolic", "badge-workspaces");

    GtkWidget *sw_blur = ui_switch(g_config.blur_workspace_overview, on_ws_overview_blur_toggled, NULL);
    ui_add_row(c_focus, "Overview Background Blur", "Enable frosted wallpaper blur in Mission Control",
               sw_blur, "preferences-desktop-wallpaper-symbolic", "badge-appearance");

    // Navigation Guide Card
    GtkWidget *c_nav = ui_card(page, "GESTURES & NAVIGATION");
    ui_add_row(c_nav, "Trackpad Switching", "Swipe left or right with 3 fingers to move smoothly between adjacent workspaces", NULL, "input-touchpad-symbolic", "badge-gestures");
    ui_add_row(c_nav, "Mission Control", "Swipe up with 3 fingers to view all active workspaces in the top Spaces bar", NULL, "view-grid-symbolic", "badge-workspaces");
    ui_add_row(c_nav, "Keyboard Navigation", "Use Ctrl + Alt + Left/Right arrows to instantly switch workspaces", NULL, "input-keyboard-symbolic", "badge-keyboard");

    return page;
}

// -------------------------------------------------------------
// PAGE: MOUSE & TOUCHPAD
// -------------------------------------------------------------

static void on_tp_tap_to_click(bool active, gpointer user_data) {
    (void)user_data;
    gsettings_set_bool("org.gnome.desktop.peripherals.touchpad", "tap-to-click", active);
}

static void on_natural_scroll_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.natural_scroll = active;
    gsettings_set_bool("org.gnome.desktop.peripherals.touchpad", "natural-scroll", active);
}

static void on_dwt_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.disable_while_typing = active;
    gsettings_set_bool("org.gnome.desktop.peripherals.touchpad", "disable-while-typing", active);
}

static void on_touchpad_speed_changed(double val, gpointer user_data) {
    (void)user_data;
    gsettings_set_double("org.gnome.desktop.peripherals.touchpad", "speed", val);
    g_config.sensitivity = 1.0 + val;
}

static void on_mouse_speed_changed(double val, gpointer user_data) {
    (void)user_data;
    gsettings_set_double("org.gnome.desktop.peripherals.mouse", "speed", val);
}

static void on_mouse_left_handed_toggled(bool active, gpointer user_data) {
    (void)user_data;
    gsettings_set_bool("org.gnome.desktop.peripherals.mouse", "left-handed", active);
}

static void on_mouse_accel_changed(int idx, gpointer user_data) {
    (void)user_data;
    const char *mode = (idx == 1) ? "flat" : (idx == 2) ? "adaptive" : "default";
    gsettings_set_string("org.gnome.desktop.peripherals.mouse", "accel-profile", mode);
}

GtkWidget *build_page_mouse(void) {
    GtkWidget *page = ui_page_container("Mouse & Touchpad", "Pointer speed, natural scrolling, tap-to-click and gesture acceleration.");

    // Touchpad Card
    GtkWidget *c_tp = ui_card(page, "TOUCHPAD");

    bool tap = gsettings_get_bool("org.gnome.desktop.peripherals.touchpad", "tap-to-click", true);
    GtkWidget *sw_tap = ui_switch(tap, on_tp_tap_to_click, NULL);
    ui_add_row(c_tp, "Tap to Click", "Tap with one finger to click, two fingers to right-click", sw_tap, "input-mouse-symbolic", "badge-mouse");

    GtkWidget *sw_nat = ui_switch(g_config.natural_scroll, on_natural_scroll_toggled, NULL);
    ui_add_row(c_tp, "Natural Scrolling", "Content tracks finger movement directly (macOS direction)", sw_nat, "input-mouse-symbolic", "badge-mouse");

    double tp_speed = gsettings_get_double("org.gnome.desktop.peripherals.touchpad", "speed", 0.0);
    GtkWidget *sc_tp_speed = ui_scale(tp_speed, -1.0, 1.0, 0.05, 2, on_touchpad_speed_changed, NULL);
    ui_add_row(c_tp, "Tracking Speed", "Adjust cursor motion sensitivity and precision", sc_tp_speed, "preferences-system-symbolic", "badge-mouse");

    GtkWidget *sw_dwt = ui_switch(g_config.disable_while_typing, on_dwt_toggled, NULL);
    ui_add_row(c_tp, "Lock While Typing", "Prevent accidental palm taps while working on the keyboard", sw_dwt, "input-keyboard-symbolic", "badge-mouse");

    // Physical Mouse Card
    GtkWidget *c_mouse = ui_card(page, "PHYSICAL MOUSE");

    double m_speed = gsettings_get_double("org.gnome.desktop.peripherals.mouse", "speed", 0.0);
    GtkWidget *sc_m_speed = ui_scale(m_speed, -1.0, 1.0, 0.05, 2, on_mouse_speed_changed, NULL);
    ui_add_row(c_mouse, "Mouse Speed", "Primary pointer tracking velocity", sc_m_speed, "input-mouse-symbolic", "badge-mouse");

    bool left_handed = gsettings_get_bool("org.gnome.desktop.peripherals.mouse", "left-handed", false);
    GtkWidget *sw_left = ui_switch(left_handed, on_mouse_left_handed_toggled, NULL);
    ui_add_row(c_mouse, "Left-Handed Mouse", "Swap primary left and right click buttons", sw_left, "input-mouse-symbolic", "badge-mouse");

    static const char *const ACCEL_MODES[] = { "Default", "Flat (No Acceleration)", "Adaptive", NULL };
    char *cur_accel = gsettings_get_string("org.gnome.desktop.peripherals.mouse", "accel-profile", "default");
    int accel_idx = (strcmp(cur_accel, "flat") == 0) ? 1 : (strcmp(cur_accel, "adaptive") == 0) ? 2 : 0;
    g_free(cur_accel);

    GtkWidget *dd_accel = ui_dropdown(ACCEL_MODES, 3, accel_idx, on_mouse_accel_changed, NULL);
    ui_add_row(c_mouse, "Acceleration Profile", "Control pointer acceleration curves", dd_accel, "preferences-system-symbolic", "badge-mouse");

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

static void on_model_changed(const char *text, gpointer user_data) {
    (void)user_data;
    strncpy(g_config.kb_model, text, sizeof(g_config.kb_model) - 1);
}

static void on_options_changed(const char *text, gpointer user_data) {
    (void)user_data;
    strncpy(g_config.kb_options, text, sizeof(g_config.kb_options) - 1);
}

static void on_key_repeat_delay_changed(double val, gpointer user_data) {
    (void)user_data;
    gsettings_set_int("org.gnome.desktop.peripherals.keyboard", "delay", (int)val);
}

static void on_key_repeat_interval_changed(double val, gpointer user_data) {
    (void)user_data;
    gsettings_set_int("org.gnome.desktop.peripherals.keyboard", "repeat-interval", (int)val);
}

GtkWidget *build_page_keyboard(void) {
    GtkWidget *page = ui_page_container("Keyboard", "Configure layout, input sources, key repeat delays and system shortcuts.");

    // Input Sources & Layout Card
    GtkWidget *c_layout = ui_card(page, "INPUT SOURCES & LAYOUT");
    GtkWidget *en_lay = ui_entry(g_config.kb_layout[0] ? g_config.kb_layout : "us,ua", "e.g. us,ua", on_layout_changed, NULL);
    ui_add_row(c_layout, "Layouts", "Comma separated XKB layouts, e.g. us,ua", en_lay, "input-keyboard-symbolic", "badge-keyboard");

    GtkWidget *en_var = ui_entry(g_config.kb_variant, "e.g. ,winkeysenhanced", on_variant_changed, NULL);
    ui_add_row(c_layout, "Variants", "One variant per layout, e.g. ,winkeysenhanced", en_var, "preferences-system-symbolic", "badge-keyboard");

    GtkWidget *en_mod = ui_entry(g_config.kb_model[0] ? g_config.kb_model : "pc105", "Usually pc105", on_model_changed, NULL);
    ui_add_row(c_layout, "Model", "Hardware keyboard model definition", en_mod, "input-keyboard-symbolic", "badge-keyboard");

    GtkWidget *en_opt = ui_entry(g_config.kb_options, "e.g. grp:alt_shift_toggle", on_options_changed, NULL);
    ui_add_row(c_layout, "Options", "XKB options for layout switching", en_opt, "preferences-system-symbolic", "badge-keyboard");

    // Key Repeat Card
    GtkWidget *c_repeat = ui_card(page, "KEY REPEAT");
    int cur_delay = gsettings_get_int("org.gnome.desktop.peripherals.keyboard", "delay", 500);
    GtkWidget *sc_delay = ui_scale(cur_delay >= 150 && cur_delay <= 1000 ? cur_delay : 500, 150.0, 1000.0, 25.0, 0, on_key_repeat_delay_changed, NULL);
    ui_add_row(c_repeat, "Repeat Delay", "Delay until repeated key strokes begin (ms)", sc_delay, "preferences-system-symbolic", "badge-keyboard");

    int cur_rep = gsettings_get_int("org.gnome.desktop.peripherals.keyboard", "repeat-interval", 30);
    GtkWidget *sc_rep = ui_scale(cur_rep >= 10 && cur_rep <= 100 ? cur_rep : 30, 10.0, 100.0, 5.0, 0, on_key_repeat_interval_changed, NULL);
    ui_add_row(c_repeat, "Repeat Interval", "Speed at which keys repeat while held down (ms)", sc_rep, "preferences-system-symbolic", "badge-keyboard");

    // Shortcuts Reference Card
    GtkWidget *c_shortcuts = ui_card(page, "KEYBOARD & GESTURE SHORTCUTS");
    static const struct { const char *name; const char *shortcut; } SHORTCUTS[] = {
        { "Switch Layout", "Super + Space" },
        { "Toggle Floating (Tiling)", "Super + V" },
        { "Mission Control", "Swipe 3 fingers Up · F3" },
        { "App Exposé", "Swipe 3 fingers Down" },
        { "Switch Workspaces", "Swipe 3 fingers Left / Right" },
        { "Launchpad", "Pinch 4 fingers In · F4" },
        { "Show Desktop", "Spread 4 fingers Out" },
        { "Control Center", "Super + C · Click Topbar Pill" },
        { "Full Screenshot", "Print" },
        { "Area Screenshot", "Shift + Print" },
        { "Lock Screen", "Super + L" },
    };
    for (size_t i = 0; i < sizeof(SHORTCUTS) / sizeof(SHORTCUTS[0]); i++) {
        GtkWidget *slbl = gtk_label_new(SHORTCUTS[i].shortcut);
        gtk_widget_add_css_class(slbl, "mono");
        gtk_label_set_wrap(GTK_LABEL(slbl), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(slbl), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_ellipsize(GTK_LABEL(slbl), PANGO_ELLIPSIZE_END);
        ui_add_row(c_shortcuts, SHORTCUTS[i].name, NULL, slbl, "input-keyboard-symbolic", "badge-keyboard");
    }

    return page;
}

// -------------------------------------------------------------
// PAGE: APPEARANCE
// -------------------------------------------------------------

static GtkWidget *g_wallpaper_label = NULL;
static GtkWidget *g_win_opacity_box = NULL;

static void on_dark_mode_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.dark_mode = active;
    apply_theme(active);
    gsettings_set_string("org.gnome.desktop.interface", "color-scheme", active ? "prefer-dark" : "prefer-light");
}

static void on_accent_color_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *hex = (const char *)user_data;
    strncpy(g_config.accent_color, hex, sizeof(g_config.accent_color) - 1);
    settings_schedule_save();
}

static void on_cursor_theme_changed(int idx, gpointer user_data) {
    (void)user_data;
    static const char *const themes[] = {"Aura-Circle", "WhiteSur-cursors", "Adwaita", "Papirus", NULL};
    if (idx >= 0 && idx < 4) {
        gsettings_set_string("org.gnome.desktop.interface", "cursor-theme", themes[idx]);
        if (g_status_label) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Cursor theme set to %s", themes[idx]);
            gtk_label_set_text(GTK_LABEL(g_status_label), msg);
        }
    }
}

static void on_cursor_size_changed(int idx, gpointer user_data) {
    (void)user_data;
    static const int sizes[] = {24, 32, 48, 64};
    if (idx >= 0 && idx < 4) {
        gsettings_set_int("org.gnome.desktop.interface", "cursor-size", sizes[idx]);
        if (g_status_label) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Cursor size set to %dpx", sizes[idx]);
            gtk_label_set_text(GTK_LABEL(g_status_label), msg);
        }
    }
}

static void on_wallpaper_file_chosen(GtkFileDialog *dialog, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, NULL);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            strncpy(g_config.wallpaper_path, path, sizeof(g_config.wallpaper_path) - 1);
            if (g_wallpaper_label) {
                gtk_label_set_text(GTK_LABEL(g_wallpaper_label), path);
            }
            settings_schedule_save();
            g_free(path);
        }
        g_object_unref(file);
    }
}

static void on_choose_wallpaper_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose a desktop wallpaper");
    gtk_file_dialog_open(dialog, GTK_WINDOW(g_main_window), NULL, (GAsyncReadyCallback)on_wallpaper_file_chosen, NULL);
}

static void on_clear_wallpaper_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    g_config.wallpaper_path[0] = '\0';
    if (g_wallpaper_label) {
        gtk_label_set_text(GTK_LABEL(g_wallpaper_label), "Built-in dynamic gradient");
    }
    settings_schedule_save();
}

static void on_music_pill_toggled(bool active, gpointer user_data) {
    (void)user_data;
    g_config.music_pill_enabled = active;
}

static void on_music_pill_pos_changed(int idx, gpointer user_data) {
    (void)user_data;
    static const char *const pos_values[] = {"topbar_center", "dock_left", "dock_right", "disabled"};
    if (idx >= 0 && idx < 4) {
        strncpy(g_config.music_pill_position, pos_values[idx], sizeof(g_config.music_pill_position) - 1);
    }
}

static void on_blur_windows_toggled(bool active, gpointer user_data) { (void)user_data; g_config.blur_windows = active; }
static void on_blur_dock_toggled(bool active, gpointer user_data) { (void)user_data; g_config.blur_dock = active; }
static void on_blur_control_center_toggled(bool active, gpointer user_data) { (void)user_data; g_config.blur_control_center = active; }
static void on_blur_power_menu_toggled(bool active, gpointer user_data) { (void)user_data; g_config.blur_power_menu = active; }
static void on_blur_notifications_toggled(bool active, gpointer user_data) { (void)user_data; g_config.blur_notifications = active; }
static void on_blur_lockscreen_toggled(bool active, gpointer user_data) { (void)user_data; g_config.blur_lockscreen = active; }
static void on_blur_topbar_toggled(bool active, gpointer user_data) { (void)user_data; g_config.blur_topbar = active; }
static void on_blur_launchpad_toggled(bool active, gpointer user_data) { (void)user_data; g_config.blur_launchpad = active; }
static void on_blur_overview_toggled(bool active, gpointer user_data) { (void)user_data; g_config.blur_workspace_overview = active; }
static void on_liquid_glass_toggled(bool active, gpointer user_data) { (void)user_data; g_config.liquid_glass = active; }
static void on_blur_strength_changed(double val, gpointer user_data) { (void)user_data; g_config.blur_strength = val; }
static void on_blur_radius_changed(double val, gpointer user_data) { (void)user_data; g_config.blur_radius = (int)lround(val); }
static void on_blur_passes_changed(double val, gpointer user_data) { (void)user_data; g_config.blur_passes = (int)lround(val); }
static void on_blur_saturation_changed(double val, gpointer user_data) { (void)user_data; g_config.blur_saturation = val; }

// Running Windows Opacity Helpers
static void populate_running_windows(GtkWidget *card);

typedef struct {
    char app_id[128];
    GtkWidget *scale;
    GtkWidget *label;
} win_rule_ctx_t;

static void on_win_opacity_slider_changed(double val, gpointer user_data) {
    win_rule_ctx_t *ctx = user_data;
    for (int i = 0; i < g_config.window_rules_count; i++) {
        if (strcmp(g_config.window_rules[i].app_id, ctx->app_id) == 0) {
            g_config.window_rules[i].opacity = val;
            char pct[16];
            snprintf(pct, sizeof(pct), "%d%%", (int)(val * 100.0));
            gtk_label_set_text(GTK_LABEL(ctx->label), pct);
            settings_schedule_save();
            return;
        }
    }
    if (g_config.window_rules_count < 64) {
        snprintf(g_config.window_rules[g_config.window_rules_count].app_id,
                 sizeof(g_config.window_rules[0].app_id), "%s", ctx->app_id);
        g_config.window_rules[g_config.window_rules_count].title[0] = '\0';
        g_config.window_rules[g_config.window_rules_count].opacity = val;
        g_config.window_rules_count++;
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", (int)(val * 100.0));
        gtk_label_set_text(GTK_LABEL(ctx->label), pct);
        settings_schedule_save();
    }
}

static void on_win_opacity_reset_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    win_rule_ctx_t *ctx = user_data;
    gtk_range_set_value(GTK_RANGE(ctx->scale), 1.0);
    on_win_opacity_slider_changed(1.0, ctx);
}

static void on_win_rule_remove_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *app_id = (const char *)user_data;
    for (int i = 0; i < g_config.window_rules_count; i++) {
        if (strcmp(g_config.window_rules[i].app_id, app_id) == 0) {
            for (int j = i; j < g_config.window_rules_count - 1; j++) {
                g_config.window_rules[j] = g_config.window_rules[j + 1];
            }
            g_config.window_rules_count--;
            settings_schedule_save();
            break;
        }
    }
    if (g_win_opacity_box) {
        populate_running_windows(g_win_opacity_box);
    }
}

static void populate_running_windows(GtkWidget *card) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(card)) != NULL) {
        gtk_box_remove(GTK_BOX(card), child);
    }

    char seen_apps[64][128];
    int seen_count = 0;

    // Load running windows from /tmp/aurade-windows.json
    struct json_object *arr = json_object_from_file("/tmp/aurade-windows.json");
    if (arr && json_object_is_type(arr, json_type_array)) {
        size_t len = json_object_array_length(arr);
        for (size_t i = 0; i < len; i++) {
            struct json_object *obj = json_object_array_get_idx(arr, i);
            if (!obj) continue;
            struct json_object *aid_o, *title_o, *op_o;
            json_object_object_get_ex(obj, "app_id", &aid_o);
            json_object_object_get_ex(obj, "title", &title_o);
            json_object_object_get_ex(obj, "opacity", &op_o);

            const char *aid = aid_o ? json_object_get_string(aid_o) : "";
            const char *title = title_o ? json_object_get_string(title_o) : aid;
            if (!aid[0] && !title[0]) continue;
            const char *app_key = aid[0] ? aid : title;

            bool already_seen = false;
            for (int k = 0; k < seen_count; k++) {
                if (strcmp(seen_apps[k], app_key) == 0) { already_seen = true; break; }
            }
            if (already_seen) continue;
            if (seen_count < 64) {
                strncpy(seen_apps[seen_count++], app_key, 127);
            }

            double cur_op = 1.0;
            for (int r = 0; r < g_config.window_rules_count; r++) {
                if (strcmp(g_config.window_rules[r].app_id, app_key) == 0) {
                    cur_op = g_config.window_rules[r].opacity;
                    break;
                }
            }
            if (op_o && cur_op == 1.0) cur_op = json_object_get_double(op_o);
            if (cur_op < 0.3) cur_op = 1.0;

            win_rule_ctx_t *ctx = g_new0(win_rule_ctx_t, 1);
            strncpy(ctx->app_id, app_key, sizeof(ctx->app_id) - 1);

            GtkWidget *ctrl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *sc = ui_scale(cur_op, 0.30, 1.00, 0.05, 2, on_win_opacity_slider_changed, ctx);
            ctx->scale = sc;

            char pct_str[16];
            snprintf(pct_str, sizeof(pct_str), "%d%%", (int)(cur_op * 100.0));
            GtkWidget *lbl_pct = gtk_label_new(pct_str);
            gtk_widget_add_css_class(lbl_pct, "mono");
            ctx->label = lbl_pct;

            GtkWidget *btn_rst = gtk_button_new_with_label("Reset");
            gtk_widget_add_css_class(btn_rst, "btn-destructive");
            g_signal_connect(btn_rst, "clicked", G_CALLBACK(on_win_opacity_reset_clicked), ctx);

            gtk_box_append(GTK_BOX(ctrl), sc);
            gtk_box_append(GTK_BOX(ctrl), lbl_pct);
            gtk_box_append(GTK_BOX(ctrl), btn_rst);

            char sub[196];
            snprintf(sub, sizeof(sub), "%s • Running", app_key);
            ui_add_row(card, title[0] ? title : app_key, sub, ctrl, "preferences-system-windows-symbolic", "badge-appearance");
        }
        json_object_put(arr);
    }

    // Also list saved rules that are not currently running
    for (int r = 0; r < g_config.window_rules_count; r++) {
        const char *saved_app = g_config.window_rules[r].app_id;
        if (!saved_app[0]) continue;
        bool already_shown = false;
        for (int k = 0; k < seen_count; k++) {
            if (strcmp(seen_apps[k], saved_app) == 0) { already_shown = true; break; }
        }
        if (already_shown) continue;

        win_rule_ctx_t *ctx = g_new0(win_rule_ctx_t, 1);
        strncpy(ctx->app_id, saved_app, sizeof(ctx->app_id) - 1);

        double cur_op = g_config.window_rules[r].opacity;
        GtkWidget *ctrl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *sc = ui_scale(cur_op, 0.30, 1.00, 0.05, 2, on_win_opacity_slider_changed, ctx);
        ctx->scale = sc;

        char pct_str[16];
        snprintf(pct_str, sizeof(pct_str), "%d%%", (int)(cur_op * 100.0));
        GtkWidget *lbl_pct = gtk_label_new(pct_str);
        gtk_widget_add_css_class(lbl_pct, "mono");
        ctx->label = lbl_pct;

        GtkWidget *btn_rem = gtk_button_new_with_label("Remove");
        gtk_widget_add_css_class(btn_rem, "btn-destructive");
        g_signal_connect(btn_rem, "clicked", G_CALLBACK(on_win_rule_remove_clicked), g_strdup(saved_app));

        gtk_box_append(GTK_BOX(ctrl), sc);
        gtk_box_append(GTK_BOX(ctrl), lbl_pct);
        gtk_box_append(GTK_BOX(ctrl), btn_rem);

        char sub[196];
        snprintf(sub, sizeof(sub), "%s • (Saved Rule)", saved_app);
        ui_add_row(card, saved_app, sub, ctrl, "preferences-system-windows-symbolic", "badge-appearance");
    }

    if (gtk_widget_get_first_child(card) == NULL) {
        ui_add_row(card, "No Active Application Windows", "Launch applications to adjust per-window opacity", NULL, "view-dual-symbolic", "badge-appearance");
    }
}

GtkWidget *build_page_appearance(void) {
    GtkWidget *page = ui_page_container("Appearance", "Theme, accent colour, cursor styling and desktop wallpaper.");

    // Theme & Colors Card
    GtkWidget *c_theme = ui_card(page, "THEME & COLORS");
    char *live_dark = gsettings_get_string("org.gnome.desktop.interface", "color-scheme", "");
    bool is_dark = (strstr(live_dark, "dark") != NULL) || g_config.dark_mode;
    g_free(live_dark);

    GtkWidget *sw_dark = ui_switch(is_dark, on_dark_mode_toggled, NULL);
    ui_add_row(c_theme, "Dark Mode", "Switch between dark glass and light glass aesthetic", sw_dark, "weather-clear-night-symbolic", "badge-appearance");

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
    ui_add_row(c_theme, "Accent Colour", "Used for toggles, highlights and Control Center controls", box_accents, "applications-graphics-symbolic", "badge-wifi");

    // Cursor Theme & Size Card
    GtkWidget *c_cursor = ui_card(page, "CURSOR & POINTER");
    static const char *const CURSOR_THEMES[] = { "Aura-Circle", "WhiteSur-cursors", "Adwaita", "Papirus", NULL };
    char *cur_theme = gsettings_get_string("org.gnome.desktop.interface", "cursor-theme", "Aura-Circle");
    int c_idx = 0;
    for (int i = 0; i < 4; i++) {
        if (strcmp(cur_theme, CURSOR_THEMES[i]) == 0) { c_idx = i; break; }
    }
    g_free(cur_theme);
    GtkWidget *dd_ct = ui_dropdown(CURSOR_THEMES, 4, c_idx, on_cursor_theme_changed, NULL);
    ui_add_row(c_cursor, "Cursor Theme", "Choose the system-wide pointer appearance", dd_ct, "input-mouse-symbolic", "badge-mouse");

    static const char *const CURSOR_SIZES[] = { "24 px", "32 px", "48 px", "64 px", NULL };
    int cur_size = gsettings_get_int("org.gnome.desktop.interface", "cursor-size", 24);
    int s_idx = (cur_size <= 24) ? 0 : (cur_size <= 32) ? 1 : (cur_size <= 48) ? 2 : 3;
    GtkWidget *dd_cs = ui_dropdown(CURSOR_SIZES, 4, s_idx, on_cursor_size_changed, NULL);
    ui_add_row(c_cursor, "Cursor Size", "Scale pointer icon size for readability", dd_cs, "zoom-fit-best-symbolic", "badge-workspaces");

    // Wallpaper Card
    GtkWidget *c_wp = ui_card(page, "DESKTOP WALLPAPER");
    g_wallpaper_label = gtk_label_new(g_config.wallpaper_path[0] ? g_config.wallpaper_path : "Built-in dynamic gradient");
    gtk_widget_add_css_class(g_wallpaper_label, "mono");
    gtk_label_set_ellipsize(GTK_LABEL(g_wallpaper_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(g_wallpaper_label), 24);

    GtkWidget *wp_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *btn_choose = gtk_button_new_with_label("Choose…");
    gtk_widget_add_css_class(btn_choose, "btn-primary");
    g_signal_connect(btn_choose, "clicked", G_CALLBACK(on_choose_wallpaper_clicked), NULL);
    gtk_box_append(GTK_BOX(wp_buttons), btn_choose);

    GtkWidget *btn_clear = gtk_button_new_with_label("Default");
    g_signal_connect(btn_clear, "clicked", G_CALLBACK(on_clear_wallpaper_clicked), NULL);
    gtk_box_append(GTK_BOX(wp_buttons), btn_clear);

    ui_add_row(c_wp, "Current Image", "Custom image or macOS dynamic wallpaper", g_wallpaper_label, "preferences-desktop-wallpaper-symbolic", "badge-appearance");
    ui_add_row(c_wp, "Wallpaper Source", "Select custom wallpaper file or reset to default", wp_buttons, "folder-pictures-symbolic", "badge-appearance");

    // Music Pill Card
    GtkWidget *c_pill = ui_card(page, "MUSIC PILL (DYNAMIC ISLAND)");
    GtkWidget *sw_mp = ui_switch(g_config.music_pill_enabled, on_music_pill_toggled, NULL);
    ui_add_row(c_pill, "Show Music Pill", "Display an interactive dynamic island music pill when media is playing", sw_mp, "audio-x-generic-symbolic", "badge-sound");

    static const char *const PILL_POSITIONS[] = { "Top Bar (Center)", "Dock (Left)", "Dock (Right)", "Disabled", NULL };
    int pill_idx = (strcmp(g_config.music_pill_position, "dock_left") == 0) ? 1 :
                   (strcmp(g_config.music_pill_position, "dock_right") == 0) ? 2 :
                   (strcmp(g_config.music_pill_position, "disabled") == 0) ? 3 : 0;
    GtkWidget *dd_mp = ui_dropdown(PILL_POSITIONS, 4, pill_idx, on_music_pill_pos_changed, NULL);
    ui_add_row(c_pill, "Pill Placement", "Choose where the Dynamic Island appears (Top Bar Center, Dock Left or Right)", dd_mp, "preferences-system-symbolic", "badge-appearance");

    // Frosted Glass Blur Card
    GtkWidget *c_blur = ui_card(page, "FROSTED GLASS & BLUR ELEMENTS");
    GtkWidget *sw_win_b = ui_switch(g_config.blur_windows, on_blur_windows_toggled, NULL);
    ui_add_row(c_blur, "Window Blur", "Frosted glass backdrop effect under client windows", sw_win_b, "preferences-system-windows-symbolic", "badge-appearance");

    GtkWidget *sw_dock_b = ui_switch(g_config.blur_dock, on_blur_dock_toggled, NULL);
    ui_add_row(c_blur, "Dock Blur", "Frosted glass blur under the application dock", sw_dock_b, "view-paged-symbolic", "badge-dock");

    GtkWidget *sw_cc_b = ui_switch(g_config.blur_control_center, on_blur_control_center_toggled, NULL);
    ui_add_row(c_blur, "Control Center Blur", "Frosted glass blur under the macOS Control Center panel", sw_cc_b, "preferences-system-symbolic", "badge-appearance");

    GtkWidget *sw_pm_b = ui_switch(g_config.blur_power_menu, on_blur_power_menu_toggled, NULL);
    ui_add_row(c_blur, "Power Menu Blur", "Frosted glass blur under the Apple menu session panel", sw_pm_b, "system-shutdown-symbolic", "badge-appearance");

    GtkWidget *sw_notif_b = ui_switch(g_config.blur_notifications, on_blur_notifications_toggled, NULL);
    ui_add_row(c_blur, "Notifications Blur", "Frosted glass blur under banner popup cards", sw_notif_b, "preferences-system-notifications-symbolic", "badge-appearance");

    GtkWidget *sw_lock_b = ui_switch(g_config.blur_lockscreen, on_blur_lockscreen_toggled, NULL);
    ui_add_row(c_blur, "Lock Screen Blur", "Frosted wallpaper backdrop blur on the lock screen", sw_lock_b, "system-lock-screen-symbolic", "badge-appearance");

    GtkWidget *sw_top_b = ui_switch(g_config.blur_topbar, on_blur_topbar_toggled, NULL);
    ui_add_row(c_blur, "Top Bar Blur", "Frosted glass blur under the menu bar", sw_top_b, "preferences-desktop-wallpaper-symbolic", "badge-appearance");

    GtkWidget *sw_lp_b = ui_switch(g_config.blur_launchpad, on_blur_launchpad_toggled, NULL);
    ui_add_row(c_blur, "Launchpad Blur", "Full-screen frosted blur behind Launchpad app grid", sw_lp_b, "view-grid-symbolic", "badge-workspaces");

    GtkWidget *sw_ws_b = ui_switch(g_config.blur_workspace_overview, on_blur_overview_toggled, NULL);
    ui_add_row(c_blur, "Workspace Overview Blur", "Frosted wallpaper blur in Mission Control", sw_ws_b, "preferences-desktop-display-symbolic", "badge-appearance");

    // Optical Blur Tuning Card
    GtkWidget *c_opt = ui_card(page, "OPTICAL BLUR & LIQUID GLASS TUNING");
    GtkWidget *sw_lg = ui_switch(g_config.liquid_glass, on_liquid_glass_toggled, NULL);
    ui_add_row(c_opt, "Liquid Glass (Apple Vibrancy)", "Hardware vibrancy saturation boost, optical caustics, and Snell refraction sheen", sw_lg, "applications-graphics-symbolic", "badge-appearance");

    GtkWidget *sc_bst = ui_scale(g_config.blur_strength > 0.0 ? g_config.blur_strength : 1.0, 0.20, 2.50, 0.05, 2, on_blur_strength_changed, NULL);
    ui_add_row(c_opt, "Blur Strength", "Overall intensity scaling factor for frosted glass blur", sc_bst, "preferences-desktop-theme-symbolic", "badge-appearance");

    GtkWidget *sc_rad = ui_scale(g_config.blur_radius > 0 ? g_config.blur_radius : 3, 1, 8, 1, 0, on_blur_radius_changed, NULL);
    ui_add_row(c_opt, "Diffusion Radius", "Base optical diffusion radius in pixels (1 = crisp, 8 = heavy diffusion)", sc_rad, "preferences-desktop-display-symbolic", "badge-appearance");

    GtkWidget *sc_pass = ui_scale(g_config.blur_passes > 0 ? g_config.blur_passes : 2, 1, 4, 1, 0, on_blur_passes_changed, NULL);
    ui_add_row(c_opt, "Blur Passes", "Number of Kawase downsample / upsample filter iterations (1 = fastest, 4 = smoothest)", sc_pass, "emblem-system-symbolic", "badge-appearance");

    GtkWidget *sc_sat = ui_scale(g_config.blur_saturation > 0.1 ? g_config.blur_saturation : 1.25, 0.50, 2.00, 0.05, 2, on_blur_saturation_changed, NULL);
    ui_add_row(c_opt, "Vibrancy Saturation", "Color vibrance and saturation enhancement for backdrop lighting", sc_sat, "color-management-symbolic", "badge-appearance");

    // Running Windows Opacity Card
    GtkWidget *c_winop = ui_card(page, "RUNNING WINDOWS OPACITY");
    g_win_opacity_box = c_winop;
    populate_running_windows(c_winop);

    return page;
}

// -------------------------------------------------------------
// PAGE: DOCK
// -------------------------------------------------------------

static void on_dock_enabled_toggled(bool active, gpointer user_data) { (void)user_data; g_config.dock_enabled = active; }
static void on_dock_autohide_toggled(bool active, gpointer user_data) { (void)user_data; g_config.dock_autohide = active; }
static void on_dock_intellihide_toggled(bool active, gpointer user_data) { (void)user_data; g_config.dock_intellihide = active; }
static void on_dock_magnification_toggled(bool active, gpointer user_data) { (void)user_data; g_config.dock_magnification = active; }

static void on_dock_size_changed(double val, gpointer user_data) { (void)user_data; g_config.dock_icon_size = (int)val; }
static void on_dock_max_scale_changed(double val, gpointer user_data) { (void)user_data; g_config.dock_max_scale = val; }
static void on_dock_mag_radius_changed(double val, gpointer user_data) { (void)user_data; g_config.dock_mag_radius = val; }
static void on_dock_opacity_changed(double val, gpointer user_data) { (void)user_data; g_config.dock_opacity = val; }

static void on_dock_theme_changed(int idx, gpointer user_data) {
    (void)user_data;
    if (idx == 0) strncpy(g_config.dock_theme, DOCK_THEME_MODERN, sizeof(g_config.dock_theme) - 1);
    else if (idx == 1) strncpy(g_config.dock_theme, DOCK_THEME_TABLE_3D, sizeof(g_config.dock_theme) - 1);
    else if (idx == 2) strncpy(g_config.dock_theme, DOCK_THEME_PLANK, sizeof(g_config.dock_theme) - 1);
}

GtkWidget *build_page_dock(void) {
    GtkWidget *page = ui_page_container("Dock", "Autohide, intellihide, icon sizing, and magnification.");

    // Theme & Style Card
    GtkWidget *c_look = ui_card(page, "THEME & STYLE");
    static const char *const DOCK_THEMES[] = { "Modern Floating Glass Pill", "Classic 3D Glass Shelf (OS X Table)", "Plank Base Shelf", NULL };
    int sel_theme = 0;
    if (strcmp(g_config.dock_theme, DOCK_THEME_TABLE_3D) == 0) sel_theme = 1;
    else if (strcmp(g_config.dock_theme, DOCK_THEME_PLANK) == 0) sel_theme = 2;
    GtkWidget *dd_th = ui_dropdown(DOCK_THEMES, 3, sel_theme, on_dock_theme_changed, NULL);
    ui_add_row(c_look, "Dock Theme", "Choose between modern floating pill, classic OS X 3D glass table shelf, or Plank shelf", dd_th, "preferences-desktop-theme-symbolic", "badge-appearance");

    GtkWidget *sc_op = ui_scale(g_config.dock_opacity >= 0.0 ? g_config.dock_opacity : 0.85, 0.0, 1.0, 0.05, 2, on_dock_opacity_changed, NULL);
    ui_add_row(c_look, "Dock Opacity", "Adjust background transparency of the dock (0.0 = clear glass, 1.0 = solid)", sc_op, "preferences-color-symbolic", "badge-appearance");

    // Behaviour Card
    GtkWidget *c_behave = ui_card(page, "BEHAVIOUR");
    GtkWidget *sw_en = ui_switch(g_config.dock_enabled, on_dock_enabled_toggled, NULL);
    ui_add_row(c_behave, "Show the Dock", "Display the application dock along the bottom edge", sw_en, "view-paged-symbolic", "badge-dock");

    GtkWidget *sw_dock_blur = ui_switch(g_config.blur_dock, on_blur_dock_toggled, NULL);
    ui_add_row(c_behave, "Frosted Glass Blur", "Enable frosted glass blur effect under the dock", sw_dock_blur, "preferences-desktop-wallpaper-symbolic", "badge-appearance");

    GtkWidget *sw_dock_lg = ui_switch(g_config.liquid_glass, on_liquid_glass_toggled, NULL);
    ui_add_row(c_behave, "Liquid Glass Aesthetic", "Multi-layer specular highlights, caustics, and diagonal refractive borders", sw_dock_lg, "applications-graphics-symbolic", "badge-appearance");

    GtkWidget *sc_bst = ui_scale(g_config.blur_strength > 0.0 ? g_config.blur_strength : 1.0, 0.20, 2.50, 0.10, 1, on_blur_strength_changed, NULL);
    ui_add_row(c_behave, "Blur Strength", "Adjust the intensity and diffusion radius of the frosted glass blur", sc_bst, "preferences-desktop-theme-symbolic", "badge-appearance");

    GtkWidget *sw_ah = ui_switch(g_config.dock_autohide, on_dock_autohide_toggled, NULL);
    ui_add_row(c_behave, "Automatically Hide", "Hide the dock until the pointer reaches the bottom edge", sw_ah, "view-reveal-symbolic", "badge-wifi");

    GtkWidget *sw_ih = ui_switch(g_config.dock_intellihide, on_dock_intellihide_toggled, NULL);
    ui_add_row(c_behave, "Intelligent Autohide", "Hide the dock only when a window overlaps it", sw_ih, "preferences-system-windows-symbolic", "badge-workspaces");

    // Size & Magnification Card
    GtkWidget *c_mag = ui_card(page, "SIZE & MAGNIFICATION");
    GtkWidget *sc_sz = ui_scale(g_config.dock_icon_size >= 32 ? g_config.dock_icon_size : 52, 32, 96, 1, 0, on_dock_size_changed, NULL);
    ui_add_row(c_mag, "Icon Size", "Base icon size in pixels (32–96)", sc_sz, "preferences-system-symbolic", "badge-dock");

    GtkWidget *sw_mag = ui_switch(g_config.dock_magnification, on_dock_magnification_toggled, NULL);
    ui_add_row(c_mag, "Magnification", "Grow icons under the pointer like macOS", sw_mag, "zoom-in-symbolic", "badge-dock");

    GtkWidget *sc_max = ui_scale(g_config.dock_max_scale >= 1.0 ? g_config.dock_max_scale : 1.5, 1.0, 2.2, 0.05, 2, on_dock_max_scale_changed, NULL);
    ui_add_row(c_mag, "Maximum Scale", "How large an icon grows under the pointer", sc_max, "zoom-fit-best-symbolic", "badge-dock");

    GtkWidget *sc_rad = ui_scale(g_config.dock_mag_radius >= 40.0 ? g_config.dock_mag_radius : 120.0, 40, 300, 5, 0, on_dock_mag_radius_changed, NULL);
    ui_add_row(c_mag, "Magnification Range", "Radius of the magnification hump in pixels", sc_rad, "preferences-system-symbolic", "badge-dock");

    // Dynamic Island / Music Pill Card
    GtkWidget *c_music = ui_card(page, "MUSIC PILL (DYNAMIC ISLAND)");
    GtkWidget *sw_mp = ui_switch(g_config.music_pill_enabled, on_music_pill_toggled, NULL);
    ui_add_row(c_music, "Show Music Pill", "Display an interactive dynamic island music pill when media is playing", sw_mp, "audio-x-generic-symbolic", "badge-sound");

    static const char *const PILL_POSITIONS[] = { "Top Bar (Center)", "Dock (Left)", "Dock (Right)", "Disabled", NULL };
    int pill_idx = (strcmp(g_config.music_pill_position, "dock_left") == 0) ? 1 :
                   (strcmp(g_config.music_pill_position, "dock_right") == 0) ? 2 :
                   (strcmp(g_config.music_pill_position, "disabled") == 0) ? 3 : 0;
    GtkWidget *dd_mp = ui_dropdown(PILL_POSITIONS, 4, pill_idx, on_music_pill_pos_changed, NULL);
    ui_add_row(c_music, "Pill Placement", "Choose where the Dynamic Island appears (Top Bar Center, Dock Left or Right)", dd_mp, "preferences-system-symbolic", "badge-appearance");

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

static void on_gesture_4_up(int idx, gpointer user_data) { (void)user_data; g_config.swipe_up_4.type = dropdown_index_to_action(idx); }
static void on_gesture_4_down(int idx, gpointer user_data) { (void)user_data; g_config.swipe_down_4.type = dropdown_index_to_action(idx); }
static void on_gesture_4_left(int idx, gpointer user_data) { (void)user_data; g_config.swipe_left_4.type = dropdown_index_to_action(idx); }
static void on_gesture_4_right(int idx, gpointer user_data) { (void)user_data; g_config.swipe_right_4.type = dropdown_index_to_action(idx); }
static void on_gesture_4_pin(int idx, gpointer user_data) { (void)user_data; g_config.pinch_in_4.type = dropdown_index_to_action(idx); }
static void on_gesture_4_pout(int idx, gpointer user_data) { (void)user_data; g_config.pinch_out_4.type = dropdown_index_to_action(idx); }

static void on_gesture_edge_rt(int idx, gpointer user_data) { (void)user_data; g_config.edge_swipe_right.type = dropdown_index_to_action(idx); }

static void on_spring_stiff_changed(double val, gpointer user_data) { (void)user_data; g_config.spring.stiffness = val; }
static void on_spring_damp_changed(double val, gpointer user_data) { (void)user_data; g_config.spring.damping = val; }
static void on_spring_mass_changed(double val, gpointer user_data) { (void)user_data; g_config.spring.mass = val; }
static void on_gest_sens_changed(double val, gpointer user_data) { (void)user_data; g_config.sensitivity = val; }
static void on_gest_snap_changed(double val, gpointer user_data) { (void)user_data; g_config.snap_threshold = val; }
static void on_gest_deadzone_changed(double val, gpointer user_data) { (void)user_data; g_config.deadzone_px = val; }
static void on_gest_flick_changed(double val, gpointer user_data) { (void)user_data; g_config.flick_velocity_threshold = val; }
static void on_gest_rubber_changed(double val, gpointer user_data) { (void)user_data; g_config.rubber_band_limit = val; }

typedef struct {
    GtkWidget *bar;
    GtkWidget *lbl_fingers;
    GtkWidget *lbl_gesture;
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

                    char f_txt[32], g_txt[64];
                    snprintf(f_txt, sizeof(f_txt), "Fingers: %d", fingers);
                    snprintf(g_txt, sizeof(g_txt), "Gesture: %s", gest);
                    gtk_label_set_text(GTK_LABEL(t->lbl_fingers), f_txt);
                    gtk_label_set_text(GTK_LABEL(t->lbl_gesture), g_txt);
                }
                json_object_put(obj);
            }
            line = strtok(NULL, "\n");
        }
    }
    return TRUE;
}

GtkWidget *build_page_gestures(void) {
    GtkWidget *page = ui_page_container("Gestures", "Touchpad trackpad swipes, pinches, edge gestures, and physics tuning.");

    // Three-finger gestures
    GtkWidget *c_3f = ui_card(page, "THREE-FINGER GESTURES");
    GtkWidget *d_up3 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_up_3.type), on_gesture_3_up, NULL);
    ui_add_row(c_3f, "Swipe Up", "Three fingers sliding upwards", d_up3, "go-up-symbolic", "badge-gestures");

    GtkWidget *d_dn3 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_down_3.type), on_gesture_3_down, NULL);
    ui_add_row(c_3f, "Swipe Down", "Three fingers sliding downwards", d_dn3, "go-down-symbolic", "badge-gestures");

    GtkWidget *d_lf3 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_left_3.type), on_gesture_3_left, NULL);
    ui_add_row(c_3f, "Swipe Left", "Three fingers sliding to the left", d_lf3, "go-previous-symbolic", "badge-gestures");

    GtkWidget *d_rt3 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_right_3.type), on_gesture_3_right, NULL);
    ui_add_row(c_3f, "Swipe Right", "Three fingers sliding to the right", d_rt3, "go-next-symbolic", "badge-gestures");

    GtkWidget *d_pi3 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.pinch_in_3.type), on_gesture_3_pin, NULL);
    ui_add_row(c_3f, "Pinch In", "Thumb and fingers closing inward", d_pi3, "zoom-in-symbolic", "badge-gestures");

    GtkWidget *d_po3 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.pinch_out_3.type), on_gesture_3_pout, NULL);
    ui_add_row(c_3f, "Pinch Out", "Thumb and fingers spreading outward", d_po3, "zoom-out-symbolic", "badge-gestures");

    // Four-finger gestures
    GtkWidget *c_4f = ui_card(page, "FOUR-FINGER GESTURES");
    GtkWidget *d_up4 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_up_4.type), on_gesture_4_up, NULL);
    ui_add_row(c_4f, "Swipe Up", "Four fingers sliding upwards", d_up4, "go-up-symbolic", "badge-gestures");

    GtkWidget *d_dn4 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_down_4.type), on_gesture_4_down, NULL);
    ui_add_row(c_4f, "Swipe Down", "Four fingers sliding downwards", d_dn4, "go-down-symbolic", "badge-gestures");

    GtkWidget *d_lf4 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_left_4.type), on_gesture_4_left, NULL);
    ui_add_row(c_4f, "Swipe Left", "Four fingers sliding to the left", d_lf4, "go-previous-symbolic", "badge-gestures");

    GtkWidget *d_rt4 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.swipe_right_4.type), on_gesture_4_right, NULL);
    ui_add_row(c_4f, "Swipe Right", "Four fingers sliding to the right", d_rt4, "go-next-symbolic", "badge-gestures");

    GtkWidget *d_pi4 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.pinch_in_4.type), on_gesture_4_pin, NULL);
    ui_add_row(c_4f, "Pinch In", "Thumb and fingers closing inward", d_pi4, "zoom-in-symbolic", "badge-gestures");

    GtkWidget *d_po4 = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.pinch_out_4.type), on_gesture_4_pout, NULL);
    ui_add_row(c_4f, "Pinch Out / Spread", "Thumb and fingers spreading outward", d_po4, "zoom-out-symbolic", "badge-gestures");

    // Edge Gestures Card
    GtkWidget *c_edge = ui_card(page, "EDGE GESTURES");
    GtkWidget *d_ert = ui_dropdown(ACTION_LABELS, GESTURE_ACTIONS_COUNT, action_to_dropdown_index(g_config.edge_swipe_right.type), on_gesture_edge_rt, NULL);
    ui_add_row(c_edge, "Right Edge Swipe", "Swipe inward from the right trackpad edge (Control Center)", d_ert, "go-previous-symbolic", "badge-gestures");

    // Physics Tuning Card
    GtkWidget *c_phys = ui_card(page, "PHYSICS & SPRINGS");
    GtkWidget *sc_st = ui_scale(g_config.spring.stiffness >= 80.0 ? g_config.spring.stiffness : 280.0, 80.0, 600.0, 10.0, 0, on_spring_stiff_changed, NULL);
    ui_add_row(c_phys, "Spring Stiffness", "Higher values snap back faster with snappier response", sc_st, "preferences-system-symbolic", "badge-gestures");

    GtkWidget *sc_da = ui_scale(g_config.spring.damping >= 0.4 ? g_config.spring.damping : 0.92, 0.40, 1.30, 0.02, 2, on_spring_damp_changed, NULL);
    ui_add_row(c_phys, "Spring Damping", "Ratio of friction. Under 1.0 produces subtle natural overshoot bounce", sc_da, "preferences-system-symbolic", "badge-gestures");

    GtkWidget *sc_ma = ui_scale(g_config.spring.mass >= 0.5 ? g_config.spring.mass : 1.0, 0.5, 3.0, 0.1, 1, on_spring_mass_changed, NULL);
    ui_add_row(c_phys, "Virtual Mass", "Apparent weight of desktop windows during spring simulation", sc_ma, "preferences-system-symbolic", "badge-gestures");

    GtkWidget *sc_gsens = ui_scale(g_config.sensitivity >= 0.3 ? g_config.sensitivity : 1.0, 0.30, 2.50, 0.05, 2, on_gest_sens_changed, NULL);
    ui_add_row(c_phys, "Gesture Sensitivity", "Touchpad tracking speed multiplier for trackpad motions", sc_gsens, "preferences-system-symbolic", "badge-gestures");

    GtkWidget *sc_snap = ui_scale(g_config.snap_threshold >= 0.1 ? g_config.snap_threshold : 0.35, 0.10, 0.70, 0.02, 2, on_gest_snap_changed, NULL);
    ui_add_row(c_phys, "Snap Threshold", "Fraction of screen distance required to commit a gesture transition", sc_snap, "preferences-system-symbolic", "badge-gestures");

    GtkWidget *sc_dead = ui_scale(g_config.deadzone_px >= 1.0 ? g_config.deadzone_px : 8.0, 1.0, 25.0, 1.0, 0, on_gest_deadzone_changed, NULL);
    ui_add_row(c_phys, "Recognition Deadzone", "Ignore accidental micro-movements smaller than this threshold (px)", sc_dead, "preferences-system-symbolic", "badge-gestures");

    GtkWidget *sc_flk = ui_scale(g_config.flick_velocity_threshold >= 150.0 ? g_config.flick_velocity_threshold : 350.0, 150.0, 1500.0, 25.0, 0, on_gest_flick_changed, NULL);
    ui_add_row(c_phys, "Flick Speed Threshold", "Speed required to trigger an instant momentum flick", sc_flk, "preferences-system-symbolic", "badge-gestures");

    GtkWidget *sc_rub = ui_scale(g_config.rubber_band_limit >= 30.0 ? g_config.rubber_band_limit : 180.0, 30.0, 300.0, 10.0, 0, on_gest_rubber_changed, NULL);
    ui_add_row(c_phys, "Rubber-band Limit", "Maximum resistance stretch distance beyond screen bounds (px)", sc_rub, "preferences-system-symbolic", "badge-gestures");

    // Live Telemetry Socket Card
    GtkWidget *c_tel = ui_card(page, "LIVE TOUCHPAD TELEMETRY");
    GtkWidget *t_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(t_box, 10);
    gtk_widget_set_margin_bottom(t_box, 10);
    gtk_widget_set_margin_start(t_box, 14);
    gtk_widget_set_margin_end(t_box, 14);

    GtkWidget *t_sub = gtk_label_new("Touch the pad with 3 or 4 fingers to inspect live gesture recognition.");
    gtk_widget_add_css_class(t_sub, "subtitle");
    gtk_label_set_xalign(GTK_LABEL(t_sub), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(t_sub), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(t_sub), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(t_box), t_sub);

    GtkWidget *stat_lbls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *lbl_fin = gtk_label_new("Fingers: —");
    gtk_widget_add_css_class(lbl_fin, "mono");
    gtk_label_set_xalign(GTK_LABEL(lbl_fin), 0.0f);
    gtk_box_append(GTK_BOX(stat_lbls), lbl_fin);

    GtkWidget *lbl_gst = gtk_label_new("Gesture: —");
    gtk_widget_add_css_class(lbl_gst, "mono");
    gtk_label_set_xalign(GTK_LABEL(lbl_gst), 0.0f);
    gtk_widget_set_hexpand(lbl_gst, TRUE);
    gtk_box_append(GTK_BOX(stat_lbls), lbl_gst);
    gtk_box_append(GTK_BOX(t_box), stat_lbls);

    GtkWidget *pbar = gtk_progress_bar_new();
    gtk_widget_set_hexpand(pbar, TRUE);
    gtk_widget_set_valign(pbar, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(t_box), pbar);

    gtk_box_append(GTK_BOX(c_tel), t_box);

    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, "/tmp/aurade-gesture.sock", sizeof(addr.sun_path) - 1);
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            telemetry_ui_t *t = g_new0(telemetry_ui_t, 1);
            t->bar = pbar;
            t->lbl_fingers = lbl_fin;
            t->lbl_gesture = lbl_gst;
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

static void on_save_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    settings_save_now();
}

static void on_open_folder_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    run_cmd_async("xdg-open ~/.config/aurade >/dev/null 2>&1 &");
}

static void on_reset_defaults_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    settings_reset_defaults();
}

GtkWidget *build_page_about(void) {
    GtkWidget *page = ui_page_container("About This Computer", "AuraDE — a macOS Sonoma/Sequoia-inspired Wayland desktop environment.");

    char hostname[64] = "AuraDE";
    gethostname(hostname, sizeof(hostname) - 1);

    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    // Hero Banner (macOS Sonoma About This Mac style)
    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(hero, "hero-banner");

    GtkWidget *hero_badge = ui_make_badge("computer-symbolic", "badge-wifi", "row-badge");
    gtk_box_append(GTK_BOX(hero), hero_badge);

    GtkWidget *hcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(hcol, TRUE);

    char ht_buf[128];
    snprintf(ht_buf, sizeof(ht_buf), "%s • AuraDE Desktop", hostname);
    GtkWidget *ht = gtk_label_new(ht_buf);
    gtk_label_set_xalign(GTK_LABEL(ht), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(ht), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(ht), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(ht, "title");
    gtk_box_append(GTK_BOX(hcol), ht);

    char hs_buf[196];
    snprintf(hs_buf, sizeof(hs_buf), "Arch Linux  •  Wayland Native Compositor  •  Kernel %s", uts.release);
    GtkWidget *hs = gtk_label_new(hs_buf);
    gtk_label_set_xalign(GTK_LABEL(hs), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(hs), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(hs), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(hs, "subtitle");
    gtk_box_append(GTK_BOX(hcol), hs);
    gtk_box_append(GTK_BOX(hero), hcol);

    GtkWidget *ver_pill = gtk_label_new("AuraDE 2.0");
    gtk_widget_add_css_class(ver_pill, "connected-pill");
    gtk_widget_set_valign(ver_pill, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hero), ver_pill);

    gtk_box_append(GTK_BOX(page), hero);

    // Hardware & System Specifications Card
    GtkWidget *c_sys = ui_card(page, "HARDWARE & SYSTEM SPECIFICATIONS");

    char cpu_model[128] = "Unknown Processor";
    FILE *fcpu = fopen("/proc/cpuinfo", "r");
    if (fcpu) {
        char line[256];
        while (fgets(line, sizeof(line), fcpu)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    strncpy(cpu_model, colon + 2, sizeof(cpu_model) - 1);
                    char *nl = strchr(cpu_model, '\n');
                    if (nl) *nl = '\0';
                    break;
                }
            }
        }
        fclose(fcpu);
    }

    char ram_str[64] = "Unknown RAM";
    FILE *fmem = fopen("/proc/meminfo", "r");
    if (fmem) {
        char line[256];
        while (fgets(line, sizeof(line), fmem)) {
            if (strncmp(line, "MemTotal:", 9) == 0) {
                long long kb = 0;
                if (sscanf(line + 9, "%lld", &kb) == 1) {
                    snprintf(ram_str, sizeof(ram_str), "%.1f GB", (double)kb / (1024.0 * 1024.0));
                    break;
                }
            }
        }
        fclose(fmem);
    }

    char kver_str[256];
    snprintf(kver_str, sizeof(kver_str), "%s (%s)", uts.release, uts.machine);

    static const struct {
        const char *name;
        const char *val;
        const char *icon;
        const char *badge;
    } SYS_ITEMS[] = {
        { "Computer Name", NULL, "computer-symbolic", "badge-wifi" },
        { "Operating System", "Arch Linux", "applications-system-symbolic", "badge-display" },
        { "Desktop Environment", "AuraDE (Wayland Native)", "preferences-desktop-display-symbolic", "badge-workspaces" },
        { "Kernel Version", NULL, "utilities-terminal-symbolic", "badge-keyboard" },
        { "Processor", NULL, "preferences-system-symbolic", "badge-dock" },
        { "Memory (RAM)", NULL, "drive-harddisk-solidstate-symbolic", "badge-gestures" },
    };

    for (size_t i = 0; i < sizeof(SYS_ITEMS) / sizeof(SYS_ITEMS[0]); i++) {
        const char *val = SYS_ITEMS[i].val;
        if (i == 0) val = hostname;
        else if (i == 3) val = kver_str;
        else if (i == 4) val = cpu_model;
        else if (i == 5) val = ram_str;

        GtkWidget *vl = gtk_label_new(val);
        gtk_widget_add_css_class(vl, "subtitle");
        gtk_label_set_ellipsize(GTK_LABEL(vl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(vl), 28);
        ui_add_row(c_sys, SYS_ITEMS[i].name, NULL, vl, SYS_ITEMS[i].icon, SYS_ITEMS[i].badge);
    }

    // Configuration & Diagnostics Card
    GtkWidget *c_cfg = ui_card(page, "CONFIGURATION & DIAGNOSTICS");
    const char *cfg_path = config_get_default_path();
    GtkWidget *path_lbl = gtk_label_new(cfg_path);
    gtk_widget_add_css_class(path_lbl, "mono");
    gtk_label_set_selectable(GTK_LABEL(path_lbl), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(path_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(path_lbl), 24);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "btn-primary");
    g_signal_connect(save, "clicked", G_CALLBACK(on_save_clicked), NULL);
    gtk_box_append(GTK_BOX(buttons), save);

    GtkWidget *folder = gtk_button_new_with_label("Folder");
    g_signal_connect(folder, "clicked", G_CALLBACK(on_open_folder_clicked), NULL);
    gtk_box_append(GTK_BOX(buttons), folder);

    GtkWidget *reset = gtk_button_new_with_label("Defaults");
    gtk_widget_add_css_class(reset, "btn-destructive");
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_defaults_clicked), NULL);
    gtk_box_append(GTK_BOX(buttons), reset);

    ui_add_row(c_cfg, "Configuration File", "JSON configuration live-reloaded by the AuraDE compositor", path_lbl, "document-properties-symbolic", "badge-about");
    ui_add_row(c_cfg, "Profile Actions", "Save, inspect, or reset all AuraDE settings to factory defaults", buttons, "preferences-system-symbolic", "badge-about");

    return page;
}
