#include "gte.h"

#include <stdio.h>

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_EQ(a, b)                                                \
    do                                                                 \
    {                                                                  \
        if ((uint32_t)(a) != (uint32_t)(b))                            \
        {                                                              \
            fprintf(stderr, "  FAIL %s:%d  expected 0x%08X got 0x%08X\n", \
                    __FILE__, __LINE__, (unsigned)(b), (unsigned)(a)); \
            g_fail++;                                                  \
        }                                                              \
        else                                                           \
        {                                                              \
            g_pass++;                                                  \
        }                                                              \
    } while (0)

static void test_color_registers(void)
{
    printf("test_color_registers ... ");
    Gte gte;
    gte_init(&gte);

    gte_write_data(&gte, 28, 0x00007FFF);
    EXPECT_EQ(gte_read_data(&gte, 9),  0x00000F80u);
    EXPECT_EQ(gte_read_data(&gte, 10), 0x00000F80u);
    EXPECT_EQ(gte_read_data(&gte, 11), 0x00000F80u);
    EXPECT_EQ(gte_read_data(&gte, 29), 0x00007FFFu);
    printf("ok\n");
}

static void test_lzcr(void)
{
    printf("test_lzcr ... ");
    Gte gte;
    gte_init(&gte);

    gte_write_data(&gte, 30, 0x00000001);
    EXPECT_EQ(gte_read_data(&gte, 31), 31u);
    gte_write_data(&gte, 30, 0xFFFFFFFF);
    EXPECT_EQ(gte_read_data(&gte, 31), 32u);
    printf("ok\n");
}

static void test_flag_and_ctrl(void)
{
    printf("test_flag_and_ctrl ... ");
    Gte gte;
    gte_init(&gte);

    gte_write_ctrl(&gte, 31, 0xFFFFFFFF);
    EXPECT_EQ(gte_read_ctrl(&gte, 31), 0xFFFFF000u);

    gte_write_ctrl(&gte, 4, 0x00008000);
    EXPECT_EQ(gte_read_ctrl(&gte, 4), 0xFFFF8000u);
    printf("ok\n");
}

static void test_nclip(void)
{
    printf("test_nclip ... ");
    Gte gte;
    gte_init(&gte);

    gte_write_data(&gte, 12, 0x00000000);
    gte_write_data(&gte, 13, 0x00000001);
    gte_write_data(&gte, 14, 0x00010000);
    gte_execute(&gte, 0x06);
    EXPECT_EQ(gte_read_data(&gte, 24), 1u);
    printf("ok\n");
}

static void test_rtps_divide_flag(void)
{
    printf("test_rtps_divide_flag ... ");
    Gte gte;
    gte_init(&gte);

    gte_write_ctrl(&gte, 7, 1);        /* TRZ */
    gte_write_ctrl(&gte, 26, 0xFFFF);  /* H */
    gte_execute(&gte, 0x01);           /* RTPS */
    EXPECT_EQ(gte_read_ctrl(&gte, 31) & (1u << 17), 1u << 17);
    EXPECT_EQ(gte_read_ctrl(&gte, 31) & (1u << 31), 1u << 31);
    printf("ok\n");
}

int main(void)
{
    printf("=== gte unit tests ===\n");
    test_color_registers();
    test_lzcr();
    test_flag_and_ctrl();
    test_nclip();
    test_rtps_divide_flag();
    printf("======================\n");
    printf("pass: %d  fail: %d\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
