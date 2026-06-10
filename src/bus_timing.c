#include "bus_timing.h"
#include <stdbool.h>

static uint32_t physical_address(uint32_t addr)
{
    static const uint32_t region_mask[8] = {
        0xFFFFFFFFu,
        0xFFFFFFFFu,
        0xFFFFFFFFu,
        0xFFFFFFFFu,
        0x7FFFFFFFu,
        0x1FFFFFFFu,
        0xFFFFFFFFu,
        0xFFFFFFFFu,
    };
    return addr & region_mask[addr >> 29];
}

static bool in_range(uint32_t addr, uint32_t start, uint32_t length)
{
    return addr >= start && addr < start + length;
}

static uint32_t by_width(unsigned width_bytes, uint32_t byte_cycles,
                         uint32_t half_cycles, uint32_t word_cycles)
{
    if (width_bytes == 1)
        return byte_cycles;
    if (width_bytes == 2)
        return half_cycles;
    return word_cycles;
}

uint32_t bus_access_cycles(uint32_t addr, unsigned width_bytes)
{
    uint32_t abs = physical_address(addr);

    if (in_range(abs, 0x00000000u, 8u * 1024u * 1024u))
        return 5;
    if (in_range(abs, 0x1F800000u, 1024u))
        return 2;
    if (in_range(abs, 0x1FC00000u, 512u * 1024u))
        return by_width(width_bytes, 8, 13, 25);
    if (in_range(abs, 0x1F000000u, 512u * 1024u))
        return by_width(width_bytes, 7, 14, 26);
    if (in_range(abs, 0x1F802000u, 66u))
        return by_width(width_bytes, 11, 26, 56);
    if (in_range(abs, 0x1FA00000u, 2u * 1024u * 1024u))
        return by_width(width_bytes, 7, 6, 10);
    if (in_range(abs, 0x1F801800u, 4u))
        return by_width(width_bytes, 8, 14, 26);
    if (in_range(abs, 0x1F801C00u, 640u))
        return by_width(width_bytes, 18, 18, 39);
    if (in_range(abs, 0xFFFE0130u, 4u))
        return 2;

    /* Most internal I/O registers complete in roughly three CPU cycles. */
    if (in_range(abs, 0x1F801000u, 0x1000u))
        return 3;
    return 3;
}
