#pragma once
#include <stdint.h>

#define BIOS_SIZE (512 * 1024)

typedef struct
{
    uint8_t data[BIOS_SIZE];
} Bios;

int bios_init(Bios *bios, const char *path);
uint32_t bios_load32(const Bios *bios, uint32_t offset);
uint16_t bios_load16(const Bios *bios, uint32_t offset);
uint8_t bios_load8(const Bios *bios, uint32_t offset);
