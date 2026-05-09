#ifndef PSX_RASTER_H
#define PSX_RASTER_H

#include "vram.h"

/* Área de clipping e offset de desenho */
typedef struct {
    int32_t x_min, y_min;
    int32_t x_max, y_max;
    int32_t x_off, y_off;
} DrawArea;

/* Contexto de texturização — onde na VRAM buscar a textura */
typedef struct {
    uint32_t page_x;   /* base X da página de textura (unidades de 64 pixels) */
    uint32_t page_y;   /* base Y (0 ou 256) */
    uint8_t  mode;     /* 0=4bpp  1=8bpp  2=15bpp */
    uint32_t clut_x;   /* endereço X do CLUT na VRAM */
    uint32_t clut_y;   /* endereço Y do CLUT na VRAM */
} TexContext;

Color tex_sample_raw(TexContext tc, const uint16_t *data, uint8_t u, uint8_t v);

/* Fill incondicional da VRAM (GP0 0x02) */
void fill_vram_rect(Vram *vram,
                    int32_t x, int32_t y, int32_t w, int32_t h,
                    Color color);

/* Retângulos */
void draw_rect_flat(Vram *vram, const DrawArea *clip,
                    int32_t x, int32_t y, int32_t w, int32_t h,
                    Color color, int apply_offset);
void draw_rect_textured(Vram *vram, const DrawArea *clip,
                        int32_t x, int32_t y, int32_t w, int32_t h,
                        uint8_t u0, uint8_t v0,
                        TexContext tc, Color flat_color, int modulate);

/* Triângulos */
void draw_triangle_flat(Vram *vram, const DrawArea *clip, Vertex v[3]);
void draw_triangle_gouraud(Vram *vram, const DrawArea *clip, Vertex v[3]);
void draw_triangle_textured(Vram *vram, const DrawArea *clip,
                             Vertex v[3], TexCoord uv[3],
                             TexContext tc, Color flat_color, int modulate);

/* Quads (divididos em dois triângulos: v0,v1,v2 e v1,v3,v2) */
void draw_quad_flat(Vram *vram, const DrawArea *clip, Vertex v[4]);
void draw_quad_gouraud(Vram *vram, const DrawArea *clip, Vertex v[4]);
void draw_quad_textured(Vram *vram, const DrawArea *clip,
                        Vertex v[4], TexCoord uv[4],
                        TexContext tc, Color flat_color, int modulate);

#endif /* PSX_RASTER_H */
