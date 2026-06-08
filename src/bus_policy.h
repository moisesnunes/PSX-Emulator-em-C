#pragma once
#include <stdint.h>

uint32_t bus_unmapped_load(uint32_t addr, unsigned width_bits);
void bus_unmapped_store(uint32_t addr, uint32_t value, unsigned width_bits);
