#ifndef PSX_MAP_H
#define PSX_MAP_H

#include <stdint.h>

typedef struct
{
    uint32_t start;
    uint32_t length;
} MemRange;

static inline int memrange_contains(MemRange r, uint32_t addr, uint32_t *offset)
{
    if (addr >= r.start && addr < r.start + r.length)
    {
        *offset = addr - r.start;
        return 1;
    }
    return 0;
}

static const MemRange MAP_BIOS = {0x1fc00000, 512 * 1024};
static const MemRange MAP_RAM = {0x00000000, 2 * 1024 * 1024};
static const MemRange MAP_IRQ_CONTROL = {0x1f801070, 8};
static const MemRange MAP_DMA = {0x1f801080, 0x80};
static const MemRange MAP_GPU = {0x1f801810, 8};
static const MemRange MAP_TIMERS = {0x1f801100, 0x30};
static const MemRange MAP_CDROM = {0x1f801800, 4};
static const MemRange MAP_SPU = {0x1f801c00, 0x280};
static const MemRange MAP_MEM_CTRL = {0x1f801000, 0x24};
static const MemRange MAP_RAM_SIZE = {0x1f801060, 4};
static const MemRange MAP_CACHE_CTRL = {0xfffe0130, 4};
static const MemRange MAP_EXPANSION1 = {0x1f000000, 0x800000};
static const MemRange MAP_EXPANSION2 = {0x1f802000, 0x42};

/* Strip virtual address region bits to get physical address */
static inline uint32_t mask_region(uint32_t addr)
{
    static const uint32_t REGION_MASK[8] = {
        0xffffffff,
        0xffffffff,
        0xffffffff,
        0xffffffff, /* KUSEG */
        0x7fffffff, /* KSEG0 */
        0x1fffffff, /* KSEG1 */
        0xffffffff,
        0xffffffff, /* KSEG2 */
    };
    return addr & REGION_MASK[addr >> 29];
}

#endif /* PSX_MAP_H */
