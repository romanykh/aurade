CC = gcc
CFLAGS = -Wall -Wextra -O3 -march=native -pipe -g -D_GNU_SOURCE -DWLR_USE_UNSTABLE -MMD -MP
INCLUDES = -Isrc $(shell pkg-config --cflags wlroots-0.20 wayland-server xkbcommon cairo pangocairo json-c libinput libdrm librsvg-2.0 gio-2.0 gio-unix-2.0 xcb xcb-icccm xcb-ewmh pixman-1)
LIBS = $(shell pkg-config --libs wlroots-0.20 wayland-server xkbcommon cairo pangocairo json-c libinput libdrm librsvg-2.0 gio-2.0 gio-unix-2.0 xcb xcb-icccm xcb-ewmh pixman-1) -lm -lpthread

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

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LIBS) -o $(TARGET)

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

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) $(PREVIEW) $(GESTURE_SIM)

# Rebuild objects when any header they include changes (e.g. server.h layout).
-include $(DEPS)

.PHONY: all clean preview test-gestures
