#ifndef PSX_RAM_H
#define PSX_RAM_H

#include <stdint.h>

#define RAM_SIZE (2 * 1024 * 1024)

typedef struct
{
    uint8_t data[RAM_SIZE];
} Ram;

void ram_init(Ram *ram);
uint32_t ram_load32(const Ram *ram, uint32_t offset);
uint16_t ram_load16(const Ram *ram, uint32_t offset);
uint8_t ram_load8(const Ram *ram, uint32_t offset);
void ram_store32(Ram *ram, uint32_t offset, uint32_t val);
void ram_store16(Ram *ram, uint32_t offset, uint16_t val);
void ram_store8(Ram *ram, uint32_t offset, uint8_t val);

#endif /* PSX_RAM_H */
