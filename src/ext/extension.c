#define _GNU_SOURCE
#include "extension.h"
#include "aurade_ext.h"
#include "../render/cairo_buffer.h"
#include "../util/apps.h"
#include "../shell/topbar.h"
#include "../shell/osd.h"
#include "../shell/notifications.h"
#include "../shell/wallpaper.h"
#include "../shell/launchpad.h"

#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>
#include <wlr/types/wlr_scene.h>
#include <scenefx/types/wlr_scene.h>

#define EXT_MAX_MODULES 64
#define EXT_MAX_PANELS  64
#define EXT_MAX_HOOKS   128

typedef struct {
    char name[128];
    char path[512];
    void *dl;
    void (*deinit)(void);
    aurade_ext_info_t info;
    bool active;
} ext_module_t;

struct aurade_ext_panel {
    aurade_ext_manager_t *mgr;
    int owner_idx;
    int x, y, w, h;
    bool visible;
    bool blur;
    cairo_buffer_t *buffer;
    struct wlr_scene_buffer *scene_buffer;
    struct wlr_scene_blur *blur_node;
    aurade_ext_draw_fn draw_cb;
    aurade_ext_panel_click_fn click_cb;
    void *userdata;
    bool in_use;
};

typedef struct { int owner; void *fn; void *ud; } ext_hook_t;

struct aurade_ext_manager {
    aurade_server_t *server;
    bool safe_mode;
    char safe_reason[256];

    ext_module_t modules[EXT_MAX_MODULES];
    int module_count;

    struct aurade_ext_panel panels[EXT_MAX_PANELS];

    ext_hook_t tick_hooks[EXT_MAX_HOOKS];
    int tick_count;
    ext_hook_t key_hooks[EXT_MAX_HOOKS];
    int key_count;
    ext_hook_t click_hooks[EXT_MAX_HOOKS];
    int click_count;
    ext_hook_t topbar_hooks[EXT_MAX_HOOKS];
    int topbar_count;
    ext_hook_t win_open_hooks[EXT_MAX_HOOKS];
    int win_open_count;
    ext_hook_t win_close_hooks[EXT_MAX_HOOKS];
    int win_close_count;
    ext_hook_t win_focus_hooks[EXT_MAX_HOOKS];
    int win_focus_count;
};

static aurade_ext_manager_t *g_mgr = NULL;
static volatile sig_atomic_t g_ext_depth = 0;
static volatile int g_current_ext_idx = -1;
static char g_disabled_flag_path[512] = {0};

/* ---- Async-signal-safe crash guard -------------------------------- */

static void *ucontext_pc(const void *uap) {
#if defined(__x86_64__)
    const ucontext_t *uc = (const ucontext_t *)uap;
    return (void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    const ucontext_t *uc = (const ucontext_t *)uap;
    return (void *)(uintptr_t)uc->uc_mcontext.pc;
#else
    (void)uap;
    return NULL;
#endif
}

static void ext_crash_handler(int sig, siginfo_t *info, void *uap) {
    (void)info;
    int offender = g_current_ext_idx;
    bool by_ext = (g_ext_depth > 0 && offender >= 0);

    if (!by_ext && g_mgr) {
        void *pc = ucontext_pc(uap);
        if (pc) {
            Dl_info dli;
            if (dladdr(pc, &dli) && dli.dli_fname) {
                for (int i = 0; i < g_mgr->module_count; i++) {
                    if (strcmp(g_mgr->modules[i].path, dli.dli_fname) == 0) {
                        by_ext = true;
                        offender = i;
                        break;
                    }
                }
            }
        }
    }

    if (by_ext && g_disabled_flag_path[0]) {
        int fd = open(g_disabled_flag_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            const char *hdr = "crash_signal=";
            write(fd, hdr, strlen(hdr));
            char num[16];
            int n = 0, v = sig;
            if (v <= 0) { num[n++] = '0'; }
            else {
                char rev[16]; int r = 0;
                while (v > 0 && r < 15) { rev[r++] = (char)('0' + (v % 10)); v /= 10; }
                while (r > 0) num[n++] = rev[--r];
            }
            num[n++] = '\n';
            write(fd, num, (size_t)n);
            if (g_mgr && offender >= 0 && offender < g_mgr->module_count) {
                const char *lbl = "extension=";
                write(fd, lbl, strlen(lbl));
                write(fd, g_mgr->modules[offender].name, strlen(g_mgr->modules[offender].name));
                write(fd, "\n", 1);
            }
            close(fd);
        }
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

static void ext_install_crash_handlers(void) {
    const char *home = getenv("HOME");
    if (home && *home) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/.config/aurade", home);
        mkdir(dir, 0755);
        snprintf(g_disabled_flag_path, sizeof(g_disabled_flag_path),
                 "%s/.config/aurade/ext_disabled", home);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ext_crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
}

static inline int ext_enter(int idx) {
    int prev = g_current_ext_idx;
    g_current_ext_idx = idx;
    g_ext_depth++;
    return prev;
}

static inline void ext_leave(int prev) {
    g_ext_depth--;
    g_current_ext_idx = prev;
}

/* ---- Public API callbacks exposed to extensions -------------------- */

static void api_log(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[AuraDE-Ext] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static void api_get_screen_size(int *out_w, int *out_h) {
    if (!g_mgr || !g_mgr->server) return;
    if (out_w) *out_w = g_mgr->server->screen_width;
    if (out_h) *out_h = g_mgr->server->screen_height;
}

static bool api_is_dark_mode(void) {
    return g_mgr && g_mgr->server ? g_mgr->server->config.dark_mode : true;
}

static void ext_panel_render_internal(aurade_ext_panel_t *p) {
    if (!p || !p->in_use || !p->buffer || !p->buffer->cr || !p->draw_cb) return;
    cairo_buffer_clear(p->buffer);
    int prev = ext_enter(p->owner_idx);
    p->draw_cb(p->buffer->cr, p->w, p->h, p->userdata);
    ext_leave(prev);
    cairo_buffer_apply_to_scene(p->buffer, p->scene_buffer);
}

static aurade_ext_panel_t *api_panel_create(int x, int y, int w, int h, bool blur,
                                            aurade_ext_draw_fn draw_cb,
                                            aurade_ext_panel_click_fn click_cb,
                                            void *userdata) {
    if (!g_mgr || !g_mgr->server || w <= 0 || h <= 0) return NULL;
    for (int i = 0; i < EXT_MAX_PANELS; i++) {
        aurade_ext_panel_t *p = &g_mgr->panels[i];
        if (p->in_use) continue;
        memset(p, 0, sizeof(*p));
        p->in_use = true;
        p->mgr = g_mgr;
        p->owner_idx = g_current_ext_idx;
        p->x = x;
        p->y = y;
        p->w = w;
        p->h = h;
        p->visible = true;
        p->blur = blur;
        p->draw_cb = draw_cb;
        p->click_cb = click_cb;
        p->userdata = userdata;
        p->buffer = cairo_buffer_create(w, h);
        if (!p->buffer) { p->in_use = false; return NULL; }
        p->scene_buffer = wlr_scene_buffer_create(g_mgr->server->overlay_tree, &p->buffer->base);
        wlr_scene_node_set_position(&p->scene_buffer->node, x, y);
        if (blur) {
            p->blur_node = wlr_scene_blur_create(g_mgr->server->overlay_tree, w, h);
            if (p->blur_node) {
                wlr_scene_blur_set_corner_radius(p->blur_node, 14);
                wlr_scene_blur_set_strength(p->blur_node, 0.85f);
                wlr_scene_node_set_position(&p->blur_node->node, x, y);
                wlr_scene_node_place_below(&p->blur_node->node, &p->scene_buffer->node);
            }
        }
        ext_panel_render_internal(p);
        schedule_all_outputs(g_mgr->server);
        return p;
    }
    return NULL;
}

static void api_panel_set_pos(aurade_ext_panel_t *p, int x, int y) {
    if (!p || !p->in_use) return;
    p->x = x;
    p->y = y;
    if (p->scene_buffer) wlr_scene_node_set_position(&p->scene_buffer->node, x, y);
    if (p->blur_node) wlr_scene_node_set_position(&p->blur_node->node, x, y);
    if (g_mgr && g_mgr->server) schedule_all_outputs(g_mgr->server);
}

static void api_panel_set_visible(aurade_ext_panel_t *p, bool visible) {
    if (!p || !p->in_use) return;
    p->visible = visible;
    if (p->scene_buffer) wlr_scene_node_set_enabled(&p->scene_buffer->node, visible);
    if (p->blur_node) wlr_scene_node_set_enabled(&p->blur_node->node, visible);
    if (g_mgr && g_mgr->server) schedule_all_outputs(g_mgr->server);
}

static void api_panel_redraw(aurade_ext_panel_t *p) {
    if (!p || !p->in_use) return;
    ext_panel_render_internal(p);
    if (g_mgr && g_mgr->server) schedule_all_outputs(g_mgr->server);
}

static void api_panel_destroy(aurade_ext_panel_t *p) {
    if (!p || !p->in_use) return;
    if (p->blur_node) { wlr_scene_node_destroy(&p->blur_node->node); p->blur_node = NULL; }
    if (p->scene_buffer) { wlr_scene_node_destroy(&p->scene_buffer->node); p->scene_buffer = NULL; }
    if (p->buffer) { wlr_buffer_drop(&p->buffer->base); p->buffer = NULL; }
    p->in_use = false;
}

static void add_hook(ext_hook_t *arr, int *cnt, void *fn, void *ud) {
    if (!fn || *cnt >= EXT_MAX_HOOKS) return;
    arr[(*cnt)++] = (ext_hook_t){ .owner = g_current_ext_idx, .fn = fn, .ud = ud };
}

static void api_hook_tick(aurade_ext_tick_fn cb, void *ud) {
    if (g_mgr) add_hook(g_mgr->tick_hooks, &g_mgr->tick_count, (void *)cb, ud);
}
static void api_hook_key(aurade_ext_key_fn cb, void *ud) {
    if (g_mgr) add_hook(g_mgr->key_hooks, &g_mgr->key_count, (void *)cb, ud);
}
static void api_hook_click(aurade_ext_click_fn cb, void *ud) {
    if (g_mgr) add_hook(g_mgr->click_hooks, &g_mgr->click_count, (void *)cb, ud);
}
static void api_hook_topbar_draw(aurade_ext_topbar_draw_fn cb, void *ud) {
    if (g_mgr) add_hook(g_mgr->topbar_hooks, &g_mgr->topbar_count, (void *)cb, ud);
}
static void api_hook_window_open(aurade_ext_win_fn cb, void *ud) {
    if (g_mgr) add_hook(g_mgr->win_open_hooks, &g_mgr->win_open_count, (void *)cb, ud);
}
static void api_hook_window_close(aurade_ext_win_fn cb, void *ud) {
    if (g_mgr) add_hook(g_mgr->win_close_hooks, &g_mgr->win_close_count, (void *)cb, ud);
}
static void api_hook_window_focus(aurade_ext_win_fn cb, void *ud) {
    if (g_mgr) add_hook(g_mgr->win_focus_hooks, &g_mgr->win_focus_count, (void *)cb, ud);
}

static void api_window_move_resize(void *win_handle, int x, int y, int w, int h) {
    if (!g_mgr || !g_mgr->server || !win_handle) return;
    aurade_view_t *v;
    wl_list_for_each(v, &g_mgr->server->views, link) {
        if (v == win_handle) {
            v->x = x;
            v->y = y;
            if (w > 0) v->width = w;
            if (h > 0) v->height = h;
            if (v->scene_tree) {
                wlr_scene_node_set_position(&v->scene_tree->node, v->x, v->y);
            }
            if (v->xdg_toplevel && w > 0 && h > 0) {
                wlr_xdg_toplevel_set_size(v->xdg_toplevel, w, h);
            }
            return;
        }
    }
}

static void api_window_focus(void *win_handle) {
    if (!g_mgr || !g_mgr->server || !win_handle) return;
    aurade_view_t *v;
    wl_list_for_each(v, &g_mgr->server->views, link) {
        if (v == win_handle) {
            view_focus(v);
            return;
        }
    }
}

static void api_window_close(void *win_handle) {
    if (!g_mgr || !g_mgr->server || !win_handle) return;
    aurade_view_t *v;
    wl_list_for_each(v, &g_mgr->server->views, link) {
        if (v == win_handle) {
            view_close(v);
            return;
        }
    }
}

static void api_exec(const char *cmd) {
    if (cmd && *cmd) app_launch_command(cmd);
}

static void api_topbar_redraw(void) {
    if (g_mgr && g_mgr->server && g_mgr->server->topbar) {
        topbar_render(g_mgr->server->topbar);
    }
}

static void api_toggle_control_center(void) {
    if (!g_mgr || !g_mgr->server) return;
    server_handle_gesture_begin(g_mgr->server, ACTION_CONTROL_CENTER);
    server_handle_gesture_update(g_mgr->server, -16.0, 0, 1.0);
    server_handle_gesture_end(g_mgr->server, false, -500.0, 0);
}

static void api_toggle_notifications(void) {
    if (g_mgr && g_mgr->server && g_mgr->server->notifications) {
        notifications_nc_toggle(g_mgr->server->notifications);
    }
}

static void api_toggle_launchpad(void) {
    if (!g_mgr || !g_mgr->server || !g_mgr->server->launchpad) return;
    if (g_mgr->server->launchpad->active) {
        server_close_overlay(g_mgr->server);
    } else {
        server_handle_gesture_begin(g_mgr->server, ACTION_LAUNCHPAD);
        server_handle_gesture_update(g_mgr->server, 0, 0, 1.5);
        server_handle_gesture_end(g_mgr->server, false, 0, 0);
    }
}

static void api_lock_screen(void) {
    if (g_mgr && g_mgr->server) server_lock_screen(g_mgr->server);
}

static void api_set_wallpaper(const char *path) {
    if (!g_mgr || !g_mgr->server || !path || !*path) return;
    strncpy(g_mgr->server->config.wallpaper_path, path,
            sizeof(g_mgr->server->config.wallpaper_path) - 1);
    if (g_mgr->server->wallpaper) wallpaper_reload(g_mgr->server->wallpaper);
}

static void api_osd_volume(double level, bool muted) {
    if (g_mgr && g_mgr->server && g_mgr->server->osd) {
        osd_show_volume(g_mgr->server->osd, level, muted);
    }
}

static void api_osd_brightness(double level) {
    if (g_mgr && g_mgr->server && g_mgr->server->osd) {
        osd_show_brightness(g_mgr->server->osd, level);
    }
}

static const aurade_ext_api_t g_ext_api = {
    .api_version = AURADE_EXT_API_VERSION,
    .log = api_log,
    .get_screen_size = api_get_screen_size,
    .is_dark_mode = api_is_dark_mode,
    .panel_create = api_panel_create,
    .panel_set_pos = api_panel_set_pos,
    .panel_set_visible = api_panel_set_visible,
    .panel_redraw = api_panel_redraw,
    .panel_destroy = api_panel_destroy,
    .hook_tick = api_hook_tick,
    .hook_key = api_hook_key,
    .hook_click = api_hook_click,
    .hook_topbar_draw = api_hook_topbar_draw,
    .hook_window_open = api_hook_window_open,
    .hook_window_close = api_hook_window_close,
    .hook_window_focus = api_hook_window_focus,
    .window_move_resize = api_window_move_resize,
    .window_focus = api_window_focus,
    .window_close = api_window_close,
    .exec = api_exec,
    .topbar_redraw = api_topbar_redraw,
    .toggle_control_center = api_toggle_control_center,
    .toggle_notifications = api_toggle_notifications,
    .toggle_launchpad = api_toggle_launchpad,
    .lock_screen = api_lock_screen,
    .set_wallpaper = api_set_wallpaper,
    .osd_show_volume = api_osd_volume,
    .osd_show_brightness = api_osd_brightness,
};

/* ---- Module loading ------------------------------------------------ */

static void ext_load_dir(aurade_ext_manager_t *mgr, const char *dir_path) {
    if (!dir_path || !*dir_path) return;
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && mgr->module_count < EXT_MAX_MODULES) {
        size_t len = strlen(ent->d_name);
        if (len < 4 || strcmp(ent->d_name + len - 3, ".so") != 0) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);

        void *dl = dlopen(full, RTLD_NOW | RTLD_LOCAL);
        if (!dl) {
            fprintf(stderr, "[AuraDE-Ext] Failed to load %s: %s\n", full, dlerror());
            continue;
        }

        typedef bool (*init_fn_t)(const aurade_ext_api_t *);
        typedef const aurade_ext_info_t *(*meta_fn_t)(void);
        typedef void (*deinit_fn_t)(void);

        init_fn_t init_fn = (init_fn_t)dlsym(dl, "aurade_ext_init");
        if (!init_fn) {
            fprintf(stderr, "[AuraDE-Ext] %s missing aurade_ext_init\n", full);
            dlclose(dl);
            continue;
        }

        int idx = mgr->module_count;
        ext_module_t *mod = &mgr->modules[idx];
        memset(mod, 0, sizeof(*mod));
        snprintf(mod->name, sizeof(mod->name), "%s", ent->d_name);
        snprintf(mod->path, sizeof(mod->path), "%s", full);
        mod->dl = dl;
        mod->deinit = (deinit_fn_t)dlsym(dl, "aurade_ext_deinit");

        meta_fn_t meta_fn = (meta_fn_t)dlsym(dl, "aurade_ext_meta");
        if (meta_fn) {
            int prev = ext_enter(idx);
            const aurade_ext_info_t *info = meta_fn();
            if (info) mod->info = *info;
            ext_leave(prev);
        }

        mgr->module_count++;
        int prev = ext_enter(idx);
        bool ok = init_fn(&g_ext_api);
        ext_leave(prev);

        if (!ok) {
            fprintf(stderr, "[AuraDE-Ext] %s init returned false, unloading\n", full);
            dlclose(dl);
            mgr->module_count--;
            continue;
        }
        mod->active = true;
        fprintf(stderr, "[AuraDE-Ext] Loaded %s (%s)\n",
                mod->info.name ? mod->info.name : mod->name,
                mod->info.version ? mod->info.version : "1.0");
    }
    closedir(d);
}

aurade_ext_manager_t *ext_manager_create(aurade_server_t *server) {
    aurade_ext_manager_t *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    mgr->server = server;
    g_mgr = mgr;

    ext_install_crash_handlers();

    // Check Safe Mode flag created by crash handler
    if (g_disabled_flag_path[0] && access(g_disabled_flag_path, F_OK) == 0) {
        mgr->safe_mode = true;
        FILE *fp = fopen(g_disabled_flag_path, "r");
        if (fp) {
            size_t r = fread(mgr->safe_reason, 1, sizeof(mgr->safe_reason) - 1, fp);
            mgr->safe_reason[r] = '\0';
            fclose(fp);
        }
        fprintf(stderr, "[AuraDE-Ext] SAFE MODE ACTIVE: All extensions disabled due to previous extension crash.\n");
        fprintf(stderr, "[AuraDE-Ext] Remove %s (or re-enable in Settings -> Extensions) to restore.\n",
                g_disabled_flag_path);
        return mgr;
    }

    const char *env_dir = getenv("AURADE_EXT_DIR");
    if (env_dir && *env_dir) ext_load_dir(mgr, env_dir);

    const char *home = getenv("HOME");
    if (home && *home) {
        char user_dir[512];
        snprintf(user_dir, sizeof(user_dir), "%s/.local/share/aurade/extensions", home);
        ext_load_dir(mgr, user_dir);
    }
    ext_load_dir(mgr, "/usr/local/share/aurade/extensions");

    return mgr;
}

void ext_manager_destroy(aurade_ext_manager_t *mgr) {
    if (!mgr) return;
    for (int i = 0; i < mgr->module_count; i++) {
        ext_module_t *m = &mgr->modules[i];
        if (m->active && m->deinit) {
            int prev = ext_enter(i);
            m->deinit();
            ext_leave(prev);
        }
    }
    for (int i = 0; i < EXT_MAX_PANELS; i++) {
        if (mgr->panels[i].in_use) api_panel_destroy(&mgr->panels[i]);
    }
    for (int i = 0; i < mgr->module_count; i++) {
        if (mgr->modules[i].dl) dlclose(mgr->modules[i].dl);
    }
    if (g_mgr == mgr) g_mgr = NULL;
    free(mgr);
}

bool ext_manager_is_safe_mode(const aurade_ext_manager_t *mgr) {
    return mgr && mgr->safe_mode;
}

int ext_manager_loaded_count(const aurade_ext_manager_t *mgr) {
    return mgr ? mgr->module_count : 0;
}

void ext_manager_tick(aurade_ext_manager_t *mgr, double dt_sec) {
    if (!mgr || mgr->safe_mode) return;
    for (int i = 0; i < mgr->tick_count; i++) {
        aurade_ext_tick_fn fn = (aurade_ext_tick_fn)mgr->tick_hooks[i].fn;
        int prev = ext_enter(mgr->tick_hooks[i].owner);
        fn(dt_sec, mgr->tick_hooks[i].ud);
        ext_leave(prev);
    }
}

bool ext_manager_handle_key(aurade_ext_manager_t *mgr, uint32_t sym, uint32_t mods, bool pressed) {
    if (!mgr || mgr->safe_mode) return false;
    for (int i = 0; i < mgr->key_count; i++) {
        aurade_ext_key_fn fn = (aurade_ext_key_fn)mgr->key_hooks[i].fn;
        int prev = ext_enter(mgr->key_hooks[i].owner);
        bool consumed = fn(sym, mods, pressed, mgr->key_hooks[i].ud);
        ext_leave(prev);
        if (consumed) return true;
    }
    return false;
}

bool ext_manager_handle_button(aurade_ext_manager_t *mgr, double lx, double ly, uint32_t button, bool pressed) {
    if (!mgr || mgr->safe_mode) return false;

    for (int i = EXT_MAX_PANELS - 1; i >= 0; i--) {
        aurade_ext_panel_t *p = &mgr->panels[i];
        if (!p->in_use || !p->visible || !p->click_cb) continue;
        if (lx >= p->x && lx <= p->x + p->w && ly >= p->y && ly <= p->y + p->h) {
            int prev = ext_enter(p->owner_idx);
            bool handled = p->click_cb(lx - p->x, ly - p->y, pressed, p->userdata);
            ext_leave(prev);
            if (handled) return true;
        }
    }

    for (int i = 0; i < mgr->click_count; i++) {
        aurade_ext_click_fn fn = (aurade_ext_click_fn)mgr->click_hooks[i].fn;
        int prev = ext_enter(mgr->click_hooks[i].owner);
        bool consumed = fn(lx, ly, button, pressed, mgr->click_hooks[i].ud);
        ext_leave(prev);
        if (consumed) return true;
    }
    return false;
}

void ext_manager_render_topbar(aurade_ext_manager_t *mgr, cairo_t *cr, int bar_w, int bar_h) {
    if (!mgr || mgr->safe_mode || !cr) return;
    for (int i = 0; i < mgr->topbar_count; i++) {
        aurade_ext_topbar_draw_fn fn = (aurade_ext_topbar_draw_fn)mgr->topbar_hooks[i].fn;
        int prev = ext_enter(mgr->topbar_hooks[i].owner);
        cairo_save(cr);
        fn(cr, bar_w, bar_h, mgr->topbar_hooks[i].ud);
        cairo_restore(cr);
        ext_leave(prev);
    }
}

static void fill_ext_win(aurade_view_t *v, aurade_ext_win_t *out) {
    memset(out, 0, sizeof(*out));
    out->handle = v;
    out->x = v->x;
    out->y = v->y;
    out->width = v->width;
    out->height = v->height;
    out->workspace = v->workspace;
    out->focused = (v->server && v->server->focused_view == v);
    if (v->xdg_toplevel) {
        out->app_id = v->xdg_toplevel->app_id;
        out->title = v->xdg_toplevel->title;
    } else if (v->xwayland_surface) {
        out->app_id = v->xwayland_surface->class;
        out->title = v->xwayland_surface->title;
    }
}

static void dispatch_win_hooks(ext_hook_t *arr, int cnt, aurade_view_t *v) {
    if (!v || cnt <= 0) return;
    aurade_ext_win_t info;
    fill_ext_win(v, &info);
    for (int i = 0; i < cnt; i++) {
        aurade_ext_win_fn fn = (aurade_ext_win_fn)arr[i].fn;
        int prev = ext_enter(arr[i].owner);
        fn(&info, arr[i].ud);
        ext_leave(prev);
    }
}

void ext_manager_notify_window_open(aurade_ext_manager_t *mgr, aurade_view_t *view) {
    if (mgr && !mgr->safe_mode) dispatch_win_hooks(mgr->win_open_hooks, mgr->win_open_count, view);
}

void ext_manager_notify_window_close(aurade_ext_manager_t *mgr, aurade_view_t *view) {
    if (mgr && !mgr->safe_mode) dispatch_win_hooks(mgr->win_close_hooks, mgr->win_close_count, view);
}

void ext_manager_notify_window_focus(aurade_ext_manager_t *mgr, aurade_view_t *view) {
    if (mgr && !mgr->safe_mode) dispatch_win_hooks(mgr->win_focus_hooks, mgr->win_focus_count, view);
}
