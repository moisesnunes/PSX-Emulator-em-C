#ifndef PSX_GPU_H
#define PSX_GPU_H

#include "vram.h"
#include "raster.h"
#include <stdint.h>

#define GP0_CMD_BUF_MAX 12

/* Qual handler GP0 está ativo */
typedef enum {
    GP0H_COMMAND = 0,
    GP0H_POLY_FLAT3,
    GP0H_POLY_FLAT4,
    GP0H_POLY_GOURAUD3,
    GP0H_POLY_GOURAUD4,
    GP0H_POLY_FLAT_TEX3,
    GP0H_POLY_FLAT_TEX4,
    GP0H_POLY_GOURAUD_TEX3,
    GP0H_POLY_GOURAUD_TEX4,
    GP0H_RECT_VARIABLE,
    GP0H_RECT_VARIABLE_TEX,
    GP0H_RECT_1X1,
    GP0H_RECT_8X8,
    GP0H_RECT_16X16,
    GP0H_FILL_VRAM,
    GP0H_COPY_VRAM_VRAM,
    GP0H_COPY_VRAM_CPU,
    GP0H_COPY_CPU_VRAM,
} Gp0Handler;

typedef enum {
    TRANSFER_CPU_TO_VRAM,
    TRANSFER_VRAM_TO_CPU,
} TransferDir;

/* Estado de transferência CPU↔VRAM em andamento */
typedef struct {
    uint32_t    x, y;
    uint32_t    x_start;
    uint32_t    width, height;
    uint32_t    remaining;    /* palavras restantes (CPU→VRAM) */
    TransferDir direction;
    uint16_t   *read_buf;     /* buffer para VRAM→CPU (heap) */
    uint32_t    read_buf_size;
    uint32_t    read_pos;
    int         active;
} VramTransfer;

typedef struct {
    Vram vram;

    /* Máquina de estado GP0 */
    uint32_t   gp0_cmd_buf[GP0_CMD_BUF_MAX];
    uint32_t   gp0_cmd_len;
    uint32_t   gp0_words_left;
    Gp0Handler gp0_handler;

    /* Transferência VRAM ativa */
    VramTransfer vram_transfer;

    /* Registradores de desenho */
    DrawArea draw_area;

    /* Textura atual */
    uint32_t tex_page_x;
    uint32_t tex_page_y;
    uint8_t  tex_mode;
    int      tex_disable;

    /* Semi-transparência e máscara */
    uint8_t semi_trans;
    int     mask_set_on_draw;
    int     mask_check_enable;

    /* Registradores de display */
    int      display_disabled;
    uint32_t display_hres;
    uint32_t display_vres;
    uint8_t  display_depth;
    int      display_interlace;
    uint32_t display_vram_x;
    uint32_t display_vram_y;
    uint32_t display_x1, display_x2;
    uint32_t display_y1, display_y2;

    uint8_t  dma_direction;
    uint32_t frame_count;
} Gpu;

void     gpu_init(Gpu *gpu);
void     gpu_destroy(Gpu *gpu);
uint32_t gpu_gpustat(const Gpu *gpu);
uint32_t gpu_gpuread(Gpu *gpu);
void     gpu_gp0_write(Gpu *gpu, uint32_t val);
void     gpu_gp1_write(Gpu *gpu, uint32_t val);
void     gpu_dump_frame(const Gpu *gpu);

#endif /* PSX_GPU_H */
