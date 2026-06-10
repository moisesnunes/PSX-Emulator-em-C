#include "cpu_cache.h"
#include <stdint.h>
#include <stdio.h>

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

int main(void)
{
    CpuICache cache;
    uint32_t value = 0;
    const uint32_t words[4] = {
        0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u
    };

    cpu_icache_init(&cache);
    EXPECT_EQ(cpu_icache_is_cacheable(0x80001000u), 1);
    EXPECT_EQ(cpu_icache_is_cacheable(0xA0001000u), 0);
    EXPECT_EQ(cpu_icache_is_cacheable(0xBFC00000u), 0);
    EXPECT_EQ(cpu_icache_lookup(&cache, 0x80001000u, &value), 0);

    cpu_icache_fill(&cache, 0x80001000u, words);
    EXPECT_EQ(cpu_icache_lookup(&cache, 0x80001000u, &value), 1);
    EXPECT_EQ(value, 0x11111111u);
    EXPECT_EQ(cpu_icache_lookup(&cache, 0x8000100Cu, &value), 1);
    EXPECT_EQ(value, 0x44444444u);
    EXPECT_EQ(cpu_icache_lookup(&cache, 0x00001004u, &value), 1);
    EXPECT_EQ(value, 0x22222222u);

    cpu_icache_init(&cache);
    EXPECT_EQ(cpu_icache_lookup(&cache, 0x80001000u, &value), 0);

    printf("cpu cache tests: pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
