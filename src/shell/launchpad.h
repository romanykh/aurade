#ifndef AURADE_LAUNCHPAD_H
#define AURADE_LAUNCHPAD_H

#include "../server.h"
#include "../render/cairo_buffer.h"
#include <wlr/types/wlr_pointer.h>

#define LAUNCHPAD_MAX_FOLDER_ITEMS 128
#define LAUNCHPAD_MAX_ITEMS 512
#define LAUNCHPAD_MAX_FILTERED 512

typedef enum {
    LP_ITEM_APP = 0,
    LP_ITEM_FOLDER,
} lp_item_type_t;

typedef struct lp_folder {
    char name[64];
    desktop_app_t *apps[LAUNCHPAD_MAX_FOLDER_ITEMS];
    int app_count;
    cairo_surface_t *preview_surface;
} lp_folder_t;

typedef struct lp_item {
    lp_item_type_t type;
    desktop_app_t *app;
    lp_folder_t *folder;
} lp_item_t;

typedef enum {
    DROP_TARGET_NONE = 0,
    DROP_TARGET_REORDER,
    DROP_TARGET_FOLDER_CREATE,
    DROP_TARGET_FOLDER_INSERT,
} lp_drop_action_t;

typedef struct aurade_launchpad {
    aurade_server_t *server;
    cairo_buffer_t *buffer;

    // Hardware-accelerated GPU zoom & fade animation
    struct wlr_scene_tree *tree;
    struct wlr_scene_buffer *bg_scene_buffer;
    struct wlr_scene_buffer *fg_scene_buffer;

    int screen_width, screen_height;
    bool active;
    double progress; // 0.0 (hidden) to 1.0 (fully open)
    bool needs_render;

    char search_query[64];
    int current_page;
    int target_page;
    int total_pages;

    // Page swipe animation
    bool page_animating;
    double page_scroll;
    spring_simulation_t page_spring;
    double scroll_accum_x;
    double scroll_accum_y;
    uint64_t last_scroll_time_ms;

    // Master items list (apps + folders in user order)
    lp_item_t items[LAUNCHPAD_MAX_ITEMS];
    int item_count;

    // Filtered items (for rendering the active search or current page)
    lp_item_t filtered_items[LAUNCHPAD_MAX_FILTERED];
    int filtered_count;

    // Open folder modal view state
    lp_folder_t *open_folder;
    lp_folder_t *anim_folder;
    int open_folder_item_idx;
    bool folder_renaming;
    char folder_rename_buf[64];
    int hovered_folder_app_idx;

    // Folder vertical scroll state
    double folder_scroll_y;
    double folder_target_scroll_y;
    bool folder_scrollbar_dragging;
    double folder_drag_start_mouse_y;
    double folder_drag_start_scroll_y;

    // Folder opening & closing spring animation
    double folder_anim_progress; // 0.0 (closed) to 1.0 (fully open)
    double folder_anim_target;   // 0.0 or 1.0
    bool folder_animating;
    spring_simulation_t folder_spring;

    // Hover state on main grid
    int hovered_index;

    // Drag-and-drop & reorganization
    bool is_dragging;
    desktop_app_t *drag_candidate;
    lp_folder_t *drag_folder_candidate;
    int drag_source_idx;
    bool drag_from_folder;
    desktop_app_t *dragged_app;
    lp_folder_t *dragged_folder;
    double drag_start_x, drag_start_y;
    double drag_x, drag_y;
    bool drag_over_dock;
    lp_drop_action_t drop_action;
    int drop_target_idx;
    uint64_t drag_edge_hover_time_ms;
    cairo_buffer_t *drag_buffer;
    struct wlr_scene_buffer *drag_scene_buffer;
} aurade_launchpad_t;

aurade_launchpad_t *launchpad_create(aurade_server_t *server);
void launchpad_resize(aurade_launchpad_t *lp, int width, int height);
void launchpad_begin(aurade_launchpad_t *lp);
void launchpad_update_progress(aurade_launchpad_t *lp, double progress);
void launchpad_end(aurade_launchpad_t *lp, bool opened);
void launchpad_switch_page(aurade_launchpad_t *lp, int new_page, bool animate);
void launchpad_tick(aurade_launchpad_t *lp, double dt);
void launchpad_handle_axis(aurade_launchpad_t *lp, enum wl_pointer_axis orientation,
                           double delta, int32_t delta_discrete);
bool launchpad_handle_press(aurade_launchpad_t *lp, double x, double y);
int launchpad_handle_release(aurade_launchpad_t *lp, double x, double y);
void launchpad_cancel_drag(aurade_launchpad_t *lp);
bool launchpad_handle_click(aurade_launchpad_t *lp, double x, double y);
void launchpad_handle_motion(aurade_launchpad_t *lp, double x, double y);
bool launchpad_handle_key(aurade_launchpad_t *lp, uint32_t keyval, bool pressed);
void launchpad_render(aurade_launchpad_t *lp);
void launchpad_refresh(aurade_launchpad_t *lp);
void launchpad_open_folder(aurade_launchpad_t *lp, lp_folder_t *folder, bool animate);
void launchpad_close_folder(aurade_launchpad_t *lp, bool animate);
void launchpad_destroy(aurade_launchpad_t *lp);

void launchpad_load_layout(aurade_launchpad_t *lp);
void launchpad_save_layout(aurade_launchpad_t *lp);

#endif // AURADE_LAUNCHPAD_H
