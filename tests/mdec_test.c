#include "mdec.h"
#include <stdint.h>
#include <stdio.h>

#define STATUS_DATA_OUT_EMPTY (1u << 31)
#define STATUS_BUSY (1u << 29)

static int passed;
static int failed;

#define EXPECT_EQ(actual, expected)                                      \
    do                                                                   \
    {                                                                    \
        uint32_t got_ = (uint32_t)(actual);                              \
        uint32_t expected_ = (uint32_t)(expected);                       \
        if (got_ != expected_)                                           \
        {                                                                \
            fprintf(stderr, "%s:%d expected 0x%08X got 0x%08X\n",        \
                    __FILE__, __LINE__, expected_, got_);                 \
            failed++;                                                    \
        }                                                                \
        else                                                             \
        {                                                                \
            passed++;                                                    \
        }                                                                \
    } while (0)

static void test_incremental_output(void)
{
    static Mdec mdec;
    mdec_init(&mdec);

    const uint32_t decode_8bit_two_words =
        (1u << 29) | (1u << 27) | 2u;
    mdec_store32(&mdec, 0, decode_8bit_two_words);

    mdec_store32(&mdec, 0, 0xFE000000u);
    EXPECT_EQ(mdec.receiving, 1);
    EXPECT_EQ(mdec.words_remaining, 1);
    EXPECT_EQ(mdec.out_count, 16);

    uint32_t status = mdec_load32(&mdec, 4);
    EXPECT_EQ((status & STATUS_BUSY) != 0, 1);
    EXPECT_EQ((status & STATUS_DATA_OUT_EMPTY) != 0, 0);

    for (int i = 0; i < 16; i++)
        EXPECT_EQ(mdec_load32(&mdec, 0), 0x80808080u);

    status = mdec_load32(&mdec, 4);
    EXPECT_EQ((status & STATUS_DATA_OUT_EMPTY) != 0, 0);
    status = mdec_load32(&mdec, 4);
    EXPECT_EQ((status & STATUS_DATA_OUT_EMPTY) != 0, 1);

    mdec_store32(&mdec, 0, 0xFE00FE00u);
    status = mdec_load32(&mdec, 4);
    EXPECT_EQ((status & STATUS_BUSY) != 0, 0);
}

int main(void)
{
    test_incremental_output();
    printf("mdec tests: pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
