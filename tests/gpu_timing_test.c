#include "gpu.h"
#include <stdint.h>
#include <stdio.h>

static int passed;
static int failed;

#define EXPECT_EQ(actual, expected)                                      \
    do                                                                   \
    {                                                                    \
        uint64_t got_ = (uint64_t)(actual);                              \
        uint64_t expected_ = (uint64_t)(expected);                       \
        if (got_ != expected_)                                           \
        {                                                                \
            fprintf(stderr, "%s:%d expected %llu got %llu\n",            \
                    __FILE__, __LINE__,                                  \
                    (unsigned long long)expected_,                        \
                    (unsigned long long)got_);                            \
            failed++;                                                    \
        }                                                                \
        else                                                             \
        {                                                                \
            passed++;                                                    \
        }                                                                \
    } while (0)

static void test_ntsc_frame(void)
{
    Gpu gpu;
    gpu_init(&gpu, NULL);

    GpuTimingEvents events = gpu_step(&gpu, 33868800u / 60u);
    EXPECT_EQ(events.hblank_count, 263);
    EXPECT_EQ(events.vblank_started, 1);
    EXPECT_EQ(events.frame_ended, 1);
    EXPECT_EQ(gpu.frames, 1);
    EXPECT_EQ(gpu.scanline, 0);
    EXPECT_EQ(events.dotclock_ticks, (33868800u / 60u) * 11u / 70u);

    gpu_destroy(&gpu);
}

static void test_pal_frame(void)
{
    Gpu gpu;
    gpu_init(&gpu, NULL);
    gpu.vmode = VMODE_PAL;

    GpuTimingEvents events = gpu_step(&gpu, 33868800u / 50u);
    EXPECT_EQ(events.hblank_count, 314);
    EXPECT_EQ(events.vblank_started, 1);
    EXPECT_EQ(events.frame_ended, 1);
    EXPECT_EQ(gpu.frames, 1);
    EXPECT_EQ(gpu.scanline, 0);

    gpu_destroy(&gpu);
}

int main(void)
{
    test_ntsc_frame();
    test_pal_frame();
    printf("gpu timing tests: pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
