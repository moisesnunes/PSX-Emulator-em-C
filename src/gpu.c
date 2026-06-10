#include "gpu.h"
#include "debug_trace.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

#define VRAM_W 1024
#define VRAM_H 512
#define MAX_PRIMITIVE_WIDTH 1024
#define MAX_PRIMITIVE_HEIGHT 512
#define GPU_NTSC_LINES_PER_FRAME 263u
#define GPU_PAL_LINES_PER_FRAME 314u
#define GPU_NTSC_CLOCKS_PER_LINE 3413u
#define GPU_PAL_CLOCKS_PER_LINE 3406u
#define GPU_HBLANK_START_CLOCK 2560u

static uint32_t g_trace_prims_limit = 0;
static uint32_t g_trace_prims_count = 0;
static bool g_trace_prims_init = false;
static uint32_t g_trace_big_prims_threshold = 0;
static uint64_t g_frame_pixels_written = 0;

static bool trace_prim_enabled(void)
{
    if (!g_trace_prims_init)
    {
        g_trace_prims_init = true;
        const char *e = getenv("PS1_TRACE_PRIMS");
        if (e)
            g_trace_prims_limit = (uint32_t)strtoul(e, NULL, 10);
        e = getenv("PS1_TRACE_BIG_PRIMS");
        if (e)
            g_trace_big_prims_threshold = (uint32_t)strtoul(e, NULL, 10);
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

static void trace_big_triangle(const Gpu *gpu, const char *kind,
                               int32_t x0, int32_t y0,
                               int32_t x1, int32_t y1,
                               int32_t x2, int32_t y2)
{
    if (!g_trace_prims_init)
        (void)trace_prim_enabled();
    if (g_trace_big_prims_threshold == 0)
        return;

    int32_t min_x = x0 < x1 ? x0 : x1;
    if (x2 < min_x)
        min_x = x2;
    int32_t max_x = x0 > x1 ? x0 : x1;
    if (x2 > max_x)
        max_x = x2;
    int32_t min_y = y0 < y1 ? y0 : y1;
    if (y2 < min_y)
        min_y = y2;
    int32_t max_y = y0 > y1 ? y0 : y1;
    if (y2 > max_y)
        max_y = y2;

    uint32_t bw = (uint32_t)(max_x - min_x);
    uint32_t bh = (uint32_t)(max_y - min_y);
    bool outside = min_x < -64 || min_y < -64 || max_x > 1088 || max_y > 576;
    if (bw < g_trace_big_prims_threshold && bh < g_trace_big_prims_threshold && !outside)
        return;

    fprintf(stderr,
            "BIG_PRIM kind=%s op=0x%02X xy=(%d,%d)(%d,%d)(%d,%d) bbox=(%d,%d %ux%u) "
            "clip=(%u,%u)-(%u,%u) off=(%d,%d) words=",
            kind, (unsigned)(gpu->gp0_command.buffer[0] >> 24),
            x0, y0, x1, y1, x2, y2,
            min_x, min_y, bw, bh,
            gpu->drawing_area_left, gpu->drawing_area_top,
            gpu->drawing_area_right, gpu->drawing_area_bottom,
            gpu->drawing_x_offset, gpu->drawing_y_offset);
    for (uint8_t i = 0; i < gpu->gp0_command.len; i++)
        fprintf(stderr, "%s%08X", i == 0 ? "" : ",", gpu->gp0_command.buffer[i]);
    fputc('\n', stderr);
}

static bool triangle_too_large(int32_t x0, int32_t y0,
                               int32_t x1, int32_t y1,
                               int32_t x2, int32_t y2)
{
    int32_t min_x = x0 < x1 ? x0 : x1;
    if (x2 < min_x)
        min_x = x2;
    int32_t max_x = x0 > x1 ? x0 : x1;
    if (x2 > max_x)
        max_x = x2;
    int32_t min_y = y0 < y1 ? y0 : y1;
    if (y2 < min_y)
        min_y = y2;
    int32_t max_y = y0 > y1 ? y0 : y1;
    if (y2 > max_y)
        max_y = y2;

    return (max_x - min_x + 1) > MAX_PRIMITIVE_WIDTH ||
           (max_y - min_y + 1) > MAX_PRIMITIVE_HEIGHT;
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

static uint16_t display_dotclock_divider(const Gpu *gpu)
{
    if (gpu->hres.hr & 1u)
        return 7;

    switch ((gpu->hres.hr >> 1) & 3u)
    {
    case 0:
        return 10;
    case 1:
        return 8;
    case 2:
        return 5;
    default:
        return 4;
    }
}

typedef struct
{
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} DisplayRect;

static DisplayRect display_rect_from_gpu(const Gpu *gpu)
{
    DisplayRect rect = {
        (uint16_t)(gpu->display_vram_x_start & 1023u),
        (uint16_t)(gpu->display_vram_y_start & 511u),
        hres_width(gpu->hres),
        (gpu->vres == VRES_480 && gpu->interlaced) ? 480 : 240,
    };

    uint32_t raw_width = (gpu->display_horiz_end >= gpu->display_horiz_start)
                             ? (uint32_t)(gpu->display_horiz_end - gpu->display_horiz_start)
                             : 0u;
    if (raw_width > 0)
    {
        uint32_t width = raw_width / display_dotclock_divider(gpu);
        if (width == 1u)
            width = 4u;
        else
            width = (width + 2u) & ~3u;

        if (width >= 4u && width <= 1024u)
            rect.w = (uint16_t)width;
    }

    uint32_t raw_height = (gpu->display_line_end >= gpu->display_line_start)
                              ? (uint32_t)(gpu->display_line_end - gpu->display_line_start)
                              : 0u;
    if (raw_height > 0u)
    {
        uint32_t height = raw_height;
        if (gpu->interlaced)
            height <<= 1;
        if (!(gpu->vres == VRES_480 && gpu->interlaced) && height > 240u)
            height = 240u;
        if (height >= 1u && height <= 512u)
            rect.h = (uint16_t)height;
    }

    if (rect.x + rect.w > 1024u)
        rect.w = (uint16_t)(1024u - rect.x);
    if (rect.y + rect.h > 512u)
        rect.h = (uint16_t)(512u - rect.y);
    if (rect.w == 0)
        rect.w = hres_width(gpu->hres);
    if (rect.h == 0)
        rect.h = (gpu->vres == VRES_480 && gpu->interlaced) ? 480 : 240;
    return rect;
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

static inline uint8_t dither_channel(uint8_t c, int32_t x, int32_t y)
{
    static const int8_t matrix[4][4] = {
        {-4, 0, -3, 1},
        {2, -2, 3, -1},
        {-3, 1, -4, 0},
        {3, -1, 2, -2},
    };
    int32_t v = (int32_t)c + matrix[y & 3][x & 3];
    if (v < 0)
        v = 0;
    if (v > 255)
        v = 255;
    return (uint8_t)v;
}

static inline uint16_t rgb_to_1555_dither(uint8_t r, uint8_t g, uint8_t b, int32_t x, int32_t y)
{
    return rgb_to_1555(dither_channel(r, x, y),
                       dither_channel(g, x, y),
                       dither_channel(b, x, y));
}

static inline int32_t gpu_coord11(uint32_t v)
{
    v &= 0x7FFu;
    return (v & 0x400u) ? (int32_t)(v | 0xFFFFF800u) : (int32_t)v;
}

static inline int32_t gpu_xy_x(uint32_t xy) { return gpu_coord11(xy); }
static inline int32_t gpu_xy_y(uint32_t xy) { return gpu_coord11(xy >> 16); }
static inline int32_t gpu_truncate_coord(int32_t v) { return gpu_coord11((uint32_t)v); }

static TextureDepth texture_depth_decode(uint32_t bits)
{
    if ((bits & 3u) == 0)
        return TEXTURE_DEPTH_4BIT;
    if ((bits & 3u) == 1)
        return TEXTURE_DEPTH_8BIT;
    return TEXTURE_DEPTH_15BIT;
}

/* ---- Texture fetch ---- */
typedef struct
{
    bool clut_valid;
    uint16_t clut[256];
} TextureCache;

static void texture_clut_cache_invalidate(Gpu *gpu)
{
    gpu->texture_clut_cache_valid = false;
    gpu->texture_clut_cache_stale = false;
}

static void texture_cache_init(Gpu *gpu, TextureCache *cache)
{
    cache->clut_valid = false;
    if (gpu->texture_disable || gpu->texture_depth == TEXTURE_DEPTH_15BIT)
        return;

    uint32_t entries = (gpu->texture_depth == TEXTURE_DEPTH_4BIT) ? 16u : 256u;
    bool stale_compatible =
        gpu->texture_clut_cache_valid &&
        gpu->texture_clut_cache_stale &&
        gpu->texture_clut_cache_x == gpu->clut_x &&
        gpu->texture_clut_cache_y == gpu->clut_y &&
        (gpu->texture_clut_cache_depth == gpu->texture_depth ||
         (gpu->texture_clut_cache_depth == TEXTURE_DEPTH_8BIT && gpu->texture_depth == TEXTURE_DEPTH_4BIT));

    if (!stale_compatible)
    {
        for (uint32_t i = 0; i < entries; i++)
            gpu->texture_clut_cache[i] = vram_load(gpu->vram, gpu->clut_x + i, gpu->clut_y);
        gpu->texture_clut_cache_x = gpu->clut_x;
        gpu->texture_clut_cache_y = gpu->clut_y;
        gpu->texture_clut_cache_depth = gpu->texture_depth;
        gpu->texture_clut_cache_valid = true;
        gpu->texture_clut_cache_stale = false;
    }

    memcpy(cache->clut, gpu->texture_clut_cache, entries * sizeof(cache->clut[0]));
    cache->clut_valid = true;
}

static uint16_t texel_fetch(const Gpu *gpu, const TextureCache *cache, uint8_t u, uint8_t v)
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
        return (cache && cache->clut_valid) ? cache->clut[idx] : vram_load(gpu->vram, gpu->clut_x + idx, gpu->clut_y);
    }
    case TEXTURE_DEPTH_8BIT:
    {
        /* 2 texels packed per 16-bit word: low byte = even u, high byte = odd u */
        uint32_t tx = page_x + u / 2;
        uint32_t ty = page_y + v;
        uint16_t raw = vram_load(gpu->vram, tx, ty);
        uint8_t idx = (u & 1) ? (raw >> 8) & 0xFF : raw & 0xFF;
        return (cache && cache->clut_valid) ? cache->clut[idx] : vram_load(gpu->vram, gpu->clut_x + idx, gpu->clut_y);
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
        default:
            or_ = fr;
            og = fg_;
            ob = fb;
            break;
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

#define GPU_ATTR_FRAC_BITS 12

typedef struct
{
    int32_t origin_x;
    int32_t origin_y;
    int64_t origin;
    int64_t step_x;
    int64_t step_y;
} GpuAttributePlane;

static int64_t attribute_step(int64_t numerator, uint32_t denominator)
{
    if (numerator == 0)
        return 0;

    uint32_t shift = (uint32_t)__builtin_clz(denominator);
    uint32_t normalized = denominator << shift;
    uint32_t reciprocal = (uint32_t)(
        ((1ULL << 62) + normalized - 1u) / normalized);
    uint32_t product_shift = 62u - shift - GPU_ATTR_FRAC_BITS;
    uint64_t magnitude = numerator < 0
                             ? (uint64_t)(-numerator)
                             : (uint64_t)numerator;
    int64_t value = (int64_t)((magnitude * reciprocal) >> product_shift);
    return numerator < 0 ? -value : value;
}

static int64_t edge_i64(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py)
{
    return (int64_t)(px - ax) * (by - ay) - (int64_t)(py - ay) * (bx - ax);
}

static bool edge_is_top_left(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    return (ay == by && ax > bx) || (ay < by);
}

static bool edge_accepts_zero(int32_t ax, int32_t ay, int32_t bx, int32_t by, bool neg)
{
    return neg ? edge_is_top_left(bx, by, ax, ay)
               : edge_is_top_left(ax, ay, bx, by);
}

static GpuAttributePlane attribute_plane(int32_t x0, int32_t y0, int32_t a0,
                                         int32_t x1, int32_t y1, int32_t a1,
                                         int32_t x2, int32_t y2, int32_t a2)
{
    int64_t area =
        (int64_t)(x1 - x0) * (y2 - y0) -
        (int64_t)(y1 - y0) * (x2 - x0);
    int64_t numerator_x =
        (int64_t)(a1 - a0) * (y2 - y0) -
        (int64_t)(a2 - a0) * (y1 - y0);
    int64_t numerator_y =
        (int64_t)(x1 - x0) * (a2 - a0) -
        (int64_t)(x2 - x0) * (a1 - a0);
    uint32_t denominator = (uint32_t)(area < 0 ? -area : area);
    if (area < 0)
    {
        numerator_x = -numerator_x;
        numerator_y = -numerator_y;
    }
    GpuAttributePlane plane = {
        .origin_x = x0,
        .origin_y = y0,
        .origin = ((int64_t)a0 << GPU_ATTR_FRAC_BITS) +
                  (1 << (GPU_ATTR_FRAC_BITS - 1)),
        .step_x = attribute_step(numerator_x, denominator),
        .step_y = attribute_step(numerator_y, denominator),
    };
    return plane;
}

static uint8_t attribute_plane_sample(const GpuAttributePlane *plane, int32_t x, int32_t y)
{
    int64_t value =
        plane->origin +
        (int64_t)(x - plane->origin_x) * plane->step_x +
        (int64_t)(y - plane->origin_y) * plane->step_y;
    if (value < 0)
        return 0;
    value >>= GPU_ATTR_FRAC_BITS;
    return value > 255 ? 255 : (uint8_t)value;
}

/* Filled shaded triangle via scanline rasterization */
static void fill_triangle(Gpu *gpu,
                          int32_t x0, int32_t y0, uint8_t r0, uint8_t g0, uint8_t b0,
                          int32_t x1, int32_t y1, uint8_t r1, uint8_t g1, uint8_t b1,
                          int32_t x2, int32_t y2, uint8_t r2, uint8_t g2, uint8_t b2,
                          bool semi, bool dither)
{
    int64_t area = edge_i64(x0, y0, x1, y1, x2, y2);
    if (area == 0)
        return;
    if (triangle_too_large(x0, y0, x1, y1, x2, y2))
        return;
    trace_big_triangle(gpu, "flat", x0, y0, x1, y1, x2, y2);

    int32_t min_x = x0 < x1 ? x0 : x1;
    if (x2 < min_x)
        min_x = x2;
    int32_t max_x = x0 > x1 ? x0 : x1;
    if (x2 > max_x)
        max_x = x2;
    int32_t min_y = y0 < y1 ? y0 : y1;
    if (y2 < min_y)
        min_y = y2;
    int32_t max_y = y0 > y1 ? y0 : y1;
    if (y2 > max_y)
        max_y = y2;

    bool neg = area < 0;
    if (neg)
        area = -area;
    bool tl01 = edge_accepts_zero(x0, y0, x1, y1, neg);
    bool tl12 = edge_accepts_zero(x1, y1, x2, y2, neg);
    bool tl20 = edge_accepts_zero(x2, y2, x0, y0, neg);
    GpuAttributePlane red = attribute_plane(x0, y0, r0, x1, y1, r1, x2, y2, r2);
    GpuAttributePlane green = attribute_plane(x0, y0, g0, x1, y1, g1, x2, y2, g2);
    GpuAttributePlane blue = attribute_plane(x0, y0, b0, x1, y1, b1, x2, y2, b2);

    for (int32_t y = min_y; y <= max_y; y++)
    {
        for (int32_t x = min_x; x <= max_x; x++)
        {
            int64_t w0 = edge_i64(x1, y1, x2, y2, x, y);
            int64_t w1 = edge_i64(x2, y2, x0, y0, x, y);
            int64_t w2 = edge_i64(x0, y0, x1, y1, x, y);
            if (neg)
            {
                w0 = -w0;
                w1 = -w1;
                w2 = -w2;
            }

            if (!((w0 > 0 || (w0 == 0 && tl12)) &&
                  (w1 > 0 || (w1 == 0 && tl20)) &&
                  (w2 > 0 || (w2 == 0 && tl01))))
                continue;

            uint8_t cr = attribute_plane_sample(&red, x, y);
            uint8_t cg = attribute_plane_sample(&green, x, y);
            uint8_t cb = attribute_plane_sample(&blue, x, y);
            uint16_t color = (dither && gpu->dithering)
                                 ? rgb_to_1555_dither(cr, cg, cb, x, y)
                                 : rgb_to_1555(cr, cg, cb);
            draw_pixel(gpu, x, y, color, semi);
        }
    }
}

/* Textured triangle: UV + per-vertex color modulation, all interpolated */
static void fill_triangle_tex(Gpu *gpu,
                              int32_t x0, int32_t y0, uint8_t u0, uint8_t v0, uint8_t r0, uint8_t g0, uint8_t b0,
                              int32_t x1, int32_t y1, uint8_t u1, uint8_t v1, uint8_t r1, uint8_t g1, uint8_t b1,
                              int32_t x2, int32_t y2, uint8_t u2, uint8_t v2, uint8_t r2, uint8_t g2, uint8_t b2,
                              bool blend, bool semi, const TextureCache *cache)
{
    int32_t sx0 = x0 * 2, sy0 = y0 * 2;
    int32_t sx1 = x1 * 2, sy1 = y1 * 2;
    int32_t sx2 = x2 * 2, sy2 = y2 * 2;
    int64_t area = edge_i64(sx0, sy0, sx1, sy1, sx2, sy2);
    if (area == 0)
        return;
    if (triangle_too_large(x0, y0, x1, y1, x2, y2))
        return;
    trace_big_triangle(gpu, "tex", x0, y0, x1, y1, x2, y2);

    int32_t min_x = x0 < x1 ? x0 : x1;
    if (x2 < min_x)
        min_x = x2;
    int32_t max_x = x0 > x1 ? x0 : x1;
    if (x2 > max_x)
        max_x = x2;
    int32_t min_y = y0 < y1 ? y0 : y1;
    if (y2 < min_y)
        min_y = y2;
    int32_t max_y = y0 > y1 ? y0 : y1;
    if (y2 > max_y)
        max_y = y2;

    bool neg = area < 0;
    if (neg)
        area = -area;
    bool tl01 = edge_accepts_zero(sx0, sy0, sx1, sy1, neg);
    bool tl12 = edge_accepts_zero(sx1, sy1, sx2, sy2, neg);
    bool tl20 = edge_accepts_zero(sx2, sy2, sx0, sy0, neg);
    GpuAttributePlane tex_u = attribute_plane(x0, y0, u0, x1, y1, u1, x2, y2, u2);
    GpuAttributePlane tex_v = attribute_plane(x0, y0, v0, x1, y1, v1, x2, y2, v2);
    GpuAttributePlane red = attribute_plane(x0, y0, r0, x1, y1, r1, x2, y2, r2);
    GpuAttributePlane green = attribute_plane(x0, y0, g0, x1, y1, g1, x2, y2, g2);
    GpuAttributePlane blue = attribute_plane(x0, y0, b0, x1, y1, b1, x2, y2, b2);

    for (int32_t y = min_y; y <= max_y; y++)
    {
        for (int32_t x = min_x; x <= max_x; x++)
        {
            int32_t sx = x * 2 + 1;
            int32_t sy = y * 2 + 1;
            int64_t w0 = edge_i64(sx1, sy1, sx2, sy2, sx, sy);
            int64_t w1 = edge_i64(sx2, sy2, sx0, sy0, sx, sy);
            int64_t w2 = edge_i64(sx0, sy0, sx1, sy1, sx, sy);
            if (neg)
            {
                w0 = -w0;
                w1 = -w1;
                w2 = -w2;
            }

            if (!((w0 > 0 || (w0 == 0 && tl12)) &&
                  (w1 > 0 || (w1 == 0 && tl20)) &&
                  (w2 > 0 || (w2 == 0 && tl01))))
                continue;

            uint8_t fu = attribute_plane_sample(&tex_u, x, y);
            uint8_t fv = attribute_plane_sample(&tex_v, x, y);
            uint8_t mcr = attribute_plane_sample(&red, x, y);
            uint8_t mcg = attribute_plane_sample(&green, x, y);
            uint8_t mcb = attribute_plane_sample(&blue, x, y);
            uint16_t texel = texel_fetch(gpu, cache, fu, fv);
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
                pixel = gpu->dithering
                            ? rgb_to_1555_dither((uint8_t)tr, (uint8_t)tg, (uint8_t)tb, x, y)
                            : rgb_to_1555((uint8_t)tr, (uint8_t)tg, (uint8_t)tb);
                pixel |= texel & 0x8000u;
            }
            else
            {
                pixel = texel;
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
    gpu->scanline = 0;
    gpu->line_phase = 0;
    gpu->dotclock_phase = 0;
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
static uint32_t gpu_status_odd_line(const Gpu *gpu)
{
    if (gpu->vres == VRES_480)
        return gpu->field == FIELD_TOP ? (1u << 31) : 0;
    return (gpu->scanline & 1u) ? (1u << 31) : 0;
}

uint32_t gpu_status(const Gpu *gpu)
{
    bool idle =
        gpu->busy_cycles == 0 &&
        gpu->gp0_mode != GP0_MODE_IMAGE_STORE;
    bool ready_vram_to_cpu =
        gpu->gp0_mode == GP0_MODE_IMAGE_STORE &&
        gpu->image_store_words_remaining > 0;
    bool ready_dma_block = idle && gpu->gp0_mode != GP0_MODE_IMAGE_STORE;

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
        ((uint32_t)idle << 26) |
        ((uint32_t)ready_vram_to_cpu << 27) |
        ((uint32_t)ready_dma_block << 28) |
        ((uint32_t)gpu->dma_direction << 29) |
        gpu_status_odd_line(gpu);

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
        dma_req = ready_dma_block;
        break;
    case DMA_DIR_VRAM_TO_CPU:
        dma_req = ready_vram_to_cpu;
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
    int32_t ax = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t ay = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t k = (ax > ay) ? ax : ay;
    if (ax >= MAX_PRIMITIVE_WIDTH || ay >= MAX_PRIMITIVE_HEIGHT)
        return;

    if (k == 0)
    {
        int32_t x = gpu_truncate_coord(x0);
        int32_t y = gpu_truncate_coord(y0);
        uint16_t color = gpu->dithering
                             ? rgb_to_1555_dither(r0, g0, b0, x, y)
                             : rgb_to_1555(r0, g0, b0);
        draw_pixel(gpu, x, y, color, semi);
        return;
    }

    if (x0 >= x1)
    {
        int32_t ti;
        uint8_t tc;
        ti = x0;
        x0 = x1;
        x1 = ti;
        ti = y0;
        y0 = y1;
        y1 = ti;
        tc = r0;
        r0 = r1;
        r1 = tc;
        tc = g0;
        g0 = g1;
        g1 = tc;
        tc = b0;
        b0 = b1;
        b1 = tc;
    }

    int64_t dx = (int64_t)x1 - x0;
    int64_t dy = (int64_t)y1 - y0;
    int64_t dxdk = ((dx << 32) - ((dx < 0) ? (k - 1) : 0) + ((dx > 0) ? (k - 1) : 0)) / k;
    int64_t dydk = ((dy << 32) - ((dy < 0) ? (k - 1) : 0) + ((dy > 0) ? (k - 1) : 0)) / k;
    int32_t drdk = (((int32_t)r1 - (int32_t)r0) << 12) / k;
    int32_t dgdk = (((int32_t)g1 - (int32_t)g0) << 12) / k;
    int32_t dbdk = (((int32_t)b1 - (int32_t)b0) << 12) / k;

    int64_t curx = ((int64_t)x0 << 32) + (1LL << 31) - 1024;
    int64_t cury = ((int64_t)y0 << 32) + (1LL << 31) - ((dydk < 0) ? 1024 : 0);
    int32_t curr = ((int32_t)r0 << 12) + (1 << 11);
    int32_t curg = ((int32_t)g0 << 12) + (1 << 11);
    int32_t curb = ((int32_t)b0 << 12) + (1 << 11);

    for (int32_t i = 0; i <= k; i++)
    {
        int32_t x = gpu_truncate_coord((int32_t)(curx >> 32));
        int32_t y = gpu_truncate_coord((int32_t)(cury >> 32));
        uint8_t cr = (uint8_t)(curr >> 12);
        uint8_t cg = (uint8_t)(curg >> 12);
        uint8_t cb = (uint8_t)(curb >> 12);
        uint16_t color = gpu->dithering
                             ? rgb_to_1555_dither(cr, cg, cb, x, y)
                             : rgb_to_1555(cr, cg, cb);
        draw_pixel(gpu, x, y, color, semi);
        curx += dxdk;
        cury += dydk;
        curr += drdk;
        curg += dgdk;
        curb += dbdk;
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
static void gp0_polyline_mono_start(Gpu *g);
static void gp0_polyline_shaded_start(Gpu *g);
static void gp0_image_load(Gpu *g);
static void gp0_image_store(Gpu *g);
static void gp0_vram_copy(Gpu *g);
static void gp0_draw_mode(Gpu *g);
static void gp0_texture_window(Gpu *g);
static void gp0_drawing_area_top_left(Gpu *g);
static void gp0_drawing_area_bottom_right(Gpu *g);
static void gp0_drawing_offset(Gpu *g);
static void gp0_mask_bit_setting(Gpu *g);
static void apply_offset(const Gpu *gpu, int32_t *x, int32_t *y);

/* ---- GP0 dispatch ---- */
static inline bool gp0_polyline_terminator(uint32_t val)
{
    return (val & 0xF000F000u) == 0x50005000u;
}

void gpu_add_busy_cycles(Gpu *gpu, uint32_t cycles)
{
    if (UINT32_MAX - gpu->busy_cycles < cycles)
        gpu->busy_cycles = UINT32_MAX;
    else
        gpu->busy_cycles += cycles;
}

uint32_t gpu_busy_cycles(const Gpu *gpu)
{
    return gpu->busy_cycles;
}

static uint32_t gp0_clamp_cost(uint64_t cycles)
{
    return cycles > 512u ? 512u : (uint32_t)cycles;
}

static uint32_t gp0_command_cost(const CommandBuffer *command)
{
    uint8_t op = (uint8_t)(command->buffer[0] >> 24);
    uint32_t w;
    uint32_t h;

    switch (op)
    {
    case 0x02:
        w = ((command->buffer[2] & 0x3FFu) + 15u) & ~15u;
        h = (command->buffer[2] >> 16) & 0x1FFu;
        return gp0_clamp_cost(23u + (4u + (w + 15u) / 16u) * (uint64_t)h);
    case 0x20 ... 0x23:
    case 0x28 ... 0x2B:
        return 23u;
    case 0x24 ... 0x27:
    case 0x2C ... 0x2F:
        return 113u;
    case 0x30 ... 0x33:
    case 0x38 ... 0x3B:
        return 167u;
    case 0x34 ... 0x37:
    case 0x3C ... 0x3F:
        return 248u;
    case 0x40 ... 0x47:
    case 0x50 ... 0x57:
        return 8u;
    case 0x60 ... 0x63:
        w = command->buffer[2] & 0x3FFu;
        h = (command->buffer[2] >> 16) & 0x1FFu;
        return gp0_clamp_cost(8u + ((uint64_t)w / 2u) * h);
    case 0x64 ... 0x67:
        w = command->buffer[3] & 0x3FFu;
        h = (command->buffer[3] >> 16) & 0x1FFu;
        return gp0_clamp_cost(8u + ((uint64_t)w / 2u) * h);
    case 0x68 ... 0x6F:
        return 8u;
    case 0x70 ... 0x77:
        return 8u + 4u * 8u;
    case 0x78 ... 0x7F:
        return 8u + 8u * 16u;
    case 0x80:
        w = command->buffer[3] & 0x3FFu;
        h = (command->buffer[3] >> 16) & 0x1FFu;
        if (w == 0)
            w = 1024;
        if (h == 0)
            h = 512;
        return gp0_clamp_cost((uint64_t)w * h);
    default:
        return 0;
    }
}

static void gp0_polyline_word(Gpu *gpu, uint32_t val)
{
    if (!gpu->polyline_shaded)
    {
        if (gp0_polyline_terminator(val))
        {
            gpu->gp0_mode = GP0_MODE_COMMAND;
            return;
        }

        int32_t x = gpu_xy_x(val);
        int32_t y = gpu_xy_y(val);
        apply_offset(gpu, &x, &y);
        uint32_t c = gpu->polyline_last_color;
        trace_prim("poly_mono seg (%d,%d)->(%d,%d) color=%02X,%02X,%02X",
                   gpu->polyline_last_x, gpu->polyline_last_y, x, y,
                   c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
        draw_line(gpu,
                  gpu->polyline_last_x, gpu->polyline_last_y,
                  c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF,
                  x, y,
                  c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF,
                  gpu->polyline_semi);
        gpu_add_busy_cycles(gpu, 8u);
        gpu->polyline_last_x = x;
        gpu->polyline_last_y = y;
        return;
    }

    if (gpu->polyline_expect_color)
    {
        if (gp0_polyline_terminator(val))
        {
            gpu->gp0_mode = GP0_MODE_COMMAND;
            return;
        }

        gpu->polyline_next_color = val & 0x00FFFFFFu;
        trace_prim("poly_shaded next_color raw=0x%08X rgb=%02X,%02X,%02X",
                   val,
                   gpu->polyline_next_color & 0xFF,
                   (gpu->polyline_next_color >> 8) & 0xFF,
                   (gpu->polyline_next_color >> 16) & 0xFF);
        gpu->polyline_expect_color = false;
        return;
    }

    int32_t x = gpu_xy_x(val);
    int32_t y = gpu_xy_y(val);
    apply_offset(gpu, &x, &y);
    uint32_t c0 = gpu->polyline_last_color;
    uint32_t c1 = gpu->polyline_next_color;
    trace_prim("poly_shaded seg raw_v=0x%08X (%d,%d)->(%d,%d) c0=%02X,%02X,%02X c1=%02X,%02X,%02X",
               val,
               gpu->polyline_last_x, gpu->polyline_last_y, x, y,
               c0 & 0xFF, (c0 >> 8) & 0xFF, (c0 >> 16) & 0xFF,
               c1 & 0xFF, (c1 >> 8) & 0xFF, (c1 >> 16) & 0xFF);
    draw_line(gpu,
              gpu->polyline_last_x, gpu->polyline_last_y,
              c0 & 0xFF, (c0 >> 8) & 0xFF, (c0 >> 16) & 0xFF,
              x, y,
              c1 & 0xFF, (c1 >> 8) & 0xFF, (c1 >> 16) & 0xFF,
              gpu->polyline_semi);
    gpu_add_busy_cycles(gpu, 8u);
    gpu->polyline_last_x = x;
    gpu->polyline_last_y = y;
    gpu->polyline_last_color = c1;
    gpu->polyline_expect_color = true;
}

void gpu_gp0(Gpu *gpu, uint32_t val)
{
    if (gpu->gp0_mode == GP0_MODE_POLYLINE)
    {
        gp0_polyline_word(gpu, val);
        return;
    }

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
            len = 3;
            method = gp0_line_mono;
            break;
        case 0x48:
        case 0x49:
        case 0x4A:
        case 0x4B:
        case 0x4C:
        case 0x4D:
        case 0x4E:
        case 0x4F:
            len = 2;
            method = gp0_polyline_mono_start;
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
            len = 4;
            method = gp0_line_shaded;
            break;
        case 0x58:
        case 0x59:
        case 0x5A:
        case 0x5B:
        case 0x5C:
        case 0x5D:
        case 0x5E:
        case 0x5F:
            len = 2;
            method = gp0_polyline_shaded_start;
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
        case 0x80:
            len = 4;
            method = gp0_vram_copy;
            break;
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
            debug_trace_gp0(gpu->frames, gpu->gp0_command.buffer, gpu->gp0_command.len,
                            gpu->drawing_x_offset, gpu->drawing_y_offset,
                            gpu->drawing_area_left, gpu->drawing_area_top,
                            gpu->drawing_area_right, gpu->drawing_area_bottom);
            gpu->gp0_command_method(gpu);
            gpu_add_busy_cycles(gpu, gp0_command_cost(&gpu->gp0_command));
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
    case GP0_MODE_POLYLINE:
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
    trace_prim("fill xy=(%u,%u) size=%ux%u color=%02X,%02X,%02X",
               x, y, w, h, r, g, b);
    if (gpu->texture_clut_cache_valid)
    {
        uint32_t entries = (gpu->texture_clut_cache_depth == TEXTURE_DEPTH_4BIT) ? 16u : 256u;
        uint32_t cache_x0 = gpu->texture_clut_cache_x;
        uint32_t cache_x1 = cache_x0 + entries;
        uint32_t fill_x0 = x;
        uint32_t fill_x1 = fill_x0 + w;
        uint32_t fill_y0 = y;
        uint32_t fill_y1 = fill_y0 + h;
        if (gpu->texture_clut_cache_y >= fill_y0 && gpu->texture_clut_cache_y < fill_y1 &&
            cache_x0 < fill_x1 && fill_x0 < cache_x1)
            gpu->texture_clut_cache_stale = true;
    }
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
    gpu->texture_depth = texture_depth_decode(v >> 7);
    gpu->dithering = ((v >> 9) & 1) != 0;
    gpu->draw_to_display = ((v >> 10) & 1) != 0;
    gpu->texture_disable =
        gpu->allow_texture_disable && ((v >> 11) & 1) != 0;
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
    int32_t x0 = gpu_xy_x(gpu->gp0_command.buffer[1]), y0 = gpu_xy_y(gpu->gp0_command.buffer[1]);
    int32_t x1 = gpu_xy_x(gpu->gp0_command.buffer[2]), y1 = gpu_xy_y(gpu->gp0_command.buffer[2]);
    int32_t x2 = gpu_xy_x(gpu->gp0_command.buffer[3]), y2 = gpu_xy_y(gpu->gp0_command.buffer[3]);
    apply_offset(gpu, &x0, &y0);
    apply_offset(gpu, &x1, &y1);
    apply_offset(gpu, &x2, &y2);
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    fill_triangle(gpu, x0, y0, r, g, b, x1, y1, r, g, b, x2, y2, r, g, b, semi, false);
}

/* ---- Mono quads ---- */
static void gp0_quad_mono_opaque(Gpu *gpu)
{
    uint32_t c = gpu->gp0_command.buffer[0];
    uint8_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    int32_t xs[4], ys[4];
    for (int i = 0; i < 4; i++)
    {
        xs[i] = gpu_xy_x(gpu->gp0_command.buffer[i + 1]);
        ys[i] = gpu_xy_y(gpu->gp0_command.buffer[i + 1]);
        apply_offset(gpu, &xs[i], &ys[i]);
    }
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    trace_prim("quad_mono op=0x%02X xy=(%d,%d)(%d,%d)(%d,%d)(%d,%d) color=%02X,%02X,%02X semi=%u mode=%u",
               (unsigned)(c >> 24), xs[0], ys[0], xs[1], ys[1], xs[2], ys[2], xs[3], ys[3],
               r, g, b, semi ? 1u : 0u, (unsigned)gpu->semi_transparency);
    fill_triangle(gpu, xs[0], ys[0], r, g, b, xs[2], ys[2], r, g, b, xs[1], ys[1], r, g, b, semi, false);
    fill_triangle(gpu, xs[1], ys[1], r, g, b, xs[2], ys[2], r, g, b, xs[3], ys[3], r, g, b, semi, false);
}

/* ---- Shaded triangles ---- */
static void gp0_triangle_shaded_opaque(Gpu *gpu)
{
    int32_t x0 = gpu_xy_x(gpu->gp0_command.buffer[1]), y0 = gpu_xy_y(gpu->gp0_command.buffer[1]);
    int32_t x1 = gpu_xy_x(gpu->gp0_command.buffer[3]), y1 = gpu_xy_y(gpu->gp0_command.buffer[3]);
    int32_t x2 = gpu_xy_x(gpu->gp0_command.buffer[5]), y2 = gpu_xy_y(gpu->gp0_command.buffer[5]);
    apply_offset(gpu, &x0, &y0);
    apply_offset(gpu, &x1, &y1);
    apply_offset(gpu, &x2, &y2);
    uint32_t c0 = gpu->gp0_command.buffer[0], c1 = gpu->gp0_command.buffer[2], c2 = gpu->gp0_command.buffer[4];
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    trace_prim("tri_shaded op=0x%02X xy=(%d,%d)(%d,%d)(%d,%d) c=(%02X,%02X,%02X)(%02X,%02X,%02X)(%02X,%02X,%02X) semi=%u dither=%u",
               (unsigned)(gpu->gp0_command.buffer[0] >> 24), x0, y0, x1, y1, x2, y2,
               c0 & 0xFF, (c0 >> 8) & 0xFF, (c0 >> 16) & 0xFF,
               c1 & 0xFF, (c1 >> 8) & 0xFF, (c1 >> 16) & 0xFF,
               c2 & 0xFF, (c2 >> 8) & 0xFF, (c2 >> 16) & 0xFF,
               semi ? 1u : 0u, gpu->dithering ? 1u : 0u);
    fill_triangle(gpu, x0, y0, c0 & 0xFF, (c0 >> 8) & 0xFF, (c0 >> 16) & 0xFF,
                  x1, y1, c1 & 0xFF, (c1 >> 8) & 0xFF, (c1 >> 16) & 0xFF,
                  x2, y2, c2 & 0xFF, (c2 >> 8) & 0xFF, (c2 >> 16) & 0xFF, semi, true);
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
        xs[i] = gpu_xy_x(gpu->gp0_command.buffer[i * 2 + 1]);
        ys[i] = gpu_xy_y(gpu->gp0_command.buffer[i * 2 + 1]);
        apply_offset(gpu, &xs[i], &ys[i]);
    }
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    fill_triangle(gpu, xs[0], ys[0], rs[0], gs[0], bs[0],
                  xs[2], ys[2], rs[2], gs[2], bs[2],
                  xs[1], ys[1], rs[1], gs[1], bs[1], semi, true);
    fill_triangle(gpu, xs[1], ys[1], rs[1], gs[1], bs[1],
                  xs[2], ys[2], rs[2], gs[2], bs[2],
                  xs[3], ys[3], rs[3], gs[3], bs[3], semi, true);
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

    int32_t x0 = gpu_xy_x(w0), y0 = gpu_xy_y(w0);
    uint8_t u0 = w1 & 0xFF, v0 = (w1 >> 8) & 0xFF;
    uint16_t clut_raw = (w1 >> 16) & 0xFFFF;
    gpu->clut_x = (clut_raw & 0x3F) * 16;
    gpu->clut_y = (clut_raw >> 6) & 0x1FF;

    int32_t x1 = gpu_xy_x(w2), y1 = gpu_xy_y(w2);
    uint8_t u1 = w3 & 0xFF, v1 = (w3 >> 8) & 0xFF;
    uint16_t page_raw = (w3 >> 16) & 0xFFFF;
    gpu->page_base_x = page_raw & 0x0F;
    gpu->page_base_y = (page_raw >> 4) & 1;
    gpu->texture_depth = texture_depth_decode(page_raw >> 7);

    int32_t x2 = gpu_xy_x(w4), y2 = gpu_xy_y(w4);
    uint8_t u2 = w5 & 0xFF, v2 = (w5 >> 8) & 0xFF;

    apply_offset(gpu, &x0, &y0);
    apply_offset(gpu, &x1, &y1);
    apply_offset(gpu, &x2, &y2);
    /* bit 0 of the command byte: 0 = blend modulation color, 1 = raw texture */
    bool blend = ((gpu->gp0_command.buffer[0] >> 24) & 0x01) == 0;
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    TextureCache cache;
    texture_cache_init(gpu, &cache);
    fill_triangle_tex(gpu, x0, y0, u0, v0, cr, cg, cb, x1, y1, u1, v1, cr, cg, cb, x2, y2, u2, v2, cr, cg, cb, blend, semi, &cache);
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
    xs[0] = gpu_xy_x(w);
    ys[0] = gpu_xy_y(w);
    w = gpu->gp0_command.buffer[2];
    us[0] = w & 0xFF;
    vs[0] = (w >> 8) & 0xFF;
    uint16_t clut_raw = (uint16_t)(w >> 16);
    gpu->clut_x = (clut_raw & 0x3F) * 16;
    gpu->clut_y = (clut_raw >> 6) & 0x1FF;

    w = gpu->gp0_command.buffer[3];
    xs[1] = gpu_xy_x(w);
    ys[1] = gpu_xy_y(w);
    w = gpu->gp0_command.buffer[4];
    us[1] = w & 0xFF;
    vs[1] = (w >> 8) & 0xFF;
    uint16_t page_raw = (uint16_t)(w >> 16);
    gpu->page_base_x = page_raw & 0x0F;
    gpu->page_base_y = (page_raw >> 4) & 1;
    gpu->texture_depth = texture_depth_decode(page_raw >> 7);

    w = gpu->gp0_command.buffer[5];
    xs[2] = gpu_xy_x(w);
    ys[2] = gpu_xy_y(w);
    w = gpu->gp0_command.buffer[6];
    us[2] = w & 0xFF;
    vs[2] = (w >> 8) & 0xFF;
    w = gpu->gp0_command.buffer[7];
    xs[3] = gpu_xy_x(w);
    ys[3] = gpu_xy_y(w);
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
    TextureCache cache;
    texture_cache_init(gpu, &cache);
    fill_triangle_tex(gpu, xs[0], ys[0], us[0], vs[0], cr, cg, cb,
                      xs[1], ys[1], us[1], vs[1], cr, cg, cb,
                      xs[3], ys[3], us[3], vs[3], cr, cg, cb, blend, semi, &cache);
    fill_triangle_tex(gpu, xs[0], ys[0], us[0], vs[0], cr, cg, cb,
                      xs[3], ys[3], us[3], vs[3], cr, cg, cb,
                      xs[2], ys[2], us[2], vs[2], cr, cg, cb, blend, semi, &cache);
}

/* ---- Shaded+textured triangle (9 words) ---- */
/* format: color0, xy0, uv0+clut, color1, xy1, uv1+page, color2, xy2, uv2 */
static void gp0_triangle_shaded_tex_opaque(Gpu *gpu)
{
    uint32_t c0 = gpu->gp0_command.buffer[0];
    uint8_t r0 = c0 & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = (c0 >> 16) & 0xFF;
    int32_t x0 = gpu_xy_x(gpu->gp0_command.buffer[1]), y0 = gpu_xy_y(gpu->gp0_command.buffer[1]);
    uint32_t uv0c = gpu->gp0_command.buffer[2];
    uint8_t u0 = uv0c & 0xFF, v0 = (uv0c >> 8) & 0xFF;
    uint16_t clut_raw = (uint16_t)(uv0c >> 16);
    gpu->clut_x = (clut_raw & 0x3F) * 16;
    gpu->clut_y = (clut_raw >> 6) & 0x1FF;

    uint32_t c1 = gpu->gp0_command.buffer[3];
    uint8_t r1 = c1 & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = (c1 >> 16) & 0xFF;
    int32_t x1 = gpu_xy_x(gpu->gp0_command.buffer[4]), y1 = gpu_xy_y(gpu->gp0_command.buffer[4]);
    uint32_t uv1p = gpu->gp0_command.buffer[5];
    uint8_t u1 = uv1p & 0xFF, v1 = (uv1p >> 8) & 0xFF;
    uint16_t page_raw = (uint16_t)(uv1p >> 16);
    gpu->page_base_x = page_raw & 0x0F;
    gpu->page_base_y = (page_raw >> 4) & 1;
    gpu->texture_depth = texture_depth_decode(page_raw >> 7);

    uint32_t c2 = gpu->gp0_command.buffer[6];
    uint8_t r2 = c2 & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = (c2 >> 16) & 0xFF;
    int32_t x2 = gpu_xy_x(gpu->gp0_command.buffer[7]), y2 = gpu_xy_y(gpu->gp0_command.buffer[7]);
    uint32_t uv2 = gpu->gp0_command.buffer[8];
    uint8_t u2 = uv2 & 0xFF, v2 = (uv2 >> 8) & 0xFF;

    apply_offset(gpu, &x0, &y0);
    apply_offset(gpu, &x1, &y1);
    apply_offset(gpu, &x2, &y2);
    bool blend = ((gpu->gp0_command.buffer[0] >> 24) & 0x01) == 0;
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    TextureCache cache;
    texture_cache_init(gpu, &cache);
    fill_triangle_tex(gpu, x0, y0, u0, v0, r0, g0, b0, x1, y1, u1, v1, r1, g1, b1, x2, y2, u2, v2, r2, g2, b2, blend, semi, &cache);
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
        xs[i] = gpu_xy_x(xy);
        ys[i] = gpu_xy_y(xy);
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
            gpu->texture_depth = texture_depth_decode(pr >> 7);
        }
        apply_offset(gpu, &xs[i], &ys[i]);
    }
    bool blend = ((gpu->gp0_command.buffer[0] >> 24) & 0x01) == 0;
    bool semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    TextureCache cache;
    texture_cache_init(gpu, &cache);
    fill_triangle_tex(gpu, xs[0], ys[0], us[0], vs[0], rs[0], gs[0], bs[0],
                      xs[1], ys[1], us[1], vs[1], rs[1], gs[1], bs[1],
                      xs[3], ys[3], us[3], vs[3], rs[3], gs[3], bs[3], blend, semi, &cache);
    fill_triangle_tex(gpu, xs[0], ys[0], us[0], vs[0], rs[0], gs[0], bs[0],
                      xs[3], ys[3], us[3], vs[3], rs[3], gs[3], bs[3],
                      xs[2], ys[2], us[2], vs[2], rs[2], gs[2], bs[2], blend, semi, &cache);
}

/* ---- Rectangle helpers ---- */
static void fill_rect_sw(Gpu *gpu, int32_t rx, int32_t ry, int32_t rw, int32_t rh, uint16_t color, bool semi)
{
    for (int32_t dy = 0; dy < rh; dy++)
        for (int32_t dx = 0; dx < rw; dx++)
            draw_pixel(gpu, rx + dx, ry + dy, color, semi);
}
static void fill_rect_tex_sw(Gpu *gpu, int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                             uint8_t u0, uint8_t v0, uint8_t cr, uint8_t cg, uint8_t cb, bool blend, bool semi,
                             const TextureCache *cache)
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
                fu = (uint8_t)(u0 + 1 - dx);
            uint16_t texel = texel_fetch(gpu, cache, fu, fv);
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
                pixel = rgb_to_1555((uint8_t)tr, (uint8_t)tg, (uint8_t)tb) | (texel & 0x8000u);
            }
            else
            {
                pixel = texel;
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
    int32_t rx = gpu_xy_x(xy), ry = gpu_xy_y(xy);
    apply_offset(gpu, &rx, &ry);
    rx = gpu_truncate_coord(rx);
    ry = gpu_truncate_coord(ry);
    bool semi = ((c >> 24) & 0x02) != 0;
    trace_prim("rect op=0x%02X xy=(%d,%d) size=%dx%d color=%02X,%02X,%02X semi=%u mode=%u",
               (unsigned)(c >> 24), rx, ry, w, h, r, g, b, semi ? 1u : 0u,
               (unsigned)gpu->semi_transparency);
    fill_rect_sw(gpu, rx, ry, w, h, rgb_to_1555(r, g, b), semi);
}
static void rect_common_var(Gpu *gpu)
{
    uint32_t c = gpu->gp0_command.buffer[0];
    uint8_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    uint32_t xy = gpu->gp0_command.buffer[1];
    int32_t rx = gpu_xy_x(xy), ry = gpu_xy_y(xy);
    apply_offset(gpu, &rx, &ry);
    rx = gpu_truncate_coord(rx);
    ry = gpu_truncate_coord(ry);
    uint32_t wh = gpu->gp0_command.buffer[2];
    int32_t w = wh & 0x3FF, h = (wh >> 16) & 0x1FF;
    bool semi = ((c >> 24) & 0x02) != 0;
    trace_prim("rect op=0x%02X xy=(%d,%d) size=%dx%d color=%02X,%02X,%02X semi=%u mode=%u",
               (unsigned)(c >> 24), rx, ry, w, h, r, g, b, semi ? 1u : 0u,
               (unsigned)gpu->semi_transparency);
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
    int32_t rx = gpu_xy_x(xy), ry = gpu_xy_y(xy);
    apply_offset(gpu, &rx, &ry);
    rx = gpu_truncate_coord(rx);
    ry = gpu_truncate_coord(ry);
    uint32_t uv_clut = gpu->gp0_command.buffer[2];
    uint8_t u0 = uv_clut & 0xFF, v0 = (uv_clut >> 8) & 0xFF;
    uint16_t clut_raw = (uint16_t)(uv_clut >> 16);
    gpu->clut_x = (clut_raw & 0x3F) * 16;
    gpu->clut_y = (clut_raw >> 6) & 0x1FF;
    bool blend = ((c >> 24) & 0x01) == 0;
    bool semi = ((c >> 24) & 0x02) != 0;
    trace_prim("rect_tex op=0x%02X xy=(%d,%d) size=%dx%d uv=(%u,%u) color=%02X,%02X,%02X blend=%u semi=%u mode=%u depth=%u clut=(%u,%u) page=(%u,%u)",
               (unsigned)(c >> 24), rx, ry, w, h, u0, v0, cr, cg, cb,
               blend ? 1u : 0u, semi ? 1u : 0u, (unsigned)gpu->semi_transparency,
               (unsigned)gpu->texture_depth, gpu->clut_x, gpu->clut_y,
               gpu->page_base_x, gpu->page_base_y);
    TextureCache cache;
    texture_cache_init(gpu, &cache);
    fill_rect_tex_sw(gpu, rx, ry, w, h, u0, v0, cr, cg, cb, blend, semi, &cache);
}
static void rect_tex_common_var(Gpu *gpu)
{
    uint32_t c = gpu->gp0_command.buffer[0];
    uint8_t cr = c & 0xFF, cg = (c >> 8) & 0xFF, cb = (c >> 16) & 0xFF;
    uint32_t xy = gpu->gp0_command.buffer[1];
    int32_t rx = gpu_xy_x(xy), ry = gpu_xy_y(xy);
    apply_offset(gpu, &rx, &ry);
    rx = gpu_truncate_coord(rx);
    ry = gpu_truncate_coord(ry);
    uint32_t uv_clut = gpu->gp0_command.buffer[2];
    uint8_t u0 = uv_clut & 0xFF, v0 = (uv_clut >> 8) & 0xFF;
    uint16_t clut_raw = (uint16_t)(uv_clut >> 16);
    gpu->clut_x = (clut_raw & 0x3F) * 16;
    gpu->clut_y = (clut_raw >> 6) & 0x1FF;
    uint32_t wh = gpu->gp0_command.buffer[3];
    int32_t w = wh & 0x3FF, h = (wh >> 16) & 0x1FF;
    bool blend = ((c >> 24) & 0x01) == 0;
    bool semi = ((c >> 24) & 0x02) != 0;
    trace_prim("rect_tex op=0x%02X xy=(%d,%d) size=%dx%d uv=(%u,%u) color=%02X,%02X,%02X blend=%u semi=%u mode=%u depth=%u clut=(%u,%u) page=(%u,%u)",
               (unsigned)(c >> 24), rx, ry, w, h, u0, v0, cr, cg, cb,
               blend ? 1u : 0u, semi ? 1u : 0u, (unsigned)gpu->semi_transparency,
               (unsigned)gpu->texture_depth, gpu->clut_x, gpu->clut_y,
               gpu->page_base_x, gpu->page_base_y);
    TextureCache cache;
    texture_cache_init(gpu, &cache);
    fill_rect_tex_sw(gpu, rx, ry, w, h, u0, v0, cr, cg, cb, blend, semi, &cache);
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
    texture_clut_cache_invalidate(gpu);
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

static void gp0_vram_copy(Gpu *gpu)
{
    uint32_t src = gpu->gp0_command.buffer[1];
    uint32_t dst = gpu->gp0_command.buffer[2];
    uint32_t size = gpu->gp0_command.buffer[3];
    uint32_t src_x = src & 0x3FFu;
    uint32_t src_y = (src >> 16) & 0x1FFu;
    uint32_t dst_x = dst & 0x3FFu;
    uint32_t dst_y = (dst >> 16) & 0x1FFu;
    uint32_t w = size & 0x3FFu;
    uint32_t h = (size >> 16) & 0x1FFu;
    if (w == 0)
        w = 1024;
    if (h == 0)
        h = 512;

    trace_prim("vram_copy src=(%u,%u) dst=(%u,%u) size=%ux%u",
               src_x, src_y, dst_x, dst_y, w, h);
    texture_clut_cache_invalidate(gpu);

    uint32_t count = w * h;
    uint16_t *tmp = malloc(count * sizeof(*tmp));
    if (!tmp)
        return;

    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            tmp[y * w + x] = vram_load(gpu->vram, src_x + x, src_y + y);

    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            uint32_t dx = (dst_x + x) & 1023u;
            uint32_t dy = (dst_y + y) & 511u;
            uint16_t color = tmp[y * w + x];
            if (gpu->preserve_masked_pixels && (gpu->vram[dy * 1024u + dx] & 0x8000u))
                continue;
            if (gpu->force_set_mask_bit)
                color |= 0x8000u;
            gpu->vram[dy * 1024u + dx] = color;
        }
    }

    free(tmp);
}

/* 0x40-0x47: mono line */
static void gp0_line_mono(Gpu *gpu)
{
    uint32_t op = gpu->gp0_command.buffer[0];
    uint32_t v0w = gpu->gp0_command.buffer[1];
    uint32_t v1w = gpu->gp0_command.buffer[2];
    bool semi = (op >> 25) & 1;
    uint8_t r = op & 0xFF, g = (op >> 8) & 0xFF, b = (op >> 16) & 0xFF;
    int32_t x0 = gpu_xy_x(v0w) + gpu->drawing_x_offset;
    int32_t y0 = gpu_xy_y(v0w) + gpu->drawing_y_offset;
    int32_t x1 = gpu_xy_x(v1w) + gpu->drawing_x_offset;
    int32_t y1 = gpu_xy_y(v1w) + gpu->drawing_y_offset;
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
    int32_t x0 = gpu_xy_x(v0w) + gpu->drawing_x_offset;
    int32_t y0 = gpu_xy_y(v0w) + gpu->drawing_y_offset;
    int32_t x1 = gpu_xy_x(v1w) + gpu->drawing_x_offset;
    int32_t y1 = gpu_xy_y(v1w) + gpu->drawing_y_offset;
    draw_line(gpu, x0, y0, r0, g0, b0, x1, y1, r1, g1, b1, semi);
}

static void gp0_polyline_mono_start(Gpu *gpu)
{
    uint32_t c = gpu->gp0_command.buffer[0] & 0x00FFFFFFu;
    uint32_t v = gpu->gp0_command.buffer[1];
    int32_t x = gpu_xy_x(v);
    int32_t y = gpu_xy_y(v);
    apply_offset(gpu, &x, &y);

    gpu->polyline_shaded = false;
    gpu->polyline_expect_color = false;
    gpu->polyline_semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    gpu->polyline_last_x = x;
    gpu->polyline_last_y = y;
    gpu->polyline_last_color = c;
    gpu->polyline_next_color = c;
    gpu->gp0_mode = GP0_MODE_POLYLINE;
}

static void gp0_polyline_shaded_start(Gpu *gpu)
{
    uint32_t c = gpu->gp0_command.buffer[0] & 0x00FFFFFFu;
    uint32_t v = gpu->gp0_command.buffer[1];
    int32_t x = gpu_xy_x(v);
    int32_t y = gpu_xy_y(v);
    apply_offset(gpu, &x, &y);

    gpu->polyline_shaded = true;
    gpu->polyline_expect_color = true;
    gpu->polyline_semi = ((gpu->gp0_command.buffer[0] >> 24) & 0x02) != 0;
    gpu->polyline_last_x = x;
    gpu->polyline_last_y = y;
    gpu->polyline_last_color = c;
    gpu->polyline_next_color = c;
    gpu->gp0_mode = GP0_MODE_POLYLINE;
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
static bool g_gp1_watch = false;
static bool g_gp1_dbg_init = false;
static uint32_t g_gp1_last_ops[8];
static uint32_t g_gp1_last_vals[8];
static uint32_t g_gp1_last_head = 0;

static void gp1_dbg_init(void)
{
    if (g_gp1_dbg_init)
        return;
    g_gp1_dbg_init = true;
    const char *e = getenv("PS1_GP1_LOOP_THRESH");
    if (e)
        g_gp1_loop_thresh = (uint32_t)strtoul(e, NULL, 10);
    g_gp1_watch = getenv("PS1_WATCH_GP1") != NULL;
}

void gpu_gp1_reset_consecutive(void) { g_gp1_consecutive = 0; }

void gpu_gp1(Gpu *gpu, uint32_t val)
{
    gp1_dbg_init();
    uint32_t op = (val >> 24) & 0xFF;
    LOG(LOG_GPU, "GP1 op=0x%02X val=0x%08X", op, val);

    /* Track consecutive GP1 calls (no GP0 draw between them) */
    g_gp1_last_ops[g_gp1_last_head & 7] = op;
    g_gp1_last_vals[g_gp1_last_head & 7] = val;
    g_gp1_last_head++;
    g_gp1_consecutive++;

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
    case 0x09:
        gpu->allow_texture_disable = (val & 1u) != 0;
        break;
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

    if (g_gp1_watch)
        fprintf(stderr,
                "GP1[%u] op=0x%02X val=0x%08X disp_dis=%d vram_xy=(%u,%u) range=(%u-%u,%u-%u) hres=%u depth=%s\n",
                g_gp1_consecutive, op, val, gpu->display_disabled,
                gpu->display_vram_x_start, gpu->display_vram_y_start,
                gpu->display_horiz_start, gpu->display_horiz_end,
                gpu->display_line_start, gpu->display_line_end,
                hres_width(gpu->hres),
                gpu->display_depth == DISPLAY_DEPTH_24 ? "24" : "15");
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
    gpu->allow_texture_disable = false;
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
    gpu->busy_cycles = 0;
    texture_clut_cache_invalidate(gpu);
    gp1_reset_command_buffer(gpu);
}

static void gp1_reset_command_buffer(Gpu *gpu)
{
    cb_clear(&gpu->gp0_command);
    gpu->gp0_words_remaining = 0;
    gpu->gp0_mode = GP0_MODE_COMMAND;
    gpu->polyline_shaded = false;
    gpu->polyline_expect_color = false;
    gpu->polyline_semi = false;
    gpu->polyline_last_x = 0;
    gpu->polyline_last_y = 0;
    gpu->polyline_last_color = 0;
    gpu->polyline_next_color = 0;
    gpu->busy_cycles = 0;
}

static uint32_t gpu_lines_per_frame(const Gpu *gpu)
{
    return gpu->vmode == VMODE_PAL ? GPU_PAL_LINES_PER_FRAME
                                   : GPU_NTSC_LINES_PER_FRAME;
}

static uint32_t gpu_cpu_cycles_per_frame(const Gpu *gpu)
{
    return gpu->vmode == VMODE_PAL ? 33868800u / 50u : 33868800u / 60u;
}

static uint32_t gpu_dotclock_divider(const Gpu *gpu)
{
    return display_dotclock_divider(gpu);
}

static uint32_t gpu_clocks_per_line(const Gpu *gpu)
{
    return gpu->vmode == VMODE_PAL ? GPU_PAL_CLOCKS_PER_LINE
                                   : GPU_NTSC_CLOCKS_PER_LINE;
}

static uint64_t gpu_hblank_phase(const Gpu *gpu)
{
    return (uint64_t)gpu_cpu_cycles_per_frame(gpu) *
           GPU_HBLANK_START_CLOCK / gpu_clocks_per_line(gpu);
}

bool gpu_in_hblank(const Gpu *gpu)
{
    return gpu->hblank;
}

bool gpu_in_vblank(const Gpu *gpu)
{
    return gpu->scanline >= 240u;
}

uint32_t gpu_cycles_until_timing_boundary(const Gpu *gpu)
{
    uint64_t target = gpu->hblank ? gpu_cpu_cycles_per_frame(gpu)
                                  : gpu_hblank_phase(gpu);
    if (gpu->line_phase >= target)
        return 1;

    uint64_t phase_left = target - gpu->line_phase;
    uint32_t lines = gpu_lines_per_frame(gpu);
    uint64_t cycles = (phase_left + lines - 1u) / lines;
    if (cycles == 0)
        return 1;
    return cycles > UINT32_MAX ? UINT32_MAX : (uint32_t)cycles;
}

GpuTimingEvents gpu_step(Gpu *gpu, uint32_t cycles)
{
    GpuTimingEvents events = {0};
    uint32_t lines = gpu_lines_per_frame(gpu);
    uint32_t frame_cycles = gpu_cpu_cycles_per_frame(gpu);
    uint32_t dotclock_denominator = 7u * gpu_dotclock_divider(gpu);

    if (gpu->busy_cycles > cycles)
        gpu->busy_cycles -= cycles;
    else
        gpu->busy_cycles = 0;

    gpu->dotclock_phase += (uint64_t)cycles * 11u;
    events.dotclock_ticks = (uint32_t)(gpu->dotclock_phase / dotclock_denominator);
    gpu->dotclock_phase %= dotclock_denominator;

    uint64_t phase_advance = (uint64_t)cycles * lines;
    while (phase_advance > 0)
    {
        uint64_t target = gpu->hblank ? frame_cycles : gpu_hblank_phase(gpu);
        uint64_t phase_left = target - gpu->line_phase;
        if (phase_advance < phase_left)
        {
            gpu->line_phase += phase_advance;
            break;
        }

        gpu->line_phase = target;
        phase_advance -= phase_left;
        if (!gpu->hblank)
        {
            gpu->hblank = true;
            events.hblank_count++;
            continue;
        }

        gpu->line_phase = 0;
        gpu->hblank = false;
        gpu->scanline++;
        if (gpu->scanline == 240u)
            events.vblank_started = true;
        if (gpu->scanline >= lines)
        {
            gpu->scanline = 0;
            gpu->frames++;
            events.frame_ended = true;
        }
    }

    return events;
}

void gpu_vblank_start(Gpu *gpu)
{
    if (gpu->interlaced)
        gpu->field = (gpu->field == FIELD_TOP) ? FIELD_BOTTOM : FIELD_TOP;
}

/* Present the completed frame after the vertical blank interval. */
void gpu_vblank(Gpu *gpu)
{
    static bool dumped = false;
    static uint32_t frame_count = 0;

    DisplayRect display = display_rect_from_gpu(gpu);
    uint16_t display_x = display.x;
    uint16_t display_y = display.y;
    uint16_t w = display.w;
    uint16_t h = display.h;
    bool display_24bit = gpu->display_depth == DISPLAY_DEPTH_24;
    debug_trace_frame(frame_count + 1u, display_x, display_y, w, h, display_24bit,
                      gpu->display_disabled, g_frame_pixels_written);
    renderer_display(&gpu->renderer, gpu->vram, display_x, display_y, w, h,
                     display_24bit);
    gpu->frame_updated = true;

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
                    uint32_t sx = (display_x + x) & 1023u;
                    uint32_t sy = (display_y + y) & 511u;
                    if ((gpu->vram[sy * 1024u + sx] & 0x7FFFu) != 0)
                        nonzero++;
                }
            }
            fprintf(stderr,
                    "GPU_FRAME %u writes=%llu display_nonzero=%u display=(%u,%u %ux%u) disabled=%d depth=%s hres=%u\n",
                    frame_count, (unsigned long long)g_frame_pixels_written, nonzero,
                    display_x, display_y, w, h,
                    gpu->display_disabled,
                    display_24bit ? "24" : "15",
                    hres_width(gpu->hres));
        }
        g_frame_pixels_written = 0;
    }
    const char *dump_path = getenv("PS1_DUMP_VRAM_PPM");
    const char *dump_frame_env = getenv("PS1_DUMP_FRAME");
    bool dump_full_vram = getenv("PS1_DUMP_FULL_VRAM") != NULL;
    bool dump_stp_alpha = getenv("PS1_DUMP_STP_ALPHA") != NULL;
    uint32_t dump_frame = dump_frame_env ? (uint32_t)strtoul(dump_frame_env, NULL, 10) : 120u;
    if (dump_path && !dumped && frame_count >= dump_frame)
    {
        FILE *f = fopen(dump_path, "wb");
        if (f)
        {
            uint32_t dump_w = dump_full_vram ? 1024u : w;
            uint32_t dump_h = dump_full_vram ? 512u : h;
            /* P7 PAM: 4 channels (RGBA) so the STP/mask bit is preserved as alpha */
            fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
                    dump_w, dump_h);
            for (uint32_t y = 0; y < dump_h; y++)
            {
                for (uint32_t x = 0; x < dump_w; x++)
                {
                    uint8_t r8, g8, b8, a8;
                    if (!dump_full_vram && display_24bit)
                    {
                        uint32_t byte_base = (((display_y + y) & 511u) * 1024u +
                                              (display_x & 1023u)) *
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
                        a8 = 255u;
                    }
                    else
                    {
                        uint32_t sx = dump_full_vram ? x : ((display_x + x) & 1023u);
                        uint32_t sy = dump_full_vram ? y : ((display_y + y) & 511u);
                        uint16_t c = gpu->vram[sy * 1024u + sx];
                        uint8_t r5 = c & 0x1Fu;
                        uint8_t g5 = (c >> 5) & 0x1Fu;
                        uint8_t b5 = (c >> 10) & 0x1Fu;
                        r8 = r5 << 3;
                        g8 = g5 << 3;
                        b8 = b5 << 3;
                        a8 = dump_stp_alpha ? ((c & 0x8000u) ? 255u : 0u) : 255u;
                    }
                    fputc(r8, f);
                    fputc(g8, f);
                    fputc(b8, f);
                    fputc(a8, f);
                }
            }
            fclose(f);
        }
        dumped = true;
    }
}
