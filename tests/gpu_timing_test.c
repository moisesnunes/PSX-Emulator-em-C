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

static void test_gpustat_readiness(void)
{
    Gpu gpu;
    gpu_init(&gpu, NULL);

    uint32_t status = gpu_status(&gpu);
    EXPECT_EQ((status >> 26) & 1u, 1);
    EXPECT_EQ((status >> 27) & 1u, 0);
    EXPECT_EQ((status >> 28) & 1u, 1);

    gpu.dma_direction = DMA_DIR_CPU_TO_GP0;
    gpu_add_busy_cycles(&gpu, 12);
    status = gpu_status(&gpu);
    EXPECT_EQ((status >> 25) & 1u, 0);
    EXPECT_EQ((status >> 26) & 1u, 0);
    EXPECT_EQ((status >> 28) & 1u, 0);

    gpu_step(&gpu, 11);
    EXPECT_EQ(gpu_busy_cycles(&gpu), 1);
    EXPECT_EQ((gpu_status(&gpu) >> 26) & 1u, 0);
    gpu_step(&gpu, 1);
    EXPECT_EQ(gpu_busy_cycles(&gpu), 0);
    EXPECT_EQ((gpu_status(&gpu) >> 25) & 1u, 1);
    EXPECT_EQ((gpu_status(&gpu) >> 26) & 1u, 1);
    EXPECT_EQ((gpu_status(&gpu) >> 28) & 1u, 1);

    gpu.gp0_mode = GP0_MODE_IMAGE_STORE;
    gpu.image_store_words_remaining = 4;
    gpu.dma_direction = DMA_DIR_VRAM_TO_CPU;
    status = gpu_status(&gpu);
    EXPECT_EQ((status >> 25) & 1u, 1);
    EXPECT_EQ((status >> 26) & 1u, 0);
    EXPECT_EQ((status >> 27) & 1u, 1);
    EXPECT_EQ((status >> 28) & 1u, 0);

    gpu.image_store_words_remaining = 0;
    status = gpu_status(&gpu);
    EXPECT_EQ((status >> 25) & 1u, 0);
    EXPECT_EQ((status >> 27) & 1u, 0);

    gpu_destroy(&gpu);
}

static void test_gp0_command_costs(void)
{
    Gpu gpu;
    gpu_init(&gpu, NULL);

    gpu_gp0(&gpu, 0x02000000u);
    gpu_gp0(&gpu, 0);
    gpu_gp0(&gpu, (16u << 16) | 32u);
    EXPECT_EQ(gpu_busy_cycles(&gpu), 119);

    gpu_gp0(&gpu, 0x20000000u);
    gpu_gp0(&gpu, 0);
    gpu_gp0(&gpu, 0);
    gpu_gp0(&gpu, 0);
    EXPECT_EQ(gpu_busy_cycles(&gpu), 142);

    gpu_gp1(&gpu, 0x01000000u);
    EXPECT_EQ(gpu_busy_cycles(&gpu), 0);

    gpu_destroy(&gpu);
}

static void test_texture_disable_permission(void)
{
    Gpu gpu;
    gpu_init(&gpu, NULL);

    gpu_gp0(&gpu, 0xE1000800u);
    EXPECT_EQ((gpu_status(&gpu) >> 15) & 1u, 0);

    gpu_gp1(&gpu, 0x09000001u);
    gpu_gp0(&gpu, 0xE1000800u);
    EXPECT_EQ((gpu_status(&gpu) >> 15) & 1u, 1);

    gpu_gp1(&gpu, 0x09000000u);
    EXPECT_EQ((gpu_status(&gpu) >> 15) & 1u, 1);
    gpu_gp0(&gpu, 0xE1000800u);
    EXPECT_EQ((gpu_status(&gpu) >> 15) & 1u, 0);

    gpu_destroy(&gpu);
}

static void test_gpu_info_and_mask_status(void)
{
    Gpu gpu;
    gpu_init(&gpu, NULL);

    gpu_gp1(&gpu, 0x10000007u);
    EXPECT_EQ(gpu_read(&gpu), 2);

    gpu_gp0(&gpu, 0xE6000003u);
    EXPECT_EQ((gpu_status(&gpu) >> 11) & 3u, 3);
    gpu_gp0(&gpu, 0xE6000000u);
    EXPECT_EQ((gpu_status(&gpu) >> 11) & 3u, 0);

    gpu_destroy(&gpu);
}

static void test_interlace_field_changes_at_vblank(void)
{
    Gpu gpu;
    gpu_init(&gpu, NULL);
    gpu.interlaced = true;
    gpu.field = FIELD_TOP;

    gpu_vblank_start(&gpu);
    EXPECT_EQ(gpu.field, FIELD_BOTTOM);
    gpu_vblank_start(&gpu);
    EXPECT_EQ(gpu.field, FIELD_TOP);

    gpu.interlaced = false;
    gpu_vblank_start(&gpu);
    EXPECT_EQ(gpu.field, FIELD_TOP);
    gpu_destroy(&gpu);
}

static void test_hblank_boundaries(void)
{
    Gpu gpu;
    gpu_init(&gpu, NULL);

    uint32_t active_cycles = gpu_cycles_until_timing_boundary(&gpu);
    EXPECT_EQ(gpu_in_hblank(&gpu), 0);
    GpuTimingEvents events = gpu_step(&gpu, active_cycles);
    EXPECT_EQ(events.hblank_count, 1);
    EXPECT_EQ(gpu_in_hblank(&gpu), 1);

    uint32_t blank_cycles = gpu_cycles_until_timing_boundary(&gpu);
    EXPECT_EQ(active_cycles + blank_cycles, 2147);
    events = gpu_step(&gpu, blank_cycles);
    EXPECT_EQ(events.hblank_count, 0);
    EXPECT_EQ(gpu_in_hblank(&gpu), 0);
    EXPECT_EQ(gpu.scanline, 1);

    gpu_destroy(&gpu);
}

static void test_pal_hblank_boundaries(void)
{
    Gpu gpu;
    gpu_init(&gpu, NULL);
    gpu.vmode = VMODE_PAL;

    uint32_t active_cycles = gpu_cycles_until_timing_boundary(&gpu);
    GpuTimingEvents events = gpu_step(&gpu, active_cycles);
    EXPECT_EQ(events.hblank_count, 1);
    EXPECT_EQ(gpu_in_hblank(&gpu), 1);

    uint32_t blank_cycles = gpu_cycles_until_timing_boundary(&gpu);
    EXPECT_EQ(active_cycles + blank_cycles, 2158);
    events = gpu_step(&gpu, blank_cycles);
    EXPECT_EQ(gpu_in_hblank(&gpu), 0);
    EXPECT_EQ(gpu.scanline, 1);

    gpu_destroy(&gpu);
}

int main(void)
{
    test_ntsc_frame();
    test_pal_frame();
    test_gpustat_readiness();
    test_gp0_command_costs();
    test_texture_disable_permission();
    test_gpu_info_and_mask_status();
    test_interlace_field_changes_at_vblank();
    test_hblank_boundaries();
    test_pal_hblank_boundaries();
    printf("gpu timing tests: pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
