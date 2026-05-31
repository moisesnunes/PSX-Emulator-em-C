#include "ram.h"
#include <string.h>

void ram_init(Ram *ram)
{
    memset(ram->data, 0xCA, RAM_SIZE);
}

uint32_t ram_load32(const Ram *ram, uint32_t offset)
{
    return (uint32_t)ram->data[offset] | ((uint32_t)ram->data[offset + 1] << 8) | ((uint32_t)ram->data[offset + 2] << 16) | ((uint32_t)ram->data[offset + 3] << 24);
}

uint16_t ram_load16(const Ram *ram, uint32_t offset)
{
    return (uint16_t)ram->data[offset] | ((uint16_t)ram->data[offset + 1] << 8);
}

uint8_t ram_load8(const Ram *ram, uint32_t offset)
{
    return ram->data[offset];
}

void ram_store32(Ram *ram, uint32_t offset, uint32_t val)
{
    ram->data[offset] = (uint8_t)(val);
    ram->data[offset + 1] = (uint8_t)(val >> 8);
    ram->data[offset + 2] = (uint8_t)(val >> 16);
    ram->data[offset + 3] = (uint8_t)(val >> 24);
}

void ram_store16(Ram *ram, uint32_t offset, uint16_t val)
{
    ram->data[offset] = (uint8_t)(val);
    ram->data[offset + 1] = (uint8_t)(val >> 8);
}

void ram_store8(Ram *ram, uint32_t offset, uint8_t val)
{
    ram->data[offset] = val;
}
