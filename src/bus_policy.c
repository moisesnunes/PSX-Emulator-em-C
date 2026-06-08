#include "bus_policy.h"
#include <stdio.h>

#define UNMAPPED_LOG_LIMIT 16u

static unsigned unmapped_read_logs;
static unsigned unmapped_write_logs;

static uint32_t open_bus_value(unsigned width_bits)
{
    switch (width_bits)
    {
    case 8:
        return 0xFFu;
    case 16:
        return 0xFFFFu;
    default:
        return 0xFFFFFFFFu;
    }
}

static void report_suppressed(const char *operation)
{
    fprintf(stderr, "Further unmapped bus %s logs suppressed\n", operation);
}

uint32_t bus_unmapped_load(uint32_t addr, unsigned width_bits)
{
    if (unmapped_read_logs < UNMAPPED_LOG_LIMIT)
    {
        fprintf(stderr, "Unmapped load%u: %08X (open bus)\n", width_bits, addr);
        unmapped_read_logs++;
        if (unmapped_read_logs == UNMAPPED_LOG_LIMIT)
            report_suppressed("read");
    }
    return open_bus_value(width_bits);
}

void bus_unmapped_store(uint32_t addr, uint32_t value, unsigned width_bits)
{
    if (unmapped_write_logs < UNMAPPED_LOG_LIMIT)
    {
        fprintf(stderr, "Unmapped store%u: %08X = %08X (ignored)\n",
                width_bits, addr, value);
        unmapped_write_logs++;
        if (unmapped_write_logs == UNMAPPED_LOG_LIMIT)
            report_suppressed("write");
    }
}
