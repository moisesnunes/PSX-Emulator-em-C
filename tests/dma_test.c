#include "dma.h"
#include "log.h"
#include <stdio.h>

static int g_pass;
static int g_fail;

#define EXPECT_TRUE(x)                                            \
    do                                                            \
    {                                                             \
        if (x)                                                    \
            g_pass++;                                             \
        else                                                      \
        {                                                         \
            g_fail++;                                             \
            printf("\nFAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        }                                                         \
    } while (0)

#define EXPECT_EQ(a, b)                                          \
    do                                                           \
    {                                                            \
        unsigned long long _a = (unsigned long long)(a);         \
        unsigned long long _b = (unsigned long long)(b);         \
        if (_a == _b)                                            \
            g_pass++;                                            \
        else                                                     \
        {                                                        \
            g_fail++;                                            \
            printf("\nFAIL %s:%d: got 0x%llX expected 0x%llX\n", \
                   __FILE__, __LINE__, _a, _b);                  \
        }                                                        \
    } while (0)

void log_enable(LogSubsystem s) { (void)s; }
bool log_is_enabled(LogSubsystem s)
{
    (void)s;
    return false;
}
void log_msg(LogSubsystem s, const char *fmt, ...)
{
    (void)s;
    (void)fmt;
}

static void test_dicr_ack_channel6(void)
{
    printf("test_dicr_ack_channel6 ... ");
    Dma dma;
    dma_init(&dma);

    dma_set_interrupt(&dma, (1u << 23) | (1u << (16 + PORT_OTC)));
    dma_mark_channel_done(&dma, PORT_OTC);

    EXPECT_TRUE(dma_irq_pending(&dma));
    EXPECT_EQ((dma_interrupt(&dma) >> 24) & 0x7Fu, 0x40u);

    dma_set_interrupt(&dma, (1u << 23) | (1u << (16 + PORT_OTC)) |
                                (1u << (24 + PORT_OTC)));

    EXPECT_TRUE(!dma_irq_pending(&dma));
    EXPECT_EQ((dma_interrupt(&dma) >> 24) & 0x7Fu, 0x00u);
    printf("ok\n");
}

int main(void)
{
    printf("=== dma unit tests ===\n");
    test_dicr_ack_channel6();
    printf("======================\n");
    printf("pass: %d  fail: %d\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
