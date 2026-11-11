CC ?= gcc
CFLAGS ?= -Wall -Wextra -O3 -march=native -pipe -g -D_GNU_SOURCE
INCLUDES = -I../src $(shell pkg-config --cflags gtk4 json-c)
LIBS = $(shell pkg-config --libs gtk4 json-c) -lm

SRCS = main.c pages.c ui_helpers.c ../src/config/config.c ../src/gestures/physics.c
OBJS = $(SRCS:.c=.o)
TARGET = aurade-settings

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LIBS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f main.o pages.o ui_helpers.o $(TARGET)

.PHONY: all clean
