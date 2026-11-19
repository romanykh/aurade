CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -fPIC -shared $(shell pkg-config --cflags cairo pangocairo xkbcommon)
LIBS = $(shell pkg-config --libs cairo pangocairo xkbcommon) -lpthread
OUT_DIR = $(HOME)/.local/share/aurade/extensions

all: clipboard.so crash_test.so

clipboard.so: clipboard_ext.c ../src/ext/aurade_ext.h
	$(CC) $(CFLAGS) clipboard_ext.c $(LIBS) -o clipboard.so

crash_test.so: crash_test_ext.c ../src/ext/aurade_ext.h
	$(CC) $(CFLAGS) crash_test_ext.c -o crash_test.so

install: all
	install -Dm755 clipboard.so "$(OUT_DIR)/clipboard.so"
	install -Dm755 crash_test.so "$(OUT_DIR)/crash_test.so"

clean:
	rm -f clipboard.so crash_test.so

.PHONY: all install clean
