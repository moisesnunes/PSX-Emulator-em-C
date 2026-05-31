#include "bios.h"
#include <stdio.h>

int bios_init(Bios *bios, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(bios->data, 1, BIOS_SIZE, f);
    fclose(f);
    if (n != BIOS_SIZE)
        return -1;
    return 0;
}

uint32_t bios_load32(const Bios *bios, uint32_t offset)
{
    return (uint32_t)bios->data[offset] | ((uint32_t)bios->data[offset + 1] << 8) | ((uint32_t)bios->data[offset + 2] << 16) | ((uint32_t)bios->data[offset + 3] << 24);
}

uint8_t bios_load8(const Bios *bios, uint32_t offset)
{
    return bios->data[offset];
}
