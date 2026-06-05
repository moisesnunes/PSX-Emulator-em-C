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

int main(void)
{
    printf("=== spu unit tests ===\n");
    test_key_on_voice_masks();
    test_key_off_voice_masks();
    test_reverb_voice_masks();
    test_cd_audio_fifo();
    test_cd_audio_fifo_overflow();
    printf("======================\n");
    printf("pass: %d  fail: %d\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
