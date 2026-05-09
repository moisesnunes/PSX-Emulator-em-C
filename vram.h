#ifndef PSX_VRAM_H
#define PSX_VRAM_H

#include <stdint.h>

#define VRAM_W 1024
#define VRAM_H 512

/* 1 MB organizado como 1024×512 pixels de 16 bits (BGR555) */
typedef struct {
    uint16_t data[VRAM_W * VRAM_H];
} Vram;

/* Cor RGB extraída dos comandos GP0 (formato 0x00BBGGRR) */
typedef struct {
    uint8_t r, g, b;
} Color;

/* Vértice com posição inteira e cor (para Gouraud shading) */
typedef struct {
    int32_t x, y;
    int32_t r, g, b;  /* i32 para facilitar interpolação */
} Vertex;

/* Coordenada de textura (U, V — cada 8 bits) */
typedef struct {
    uint8_t u, v;
} TexCoord;

void     vram_init(Vram *vram);
uint16_t vram_get(const Vram *vram, int32_t x, int32_t y);
void     vram_set(Vram *vram, int32_t x, int32_t y, uint16_t val);
uint16_t vram_load16(const Vram *vram, uint32_t x, uint32_t y);
void     vram_store16(Vram *vram, uint32_t x, uint32_t y, uint16_t val);
void     vram_dump_ppm(const Vram *vram, const char *path,
                       uint16_t ox, uint16_t oy, uint16_t w, uint16_t h);

Color    color_from_cmd(uint32_t w);
uint16_t color_to_bgr555(Color c);
int32_t  sign_ext11(uint32_t v);
Vertex   vertex_new(uint32_t pos, Color c);

#endif /* PSX_VRAM_H */
