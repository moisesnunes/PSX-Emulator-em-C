#pragma once
#include "renderer.h"
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

typedef enum
{
    TEXTURE_DEPTH_4BIT = 0,
    TEXTURE_DEPTH_8BIT = 1,
    TEXTURE_DEPTH_15BIT = 2
} TextureDepth;
typedef enum
{
    FIELD_TOP = 1,
    FIELD_BOTTOM = 0
} Field;
typedef enum
{
    VRES_240 = 0,
    VRES_480 = 1
} VerticalRes;
typedef enum
{
    VMODE_NTSC = 0,
    VMODE_PAL = 1
} VMode;
typedef enum
{
    DISPLAY_DEPTH_15 = 0,
    DISPLAY_DEPTH_24 = 1
} DisplayDepth;
typedef enum
{
    DMA_DIR_OFF = 0,
    DMA_DIR_FIFO = 1,
    DMA_DIR_CPU_TO_GP0 = 2,
    DMA_DIR_VRAM_TO_CPU = 3
} DmaDirection;
typedef enum
{
    GP0_MODE_COMMAND = 0,
    GP0_MODE_IMAGE_LOAD = 1,
    GP0_MODE_IMAGE_STORE = 2,
    GP0_MODE_POLYLINE = 3
} Gp0Mode;

typedef struct
{
    uint8_t hr;
} HorizontalRes;
HorizontalRes hres_from_fields(uint8_t hr1, uint8_t hr2);
uint32_t hres_into_status(HorizontalRes hr);

#define COMMAND_BUFFER_LEN 16
typedef struct
{
    uint32_t buffer[COMMAND_BUFFER_LEN];
    uint8_t len;
} CommandBuffer;

typedef struct Gpu Gpu;
typedef void (*Gp0Method)(Gpu *);

struct Gpu
{
    /* Draw state */
    uint8_t page_base_x;
    uint8_t page_base_y;
    uint8_t semi_transparency;
    TextureDepth texture_depth;
    bool dithering;
    bool draw_to_display;
    bool force_set_mask_bit;
    bool preserve_masked_pixels;
    Field field;
    bool texture_disable;
    bool rectangle_texture_x_flip;
    bool rectangle_texture_y_flip;

    uint8_t texture_window_x_mask;
    uint8_t texture_window_y_mask;
    uint8_t texture_window_x_offset;
    uint8_t texture_window_y_offset;

    uint16_t drawing_area_left;
    uint16_t drawing_area_top;
    uint16_t drawing_area_right;
    uint16_t drawing_area_bottom;
    int16_t drawing_x_offset;
    int16_t drawing_y_offset;

    /* CLUT address (updated from texture commands) */
    uint16_t clut_x;
    uint16_t clut_y;

    /* Display state */
    HorizontalRes hres;
    VerticalRes vres;
    VMode vmode;
    DisplayDepth display_depth;
    bool interlaced;
    bool display_disabled;
    bool interrupt;
    DmaDirection dma_direction;

    uint16_t display_vram_x_start;
    uint16_t display_vram_y_start;
    uint16_t display_horiz_start;
    uint16_t display_horiz_end;
    uint16_t display_line_start;
    uint16_t display_line_end;

    /* GP0 command accumulator */
    CommandBuffer gp0_command;
    uint32_t gp0_words_remaining;
    Gp0Method gp0_command_method;
    Gp0Mode gp0_mode;
    uint32_t gpuread_latch;

    /* Streaming polyline state (GP0 0x48-0x4F / 0x58-0x5F) */
    bool polyline_shaded;
    bool polyline_expect_color;
    bool polyline_semi;
    int32_t polyline_last_x;
    int32_t polyline_last_y;
    uint32_t polyline_last_color;
    uint32_t polyline_next_color;

    /* VRAM: 1024x512 x 16-bit (ABGR1555), heap-allocated */
    uint16_t *vram;

    /* VRAM transfer state */
    uint16_t image_load_x, image_load_y, image_load_w, image_load_h;
    uint32_t image_load_cur_x, image_load_cur_y;
    uint16_t image_store_x, image_store_y, image_store_w, image_store_h;
    uint32_t image_store_cur_x, image_store_cur_y, image_store_words_remaining;

    Renderer renderer;
    bool frame_updated;
    int32_t vblank_cycles_left;
    uint32_t frames;
};

void gpu_init(Gpu *gpu, SDL_Window *window);
uint32_t gpu_status(const Gpu *gpu);
uint32_t gpu_read(Gpu *gpu);
void gpu_gp0(Gpu *gpu, uint32_t val);
void gpu_gp1(Gpu *gpu, uint32_t val);
void gpu_gp1_reset_consecutive(void);
bool gpu_step(Gpu *gpu, uint32_t cycles);
void gpu_vblank(Gpu *gpu);
void gpu_destroy(Gpu *gpu);
