#include "cpu_cache.h"
#include <string.h>

#define RAM_MIRROR_END 0x00800000u

static uint32_t physical_address(uint32_t addr)
{
    return addr & 0x1FFFFFFFu;
}

static CpuICacheLine *cache_line(CpuICache *cache, uint32_t addr)
{
    return &cache->lines[(physical_address(addr) >> 4) &
                         (CPU_ICACHE_LINE_COUNT - 1u)];
}

static const CpuICacheLine *cache_line_const(const CpuICache *cache, uint32_t addr)
{
    return &cache->lines[(physical_address(addr) >> 4) &
                         (CPU_ICACHE_LINE_COUNT - 1u)];
}

void cpu_icache_init(CpuICache *cache)
{
    memset(cache, 0, sizeof(*cache));
}

bool cpu_icache_is_cacheable(uint32_t addr)
{
    return addr < 0xA0000000u && physical_address(addr) < RAM_MIRROR_END;
}

bool cpu_icache_lookup(const CpuICache *cache, uint32_t addr, uint32_t *value)
{
    if (!cpu_icache_is_cacheable(addr))
        return false;

    const CpuICacheLine *line = cache_line_const(cache, addr);
    uint32_t tag = physical_address(addr) & ~0xFu;
    if (!line->valid || line->tag != tag)
        return false;

    *value = line->words[(addr >> 2) & 3u];
    return true;
}

void cpu_icache_fill(CpuICache *cache, uint32_t line_addr,
                     const uint32_t words[CPU_ICACHE_WORDS_PER_LINE])
{
    CpuICacheLine *line = cache_line(cache, line_addr);
    line->tag = physical_address(line_addr) & ~0xFu;
    memcpy(line->words, words, sizeof(line->words));
    line->valid = true;
}
