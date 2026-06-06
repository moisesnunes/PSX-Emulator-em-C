#include "spu.h"

#include <stdio.h>

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_EQ(a, b)                                                \
    do                                                                 \
    {                                                                  \
        if ((uint32_t)(a) != (uint32_t)(b))                            \
        {                                                              \
            fprintf(stderr, "  FAIL %s:%d  expected %u got %u\n",      \
                    __FILE__, __LINE__, (unsigned)(b), (unsigned)(a)); \
            g_fail++;                                                  \
        }                                                              \
        else                                                           \
        {                                                              \
            g_pass++;                                                  \
        }                                                              \
    } while (0)

static void clear_voice_runtime(Spu *spu)
{
    for (int i = 0; i < NUM_VOICES; i++)
    {
        spu->voices[i].keyed_on = false;
        spu->voices[i].reverb_enabled = false;
    }
}

static void test_key_on_voice_masks(void)
{
    printf("test_key_on_voice_masks ... ");
    Spu spu;
    spu_init(&spu, false);
    clear_voice_runtime(&spu);

    spu_store(&spu, 0, 0x0188, 0x8000);
    EXPECT_EQ(spu.voices[15].keyed_on, 1u);
    EXPECT_EQ(spu.voices[16].keyed_on, 0u);

    clear_voice_runtime(&spu);
    spu_store(&spu, 0, 0x018A, 0x0001);
    EXPECT_EQ(spu.voices[15].keyed_on, 0u);
    EXPECT_EQ(spu.voices[16].keyed_on, 1u);
    printf("ok\n");
}

static void test_key_off_voice_masks(void)
{
    printf("test_key_off_voice_masks ... ");
    Spu spu;
    spu_init(&spu, false);
    for (int i = 0; i < NUM_VOICES; i++)
        spu.voices[i].keyed_on = true;

    spu_store(&spu, 0, 0x018C, 0x8000);
    EXPECT_EQ(spu.voices[15].keyed_on, 0u);
    EXPECT_EQ(spu.voices[16].keyed_on, 1u);

    for (int i = 0; i < NUM_VOICES; i++)
        spu.voices[i].keyed_on = true;
    spu_store(&spu, 0, 0x018E, 0x0001);
    EXPECT_EQ(spu.voices[15].keyed_on, 1u);
    EXPECT_EQ(spu.voices[16].keyed_on, 0u);
    printf("ok\n");
}

static void test_reverb_voice_masks(void)
{
    printf("test_reverb_voice_masks ... ");
    Spu spu;
    spu_init(&spu, false);
    clear_voice_runtime(&spu);

    spu_store(&spu, 0, 0x0198, 0x8000);
    EXPECT_EQ(spu.voices[15].reverb_enabled, 1u);
    EXPECT_EQ(spu.voices[16].reverb_enabled, 0u);

    clear_voice_runtime(&spu);
    spu_store(&spu, 0, 0x019A, 0x0001);
    EXPECT_EQ(spu.voices[15].reverb_enabled, 0u);
    EXPECT_EQ(spu.voices[16].reverb_enabled, 1u);
    printf("ok\n");
}

static void test_cd_audio_fifo(void)
{
    printf("test_cd_audio_fifo ... ");
    Spu spu;
    spu_init(&spu, false);

    spu_push_cd_audio_frame(&spu, 1000, -1000);
    EXPECT_EQ(spu.cd_audio_count, 1u);
    spu_clock(&spu);
    EXPECT_EQ(spu.cd_audio_count, 0u);

    spu_push_cd_audio_frame(&spu, 1, 2);
    spu_clear_cd_audio(&spu);
    EXPECT_EQ(spu.cd_audio_count, 0u);
    printf("ok\n");
}

static void test_cd_audio_fifo_overflow(void)
{
    printf("test_cd_audio_fifo_overflow ... ");
    Spu spu;
    spu_init(&spu, false);

    for (uint32_t i = 0; i < SPU_CD_AUDIO_BUFFER_FRAMES + 4u; i++)
        spu_push_cd_audio_frame(&spu, (int16_t)i, (int16_t)-i);
    EXPECT_EQ(spu.cd_audio_count, (uint32_t)SPU_CD_AUDIO_BUFFER_FRAMES);
    printf("ok\n");
}

static void test_noise_enable(void)
{
    printf("test_noise_enable ... ");
    Spu spu;
    spu_init(&spu, false);
    clear_voice_runtime(&spu);

    spu_store(&spu, 0, 0x0194, 0x0003); /* NON lo: voices 0 and 1 */
    EXPECT_EQ(spu.voices[0].noise_enabled, 1u);
    EXPECT_EQ(spu.voices[1].noise_enabled, 1u);
    EXPECT_EQ(spu.voices[2].noise_enabled, 0u);

    spu_store(&spu, 0, 0x0196, 0x0001); /* NON hi: voice 16 */
    EXPECT_EQ(spu.voices[16].noise_enabled, 1u);
    EXPECT_EQ(spu.voices[17].noise_enabled, 0u);
    printf("ok\n");
}

static void test_pmon_enable(void)
{
    printf("test_pmon_enable ... ");
    Spu spu;
    spu_init(&spu, false);
    clear_voice_runtime(&spu);

    spu_store(&spu, 0, 0x0190, 0x0004); /* PMON lo: voice 2 */
    EXPECT_EQ(spu.voices[0].pitch_mod_enabled, 0u); /* bit 0 always ignored */
    EXPECT_EQ(spu.voices[2].pitch_mod_enabled, 1u);
    EXPECT_EQ(spu.voices[3].pitch_mod_enabled, 0u);

    spu_store(&spu, 0, 0x0192, 0x0002); /* PMON hi: voice 17 */
    EXPECT_EQ(spu.voices[17].pitch_mod_enabled, 1u);
    EXPECT_EQ(spu.voices[16].pitch_mod_enabled, 0u);
    printf("ok\n");
}

static void test_spustat_mirrors_spucnt(void)
{
    printf("test_spustat_mirrors_spucnt ... ");
    Spu spu;
    spu_init(&spu, false);

    /* Write SPUCNT with some bits set */
    spu_store(&spu, 0, 0x01AA, 0x8021); /* bit 5 (reverb), bit 0 (CD enable) */
    spu_clock(&spu);                     /* SPUSTAT is updated during clock */
    uint16_t stat = spu_load(&spu, 0, 0x01AE);
    /* Lower 6 bits of SPUCNT should appear in SPUSTAT */
    EXPECT_EQ(stat & 0x003F, 0x0021u);
    printf("ok\n");
}

static void test_endx_set_on_loop_end(void)
{
    printf("test_endx_set_on_loop_end ... ");
    Spu spu;
    spu_init(&spu, false);
    clear_voice_runtime(&spu);

    /* Manually mark voice 3 as having reached loop end */
    spu.voices[3].reached_loop_end = true;
    spu.voices[3].keyed_on = false;

    spu_clock(&spu);

    uint16_t endx_lo = spu_load(&spu, 0, 0x019C);
    EXPECT_EQ(endx_lo & (1u << 3), (uint32_t)(1u << 3));
    EXPECT_EQ(endx_lo & (1u << 4), 0u);
    printf("ok\n");
}

static void test_noise_produces_different_samples(void)
{
    printf("test_noise_produces_different_samples ... ");
    Spu spu;
    spu_init(&spu, false);
    /* Set noise frequency */
    spu_store(&spu, 0, 0x01AA, 0x0100); /* shift=0, step=1 */

    uint32_t prev = spu.noise_level;
    uint32_t changes = 0;
    for (int i = 0; i < 100; i++)
    {
        spu_clock(&spu);
        if (spu.noise_level != prev)
        {
            changes++;
            prev = spu.noise_level;
        }
    }
    EXPECT_EQ(changes > 0u, 1u);
    printf("ok\n");
}

static void test_dma_read_write(void)
{
    printf("test_dma_read_write ... ");
    Spu spu;
    spu_init(&spu, false);

    spu.sound_ram_start_address = 0x100;
    spu_dma_write(&spu, 0x44332211);
    EXPECT_EQ(spu.sound_ram[0x100], 0x11u);
    EXPECT_EQ(spu.sound_ram[0x101], 0x22u);
    EXPECT_EQ(spu.sound_ram[0x102], 0x33u);
    EXPECT_EQ(spu.sound_ram[0x103], 0x44u);
    EXPECT_EQ(spu.sound_ram_start_address, 0x104u);

    spu.sound_ram_start_address = 0x100;
    EXPECT_EQ(spu_dma_read(&spu), 0x44332211u);
    EXPECT_EQ(spu.sound_ram_start_address, 0x104u);
    printf("ok\n");
}

static void test_dma_wrap(void)
{
    printf("test_dma_wrap ... ");
    Spu spu;
    spu_init(&spu, false);

    spu.sound_ram_start_address = SOUND_RAM_SIZE - 2u;
    spu_dma_write(&spu, 0xDDCCBBAA);
    EXPECT_EQ(spu.sound_ram[SOUND_RAM_SIZE - 2u], 0xAAu);
    EXPECT_EQ(spu.sound_ram[SOUND_RAM_SIZE - 1u], 0xBBu);
    EXPECT_EQ(spu.sound_ram[0], 0xCCu);
    EXPECT_EQ(spu.sound_ram[1], 0xDDu);

    spu.sound_ram_start_address = SOUND_RAM_SIZE - 2u;
    EXPECT_EQ(spu_dma_read(&spu), 0xDDCCBBAAu);
    EXPECT_EQ(spu.sound_ram_start_address, 2u);
    printf("ok\n");
}

int main(void)
{
    printf("=== spu unit tests ===\n");
    test_key_on_voice_masks();
    test_key_off_voice_masks();
    test_reverb_voice_masks();
    test_cd_audio_fifo();
    test_cd_audio_fifo_overflow();
    test_noise_enable();
    test_pmon_enable();
    test_spustat_mirrors_spucnt();
    test_endx_set_on_loop_end();
    test_noise_produces_different_samples();
    test_dma_read_write();
    test_dma_wrap();
    printf("======================\n");
    printf("pass: %d  fail: %d\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
