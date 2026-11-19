# AuraDE Extensions SDK

Розширення AuraDE — це нативні модулі (`.so`), які можуть додавати нові елементи інтерфейсу (панелі з розмиттям, віджети на верхньому барі) або змінювати поведінку композитора (перехоплювати клавіші, кліки, події вікон, керувати вікнами та оболонкою).

---

## 1. Де зберігаються модулі

При запуску `aurade` автоматично завантажує всі `*.so` файли з:
1. Директорії зі змінної середовища `AURADE_EXT_DIR` (якщо задана)
2. `~/.local/share/aurade/extensions/`
3. `/usr/local/share/aurade/extensions/`

---

## 2. Мінімальний приклад розширення

Підключіть заголовок `../src/ext/aurade_ext.h` і реалізуйте `aurade_ext_init`:

```c
#include "../src/ext/aurade_ext.h"
#include <xkbcommon/xkbcommon-keysyms.h>

static const aurade_ext_api_t *g_api = NULL;

static bool on_key(uint32_t sym, uint32_t mods, bool pressed, void *ud) {
    (void)ud;
    // mods: 1 = Super, 2 = Shift, 4 = Ctrl, 8 = Alt
    if (pressed && (mods & 1u) && sym == XKB_KEY_F9) {
        g_api->toggle_control_center();
        return true; // перехопити подію
    }
    return false;
}

static const aurade_ext_info_t META = {
    .name = "My Extension",
    .version = "1.0",
    .author = "Author",
    .description = "Example AuraDE module"
};

const aurade_ext_info_t *aurade_ext_meta(void) { return &META; }

bool aurade_ext_init(const aurade_ext_api_t *api) {
    if (!api || api->api_version != AURADE_EXT_API_VERSION) return false;
    g_api = api;
    api->hook_key(on_key, NULL);
    api->log("My Extension loaded!");
    return true;
}

void aurade_ext_deinit(void) {
    // Звільнення ресурсів при завершенні DE
}
```

---

## 3. Компіляція та встановлення

```bash
gcc -Wall -Wextra -O2 -fPIC -shared my_ext.c \
    $(pkg-config --cflags --libs cairo pangocairo xkbcommon) \
    -o my_ext.so

mkdir -p ~/.local/share/aurade/extensions
cp my_ext.so ~/.local/share/aurade/extensions/
```

Готові приклади в цій папці:
- `clipboard_ext.c` — історія буфера обміну та скопійованих файлів (`Super+V` або кнопка на топбарі).
- `crash_test_ext.c` — тестовий модуль для перевірки Safe Mode (`Super+Shift+X`).

Збірка та встановлення прикладів:
```bash
make -C extensions install
```

---

## 4. Доступне API (`aurade_ext_api_t`)

| Група | Функції |
|---|---|
| **Система** | `log`, `get_screen_size`, `is_dark_mode`, `exec` |
| **Нові UI-панелі** | `panel_create`, `panel_set_pos`, `panel_set_visible`, `panel_redraw`, `panel_destroy` |
| **Хуки подій** | `hook_tick`, `hook_key`, `hook_click`, `hook_topbar_draw`, `hook_window_open`, `hook_window_close`, `hook_window_focus` |
| **Вікна та DE** | `window_move_resize`, `window_focus`, `window_close`, `topbar_redraw`, `toggle_control_center`, `toggle_notifications`, `toggle_launchpad`, `lock_screen`, `set_wallpaper`, `osd_show_volume`, `osd_show_brightness` |

---

## 5. Захист від збоїв (Safe Mode)

Якщо під час виконання розширення стається фатальний збій (`SIGSEGV`, `SIGABRT`, `SIGFPE`, `SIGBUS`, `SIGILL`), обробник сигналів фіксує проблемний модуль у `~/.config/aurade/ext_disabled`.

При наступному запуску `aurade` **автоматично вимикає всі розширення**, щоб не допустити циклічного крашу робочого столу.

**Як увімкнути розширення знову:**
- Відкрийте **System Settings → Extensions** і натисніть **Re-enable Extensions**, або
- Видаліть файл `~/.config/aurade/ext_disabled` вручну та перезапустіть `aurade`.
