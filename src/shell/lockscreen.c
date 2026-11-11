#include "lockscreen.h"
#include "wallpaper.h"
#include "../util/apps.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#include <pthread.h>
#include <security/pam_appl.h>

#define FONT_UI "SF Pro Display, -apple-system, Cantarell, Ubuntu, sans-serif"

static int utf8_codepoint_count(const char *s) {
    if (!s) return 0;
    int count = 0;
    while (*s) {
        if (((unsigned char)*s & 0xC0) != 0x80) {
            count++;
        }
        s++;
    }
    return count;
}

static void utf8_remove_last_char(char *s, int *len) {
    if (!s || !len || *len <= 0) return;
    int l = *len;
    do {
        l--;
    } while (l > 0 && (((unsigned char)s[l]) & 0xC0) == 0x80);
    s[l] = '\0';
    *len = l;
}

static int pam_conv_cb(int num_msg, const struct pam_message **msg,
                       struct pam_response **resp, void *appdata_ptr) {
    if (num_msg <= 0) return PAM_CONV_ERR;
    struct pam_response *r = calloc(num_msg, sizeof(struct pam_response));
    if (!r) return PAM_BUF_ERR;
    const char *pass = (const char *)appdata_ptr;
    for (int i = 0; i < num_msg; i++) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            r[i].resp = strdup(pass ? pass : "");
            r[i].resp_retcode = 0;
        }
    }
    *resp = r;
    return PAM_SUCCESS;
}

static void *pam_worker_thread(void *arg) {
    pam_task_t *task = (pam_task_t *)arg;
    struct pam_conv conv = {
        .conv = pam_conv_cb,
        .appdata_ptr = task->password
    };
    pam_handle_t *pamh = NULL;
    int rc = pam_start("login", task->username, &conv, &pamh);
    if (rc != PAM_SUCCESS) {
        rc = pam_start("hyprlock", task->username, &conv, &pamh);
    }
    if (rc == PAM_SUCCESS) {
        rc = pam_authenticate(pamh, 0);
        if (rc == PAM_SUCCESS) {
            rc = pam_acct_mgmt(pamh, 0);
        }
        pam_end(pamh, rc);
    }

    task->success = (rc == PAM_SUCCESS);
    memset(task->password, 0, sizeof(task->password));
    __atomic_store_n(&task->done, true, __ATOMIC_RELEASE);
    return NULL;
}

static void lockscreen_start_pam_task(aurade_lockscreen_t *ls) {
    if (ls->pam_task || ls->authenticating) return;
    pam_task_t *task = calloc(1, sizeof(pam_task_t));
    if (!task) return;

    snprintf(task->username, sizeof(task->username), "%s", ls->username);
    snprintf(task->password, sizeof(task->password), "%s", ls->password);
    task->done = false;
    task->success = false;
    ls->pam_task = task;
    ls->authenticating = true;
    ls->spinner_angle = 0.0;
    ls->needs_render = true;

    if (pthread_create(&task->thread, NULL, pam_worker_thread, task) != 0) {
        free(task);
        ls->pam_task = NULL;
        ls->authenticating = false;
    }
}

aurade_lockscreen_t *lockscreen_create(aurade_server_t *server) {
    if (!server || !server->lock_tree) return NULL;
    aurade_lockscreen_t *ls = calloc(1, sizeof(*ls));
    if (!ls) return NULL;

    ls->server = server;
    ls->width = 1920;
    ls->height = 1080;
    if (!wl_list_empty(&server->outputs)) {
        aurade_output_t *out = wl_container_of(server->outputs.next, out, link);
        if (out->width > 0 && out->height > 0) {
            ls->width = out->width;
            ls->height = out->height;
        }
    }

    struct passwd *pw = getpwuid(getuid());
    if (pw) {
        snprintf(ls->username, sizeof(ls->username), "%s", pw->pw_name);
        if (pw->pw_gecos && *pw->pw_gecos) {
            char *comma = strchr(pw->pw_gecos, ',');
            if (comma) {
                int len = (int)(comma - pw->pw_gecos);
                if (len >= (int)sizeof(ls->display_name)) len = sizeof(ls->display_name) - 1;
                strncpy(ls->display_name, pw->pw_gecos, len);
                ls->display_name[len] = '\0';
            } else {
                snprintf(ls->display_name, sizeof(ls->display_name), "%s", pw->pw_gecos);
            }
        } else {
            snprintf(ls->display_name, sizeof(ls->display_name), "%s", pw->pw_name);
        }
    } else {
        snprintf(ls->username, sizeof(ls->username), "user");
        snprintf(ls->display_name, sizeof(ls->display_name), "User");
    }

    ls->state = LOCKSCREEN_STATE_UNLOCKED;
    ls->progress = 0.0;

    ls->buffer = cairo_buffer_create(ls->width, ls->height);
    ls->scene_tree = wlr_scene_tree_create(server->lock_tree);
    ls->scene_buffer = wlr_scene_buffer_create(ls->scene_tree, &ls->buffer->base);
    wlr_scene_node_set_enabled(&ls->scene_tree->node, false);

    return ls;
}

void lockscreen_destroy(aurade_lockscreen_t *ls) {
    if (!ls) return;
    if (ls->pam_task) {
        pthread_join(ls->pam_task->thread, NULL);
        free(ls->pam_task);
        ls->pam_task = NULL;
    }
    if (ls->buffer) {
        wlr_buffer_drop(&ls->buffer->base);
        ls->buffer = NULL;
    }
    free(ls);
}

void lockscreen_resize(aurade_lockscreen_t *ls, int width, int height) {
    if (!ls || (ls->width == width && ls->height == height)) return;
    ls->width = width;
    ls->height = height;
    cairo_buffer_resize(ls->buffer, width, height);
    ls->needs_render = true;
}

void lockscreen_show(aurade_lockscreen_t *ls) {
    if (!ls) return;
    ls->state = LOCKSCREEN_STATE_LOCKING;
    ls->progress = 0.0;
    ls->password_len = 0;
    ls->password[0] = '\0';
    ls->error_msg[0] = '\0';
    ls->shake_active = false;
    ls->shake_offset = 0.0;
    ls->authenticating = false;
    ls->hover_submit = false;
    ls->hover_action = 0;
    ls->needs_render = true;

    wlr_scene_node_set_enabled(&ls->scene_tree->node, true);
    wlr_scene_node_raise_to_top(&ls->scene_tree->node);
    schedule_all_outputs(ls->server);
}

void lockscreen_unlock(aurade_lockscreen_t *ls) {
    if (!ls || ls->state == LOCKSCREEN_STATE_UNLOCKED) return;
    ls->state = LOCKSCREEN_STATE_UNLOCKING;
    ls->progress = 1.0;
    ls->password_len = 0;
    memset(ls->password, 0, sizeof(ls->password));
    ls->needs_render = true;
    schedule_all_outputs(ls->server);
}

bool lockscreen_is_active(const aurade_lockscreen_t *ls) {
    return ls && ls->state != LOCKSCREEN_STATE_UNLOCKED;
}

bool lockscreen_is_animating(const aurade_lockscreen_t *ls) {
    if (!ls) return false;
    if (ls->state == LOCKSCREEN_STATE_LOCKING || ls->state == LOCKSCREEN_STATE_UNLOCKING) return true;
    if (ls->shake_active || ls->authenticating) return true;
    return false;
}

void lockscreen_tick(aurade_lockscreen_t *ls, double dt) {
    if (!ls || ls->state == LOCKSCREEN_STATE_UNLOCKED) return;

    if (ls->state == LOCKSCREEN_STATE_LOCKING) {
        ls->progress += dt / 0.24;
        if (ls->progress >= 1.0) {
            ls->progress = 1.0;
            ls->state = LOCKSCREEN_STATE_LOCKED;
        }
        ls->needs_render = true;
    } else if (ls->state == LOCKSCREEN_STATE_UNLOCKING) {
        ls->progress -= dt / 0.22;
        if (ls->progress <= 0.0) {
            ls->progress = 0.0;
            ls->state = LOCKSCREEN_STATE_UNLOCKED;
            wlr_scene_node_set_enabled(&ls->scene_tree->node, false);
        }
        ls->needs_render = true;
    }

    if (ls->shake_active) {
        ls->shake_time += dt;
        double dur = 0.40;
        if (ls->shake_time >= dur) {
            ls->shake_active = false;
            ls->shake_offset = 0.0;
        } else {
            double p = ls->shake_time / dur;
            ls->shake_offset = sin(p * 5.0 * 2.0 * M_PI) * (1.0 - p) * 16.0;
        }
        ls->needs_render = true;
    }

    if (ls->authenticating) {
        ls->spinner_angle += dt * 8.0;
        ls->needs_render = true;
    }

    if (ls->pam_task && __atomic_load_n(&ls->pam_task->done, __ATOMIC_ACQUIRE)) {
        pthread_join(ls->pam_task->thread, NULL);
        bool ok = ls->pam_task->success;
        free(ls->pam_task);
        ls->pam_task = NULL;
        ls->authenticating = false;

        if (ok) {
            lockscreen_unlock(ls);
        } else {
            ls->shake_active = true;
            ls->shake_time = 0.0;
            ls->password_len = 0;
            memset(ls->password, 0, sizeof(ls->password));
            snprintf(ls->error_msg, sizeof(ls->error_msg), "Incorrect password. Try again.");
            ls->needs_render = true;
            schedule_all_outputs(ls->server);
        }
    }
}

static bool read_battery_status(int *capacity, bool *charging) {
    FILE *f_cap = fopen("/sys/class/power_supply/BAT0/capacity", "r");
    if (!f_cap) f_cap = fopen("/sys/class/power_supply/BAT1/capacity", "r");
    if (!f_cap) return false;
    if (fscanf(f_cap, "%d", capacity) != 1) {
        fclose(f_cap);
        return false;
    }
    fclose(f_cap);

    FILE *f_st = fopen("/sys/class/power_supply/BAT0/status", "r");
    if (!f_st) f_st = fopen("/sys/class/power_supply/BAT1/status", "r");
    *charging = false;
    if (f_st) {
        char buf[32] = {0};
        if (fscanf(f_st, "%31s", buf) == 1) {
            if (strcmp(buf, "Charging") == 0 || strcmp(buf, "Full") == 0) {
                *charging = true;
            }
        }
        fclose(f_st);
    }
    return true;
}

void lockscreen_render(aurade_lockscreen_t *ls) {
    if (!ls || !ls->buffer || !ls->buffer->cr) return;
    ls->needs_render = false;
    cairo_t *cr = ls->buffer->cr;
    cairo_buffer_clear(ls->buffer);

    double p = ls->progress;
    if (p <= 0.001) {
        cairo_buffer_apply_to_scene(ls->buffer, ls->scene_buffer);
        return;
    }
    if (p > 1.0) p = 1.0;
    double alpha = p;

    int w = ls->width > 0 ? ls->width : 1920;
    int h = ls->height > 0 ? ls->height : 1080;

    // 1. Wallpaper background backdrop
    bool blur_lock = ls->server ? ls->server->config.blur_lockscreen : true;
    if (ls->server && ls->server->wallpaper && blur_lock) {
        wallpaper_draw_backdrop(ls->server->wallpaper, cr, w, h, 0.35, alpha);
    }

    // 2. Translucent dark frosted glass overlay
    cairo_set_source_rgba(cr, 0.03, 0.04, 0.06, (blur_lock ? 0.50 : 0.85) * alpha);
    cairo_paint(cr);

    // 3. Top status indicators (Battery, Wi-Fi, Keyboard layout)
    double top_y = 22.0;
    double cur_right = w - 28.0;

    // Battery
    int bat_cap = 0;
    bool bat_chg = false;
    if (read_battery_status(&bat_cap, &bat_chg)) {
        double bw = 24.0, bh = 11.5;
        cur_right -= bw;
        draw_battery_icon(cr, cur_right, top_y - bh / 2.0, bw, bh,
                          bat_cap / 100.0, 0.90 * alpha, bat_chg);
        char cap_str[16];
        snprintf(cap_str, sizeof(cap_str), "%d%%", bat_cap);
        double tw = text_width(cr, cap_str, FONT_UI, 11.5, true);
        cur_right -= (tw + 6.0);
        draw_text(cr, cap_str, FONT_UI, 11.5, true, 1.0, 1.0, 1.0, 0.88 * alpha,
                  cur_right, top_y, false, true);
        cur_right -= 18.0;
    }

    // Wi-Fi icon
    draw_wifi_icon(cr, cur_right - 8.0, top_y, 16.0, 0.90 * alpha);
    cur_right -= 30.0;

    // Keyboard layout pill (e.g. ABC, UA, EN)
    const char *layout_str = "ABC";
    if (ls->server && ls->server->seat && ls->server->seat->keyboard_state.keyboard) {
        struct wlr_keyboard *kbd = ls->server->seat->keyboard_state.keyboard;
        if (kbd->keymap) {
            xkb_layout_index_t grp = xkb_state_serialize_layout(kbd->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
            const char *name = xkb_keymap_layout_get_name(kbd->keymap, grp);
            if (name) {
                if (strstr(name, "Ukrainian") || strstr(name, "Ukraine")) layout_str = "UA";
                else if (strstr(name, "English") || strstr(name, "US")) layout_str = "EN";
                else if (strstr(name, "German")) layout_str = "DE";
                else layout_str = "ABC";
            }
        }
    }
    double pill_tw = text_width(cr, layout_str, FONT_UI, 10.5, true);
    double lay_w = pill_tw + 14.0;
    double lay_h = 19.0;
    cur_right -= lay_w;
    ls->layout_x = cur_right;
    ls->layout_y = top_y - lay_h / 2.0;
    ls->layout_w = lay_w;
    ls->layout_h = lay_h;

    draw_rounded_rect(cr, cur_right, top_y - lay_h / 2.0, lay_w, lay_h, 5.0);
    if (ls->hover_layout) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.30 * alpha);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18 * alpha);
    }
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.28 * alpha);
    cairo_set_line_width(cr, 0.8);
    cairo_stroke(cr);
    draw_text(cr, layout_str, FONT_UI, 10.5, true, 1.0, 1.0, 1.0, 0.95 * alpha,
              cur_right + lay_w / 2.0, top_y, true, true);

    // 4. Time & Date (Upper-center)
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M", &tm);
    char date_str[64];
    strftime(date_str, sizeof(date_str), "%A, %B %-d", &tm);

    double cx = w / 2.0;
    double time_y = h * 0.17;

    draw_text_shadowed(cr, time_str, FONT_UI, 72.0, true,
                       1.0, 1.0, 1.0, 0.98 * alpha, 0.50 * alpha,
                       cx, time_y, true, true);
    draw_text_shadowed(cr, date_str, FONT_UI, 16.5, false,
                       0.94, 0.95, 0.98, 0.92 * alpha, 0.35 * alpha,
                       cx, time_y + 60.0, true, true);

    // 5. User Avatar & Display Name
    double avatar_y = h * 0.43;
    double avatar_r = 44.0;

    // Avatar drop shadow
    draw_drop_shadow(cr, cx - avatar_r, avatar_y - avatar_r + 2.0,
                     avatar_r * 2.0, avatar_r * 2.0, avatar_r, 14.0, 0.45 * alpha);

    // Avatar circle
    cairo_save(cr);
    cairo_arc(cr, cx, avatar_y, avatar_r, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.18, 0.20, 0.26, 0.88 * alpha);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.35 * alpha);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    // Silhouette icon inside avatar
    cairo_arc(cr, cx, avatar_y, avatar_r - 1.0, 0, 2 * M_PI);
    cairo_clip(cr);

    // Head
    cairo_arc(cr, cx, avatar_y - 9.0, 14.0, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85 * alpha);
    cairo_fill(cr);

    // Torso shoulders
    cairo_arc(cr, cx, avatar_y + 36.0, 26.0, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85 * alpha);
    cairo_fill(cr);
    cairo_restore(cr);

    // User name below avatar
    double name_y = avatar_y + avatar_r + 20.0;
    draw_text_shadowed(cr, ls->display_name, FONT_UI, 18.0, true,
                       1.0, 1.0, 1.0, 0.98 * alpha, 0.45 * alpha,
                       cx, name_y, true, true);

    // 6. Password Input Pill with Shake animation
    double shake_x = cx + ls->shake_offset;
    double pill_w = 216.0;
    double pill_h = 34.0;
    double pill_r = pill_h / 2.0;
    double pill_x = shake_x - pill_w / 2.0;
    double pill_y = name_y + 36.0;

    ls->input_x = pill_x;
    ls->input_y = pill_y;
    ls->input_w = pill_w;
    ls->input_h = pill_h;

    // Drop shadow behind input
    draw_drop_shadow(cr, pill_x, pill_y + 1.0, pill_w, pill_h, pill_r, 8.0, 0.35 * alpha);

    // Pill glass background
    draw_rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, pill_r);
    cairo_set_source_rgba(cr, 0.12, 0.14, 0.18, 0.65 * alpha);
    cairo_fill_preserve(cr);

    // Active focus border
    cairo_set_source_rgba(cr, 0.28, 0.68, 1.0, 0.85 * alpha);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    // Submit arrow button on right
    double btn_cx = pill_x + pill_w - 18.0;
    double btn_cy = pill_y + pill_h / 2.0;
    double btn_r = 11.5;

    ls->submit_x = btn_cx;
    ls->submit_y = btn_cy;
    ls->submit_r = btn_r;

    if (ls->password_len > 0) {
        cairo_arc(cr, btn_cx, btn_cy, btn_r, 0, 2 * M_PI);
        if (ls->hover_submit) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.45 * alpha);
        } else {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.24 * alpha);
        }
        cairo_fill(cr);

        if (ls->authenticating) {
            // Rotating spinner
            cairo_save(cr);
            cairo_translate(cr, btn_cx, btn_cy);
            cairo_rotate(cr, ls->spinner_angle);
            cairo_set_line_width(cr, 1.8);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_arc(cr, 0, 0, 6.0, 0, 1.5 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * alpha);
            cairo_stroke(cr);
            cairo_restore(cr);
        } else {
            // Right arrow (➔)
            double arr_sz = 3.5;
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * alpha);
            cairo_set_line_width(cr, 1.5);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_new_path(cr);
            cairo_move_to(cr, btn_cx - 3.5, btn_cy);
            cairo_line_to(cr, btn_cx + 3.0, btn_cy);
            cairo_move_to(cr, btn_cx + 3.0 - arr_sz, btn_cy - arr_sz);
            cairo_line_to(cr, btn_cx + 3.0, btn_cy);
            cairo_line_to(cr, btn_cx + 3.0 - arr_sz, btn_cy + arr_sz);
            cairo_stroke(cr);
        }
    }

    // Caps Lock indicator icon
    if (ls->caps_lock) {
        double caps_cx = btn_cx - 24.0;
        double caps_cy = btn_cy;
        draw_rounded_rect(cr, caps_cx - 8.0, caps_cy - 8.0, 16.0, 16.0, 4.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22 * alpha);
        cairo_fill(cr);
        draw_text(cr, "⇪", FONT_UI, 11.0, true, 1.0, 1.0, 1.0, 0.95 * alpha,
                  caps_cx, caps_cy, true, true);
    }

    // Password input text / dots
    int dot_count = utf8_codepoint_count(ls->password);
    if (dot_count == 0) {
        draw_text(cr, "Enter Password", FONT_UI, 12.5, false,
                  0.75, 0.78, 0.84, 0.60 * alpha,
                  pill_x + 18.0, pill_y + pill_h / 2.0, false, true);
    } else {
        // Centered masked password dots (●)
        double dot_spacing = 11.0;
        double total_dots_w = (dot_count - 1) * dot_spacing;
        double start_dot_x = shake_x - total_dots_w / 2.0;
        if (start_dot_x + total_dots_w > btn_cx - 20.0) {
            start_dot_x = btn_cx - 20.0 - total_dots_w;
        }
        for (int i = 0; i < dot_count; i++) {
            double dx = start_dot_x + i * dot_spacing;
            cairo_arc(cr, dx, pill_y + pill_h / 2.0, 3.2, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96 * alpha);
            cairo_fill(cr);
        }
    }

    // 7. Error Message
    if (ls->error_msg[0] != '\0') {
        draw_text(cr, ls->error_msg, FONT_UI, 12.0, true,
                  1.0, 0.35, 0.35, 0.95 * alpha,
                  cx, pill_y + pill_h + 16.0, true, true);
    }

    // 8. Bottom Power Controls (Sleep, Restart, Shut Down)
    double bottom_y = h - 68.0;
    double spacing = 96.0;

    ls->btn_sleep_x = cx - spacing;
    ls->btn_sleep_y = bottom_y;
    ls->btn_restart_x = cx;
    ls->btn_restart_y = bottom_y;
    ls->btn_power_x = cx + spacing;
    ls->btn_power_y = bottom_y;
    ls->btn_r = 19.0;

    // Helper to draw clean circular action buttons
    struct { double x; const char *label; int act_id; } actions[3] = {
        { ls->btn_sleep_x, "Sleep", 1 },
        { ls->btn_restart_x, "Restart", 2 },
        { ls->btn_power_x, "Shut Down", 3 }
    };

    for (int i = 0; i < 3; i++) {
        double bx = actions[i].x;
        bool hover = (ls->hover_action == actions[i].act_id);

        cairo_arc(cr, bx, bottom_y, ls->btn_r, 0, 2 * M_PI);
        if (hover) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.30 * alpha);
        } else {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15 * alpha);
        }
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.28 * alpha);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Icon
        if (actions[i].act_id == 1) {
            draw_moon_icon(cr, bx, bottom_y, 14.0, 0.92 * alpha);
        } else if (actions[i].act_id == 2) {
            // Restart icon (circular arrow)
            cairo_save(cr);
            cairo_set_line_width(cr, 1.5);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_arc(cr, bx, bottom_y, 6.0, -0.6 * M_PI, 1.2 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92 * alpha);
            cairo_stroke(cr);
            // arrow head
            cairo_move_to(cr, bx - 6.0, bottom_y - 2.0);
            cairo_line_to(cr, bx - 6.0, bottom_y + 2.0);
            cairo_line_to(cr, bx - 2.0, bottom_y);
            cairo_fill(cr);
            cairo_restore(cr);
        } else {
            // Power / Shut down icon (circle + top tick)
            cairo_save(cr);
            cairo_set_line_width(cr, 1.5);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_arc(cr, bx, bottom_y + 1.0, 6.0, -0.35 * M_PI, 1.35 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92 * alpha);
            cairo_stroke(cr);
            cairo_move_to(cr, bx, bottom_y - 6.0);
            cairo_line_to(cr, bx, bottom_y);
            cairo_stroke(cr);
            cairo_restore(cr);
        }

        // Label below
        draw_text_shadowed(cr, actions[i].label, FONT_UI, 11.0, false,
                           1.0, 1.0, 1.0, 0.88 * alpha, 0.35 * alpha,
                           bx, bottom_y + ls->btn_r + 14.0, true, true);
    }

    cairo_buffer_apply_to_scene(ls->buffer, ls->scene_buffer);
}

bool lockscreen_handle_key(aurade_lockscreen_t *ls, xkb_keysym_t sym,
                           struct xkb_state *xkb_state, bool pressed) {
    if (!ls || ls->state == LOCKSCREEN_STATE_UNLOCKED) return false;

    if (xkb_state) {
        bool caps = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CAPS,
                                                 XKB_STATE_MODS_EFFECTIVE) > 0;
        if (caps != ls->caps_lock) {
            ls->caps_lock = caps;
            ls->needs_render = true;
            schedule_all_outputs(ls->server);
        }
    }

    if (!pressed) return true; // Consume key release events unconditionally

    bool ctrl = xkb_state && xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL,
                                                          XKB_STATE_MODS_EFFECTIVE) > 0;
    bool alt = xkb_state && xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_ALT,
                                                         XKB_STATE_MODS_EFFECTIVE) > 0;
    bool logo = xkb_state && xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_LOGO,
                                                          XKB_STATE_MODS_EFFECTIVE) > 0;

    if (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
        ls->password_len = 0;
        ls->password[0] = '\0';
        ls->error_msg[0] = '\0';
        ls->needs_render = true;
        schedule_all_outputs(ls->server);
        return true;
    }

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (!ls->authenticating) {
            if (ls->password_len == 0) {
                ls->shake_active = true;
                ls->shake_time = 0.0;
                snprintf(ls->error_msg, sizeof(ls->error_msg), "Password required");
                ls->needs_render = true;
                schedule_all_outputs(ls->server);
            } else {
                lockscreen_start_pam_task(ls);
            }
        }
        return true;
    }

    if (sym == XKB_KEY_BackSpace) {
        if (ls->password_len > 0) {
            utf8_remove_last_char(ls->password, &ls->password_len);
            ls->error_msg[0] = '\0';
            ls->needs_render = true;
            schedule_all_outputs(ls->server);
        }
        return true;
    }

    if (sym == XKB_KEY_Escape) {
        ls->password_len = 0;
        ls->password[0] = '\0';
        ls->error_msg[0] = '\0';
        ls->needs_render = true;
        schedule_all_outputs(ls->server);
        return true;
    }

    // Do not append characters if modifiers are pressed (Ctrl, Alt, Super/Logo)
    if (ctrl || alt || logo) {
        return true;
    }

    // Convert keysym to UTF-8
    char utf8[16] = {0};
    int ret = xkb_keysym_to_utf8(sym, utf8, sizeof(utf8));
    // xkb_keysym_to_utf8 returns size INCLUDING null terminator, so ret > 1 means characters exist
    if (ret > 1 && (unsigned char)utf8[0] >= 32 && (unsigned char)utf8[0] != 127) {
        int byte_len = (int)strlen(utf8);
        if (ls->password_len + byte_len < (int)sizeof(ls->password) - 1) {
            memcpy(ls->password + ls->password_len, utf8, byte_len);
            ls->password_len += byte_len;
            ls->password[ls->password_len] = '\0';
            ls->error_msg[0] = '\0';
            ls->needs_render = true;
            schedule_all_outputs(ls->server);
        }
    }

    return true; // All key strokes are swallowed while locked
}

void lockscreen_handle_pointer_motion(aurade_lockscreen_t *ls, double x, double y) {
    if (!ls || ls->state == LOCKSCREEN_STATE_UNLOCKED) return;

    bool prev_sub = ls->hover_submit;
    int prev_act = ls->hover_action;
    bool prev_lay = ls->hover_layout;

    ls->hover_submit = (hypot(x - ls->submit_x, y - ls->submit_y) <= ls->submit_r + 4.0);

    ls->hover_layout = (x >= ls->layout_x && x <= ls->layout_x + ls->layout_w &&
                        y >= ls->layout_y && y <= ls->layout_y + ls->layout_h);

    ls->hover_action = 0;
    if (hypot(x - ls->btn_sleep_x, y - ls->btn_sleep_y) <= ls->btn_r + 6.0) {
        ls->hover_action = 1;
    } else if (hypot(x - ls->btn_restart_x, y - ls->btn_restart_y) <= ls->btn_r + 6.0) {
        ls->hover_action = 2;
    } else if (hypot(x - ls->btn_power_x, y - ls->btn_power_y) <= ls->btn_r + 6.0) {
        ls->hover_action = 3;
    }

    if (ls->hover_submit != prev_sub || ls->hover_action != prev_act || ls->hover_layout != prev_lay) {
        ls->needs_render = true;
        schedule_all_outputs(ls->server);
    }
}

bool lockscreen_handle_button(aurade_lockscreen_t *ls, double x, double y,
                              uint32_t button, bool pressed) {
    if (!ls || ls->state == LOCKSCREEN_STATE_UNLOCKED) return false;
    (void)button;

    if (pressed) {
        if (x >= ls->layout_x && x <= ls->layout_x + ls->layout_w &&
            y >= ls->layout_y && y <= ls->layout_y + ls->layout_h) {
            server_cycle_keyboard_layout(ls->server);
            ls->needs_render = true;
            schedule_all_outputs(ls->server);
            return true;
        }

        if (hypot(x - ls->submit_x, y - ls->submit_y) <= ls->submit_r + 4.0) {
            if (!ls->authenticating && ls->password_len > 0) {
                lockscreen_start_pam_task(ls);
            }
            return true;
        }

        if (hypot(x - ls->btn_sleep_x, y - ls->btn_sleep_y) <= ls->btn_r + 6.0) {
            app_launch_command("systemctl suspend || loginctl suspend");
            return true;
        }
        if (hypot(x - ls->btn_restart_x, y - ls->btn_restart_y) <= ls->btn_r + 6.0) {
            app_launch_command("systemctl reboot || reboot");
            return true;
        }
        if (hypot(x - ls->btn_power_x, y - ls->btn_power_y) <= ls->btn_r + 6.0) {
            app_launch_command("systemctl poweroff || poweroff");
            return true;
        }
    }

    return true; // Swallowed
}
