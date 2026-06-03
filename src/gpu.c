#include "gpu.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

#define VRAM_W 1024
#define VRAM_H 512

static uint32_t g_trace_prims_limit = 0;
static uint32_t g_trace_prims_count = 0;
static bool g_trace_prims_init = false;
static uint64_t g_frame_pixels_written = 0;

static bool trace_prim_enabled(void)
{
    if (!g_trace_prims_init)
    {
        g_trace_prims_init = true;
        const char *e = getenv("PS1_TRACE_PRIMS");
        if (e)
            g_trace_prims_limit = (uint32_t)strtoul(e, NULL, 10);
    }
    return g_trace_prims_count < g_trace_prims_limit;
}

static void trace_prim(const char *fmt, ...)
{
    if (!trace_prim_enabled())
        return;
    g_trace_prims_count++;
    fprintf(stderr, "PRIM[%u] ", g_trace_prims_count);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

/* ---- HorizontalRes ---- */
HorizontalRes hres_from_fields(uint8_t hr1, uint8_t hr2)
{
    HorizontalRes hr;
    hr.hr = (hr2 & 1) | ((hr1 & 3) << 1);
    return hr;
}
uint32_t hres_into_status(HorizontalRes hr) { return (uint32_t)hr.hr << 16; }

/* Returns the nominal display width in pixels for the current hres setting */
static uint16_t hres_width(HorizontalRes hr)
{
    switch (hr.hr & 7)
    {
    case 0:
        return 256;
    case 2:
        return 320;
    case 4:
        return 512;
    case 6:
        return 640;
    case 1:
        return 368;
    default:
        return 256;
    }
}

/* ---- CommandBuffer ---- */
static void cb_clear(CommandBuffer *cb) { cb->len = 0; }
static void cb_push(CommandBuffer *cb, uint32_t word)
{
    cb->buffer[cb->len++] = word;
}

/* ---- VRAM helpers ---- */
static inline uint16_t vram_load(const uint16_t *vram, uint32_t x, uint32_t y)
{
    return vram[(y & 511u) * 1024u + (x & 1023u)];
}
static inline void vram_store(uint16_t *vram, uint32_t x, uint32_t y, uint16_t v)
{
    vram[(y & 511u) * 1024u + (x & 1023u)] = v;
}

/* Convert 24-bit RGB to ABGR1555 (mask bit = 0) */
static inline uint16_t rgb_to_1555(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) & 0x1F) |
                      (((g >> 3) & 0x1F) << 5) |
                      (((b >> 3) & 0x1F) << 10));
}

/* ---- Texture fetch ---- */
static uint16_t texel_fetch(const Gpu *gpu, uint8_t u, uint8_t v)
{
    /* When texture_disable is set the rasterizer still runs but ignores the
       texture map; returning 0x7FFF (full white, non-transparent) causes the
       blend path to output the vertex color unchanged. */
    if (gpu->texture_disable)
        return 0x7FFF;

    uint16_t page_x = (uint16_t)gpu->page_base_x * 64u;
    uint16_t page_y = (uint16_t)gpu->page_base_y * 256u;

    /* Apply texture window mask/offset */
    u = (uint8_t)((u & ~(gpu->texture_window_x_mask * 8)) |
                  ((gpu->texture_window_x_offset & gpu->texture_window_x_mask) * 8));
    v = (uint8_t)((v & ~(gpu->texture_window_y_mask * 8)) |
                  ((gpu->texture_window_y_offset & gpu->texture_window_y_mask) * 8));

    switch (gpu->texture_depth)
    {
    case TEXTURE_DEPTH_4BIT:
    {
        /* 4 texels packed per 16-bit word: bits[3:0], [7:4], [11:8], [15:12] */
        uint32_t tx = page_x + u / 4;
        uint32_t ty = page_y + v;
        uint16_t raw = vram_load(gpu->vram, tx, ty);
        uint8_t idx = (raw >> ((u % 4) * 4)) & 0xF;
        return vram_load(gpu->vram, gpu->clut_x + idx, gpu->clut_y);
    }
    case TEXTURE_DEPTH_8BIT:
    {
        /* 2 texels packed per 16-bit word: low byte = even u, high byte = odd u */
        uint32_t tx = page_x + u / 2;
        uint32_t ty = page_y + v;
        uint16_t raw = vram_load(gpu->vram, tx, ty);
        uint8_t idx = (u & 1) ? (raw >> 8) & 0xFF : raw & 0xFF;
        return vram_load(gpu->vram, gpu->clut_x + idx, gpu->clut_y);
    }
    case TEXTURE_DEPTH_15BIT:
    {
        uint32_t tx = page_x + u;
        uint32_t ty = page_y + v;
        return vram_load(gpu->vram, tx, ty);
    }
    }
    return 0;
}

/* ---- Draw pixel with clipping, mask-bit, and semi-transparency ---- */
static void draw_pixel(Gpu *gpu, int32_t x, int32_t y, uint16_t color, bool semi)
{
    if (x < (int32_t)gpu->drawing_area_left || x > (int32_t)gpu->drawing_area_right)
        return;
    if (y < (int32_t)gpu->drawing_area_top || y > (int32_t)gpu->drawing_area_bottom)
        return;
    uint32_t ux = (uint32_t)x & 1023u;
    uint32_t uy = (uint32_t)y & 511u;
    if (gpu->preserve_masked_pixels && (gpu->vram[uy * 1024u + ux] & 0x8000u))
        return;

    if (semi)
    {
        uint16_t bg = gpu->vram[uy * 1024u + ux];
        uint32_t br = (bg & 0x1Fu);
        uint32_t bg_ = (bg >> 5) & 0x1Fu;
        uint32_t bb = (bg >> 10) & 0x1Fu;
        uint32_t fr = (color & 0x1Fu);
        uint32_t fg_ = (color >> 5) & 0x1Fu;
        uint32_t fb = (color >> 10) & 0x1Fu;
        uint32_t or_, og, ob;
        switch (gpu->semi_transparency)
        {
        default:
        case 0:
            or_ = (br + fr) / 2;
            og = (bg_ + fg_) / 2;
            ob = (bb + fb) / 2;
            break; /* (B+F)/2 */
        case 1:
            or_ = br + fr;
            og = bg_ + fg_;
            ob = bb + fb;
            break; /* B+F     */
        case 2:
            or_ = (br > fr) ? br - fr : 0;
            og = (bg_ > fg_) ? bg_ - fg_ : 0;
            ob = (bb > fb) ? bb - fb : 0;
            break; /* B-F */
        case 3:
            or_ = br + fr / 4;
            og = bg_ + fg_ / 4;
            ob = bb + fb / 4;
            break; /* B+F/4   */
        }
        if (or_ > 31u)
            or_ = 31u;
        if (og > 31u)
            og = 31u;
        if (ob > 31u)
            ob = 31u;
        color = (uint16_t)(or_ | (og << 5) | (ob << 10));
    }

    if (gpu->force_set_mask_bit)
        color |= 0x8000u;
    gpu->vram[uy * 1024u + ux] = color;
    g_frame_pixels_written++;
}

/* ---- Software rasterizer ---- */

#define FRAC 16

static void swap_i32(int32_t *a, int32_t *b)
{
    int32_t t = *a;
    *a = *b;
    *b = t;
}
static void swap_u8(uint8_t *a, uint8_t *b)
{
    uint8_t t = *a;
    *a = *b;
    *b = t;
}

/* Sort three vertices by Y using simple comparisons */
static void sort3_y(int32_t px[3], int32_t py[3],
                    uint8_t pr[3], uint8_t pg[3], uint8_t pb[3])
{
    if (py[0] > py[1])
    {
        swap_i32(&px[0], &px[1]);
        swap_i32(&py[0], &py[1]);
        swap_u8(&pr[0], &pr[1]);
        swap_u8(&pg[0], &pg[1]);
        swap_u8(&pb[0], &pb[1]);
    }
    if (py[1] > py[2])
    {
        swap_i32(&px[1], &px[2]);
        swap_i32(&py[1], &py[2]);
        swap_u8(&pr[1], &pr[2]);
        swap_u8(&pg[1], &pg[2]);
        swap_u8(&pb[1], &pb[2]);
    }
    if (py[0] > py[1])
    {
        swap_i32(&px[0], &px[1]);
        swap_i32(&py[0], &py[1]);
        swap_u8(&pr[0], &pr[1]);
        swap_u8(&pg[0], &pg[1]);
        swap_u8(&pb[0], &pb[1]);
    }
}

static void sort3_y_tex(int32_t px[3], int32_t py[3],
                        uint8_t pu[3], uint8_t pv[3],
                        uint8_t pr[3], uint8_t pg[3], uint8_t pb[3])
{
#define SWAP3T(i, j)              \
    do                            \
    {                             \
        swap_i32(&px[i], &px[j]); \
        swap_i32(&py[i], &py[j]); \
        swap_u8(&pu[i], &pu[j]);  \
        swap_u8(&pv[i], &pv[j]);  \
        swap_u8(&pr[i], &pr[j]);  \
        swap_u8(&pg[i], &pg[j]);  \
        swap_u8(&pb[i], &pb[j]);  \
    } while (0)
    if (py[0] > py[1])
        SWAP3T(0, 1);
    if (py[1] > py[2])
        SWAP3T(1, 2);
    if (py[0] > py[1])
        SWAP3T(0, 1);
#undef SWAP3T
}

/* Filled shaded triangle via scanline rasterization */
static void fill_triangle(Gpu *gpu,
                          int32_t x0, int32_t y0, uint8_t r0, uint8_t g0, uint8_t b0,
                          int32_t x1, int32_t y1, uint8_t r1, uint8_t g1, uint8_t b1,
                          int32_t x2, int32_t y2, uint8_t r2, uint8_t g2, uint8_t b2,
                          bool semi)
{
    int32_t px[3] = {x0, x1, x2}, py[3] = {y0, y1, y2};
    uint8_t pr[3] = {r0, r1, r2}, pg[3] = {g0, g1, g2}, pb[3] = {b0, b1, b2};
    sort3_y(px, py, pr, pg, pb);

    if (py[0] == py[2])
        return; /* degenerate */

    for (int32_t y = py[0]; y <= py[2]; y++)
    {
        int32_t dy02 = py[2] - py[0];
        int32_t t02 = (dy02 == 0) ? 0 : ((y - py[0]) << FRAC) / dy02;
        int32_t xa = px[0] + (((px[2] - px[0]) * t02) >> FRAC);
        int32_t ra = pr[0] + (((int32_t)(pr[2] - pr[0]) * t02) >> FRAC);
        int32_t ga = pg[0] + (((int32_t)(pg[2] - pg[0]) * t02) >> FRAC);
        int32_t ba = pb[0] + (((int32_t)(pb[2] - pb[0]) * t02) >> FRAC);

        int32_t xb, rb, gb, bb;
        if (y <= py[1])
        {
            int32_t dy01 = py[1] - py[0];
            int32_t t01 = (dy01 == 0) ? (1 << FRAC) : ((y - py[0]) << FRAC) / dy01;
            xb = px[0] + (((px[1] - px[0]) * t01) >> FRAC);
            rb = pr[0] + (((int32_t)(pr[1] - pr[0]) * t01) >> FRAC);
            gb = pg[0] + (((int32_t)(pg[1] - pg[0]) * t01) >> FRAC);
            bb = pb[0] + (((int32_t)(pb[1] - pb[0]) * t01) >> FRAC);
        }
        else
        {
            int32_t dy12 = py[2] - py[1];
            int32_t t12 = (dy12 == 0) ? 0 : ((y - py[1]) << FRAC) / dy12;
            xb = px[1] + (((px[2] - px[1]) * t12) >> FRAC);
            rb = pr[1] + (((int32_t)(pr[2] - pr[1]) * t12) >> FRAC);
            gb = pg[1] + (((int32_t)(pg[2] - pg[1]) * t12) >> FRAC);
            bb = pb[1] + (((int32_t)(pb[2] - pb[1]) * t12) >> FRAC);
        }

        if (xa > xb)
        {
            swap_i32(&xa, &xb);
            int32_t t;
            t = ra;
            ra = rb;
            rb = t;
            t = ga;
            ga = gb;
            gb = t;
            t = ba;
            ba = bb;
            bb = t;
        }
        int32_t dx = xb - xa;
        for (int32_t x = xa; x <= xb; x++)
        {
            uint8_t cr, cg, cb;
            if (dx == 0)
            {
                cr = (uint8_t)ra;
                cg = (uint8_t)ga;
                cb = (uint8_t)ba;
            }
            else
            {
                int32_t t = ((x - xa) << 16) / dx;
                cr = (uint8_t)(ra + (((rb - ra) * t) >> 16));
                cg = (uint8_t)(ga + (((gb - ga) * t) >> 16));
                cb = (uint8_t)(ba + (((bb - ba) * t) >> 16));
            }
            draw_pixel(gpu, x, y, rgb_to_1555(cr, cg, cb), semi);
        }
    }
}

/* Textured triangle: UV + per-vertex color modulation, all interpolated */
static void fill_triangle_tex(Gpu *gpu,
                              int32_t x0, int32_t y0, uint8_t u0, uint8_t v0, uint8_t r0, uint8_t g0, uint8_t b0,
                              int32_t x1, int32_t y1, uint8_t u1, uint8_t v1, uint8_t r1, uint8_t g1, uint8_t b1,
                              int32_t x2, int32_t y2, uint8_t u2, uint8_t v2, uint8_t r2, uint8_t g2, uint8_t b2,
                              bool blend, bool semi)
{
    int32_t px[3] = {x0, x1, x2}, py[3] = {y0, y1, y2};
    uint8_t pu[3] = {u0, u1, u2}, pv[3] = {v0, v1, v2};
    uint8_t pr[3] = {r0, r1, r2}, pg[3] = {g0, g1, g2}, pb[3] = {b0, b1, b2};
    sort3_y_tex(px, py, pu, pv, pr, pg, pb);

    for (int32_t y = py[0]; y <= py[2]; y++)
    {
        int32_t dy02 = py[2] - py[0];
        int32_t xa, xb;
        uint8_t ua, va, ub, vb;
        int32_t ra, ga, ba, rb, gb, bb;
        int32_t t02 = (dy02 == 0) ? 0 : ((y - py[0]) << FRAC) / dy02;
        xa = px[0] + (((px[2] - px[0]) * t02) >> FRAC);
        ua = (uint8_t)(pu[0] + (((int32_t)(pu[2] - pu[0]) * t02) >> FRAC));
        va = (uint8_t)(pv[0] + (((int32_t)(pv[2] - pv[0]) * t02) >> FRAC));
        ra = pr[0] + (((int32_t)(pr[2] - pr[0]) * t02) >> FRAC);
        ga = pg[0] + (((int32_t)(pg[2] - pg[0]) * t02) >> FRAC);
        ba = pb[0] + (((int32_t)(pb[2] - pb[0]) * t02) >> FRAC);

        if (y <= py[1])
        {
            int32_t dy01 = py[1] - py[0];
            int32_t t01 = (dy01 == 0) ? (1 << FRAC) : ((y - py[0]) << FRAC) / dy01;
            xb = (int32_t)(px[0] + (((px[1] - px[0]) * t01) >> FRAC));
            ub = (uint8_t)(pu[0] + (((int32_t)(pu[1] - pu[0]) * t01) >> FRAC));
            vb = (uint8_t)(pv[0] + (((int32_t)(pv[1] - pv[0]) * t01) >> FRAC));
            rb = pr[0] + (((int32_t)(pr[1] - pr[0]) * t01) >> FRAC);
            gb = pg[0] + (((int32_t)(pg[1] - pg[0]) * t01) >> FRAC);
            bb = pb[0] + (((int32_t)(pb[1] - pb[0]) * t01) >> FRAC);
        }
        else
        {
            int32_t dy12 = py[2] - py[1];
            int32_t t12 = (dy12 == 0) ? 0 : ((y - py[1]) << FRAC) / dy12;
            xb = (int32_t)(px[1] + (((px[2] - px[1]) * t12) >> FRAC));
            ub = (uint8_t)(pu[1] + (((int32_t)(pu[2] - pu[1]) * t12) >> FRAC));
            vb = (uint8_t)(pv[1] + (((int32_t)(pv[2] - pv[1]) * t12) >> FRAC));
            rb = pr[1] + (((int32_t)(pr[2] - pr[1]) * t12) >> FRAC);
            gb = pg[1] + (((int32_t)(pg[2] - pg[1]) * t12) >> FRAC);
            bb = pb[1] + (((int32_t)(pb[2] - pb[1]) * t12) >> FRAC);
        }

        if (xa > xb)
        {
            int32_t t;
            t = xa;
            xa = xb;
            xb = t;
            uint8_t tc;
            tc = ua;
            ua = ub;
            ub = tc;
            tc = va;
            va = vb;
            vb = tc;
            t = ra;
            ra = rb;
            rb = t;
            t = ga;
            ga = gb;
            gb = t;
            t = ba;
            ba = bb;
            bb = t;
        }
        int32_t dx = xb - xa;
        for (int32_t x = xa; x <= xb; x++)
        {
            uint8_t fu, fv, mcr, mcg, mcb;
            if (dx == 0)
            {
                fu = ua;
                fv = va;
                mcr = (uint8_t)ra;
                mcg = (uint8_t)ga;
                mcb = (uint8_t)ba;
            }
            else
            {
                int32_t t = ((x - xa) << 16) / dx;
                fu = (uint8_t)(ua + (((int32_t)(ub - ua) * t) >> 16));
                fv = (uint8_t)(va + (((int32_t)(vb - va) * t) >> 16));
                mcr = (uint8_t)(ra + (((rb - ra) * t) >> 16));
                mcg = (uint8_t)(ga + (((gb - ga) * t) >> 16));
                mcb = (uint8_t)(ba + (((bb - ba) * t) >> 16));
            }
            uint16_t texel = texel_fetch(gpu, fu, fv);
            if (texel == 0)
                continue; /* transparent */
            bool pix_semi = semi && (texel & 0x8000u);
            uint16_t pixel;
            if (blend)
            {
                uint32_t tr = ((texel & 0x1Fu) << 3) * mcr / 128u;
                uint32_t tg = (((texel >> 5) & 0x1Fu) << 3) * mcg / 128u;
                uint32_t tb = (((texel >> 10) & 0x1Fu) << 3) * mcb / 128u;
                if (tr > 255u)
                    tr = 255u;
                if (tg > 255u)
                    tg = 255u;
                if (tb > 255u)
                    tb = 255u;
                pixel = rgb_to_1555((uint8_t)tr, (uint8_t)tg, (uint8_t)tb);
            }
            else
            {
                pixel = texel & 0x7FFFu; /* strip STP bit before writing */
            }
            draw_pixel(gpu, x, y, pixel, pix_semi);
        }
    }
}

/* ---- Init ---- */
void gpu_init(Gpu *gpu, SDL_Window *window)
{
    memset(gpu, 0, sizeof(*gpu));
    gpu->vram = calloc(VRAM_W * VRAM_H, sizeof(uint16_t));
    if (!gpu->vram)
    {
        fprintf(stderr, "VRAM alloc failed\n");
        exit(1);
    }
    gpu->texture_depth = TEXTURE_DEPTH_4BIT;
    gpu->field = FIELD_TOP;
    gpu->vres = VRES_240;
    gpu->vmode = VMODE_NTSC;
    gpu->display_depth = DISPLAY_DEPTH_15;
    gpu->display_disabled = true;
    gpu->dma_direction = DMA_DIR_OFF;
    gpu->hres = hres_from_fields(0, 0);
    gpu->gp0_mode = GP0_MODE_COMMAND;
    if (window)
        renderer_init(&gpu->renderer, window);
}

void gpu_destroy(Gpu *gpu)
{
    renderer_destroy(&gpu->renderer);
    free(gpu->vram);
    gpu->vram = NULL;
}

/* ---- Status / Read ---- */
uint32_t gpu_status(const Gpu *gpu)
{
    uint32_t r =
        ((uint32_t)gpu->page_base_x << 0) |
        ((uint32_t)gpu->page_base_y << 4) |
        ((uint32_t)gpu->semi_transparency << 5) |
        ((uint32_t)gpu->texture_depth << 7) |
        ((uint32_t)gpu->dithering << 9) |
        ((uint32_t)gpu->draw_to_display << 10) |
        ((uint32_t)gpu->force_set_mask_bit << 11) |
        ((uint32_t)gpu->preserve_masked_pixels << 12) |
        ((uint32_t)gpu->field << 13) |
        ((uint32_t)gpu->texture_disable << 15) |
        hres_into_status(gpu->hres) |
        ((uint32_t)gpu->vmode << 20) |
        ((uint32_t)gpu->display_depth << 21) |
        ((uint32_t)gpu->interlaced << 22) |
        ((uint32_t)gpu->display_disabled << 23) |
        ((uint32_t)gpu->interrupt << 24) |
        (1u << 26) | (1u << 27) | (1u << 28) |
        ((uint32_t)gpu->dma_direction << 29);

    uint32_t dma_req;
    switch (gpu->dma_direction)
    {
    case DMA_DIR_OFF:
        dma_req = 0;
        break;
    case DMA_DIR_FIFO:
        dma_req = 1;
        break;
    case DMA_DIR_CPU_TO_GP0:
        dma_req = (r >> 28) & 1;
        break;
    case DMA_DIR_VRAM_TO_CPU:
        dma_req = (r >> 27) & 1;
        break;
    default:
        dma_req = 0;
        break;
    }
    return r | (dma_req << 25);
}

uint32_t gpu_read(Gpu *gpu)
{
    if (gpu->gp0_mode != GP0_MODE_IMAGE_STORE || gpu->image_store_words_remaining == 0)
        return gpu->gpuread_latch;

    uint16_t p0 = 0, p1 = 0;
    uint32_t ax = (gpu->image_store_x + gpu->image_store_cur_x) & 1023u;
    uint32_t ay = (gpu->image_store_y + gpu->image_store_cur_y) & 511u;
    p0 = gpu->vram[ay * 1024u + ax];
    gpu->image_store_cur_x++;
    if (gpu->image_store_cur_x >= gpu->image_store_w)
    {
        gpu->image_store_cur_x = 0;
        gpu->image_store_cur_y++;
    }
    /* Always read p1: even on the last word, both 16-bit halves belong to this transfer */
    {
        uint32_t bx = (gpu->image_store_x + gpu->image_store_cur_x) & 1023u;
        uint32_t by = (gpu->image_store_y + gpu->image_store_cur_y) & 511u;
        p1 = gpu->vram[by * 1024u + bx];
        gpu->image_store_cur_x++;
        if (gpu->image_store_cur_x >= gpu->image_store_w)
        {
            gpu->image_store_cur_x = 0;
            gpu->image_store_cur_y++;
        }
    }
    gpu->image_store_words_remaining--;
    if (gpu->image_store_words_remaining == 0)
        gpu->gp0_mode = GP0_MODE_COMMAND;
    return ((uint32_t)p1 << 16) | p0;
}

/* ---- Bresenham line rasterizer ---- */
static void draw_line(Gpu *gpu,
                      int32_t x0, int32_t y0, uint8_t r0, uint8_t g0, uint8_t b0,
                      int32_t x1, int32_t y1, uint8_t r1, uint8_t g1, uint8_t b1,
                      bool semi)
{
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t ax = dx < 0 ? -dx : dx;
    int32_t ay = dy < 0 ? -dy : dy;
    int32_t steps = (ax > ay) ? ax : ay;
    if (steps == 0)
    {
        draw_pixel(gpu, x0, y0, rgb_to_1555(r0, g0, b0), semi);
        return;
    }
    for (int32_t i = 0; i <= steps; i++)
    {
        int32_t t = (i << 16) / steps;
        int32_t x = x0 + (dx * t >> 16);
        int32_t y = y0 + (dy * t >> 16);
        uint8_t cr = (uint8_t)(r0 + ((int32_t)(r1 - r0) * t >> 16));
        uint8_t cg = (uint8_t)(g0 + ((int32_t)(g1 - g0) * t >> 16));
        uint8_t cb = (uint8_t)(b0 + ((int32_t)(b1 - b0) * t >> 16));
        draw_pixel(gpu, x, y, rgb_to_1555(cr, cg, cb), semi);
    }
}

/* ---- GP0 handlers (forward decls) ---- */
static void gp0_nop(Gpu *g);
static void gp0_fill_rect(Gpu *g);
static void gp0_clear_cache(Gpu *g);
static void gp0_triangle_mono_opaque(Gpu *g);
static void gp0_quad_mono_opaque(Gpu *g);
static void gp0_triangle_tex_opaque(Gpu *g);
static void gp0_quad_tex_opaque(Gpu *g);
static void gp0_triangle_shaded_opaque(Gpu *g);
static void gp0_quad_shaded_opaque(Gpu *g);
static void gp0_triangle_shaded_tex_opaque(Gpu *g);
static void gp0_quad_shaded_tex_opaque(Gpu *g);
static void gp0_rect_variable_opaque(Gpu *g);
static void gp0_rect_1x1_opaque(Gpu *g);
static void gp0_rect_8x8_opaque(Gpu *g);
static void gp0_rect_16x16_opaque(Gpu *g);
static void gp0_rect_variable_tex_opaque(Gpu *g);
static void gp0_rect_1x1_tex_opaque(Gpu *g);
static void gp0_rect_8x8_tex_opaque(Gpu *g);
static void gp0_rect_16x16_tex_opaque(Gpu *g);
static void gp0_line_mono(Gpu *g);
static void gp0_line_shaded(Gpu *g);
static void gp0_image_load(Gpu *g);
static void gp0_image_store(Gpu *g);
static void gp0_draw_mode(Gpu *g);
static void gp0_texture_window(Gpu *g);
static void gp0_drawing_area_top_left(Gpu *g);
static void gp0_drawing_area_bottom_right(Gpu *g);
static void gp0_drawing_offset(Gpu *g);
static void gp0_mask_bit_setting(Gpu *g);

/* ---- GP0 dispatch ---- */
void gpu_gp0(Gpu *gpu, uint32_t val)
{
    if (gpu->gp0_words_remaining == 0)
    {
        uint32_t op = (val >> 24) & 0xFF;
        uint32_t len;
        void (*method)(Gpu *);
        switch (op)
        {
        case 0x00:
            len = 1;
            method = gp0_nop;
            break;
        case 0x01:
            len = 1;
            method = gp0_clear_cache;
            break;
        case 0x02:
            len = 3;
            method = gp0_fill_rect;
            break;
        /* mono triangles: 0x20-0x23 */
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
            len = 4;
            method = gp0_triangle_mono_opaque;
            break;
        /* textured triangles: 0x24-0x27 */
        case 0x24:
        case 0x25:
        case 0x26:
        case 0x27:
            len = 7;
            method = gp0_triangle_tex_opaque;
            break;
        /* mono quads: 0x28-0x2B */
        case 0x28:
        case 0x29:
        case 0x2A:
        case 0x2B:
            len = 5;
            method = gp0_quad_mono_opaque;
            break;
        /* textured quads: 0x2C-0x2F */
        case 0x2C:
        case 0x2D:
        case 0x2E:
        case 0x2F:
            len = 9;
            method = gp0_quad_tex_opaque;
            break;
        /* shaded triangles: 0x30-0x33 */
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
            len = 6;
            method = gp0_triangle_shaded_opaque;
            break;
        /* shaded+textured triangles: 0x34-0x37 */
        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
            len = 9;
            method = gp0_triangle_shaded_tex_opaque;
            break;
        /* shaded quads: 0x38-0x3B */
        case 0x38:
        case 0x39:
        case 0x3A:
        case 0x3B:
            len = 8;
            method = gp0_quad_shaded_opaque;
            break;
        /* shaded+textured quads: 0x3C-0x3F */
        case 0x3C:
        case 0x3D:
        case 0x3E:
        case 0x3F:
            len = 12;
            method = gp0_quad_shaded_tex_opaque;
            break;
        /* mono lines: 0x40-0x4F (0x48-0x4F = polyline sentinel variant) */
        case 0x40:
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
        case 0x48:
        case 0x49:
        case 0x4A:
        case 0x4B:
        case 0x4C:
        case 0x4D:
        case 0x4E:
        case 0x4F:
            len = 3;
            method = gp0_line_mono;
            break;
        /* shaded lines: 0x50-0x5F (0x58-0x5F = polyline sentinel variant) */
        case 0x50:
        case 0x51:
        case 0x52:
        case 0x53:
        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57:
        case 0x58:
        case 0x59:
        case 0x5A:
        case 0x5B:
        case 0x5C:
        case 0x5D:
        case 0x5E:
        case 0x5F:
            len = 4;
            method = gp0_line_shaded;
            break;
        /* variable rectangles: 0x60-0x63 */
        case 0x60:
        case 0x61:
        case 0x62:
        case 0x63:
            len = 3;
            method = gp0_rect_variable_opaque;
            break;
        /* variable textured rectangles: 0x64-0x67 */
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67:
            len = 4;
            method = gp0_rect_variable_tex_opaque;
            break;
        /* 1x1 rectangles: 0x68-0x6B */
        case 0x68:
        case 0x69:
        case 0x6A:
        case 0x6B:
            len = 2;
            method = gp0_rect_1x1_opaque;
            break;
        /* 1x1 textured rectangles: 0x6C-0x6F */
        case 0x6C:
        case 0x6D:
        case 0x6E:
        case 0x6F:
            len = 3;
            method = gp0_rect_1x1_tex_opaque;
            break;
        /* 8x8 rectangles: 0x70-0x73 */
        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
            len = 2;
            method = gp0_rect_8x8_opaque;
            break;
        /* 8x8 textured rectangles: 0x74-0x77 */
        case 0x74:
        case 0x75:
        case 0x76:
        case 0x77:
            len = 3;
            method = gp0_rect_8x8_tex_opaque;
            break;
        /* 16x16 rectangles: 0x78-0x7B */
        case 0x78:
        case 0x79:
        case 0x7A:
        case 0x7B:
            len = 2;
            method = gp0_rect_16x16_opaque;
            break;
        /* 16x16 textured rectangles: 0x7C-0x7F */
        case 0x7C:
        case 0x7D:
        case 0x7E:
        case 0x7F:
            len = 3;
            method = gp0_rect_16x16_tex_opaque;
            break;
        /* VRAM transfers */
        case 0xA0:
            len = 3;
            method = gp0_image_load;
            break;
        case 0xC0:
            len = 3;
            method = gp0_image_store;
            break;
        /* state */
        case 0xE1:
            len = 1;
            method = gp0_draw_mode;
            break;
        case 0xE2:
            len = 1;
            method = gp0_texture_window;
            break;
        case 0xE3:
            len = 1;
            method = gp0_drawing_area_top_left;
            break;
        case 0xE4:
            len = 1;
            method = gp0_drawing_area_bottom_right;
            break;
        case 0xE5:
            len = 1;
            method = gp0_drawing_offset;
            break;
        case 0xE6:
            len = 1;
            method = gp0_mask_bit_setting;
            break;
        default:
            fprintf(stderr, "Unhandled GP0 0x%02X (word 0x%08X)\n", op, val);
            return; /* tolerate instead of exit */
        }
        LOG(LOG_GPU, "GP0 op=0x%02X val=0x%08X len=%u", op, val, len);
        gpu->gp0_words_remaining = len;
        gpu->gp0_command_method = method;
        cb_clear(&gpu->gp0_command);
    }

    gpu->gp0_words_remaining--;

    switch (gpu->gp0_mode)
    {
    case GP0_MODE_COMMAND:
        cb_push(&gpu->gp0_command, val);
        if (gpu->gp0_words_remaining == 0)
        {
            gpu_gp1_reset_consecutive();
            gpu->gp0_command_method(gpu);
        }
        break;
    case GP0_MODE_IMAGE_LOAD:
    {
        uint16_t pix[2] = {(uint16_t)(val & 0xFFFF), (uint16_t)(val >> 16)};
        for (int i = 0; i < 2; i++)
        {
            if (gpu->image_load_cur_y >= gpu->image_load_h)
                break;
            uint32_t ax = (gpu->image_load_x + gpu->image_load_cur_x) & 1023u;
            uint32_t ay = (gpu->image_load_y + gpu->image_load_cur_y) & 511u;
            gpu->vram[ay * 1024u + ax] = pix[i];
            gpu->image_load_cur_x++;
            if (gpu->image_load_cur_x >= gpu->image_load_w)
            {
                gpu->image_load_cur_x = 0;
                gpu->image_load_cur_y++;
            }
        }
        if (gpu->gp0_words_remaining == 0)
            gpu->gp0_mode = GP0_MODE_COMMAND;
        break;
    }
    case GP0_MODE_IMAGE_STORE:
        break;
    }
}

/* ---- GP0 implementations ---- */

static void gp0_nop(Gpu *g) { (void)g; }
static void gp0_clear_cache(Gpu *g) { (void)g; }

/* 0x02: Fill rectangle (ignores clipping, uses raw XY mod 1024/512) */
static void gp0_fill_rect(Gpu *gpu)
{
    uint32_t c = gpu->gp0_command.buffer[0];
    uint32_t xy = gpu->gp0_command.buffer[1];
    uint32_t wh = gpu->gp0_command.buffer[2];
    uint8_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    uint16_t col = rgb_to_1555(r, g, b);
    uint16_t x = (uint16_t)(xy & 0x3F0); /* already aligned */
    uint16_t y = (uint16_t)((xy >> 16) & 0x1FF);
    uint16_t w = (uint16_t)((wh & 0x3FF) + 0xF) & ~0xF;
    uint16_t h = (uint16_t)((wh >> 16) & 0x1FF);
    for (uint16_t dy = 0; dy < h; dy++)
        for (uint16_t dx = 0; dx < w; dx++)
            vram_store(gpu->vram, (x + dx) & 1023u, (y + dy) & 511u, col);
}

static void gp0_draw_mode(Gpu *gpu)
{
    uint32_t v = gpu->gp0_command.buffer[0];
    gpu->page_base_x = v & 0x0F;
    gpu->page_base_y = (v >> 4) & 1;
    gpu->semi_transparency = (v >> 5) & 3;
    switch ((v >> 7) & 3)
    {
    case 0:
        gpu->texture_depth = TEXTURE_DEPTH_4BIT;
        break;
    case 1:
        gpu->texture_depth = TEXTURE_DEPTH_8BIT;
        break;
    case 2:
        gpu->texture_depth = TEXTURE_DEPTH_15BIT;
        break;
    default:
        break;
    }
    gpu->dithering = ((v >> 9) & 1) != 0;
    gpu->draw_to_display = ((v >> 10) & 1) != 0;
    gpu->texture_disable = ((v >> 11) & 1) != 0;
    gpu->rectangle_texture_x_flip = ((v >> 12) & 1) != 0;
    gpu->rectangle_texture_y_flip = ((v >> 13) & 1) != 0;
}

static void gp0_texture_window(Gpu *gpu)
{
    uint32_t v = gpu->gp0_command.buffer[0];
    gpu->texture_window_x_mask = v & 0x1F;
    gpu->texture_window_y_mask = (v >> 5) & 0x1F;
    gpu->texture_window_x_offset = (v >> 10) & 0x1F;
    gpu->texture_window_y_offset = (v >> 15) & 0x1F;
}

static void gp0_drawing_area_top_left(Gpu *gpu)
{
    uint32_t v = gpu->gp0_command.buffer[0];
    gpu->drawing_area_left = v & 0x3FF;
    gpu->drawing_area_top = (v >> 10) & 0x3FF;
}

static void gp0_drawing_area_bottom_right(Gpu *gpu)
{
    uint32_t v = gpu->gp0_command.buffer[0];
    gpu->drawing_area_right = v & 0x3FF;
    gpu->drawing_area_bottom = (v >> 10) & 0x3FF;
}

static void gp0_drawing_offset(Gpu *gpu)
{
    uint32_t v = gpu->gp0_command.buffer[0];
    uint16_t x = v & 0x07FF;
    uint16_t y = (v >> 11) & 0x07FF;
    gpu->drawing_x_offset = (int16_t)(x << 5) >> 5;
    gpu->drawing_y_offset = (int16_t)(y << 5) >> 5;
}

static void gp0_mask_bit_setting(Gpu *gpu)
{
    uint32_t v = gpu->gp0_command.buffer[0];
    gpu->force_set_mask_bit = (v & 1) != 0;
    gpu->preserve_masked_pixels = (v & 2) != 0;
}

/* ---- helper: apply drawing offset to buffer coordinates ---- */
static void apply_offset(const Gpu *gpu, int32_t *x, int32_t *y)
{
    *x += gpu->drawing_x_offset;
    *y += gpu->drawing_y_offset;
}

/* ---- Mono triangles ---- */
static void gp0_triangle_mono_opaque(Gpu *gpu)
{
    uint32_t c = gpu->gp0_command.buffer[0];
    uint8_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    int32_t x0 = (int16_t)(gpu->gp0_command.buffer[1] & 0xFFFF), y0 = (int16_t)(gpu->gp0_command.buffer[1] >> 16);
    int32_t x1 = (int16_t)(gpu->gp0_command.buffer[2] & 0xFFFF), y1 = (int16_t)(gpu->gp0_command.buffer[2] >> 16);
    int32_t x2 = (int16_t)(gpu->gp0_command.buffer[3] & 0xFFFF), y2 = (int16_t)(gpu->gp0_command.buffer[3] >> 16);
    apply_offset(gpu, &x0, &y0);
    apply_offset(gpu, &x1, &y1);
    apply_offset(gpu, &x2, &y2);
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    fill_triangle(gpu, x0, y0, r, g, b, x1, y1, r, g, b, x2, y2, r, g, b, semi);
}

/* ---- Mono quads ---- */
static void gp0_quad_mono_opaque(Gpu *gpu)
{
    uint32_t c = gpu->gp0_command.buffer[0];
    uint8_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    int32_t xs[4], ys[4];
    for (int i = 0; i < 4; i++)
    {
        xs[i] = (int16_t)(gpu->gp0_command.buffer[i + 1] & 0xFFFF);
        ys[i] = (int16_t)(gpu->gp0_command.buffer[i + 1] >> 16);
        apply_offset(gpu, &xs[i], &ys[i]);
    }
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    fill_triangle(gpu, xs[0], ys[0], r, g, b, xs[1], ys[1], r, g, b, xs[2], ys[2], r, g, b, semi);
    fill_triangle(gpu, xs[1], ys[1], r, g, b, xs[2], ys[2], r, g, b, xs[3], ys[3], r, g, b, semi);
}

/* ---- Shaded triangles ---- */
static void gp0_triangle_shaded_opaque(Gpu *gpu)
{
    int32_t x0 = (int16_t)(gpu->gp0_command.buffer[1] & 0xFFFF), y0 = (int16_t)(gpu->gp0_command.buffer[1] >> 16);
    int32_t x1 = (int16_t)(gpu->gp0_command.buffer[3] & 0xFFFF), y1 = (int16_t)(gpu->gp0_command.buffer[3] >> 16);
    int32_t x2 = (int16_t)(gpu->gp0_command.buffer[5] & 0xFFFF), y2 = (int16_t)(gpu->gp0_command.buffer[5] >> 16);
    apply_offset(gpu, &x0, &y0);
    apply_offset(gpu, &x1, &y1);
    apply_offset(gpu, &x2, &y2);
    uint32_t c0 = gpu->gp0_command.buffer[0], c1 = gpu->gp0_command.buffer[2], c2 = gpu->gp0_command.buffer[4];
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    fill_triangle(gpu, x0, y0, c0 & 0xFF, (c0 >> 8) & 0xFF, (c0 >> 16) & 0xFF,
                  x1, y1, c1 & 0xFF, (c1 >> 8) & 0xFF, (c1 >> 16) & 0xFF,
                  x2, y2, c2 & 0xFF, (c2 >> 8) & 0xFF, (c2 >> 16) & 0xFF, semi);
}

/* ---- Shaded quads ---- */
static void gp0_quad_shaded_opaque(Gpu *gpu)
{
    int32_t xs[4], ys[4];
    uint8_t rs[4], gs[4], bs[4];
    for (int i = 0; i < 4; i++)
    {
        uint32_t ci = gpu->gp0_command.buffer[i * 2];
        rs[i] = ci & 0xFF;
        gs[i] = (ci >> 8) & 0xFF;
        bs[i] = (ci >> 16) & 0xFF;
        xs[i] = (int16_t)(gpu->gp0_command.buffer[i * 2 + 1] & 0xFFFF);
        ys[i] = (int16_t)(gpu->gp0_command.buffer[i * 2 + 1] >> 16);
        apply_offset(gpu, &xs[i], &ys[i]);
    }
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    fill_triangle(gpu, xs[0], ys[0], rs[0], gs[0], bs[0],
                  xs[1], ys[1], rs[1], gs[1], bs[1],
                  xs[2], ys[2], rs[2], gs[2], bs[2], semi);
    fill_triangle(gpu, xs[1], ys[1], rs[1], gs[1], bs[1],
                  xs[2], ys[2], rs[2], gs[2], bs[2],
                  xs[3], ys[3], rs[3], gs[3], bs[3], semi);
}

/* ---- Textured triangles (cmd col + uv*3) ---- */
/* format: [color/clut] [xy0+uv0] [page_xy1+uv1] ... */
/* GP0 0x24: color, v0+u0, clut, v1+u1, page, v2+u2 */
static void gp0_triangle_tex_opaque(Gpu *gpu)
{
    uint32_t cv = gpu->gp0_command.buffer[0];
    uint8_t cr = cv & 0xFF, cg = (cv >> 8) & 0xFF, cb = (cv >> 16) & 0xFF;

    uint32_t w0 = gpu->gp0_command.buffer[1];
    uint32_t w1 = gpu->gp0_command.buffer[2];
    uint32_t w2 = gpu->gp0_command.buffer[3];
    uint32_t w3 = gpu->gp0_command.buffer[4];
    uint32_t w4 = gpu->gp0_command.buffer[5];
    uint32_t w5 = gpu->gp0_command.buffer[6];

    int32_t x0 = (int16_t)(w0 & 0xFFFF), y0 = (int16_t)(w0 >> 16);
    uint8_t u0 = w1 & 0xFF, v0 = (w1 >> 8) & 0xFF;
    uint16_t clut_raw = (w1 >> 16) & 0xFFFF;
    gpu->clut_x = (clut_raw & 0x3F) * 16;
    gpu->clut_y = (clut_raw >> 6) & 0x1FF;

    int32_t x1 = (int16_t)(w2 & 0xFFFF), y1 = (int16_t)(w2 >> 16);
    uint8_t u1 = w3 & 0xFF, v1 = (w3 >> 8) & 0xFF;
    uint16_t page_raw = (w3 >> 16) & 0xFFFF;
    gpu->page_base_x = page_raw & 0x0F;
    gpu->page_base_y = (page_raw >> 4) & 1;
    switch ((page_raw >> 7) & 3)
    {
    case 0:
        gpu->texture_depth = TEXTURE_DEPTH_4BIT;
        break;
    case 1:
        gpu->texture_depth = TEXTURE_DEPTH_8BIT;
        break;
    case 2:
        gpu->texture_depth = TEXTURE_DEPTH_15BIT;
        break;
    }

    int32_t x2 = (int16_t)(w4 & 0xFFFF), y2 = (int16_t)(w4 >> 16);
    uint8_t u2 = w5 & 0xFF, v2 = (w5 >> 8) & 0xFF;

    apply_offset(gpu, &x0, &y0);
    apply_offset(gpu, &x1, &y1);
    apply_offset(gpu, &x2, &y2);
    /* bit 0 of the command byte: 0 = blend modulation color, 1 = raw texture */
    bool blend = ((gpu->gp0_command.buffer[0] >> 24) & 0x01) == 0;
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    fill_triangle_tex(gpu, x0, y0, u0, v0, cr, cg, cb, x1, y1, u1, v1, cr, cg, cb, x2, y2, u2, v2, cr, cg, cb, blend, semi);
}

/* ---- Textured quads (9 words) ---- */
/* format: color, xy0+uv0+clut, xy1+uv1+page, xy2+uv2, xy3+uv3 */
static void gp0_quad_tex_opaque(Gpu *gpu)
{
    uint32_t cv = gpu->gp0_command.buffer[0];
    uint8_t cr = cv & 0xFF, cg = (cv >> 8) & 0xFF, cb = (cv >> 16) & 0xFF;

    int32_t xs[4], ys[4];
    uint8_t us[4], vs[4];
    uint32_t w;

    w = gpu->gp0_command.buffer[1];
    xs[0] = (int16_t)(w & 0xFFFF);
    ys[0] = (int16_t)(w >> 16);
    w = gpu->gp0_command.buffer[2];
    us[0] = w & 0xFF;
    vs[0] = (w >> 8) & 0xFF;
    uint16_t clut_raw = (uint16_t)(w >> 16);
    gpu->clut_x = (clut_raw & 0x3F) * 16;
    gpu->clut_y = (clut_raw >> 6) & 0x1FF;

    w = gpu->gp0_command.buffer[3];
    xs[1] = (int16_t)(w & 0xFFFF);
    ys[1] = (int16_t)(w >> 16);
    w = gpu->gp0_command.buffer[4];
    us[1] = w & 0xFF;
    vs[1] = (w >> 8) & 0xFF;
    uint16_t page_raw = (uint16_t)(w >> 16);
    gpu->page_base_x = page_raw & 0x0F;
    gpu->page_base_y = (page_raw >> 4) & 1;
    switch ((page_raw >> 7) & 3)
    {
    case 0:
        gpu->texture_depth = TEXTURE_DEPTH_4BIT;
        break;
    case 1:
        gpu->texture_depth = TEXTURE_DEPTH_8BIT;
        break;
    case 2:
        gpu->texture_depth = TEXTURE_DEPTH_15BIT;
        break;
    }

    w = gpu->gp0_command.buffer[5];
    xs[2] = (int16_t)(w & 0xFFFF);
    ys[2] = (int16_t)(w >> 16);
    w = gpu->gp0_command.buffer[6];
    us[2] = w & 0xFF;
    vs[2] = (w >> 8) & 0xFF;
    w = gpu->gp0_command.buffer[7];
    xs[3] = (int16_t)(w & 0xFFFF);
    ys[3] = (int16_t)(w >> 16);
    w = gpu->gp0_command.buffer[8];
    us[3] = w & 0xFF;
    vs[3] = (w >> 8) & 0xFF;

    for (int i = 0; i < 4; i++)
        apply_offset(gpu, &xs[i], &ys[i]);
    bool blend = ((gpu->gp0_command.buffer[0] >> 24) & 0x01) == 0;
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    trace_prim("0x2C color=%02X,%02X,%02X xy=(%d,%d)(%d,%d)(%d,%d)(%d,%d) "
               "uv=(%u,%u)(%u,%u)(%u,%u)(%u,%u) page=(%u,%u) depth=%u clut=(%u,%u) "
               "clip=(%u,%u)-(%u,%u) off=(%d,%d)",
               cr, cg, cb,
               xs[0], ys[0], xs[1], ys[1], xs[2], ys[2], xs[3], ys[3],
               us[0], vs[0], us[1], vs[1], us[2], vs[2], us[3], vs[3],
               gpu->page_base_x, gpu->page_base_y, (unsigned)gpu->texture_depth,
               gpu->clut_x, gpu->clut_y,
               gpu->drawing_area_left, gpu->drawing_area_top,
               gpu->drawing_area_right, gpu->drawing_area_bottom,
               gpu->drawing_x_offset, gpu->drawing_y_offset);
    fill_triangle_tex(gpu, xs[0], ys[0], us[0], vs[0], cr, cg, cb, xs[1], ys[1], us[1], vs[1], cr, cg, cb, xs[2], ys[2], us[2], vs[2], cr, cg, cb, blend, semi);
    fill_triangle_tex(gpu, xs[1], ys[1], us[1], vs[1], cr, cg, cb, xs[2], ys[2], us[2], vs[2], cr, cg, cb, xs[3], ys[3], us[3], vs[3], cr, cg, cb, blend, semi);
}

/* ---- Shaded+textured triangle (9 words) ---- */
/* format: color0, xy0, uv0+clut, color1, xy1, uv1+page, color2, xy2, uv2 */
static void gp0_triangle_shaded_tex_opaque(Gpu *gpu)
{
    uint32_t c0 = gpu->gp0_command.buffer[0];
    uint8_t r0 = c0 & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = (c0 >> 16) & 0xFF;
    int32_t x0 = (int16_t)(gpu->gp0_command.buffer[1] & 0xFFFF), y0 = (int16_t)(gpu->gp0_command.buffer[1] >> 16);
    uint32_t uv0c = gpu->gp0_command.buffer[2];
    uint8_t u0 = uv0c & 0xFF, v0 = (uv0c >> 8) & 0xFF;
    uint16_t clut_raw = (uint16_t)(uv0c >> 16);
    gpu->clut_x = (clut_raw & 0x3F) * 16;
    gpu->clut_y = (clut_raw >> 6) & 0x1FF;

    uint32_t c1 = gpu->gp0_command.buffer[3];
    uint8_t r1 = c1 & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = (c1 >> 16) & 0xFF;
    int32_t x1 = (int16_t)(gpu->gp0_command.buffer[4] & 0xFFFF), y1 = (int16_t)(gpu->gp0_command.buffer[4] >> 16);
    uint32_t uv1p = gpu->gp0_command.buffer[5];
    uint8_t u1 = uv1p & 0xFF, v1 = (uv1p >> 8) & 0xFF;
    uint16_t page_raw = (uint16_t)(uv1p >> 16);
    gpu->page_base_x = page_raw & 0x0F;
    gpu->page_base_y = (page_raw >> 4) & 1;
    switch ((page_raw >> 7) & 3)
    {
    case 0:
        gpu->texture_depth = TEXTURE_DEPTH_4BIT;
        break;
    case 1:
        gpu->texture_depth = TEXTURE_DEPTH_8BIT;
        break;
    case 2:
        gpu->texture_depth = TEXTURE_DEPTH_15BIT;
        break;
    }

    uint32_t c2 = gpu->gp0_command.buffer[6];
    uint8_t r2 = c2 & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = (c2 >> 16) & 0xFF;
    int32_t x2 = (int16_t)(gpu->gp0_command.buffer[7] & 0xFFFF), y2 = (int16_t)(gpu->gp0_command.buffer[7] >> 16);
    uint32_t uv2 = gpu->gp0_command.buffer[8];
    uint8_t u2 = uv2 & 0xFF, v2 = (uv2 >> 8) & 0xFF;

    apply_offset(gpu, &x0, &y0);
    apply_offset(gpu, &x1, &y1);
    apply_offset(gpu, &x2, &y2);
    bool blend = ((gpu->gp0_command.buffer[0] >> 24) & 0x01) == 0;
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    fill_triangle_tex(gpu, x0, y0, u0, v0, r0, g0, b0, x1, y1, u1, v1, r1, g1, b1, x2, y2, u2, v2, r2, g2, b2, blend, semi);
}

/* ---- Shaded+textured quad (12 words) ---- */
/* format: color0,xy0,uv0+clut, color1,xy1,uv1+page, color2,xy2,uv2, color3,xy3,uv3 */
static void gp0_quad_shaded_tex_opaque(Gpu *gpu)
{
    int32_t xs[4], ys[4];
    uint8_t us[4], vs[4], rs[4], gs[4], bs[4];
    for (int i = 0; i < 4; i++)
    {
        uint32_t ci = gpu->gp0_command.buffer[i * 3];
        rs[i] = ci & 0xFF;
        gs[i] = (ci >> 8) & 0xFF;
        bs[i] = (ci >> 16) & 0xFF;
        uint32_t xy = gpu->gp0_command.buffer[i * 3 + 1];
        xs[i] = (int16_t)(xy & 0xFFFF);
        ys[i] = (int16_t)(xy >> 16);
        uint32_t uv = gpu->gp0_command.buffer[i * 3 + 2];
        us[i] = uv & 0xFF;
        vs[i] = (uv >> 8) & 0xFF;
        if (i == 0)
        {
            uint16_t cr2 = (uint16_t)(uv >> 16);
            gpu->clut_x = (cr2 & 0x3F) * 16;
            gpu->clut_y = (cr2 >> 6) & 0x1FF;
        }
        if (i == 1)
        {
            uint16_t pr = (uint16_t)(uv >> 16);
            gpu->page_base_x = pr & 0x0F;
            gpu->page_base_y = (pr >> 4) & 1;
            switch ((pr >> 7) & 3)
            {
            case 0:
                gpu->texture_depth = TEXTURE_DEPTH_4BIT;
                break;
            case 1:
                gpu->texture_depth = TEXTURE_DEPTH_8BIT;
                break;
            case 2:
                gpu->texture_depth = TEXTURE_DEPTH_15BIT;
                break;
            }
        }
        apply_offset(gpu, &xs[i], &ys[i]);
    }
    bool blend = ((gpu->gp0_command.buffer[0] >> 24) & 0x01) == 0;
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    fill_triangle_tex(gpu, xs[0], ys[0], us[0], vs[0], rs[0], gs[0], bs[0],
                      xs[1], ys[1], us[1], vs[1], rs[1], gs[1], bs[1],
                      xs[2], ys[2], us[2], vs[2], rs[2], gs[2], bs[2], blend, semi);
    fill_triangle_tex(gpu, xs[1], ys[1], us[1], vs[1], rs[1], gs[1], bs[1],
                      xs[2], ys[2], us[2], vs[2], rs[2], gs[2], bs[2],
                      xs[3], ys[3], us[3], vs[3], rs[3], gs[3], bs[3], blend, semi);
}

/* ---- Rectangle helpers ---- */
static void fill_rect_sw(Gpu *gpu, int32_t rx, int32_t ry, int32_t rw, int32_t rh, uint16_t color, bool semi)
{
    for (int32_t dy = 0; dy < rh; dy++)
        for (int32_t dx = 0; dx < rw; dx++)
            draw_pixel(gpu, rx + dx, ry + dy, color, semi);
}
static void fill_rect_tex_sw(Gpu *gpu, int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                             uint8_t u0, uint8_t v0, uint8_t cr, uint8_t cg, uint8_t cb, bool blend, bool semi)
{
    for (int32_t dy = 0; dy < rh; dy++)
    {
        uint8_t fv = (uint8_t)(v0 + dy);
        if (gpu->rectangle_texture_y_flip)
            fv = (uint8_t)(v0 - dy);
        for (int32_t dx = 0; dx < rw; dx++)
        {
            uint8_t fu = (uint8_t)(u0 + dx);
            if (gpu->rectangle_texture_x_flip)
                fu = (uint8_t)(u0 - dx);
            uint16_t texel = texel_fetch(gpu, fu, fv);
            if (texel == 0)
                continue;
            bool pix_semi = semi && (texel & 0x8000u);
            uint16_t pixel;
            if (blend)
            {
                uint32_t tr = (uint32_t)((texel & 0x1Fu) << 3) * cr / 128u;
                uint32_t tg = (uint32_t)(((texel >> 5) & 0x1Fu) << 3) * cg / 128u;
                uint32_t tb = (uint32_t)(((texel >> 10) & 0x1Fu) << 3) * cb / 128u;
                if (tr > 255u)
                    tr = 255u;
                if (tg > 255u)
                    tg = 255u;
                if (tb > 255u)
                    tb = 255u;
                pixel = rgb_to_1555((uint8_t)tr, (uint8_t)tg, (uint8_t)tb);
            }
            else
            {
                pixel = texel & 0x7FFFu;
            }
            draw_pixel(gpu, rx + dx, ry + dy, pixel, pix_semi);
        }
    }
}

static void rect_common(Gpu *gpu, int32_t w, int32_t h)
{
    uint32_t c = gpu->gp0_command.buffer[0];
    uint8_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    uint32_t xy = gpu->gp0_command.buffer[1];
    int32_t rx = (int16_t)(xy & 0xFFFF), ry = (int16_t)(xy >> 16);
    apply_offset(gpu, &rx, &ry);
    bool semi = ((c >> 24) & 0x02) != 0;
    fill_rect_sw(gpu, rx, ry, w, h, rgb_to_1555(r, g, b), semi);
}
static void rect_common_var(Gpu *gpu)
{
    uint32_t c = gpu->gp0_command.buffer[0];
    uint8_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    uint32_t xy = gpu->gp0_command.buffer[1];
    int32_t rx = (int16_t)(xy & 0xFFFF), ry = (int16_t)(xy >> 16);
    apply_offset(gpu, &rx, &ry);
    uint32_t wh = gpu->gp0_command.buffer[2];
    int32_t w = wh & 0x3FF, h = (wh >> 16) & 0x1FF;
    bool semi = ((c >> 24) & 0x02) != 0;
    fill_rect_sw(gpu, rx, ry, w, h, rgb_to_1555(r, g, b), semi);
}
static void gp0_rect_variable_opaque(Gpu *gpu) { rect_common_var(gpu); }
static void gp0_rect_1x1_opaque(Gpu *gpu) { rect_common(gpu, 1, 1); }
static void gp0_rect_8x8_opaque(Gpu *gpu) { rect_common(gpu, 8, 8); }
static void gp0_rect_16x16_opaque(Gpu *gpu) { rect_common(gpu, 16, 16); }

static void rect_tex_common(Gpu *gpu, int32_t w, int32_t h)
{
    uint32_t c = gpu->gp0_command.buffer[0];
    uint8_t cr = c & 0xFF, cg = (c >> 8) & 0xFF, cb = (c >> 16) & 0xFF;
    uint32_t xy = gpu->gp0_command.buffer[1];
    int32_t rx = (int16_t)(xy & 0xFFFF), ry = (int16_t)(xy >> 16);
    apply_offset(gpu, &rx, &ry);
    uint32_t uv_clut = gpu->gp0_command.buffer[2];
    uint8_t u0 = uv_clut & 0xFF, v0 = (uv_clut >> 8) & 0xFF;
    uint16_t clut_raw = (uint16_t)(uv_clut >> 16);
    gpu->clut_x = (clut_raw & 0x3F) * 16;
    gpu->clut_y = (clut_raw >> 6) & 0x1FF;
    bool blend = ((c >> 24) & 0x01) == 0;
    bool semi = ((c >> 24) & 0x02) != 0;
    fill_rect_tex_sw(gpu, rx, ry, w, h, u0, v0, cr, cg, cb, blend, semi);
}
static void rect_tex_common_var(Gpu *gpu)
{
    uint32_t c = gpu->gp0_command.buffer[0];
    uint8_t cr = c & 0xFF, cg = (c >> 8) & 0xFF, cb = (c >> 16) & 0xFF;
    uint32_t xy = gpu->gp0_command.buffer[1];
    int32_t rx = (int16_t)(xy & 0xFFFF), ry = (int16_t)(xy >> 16);
    apply_offset(gpu, &rx, &ry);
    uint32_t uv_clut = gpu->gp0_command.buffer[2];
    uint8_t u0 = uv_clut & 0xFF, v0 = (uv_clut >> 8) & 0xFF;
    uint16_t clut_raw = (uint16_t)(uv_clut >> 16);
    gpu->clut_x = (clut_raw & 0x3F) * 16;
    gpu->clut_y = (clut_raw >> 6) & 0x1FF;
    uint32_t wh = gpu->gp0_command.buffer[3];
    int32_t w = wh & 0x3FF, h = (wh >> 16) & 0x1FF;
    bool blend = ((c >> 24) & 0x01) == 0;
    bool semi = ((c >> 24) & 0x02) != 0;
    fill_rect_tex_sw(gpu, rx, ry, w, h, u0, v0, cr, cg, cb, blend, semi);
}
static void gp0_rect_variable_tex_opaque(Gpu *gpu) { rect_tex_common_var(gpu); }
static void gp0_rect_1x1_tex_opaque(Gpu *gpu) { rect_tex_common(gpu, 1, 1); }
static void gp0_rect_8x8_tex_opaque(Gpu *gpu) { rect_tex_common(gpu, 8, 8); }
static void gp0_rect_16x16_tex_opaque(Gpu *gpu) { rect_tex_common(gpu, 16, 16); }

static void gp0_image_load(Gpu *gpu)
{
    uint32_t dest = gpu->gp0_command.buffer[1];
    uint32_t res = gpu->gp0_command.buffer[2];
    uint16_t x = dest & 0x3FF, y = (dest >> 16) & 0x1FF;
    uint16_t w = res & 0xFFFF, h = res >> 16;
    if (w == 0 || h == 0)
        return;
    LOG(LOG_GPU, "GP0 image load dst=%u,%u size=%ux%u words=%u",
        x, y, w, h, ((uint32_t)w * h + 1u) / 2u);
    gpu->image_load_x = x;
    gpu->image_load_y = y;
    gpu->image_load_w = w;
    gpu->image_load_h = h;
    gpu->image_load_cur_x = gpu->image_load_cur_y = 0;
    uint32_t npix = (uint32_t)w * h;
    gpu->gp0_words_remaining = (npix + 1) / 2;
    gpu->gp0_mode = GP0_MODE_IMAGE_LOAD;
}

static void gp0_image_store(Gpu *gpu)
{
    uint32_t src = gpu->gp0_command.buffer[1];
    uint32_t res = gpu->gp0_command.buffer[2];
    uint16_t x = src & 0x3FF, y = (src >> 16) & 0x1FF;
    uint16_t w = res & 0xFFFF, h = res >> 16;
    if (w == 0 || h == 0)
        return;
    gpu->image_store_x = x;
    gpu->image_store_y = y;
    gpu->image_store_w = w;
    gpu->image_store_h = h;
    gpu->image_store_cur_x = gpu->image_store_cur_y = 0;
    gpu->image_store_words_remaining = ((uint32_t)w * h + 1) / 2;
    gpu->gp0_mode = GP0_MODE_IMAGE_STORE;
}

/* 0x40-0x4F: mono line (single segment; polylines share the same handler via
   sentinel-driven re-dispatch but we handle only two-point here — sufficient
   for the vast majority of PS1 games). */
static void gp0_line_mono(Gpu *gpu)
{
    uint32_t op = gpu->gp0_command.buffer[0];
    uint32_t v0w = gpu->gp0_command.buffer[1];
    uint32_t v1w = gpu->gp0_command.buffer[2];
    bool semi = (op >> 25) & 1;
    uint8_t r = op & 0xFF, g = (op >> 8) & 0xFF, b = (op >> 16) & 0xFF;
    int32_t x0 = (int32_t)((int16_t)(v0w & 0xFFFF)) + gpu->drawing_x_offset;
    int32_t y0 = (int32_t)((int16_t)(v0w >> 16)) + gpu->drawing_y_offset;
    int32_t x1 = (int32_t)((int16_t)(v1w & 0xFFFF)) + gpu->drawing_x_offset;
    int32_t y1 = (int32_t)((int16_t)(v1w >> 16)) + gpu->drawing_y_offset;
    draw_line(gpu, x0, y0, r, g, b, x1, y1, r, g, b, semi);
}

/* 0x50-0x5F: shaded line */
static void gp0_line_shaded(Gpu *gpu)
{
    uint32_t c0w = gpu->gp0_command.buffer[0];
    uint32_t v0w = gpu->gp0_command.buffer[1];
    uint32_t c1w = gpu->gp0_command.buffer[2];
    uint32_t v1w = gpu->gp0_command.buffer[3];
    bool semi = (c0w >> 25) & 1;
    uint8_t r0 = c0w & 0xFF, g0 = (c0w >> 8) & 0xFF, b0 = (c0w >> 16) & 0xFF;
    uint8_t r1 = c1w & 0xFF, g1 = (c1w >> 8) & 0xFF, b1 = (c1w >> 16) & 0xFF;
    int32_t x0 = (int32_t)((int16_t)(v0w & 0xFFFF)) + gpu->drawing_x_offset;
    int32_t y0 = (int32_t)((int16_t)(v0w >> 16)) + gpu->drawing_y_offset;
    int32_t x1 = (int32_t)((int16_t)(v1w & 0xFFFF)) + gpu->drawing_x_offset;
    int32_t y1 = (int32_t)((int16_t)(v1w >> 16)) + gpu->drawing_y_offset;
    draw_line(gpu, x0, y0, r0, g0, b0, x1, y1, r1, g1, b1, semi);
}

/* ---- GP1 ---- */
static void gp1_reset(Gpu *gpu);
static void gp1_reset_command_buffer(Gpu *gpu);

/* Instrumentation: GP1 loop detector.
 * Set PS1_WATCH_GP1=1 to log every GP1 call.
 * Set PS1_GP1_LOOP_THRESH=N to abort after N consecutive GP1 calls with no GP0
 * draw command between them (default 0 = disabled). */
static uint32_t g_gp1_consecutive = 0;
static uint32_t g_gp1_loop_thresh = 0;
static bool     g_gp1_watch       = false;
static bool     g_gp1_dbg_init    = false;
static uint32_t g_gp1_last_ops[8];
static uint32_t g_gp1_last_vals[8];
static uint32_t g_gp1_last_head   = 0;

static void gp1_dbg_init(void)
{
    if (g_gp1_dbg_init) return;
    g_gp1_dbg_init = true;
    const char *e = getenv("PS1_GP1_LOOP_THRESH");
    if (e) g_gp1_loop_thresh = (uint32_t)strtoul(e, NULL, 10);
    g_gp1_watch = getenv("PS1_WATCH_GP1") != NULL;
}

void gpu_gp1_reset_consecutive(void) { g_gp1_consecutive = 0; }

void gpu_gp1(Gpu *gpu, uint32_t val)
{
    gp1_dbg_init();
    uint32_t op = (val >> 24) & 0xFF;
    LOG(LOG_GPU, "GP1 op=0x%02X val=0x%08X", op, val);

    /* Track consecutive GP1 calls (no GP0 draw between them) */
    g_gp1_last_ops [g_gp1_last_head & 7] = op;
    g_gp1_last_vals[g_gp1_last_head & 7] = val;
    g_gp1_last_head++;
    g_gp1_consecutive++;

    if (g_gp1_watch)
        fprintf(stderr, "GP1[%u] op=0x%02X val=0x%08X disp_dis=%d vram_xy=(%u,%u)\n",
                g_gp1_consecutive, op, val, gpu->display_disabled,
                gpu->display_vram_x_start, gpu->display_vram_y_start);

    if (g_gp1_loop_thresh && g_gp1_consecutive >= g_gp1_loop_thresh)
    {
        fprintf(stderr, "GP1_LOOP: %u consecutive GP1 calls without GP0 draw.\n",
                g_gp1_consecutive);
        fprintf(stderr, "  Last 8 GP1: ");
        for (uint32_t i = 0; i < 8; i++)
        {
            uint32_t idx = (g_gp1_last_head - 8 + i) & 7;
            fprintf(stderr, "op=0x%02X(0x%08X) ", g_gp1_last_ops[idx], g_gp1_last_vals[idx]);
        }
        fprintf(stderr, "\n  display_disabled=%d vram_xy=(%u,%u) hres=%u vres=%s\n",
                gpu->display_disabled,
                gpu->display_vram_x_start, gpu->display_vram_y_start,
                hres_width(gpu->hres),
                gpu->vres == VRES_480 ? "480" : "240");
        exit(1);
    }

    switch (op)
    {
    case 0x00:
        gp1_reset(gpu);
        break;
    case 0x01:
        gp1_reset_command_buffer(gpu);
        break;
    case 0x02:
        gpu->interrupt = false;
        break;
    case 0x03:
        gpu->display_disabled = val & 1;
        break;
    case 0x04:
        switch (val & 3)
        {
        case 0:
            gpu->dma_direction = DMA_DIR_OFF;
            break;
        case 1:
            gpu->dma_direction = DMA_DIR_FIFO;
            break;
        case 2:
            gpu->dma_direction = DMA_DIR_CPU_TO_GP0;
            break;
        case 3:
            gpu->dma_direction = DMA_DIR_VRAM_TO_CPU;
            break;
        }
        break;
    case 0x05:
        gpu->display_vram_x_start = val & 0x03FE;
        gpu->display_vram_y_start = (val >> 10) & 0x1FF;
        break;
    case 0x06:
        gpu->display_horiz_start = val & 0x0FFF;
        gpu->display_horiz_end = (val >> 12) & 0x0FFF;
        break;
    case 0x07:
        gpu->display_line_start = val & 0x03FF;
        gpu->display_line_end = (val >> 10) & 0x03FF;
        break;
    case 0x08:
    {
        uint8_t hr1 = val & 3;
        uint8_t hr2 = (val >> 6) & 1;
        gpu->hres = hres_from_fields(hr1, hr2);
        gpu->vres = (val & 0x04) ? VRES_480 : VRES_240;
        gpu->vmode = (val & 0x08) ? VMODE_PAL : VMODE_NTSC;
        gpu->display_depth = (val & 0x10) ? DISPLAY_DEPTH_24 : DISPLAY_DEPTH_15;
        gpu->interlaced = (val & 0x20) != 0;
        break;
    }
    case 0x10:
        switch (val & 0x0F)
        {
        case 0x02:
            gpu->gpuread_latch =
                (uint32_t)gpu->texture_window_x_mask |
                ((uint32_t)gpu->texture_window_y_mask << 5) |
                ((uint32_t)gpu->texture_window_x_offset << 10) |
                ((uint32_t)gpu->texture_window_y_offset << 15);
            break;
        case 0x03:
            gpu->gpuread_latch =
                (uint32_t)gpu->drawing_area_left |
                ((uint32_t)gpu->drawing_area_top << 10);
            break;
        case 0x04:
            gpu->gpuread_latch =
                (uint32_t)gpu->drawing_area_right |
                ((uint32_t)gpu->drawing_area_bottom << 10);
            break;
        case 0x05:
            gpu->gpuread_latch =
                ((uint32_t)gpu->drawing_x_offset & 0x7FFu) |
                (((uint32_t)gpu->drawing_y_offset & 0x7FFu) << 11);
            break;
        case 0x07:
            gpu->gpuread_latch = 0x00000002; /* GPU type: GPUv2 */
            break;
        default:
            gpu->gpuread_latch = 0;
            break;
        }
        break;
    default:
        fprintf(stderr, "Unhandled GP1 0x%02X\n", op);
        break;
    }
}

static void gp1_reset(Gpu *gpu)
{
    gpu->interrupt = false;
    gpu->page_base_x = gpu->page_base_y = 0;
    gpu->semi_transparency = 0;
    gpu->texture_depth = TEXTURE_DEPTH_4BIT;
    gpu->texture_window_x_mask = gpu->texture_window_y_mask = 0;
    gpu->texture_window_x_offset = gpu->texture_window_y_offset = 0;
    gpu->dithering = gpu->draw_to_display = gpu->texture_disable = false;
    gpu->rectangle_texture_x_flip = gpu->rectangle_texture_y_flip = false;
    gpu->drawing_area_left = gpu->drawing_area_top = 0;
    gpu->drawing_area_right = gpu->drawing_area_bottom = 0;
    gpu->drawing_x_offset = gpu->drawing_y_offset = 0;
    gpu->force_set_mask_bit = gpu->preserve_masked_pixels = false;
    gpu->dma_direction = DMA_DIR_OFF;
    gpu->display_disabled = true;
    gpu->display_vram_x_start = gpu->display_vram_y_start = 0;
    gpu->hres = hres_from_fields(0, 0);
    gpu->vres = VRES_240;
    gpu->vmode = VMODE_NTSC;
    gpu->interlaced = true;
    gpu->display_horiz_start = 0x0200;
    gpu->display_horiz_end = 0x0C00;
    gpu->display_line_start = 0x0010;
    gpu->display_line_end = 0x0100;
    gpu->display_depth = DISPLAY_DEPTH_15;
    gp1_reset_command_buffer(gpu);
}

static void gp1_reset_command_buffer(Gpu *gpu)
{
    cb_clear(&gpu->gp0_command);
    gpu->gp0_words_remaining = 0;
    gpu->gp0_mode = GP0_MODE_COMMAND;
}

/* Called externally (e.g. from scheduler VBlank) to present the frame */
void gpu_vblank(Gpu *gpu)
{
    /* Toggle interlace field each VBlank so GPUSTAT bit 13 reflects the current
       field. The BIOS checks this to decide which scanlines to draw into. */
    if (gpu->interlaced)
        gpu->field = (gpu->field == FIELD_TOP) ? FIELD_BOTTOM : FIELD_TOP;

    uint16_t w = hres_width(gpu->hres);
    uint16_t h = (gpu->vres == VRES_480) ? 480 : 240;
    bool display_24bit = gpu->display_depth == DISPLAY_DEPTH_24;
    renderer_display(&gpu->renderer, gpu->vram,
                     gpu->display_vram_x_start, gpu->display_vram_y_start, w, h,
                     display_24bit);
    gpu->frame_updated = true;

    static bool dumped = false;
    static uint32_t frame_count = 0;
    frame_count++;
    const char *stats_env = getenv("PS1_GPU_FRAME_STATS");
    if (stats_env)
    {
        uint32_t period = (uint32_t)strtoul(stats_env, NULL, 10);
        if (period == 0)
            period = 1;
        if ((frame_count % period) == 0)
        {
            uint32_t nonzero = 0;
            for (uint32_t y = 0; y < h; y++)
            {
                for (uint32_t x = 0; x < w; x++)
                {
                    uint32_t sx = (gpu->display_vram_x_start + x) & 1023u;
                    uint32_t sy = (gpu->display_vram_y_start + y) & 511u;
                    if ((gpu->vram[sy * 1024u + sx] & 0x7FFFu) != 0)
                        nonzero++;
                }
            }
            fprintf(stderr,
                    "GPU_FRAME %u writes=%llu display_nonzero=%u display=(%u,%u %ux%u) disabled=%d\n",
                    frame_count, (unsigned long long)g_frame_pixels_written, nonzero,
                    gpu->display_vram_x_start, gpu->display_vram_y_start, w, h,
                    gpu->display_disabled);
        }
        g_frame_pixels_written = 0;
    }
    const char *dump_path = getenv("PS1_DUMP_VRAM_PPM");
    const char *dump_frame_env = getenv("PS1_DUMP_FRAME");
    bool dump_full_vram = getenv("PS1_DUMP_FULL_VRAM") != NULL;
    uint32_t dump_frame = dump_frame_env ? (uint32_t)strtoul(dump_frame_env, NULL, 10) : 120u;
    if (dump_path && !dumped && frame_count >= dump_frame)
    {
        FILE *f = fopen(dump_path, "wb");
        if (f)
        {
            uint32_t dump_w = dump_full_vram ? 1024u : w;
            uint32_t dump_h = dump_full_vram ? 512u : h;
            fprintf(f, "P6\n%u %u\n255\n", dump_w, dump_h);
            for (uint32_t y = 0; y < dump_h; y++)
            {
                for (uint32_t x = 0; x < dump_w; x++)
                {
                    uint8_t r8, g8, b8;
                    if (!dump_full_vram && display_24bit)
                    {
                        uint32_t byte_base = (((gpu->display_vram_y_start + y) & 511u) * 1024u +
                                              (gpu->display_vram_x_start & 1023u)) *
                                                 2u +
                                             x * 3u;
                        uint16_t w0 = gpu->vram[(byte_base / 2u) & (1024u * 512u - 1u)];
                        uint16_t w1 = gpu->vram[((byte_base / 2u) + 1u) & (1024u * 512u - 1u)];
                        uint32_t bytes = ((uint32_t)w1 << 16) | w0;
                        if (byte_base & 1u)
                            bytes >>= 8;
                        r8 = bytes & 0xFFu;
                        g8 = (bytes >> 8) & 0xFFu;
                        b8 = (bytes >> 16) & 0xFFu;
                    }
                    else
                    {
                        uint32_t sx = dump_full_vram ? x : ((gpu->display_vram_x_start + x) & 1023u);
                        uint32_t sy = dump_full_vram ? y : ((gpu->display_vram_y_start + y) & 511u);
                        uint16_t c = gpu->vram[sy * 1024u + sx];
                        uint8_t r5 = c & 0x1Fu;
                        uint8_t g5 = (c >> 5) & 0x1Fu;
                        uint8_t b5 = (c >> 10) & 0x1Fu;
                        r8 = r5 << 3;
                        g8 = g5 << 3;
                        b8 = b5 << 3;
                    }
                    fputc(r8, f);
                    fputc(g8, f);
                    fputc(b8, f);
                }
            }
            fclose(f);
        }
        dumped = true;
    }
}
