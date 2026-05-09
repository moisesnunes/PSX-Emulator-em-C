#include "vram.h"
#include <string.h>
#include <stdio.h>

void vram_init(Vram *vram)
{
    memset(vram->data, 0, sizeof(vram->data));
}

uint16_t vram_get(const Vram *vram, int32_t x, int32_t y)
{
    uint32_t xi = (uint32_t)x & (VRAM_W - 1);
    uint32_t yi = (uint32_t)y & (VRAM_H - 1);
    return vram->data[yi * VRAM_W + xi];
}

void vram_set(Vram *vram, int32_t x, int32_t y, uint16_t val)
{
    uint32_t xi = (uint32_t)x & (VRAM_W - 1);
    uint32_t yi = (uint32_t)y & (VRAM_H - 1);
    vram->data[yi * VRAM_W + xi] = val;
}

uint16_t vram_load16(const Vram *vram, uint32_t x, uint32_t y)
{
    return vram_get(vram, (int32_t)x, (int32_t)y);
}

void vram_store16(Vram *vram, uint32_t x, uint32_t y, uint16_t val)
{
    vram_set(vram, (int32_t)x, (int32_t)y, val);
}

void vram_dump_ppm(const Vram *vram, const char *path,
                   uint16_t ox, uint16_t oy, uint16_t w, uint16_t h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[VRAM] Cannot open %s\n", path); return; }
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
    for (int row = oy; row < oy + h; row++) {
        for (int col = ox; col < ox + w; col++) {
            uint16_t p = vram_get(vram, col, row);
            uint8_t r = (uint8_t)((p & 0x1f) << 3);
            uint8_t g = (uint8_t)(((p >> 5) & 0x1f) << 3);
            uint8_t b = (uint8_t)(((p >> 10) & 0x1f) << 3);
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
}

Color color_from_cmd(uint32_t w)
{
    Color c;
    c.r = (uint8_t)w;
    c.g = (uint8_t)(w >> 8);
    c.b = (uint8_t)(w >> 16);
    return c;
}

uint16_t color_to_bgr555(Color c)
{
    return (uint16_t)(((uint16_t)c.r >> 3)
                    | (((uint16_t)c.g >> 3) << 5)
                    | (((uint16_t)c.b >> 3) << 10));
}

int32_t sign_ext11(uint32_t v)
{
    if (v & 0x400u)
        return (int32_t)(v | 0xfffff800u);
    return (int32_t)v;
}

Vertex vertex_new(uint32_t pos, Color c)
{
    Vertex vx;
    vx.x = sign_ext11(pos & 0x7ffu);
    vx.y = sign_ext11((pos >> 16) & 0x7ffu);
    vx.r = c.r;
    vx.g = c.g;
    vx.b = c.b;
    return vx;
}
