#include "../src/ext/aurade_ext.h"
#include <stddef.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static const aurade_ext_api_t *g_api = NULL;

static bool on_key(uint32_t sym, uint32_t mods, bool pressed, void *ud) {
    (void)ud;
    /* Super+Shift+X triggers an intentional SIGSEGV inside the extension */
    if (pressed && (mods & 1u) && (mods & 2u) &&
        (sym == XKB_KEY_x || sym == XKB_KEY_X)) {
        g_api->log("crash_test: triggering intentional SIGSEGV now!");
        volatile int *bad = NULL;
        *bad = 42;
        return true;
    }
    return false;
}

static const aurade_ext_info_t EXT_INFO = {
    .name = "Crash Test",
    .version = "1.0",
    .author = "AuraDE",
    .description = "Press Super+Shift+X to crash DE and test Safe Mode"
};

const aurade_ext_info_t *aurade_ext_meta(void) {
    return &EXT_INFO;
}

bool aurade_ext_init(const aurade_ext_api_t *api) {
    if (!api || api->api_version != AURADE_EXT_API_VERSION) return false;
    g_api = api;
    api->hook_key(on_key, NULL);
    api->log("crash_test loaded: press Super+Shift+X to test Safe Mode");
    return true;
}
