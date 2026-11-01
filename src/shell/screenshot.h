#ifndef AURADE_SCREENSHOT_H
#define AURADE_SCREENSHOT_H

#include <stdbool.h>
#include <stdint.h>

struct aurade_server;
struct aurade_screenshot;
typedef struct aurade_screenshot aurade_screenshot_t;

typedef enum {
    SHOT_MODE_STILL = 0,    // Screenshot (photo)
    SHOT_MODE_VIDEO,        // Screen recording (video)
} shot_mode_t;

typedef enum {
    SHOT_TARGET_SELECTION = 0, // Crop / rectangle
    SHOT_TARGET_FULLSCREEN,    // Entire display
} shot_target_t;

aurade_screenshot_t *screenshot_create(struct aurade_server *server);
void screenshot_destroy(aurade_screenshot_t *shot);
void screenshot_resize(aurade_screenshot_t *shot, int width, int height);

bool screenshot_is_active(struct aurade_screenshot *shot);
void screenshot_begin(struct aurade_screenshot *shot);
void screenshot_begin_region(struct aurade_screenshot *shot);
void screenshot_begin_video(struct aurade_screenshot *shot);
void screenshot_capture_full(struct aurade_screenshot *shot);

// Video Recording API
bool screenshot_is_recording(struct aurade_screenshot *shot);
uint32_t screenshot_get_recording_duration_sec(struct aurade_screenshot *shot);
void screenshot_start_recording(struct aurade_screenshot *shot);
void screenshot_stop_recording(struct aurade_screenshot *shot);

// Input handling
bool screenshot_handle_motion(struct aurade_screenshot *shot, double x, double y);
bool screenshot_handle_button(struct aurade_screenshot *shot, double x, double y,
                              uint32_t button, bool pressed);
bool screenshot_handle_key(struct aurade_screenshot *shot, uint32_t sym, bool pressed);

#endif // AURADE_SCREENSHOT_H
