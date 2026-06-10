#include "bus_timing.h"
#include <stdint.h>
#include <stdio.h>

static int passed;
static int failed;

static void expect_cycles(const char *name, uint32_t addr, unsigned width,
                          uint32_t expected)
{
    uint32_t actual = bus_access_cycles(addr, width);
    if (actual != expected)
    {
        fprintf(stderr, "%s: expected %u got %u\n", name, expected, actual);
        failed++;
        return;
    }
    passed++;
}

int main(void)
{
    expect_cycles("ram-kseg0", 0x80000000u, 4, 5);
    expect_cycles("ram-kseg1", 0xA0000000u, 4, 5);
    expect_cycles("scratchpad", 0x1F800000u, 4, 2);
    expect_cycles("bios-byte", 0xBFC00000u, 1, 8);
    expect_cycles("bios-half", 0xBFC00000u, 2, 13);
    expect_cycles("bios-word", 0xBFC00000u, 4, 25);
    expect_cycles("expansion2-word", 0x1F802000u, 4, 56);
    expect_cycles("cdrom-word", 0x1F801800u, 4, 26);
    expect_cycles("spu-half", 0x1F801DAAu, 2, 18);
    expect_cycles("spu-word", 0x1F801DA8u, 4, 39);
    expect_cycles("internal-io", 0x1F801070u, 4, 3);
    expect_cycles("cache-control", 0xFFFE0130u, 4, 2);

    printf("bus timing tests: pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
