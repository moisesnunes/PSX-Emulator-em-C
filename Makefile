CC           = gcc
CFLAGS       = -std=c11 -Wall -Wextra -O2 -fno-strict-aliasing $(shell sdl2-config --cflags)
DEBUG_CFLAGS = -std=c11 -Wall -Wextra -O0 -g -fsanitize=address,undefined $(shell sdl2-config --cflags)
LDFLAGS      = $(shell sdl2-config --libs) -lGL -lGLEW -lm
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
       src/mdec.c         \
       src/renderer.c     \
       src/spu.c          \
       src/log.c          \
       src/irq.c          \
       src/scheduler.c    \
       src/timer.c        \
       src/exe.c          \
       src/disc.c         \
       src/cdrom.c        \
       src/sio.c          \
       src/gte.c

OBJS       = $(SRCS:.c=.o)
DEBUG_OBJS = $(SRCS:.c=.debug.o)
TARGET     = ps1_boot
PSX_TEST_RUNNER = python3 tests/run_psx_tests.py
PSX_TEST_ARGS ?=

.PHONY: all clean run debug smoke test-cdrom test-sio test-gte test-dma \
        test-psx-list test-psx-all test-psx-cdrom test-psx-cpu test-psx-dma \
        test-psx-gpu test-psx-gte test-psx-gte-fuzz test-psx-input \
        test-psx-mdec test-psx-spu test-psx-timer-dump test-psx-timers \
        test-psx-psxtest-cpu test-psx-psxtest-cpx test-psx-psxtest-gpu \
        test-psx-psxtest-gte test-psx-resolution test-psx-extras

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

test-sio: src/sio.c tests/sio_test.c
	$(CC) -std=c11 -Wall -Wextra -O2 -Isrc $(shell sdl2-config --cflags) \
	    tests/sio_test.c src/sio.c \
	    -o tests/sio_test
	./tests/sio_test

test-gte: src/gte.c tests/gte_test.c
	$(CC) -std=c11 -Wall -Wextra -O2 -Isrc \
	    tests/gte_test.c src/gte.c \
	    -o tests/gte_test
	./tests/gte_test

test-dma: src/dma.c src/channel.c tests/dma_test.c
	$(CC) -std=c11 -Wall -Wextra -O2 -Isrc \
	    tests/dma_test.c src/dma.c src/channel.c \
	    -o tests/dma_test
	./tests/dma_test

test-psx-list:
	$(PSX_TEST_RUNNER) --list $(PSX_TEST_ARGS)

test-psx-all: $(TARGET)
	$(PSX_TEST_RUNNER) --keep-going $(PSX_TEST_ARGS)

test-psx-cdrom: $(TARGET)
	$(PSX_TEST_RUNNER) --category cdrom --keep-going $(PSX_TEST_ARGS)

test-psx-cpu: $(TARGET)
	$(PSX_TEST_RUNNER) --category cpu --keep-going $(PSX_TEST_ARGS)

test-psx-dma: $(TARGET)
	$(PSX_TEST_RUNNER) --category dma --keep-going $(PSX_TEST_ARGS)

test-psx-gpu: $(TARGET)
	$(PSX_TEST_RUNNER) --category gpu --keep-going $(PSX_TEST_ARGS)

test-psx-gte: $(TARGET)
	$(PSX_TEST_RUNNER) --category gte --keep-going $(PSX_TEST_ARGS)

test-psx-gte-fuzz: $(TARGET)
	$(PSX_TEST_RUNNER) --category gte-fuzz --keep-going $(PSX_TEST_ARGS)

test-psx-input: $(TARGET)
	$(PSX_TEST_RUNNER) --category input --keep-going $(PSX_TEST_ARGS)

test-psx-mdec: $(TARGET)
	$(PSX_TEST_RUNNER) --category mdec --keep-going $(PSX_TEST_ARGS)

test-psx-spu: $(TARGET)
	$(PSX_TEST_RUNNER) --category spu --keep-going $(PSX_TEST_ARGS)

test-psx-timer-dump: $(TARGET)
	$(PSX_TEST_RUNNER) --category timer-dump --keep-going $(PSX_TEST_ARGS)

test-psx-timers: $(TARGET)
	$(PSX_TEST_RUNNER) --category timers --keep-going $(PSX_TEST_ARGS)

test-psx-psxtest-cpu: $(TARGET)
	$(PSX_TEST_RUNNER) --category psxtest_cpu --keep-going $(PSX_TEST_ARGS)

test-psx-psxtest-cpx: $(TARGET)
	$(PSX_TEST_RUNNER) --category psxtest_cpx --keep-going $(PSX_TEST_ARGS)

test-psx-psxtest-gpu: $(TARGET)
	$(PSX_TEST_RUNNER) --category psxtest_gpu --keep-going $(PSX_TEST_ARGS)

test-psx-psxtest-gte: $(TARGET)
	$(PSX_TEST_RUNNER) --category psxtest_gte --keep-going $(PSX_TEST_ARGS)

test-psx-resolution: $(TARGET)
	$(PSX_TEST_RUNNER) --category resolution --keep-going $(PSX_TEST_ARGS)

test-psx-extras: $(TARGET)
	$(PSX_TEST_RUNNER) --category psxtest_cpu --category psxtest_cpx \
	    --category psxtest_gpu --category psxtest_gte --category resolution \
	    --keep-going $(PSX_TEST_ARGS)

clean:
	rm -f $(OBJS) $(DEBUG_OBJS) $(TARGET) $(TARGET)_debug tests/cdrom_test tests/sio_test tests/gte_test tests/dma_test
