CC           = gcc
CFLAGS       = -std=c11 -Wall -Wextra -O2 $(shell sdl2-config --cflags)
DEBUG_CFLAGS = -std=c11 -Wall -Wextra -O0 -g -fsanitize=address,undefined $(shell sdl2-config --cflags)
LDFLAGS      = $(shell sdl2-config --libs) -lGL -lGLEW
DEBUG_LDFLAGS= $(LDFLAGS) -fsanitize=address,undefined

BIOS_PATH ?= bios/BIOS.ROM

SRCS = src/main.c         \
       src/cpu.c          \
       src/interconnect.c \
       src/bios.c         \
       src/ram.c          \
       src/dma.c          \
       src/channel.c      \
       src/gpu.c          \
       src/renderer.c     \
       src/spu.c          \
       src/log.c          \
       src/irq.c          \
       src/scheduler.c    \
       src/timer.c        \
       src/exe.c          \
       src/disc.c         \
       src/cdrom.c

OBJS       = $(SRCS:.c=.o)
DEBUG_OBJS = $(SRCS:.c=.debug.o)
TARGET     = ps1_boot

.PHONY: all clean run debug smoke test-cdrom

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.debug.o: %.c
	$(CC) $(DEBUG_CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET) --bios $(BIOS_PATH)

debug: $(DEBUG_OBJS)
	$(CC) $(DEBUG_OBJS) $(DEBUG_LDFLAGS) -o $(TARGET)_debug
	ASAN_OPTIONS=detect_leaks=0 ./$(TARGET)_debug --bios $(BIOS_PATH)

smoke: $(TARGET)
	./$(TARGET) --bios $(BIOS_PATH) --headless --max-instructions 500000

test-cdrom: src/cdrom.c tests/cdrom_test.c
	$(CC) -std=c11 -Wall -Wextra -O2 -Isrc \
	    tests/cdrom_test.c src/cdrom.c \
	    -o tests/cdrom_test
	./tests/cdrom_test

clean:
	rm -f $(OBJS) $(DEBUG_OBJS) $(TARGET) $(TARGET)_debug tests/cdrom_test
