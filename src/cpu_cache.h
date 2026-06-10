#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CPU_ICACHE_LINE_COUNT 256u
#define CPU_ICACHE_WORDS_PER_LINE 4u

typedef struct
{
    uint32_t tag;
    uint32_t words[CPU_ICACHE_WORDS_PER_LINE];
    bool valid;
} CpuICacheLine;

typedef struct
{
    CpuICacheLine lines[CPU_ICACHE_LINE_COUNT];
} CpuICache;

void cpu_icache_init(CpuICache *cache);
bool cpu_icache_is_cacheable(uint32_t addr);
bool cpu_icache_lookup(const CpuICache *cache, uint32_t addr, uint32_t *value);
void cpu_icache_fill(CpuICache *cache, uint32_t line_addr,
                     const uint32_t words[CPU_ICACHE_WORDS_PER_LINE]);
