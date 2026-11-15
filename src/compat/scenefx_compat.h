#ifndef AURADE_SCENEFX_COMPAT_H
#define AURADE_SCENEFX_COMPAT_H

#include <stdbool.h>

struct wlr_scene_blur;

#ifndef HAVE_SCENEFX_LIQUID_GLASS
static inline void wlr_scene_blur_set_liquid_glass(struct wlr_scene_blur *blur, bool enable) {
    (void)blur;
    (void)enable;
}
#endif

#endif /* AURADE_SCENEFX_COMPAT_H */
