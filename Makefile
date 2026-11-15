CC ?= gcc
PREFIX ?= /usr/local
DATADIR ?= /usr/share
CFLAGS ?= -Wall -Wextra -O3 -march=native -pipe -g -D_GNU_SOURCE -DWLR_USE_UNSTABLE -MMD -MP

PKG_PATHS := $(CURDIR)/deps/local/lib/pkgconfig:$(CURDIR)/deps/local/lib64/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:$(HOME)/.local/lib/pkgconfig:/home/user/AI/local/lib/pkgconfig:$(PKG_CONFIG_PATH)
PKGS := scenefx-0.5 wlroots-0.20 wayland-server xkbcommon cairo pangocairo json-c libinput libdrm librsvg-2.0 gio-2.0 gio-unix-2.0 xcb xcb-icccm xcb-ewmh pixman-1

SKIP_DEP_CHECK_GOALS := clean deps uninstall settings
ifeq ($(filter $(SKIP_DEP_CHECK_GOALS),$(MAKECMDGOALS)),)
ifneq ($(shell PKG_CONFIG_PATH="$(PKG_PATHS)" pkg-config --exists $(PKGS) && echo ok),ok)
$(info Missing required pkg-config packages:)
$(info $(shell PKG_CONFIG_PATH="$(PKG_PATHS)" pkg-config --print-errors --exists $(PKGS) 2>&1))
$(error Run 'make deps' to build scenefx-0.5 locally, or install the missing *-dev packages first.)
endif
endif

INCLUDES = -Isrc $(shell PKG_CONFIG_PATH="$(PKG_PATHS)" pkg-config --cflags $(PKGS))
LIBS = $(shell PKG_CONFIG_PATH="$(PKG_PATHS)" pkg-config --libs $(PKGS)) -Wl,-rpath,$(CURDIR)/deps/local/lib -Wl,-rpath,$(PREFIX)/lib -Wl,-rpath,/home/user/AI/local/lib -lm -lpthread -lpam

SRCS = src/main.c \
       src/gestures/physics.c \
       src/gestures/gesture_engine.c \
       src/config/config.c \
       src/render/cairo_buffer.c \
       src/util/apps.c \
       src/shell/wallpaper.c \
       src/shell/topbar.c \
       src/shell/dock.c \
       src/shell/mission_control.c \
       src/shell/launchpad.c \
       src/shell/control_center.c \
       src/shell/osd.c \
       src/shell/screenshot.c \
       src/shell/notifications.c \
       src/shell/music_pill.c \
       src/shell/lockscreen.c \
       src/window/xdg_shell.c \
       src/window/workspace.c \
       src/window/xwayland.c

OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)
TARGET = aurade

# Offline Control Center renderer: `make preview` writes build/preview/*.png
# without needing a compositor or a display.
PREVIEW = tools/cc_preview
PREVIEW_SRCS = tools/cc_preview.c src/shell/control_center.c \
               src/render/cairo_buffer.c src/util/apps.c

SETTINGS = settings/aurade-settings

all: $(TARGET) $(SETTINGS)

deps:
	./scripts/install-deps.sh

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LIBS) -o $(TARGET)

$(SETTINGS): src/config/config.o src/gestures/physics.o settings/main.c settings/pages.c settings/ui_helpers.c settings/settings.h settings/css.h
	$(MAKE) -C settings

$(PREVIEW): $(PREVIEW_SRCS) src/shell/control_center.h src/render/cairo_buffer.h
	$(CC) $(CFLAGS) $(INCLUDES) $(PREVIEW_SRCS) $(LIBS) -o $(PREVIEW)

# Deterministic gesture-recogniser tests (no touchpad required).
GESTURE_SIM = tools/gesture_sim

$(GESTURE_SIM): tools/gesture_sim.c src/gestures/gesture_engine.c src/gestures/gesture_engine.h
	$(CC) $(CFLAGS) $(INCLUDES) tools/gesture_sim.c src/gestures/gesture_engine.c $(LIBS) -o $(GESTURE_SIM)

test-gestures: $(GESTURE_SIM)
	./$(GESTURE_SIM)

preview: $(PREVIEW)
	./$(PREVIEW) build/preview

settings: $(SETTINGS)

install: all
	install -Dm755 $(TARGET) "$(DESTDIR)$(PREFIX)/bin/$(TARGET)"
	install -Dm755 $(SETTINGS) "$(DESTDIR)$(PREFIX)/bin/aurade-settings"
	install -Dm755 session/aurade-session "$(DESTDIR)$(PREFIX)/bin/aurade-session"
	install -Dm644 session/aurade.desktop "$(DESTDIR)$(DATADIR)/wayland-sessions/aurade.desktop"
	install -Dm644 settings/aurade-settings.desktop "$(DESTDIR)$(PREFIX)/share/applications/aurade-settings.desktop"
	@if ls "$(CURDIR)/deps/local/lib/libscenefx-0.5.so"* >/dev/null 2>&1; then \
		install -d "$(DESTDIR)$(PREFIX)/lib"; \
		cp -a "$(CURDIR)/deps/local/lib/libscenefx-0.5.so"* "$(DESTDIR)$(PREFIX)/lib/"; \
		if [ -z "$(DESTDIR)" ] && [ "$$(id -u)" = "0" ]; then ldconfig; fi; \
	fi

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(TARGET)" \
	      "$(DESTDIR)$(PREFIX)/bin/aurade-settings" \
	      "$(DESTDIR)$(PREFIX)/bin/aurade-session" \
	      "$(DESTDIR)$(DATADIR)/wayland-sessions/aurade.desktop" \
	      "$(DESTDIR)$(PREFIX)/share/applications/aurade-settings.desktop"

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) $(PREVIEW) $(GESTURE_SIM)
	$(MAKE) -C settings clean

# Rebuild objects when any header they include changes (e.g. server.h layout).
-include $(DEPS)

.PHONY: all clean deps install uninstall preview test-gestures settings
