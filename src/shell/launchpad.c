#include "launchpad.h"
#include "dock.h"
#include "../shell/wallpaper.h"
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>
#include <json-c/json.h>
#include <sys/stat.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LP_COLS 7
#define LP_ROWS 4
#define LP_PAGE_SIZE (LP_COLS * LP_ROWS)
#define LP_FONT "Inter"

typedef struct {
    double grid_x, grid_y, grid_w, grid_h;
    double cell_w, cell_h;
    double icon_size;
    double search_x, search_y, search_w, search_h;
} lp_layout_t;

typedef struct {
    double win_x, win_y, win_w, win_h;
    double title_x, title_y;
    double close_x, close_y, close_r;
    double grid_x, grid_y, grid_w, grid_h;
    double cell_w, cell_h;
    double icon_size;
    int cols, rows;
} lp_folder_layout_t;

static void launchpad_render_drag_preview(aurade_launchpad_t *lp, bool over_dock);
static void filter_items(aurade_launchpad_t *lp);

static uint64_t lp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static const char *lp_get_config_path(void) {
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/home/user";
    snprintf(path, sizeof(path), "%s/.config/aurade/launchpad.json", home);
    return path;
}

static desktop_app_t *find_app_exact_or_match(desktop_apps_t *list, const char *name, const char *exec) {
    if (!list) return NULL;
    // 1. Exact match on name
    if (name && *name) {
        for (int i = 0; i < list->count; i++) {
            if (strcasecmp(list->apps[i].name, name) == 0) return &list->apps[i];
        }
    }
    // 2. Exact match on exec
    if (exec && *exec) {
        for (int i = 0; i < list->count; i++) {
            if (strcmp(list->apps[i].exec, exec) == 0) return &list->apps[i];
        }
    }
    // 3. Fallback to apps_find
    if (name && *name) {
        desktop_app_t *a = apps_find(list, name);
        if (a) return a;
    }
    if (exec && *exec) {
        return apps_find(list, exec);
    }
    return NULL;
}

void launchpad_load_layout(aurade_launchpad_t *lp) {
    if (!lp || !lp->server || !lp->server->apps) return;

    // Free any existing folders
    for (int i = 0; i < lp->item_count; i++) {
        if (lp->items[i].type == LP_ITEM_FOLDER && lp->items[i].folder) {
            free(lp->items[i].folder);
            lp->items[i].folder = NULL;
        }
    }
    lp->item_count = 0;

    const char *path = lp_get_config_path();
    struct json_object *root = json_object_from_file(path);

    if (root) {
        struct json_object *items_arr = NULL;
        if (json_object_object_get_ex(root, "items", &items_arr) &&
            json_type_array == json_object_get_type(items_arr)) {
            int n = json_object_array_length(items_arr);
            for (int i = 0; i < n && lp->item_count < LAUNCHPAD_MAX_ITEMS; i++) {
                struct json_object *entry = json_object_array_get_idx(items_arr, i);
                if (!entry) continue;

                struct json_object *type_val = NULL;
                json_object_object_get_ex(entry, "type", &type_val);
                const char *type_str = type_val ? json_object_get_string(type_val) : "app";

                if (strcmp(type_str, "folder") == 0) {
                    struct json_object *name_val = NULL, *apps_arr = NULL;
                    json_object_object_get_ex(entry, "name", &name_val);
                    json_object_object_get_ex(entry, "apps", &apps_arr);

                    lp_folder_t *f = calloc(1, sizeof(lp_folder_t));
                    const char *fname = name_val ? json_object_get_string(name_val) : "Folder";
                    snprintf(f->name, sizeof(f->name), "%s", fname);

                    if (apps_arr && json_type_array == json_object_get_type(apps_arr)) {
                        int an = json_object_array_length(apps_arr);
                        for (int j = 0; j < an && f->app_count < LAUNCHPAD_MAX_FOLDER_ITEMS; j++) {
                            struct json_object *app_entry = json_object_array_get_idx(apps_arr, j);
                            struct json_object *aname_val = NULL, *aexec_val = NULL;
                            json_object_object_get_ex(app_entry, "name", &aname_val);
                            json_object_object_get_ex(app_entry, "exec", &aexec_val);
                            const char *aname = aname_val ? json_object_get_string(aname_val) : "";
                            const char *aexec = aexec_val ? json_object_get_string(aexec_val) : "";

                            desktop_app_t *dapp = find_app_exact_or_match(lp->server->apps, aname, aexec);
                            if (dapp) {
                                f->apps[f->app_count++] = dapp;
                            }
                        }
                    }

                    if (f->app_count > 1) {
                        lp->items[lp->item_count].type = LP_ITEM_FOLDER;
                        lp->items[lp->item_count].folder = f;
                        lp->items[lp->item_count].app = NULL;
                        lp->item_count++;
                    } else if (f->app_count == 1) {
                        // Dissolve single-item folder into regular app
                        lp->items[lp->item_count].type = LP_ITEM_APP;
                        lp->items[lp->item_count].app = f->apps[0];
                        lp->items[lp->item_count].folder = NULL;
                        lp->item_count++;
                        free(f);
                    } else {
                        free(f);
                    }
                } else {
                    // Normal app
                    struct json_object *name_val = NULL, *exec_val = NULL;
                    json_object_object_get_ex(entry, "name", &name_val);
                    json_object_object_get_ex(entry, "exec", &exec_val);
                    const char *aname = name_val ? json_object_get_string(name_val) : "";
                    const char *aexec = exec_val ? json_object_get_string(exec_val) : "";

                    desktop_app_t *dapp = find_app_exact_or_match(lp->server->apps, aname, aexec);
                    if (dapp) {
                        lp->items[lp->item_count].type = LP_ITEM_APP;
                        lp->items[lp->item_count].app = dapp;
                        lp->items[lp->item_count].folder = NULL;
                        lp->item_count++;
                    }
                }
            }
        }
        json_object_put(root);
    }

    // Now append any system apps that aren't yet in lp->items or any folder
    for (int i = 0; i < lp->server->apps->count && lp->item_count < LAUNCHPAD_MAX_ITEMS; i++) {
        desktop_app_t *a = &lp->server->apps->apps[i];
        bool present = false;
        for (int k = 0; k < lp->item_count; k++) {
            if (lp->items[k].type == LP_ITEM_APP && lp->items[k].app == a) {
                present = true;
                break;
            } else if (lp->items[k].type == LP_ITEM_FOLDER && lp->items[k].folder) {
                for (int m = 0; m < lp->items[k].folder->app_count; m++) {
                    if (lp->items[k].folder->apps[m] == a) {
                        present = true;
                        break;
                    }
                }
                if (present) break;
            }
        }
        if (!present) {
            lp->items[lp->item_count].type = LP_ITEM_APP;
            lp->items[lp->item_count].app = a;
            lp->items[lp->item_count].folder = NULL;
            lp->item_count++;
        }
    }
}

static void ensure_parent_dirs(const char *file_path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", file_path);
    char *p = dir;
    if (*p == '/') p++;
    while ((p = strchr(p, '/')) != NULL) {
        *p = '\0';
        mkdir(dir, 0755);
        *p = '/';
        p++;
    }
}

void launchpad_save_layout(aurade_launchpad_t *lp) {
    if (!lp) return;
    const char *path = lp_get_config_path();
    ensure_parent_dirs(path);

    struct json_object *root = json_object_new_object();
    struct json_object *items_arr = json_object_new_array();

    for (int i = 0; i < lp->item_count; i++) {
        struct json_object *entry = json_object_new_object();
        if (lp->items[i].type == LP_ITEM_FOLDER && lp->items[i].folder) {
            lp_folder_t *f = lp->items[i].folder;
            json_object_object_add(entry, "type", json_object_new_string("folder"));
            json_object_object_add(entry, "name", json_object_new_string(f->name));

            struct json_object *apps_arr = json_object_new_array();
            for (int j = 0; j < f->app_count; j++) {
                if (!f->apps[j]) continue;
                struct json_object *a_entry = json_object_new_object();
                json_object_object_add(a_entry, "name", json_object_new_string(f->apps[j]->name));
                json_object_object_add(a_entry, "exec", json_object_new_string(f->apps[j]->exec));
                json_object_array_add(apps_arr, a_entry);
            }
            json_object_object_add(entry, "apps", apps_arr);
        } else if (lp->items[i].type == LP_ITEM_APP && lp->items[i].app) {
            desktop_app_t *a = lp->items[i].app;
            json_object_object_add(entry, "type", json_object_new_string("app"));
            json_object_object_add(entry, "name", json_object_new_string(a->name));
            json_object_object_add(entry, "exec", json_object_new_string(a->exec));
        } else {
            json_object_put(entry);
            continue;
        }
        json_object_array_add(items_arr, entry);
    }
    json_object_object_add(root, "items", items_arr);

    char tmp_path[540];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (json_object_to_file_ext(tmp_path, root, JSON_C_TO_STRING_PRETTY) == 0) {
        rename(tmp_path, path);
    }
    json_object_put(root);
}

static double launchpad_dots_y(aurade_launchpad_t *lp) {
    if (lp && lp->server && lp->server->dock && lp->server->dock->pill_y > 0) {
        return lp->server->dock->pill_y - 24.0;
    }
    return (lp ? lp->screen_height : 1080) - 108.0;
}

static void lp_layout(aurade_launchpad_t *lp, lp_layout_t *l) {
    double sw = lp->screen_width;
    double sh = lp->screen_height;

    l->search_w = 260.0;
    l->search_h = 32.0;
    l->search_x = (sw - l->search_w) / 2.0;
    l->search_y = 58.0;

    double margin_x = sw * 0.09;
    if (margin_x < 80.0) margin_x = 80.0;
    l->grid_x = margin_x;
    l->grid_w = sw - margin_x * 2.0;
    l->grid_y = 132.0;
    double dots_y = launchpad_dots_y(lp);
    l->grid_h = dots_y - l->grid_y - 28.0;
    if (l->grid_h < 200.0) l->grid_h = sh - l->grid_y - 128.0;
    l->cell_w = l->grid_w / LP_COLS;
    l->cell_h = l->grid_h / LP_ROWS;
    l->icon_size = fmin(64.0, fmin(l->cell_w * 0.62, l->cell_h * 0.52));
    if (l->icon_size < 36.0) l->icon_size = 36.0;
}

static void lp_folder_layout(aurade_launchpad_t *lp, lp_folder_layout_t *fl) {
    double sw = lp->screen_width;
    double sh = lp->screen_height;

    fl->cols = 4;
    fl->rows = 3;

    fl->win_w = fmin(680.0, sw * 0.70);
    fl->win_h = fmin(460.0, sh * 0.65);
    fl->win_x = (sw - fl->win_w) / 2.0;
    fl->win_y = (sh - fl->win_h) / 2.0;

    fl->close_x = fl->win_x + fl->win_w - 32.0;
    fl->close_y = fl->win_y + 32.0;
    fl->close_r = 14.0;

    fl->title_x = fl->win_x + fl->win_w / 2.0;
    fl->title_y = fl->win_y + 40.0;

    fl->grid_x = fl->win_x + 36.0;
    fl->grid_y = fl->win_y + 75.0;
    fl->grid_w = fl->win_w - 72.0;
    fl->grid_h = fl->win_h - 95.0;

    fl->cell_w = fl->grid_w / fl->cols;
    fl->cell_h = fl->grid_h / fl->rows;
    fl->icon_size = fmin(62.0, fmin(fl->cell_w * 0.60, fl->cell_h * 0.52));
    if (fl->icon_size < 36.0) fl->icon_size = 36.0;
}

static void filter_items(aurade_launchpad_t *lp) {
    lp->filtered_count = 0;
    if (!lp->server->apps) return;

    if (lp->search_query[0] == '\0') {
        for (int i = 0; i < lp->item_count && lp->filtered_count < LAUNCHPAD_MAX_FILTERED; i++) {
            lp->filtered_items[lp->filtered_count++] = lp->items[i];
        }
    } else {
        for (int i = 0; i < lp->item_count && lp->filtered_count < LAUNCHPAD_MAX_FILTERED; i++) {
            if (lp->items[i].type == LP_ITEM_APP) {
                desktop_app_t *app = lp->items[i].app;
                if (!app) continue;
                if (strcasestr(app->name, lp->search_query) ||
                    strcasestr(app->exec, lp->search_query) ||
                    strcasestr(app->generic_name, lp->search_query)) {
                    lp->filtered_items[lp->filtered_count++] = lp->items[i];
                }
            } else if (lp->items[i].type == LP_ITEM_FOLDER && lp->items[i].folder) {
                lp_folder_t *f = lp->items[i].folder;
                bool folder_name_match = (strcasestr(f->name, lp->search_query) != NULL);
                if (folder_name_match) {
                    lp->filtered_items[lp->filtered_count++] = lp->items[i];
                }
                // Also search individual apps inside the folder so user can launch directly!
                for (int j = 0; j < f->app_count && lp->filtered_count < LAUNCHPAD_MAX_FILTERED; j++) {
                    desktop_app_t *app = f->apps[j];
                    if (!app) continue;
                    if (strcasestr(app->name, lp->search_query) ||
                        strcasestr(app->exec, lp->search_query) ||
                        strcasestr(app->generic_name, lp->search_query)) {
                        lp_item_t item = {
                            .type = LP_ITEM_APP,
                            .app = app,
                            .folder = NULL,
                        };
                        lp->filtered_items[lp->filtered_count++] = item;
                    }
                }
            }
        }
    }

    lp->total_pages = (lp->filtered_count + LP_PAGE_SIZE - 1) / LP_PAGE_SIZE;
    if (lp->total_pages < 1) lp->total_pages = 1;
    if (lp->current_page >= lp->total_pages) {
        lp->current_page = 0;
        lp->target_page = 0;
        lp->page_scroll = 0.0;
        lp->page_animating = false;
    }
}

aurade_launchpad_t *launchpad_create(aurade_server_t *server) {
    aurade_launchpad_t *lp = calloc(1, sizeof(*lp));
    lp->server = server;
    lp->screen_width = 1920;
    lp->screen_height = 1080;
    lp->progress = 0.0;
    lp->active = false;
    lp->hovered_index = -1;
    lp->hovered_folder_app_idx = -1;
    lp->open_folder = NULL;
    lp->folder_renaming = false;
    lp->drop_action = DROP_TARGET_NONE;
    lp->drop_target_idx = -1;

    lp->buffer = cairo_buffer_create(lp->screen_width, lp->screen_height);
    lp->scene_buffer = wlr_scene_buffer_create(server->overlay_tree, &lp->buffer->base);
    wlr_scene_node_set_enabled(&lp->scene_buffer->node, false);

    lp->drag_buffer = cairo_buffer_create(220, 180);
    lp->drag_scene_buffer = wlr_scene_buffer_create(server->dock_tree ? server->dock_tree : server->overlay_tree,
                                                    &lp->drag_buffer->base);
    wlr_scene_node_set_enabled(&lp->drag_scene_buffer->node, false);

    launchpad_load_layout(lp);
    filter_items(lp);
    return lp;
}

void launchpad_resize(aurade_launchpad_t *lp, int width, int height) {
    if (!lp) return;
    lp->screen_width = width;
    lp->screen_height = height;
    cairo_buffer_resize(lp->buffer, width, height);
}

void launchpad_begin(aurade_launchpad_t *lp) {
    if (!lp) return;
    lp->active = true;
    lp->search_query[0] = '\0';
    lp->current_page = 0;
    lp->target_page = 0;
    lp->page_scroll = 0.0;
    lp->page_animating = false;
    lp->hovered_index = -1;
    lp->hovered_folder_app_idx = -1;
    lp->open_folder = NULL;
    lp->folder_renaming = false;
    lp->is_dragging = false;
    lp->drag_candidate = NULL;
    lp->drag_folder_candidate = NULL;
    lp->dragged_app = NULL;
    lp->dragged_folder = NULL;
    lp->drag_over_dock = false;
    lp->drop_action = DROP_TARGET_NONE;
    lp->drop_target_idx = -1;
    lp->drag_edge_hover_time_ms = 0;

    if (lp->drag_scene_buffer) {
        wlr_scene_node_set_enabled(&lp->drag_scene_buffer->node, false);
    }
    lp->scroll_accum_x = 0;
    lp->scroll_accum_y = 0;

    if (lp->realtime_blur) {
        cairo_surface_destroy(lp->realtime_blur);
        lp->realtime_blur = NULL;
    }
    if (lp->server && lp->server->config.blur_launchpad) {
        int bw = lp->screen_width / 16;
        int bh = lp->screen_height / 16;
        if (bw < 32) bw = 32;
        if (bh < 32) bh = 32;
        lp->realtime_blur = wallpaper_capture_workspace_blur(lp->server, bw, bh);
    }

    filter_items(lp);
    wlr_scene_node_set_enabled(&lp->scene_buffer->node, true);
    if (lp->server && lp->server->dock) {
        dock_update_visibility(lp->server->dock);
    }
}

void launchpad_update_progress(aurade_launchpad_t *lp, double progress) {
    if (!lp) return;
    if (!lp->active && progress > 0.001) return;
    lp->progress = progress;

    if (progress <= 0.001) {
        wlr_scene_node_set_enabled(&lp->scene_buffer->node, false);
        if (lp->drag_scene_buffer) {
            wlr_scene_node_set_enabled(&lp->drag_scene_buffer->node, false);
        }
        if (lp->realtime_blur) {
            cairo_surface_destroy(lp->realtime_blur);
            lp->realtime_blur = NULL;
        }
        lp->needs_render = false;
        return;
    }

    wlr_scene_node_set_enabled(&lp->scene_buffer->node, true);
    lp->needs_render = true;
}

void launchpad_end(aurade_launchpad_t *lp, bool opened) {
    if (!lp) return;
    lp->active = opened;
    if (!opened) {
        if (lp->drag_scene_buffer) {
            wlr_scene_node_set_enabled(&lp->drag_scene_buffer->node, false);
        }
        if (lp->realtime_blur) {
            cairo_surface_destroy(lp->realtime_blur);
            lp->realtime_blur = NULL;
        }
        lp->is_dragging = false;
        lp->drag_candidate = NULL;
        lp->drag_folder_candidate = NULL;
        lp->dragged_app = NULL;
        lp->dragged_folder = NULL;
        lp->drag_over_dock = false;
        lp->open_folder = NULL;
        lp->folder_renaming = false;
        lp->page_animating = false;
        launchpad_update_progress(lp, 0.0);
    }
    if (lp->server && lp->server->dock) {
        dock_update_visibility(lp->server->dock);
    }
}

void launchpad_switch_page(aurade_launchpad_t *lp, int new_page, bool animate) {
    if (!lp || lp->total_pages <= 1) return;
    if (new_page < 0) new_page = 0;
    if (new_page >= lp->total_pages) new_page = lp->total_pages - 1;
    if (new_page == lp->current_page && !animate && !lp->page_animating) return;

    lp->target_page = new_page;
    lp->current_page = new_page;
    lp->hovered_index = -1;

    if (!animate) {
        lp->page_animating = false;
        lp->page_scroll = (double)new_page;
        launchpad_render(lp);
        return;
    }

    spring_config_t spring = {
        .stiffness = 380.0,
        .damping = 1.05,
        .mass = 1.0,
    };
    if (lp->server && lp->server->config.spring.stiffness > 10.0) {
        spring = lp->server->config.spring;
    }

    spring_simulation_start(&lp->page_spring, &spring, lp->page_scroll, (double)new_page, 0.0);
    lp->page_animating = true;
    lp->needs_render = true;
    if (lp->server) schedule_all_outputs(lp->server);
}

void launchpad_tick(aurade_launchpad_t *lp, double dt) {
    if (!lp || !lp->page_animating) return;

    bool active = spring_simulation_step(&lp->page_spring, dt);
    lp->page_scroll = lp->page_spring.current_pos;
    lp->needs_render = true;

    if (!active) {
        lp->page_animating = false;
        lp->page_scroll = (double)lp->current_page;
    }
}

void launchpad_handle_axis(aurade_launchpad_t *lp, enum wl_pointer_axis orientation,
                           double delta, int32_t delta_discrete) {
    if (!lp || !lp->active || lp->total_pages <= 1 || lp->open_folder != NULL) return;
    if (lp->page_animating) return;

    uint64_t now = lp_now_ms();
    if (now - lp->last_scroll_time_ms > 280) {
        lp->scroll_accum_x = 0;
        lp->scroll_accum_y = 0;
    }
    lp->last_scroll_time_ms = now;

    if (delta_discrete != 0) {
        if (delta_discrete > 0) {
            if (lp->current_page < lp->total_pages - 1) {
                launchpad_switch_page(lp, lp->current_page + 1, true);
            }
        } else {
            if (lp->current_page > 0) {
                launchpad_switch_page(lp, lp->current_page - 1, true);
            }
        }
        return;
    }

    if (orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        lp->scroll_accum_x += delta;
        if (lp->scroll_accum_x > 32.0) {
            lp->scroll_accum_x = 0;
            if (lp->current_page < lp->total_pages - 1) {
                launchpad_switch_page(lp, lp->current_page + 1, true);
            }
        } else if (lp->scroll_accum_x < -32.0) {
            lp->scroll_accum_x = 0;
            if (lp->current_page > 0) {
                launchpad_switch_page(lp, lp->current_page - 1, true);
            }
        }
    } else {
        lp->scroll_accum_y += delta;
        if (lp->scroll_accum_y > 32.0) {
            lp->scroll_accum_y = 0;
            if (lp->current_page < lp->total_pages - 1) {
                launchpad_switch_page(lp, lp->current_page + 1, true);
            }
        } else if (lp->scroll_accum_y < -32.0) {
            lp->scroll_accum_y = 0;
            if (lp->current_page > 0) {
                launchpad_switch_page(lp, lp->current_page - 1, true);
            }
        }
    }
}

bool launchpad_handle_press(aurade_launchpad_t *lp, double x, double y) {
    if (!lp || !lp->active || lp->progress < 0.4) return false;

    // IF A FOLDER IS OPEN:
    if (lp->open_folder != NULL) {
        lp_folder_layout_t fl;
        lp_folder_layout(lp, &fl);

        // Click on close button
        double cdist = hypot(x - fl.close_x, y - fl.close_y);
        if (cdist <= fl.close_r + 8.0) {
            lp->open_folder = NULL;
            lp->folder_renaming = false;
            launchpad_render(lp);
            return true;
        }

        bool inside_window = (x >= fl.win_x && x <= fl.win_x + fl.win_w &&
                              y >= fl.win_y && y <= fl.win_y + fl.win_h);

        if (!inside_window) {
            // Click outside folder window closes the folder view
            lp->open_folder = NULL;
            lp->folder_renaming = false;
            launchpad_render(lp);
            return true;
        }

        // Click on folder title: toggle rename
        if (fabs(x - fl.title_x) < 140.0 && fabs(y - fl.title_y) < 24.0) {
            lp->folder_renaming = !lp->folder_renaming;
            if (lp->folder_renaming) {
                snprintf(lp->folder_rename_buf, sizeof(lp->folder_rename_buf), "%s", lp->open_folder->name);
            } else {
                if (lp->folder_rename_buf[0]) {
                    snprintf(lp->open_folder->name, sizeof(lp->open_folder->name), "%s", lp->folder_rename_buf);
                    launchpad_save_layout(lp);
                }
            }
            launchpad_render(lp);
            return true;
        } else if (lp->folder_renaming) {
            if (lp->folder_rename_buf[0]) {
                snprintf(lp->open_folder->name, sizeof(lp->open_folder->name), "%s", lp->folder_rename_buf);
                launchpad_save_layout(lp);
            }
            lp->folder_renaming = false;
            launchpad_render(lp);
        }

        // Click on an app inside the folder
        for (int i = 0; i < lp->open_folder->app_count; i++) {
            int r = i / fl.cols;
            int c = i % fl.cols;
            double cx = fl.grid_x + c * fl.cell_w + fl.cell_w / 2.0;
            double cy = fl.grid_y + r * fl.cell_h + fl.cell_h / 2.0;

            if (fabs(x - cx) < fl.cell_w * 0.46 && fabs(y - cy) < fl.cell_h * 0.46) {
                lp->drag_candidate = lp->open_folder->apps[i];
                lp->drag_folder_candidate = NULL;
                lp->drag_source_idx = i;
                lp->drag_from_folder = true;
                lp->drag_start_x = x;
                lp->drag_start_y = y;
                lp->drag_x = x;
                lp->drag_y = y;
                lp->is_dragging = false;
                return true;
            }
        }

        return true;
    }

    // MAIN LAUNCHPAD GRID:
    lp_layout_t l;
    lp_layout(lp, &l);

    // Search bar
    if (x >= l.search_x && x <= l.search_x + l.search_w &&
        y >= l.search_y && y <= l.search_y + l.search_h) {
        return true;
    }

    // Pagination dots / chevrons
    double dots_y = launchpad_dots_y(lp);
    if (fabs(y - dots_y) < 22.0 && lp->total_pages > 1) {
        double dots_start = (lp->screen_width - (lp->total_pages * 22.0)) / 2.0;
        for (int p = 0; p < lp->total_pages; p++) {
            double dot_x = dots_start + p * 22.0 + 11.0;
            if (fabs(x - dot_x) < 13.0) {
                launchpad_switch_page(lp, p, true);
                return true;
            }
        }
        double left_x = dots_start - 34.0;
        double right_x = dots_start + lp->total_pages * 22.0 + 34.0;
        if (fabs(x - left_x) < 22.0) {
            launchpad_switch_page(lp, lp->current_page - 1, true);
            return true;
        }
        if (fabs(x - right_x) < 22.0) {
            launchpad_switch_page(lp, lp->current_page + 1, true);
            return true;
        }
    }

    // Items on current page
    int page_start = lp->current_page * LP_PAGE_SIZE;
    int page_end = page_start + LP_PAGE_SIZE;
    if (page_end > lp->filtered_count) page_end = lp->filtered_count;

    for (int i = page_start; i < page_end; i++) {
        int idx = i - page_start;
        int r = idx / LP_COLS;
        int c = idx % LP_COLS;
        double cx = l.grid_x + c * l.cell_w + l.cell_w / 2.0;
        double cy = l.grid_y + r * l.cell_h + l.cell_h / 2.0;

        if (fabs(x - cx) < l.cell_w * 0.46 && fabs(y - cy) < l.cell_h * 0.46) {
            lp->drag_source_idx = i;
            lp->drag_from_folder = false;
            lp->drag_start_x = x;
            lp->drag_start_y = y;
            lp->drag_x = x;
            lp->drag_y = y;
            lp->is_dragging = false;

            if (lp->filtered_items[i].type == LP_ITEM_APP) {
                lp->drag_candidate = lp->filtered_items[i].app;
                lp->drag_folder_candidate = NULL;
            } else {
                lp->drag_candidate = NULL;
                lp->drag_folder_candidate = lp->filtered_items[i].folder;
            }
            return true;
        }
    }

    lp->drag_candidate = NULL;
    lp->drag_folder_candidate = NULL;
    return false; // Clicked backdrop
}

int launchpad_handle_release(aurade_launchpad_t *lp, double x, double y) {
    if (!lp || !lp->active) return 0;

    // 1. SIMPLE CLICK (NOT DRAGGED)
    if (!lp->is_dragging) {
        if (lp->drag_from_folder && lp->drag_candidate) {
            desktop_app_t *app = lp->drag_candidate;
            lp->drag_candidate = NULL;
            if (lp->server && lp->server->dock) dock_start_bounce(lp->server->dock, app->name);
            app_launch_command(app->exec);
            return 2; // Close overlay on launch
        } else if (lp->drag_folder_candidate) {
            lp->open_folder = lp->drag_folder_candidate;
            lp->open_folder_item_idx = lp->drag_source_idx;
            lp->folder_renaming = false;
            lp->drag_folder_candidate = NULL;
            launchpad_render(lp);
            if (lp->server) schedule_all_outputs(lp->server);
            return 1;
        } else if (lp->drag_candidate) {
            desktop_app_t *app = lp->drag_candidate;
            lp->drag_candidate = NULL;
            if (lp->server && lp->server->dock) dock_start_bounce(lp->server->dock, app->name);
            app_launch_command(app->exec);
            return 2; // Close overlay on launch
        }
        return 0;
    }

    // 2. DRAGGED ITEMS
    if (lp->drag_over_dock) {
        launchpad_cancel_drag(lp);
        return 1;
    }

    if (lp->drag_from_folder && lp->open_folder) {
        lp_folder_layout_t fl;
        lp_folder_layout(lp, &fl);
        bool inside_window = (x >= fl.win_x && x <= fl.win_x + fl.win_w &&
                              y >= fl.win_y && y <= fl.win_y + fl.win_h);

        if (!inside_window) {
            // Dragged out of folder -> extract to main Launchpad grid
            desktop_app_t *app = lp->dragged_app;
            if (app) {
                int s_idx = lp->drag_source_idx;
                for (int k = s_idx; k < lp->open_folder->app_count - 1; k++) {
                    lp->open_folder->apps[k] = lp->open_folder->apps[k + 1];
                }
                lp->open_folder->app_count--;

                // Insert into main items list
                if (lp->item_count < LAUNCHPAD_MAX_ITEMS) {
                    lp->items[lp->item_count].type = LP_ITEM_APP;
                    lp->items[lp->item_count].app = app;
                    lp->items[lp->item_count].folder = NULL;
                    lp->item_count++;
                }

                // If folder has <= 1 app left, dissolve folder
                if (lp->open_folder->app_count <= 1) {
                    int f_item_idx = lp->open_folder_item_idx;
                    if (lp->open_folder->app_count == 1) {
                        desktop_app_t *last_app = lp->open_folder->apps[0];
                        lp->items[f_item_idx].type = LP_ITEM_APP;
                        lp->items[f_item_idx].app = last_app;
                        free(lp->items[f_item_idx].folder);
                        lp->items[f_item_idx].folder = NULL;
                    } else {
                        free(lp->items[f_item_idx].folder);
                        for (int k = f_item_idx; k < lp->item_count - 1; k++) {
                            lp->items[k] = lp->items[k + 1];
                        }
                        lp->item_count--;
                    }
                    lp->open_folder = NULL;
                }
                filter_items(lp);
                launchpad_save_layout(lp);
            }
        } else {
            // Reorder inside folder
            if (lp->drop_target_idx >= 0 && lp->drop_target_idx < lp->open_folder->app_count &&
                lp->drop_target_idx != lp->drag_source_idx) {
                int s = lp->drag_source_idx;
                int t = lp->drop_target_idx;
                desktop_app_t *tmp = lp->open_folder->apps[s];
                if (s < t) {
                    for (int k = s; k < t; k++) lp->open_folder->apps[k] = lp->open_folder->apps[k + 1];
                } else {
                    for (int k = s; k > t; k--) lp->open_folder->apps[k] = lp->open_folder->apps[k - 1];
                }
                lp->open_folder->apps[t] = tmp;
                launchpad_save_layout(lp);
            }
        }
    } else {
        // Dragged on main grid
        int s = lp->drag_source_idx;
        int t = lp->drop_target_idx;

        if (lp->drop_action == DROP_TARGET_FOLDER_CREATE && lp->dragged_app &&
            t >= 0 && t < lp->item_count && s != t && lp->items[t].type == LP_ITEM_APP) {
            // Create a new folder!
            desktop_app_t *target_app = lp->items[t].app;
            desktop_app_t *source_app = lp->dragged_app;

            lp_folder_t *f = calloc(1, sizeof(lp_folder_t));
            snprintf(f->name, sizeof(f->name), "Folder");
            f->apps[0] = target_app;
            f->apps[1] = source_app;
            f->app_count = 2;

            lp->items[t].type = LP_ITEM_FOLDER;
            lp->items[t].folder = f;
            lp->items[t].app = NULL;

            // Remove source item
            for (int k = s; k < lp->item_count - 1; k++) {
                lp->items[k] = lp->items[k + 1];
            }
            lp->item_count--;

            filter_items(lp);
            launchpad_save_layout(lp);
        } else if (lp->drop_action == DROP_TARGET_FOLDER_INSERT && lp->dragged_app &&
                   t >= 0 && t < lp->item_count && s != t && lp->items[t].type == LP_ITEM_FOLDER) {
            // Insert into existing folder!
            lp_folder_t *f = lp->items[t].folder;
            if (f && f->app_count < LAUNCHPAD_MAX_FOLDER_ITEMS) {
                f->apps[f->app_count++] = lp->dragged_app;

                // Remove source item
                for (int k = s; k < lp->item_count - 1; k++) {
                    lp->items[k] = lp->items[k + 1];
                }
                lp->item_count--;

                filter_items(lp);
                launchpad_save_layout(lp);
            }
        } else if (lp->drop_action == DROP_TARGET_REORDER && s >= 0 && s < lp->item_count &&
                   t >= 0 && t < lp->item_count && s != t) {
            // Reorder items on grid
            lp_item_t tmp = lp->items[s];
            if (s < t) {
                for (int k = s; k < t; k++) lp->items[k] = lp->items[k + 1];
            } else {
                for (int k = s; k > t; k--) lp->items[k] = lp->items[k - 1];
            }
            lp->items[t] = tmp;

            filter_items(lp);
            launchpad_save_layout(lp);
        }
    }

    launchpad_cancel_drag(lp);
    launchpad_render(lp);
    if (lp->server) schedule_all_outputs(lp->server);
    return 1;
}

void launchpad_cancel_drag(aurade_launchpad_t *lp) {
    if (!lp) return;
    bool was_dragging = lp->is_dragging;
    lp->is_dragging = false;
    lp->drag_candidate = NULL;
    lp->drag_folder_candidate = NULL;
    lp->dragged_app = NULL;
    lp->dragged_folder = NULL;
    lp->drag_over_dock = false;
    lp->drop_action = DROP_TARGET_NONE;
    lp->drop_target_idx = -1;
    lp->drag_edge_hover_time_ms = 0;

    if (lp->drag_scene_buffer) {
        wlr_scene_node_set_enabled(&lp->drag_scene_buffer->node, false);
    }
    if (was_dragging) {
        launchpad_render(lp);
    }
    if (lp->server) schedule_all_outputs(lp->server);
}

void launchpad_handle_motion(aurade_launchpad_t *lp, double x, double y) {
    if (!lp || !lp->active || lp->progress < 0.6) return;

    if (lp->drag_candidate || lp->drag_folder_candidate) {
        double dist = hypot(x - lp->drag_start_x, y - lp->drag_start_y);
        if (dist > 8.0 || lp->is_dragging) {
            bool just_started = !lp->is_dragging;
            lp->is_dragging = true;
            lp->dragged_app = lp->drag_candidate;
            lp->dragged_folder = lp->drag_folder_candidate;
            lp->drag_x = x;
            lp->drag_y = y;

            bool over_dock = false;
            if (lp->server && lp->server->dock) {
                over_dock = dock_contains_point(lp->server->dock, x, y);
            }

            if (just_started || over_dock != lp->drag_over_dock) {
                lp->drag_over_dock = over_dock;
                launchpad_render_drag_preview(lp, over_dock);
                if (lp->drag_scene_buffer) {
                    wlr_scene_node_set_enabled(&lp->drag_scene_buffer->node, true);
                }
            }

            if (lp->drag_scene_buffer) {
                wlr_scene_node_set_position(&lp->drag_scene_buffer->node,
                                            (int)round(x - 110.0),
                                            (int)round(y - 52.0));
                wlr_scene_node_raise_to_top(&lp->drag_scene_buffer->node);
            }

            // EDGE PAGING DURING DRAG
            uint64_t now = lp_now_ms();
            if (!lp->open_folder && lp->total_pages > 1) {
                if (x < 50.0 && lp->current_page > 0) {
                    if (lp->drag_edge_hover_time_ms == 0) {
                        lp->drag_edge_hover_time_ms = now;
                    } else if (now - lp->drag_edge_hover_time_ms > 450) {
                        launchpad_switch_page(lp, lp->current_page - 1, true);
                        lp->drag_edge_hover_time_ms = now + 400;
                    }
                } else if (x > lp->screen_width - 50.0 && lp->current_page < lp->total_pages - 1) {
                    if (lp->drag_edge_hover_time_ms == 0) {
                        lp->drag_edge_hover_time_ms = now;
                    } else if (now - lp->drag_edge_hover_time_ms > 450) {
                        launchpad_switch_page(lp, lp->current_page + 1, true);
                        lp->drag_edge_hover_time_ms = now + 400;
                    }
                } else {
                    lp->drag_edge_hover_time_ms = 0;
                }
            }

            // TARGET DETECTION
            lp_drop_action_t prev_drop_action = lp->drop_action;
            int prev_drop_target = lp->drop_target_idx;

            if (lp->open_folder != NULL) {
                lp_folder_layout_t fl;
                lp_folder_layout(lp, &fl);
                bool inside_window = (x >= fl.win_x && x <= fl.win_x + fl.win_w &&
                                      y >= fl.win_y && y <= fl.win_y + fl.win_h);
                if (inside_window) {
                    lp->drop_action = DROP_TARGET_REORDER;
                    lp->drop_target_idx = -1;
                    for (int i = 0; i < lp->open_folder->app_count; i++) {
                        int r = i / fl.cols;
                        int c = i % fl.cols;
                        double cx = fl.grid_x + c * fl.cell_w + fl.cell_w / 2.0;
                        double cy = fl.grid_y + r * fl.cell_h + fl.cell_h / 2.0;
                        if (fabs(x - cx) < fl.cell_w * 0.46 && fabs(y - cy) < fl.cell_h * 0.46) {
                            lp->drop_target_idx = i;
                            break;
                        }
                    }
                } else {
                    // Outside folder window -> will extract
                    lp->drop_action = DROP_TARGET_REORDER;
                    lp->drop_target_idx = -1;
                }
            } else {
                // On main grid
                lp_layout_t l;
                lp_layout(lp, &l);
                int target_page = lp->current_page;
                int page_start = target_page * LP_PAGE_SIZE;
                int page_end = page_start + LP_PAGE_SIZE;
                if (page_end > lp->filtered_count) page_end = lp->filtered_count;

                lp->drop_action = DROP_TARGET_REORDER;
                lp->drop_target_idx = -1;

                for (int i = page_start; i < page_end; i++) {
                    int idx = i - page_start;
                    int r = idx / LP_COLS;
                    int c = idx % LP_COLS;
                    double cx = l.grid_x + c * l.cell_w + l.cell_w / 2.0;
                    double cy = l.grid_y + r * l.cell_h + l.cell_h / 2.0;
                    double dist_to_cell = hypot(x - cx, y - cy);

                    if (dist_to_cell < l.cell_w * 0.46) {
                        lp->drop_target_idx = i;
                        if (dist_to_cell < l.icon_size * 0.55 && i != lp->drag_source_idx) {
                            if (lp->dragged_app && lp->filtered_items[i].type == LP_ITEM_APP) {
                                lp->drop_action = DROP_TARGET_FOLDER_CREATE;
                            } else if (lp->dragged_app && lp->filtered_items[i].type == LP_ITEM_FOLDER) {
                                lp->drop_action = DROP_TARGET_FOLDER_INSERT;
                            }
                        }
                        break;
                    }
                }
            }

            if (just_started || prev_drop_action != lp->drop_action || prev_drop_target != lp->drop_target_idx) {
                launchpad_render(lp);
            }
            if (lp->server) schedule_all_outputs(lp->server);
            return;
        }
    }

    // Normal motion (not dragging)
    if (lp->open_folder != NULL) {
        lp_folder_layout_t fl;
        lp_folder_layout(lp, &fl);
        int prev_hover = lp->hovered_folder_app_idx;
        lp->hovered_folder_app_idx = -1;

        for (int i = 0; i < lp->open_folder->app_count; i++) {
            int r = i / fl.cols;
            int c = i % fl.cols;
            double cx = fl.grid_x + c * fl.cell_w + fl.cell_w / 2.0;
            double cy = fl.grid_y + r * fl.cell_h + fl.cell_h / 2.0;
            if (fabs(x - cx) < fl.cell_w * 0.46 && fabs(y - cy) < fl.cell_h * 0.46) {
                lp->hovered_folder_app_idx = i;
                break;
            }
        }
        if (lp->hovered_folder_app_idx != prev_hover) {
            launchpad_render(lp);
        }
        return;
    }

    // Main grid hover
    lp_layout_t l;
    lp_layout(lp, &l);
    int prev_hover = lp->hovered_index;
    lp->hovered_index = -1;

    int page_start = lp->current_page * LP_PAGE_SIZE;
    int page_end = page_start + LP_PAGE_SIZE;
    if (page_end > lp->filtered_count) page_end = lp->filtered_count;

    for (int i = page_start; i < page_end; i++) {
        int idx = i - page_start;
        int r = idx / LP_COLS;
        int c = idx % LP_COLS;
        double cx = l.grid_x + c * l.cell_w + l.cell_w / 2.0;
        double cy = l.grid_y + r * l.cell_h + l.cell_h / 2.0;
        if (fabs(x - cx) < l.cell_w * 0.46 && fabs(y - cy) < l.cell_h * 0.46) {
            lp->hovered_index = i;
            break;
        }
    }

    if (lp->hovered_index != prev_hover) {
        launchpad_render(lp);
    }
}

bool launchpad_handle_click(aurade_launchpad_t *lp, double x, double y) {
    int res = launchpad_handle_release(lp, x, y);
    if (res == 2) {
        server_close_overlay(lp->server);
        return true;
    }
    return res != 0;
}

bool launchpad_handle_key(aurade_launchpad_t *lp, uint32_t keyval, bool pressed) {
    if (!lp || !lp->active || !pressed) return false;

    // FOLDER RENAMING ACTIVE
    if (lp->folder_renaming && lp->open_folder) {
        if (keyval == XKB_KEY_Return || keyval == XKB_KEY_KP_Enter) {
            if (lp->folder_rename_buf[0]) {
                snprintf(lp->open_folder->name, sizeof(lp->open_folder->name), "%s", lp->folder_rename_buf);
                launchpad_save_layout(lp);
            }
            lp->folder_renaming = false;
            launchpad_render(lp);
            return true;
        } else if (keyval == XKB_KEY_Escape) {
            lp->folder_renaming = false;
            launchpad_render(lp);
            return true;
        } else if (keyval == XKB_KEY_BackSpace) {
            size_t len = strlen(lp->folder_rename_buf);
            if (len > 0) {
                lp->folder_rename_buf[len - 1] = '\0';
                launchpad_render(lp);
            }
            return true;
        }

        char utf8[8];
        int n = xkb_keysym_to_utf8(keyval, utf8, sizeof(utf8));
        if (n > 0) {
            size_t len = strlen(lp->folder_rename_buf);
            if (len + (size_t)n < sizeof(lp->folder_rename_buf) - 1) {
                memcpy(lp->folder_rename_buf + len, utf8, (size_t)n);
                lp->folder_rename_buf[len + n] = '\0';
                launchpad_render(lp);
            }
            return true;
        }
        return true;
    }

    // FOLDER OPEN (NOT RENAMING)
    if (lp->open_folder != NULL) {
        if (keyval == XKB_KEY_Escape) {
            lp->open_folder = NULL;
            launchpad_render(lp);
            return true;
        } else if (keyval == XKB_KEY_Return || keyval == XKB_KEY_KP_Enter) {
            if (lp->open_folder->app_count > 0) {
                desktop_app_t *app = lp->open_folder->apps[0];
                app_launch_command(app->exec);
                server_close_overlay(lp->server);
            }
            return true;
        }
    }

    // MAIN LAUNCHPAD KEYS
    if (keyval == XKB_KEY_Escape) {
        if (lp->search_query[0] != '\0') {
            lp->search_query[0] = '\0';
            filter_items(lp);
            launchpad_render(lp);
        } else {
            server_close_overlay(lp->server);
        }
        return true;
    } else if (keyval == XKB_KEY_Return || keyval == XKB_KEY_KP_Enter) {
        if (lp->filtered_count > 0) {
            if (lp->filtered_items[0].type == LP_ITEM_APP && lp->filtered_items[0].app) {
                desktop_app_t *app = lp->filtered_items[0].app;
                if (lp->server && lp->server->dock) dock_start_bounce(lp->server->dock, app->name);
                app_launch_command(app->exec);
                server_close_overlay(lp->server);
            } else if (lp->filtered_items[0].type == LP_ITEM_FOLDER && lp->filtered_items[0].folder) {
                lp->open_folder = lp->filtered_items[0].folder;
                lp->open_folder_item_idx = 0;
                lp->folder_renaming = false;
                launchpad_render(lp);
            }
        }
        return true;
    } else if (keyval == XKB_KEY_BackSpace) {
        size_t len = strlen(lp->search_query);
        if (len > 0) {
            lp->search_query[len - 1] = '\0';
            filter_items(lp);
            launchpad_render(lp);
        }
        return true;
    } else if (keyval == XKB_KEY_Left || keyval == XKB_KEY_Page_Up) {
        if (lp->total_pages > 1) {
            int prev = lp->current_page - 1;
            if (prev < 0) prev = lp->total_pages - 1;
            launchpad_switch_page(lp, prev, true);
        }
        return true;
    } else if (keyval == XKB_KEY_Right || keyval == XKB_KEY_Page_Down) {
        if (lp->total_pages > 1) {
            int next = (lp->current_page + 1) % lp->total_pages;
            launchpad_switch_page(lp, next, true);
        }
        return true;
    }

    char utf8[8];
    int n = xkb_keysym_to_utf8(keyval, utf8, sizeof(utf8));
    if (n > 0) {
        size_t len = strlen(lp->search_query);
        if (len + (size_t)n < sizeof(lp->search_query) - 1) {
            memcpy(lp->search_query + len, utf8, (size_t)n);
            lp->search_query[len + n] = '\0';
            filter_items(lp);
            launchpad_render(lp);
        }
        return true;
    }

    return false;
}

static void draw_squircle_icon(cairo_t *cr, double cx, double cy, double size,
                               const char *name, cairo_surface_t *surface, double alpha) {
    if (surface) {
        int iw = cairo_image_surface_get_width(surface);
        int ih = cairo_image_surface_get_height(surface);
        if (iw > 0 && ih > 0) {
            cairo_save(cr);
            cairo_translate(cr, cx - size / 2.0, cy - size / 2.0);
            cairo_scale(cr, size / iw, size / ih);
            cairo_set_source_surface(cr, surface, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint_with_alpha(cr, alpha);
            cairo_restore(cr);
            return;
        }
    }

    // Fallback Apple-style gradient squircle
    unsigned hash = 2166136261u;
    for (const char *p = name; *p; p++) {
        hash ^= (unsigned char)*p;
        hash *= 16777619u;
    }
    double h = (hash % 360) / 360.0;
    cairo_save(cr);
    draw_rounded_rect(cr, cx - size / 2.0, cy - size / 2.0, size, size, size * 0.23);

    cairo_pattern_t *pat = cairo_pattern_create_linear(cx, cy - size / 2.0, cx, cy + size / 2.0);
    double r = 0.30 + 0.35 * fabs(sin(h * M_PI));
    double g = 0.30 + 0.35 * fabs(sin((h + 0.33) * M_PI));
    double b = 0.40 + 0.35 * fabs(sin((h + 0.66) * M_PI));
    cairo_pattern_add_color_stop_rgba(pat, 0.0, fmin(1, r + 0.18), fmin(1, g + 0.18), fmin(1, b + 0.18), alpha);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, r * 0.72, g * 0.72, b * 0.72, alpha);
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);

    // Gloss
    draw_rounded_rect(cr, cx - size / 2.0 + 1.5, cy - size / 2.0 + 1.5,
                      size - 3.0, size * 0.4, size * 0.19);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16 * alpha);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22 * alpha);
    draw_rounded_rect(cr, cx - size / 2.0, cy - size / 2.0, size, size, size * 0.23);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    char letter[2] = { name[0] ? name[0] : '?', '\0' };
    draw_text(cr, letter, LP_FONT, size * 0.42, true,
              1.0, 1.0, 1.0, 0.95 * alpha, cx, cy, true, true);
    cairo_restore(cr);
}

static void draw_folder_icon(cairo_t *cr, double cx, double cy, double size,
                             lp_folder_t *folder, double alpha) {
    if (!folder) return;

    // Outer frosted squircle container
    draw_glass_panel(cr, cx - size / 2.0, cy - size / 2.0, size, size, size * 0.23,
                     0.22, 0.24, 0.32, 0.75 * alpha);

    cairo_save(cr);
    draw_rounded_rect(cr, cx - size / 2.0, cy - size / 2.0, size, size, size * 0.23);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.26 * alpha);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);

    int count = folder->app_count;
    if (count <= 0) return;

    // 2x2 grid if <= 4 apps, 3x3 if > 4 apps
    int cols = (count <= 4) ? 2 : 3;
    int max_draw = cols * cols;
    double inner_pad = size * 0.14;
    double inner_w = size - inner_pad * 2.0;
    double cell_size = inner_w / cols;
    double mini_size = cell_size * 0.82;

    for (int k = 0; k < count && k < max_draw; k++) {
        desktop_app_t *app = folder->apps[k];
        if (!app) continue;
        int r = k / cols;
        int c = k % cols;
        double mcx = (cx - size / 2.0) + inner_pad + c * cell_size + cell_size / 2.0;
        double mcy = (cy - size / 2.0) + inner_pad + r * cell_size + cell_size / 2.0;
        draw_squircle_icon(cr, mcx, mcy, mini_size, app->name, app->icon_surface, alpha);
    }
}

static void launchpad_render_drag_preview(aurade_launchpad_t *lp, bool over_dock) {
    if (!lp || !lp->drag_buffer || !lp->drag_buffer->cr) return;
    if (!lp->dragged_app && !lp->dragged_folder) return;

    cairo_t *cr = lp->drag_buffer->cr;
    cairo_buffer_clear(lp->drag_buffer);

    double dsize = 72.0;
    double cx = 110.0;
    double cy = 52.0;

    // Soft drop shadow
    cairo_arc(cr, cx, cy + 6.0, dsize * 0.52, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45);
    cairo_fill(cr);

    // Icon or folder
    if (lp->dragged_folder) {
        draw_folder_icon(cr, cx, cy, dsize, lp->dragged_folder, 1.0);
    } else if (lp->dragged_app) {
        draw_squircle_icon(cr, cx, cy, dsize, lp->dragged_app->name,
                           lp->dragged_app->icon_surface, 1.0);
    }

    // Pill badge below icon
    const char *badge_text = "";
    if (over_dock) {
        badge_text = "+ Pin to Dock";
    } else if (lp->drop_action == DROP_TARGET_FOLDER_CREATE) {
        badge_text = "+ Create Folder";
    } else if (lp->drop_action == DROP_TARGET_FOLDER_INSERT) {
        badge_text = "+ Add to Folder";
    } else if (lp->drag_from_folder) {
        badge_text = "Move to Launchpad";
    } else if (lp->dragged_folder) {
        badge_text = lp->dragged_folder->name;
    } else if (lp->dragged_app) {
        badge_text = lp->dragged_app->name;
    }

    double text_len = (double)strlen(badge_text);
    double badge_w = fmax(70.0, text_len * 7.5 + 24.0);
    double badge_h = 24.0;
    double badge_x = cx - badge_w / 2.0;
    double badge_y = cy + dsize / 2.0 + 8.0;

    if (over_dock || lp->drop_action == DROP_TARGET_FOLDER_CREATE || lp->drop_action == DROP_TARGET_FOLDER_INSERT) {
        draw_rounded_rect(cr, badge_x, badge_y, badge_w, badge_h, 12.0);
        cairo_set_source_rgba(cr, 0.12, 0.55, 0.95, 0.95);
        cairo_fill(cr);
    } else {
        draw_glass_panel(cr, badge_x, badge_y, badge_w, badge_h, 12.0,
                         0.15, 0.16, 0.20, 0.88);
    }

    draw_text(cr, badge_text, LP_FONT, 11.5, true,
              1.0, 1.0, 1.0, 0.98,
              cx, badge_y + badge_h / 2.0, true, true);

    cairo_buffer_apply_to_scene(lp->drag_buffer, lp->drag_scene_buffer);
}

void launchpad_render(aurade_launchpad_t *lp) {
    if (!lp || !lp->buffer || !lp->buffer->cr) return;
    lp->needs_render = false;
    cairo_t *cr = lp->buffer->cr;
    cairo_buffer_clear(lp->buffer);

    double p_raw = lp->progress;
    if (p_raw <= 0.0) return;
    double p = fmin(1.0, fmax(0.0, p_raw));

    // Background: Frosted real-time blur, wallpaper blur or translucent dark scrim
    if (lp->server->config.blur_launchpad && lp->realtime_blur) {
        int bw = cairo_image_surface_get_width(lp->realtime_blur);
        int bh = cairo_image_surface_get_height(lp->realtime_blur);
        if (bw > 0 && bh > 0) {
            cairo_save(cr);
            cairo_scale(cr, (double)lp->screen_width / bw, (double)lp->screen_height / bh);
            cairo_set_source_surface(cr, lp->realtime_blur, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
            cairo_paint_with_alpha(cr, p);
            cairo_restore(cr);

            cairo_set_source_rgba(cr, 0.02, 0.03, 0.06, 0.65 * p);
            cairo_paint(cr);
        }
    } else if (lp->server->config.blur_launchpad && lp->server->wallpaper) {
        wallpaper_draw_backdrop(lp->server->wallpaper, cr,
                                lp->screen_width, lp->screen_height, 0.65, p);
    } else {
        cairo_set_source_rgba(cr, 0.04, 0.05, 0.09, 0.82 * p);
        cairo_paint(cr);
    }

    lp_layout_t l;
    lp_layout(lp, &l);

    // Zoom & convergence transform:
    // Opening: items start scaled up (1.30) in the 4 corners and converge towards the center (1.0).
    // Closing: items disperse radially outwards towards the 4 corners (1.0 -> 1.30) and fade out.
    double zoom = 1.0 + 0.30 * (1.0 - p_raw);
    cairo_save(cr);
    cairo_translate(cr, lp->screen_width / 2.0, lp->screen_height / 2.0);
    cairo_scale(cr, zoom, zoom);
    cairo_translate(cr, -lp->screen_width / 2.0, -lp->screen_height / 2.0);

    // Search bar
    draw_glass_panel(cr, l.search_x, l.search_y, l.search_w, l.search_h, l.search_h / 2.0,
                     0.22, 0.24, 0.30, 0.72 * p);
    draw_search_icon(cr, l.search_x + 19.0, l.search_y + l.search_h / 2.0, 15.0, 0.65 * p);

    if (lp->search_query[0] == '\0') {
        draw_text(cr, "Search", LP_FONT, 13.0, false,
                  0.82, 0.85, 0.90, 0.55 * p,
                  l.search_x + 36.0, l.search_y + l.search_h / 2.0 + 0.5, false, true);
    } else {
        draw_text(cr, lp->search_query, LP_FONT, 13.0, false,
                  1.0, 1.0, 1.0, 0.95 * p,
                  l.search_x + 36.0, l.search_y + l.search_h / 2.0 + 0.5, false, true);
    }

    // RENDER MAIN GRID ITEMS ACROSS PAGES
    for (int p_idx = 0; p_idx < lp->total_pages; p_idx++) {
        double offset_x = (p_idx - lp->page_scroll) * lp->screen_width;
        if (offset_x + lp->screen_width < -50 || offset_x > lp->screen_width + 50) {
            continue;
        }

        int page_start = p_idx * LP_PAGE_SIZE;
        int page_end = page_start + LP_PAGE_SIZE;
        if (page_end > lp->filtered_count) page_end = lp->filtered_count;

        for (int i = page_start; i < page_end; i++) {
            lp_item_t *item = &lp->filtered_items[i];
            int idx = i - page_start;
            int r = idx / LP_COLS;
            int c = idx % LP_COLS;

            double cx = offset_x + l.grid_x + c * l.cell_w + l.cell_w / 2.0;
            double cy = l.grid_y + r * l.cell_h + l.cell_h / 2.0;

            bool is_dragged = (lp->is_dragging &&
                               ((item->type == LP_ITEM_APP && item->app == lp->dragged_app) ||
                                (item->type == LP_ITEM_FOLDER && item->folder == lp->dragged_folder)));
            double alpha = is_dragged ? 0.25 * p : p;

            bool hover = (lp->hovered_index == i && !lp->is_dragging && lp->open_folder == NULL);
            double size = l.icon_size * (hover ? 1.06 : 1.0);

            // Drop highlight indicators
            if (lp->is_dragging && lp->drop_target_idx == i && !lp->open_folder) {
                if (lp->drop_action == DROP_TARGET_FOLDER_CREATE) {
                    // Glowing halo around target app icon to create folder
                    cairo_arc(cr, cx, cy, size * 0.78, 0, 2 * M_PI);
                    cairo_set_source_rgba(cr, 0.12, 0.55, 0.95, 0.35 * p);
                    cairo_fill(cr);

                    cairo_arc(cr, cx, cy, size * 0.78, 0, 2 * M_PI);
                    cairo_set_source_rgba(cr, 0.25, 0.75, 1.0, 0.85 * p);
                    cairo_set_line_width(cr, 2.5);
                    cairo_stroke(cr);
                } else if (lp->drop_action == DROP_TARGET_FOLDER_INSERT) {
                    // Highlight folder to insert app
                    draw_rounded_rect(cr, cx - size / 2.0 - 5.0, cy - size / 2.0 - 5.0,
                                      size + 10.0, size + 10.0, (size + 10.0) * 0.23);
                    cairo_set_source_rgba(cr, 0.12, 0.55, 0.95, 0.35 * p);
                    cairo_fill(cr);

                    draw_rounded_rect(cr, cx - size / 2.0 - 5.0, cy - size / 2.0 - 5.0,
                                      size + 10.0, size + 10.0, (size + 10.0) * 0.23);
                    cairo_set_source_rgba(cr, 0.25, 0.75, 1.0, 0.85 * p);
                    cairo_set_line_width(cr, 2.5);
                    cairo_stroke(cr);
                } else if (lp->drop_action == DROP_TARGET_REORDER) {
                    // Reorder indicator line
                    cairo_move_to(cr, cx - l.cell_w * 0.44, cy - size * 0.55);
                    cairo_line_to(cr, cx - l.cell_w * 0.44, cy + size * 0.55);
                    cairo_set_source_rgba(cr, 0.25, 0.75, 1.0, 0.85 * p);
                    cairo_set_line_width(cr, 3.0);
                    cairo_stroke(cr);
                }
            } else if (hover) {
                cairo_arc(cr, cx, cy, size * 0.74, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.13 * p);
                cairo_fill(cr);
            }

            if (item->type == LP_ITEM_APP && item->app) {
                draw_squircle_icon(cr, cx, cy, size, item->app->name,
                                   item->app->icon_surface, alpha);
                draw_text_shadowed(cr, item->app->name, LP_FONT, 12.0, true,
                                   1.0, 1.0, 1.0, 0.96 * alpha, 0.35 * alpha,
                                   cx, cy + size / 2.0 + 17.0, true, true);
            } else if (item->type == LP_ITEM_FOLDER && item->folder) {
                draw_folder_icon(cr, cx, cy, size, item->folder, alpha);
                draw_text_shadowed(cr, item->folder->name, LP_FONT, 12.0, true,
                                   1.0, 1.0, 1.0, 0.96 * alpha, 0.35 * alpha,
                                   cx, cy + size / 2.0 + 17.0, true, true);
            }
        }
    }

    if (lp->filtered_count == 0) {
        draw_text(cr, "No applications found", LP_FONT, 15.0, false,
                  1.0, 1.0, 1.0, 0.6 * p, lp->screen_width / 2.0,
                  lp->screen_height / 2.0, true, true);
    }

    // Pagination dots + chevrons
    if (lp->total_pages > 1 && lp->open_folder == NULL) {
        double dots_y = launchpad_dots_y(lp);
        double dots_start = (lp->screen_width - (lp->total_pages * 22.0)) / 2.0;

        for (int page = 0; page < lp->total_pages; page++) {
            double dot_x = dots_start + page * 22.0 + 11.0;
            double dist = fabs((double)page - lp->page_scroll);
            double active_weight = 1.0 - fmin(1.0, dist);
            double radius = 3.5 + 1.5 * active_weight;
            double alpha = (0.35 + 0.60 * active_weight) * p;

            cairo_arc(cr, dot_x, dots_y, radius, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
            cairo_fill(cr);
        }

        draw_chevron(cr, dots_start - 34.0, dots_y, 16.0, 0, 0.55 * p);
        draw_chevron(cr, dots_start + lp->total_pages * 22.0 + 34.0, dots_y, 16.0, 1, 0.55 * p);
    }

    // OPEN FOLDER MODAL WINDOW (macOS STYLE)
    if (lp->open_folder != NULL) {
        // Dim background grid further
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.40 * p);
        cairo_paint(cr);

        lp_folder_layout_t fl;
        lp_folder_layout(lp, &fl);

        // Drop shadow for modal window
        draw_drop_shadow(cr, fl.win_x, fl.win_y, fl.win_w, fl.win_h, 24.0, 36.0, 0.50 * p);

        // Glass panel background
        draw_glass_panel(cr, fl.win_x, fl.win_y, fl.win_w, fl.win_h, 24.0,
                         0.14, 0.16, 0.22, 0.94 * p);

        // Subtle 1px bright border
        cairo_save(cr);
        draw_rounded_rect(cr, fl.win_x, fl.win_y, fl.win_w, fl.win_h, 24.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22 * p);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_restore(cr);

        // Close button (top right)
        cairo_arc(cr, fl.close_x, fl.close_y, fl.close_r, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14 * p);
        cairo_fill(cr);
        cairo_move_to(cr, fl.close_x - 5.0, fl.close_y - 5.0);
        cairo_line_to(cr, fl.close_x + 5.0, fl.close_y + 5.0);
        cairo_move_to(cr, fl.close_x + 5.0, fl.close_y - 5.0);
        cairo_line_to(cr, fl.close_x - 5.0, fl.close_y + 5.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.75 * p);
        cairo_set_line_width(cr, 1.8);
        cairo_stroke(cr);

        // Folder Title
        if (lp->folder_renaming) {
            double tw = fmax(180.0, strlen(lp->folder_rename_buf) * 12.0 + 40.0);
            draw_glass_panel(cr, fl.title_x - tw / 2.0, fl.title_y - 18.0, tw, 36.0, 18.0,
                             0.25, 0.28, 0.38, 0.90 * p);

            char display_buf[80];
            snprintf(display_buf, sizeof(display_buf), "%s|", lp->folder_rename_buf);
            draw_text(cr, display_buf, LP_FONT, 18.0, true,
                      1.0, 1.0, 1.0, 0.98 * p,
                      fl.title_x, fl.title_y, true, true);
        } else {
            draw_text_shadowed(cr, lp->open_folder->name, LP_FONT, 20.0, true,
                               1.0, 1.0, 1.0, 0.98 * p, 0.40 * p,
                               fl.title_x, fl.title_y, true, true);
        }

        // Apps inside the folder
        for (int i = 0; i < lp->open_folder->app_count; i++) {
            desktop_app_t *app = lp->open_folder->apps[i];
            if (!app) continue;

            int r = i / fl.cols;
            int c = i % fl.cols;
            double cx = fl.grid_x + c * fl.cell_w + fl.cell_w / 2.0;
            double cy = fl.grid_y + r * fl.cell_h + fl.cell_h / 2.0;

            bool is_dragged = (lp->is_dragging && lp->drag_from_folder && lp->dragged_app == app);
            double alpha = is_dragged ? 0.25 * p : p;

            bool hover = (lp->hovered_folder_app_idx == i && !lp->is_dragging);
            double size = fl.icon_size * (hover ? 1.06 : 1.0);

            if (hover) {
                cairo_arc(cr, cx, cy, size * 0.74, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14 * p);
                cairo_fill(cr);
            }

            draw_squircle_icon(cr, cx, cy, size, app->name, app->icon_surface, alpha);
            draw_text_shadowed(cr, app->name, LP_FONT, 11.5, true,
                               1.0, 1.0, 1.0, 0.96 * alpha, 0.35 * alpha,
                               cx, cy + size / 2.0 + 15.0, true, true);
        }
    }

    cairo_restore(cr);

    cairo_buffer_apply_to_scene(lp->buffer, lp->scene_buffer);
}

void launchpad_destroy(aurade_launchpad_t *lp) {
    if (!lp) return;
    if (lp->realtime_blur) {
        cairo_surface_destroy(lp->realtime_blur);
        lp->realtime_blur = NULL;
    }
    for (int i = 0; i < lp->item_count; i++) {
        if (lp->items[i].type == LP_ITEM_FOLDER && lp->items[i].folder) {
            free(lp->items[i].folder);
            lp->items[i].folder = NULL;
        }
    }
    if (lp->drag_buffer) wlr_buffer_drop(&lp->drag_buffer->base);
    if (lp->buffer) wlr_buffer_drop(&lp->buffer->base);
    free(lp);
}
