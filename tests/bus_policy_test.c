#include "bus_policy.h"
#include <stdint.h>
#include <stdio.h>

static int passed;
static int failed;

static void expect_value(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "%s: expected 0x%08X, got 0x%08X\n",
                name, expected, actual);
        failed++;
        return;
    }
    passed++;
}

int main(void)
{
    expect_value("open-bus-8", bus_unmapped_load(0x1F900000u, 8), 0xFFu);
    expect_value("open-bus-16", bus_unmapped_load(0x1F900000u, 16), 0xFFFFu);
    expect_value("open-bus-32", bus_unmapped_load(0x1F900000u, 32), 0xFFFFFFFFu);
    bus_unmapped_store(0x1F900000u, 0x12345678u, 8);
    bus_unmapped_store(0x1F900000u, 0x12345678u, 16);
    bus_unmapped_store(0x1F900000u, 0x12345678u, 32);
    passed += 3;

    printf("bus policy tests: pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
