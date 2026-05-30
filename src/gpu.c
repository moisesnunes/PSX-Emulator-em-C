#include "gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VRAM_WIDTH  1024
#define VRAM_HEIGHT 512

/* ---- HorizontalRes ---- */
HorizontalRes hres_from_fields(uint8_t hr1, uint8_t hr2) {
    HorizontalRes hr;
    hr.hr = (hr2 & 1) | ((hr1 & 3) << 1);
    return hr;
}

uint32_t hres_into_status(HorizontalRes hr) {
    return (uint32_t)hr.hr << 16;
}

/* ---- CommandBuffer ---- */
static void cb_clear(CommandBuffer *cb) { cb->len = 0; }

static void cb_push(CommandBuffer *cb, uint32_t word) {
    cb->buffer[cb->len++] = word;
}

/* ---- Forward declarations for GP0 methods ---- */
static void gp0_nop(Gpu *gpu);
static void gp0_clear_cache(Gpu *gpu);
static void gp0_quad_mono_opaque(Gpu *gpu);
static void gp0_quad_texture_blend_opaque(Gpu *gpu);
static void gp0_triangle_shaded_opaque(Gpu *gpu);
static void gp0_quad_shaded_opaque(Gpu *gpu);
static void gp0_image_load(Gpu *gpu);
static void gp0_image_store(Gpu *gpu);
static void gp0_draw_mode(Gpu *gpu);
static void gp0_texture_window(Gpu *gpu);
static void gp0_drawing_area_top_left(Gpu *gpu);
static void gp0_drawing_area_bottom_right(Gpu *gpu);
static void gp0_drawing_offset(Gpu *gpu);
static void gp0_mask_bit_setting(Gpu *gpu);

/* ---- Init ---- */
void gpu_init(Gpu *gpu, SDL_Window *window) {
    memset(gpu, 0, sizeof(*gpu));
    gpu->vram = calloc(VRAM_WIDTH * VRAM_HEIGHT, sizeof(uint16_t));
    if (!gpu->vram) { fprintf(stderr, "Failed to allocate VRAM\n"); exit(1); }
    gpu->texture_depth    = TEXTURE_DEPTH_4BIT;
    gpu->field            = FIELD_TOP;
    gpu->vres             = VRES_240;
    gpu->vmode            = VMODE_NTSC;
    gpu->display_depth    = DISPLAY_DEPTH_15;
    gpu->display_disabled = true;
    gpu->dma_direction    = DMA_DIR_OFF;
    gpu->hres             = hres_from_fields(0, 0);
    gpu->gp0_command_method = gp0_nop;
    gpu->gp0_mode           = GP0_MODE_COMMAND;
    if (window) renderer_init(&gpu->renderer, window);
}

void gpu_destroy(Gpu *gpu) {
    renderer_destroy(&gpu->renderer);
    free(gpu->vram);
    gpu->vram = NULL;
}

/* ---- Status ---- */
uint32_t gpu_status(const Gpu *gpu) {
    uint32_t r =
        ((uint32_t)gpu->page_base_x           << 0)  |
        ((uint32_t)gpu->page_base_y           << 4)  |
        ((uint32_t)gpu->semi_transparency     << 5)  |
        ((uint32_t)gpu->texture_depth         << 7)  |
        ((uint32_t)gpu->dithering             << 9)  |
        ((uint32_t)gpu->draw_to_display       << 10) |
        ((uint32_t)gpu->force_set_mask_bit    << 11) |
        ((uint32_t)gpu->preserve_masked_pixels<< 12) |
        ((uint32_t)gpu->field                 << 13) |
        ((uint32_t)gpu->texture_disable       << 15) |
        hres_into_status(gpu->hres)                  |
        ((uint32_t)gpu->vmode                 << 20) |
        ((uint32_t)gpu->display_depth         << 21) |
        ((uint32_t)gpu->interlaced            << 22) |
        ((uint32_t)gpu->display_disabled      << 23) |
        ((uint32_t)gpu->interrupt             << 24) |
        (1u << 26) | (1u << 27) | (1u << 28)        |
        ((uint32_t)gpu->dma_direction         << 29) |
        (0u << 31);

    uint32_t dma_request;
    switch (gpu->dma_direction) {
        case DMA_DIR_OFF:         dma_request = 0;              break;
        case DMA_DIR_FIFO:        dma_request = 1;              break;
        case DMA_DIR_CPU_TO_GP0:  dma_request = (r >> 28) & 1; break;
        case DMA_DIR_VRAM_TO_CPU: dma_request = (r >> 27) & 1; break;
        default:                  dma_request = 0;              break;
    }
    return r | (dma_request << 25);
}

uint32_t gpu_read(Gpu *gpu) {
    if (gpu->gp0_mode != GP0_MODE_IMAGE_STORE || gpu->image_store_words_remaining == 0)
        return 0;

    uint16_t p0 = 0, p1 = 0;

    uint32_t ax = gpu->image_store_x + gpu->image_store_cur_x;
    uint32_t ay = gpu->image_store_y + gpu->image_store_cur_y;
    p0 = gpu->vram[(ay % 512) * 1024 + (ax % 1024)];
    gpu->image_store_cur_x++;
    if (gpu->image_store_cur_x >= gpu->image_store_w) {
        gpu->image_store_cur_x = 0;
        gpu->image_store_cur_y++;
    }

    if (gpu->image_store_words_remaining > 1) {
        uint32_t bx = gpu->image_store_x + gpu->image_store_cur_x;
        uint32_t by = gpu->image_store_y + gpu->image_store_cur_y;
        p1 = gpu->vram[(by % 512) * 1024 + (bx % 1024)];
        gpu->image_store_cur_x++;
        if (gpu->image_store_cur_x >= gpu->image_store_w) {
            gpu->image_store_cur_x = 0;
            gpu->image_store_cur_y++;
        }
    }

    gpu->image_store_words_remaining--;
    if (gpu->image_store_words_remaining == 0)
        gpu->gp0_mode = GP0_MODE_COMMAND;

    return ((uint32_t)p1 << 16) | p0;
}

/* ---- GP0 ---- */
void gpu_gp0(Gpu *gpu, uint32_t val) {
    if (gpu->gp0_words_remaining == 0) {
        uint32_t opcode = (val >> 24) & 0xFF;
        uint32_t len;
        Gp0Method method;
        switch (opcode) {
            case 0x00: len = 1; method = gp0_nop;                       break;
            case 0x01: len = 1; method = gp0_clear_cache;               break;
            case 0x28: len = 5; method = gp0_quad_mono_opaque;          break;
            case 0x2C: len = 9; method = gp0_quad_texture_blend_opaque; break;
            case 0x30: len = 6; method = gp0_triangle_shaded_opaque;    break;
            case 0x38: len = 8; method = gp0_quad_shaded_opaque;        break;
            case 0xA0: len = 3; method = gp0_image_load;                break;
            case 0xC0: len = 3; method = gp0_image_store;               break;
            case 0xE1: len = 1; method = gp0_draw_mode;                 break;
            case 0xE2: len = 1; method = gp0_texture_window;            break;
            case 0xE3: len = 1; method = gp0_drawing_area_top_left;     break;
            case 0xE4: len = 1; method = gp0_drawing_area_bottom_right; break;
            case 0xE5: len = 1; method = gp0_drawing_offset;            break;
            case 0xE6: len = 1; method = gp0_mask_bit_setting;          break;
            default:
                fprintf(stderr, "Unhandled GP0 command: %08X\n", val);
                exit(1);
        }
        gpu->gp0_words_remaining = len;
        gpu->gp0_command_method  = method;
        cb_clear(&gpu->gp0_command);
    }
    gpu->gp0_words_remaining--;

    switch (gpu->gp0_mode) {
        case GP0_MODE_COMMAND:
            cb_push(&gpu->gp0_command, val);
            if (gpu->gp0_words_remaining == 0)
                gpu->gp0_command_method(gpu);
            break;
        case GP0_MODE_IMAGE_LOAD: {
            uint16_t pixels[2] = { (uint16_t)(val & 0xFFFF), (uint16_t)(val >> 16) };
            for (int i = 0; i < 2; i++) {
                if (gpu->image_load_cur_y >= gpu->image_load_h) break;
                uint32_t ax = (gpu->image_load_x + gpu->image_load_cur_x) % 1024;
                uint32_t ay = (gpu->image_load_y + gpu->image_load_cur_y) % 512;
                gpu->vram[ay * 1024 + ax] = pixels[i];
                gpu->image_load_cur_x++;
                if (gpu->image_load_cur_x >= gpu->image_load_w) {
                    gpu->image_load_cur_x = 0;
                    gpu->image_load_cur_y++;
                }
            }
            if (gpu->gp0_words_remaining == 0)
                gpu->gp0_mode = GP0_MODE_COMMAND;
            break;
        }
        case GP0_MODE_IMAGE_STORE:
            /* pixels são lidos via gpu_read(), não chegam pelo GP0 */
            break;
    }
}

/* ---- GP0 handlers ---- */
static void gp0_nop(Gpu *gpu)         { (void)gpu; }
static void gp0_clear_cache(Gpu *gpu) { (void)gpu; }

static void gp0_draw_mode(Gpu *gpu) {
    uint32_t val = gpu->gp0_command.buffer[0];
    gpu->page_base_x = val & 0x0F;
    gpu->page_base_y = (val >> 4) & 1;
    gpu->semi_transparency = (val >> 5) & 3;
    switch ((val >> 7) & 3) {
        case 0: gpu->texture_depth = TEXTURE_DEPTH_4BIT;  break;
        case 1: gpu->texture_depth = TEXTURE_DEPTH_8BIT;  break;
        case 2: gpu->texture_depth = TEXTURE_DEPTH_15BIT; break;
        default: fprintf(stderr, "Unhandled texture depth\n"); exit(1);
    }
    gpu->dithering                 = ((val >> 9) & 1) != 0;
    gpu->draw_to_display           = ((val >> 10) & 1) != 0;
    gpu->texture_disable           = ((val >> 11) & 1) != 0;
    gpu->rectangle_texture_x_flip  = ((val >> 12) & 1) != 0;
    gpu->rectangle_texture_y_flip  = ((val >> 13) & 1) != 0;
}

static void gp0_drawing_area_top_left(Gpu *gpu) {
    uint32_t val = gpu->gp0_command.buffer[0];
    gpu->drawing_area_top  = (val >> 10) & 0x03FF;
    gpu->drawing_area_left = val & 0x03FF;
}

static void gp0_drawing_area_bottom_right(Gpu *gpu) {
    uint32_t val = gpu->gp0_command.buffer[0];
    gpu->drawing_area_bottom = (val >> 10) & 0x03FF;
    gpu->drawing_area_right  = val & 0x03FF;
}

static void gp0_drawing_offset(Gpu *gpu) {
    uint32_t val = gpu->gp0_command.buffer[0];
    uint16_t x = val & 0x07FF;
    uint16_t y = (val >> 11) & 0x07FF;
    gpu->drawing_x_offset = (int16_t)((x << 5)) >> 5;
    gpu->drawing_y_offset = (int16_t)((y << 5)) >> 5;
    renderer_set_draw_offset(&gpu->renderer, gpu->drawing_x_offset, gpu->drawing_y_offset);
    renderer_display(&gpu->renderer);
    gpu->frame_updated = true;
}

static void gp0_texture_window(Gpu *gpu) {
    uint32_t val = gpu->gp0_command.buffer[0];
    gpu->texture_window_x_mask   = val & 0x1F;
    gpu->texture_window_y_mask   = (val >> 5) & 0x1F;
    gpu->texture_window_x_offset = (val >> 10) & 0x1F;
    gpu->texture_window_y_offset = (val >> 15) & 0x1F;
}

static void gp0_mask_bit_setting(Gpu *gpu) {
    uint32_t val = gpu->gp0_command.buffer[0];
    gpu->force_set_mask_bit     = (val & 1) != 0;
    gpu->preserve_masked_pixels = (val & 2) != 0;
}

static void gp0_quad_mono_opaque(Gpu *gpu) {
    Position positions[4] = {
        position_from_gp0(gpu->gp0_command.buffer[1]),
        position_from_gp0(gpu->gp0_command.buffer[2]),
        position_from_gp0(gpu->gp0_command.buffer[3]),
        position_from_gp0(gpu->gp0_command.buffer[4]),
    };
    Color c = color_from_gp0(gpu->gp0_command.buffer[0]);
    Color colors[4] = { c, c, c, c };
    renderer_push_quad(&gpu->renderer, positions, colors);
}

static void gp0_image_load(Gpu *gpu) {
    uint32_t dest = gpu->gp0_command.buffer[1];
    uint32_t res  = gpu->gp0_command.buffer[2];

    uint16_t x = dest & 0x3FF;
    uint16_t y = (dest >> 16) & 0x1FF;
    uint16_t w = res & 0xFFFF;
    uint16_t h = res >> 16;

    if (w == 0 || h == 0) return;

    gpu->image_load_x     = x;
    gpu->image_load_y     = y;
    gpu->image_load_w     = w;
    gpu->image_load_h     = h;
    gpu->image_load_cur_x = 0;
    gpu->image_load_cur_y = 0;

    uint32_t npixels = (uint32_t)w * h;
    uint32_t nwords  = (npixels + 1) / 2;
    gpu->gp0_words_remaining = nwords;
    gpu->gp0_mode = GP0_MODE_IMAGE_LOAD;
}

static void gp0_image_store(Gpu *gpu) {
    uint32_t src = gpu->gp0_command.buffer[1];
    uint32_t res = gpu->gp0_command.buffer[2];

    uint16_t x = src & 0x3FF;
    uint16_t y = (src >> 16) & 0x1FF;
    uint16_t w = res & 0xFFFF;
    uint16_t h = res >> 16;

    if (w == 0 || h == 0) return;

    gpu->image_store_x                = x;
    gpu->image_store_y                = y;
    gpu->image_store_w                = w;
    gpu->image_store_h                = h;
    gpu->image_store_cur_x            = 0;
    gpu->image_store_cur_y            = 0;
    uint32_t npixels                  = (uint32_t)w * h;
    gpu->image_store_words_remaining  = (npixels + 1) / 2;
    gpu->gp0_mode = GP0_MODE_IMAGE_STORE;
}

static void gp0_quad_shaded_opaque(Gpu *gpu) {
    Position positions[4] = {
        position_from_gp0(gpu->gp0_command.buffer[1]),
        position_from_gp0(gpu->gp0_command.buffer[3]),
        position_from_gp0(gpu->gp0_command.buffer[5]),
        position_from_gp0(gpu->gp0_command.buffer[7]),
    };
    Color colors[4] = {
        color_from_gp0(gpu->gp0_command.buffer[0]),
        color_from_gp0(gpu->gp0_command.buffer[2]),
        color_from_gp0(gpu->gp0_command.buffer[4]),
        color_from_gp0(gpu->gp0_command.buffer[6]),
    };
    renderer_push_quad(&gpu->renderer, positions, colors);
}

static void gp0_triangle_shaded_opaque(Gpu *gpu) {
    Position positions[3] = {
        position_from_gp0(gpu->gp0_command.buffer[1]),
        position_from_gp0(gpu->gp0_command.buffer[3]),
        position_from_gp0(gpu->gp0_command.buffer[5]),
    };
    Color colors[3] = {
        color_from_gp0(gpu->gp0_command.buffer[0]),
        color_from_gp0(gpu->gp0_command.buffer[2]),
        color_from_gp0(gpu->gp0_command.buffer[4]),
    };
    renderer_push_triangle(&gpu->renderer, positions, colors);
}

static void gp0_quad_texture_blend_opaque(Gpu *gpu) {
    Position positions[4] = {
        position_from_gp0(gpu->gp0_command.buffer[1]),
        position_from_gp0(gpu->gp0_command.buffer[3]),
        position_from_gp0(gpu->gp0_command.buffer[5]),
        position_from_gp0(gpu->gp0_command.buffer[7]),
    };
    Color colors[4] = {
        {0x80, 0x00, 0x00},
        {0x80, 0x00, 0x00},
        {0x80, 0x00, 0x00},
        {0x80, 0x00, 0x00},
    };
    renderer_push_quad(&gpu->renderer, positions, colors);
}

/* ---- GP1 ---- */
static void gp1_reset(Gpu *gpu);
static void gp1_reset_command_buffer(Gpu *gpu);
static void gp1_acknowledge_irq(Gpu *gpu, uint32_t val);
static void gp1_display_enable(Gpu *gpu, uint32_t val);
static void gp1_dma_direction(Gpu *gpu, uint32_t val);
static void gp1_display_vram_start(Gpu *gpu, uint32_t val);
static void gp1_display_horizontal_range(Gpu *gpu, uint32_t val);
static void gp1_display_vertical_range(Gpu *gpu, uint32_t val);
static void gp1_display_mode(Gpu *gpu, uint32_t val);

void gpu_gp1(Gpu *gpu, uint32_t val) {
    uint32_t opcode = (val >> 24) & 0xFF;
    switch (opcode) {
        case 0x00: gp1_reset(gpu);                        break;
        case 0x01: gp1_reset_command_buffer(gpu);         break;
        case 0x02: gp1_acknowledge_irq(gpu, val);         break;
        case 0x03: gp1_display_enable(gpu, val);          break;
        case 0x04: gp1_dma_direction(gpu, val);           break;
        case 0x05: gp1_display_vram_start(gpu, val);      break;
        case 0x06: gp1_display_horizontal_range(gpu, val);break;
        case 0x07: gp1_display_vertical_range(gpu, val);  break;
        case 0x08: gp1_display_mode(gpu, val);            break;
        default:
            fprintf(stderr, "Unhandled GP1 command: %08X\n", val);
            exit(1);
    }
}

static void gp1_reset(Gpu *gpu) {
    gpu->interrupt             = false;
    gpu->page_base_x           = 0;
    gpu->page_base_y           = 0;
    gpu->semi_transparency     = 0;
    gpu->texture_depth         = TEXTURE_DEPTH_4BIT;
    gpu->texture_window_x_mask = gpu->texture_window_y_mask = 0;
    gpu->texture_window_x_offset = gpu->texture_window_y_offset = 0;
    gpu->dithering             = false;
    gpu->draw_to_display       = false;
    gpu->texture_disable       = false;
    gpu->rectangle_texture_x_flip = gpu->rectangle_texture_y_flip = false;
    gpu->drawing_area_left     = gpu->drawing_area_top    = 0;
    gpu->drawing_area_right    = gpu->drawing_area_bottom = 0;
    gpu->drawing_x_offset      = gpu->drawing_y_offset   = 0;
    gpu->force_set_mask_bit    = gpu->preserve_masked_pixels = false;
    gpu->dma_direction         = DMA_DIR_OFF;
    gpu->display_disabled      = true;
    gpu->display_vram_x_start  = gpu->display_vram_y_start = 0;
    gpu->hres                  = hres_from_fields(0, 0);
    gpu->vres                  = VRES_240;
    gpu->vmode                 = VMODE_NTSC;
    gpu->interlaced            = true;
    gpu->display_horiz_start   = 0x0200;
    gpu->display_horiz_end     = 0x0C00;
    gpu->display_line_start    = 0x0010;
    gpu->display_line_end      = 0x0100;
    gpu->display_depth         = DISPLAY_DEPTH_15;
}

static void gp1_reset_command_buffer(Gpu *gpu) {
    cb_clear(&gpu->gp0_command);
    gpu->gp0_words_remaining = 0;
    gpu->gp0_mode = GP0_MODE_COMMAND;
}

static void gp1_acknowledge_irq(Gpu *gpu, uint32_t val) { (void)val; gpu->interrupt = false; }

static void gp1_display_enable(Gpu *gpu, uint32_t val) {
    gpu->display_disabled = val & 1;
}

static void gp1_dma_direction(Gpu *gpu, uint32_t val) {
    switch (val & 3) {
        case 0: gpu->dma_direction = DMA_DIR_OFF;         break;
        case 1: gpu->dma_direction = DMA_DIR_FIFO;        break;
        case 2: gpu->dma_direction = DMA_DIR_CPU_TO_GP0;  break;
        case 3: gpu->dma_direction = DMA_DIR_VRAM_TO_CPU; break;
    }
}

static void gp1_display_vram_start(Gpu *gpu, uint32_t val) {
    gpu->display_vram_x_start = (val & 0x03FE);
    gpu->display_vram_y_start = (val >> 10) & 0x1FF;
}

static void gp1_display_horizontal_range(Gpu *gpu, uint32_t val) {
    gpu->display_horiz_start = val & 0x0FFF;
    gpu->display_horiz_end   = (val >> 12) & 0x0FFF;
}

static void gp1_display_vertical_range(Gpu *gpu, uint32_t val) {
    gpu->display_line_start = val & 0x03FF;
    gpu->display_line_end   = (val >> 10) & 0x03FF;
}

static void gp1_display_mode(Gpu *gpu, uint32_t val) {
    uint8_t hr1 = val & 3;
    uint8_t hr2 = (val >> 6) & 1;
    gpu->hres       = hres_from_fields(hr1, hr2);
    gpu->vres       = (val & 0x04) ? VRES_480 : VRES_240;
    gpu->vmode      = (val & 0x08) ? VMODE_PAL : VMODE_NTSC;
    gpu->display_depth = (val & 0x10) ? DISPLAY_DEPTH_15 : DISPLAY_DEPTH_24;
    gpu->interlaced = (val & 0x20) != 0;
    if (val & 0x80) {
        fprintf(stderr, "Unsupported display mode: %08X\n", val);
        exit(1);
    }
}
