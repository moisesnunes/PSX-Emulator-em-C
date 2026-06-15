#include "gpu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VRAM_WIDTH 1024
#define VRAM_HEIGHT 512
#define VRAM_PIXELS (VRAM_WIDTH * VRAM_HEIGHT)

typedef struct
{
    int16_t x;
    int16_t y;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t u;
    uint8_t v;
} Vertex;

static int passed;
static int failed;

static void check(int condition, const char *name)
{
    if (condition)
    {
        passed++;
        return;
    }
    fprintf(stderr, "FAIL: %s\n", name);
    failed++;
}

static uint16_t rgb5(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(r | ((uint16_t)g << 5) | ((uint16_t)b << 10));
}

static uint16_t rgb8(uint8_t r, uint8_t g, uint8_t b)
{
    return rgb5(r >> 3, g >> 3, b >> 3);
}

static uint16_t dithered_rgb8(uint8_t r, uint8_t g, uint8_t b, int x, int y)
{
    static const int8_t matrix[4][4] = {
        {-4, 0, -3, 1},
        {2, -2, 3, -1},
        {-3, 1, -4, 0},
        {3, -1, 2, -2},
    };
    int values[3] = {r, g, b};
    for (int i = 0; i < 3; i++)
    {
        values[i] += matrix[y & 3][x & 3];
        if (values[i] < 0)
            values[i] = 0;
        if (values[i] > 255)
            values[i] = 255;
    }
    return rgb8((uint8_t)values[0], (uint8_t)values[1], (uint8_t)values[2]);
}

static uint32_t color_word(uint8_t opcode, const Vertex *v)
{
    return ((uint32_t)opcode << 24) |
           ((uint32_t)v->b << 16) |
           ((uint32_t)v->g << 8) |
           v->r;
}

static uint32_t xy_word(int x, int y)
{
    return ((uint32_t)y & 0x7ffu) << 16 | ((uint32_t)x & 0x7ffu);
}

static uint32_t uv_word(const Vertex *v, uint16_t high)
{
    return ((uint32_t)high << 16) | ((uint32_t)v->v << 8) | v->u;
}

static void init_gpu(Gpu *gpu)
{
    gpu_init(gpu, NULL);
    gpu_gp0(gpu, 0xe3000000u);
    gpu_gp0(gpu, 0xe4000000u | (511u << 10) | 1023u);
}

static void set_clip(Gpu *gpu, int left, int top, int right, int bottom)
{
    gpu_gp0(gpu, 0xe3000000u | ((uint32_t)top << 10) | (uint32_t)left);
    gpu_gp0(gpu, 0xe4000000u | ((uint32_t)bottom << 10) | (uint32_t)right);
}

static void set_offset(Gpu *gpu, int x, int y)
{
    gpu_gp0(gpu, 0xe5000000u |
                     (((uint32_t)y & 0x7ffu) << 11) |
                     ((uint32_t)x & 0x7ffu));
}

static void set_draw_mode(Gpu *gpu, TextureDepth depth, int semi_mode, int dither)
{
    uint32_t depth_bits = depth == TEXTURE_DEPTH_15BIT ? 2u : (uint32_t)depth;
    gpu_gp0(gpu, 0xe1000000u |
                     ((uint32_t)(semi_mode & 3) << 5) |
                     (depth_bits << 7) |
                     ((uint32_t)(dither != 0) << 9));
}

static void draw_gouraud(Gpu *gpu, const Vertex vertices[3], int semi)
{
    gpu_gp0(gpu, color_word(semi ? 0x32 : 0x30, &vertices[0]));
    gpu_gp0(gpu, xy_word(vertices[0].x, vertices[0].y));
    gpu_gp0(gpu, color_word(0, &vertices[1]));
    gpu_gp0(gpu, xy_word(vertices[1].x, vertices[1].y));
    gpu_gp0(gpu, color_word(0, &vertices[2]));
    gpu_gp0(gpu, xy_word(vertices[2].x, vertices[2].y));
}

static uint16_t tpage_word(TextureDepth depth, int semi_mode)
{
    uint16_t depth_bits = depth == TEXTURE_DEPTH_15BIT ? 2u : (uint16_t)depth;
    return (uint16_t)(0x10u | ((uint16_t)(semi_mode & 3) << 5) |
                      (depth_bits << 7));
}

static void draw_textured_gouraud(Gpu *gpu, const Vertex vertices[3],
                                  TextureDepth depth, int semi_mode, int semi)
{
    const uint16_t clut = 48u;
    gpu_gp0(gpu, color_word(semi ? 0x36 : 0x34, &vertices[0]));
    gpu_gp0(gpu, xy_word(vertices[0].x, vertices[0].y));
    gpu_gp0(gpu, uv_word(&vertices[0], clut));
    gpu_gp0(gpu, color_word(0, &vertices[1]));
    gpu_gp0(gpu, xy_word(vertices[1].x, vertices[1].y));
    gpu_gp0(gpu, uv_word(&vertices[1], tpage_word(depth, semi_mode)));
    gpu_gp0(gpu, color_word(0, &vertices[2]));
    gpu_gp0(gpu, xy_word(vertices[2].x, vertices[2].y));
    gpu_gp0(gpu, uv_word(&vertices[2], 0));
}

static void install_texture(Gpu *gpu, TextureDepth depth, uint16_t texel)
{
    const int page_y = 256;
    const int clut_x = 768;

    if (depth == TEXTURE_DEPTH_4BIT)
    {
        gpu->vram[clut_x + 1] = texel;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 16; x++)
                gpu->vram[(page_y + y) * VRAM_WIDTH + x] = 0x1111u;
    }
    else if (depth == TEXTURE_DEPTH_8BIT)
    {
        gpu->vram[clut_x + 1] = texel;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 32; x++)
                gpu->vram[(page_y + y) * VRAM_WIDTH + x] = 0x0101u;
    }
    else
    {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                gpu->vram[(page_y + y) * VRAM_WIDTH + x] = texel;
    }
}

static size_t count_changed(const Gpu *gpu, uint16_t initial)
{
    size_t count = 0;
    for (size_t i = 0; i < VRAM_PIXELS; i++)
        count += gpu->vram[i] != initial;
    return count;
}

static int compare_vram(const uint16_t *a, const uint16_t *b)
{
    return memcmp(a, b, VRAM_PIXELS * sizeof(*a)) == 0;
}

static int compare_gouraud_permutation(const uint16_t *a, const uint16_t *b)
{
    for (size_t i = 0; i < VRAM_PIXELS; i++)
    {
        uint16_t pa = a[i];
        uint16_t pb = b[i];
        if ((pa == 0) != (pb == 0) || ((pa ^ pb) & 0x8000u) != 0)
            return 0;
        if (pa == 0)
            continue;

        for (int shift = 0; shift <= 10; shift += 5)
        {
            int ca = (pa >> shift) & 31;
            int cb = (pb >> shift) & 31;
            int difference = ca > cb ? ca - cb : cb - ca;
            if (difference > 1)
                return 0;
        }
    }
    return 1;
}

static void permute_vertices(Vertex out[3], const Vertex in[3], const int order[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = in[order[i]];
}

static void test_untextured_permutations(void)
{
    static const int permutations[6][3] = {
        {0, 1, 2}, {1, 2, 0}, {2, 0, 1},
        {0, 2, 1}, {2, 1, 0}, {1, 0, 2},
    };
    const Vertex base[3] = {
        {80, 60, 255, 16, 80, 0, 0},
        {190, 92, 24, 240, 64, 0, 0},
        {112, 205, 32, 72, 255, 0, 0},
    };
    uint16_t *reference = malloc(VRAM_PIXELS * sizeof(*reference));
    check(reference != NULL, "allocate permutation reference");
    if (!reference)
        return;

    for (int dither = 0; dither <= 1; dither++)
    {
        for (int p = 0; p < 6; p++)
        {
            Gpu gpu;
            Vertex vertices[3];
            init_gpu(&gpu);
            set_draw_mode(&gpu, TEXTURE_DEPTH_4BIT, 0, dither);
            permute_vertices(vertices, base, permutations[p]);
            draw_gouraud(&gpu, vertices, 0);
            if (p == 0)
                memcpy(reference, gpu.vram, VRAM_PIXELS * sizeof(*reference));
            else
            {
                char name[96];
                snprintf(name, sizeof(name),
                         "untextured permutation %d dither=%d", p, dither);
                check(compare_gouraud_permutation(reference, gpu.vram), name);
            }
            gpu_destroy(&gpu);
        }
    }
    free(reference);
}

static void test_triangle_shapes(void)
{
    static const int permutations[6][3] = {
        {0, 1, 2}, {1, 2, 0}, {2, 0, 1},
        {0, 2, 1}, {2, 1, 0}, {1, 0, 2},
    };
    static const Vertex shapes[][3] = {
        {{40, 40, 255, 0, 0, 0, 0}, {180, 40, 0, 255, 0, 0, 0}, {92, 160, 0, 0, 255, 0, 0}},
        {{40, 160, 255, 0, 0, 0, 0}, {180, 160, 0, 255, 0, 0, 0}, {92, 40, 0, 0, 255, 0, 0}},
        {{70, 40, 255, 0, 0, 0, 0}, {72, 220, 0, 255, 0, 0, 0}, {73, 42, 0, 0, 255, 0, 0}},
    };

    for (size_t shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); shape++)
    {
        uint16_t *reference = malloc(VRAM_PIXELS * sizeof(*reference));
        check(reference != NULL, "allocate shape reference");
        if (!reference)
            return;
        for (int p = 0; p < 6; p++)
        {
            Gpu gpu;
            Vertex vertices[3];
            init_gpu(&gpu);
            permute_vertices(vertices, shapes[shape], permutations[p]);
            draw_gouraud(&gpu, vertices, 0);
            if (p == 0)
            {
                char name[80];
                snprintf(name, sizeof(name), "shape %zu renders pixels", shape);
                check(count_changed(&gpu, 0) > 0, name);
                memcpy(reference, gpu.vram, VRAM_PIXELS * sizeof(*reference));
            }
            else
            {
                char name[96];
                snprintf(name, sizeof(name),
                         "shape %zu permutation %d", shape, p);
                check(compare_gouraud_permutation(reference, gpu.vram), name);
            }
            gpu_destroy(&gpu);
        }
        free(reference);
    }

    Gpu gpu;
    const Vertex degenerate[3] = {
        {40, 40, 255, 0, 0, 0, 0},
        {80, 80, 0, 255, 0, 0, 0},
        {160, 160, 0, 0, 255, 0, 0},
    };
    init_gpu(&gpu);
    draw_gouraud(&gpu, degenerate, 0);
    check(count_changed(&gpu, 0) == 0, "degenerate triangle draws no pixels");
    gpu_destroy(&gpu);
}

static void check_clip_result(const Gpu *gpu, int left, int top, int right,
                              int bottom, const char *name)
{
    size_t inside = 0;
    size_t outside = 0;
    for (int y = 0; y < VRAM_HEIGHT; y++)
    {
        for (int x = 0; x < VRAM_WIDTH; x++)
        {
            if (gpu->vram[y * VRAM_WIDTH + x] == 0)
                continue;
            if (x >= left && x <= right && y >= top && y <= bottom)
                inside++;
            else
                outside++;
        }
    }
    char detail[112];
    snprintf(detail, sizeof(detail), "%s writes inside clip", name);
    check(inside > 0, detail);
    snprintf(detail, sizeof(detail), "%s never writes outside clip", name);
    check(outside == 0, detail);
}

static void test_clipping_and_coordinate_limits(void)
{
    static const Vertex cases[][3] = {
        {{20, 55, 255, 0, 0, 0, 0}, {70, 20, 0, 255, 0, 0, 0}, {70, 100, 0, 0, 255, 0, 0}},
        {{65, 20, 255, 0, 0, 0, 0}, {110, 55, 0, 255, 0, 0, 0}, {65, 100, 0, 0, 255, 0, 0}},
        {{20, 65, 255, 0, 0, 0, 0}, {110, 65, 0, 255, 0, 0, 0}, {65, 20, 0, 0, 255, 0, 0}},
        {{20, 65, 255, 0, 0, 0, 0}, {110, 65, 0, 255, 0, 0, 0}, {65, 110, 0, 0, 255, 0, 0}},
    };
    static const char *names[] = {"left", "right", "top", "bottom"};

    for (int i = 0; i < 4; i++)
    {
        Gpu gpu;
        init_gpu(&gpu);
        set_clip(&gpu, 40, 40, 90, 90);
        draw_gouraud(&gpu, cases[i], 0);
        check_clip_result(&gpu, 40, 40, 90, 90, names[i]);
        gpu_destroy(&gpu);
    }

    {
        Gpu gpu;
        const Vertex near_min[3] = {
            {-1024, 30, 255, 0, 0, 0, 0},
            {-980, 70, 0, 255, 0, 0, 0},
            {-1024, 110, 0, 0, 255, 0, 0},
        };
        init_gpu(&gpu);
        set_clip(&gpu, 0, 0, 80, 160);
        set_offset(&gpu, 1023, 0);
        draw_gouraud(&gpu, near_min, 0);
        check_clip_result(&gpu, 0, 0, 80, 160, "coordinate -1024");
        gpu_destroy(&gpu);
    }
    {
        Gpu gpu;
        const Vertex near_max[3] = {
            {980, 30, 255, 0, 0, 0, 0},
            {1023, 70, 0, 255, 0, 0, 0},
            {980, 110, 0, 0, 255, 0, 0},
        };
        init_gpu(&gpu);
        set_clip(&gpu, 0, 0, 80, 160);
        set_offset(&gpu, -960, 0);
        draw_gouraud(&gpu, near_max, 0);
        check_clip_result(&gpu, 0, 0, 80, 160, "coordinate 1023");
        gpu_destroy(&gpu);
    }
}

static void test_shared_edges(void)
{
    Gpu gpu;
    const uint16_t background = rgb5(4, 4, 4);
    const uint16_t once = rgb5(12, 12, 12);
    const Vertex first[3] = {
        {100, 100, 64, 64, 64, 0, 0},
        {132, 100, 64, 64, 64, 0, 0},
        {100, 132, 64, 64, 64, 0, 0},
    };
    const Vertex second[3] = {
        {132, 100, 64, 64, 64, 0, 0},
        {132, 132, 64, 64, 64, 0, 0},
        {100, 132, 64, 64, 64, 0, 0},
    };
    size_t holes = 0;
    size_t overlaps = 0;

    init_gpu(&gpu);
    set_draw_mode(&gpu, TEXTURE_DEPTH_4BIT, 1, 0);
    for (int y = 100; y < 132; y++)
        for (int x = 100; x < 132; x++)
            gpu.vram[y * VRAM_WIDTH + x] = background;
    draw_gouraud(&gpu, first, 1);
    draw_gouraud(&gpu, second, 1);

    for (int y = 100; y < 132; y++)
    {
        for (int x = 100; x < 132; x++)
        {
            uint16_t pixel = gpu.vram[y * VRAM_WIDTH + x];
            holes += pixel == background;
            overlaps += pixel != background && pixel != once;
        }
    }
    check(holes == 0, "shared edge has no holes");
    check(overlaps == 0, "shared edge has no double blending");
    gpu_destroy(&gpu);
}

static void test_dithering(void)
{
    const Vertex triangle[3] = {
        {40, 40, 127, 127, 127, 0, 0},
        {180, 40, 127, 127, 127, 0, 0},
        {40, 180, 127, 127, 127, 0, 0},
    };
    Gpu off;
    Gpu on;
    init_gpu(&off);
    init_gpu(&on);
    set_draw_mode(&off, TEXTURE_DEPTH_4BIT, 0, 0);
    set_draw_mode(&on, TEXTURE_DEPTH_4BIT, 0, 1);
    draw_gouraud(&off, triangle, 0);
    draw_gouraud(&on, triangle, 0);
    check(!compare_vram(off.vram, on.vram), "dithering changes Gouraud output");
    check(off.vram[80 * VRAM_WIDTH + 80] == rgb8(127, 127, 127),
          "dithering disabled uses direct RGB555");
    check(on.vram[80 * VRAM_WIDTH + 80] ==
              dithered_rgb8(127, 127, 127, 80, 80),
          "dithering enabled follows the 4x4 matrix");
    check(on.vram[80 * VRAM_WIDTH + 81] ==
              dithered_rgb8(127, 127, 127, 81, 80),
          "dithering matrix advances with X");
    gpu_destroy(&off);
    gpu_destroy(&on);
}

static uint16_t blend_expected(uint16_t background, uint16_t foreground, int mode)
{
    uint32_t br = background & 31u;
    uint32_t bg = (background >> 5) & 31u;
    uint32_t bb = (background >> 10) & 31u;
    uint32_t fr = foreground & 31u;
    uint32_t fg = (foreground >> 5) & 31u;
    uint32_t fb = (foreground >> 10) & 31u;
    uint32_t out[3];
    uint32_t back[3] = {br, bg, bb};
    uint32_t front[3] = {fr, fg, fb};

    for (int i = 0; i < 3; i++)
    {
        if (mode == 0)
            out[i] = (back[i] + front[i]) / 2;
        else if (mode == 1)
            out[i] = back[i] + front[i];
        else if (mode == 2)
            out[i] = back[i] > front[i] ? back[i] - front[i] : 0;
        else
            out[i] = back[i] + front[i] / 4;
        if (out[i] > 31)
            out[i] = 31;
    }
    return rgb5((uint8_t)out[0], (uint8_t)out[1], (uint8_t)out[2]);
}

static void test_semitransparency_modes(void)
{
    const Vertex triangle[3] = {
        {40, 40, 80, 96, 112, 0, 0},
        {180, 40, 80, 96, 112, 0, 0},
        {40, 180, 80, 96, 112, 0, 0},
    };
    const uint16_t background = rgb5(6, 18, 25);
    const uint16_t foreground = rgb8(80, 96, 112);

    for (int mode = 0; mode < 4; mode++)
    {
        Gpu gpu;
        char name[80];
        init_gpu(&gpu);
        set_draw_mode(&gpu, TEXTURE_DEPTH_4BIT, mode, 0);
        gpu.vram[80 * VRAM_WIDTH + 80] = background;
        draw_gouraud(&gpu, triangle, 1);
        snprintf(name, sizeof(name), "semi-transparency Gouraud mode %d", mode);
        check(gpu.vram[80 * VRAM_WIDTH + 80] ==
                  blend_expected(background, foreground, mode),
              name);
        gpu_destroy(&gpu);
    }
}

static void test_textured_depth_and_permutations(void)
{
    static const int permutations[6][3] = {
        {0, 1, 2}, {1, 2, 0}, {2, 0, 1},
        {0, 2, 1}, {2, 1, 0}, {1, 0, 2},
    };
    const Vertex base[3] = {
        {80, 60, 255, 32, 48, 0, 0},
        {190, 92, 32, 255, 64, 31, 0},
        {112, 205, 48, 64, 255, 0, 31},
    };

    for (TextureDepth depth = TEXTURE_DEPTH_4BIT;
         depth <= TEXTURE_DEPTH_15BIT; depth++)
    {
        uint16_t *reference = malloc(VRAM_PIXELS * sizeof(*reference));
        check(reference != NULL, "allocate textured permutation reference");
        if (!reference)
            return;
        for (int p = 0; p < 6; p++)
        {
            Gpu gpu;
            Vertex vertices[3];
            init_gpu(&gpu);
            set_draw_mode(&gpu, depth, 0, 0);
            install_texture(&gpu, depth, 0x7fffu);
            permute_vertices(vertices, base, permutations[p]);
            draw_textured_gouraud(&gpu, vertices, depth, 0, 0);
            if (p == 0)
            {
                char name[96];
                snprintf(name, sizeof(name),
                         "textured Gouraud depth=%d renders pixels", depth);
                check(gpu.vram[110 * VRAM_WIDTH + 120] != 0, name);
                memcpy(reference, gpu.vram, VRAM_PIXELS * sizeof(*reference));
            }
            else
            {
                char name[96];
                snprintf(name, sizeof(name),
                         "textured permutation %d depth=%d", p, depth);
                check(compare_vram(reference, gpu.vram), name);
            }
            gpu_destroy(&gpu);
        }
        free(reference);
    }
}

static void test_textured_stp_and_mask_bits(void)
{
    const Vertex triangle[3] = {
        {40, 40, 128, 128, 128, 0, 0},
        {180, 40, 128, 128, 128, 31, 0},
        {40, 180, 128, 128, 128, 0, 31},
    };
    const uint16_t background = rgb5(4, 4, 4);
    const uint16_t texel = rgb5(16, 16, 16);

    {
        Gpu gpu;
        init_gpu(&gpu);
        set_draw_mode(&gpu, TEXTURE_DEPTH_15BIT, 1, 0);
        install_texture(&gpu, TEXTURE_DEPTH_15BIT, texel);
        gpu.vram[80 * VRAM_WIDTH + 80] = background;
        draw_textured_gouraud(&gpu, triangle, TEXTURE_DEPTH_15BIT, 1, 1);
        check(gpu.vram[80 * VRAM_WIDTH + 80] == texel,
              "textured semi command ignores texels with STP clear");
        gpu_destroy(&gpu);
    }
    {
        Gpu gpu;
        init_gpu(&gpu);
        set_draw_mode(&gpu, TEXTURE_DEPTH_15BIT, 1, 0);
        install_texture(&gpu, TEXTURE_DEPTH_15BIT, texel | 0x8000u);
        gpu.vram[80 * VRAM_WIDTH + 80] = background;
        draw_textured_gouraud(&gpu, triangle, TEXTURE_DEPTH_15BIT, 1, 1);
        check(gpu.vram[80 * VRAM_WIDTH + 80] ==
                  blend_expected(background, texel, 1),
              "textured semi command blends texels with STP set");
        gpu_destroy(&gpu);
    }
    {
        Gpu gpu;
        init_gpu(&gpu);
        set_draw_mode(&gpu, TEXTURE_DEPTH_15BIT, 0, 0);
        install_texture(&gpu, TEXTURE_DEPTH_15BIT, texel);
        gpu_gp0(&gpu, 0xe6000001u);
        draw_textured_gouraud(&gpu, triangle, TEXTURE_DEPTH_15BIT, 0, 0);
        check((gpu.vram[80 * VRAM_WIDTH + 80] & 0x8000u) != 0,
              "force-mask sets bit 15 on textured Gouraud output");
        gpu_destroy(&gpu);
    }
    {
        Gpu gpu;
        const uint16_t protected_pixel = 0x8001u;
        init_gpu(&gpu);
        set_draw_mode(&gpu, TEXTURE_DEPTH_15BIT, 0, 0);
        install_texture(&gpu, TEXTURE_DEPTH_15BIT, texel);
        gpu.vram[80 * VRAM_WIDTH + 80] = protected_pixel;
        gpu_gp0(&gpu, 0xe6000002u);
        draw_textured_gouraud(&gpu, triangle, TEXTURE_DEPTH_15BIT, 0, 0);
        check(gpu.vram[80 * VRAM_WIDTH + 80] == protected_pixel,
              "preserve-mask protects textured Gouraud destination");
        check(gpu.vram[80 * VRAM_WIDTH + 81] != 0,
              "preserve-mask does not suppress adjacent pixels");
        gpu_destroy(&gpu);
    }
}

int main(void)
{
    test_untextured_permutations();
    test_triangle_shapes();
    test_clipping_and_coordinate_limits();
    test_shared_edges();
    test_dithering();
    test_semitransparency_modes();
    test_textured_depth_and_permutations();
    test_textured_stp_and_mask_bits();

    printf("gpu Gouraud conformance: pass=%d fail=%d\n", passed, failed);
    if (failed == 0)
        printf("scope: GP0 regression matrix; hardware RAW references still required for certification\n");
    return failed == 0 ? 0 : 1;
}
