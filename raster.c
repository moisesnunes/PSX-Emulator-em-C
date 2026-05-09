/* ============================================================
 * RASTERIZADOR SOFTWARE DA GPU
 * ============================================================
 * Algoritmo de triângulo: half-space (edge functions)
 *   Para cada pixel no bounding box, calcula o sinal de três
 *   cross-products. Se todos >= 0 (ou todos <= 0), o pixel
 *   está dentro do triângulo.
 *
 * Gouraud shading: interpola as cores dos 3 vértices usando
 *   coordenadas baricêntricas.
 */

#include "raster.h"
#include <stddef.h>

/* ---- helpers internos --------------------------------------------- */

static inline int32_t i32_min3(int32_t a, int32_t b, int32_t c)
{
    int32_t m = a < b ? a : b;
    return m < c ? m : c;
}
static inline int32_t i32_max3(int32_t a, int32_t b, int32_t c)
{
    int32_t m = a > b ? a : b;
    return m > c ? m : c;
}
static inline int32_t i32_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Função de aresta: cross-product do segmento v0→v1 com ponto p */
static inline int32_t edge(int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1,
                            int32_t px, int32_t py)
{
    return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
}

static inline Color bgr555_to_color(uint16_t p)
{
    Color c;
    c.r = (uint8_t)((p & 0x1f) << 3);
    c.g = (uint8_t)(((p >> 5)  & 0x1f) << 3);
    c.b = (uint8_t)(((p >> 10) & 0x1f) << 3);
    return c;
}

/* ---- tex_sample_raw ----------------------------------------------- */

Color tex_sample_raw(TexContext tc, const uint16_t *data, uint8_t u, uint8_t v)
{
    size_t base_x = (size_t)tc.page_x * 64;
    size_t base_y = (size_t)tc.page_y;

    switch (tc.mode) {
    case 0: { /* 4bpp — índice em nibble */
        size_t   tx  = base_x + (size_t)u / 4;
        size_t   ty  = base_y + (size_t)v;
        uint16_t raw = data[ty * VRAM_W + (tx & (VRAM_W - 1))];
        unsigned nib = ((unsigned)u % 4u) * 4u;
        size_t   idx = (raw >> nib) & 0xfu;
        size_t   cx  = (tc.clut_x + idx) & (VRAM_W - 1);
        return bgr555_to_color(data[tc.clut_y * VRAM_W + cx]);
    }
    case 1: { /* 8bpp — índice em byte */
        size_t   tx  = base_x + (size_t)u / 2;
        size_t   ty  = base_y + (size_t)v;
        uint16_t raw = data[ty * VRAM_W + (tx & (VRAM_W - 1))];
        size_t   idx = (u & 1u) ? (raw >> 8) : (raw & 0xffu);
        size_t   cx  = (tc.clut_x + idx) & (VRAM_W - 1);
        return bgr555_to_color(data[tc.clut_y * VRAM_W + cx]);
    }
    default: { /* 15bpp direto */
        size_t tx = (base_x + (size_t)u) & (VRAM_W - 1);
        size_t ty = base_y + (size_t)v;
        return bgr555_to_color(data[ty * VRAM_W + tx]);
    }
    }
}

/* ---- fill_vram_rect (GP0 0x02) ------------------------------------ */

void fill_vram_rect(Vram *vram, int32_t x, int32_t y,
                    int32_t w, int32_t h, Color color)
{
    uint16_t pixel = color_to_bgr555(color);
    int32_t  x_end = x + w; if (x_end > (int32_t)VRAM_W) x_end = (int32_t)VRAM_W;
    int32_t  y_end = y + h; if (y_end > (int32_t)VRAM_H) y_end = (int32_t)VRAM_H;
    for (int32_t row = y; row < y_end; row++)
        for (int32_t col = x; col < x_end; col++)
            vram_set(vram, col, row, pixel);
}

/* ---- draw_rect_flat ----------------------------------------------- */

void draw_rect_flat(Vram *vram, const DrawArea *clip,
                    int32_t x, int32_t y, int32_t w, int32_t h,
                    Color color, int apply_offset)
{
    int32_t ox  = apply_offset ? clip->x_off : 0;
    int32_t oy  = apply_offset ? clip->y_off : 0;
    int32_t px  = x + ox, py = y + oy;
    uint16_t pixel = color_to_bgr555(color);
    int32_t x0  = px; if (x0 < clip->x_min) x0 = clip->x_min;
    int32_t x1  = px + w - 1; if (x1 > clip->x_max) x1 = clip->x_max;
    int32_t y0  = py; if (y0 < clip->y_min) y0 = clip->y_min;
    int32_t y1  = py + h - 1; if (y1 > clip->y_max) y1 = clip->y_max;
    for (int32_t row = y0; row <= y1; row++)
        for (int32_t col = x0; col <= x1; col++)
            vram_set(vram, col, row, pixel);
}

/* ---- draw_rect_textured ------------------------------------------- */

void draw_rect_textured(Vram *vram, const DrawArea *clip,
                        int32_t x, int32_t y, int32_t w, int32_t h,
                        uint8_t u0, uint8_t v0,
                        TexContext tc, Color flat_color, int modulate)
{
    const uint16_t *vd = vram->data;
    int32_t ox = clip->x_off, oy = clip->y_off;
    int32_t px = x + ox, py = y + oy;
    int32_t xs = px; if (xs < clip->x_min) xs = clip->x_min;
    int32_t xe = px + w - 1; if (xe > clip->x_max) xe = clip->x_max;
    int32_t ys = py; if (ys < clip->y_min) ys = clip->y_min;
    int32_t ye = py + h - 1; if (ye > clip->y_max) ye = clip->y_max;

    for (int32_t row = ys; row <= ye; row++) {
        uint8_t tv = (uint8_t)(v0 + (uint8_t)(row - py));
        for (int32_t col = xs; col <= xe; col++) {
            uint8_t tu = (uint8_t)(u0 + (uint8_t)(col - px));
            Color tex = tex_sample_raw(tc, vd, tu, tv);
            if (tex.r == 0 && tex.g == 0 && tex.b == 0) continue;
            uint16_t pixel;
            if (modulate) {
                Color mc;
                mc.r = (uint8_t)(((uint16_t)tex.r * flat_color.r) >> 7);
                mc.g = (uint8_t)(((uint16_t)tex.g * flat_color.g) >> 7);
                mc.b = (uint8_t)(((uint16_t)tex.b * flat_color.b) >> 7);
                pixel = color_to_bgr555(mc);
            } else {
                pixel = color_to_bgr555(tex);
            }
            vram_set(vram, col, row, pixel);
        }
    }
}

/* ---- núcleo do rasterizador de triângulos ------------------------- */

/* Calcula bounding box com clip e área orientada. Retorna 0 se degenerado. */
static int tri_bbox(const DrawArea *clip,
                    int32_t x0, int32_t y0,
                    int32_t x1, int32_t y1,
                    int32_t x2, int32_t y2,
                    int32_t *min_x, int32_t *max_x,
                    int32_t *min_y, int32_t *max_y,
                    int32_t *area2)
{
    *min_x = i32_max3(i32_min3(x0, x1, x2), clip->x_min, clip->x_min);
    *max_x = i32_min3(i32_max3(x0, x1, x2), clip->x_max, clip->x_max);
    *min_y = i32_max3(i32_min3(y0, y1, y2), clip->y_min, clip->y_min);
    *max_y = i32_min3(i32_max3(y0, y1, y2), clip->y_max, clip->y_max);
    if (*min_x > *max_x || *min_y > *max_y) return 0;
    *area2 = edge(x0, y0, x1, y1, x2, y2);
    return (*area2 != 0);
}

/* ---- draw_triangle_flat ------------------------------------------- */

void draw_triangle_flat(Vram *vram, const DrawArea *clip, Vertex v[3])
{
    Color fc; fc.r = (uint8_t)v[0].r; fc.g = (uint8_t)v[0].g; fc.b = (uint8_t)v[0].b;
    uint16_t pixel = color_to_bgr555(fc);
    int32_t ox = clip->x_off, oy = clip->y_off;
    int32_t x0 = v[0].x + ox, y0 = v[0].y + oy;
    int32_t x1 = v[1].x + ox, y1 = v[1].y + oy;
    int32_t x2 = v[2].x + ox, y2 = v[2].y + oy;
    int32_t min_x, max_x, min_y, max_y, area2;
    if (!tri_bbox(clip, x0, y0, x1, y1, x2, y2,
                  &min_x, &max_x, &min_y, &max_y, &area2)) return;

    for (int32_t py = min_y; py <= max_y; py++) {
        for (int32_t px = min_x; px <= max_x; px++) {
            int32_t w0 = edge(x1, y1, x2, y2, px, py);
            int32_t w1 = edge(x2, y2, x0, y0, px, py);
            int32_t w2 = edge(x0, y0, x1, y1, px, py);
            int inside = (area2 > 0) ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                                     : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (inside) vram_set(vram, px, py, pixel);
        }
    }
}

/* ---- draw_triangle_gouraud ---------------------------------------- */

void draw_triangle_gouraud(Vram *vram, const DrawArea *clip, Vertex v[3])
{
    int32_t ox = clip->x_off, oy = clip->y_off;
    int32_t x0 = v[0].x + ox, y0 = v[0].y + oy;
    int32_t x1 = v[1].x + ox, y1 = v[1].y + oy;
    int32_t x2 = v[2].x + ox, y2 = v[2].y + oy;
    int32_t min_x, max_x, min_y, max_y, area2;
    if (!tri_bbox(clip, x0, y0, x1, y1, x2, y2,
                  &min_x, &max_x, &min_y, &max_y, &area2)) return;

    for (int32_t py = min_y; py <= max_y; py++) {
        for (int32_t px = min_x; px <= max_x; px++) {
            int32_t w0 = edge(x1, y1, x2, y2, px, py);
            int32_t w1 = edge(x2, y2, x0, y0, px, py);
            int32_t w2 = edge(x0, y0, x1, y1, px, py);
            int inside = (area2 > 0) ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                                     : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (!inside) continue;
            int32_t bw0 = area2 > 0 ? w0 : -w0;
            int32_t bw1 = area2 > 0 ? w1 : -w1;
            int32_t bw2 = area2 > 0 ? w2 : -w2;
            int32_t tot = bw0 + bw1 + bw2;
            if (tot == 0) continue;
            Color c;
            c.r = (uint8_t)i32_clamp((v[0].r*bw0 + v[1].r*bw1 + v[2].r*bw2) / tot, 0, 255);
            c.g = (uint8_t)i32_clamp((v[0].g*bw0 + v[1].g*bw1 + v[2].g*bw2) / tot, 0, 255);
            c.b = (uint8_t)i32_clamp((v[0].b*bw0 + v[1].b*bw1 + v[2].b*bw2) / tot, 0, 255);
            vram_set(vram, px, py, color_to_bgr555(c));
        }
    }
}

/* ---- draw_triangle_textured --------------------------------------- */

void draw_triangle_textured(Vram *vram, const DrawArea *clip,
                             Vertex v[3], TexCoord uv[3],
                             TexContext tc, Color flat_color, int modulate)
{
    const uint16_t *vd = vram->data;
    int32_t ox = clip->x_off, oy = clip->y_off;
    int32_t x0 = v[0].x + ox, y0 = v[0].y + oy;
    int32_t x1 = v[1].x + ox, y1 = v[1].y + oy;
    int32_t x2 = v[2].x + ox, y2 = v[2].y + oy;
    int32_t min_x, max_x, min_y, max_y, area2;
    if (!tri_bbox(clip, x0, y0, x1, y1, x2, y2,
                  &min_x, &max_x, &min_y, &max_y, &area2)) return;

    for (int32_t py = min_y; py <= max_y; py++) {
        for (int32_t px = min_x; px <= max_x; px++) {
            int32_t w0 = edge(x1, y1, x2, y2, px, py);
            int32_t w1 = edge(x2, y2, x0, y0, px, py);
            int32_t w2 = edge(x0, y0, x1, y1, px, py);
            int inside = (area2 > 0) ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                                     : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (!inside) continue;
            int32_t bw0 = area2 > 0 ? w0 : -w0;
            int32_t bw1 = area2 > 0 ? w1 : -w1;
            int32_t bw2 = area2 > 0 ? w2 : -w2;
            int32_t area = bw0 + bw1 + bw2;
            if (area == 0) continue;

            uint8_t u  = (uint8_t)((uv[0].u * bw0 + uv[1].u * bw1 + uv[2].u * bw2) / area);
            uint8_t vc = (uint8_t)((uv[0].v * bw0 + uv[1].v * bw1 + uv[2].v * bw2) / area);
            Color tex  = tex_sample_raw(tc, vd, u, vc);
            if (tex.r == 0 && tex.g == 0 && tex.b == 0) continue;

            uint16_t pixel;
            if (modulate) {
                Color mc;
                mc.r = (uint8_t)(((uint16_t)tex.r * flat_color.r) >> 7);
                mc.g = (uint8_t)(((uint16_t)tex.g * flat_color.g) >> 7);
                mc.b = (uint8_t)(((uint16_t)tex.b * flat_color.b) >> 7);
                pixel = color_to_bgr555(mc);
            } else {
                pixel = color_to_bgr555(tex);
            }
            vram_set(vram, px, py, pixel);
        }
    }
}

/* ---- quad helpers ------------------------------------------------- */

void draw_quad_flat(Vram *vram, const DrawArea *clip, Vertex v[4])
{
    Vertex t0[3] = { v[0], v[1], v[2] };
    Vertex t1[3] = { v[1], v[3], v[2] };
    draw_triangle_flat(vram, clip, t0);
    draw_triangle_flat(vram, clip, t1);
}

void draw_quad_gouraud(Vram *vram, const DrawArea *clip, Vertex v[4])
{
    Vertex t0[3] = { v[0], v[1], v[2] };
    Vertex t1[3] = { v[1], v[3], v[2] };
    draw_triangle_gouraud(vram, clip, t0);
    draw_triangle_gouraud(vram, clip, t1);
}

void draw_quad_textured(Vram *vram, const DrawArea *clip,
                        Vertex v[4], TexCoord uv[4],
                        TexContext tc, Color flat_color, int modulate)
{
    Vertex   t0v[3] = { v[0],  v[1],  v[2]  };
    TexCoord t0u[3] = { uv[0], uv[1], uv[2] };
    Vertex   t1v[3] = { v[1],  v[3],  v[2]  };
    TexCoord t1u[3] = { uv[1], uv[3], uv[2] };
    draw_triangle_textured(vram, clip, t0v, t0u, tc, flat_color, modulate);
    draw_triangle_textured(vram, clip, t1v, t1u, tc, flat_color, modulate);
}
