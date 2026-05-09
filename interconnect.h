#ifndef PSX_INTERCONNECT_H
#define PSX_INTERCONNECT_H

#include "bios.h"
#include "ram.h"

typedef struct
{
    Bios bios;
    Ram ram;
} Interconnect;

void inter_init(Interconnect *inter, Bios bios);
uint32_t inter_load32(const Interconnect *inter, uint32_t addr);
uint16_t inter_load16(const Interconnect *inter, uint32_t addr);
uint8_t inter_load8(const Interconnect *inter, uint32_t addr);
void inter_store32(Interconnect *inter, uint32_t addr, uint32_t val);
void inter_store16(Interconnect *inter, uint32_t addr, uint16_t val);
void inter_store8(Interconnect *inter, uint32_t addr, uint8_t val);

#endif /* PSX_INTERCONNECT_H */
