/* ============================================================
 * GPU — Graphics Processing Unit do PlayStation
 * ============================================================
 * VRAM: 1024×512 @16bpp (BGR555)
 * GP0 (0x1f801810) — comandos de rendering
 * GP1 (0x1f801814) — comandos de display/controle
 * GPUREAD (ler GP0) — dados de resposta / VRAM→CPU
 * GPUSTAT (ler GP1) — status da GPU
 */

#include "gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tamanho (em palavras) de cada handler ----------------------- */

static uint32_t handler_len(Gp0Handler h)
{
    switch (h) {
    case GP0H_COMMAND:           return 1;
    case GP0H_POLY_FLAT3:        return 4;
    case GP0H_POLY_FLAT4:        return 5;
    case GP0H_POLY_GOURAUD3:     return 6;
    case GP0H_POLY_GOURAUD4:     return 8;
    case GP0H_POLY_FLAT_TEX3:    return 7;
    case GP0H_POLY_FLAT_TEX4:    return 9;
    case GP0H_POLY_GOURAUD_TEX3: return 9;
    case GP0H_POLY_GOURAUD_TEX4: return 12;
    case GP0H_RECT_VARIABLE:     return 3;
    case GP0H_RECT_VARIABLE_TEX: return 4;
    case GP0H_RECT_1X1:          return 2;
    case GP0H_RECT_8X8:          return 2;
    case GP0H_RECT_16X16:        return 2;
    case GP0H_FILL_VRAM:         return 3;
    case GP0H_COPY_VRAM_VRAM:    return 4;
    case GP0H_COPY_VRAM_CPU:     return 3;
    case GP0H_COPY_CPU_VRAM:     return 3;
    default:                     return 1;
    }
}

/* ---- gpu_init / gpu_destroy -------------------------------------- */

void gpu_init(Gpu *gpu)
{
    vram_init(&gpu->vram);

    memset(gpu->gp0_cmd_buf, 0, sizeof(gpu->gp0_cmd_buf));
    gpu->gp0_cmd_len    = 0;
    gpu->gp0_words_left = 0;
    gpu->gp0_handler    = GP0H_COMMAND;

    memset(&gpu->vram_transfer, 0, sizeof(gpu->vram_transfer));

    gpu->draw_area.x_min = 0;    gpu->draw_area.y_min = 0;
    gpu->draw_area.x_max = 1023; gpu->draw_area.y_max = 511;
    gpu->draw_area.x_off = 0;    gpu->draw_area.y_off = 0;

    gpu->tex_page_x  = 0; gpu->tex_page_y  = 0;
    gpu->tex_mode    = 0; gpu->tex_disable = 0;
    gpu->semi_trans  = 0;
    gpu->mask_set_on_draw  = 0;
    gpu->mask_check_enable = 0;

    gpu->display_disabled  = 1;
    gpu->display_hres      = 320;
    gpu->display_vres      = 240;
    gpu->display_depth     = 0;
    gpu->display_interlace = 0;
    gpu->display_vram_x    = 0; gpu->display_vram_y = 0;
    gpu->display_x1        = 0x200; gpu->display_x2 = 0xc00;
    gpu->display_y1        = 0x010; gpu->display_y2 = 0x100;

    gpu->dma_direction = 0;
    gpu->frame_count   = 0;
}

void gpu_destroy(Gpu *gpu)
{
    if (gpu->vram_transfer.active && gpu->vram_transfer.read_buf) {
        free(gpu->vram_transfer.read_buf);
        gpu->vram_transfer.read_buf = NULL;
    }
}

/* ---- GPUSTAT ----------------------------------------------------- */

uint32_t gpu_gpustat(const Gpu *gpu)
{
    uint32_t s = 0;
    s |= gpu->tex_page_x & 0xf;
    s |= (gpu->tex_page_y & 0x1u) << 4;
    s |= ((uint32_t)gpu->semi_trans & 3u) << 5;
    s |= ((uint32_t)gpu->tex_mode  & 3u) << 7;
    if (gpu->mask_set_on_draw)  s |= 1u << 11;
    if (gpu->mask_check_enable) s |= 1u << 12;
    if (gpu->display_interlace) s |= 1u << 22;
    if (gpu->display_disabled)  s |= 1u << 23;
    if (gpu->display_depth)     s |= 1u << 24;
    s |= ((uint32_t)gpu->dma_direction & 3u) << 29;

    /* GPU pronta para receber comandos */
    s |= 1u << 26;  /* ready to receive command */
    s |= 1u << 27;  /* ready to send VRAM to CPU */
    s |= 1u << 28;  /* ready to receive DMA block */

    /* DMA request */
    switch (gpu->dma_direction) {
    case 1: s |= (s >> 28) & 1u; break;
    case 2: s |= (s >> 28) & 1u; break;
    case 3: s |= (s >> 27) & 1u; break;
    default: break;
    }
    return s;
}

/* ---- GPUREAD ----------------------------------------------------- */

uint32_t gpu_gpuread(Gpu *gpu)
{
    VramTransfer *t = &gpu->vram_transfer;
    if (t->active && t->direction == TRANSFER_VRAM_TO_CPU && t->read_buf) {
        uint32_t lo = t->read_buf[t->read_pos];
        uint32_t hi = (t->read_pos + 1 < t->read_buf_size)
                      ? t->read_buf[t->read_pos + 1] : 0u;
        t->read_pos += 2;
        if (t->read_pos >= t->read_buf_size) {
            free(t->read_buf);
            t->read_buf = NULL;
            t->active   = 0;
        }
        return lo | (hi << 16);
    }
    return 0;
}

/* ---- helpers internos -------------------------------------------- */

static TexContext make_tex_ctx(const Gpu *gpu,
                               uint32_t clut_word, uint32_t tpage_word)
{
    uint32_t clut   = (clut_word >> 16) & 0x7fffu;
    uint32_t clut_x = (clut & 0x3fu) * 16u;
    uint32_t clut_y = (clut >> 6) & 0x1ffu;

    uint32_t tp   = (tpage_word >> 16) & 0x1ffu;
    uint32_t pg_x = tp & 0xfu;
    uint32_t pg_y = (tp >> 4) & 1u;
    uint8_t  mode = (uint8_t)((tp >> 7) & 3u);

    TexContext tc;
    tc.page_x = pg_x;
    tc.page_y = pg_y * 256u;
    tc.mode   = (mode > 2) ? gpu->tex_mode : mode;
    tc.clut_x = clut_x;
    tc.clut_y = clut_y;
    return tc;
}

/* ---- cópias de VRAM ---------------------------------------------- */

static void copy_vram_to_vram(Gpu *gpu, const uint32_t *cmd)
{
    uint32_t src_x = cmd[1] & 0xffffu;
    uint32_t src_y = (cmd[1] >> 16) & 0xffffu;
    uint32_t dst_x = cmd[2] & 0xffffu;
    uint32_t dst_y = (cmd[2] >> 16) & 0xffffu;
    uint32_t w     = cmd[3] & 0xffffu;
    uint32_t h     = (cmd[3] >> 16) & 0xffffu;
    uint32_t total = w * h;

    uint16_t *buf = (uint16_t *)malloc(total * sizeof(uint16_t));
    if (!buf) return;

    for (uint32_t row = 0; row < h; row++)
        for (uint32_t col = 0; col < w; col++)
            buf[row * w + col] = vram_get(&gpu->vram,
                                          (int32_t)(src_x + col),
                                          (int32_t)(src_y + row));
    for (uint32_t row = 0; row < h; row++)
        for (uint32_t col = 0; col < w; col++)
            vram_set(&gpu->vram,
                     (int32_t)(dst_x + col),
                     (int32_t)(dst_y + row),
                     buf[row * w + col]);
    free(buf);
}

static void start_cpu_to_vram(Gpu *gpu, const uint32_t *cmd)
{
    VramTransfer *t = &gpu->vram_transfer;
    if (t->active && t->read_buf) { free(t->read_buf); t->read_buf = NULL; }
    t->x       = cmd[1] & 0xffffu;
    t->y       = (cmd[1] >> 16) & 0xffffu;
    t->x_start = t->x;
    t->width   = cmd[2] & 0xffffu;
    t->height  = (cmd[2] >> 16) & 0xffffu;
    t->remaining   = (t->width * t->height + 1u) / 2u;
    t->direction   = TRANSFER_CPU_TO_VRAM;
    t->read_buf    = NULL;
    t->read_buf_size = 0;
    t->read_pos    = 0;
    t->active      = 1;
}

static void start_vram_to_cpu(Gpu *gpu, const uint32_t *cmd)
{
    VramTransfer *t = &gpu->vram_transfer;
    if (t->active && t->read_buf) { free(t->read_buf); t->read_buf = NULL; }
    uint32_t x = cmd[1] & 0xffffu;
    uint32_t y = (cmd[1] >> 16) & 0xffffu;
    uint32_t w = cmd[2] & 0xffffu;
    uint32_t h = (cmd[2] >> 16) & 0xffffu;
    uint32_t total = w * h;

    uint16_t *buf = (uint16_t *)malloc(total * sizeof(uint16_t));
    if (!buf) return;
    for (uint32_t row = 0; row < h; row++)
        for (uint32_t col = 0; col < w; col++)
            buf[row * w + col] = vram_get(&gpu->vram,
                                          (int32_t)(x + col),
                                          (int32_t)(y + row));
    t->x = x; t->y = y; t->x_start = x;
    t->width = w; t->height = h;
    t->remaining   = 0;
    t->direction   = TRANSFER_VRAM_TO_CPU;
    t->read_buf    = buf;
    t->read_buf_size = total;
    t->read_pos    = 0;
    t->active      = 1;
}

/* ---- gp0_execute — executa o comando acumulado em gp0_cmd_buf ---- */

static void gp0_execute(Gpu *gpu)
{
    const uint32_t *cmd = gpu->gp0_cmd_buf;
    uint32_t op = (cmd[0] >> 24) & 0xffu;

    switch (op) {

    /* ---- Fill VRAM (0x02) ---------------------------------------- */
    case 0x02: {
        Color  c   = color_from_cmd(cmd[0]);
        int32_t x  = (int32_t)(cmd[1] & 0xffffu);
        int32_t y  = (int32_t)((cmd[1] >> 16) & 0xffffu);
        int32_t w  = (int32_t)(cmd[2] & 0xffffu);
        int32_t h  = (int32_t)((cmd[2] >> 16) & 0xffffu);
        fill_vram_rect(&gpu->vram, x, y, w, h, c);
        break;
    }

    /* ---- Triângulo flat (0x20-0x23) ------------------------------ */
    case 0x20: case 0x21: case 0x22: case 0x23: {
        Color  c  = color_from_cmd(cmd[0]);
        Vertex v[3] = { vertex_new(cmd[1], c),
                        vertex_new(cmd[2], c),
                        vertex_new(cmd[3], c) };
        draw_triangle_flat(&gpu->vram, &gpu->draw_area, v);
        break;
    }

    /* ---- Quad flat (0x28-0x2b) ----------------------------------- */
    case 0x28: case 0x29: case 0x2a: case 0x2b: {
        Color  c  = color_from_cmd(cmd[0]);
        Vertex v[4] = { vertex_new(cmd[1], c), vertex_new(cmd[2], c),
                        vertex_new(cmd[3], c), vertex_new(cmd[4], c) };
        draw_quad_flat(&gpu->vram, &gpu->draw_area, v);
        break;
    }

    /* ---- Triângulo flat+tex (0x24-0x27) -------------------------- */
    case 0x24: case 0x25: case 0x26: case 0x27: {
        Color    fc  = color_from_cmd(cmd[0]);
        Vertex   v[3] = { vertex_new(cmd[1], fc),
                          vertex_new(cmd[3], fc),
                          vertex_new(cmd[5], fc) };
        TexCoord uv[3] = {
            { (uint8_t)(cmd[2] & 0xffu),  (uint8_t)((cmd[2] >> 8) & 0xffu) },
            { (uint8_t)(cmd[4] & 0xffu),  (uint8_t)((cmd[4] >> 8) & 0xffu) },
            { (uint8_t)(cmd[6] & 0xffu),  (uint8_t)((cmd[6] >> 8) & 0xffu) },
        };
        TexContext tc = make_tex_ctx(gpu, cmd[2], cmd[4]);
        int mod = (op == 0x24 || op == 0x25);
        draw_triangle_textured(&gpu->vram, &gpu->draw_area, v, uv, tc, fc, mod);
        break;
    }

    /* ---- Quad flat+tex (0x2c-0x2f) ------------------------------- */
    case 0x2c: case 0x2d: case 0x2e: case 0x2f: {
        Color    fc  = color_from_cmd(cmd[0]);
        Vertex   v[4] = { vertex_new(cmd[1], fc), vertex_new(cmd[3], fc),
                          vertex_new(cmd[5], fc), vertex_new(cmd[7], fc) };
        TexCoord uv[4] = {
            { (uint8_t)(cmd[2] & 0xffu),  (uint8_t)((cmd[2] >> 8) & 0xffu) },
            { (uint8_t)(cmd[4] & 0xffu),  (uint8_t)((cmd[4] >> 8) & 0xffu) },
            { (uint8_t)(cmd[6] & 0xffu),  (uint8_t)((cmd[6] >> 8) & 0xffu) },
            { (uint8_t)(cmd[8] & 0xffu),  (uint8_t)((cmd[8] >> 8) & 0xffu) },
        };
        TexContext tc = make_tex_ctx(gpu, cmd[2], cmd[4]);
        int mod = (op == 0x2c || op == 0x2d);
        draw_quad_textured(&gpu->vram, &gpu->draw_area, v, uv, tc, fc, mod);
        break;
    }

    /* ---- Triângulo Gouraud (0x30-0x33) --------------------------- */
    case 0x30: case 0x31: case 0x32: case 0x33: {
        Vertex v[3] = { vertex_new(cmd[1], color_from_cmd(cmd[0])),
                        vertex_new(cmd[3], color_from_cmd(cmd[2])),
                        vertex_new(cmd[5], color_from_cmd(cmd[4])) };
        draw_triangle_gouraud(&gpu->vram, &gpu->draw_area, v);
        break;
    }

    /* ---- Quad Gouraud (0x38-0x3b) -------------------------------- */
    case 0x38: case 0x39: case 0x3a: case 0x3b: {
        Vertex v[4] = { vertex_new(cmd[1], color_from_cmd(cmd[0])),
                        vertex_new(cmd[3], color_from_cmd(cmd[2])),
                        vertex_new(cmd[5], color_from_cmd(cmd[4])),
                        vertex_new(cmd[7], color_from_cmd(cmd[6])) };
        draw_quad_gouraud(&gpu->vram, &gpu->draw_area, v);
        break;
    }

    /* ---- Triângulo Gouraud+tex (0x34-0x37) ----------------------- */
    case 0x34: case 0x35: case 0x36: case 0x37: {
        Vertex v[3] = { vertex_new(cmd[1],  color_from_cmd(cmd[0])),
                        vertex_new(cmd[4],  color_from_cmd(cmd[3])),
                        vertex_new(cmd[7],  color_from_cmd(cmd[6])) };
        TexCoord uv[3] = {
            { (uint8_t)(cmd[2] & 0xffu), (uint8_t)((cmd[2] >> 8) & 0xffu) },
            { (uint8_t)(cmd[5] & 0xffu), (uint8_t)((cmd[5] >> 8) & 0xffu) },
            { (uint8_t)(cmd[8] & 0xffu), (uint8_t)((cmd[8] >> 8) & 0xffu) },
        };
        TexContext tc = make_tex_ctx(gpu, cmd[2], cmd[5]);
        Color neutral = { 128, 128, 128 };
        draw_triangle_textured(&gpu->vram, &gpu->draw_area, v, uv, tc, neutral, 0);
        break;
    }

    /* ---- Quad Gouraud+tex (0x3c-0x3f) ---------------------------- */
    case 0x3c: case 0x3d: case 0x3e: case 0x3f: {
        Vertex v[4] = { vertex_new(cmd[1],  color_from_cmd(cmd[0])),
                        vertex_new(cmd[4],  color_from_cmd(cmd[3])),
                        vertex_new(cmd[7],  color_from_cmd(cmd[6])),
                        vertex_new(cmd[10], color_from_cmd(cmd[9])) };
        TexCoord uv[4] = {
            { (uint8_t)(cmd[2]  & 0xffu), (uint8_t)((cmd[2]  >> 8) & 0xffu) },
            { (uint8_t)(cmd[5]  & 0xffu), (uint8_t)((cmd[5]  >> 8) & 0xffu) },
            { (uint8_t)(cmd[8]  & 0xffu), (uint8_t)((cmd[8]  >> 8) & 0xffu) },
            { (uint8_t)(cmd[11] & 0xffu), (uint8_t)((cmd[11] >> 8) & 0xffu) },
        };
        TexContext tc = make_tex_ctx(gpu, cmd[2], cmd[5]);
        Color neutral = { 128, 128, 128 };
        draw_quad_textured(&gpu->vram, &gpu->draw_area, v, uv, tc, neutral, 0);
        break;
    }

    /* ---- Retângulo flat variável (0x60-0x63) ---------------------- */
    case 0x60: case 0x61: case 0x62: case 0x63: {
        Color   c = color_from_cmd(cmd[0]);
        int32_t x = sign_ext11(cmd[1] & 0x7ffu);
        int32_t y = sign_ext11((cmd[1] >> 16) & 0x7ffu);
        int32_t w = (int32_t)(cmd[2] & 0xffffu);
        int32_t h = (int32_t)((cmd[2] >> 16) & 0xffffu);
        draw_rect_flat(&gpu->vram, &gpu->draw_area, x, y, w, h, c, 1);
        break;
    }

    /* ---- Retângulo texturizado variável (0x64-0x67) --------------- */
    case 0x64: case 0x65: case 0x66: case 0x67: {
        Color   fc = color_from_cmd(cmd[0]);
        int32_t x  = sign_ext11(cmd[1] & 0x7ffu);
        int32_t y  = sign_ext11((cmd[1] >> 16) & 0x7ffu);
        uint8_t u0 = (uint8_t)(cmd[2] & 0xffu);
        uint8_t v0 = (uint8_t)((cmd[2] >> 8) & 0xffu);
        TexContext tc = make_tex_ctx(gpu, cmd[2], 0);
        int32_t w  = (int32_t)(cmd[3] & 0xffffu);
        int32_t h  = (int32_t)((cmd[3] >> 16) & 0xffffu);
        draw_rect_textured(&gpu->vram, &gpu->draw_area, x, y, w, h, u0, v0, tc, fc, 0);
        break;
    }

    /* ---- Retângulo 1×1 (0x68-0x6b) ------------------------------- */
    case 0x68: case 0x69: case 0x6a: case 0x6b: {
        Color   c = color_from_cmd(cmd[0]);
        int32_t x = sign_ext11(cmd[1] & 0x7ffu);
        int32_t y = sign_ext11((cmd[1] >> 16) & 0x7ffu);
        draw_rect_flat(&gpu->vram, &gpu->draw_area, x, y, 1, 1, c, 1);
        break;
    }

    /* ---- Retângulo 8×8 (0x70-0x73) ------------------------------- */
    case 0x70: case 0x71: case 0x72: case 0x73: {
        Color   c = color_from_cmd(cmd[0]);
        int32_t x = sign_ext11(cmd[1] & 0x7ffu);
        int32_t y = sign_ext11((cmd[1] >> 16) & 0x7ffu);
        draw_rect_flat(&gpu->vram, &gpu->draw_area, x, y, 8, 8, c, 1);
        break;
    }

    /* ---- Retângulo 16×16 (0x78-0x7b) ----------------------------- */
    case 0x78: case 0x79: case 0x7a: case 0x7b: {
        Color   c = color_from_cmd(cmd[0]);
        int32_t x = sign_ext11(cmd[1] & 0x7ffu);
        int32_t y = sign_ext11((cmd[1] >> 16) & 0x7ffu);
        draw_rect_flat(&gpu->vram, &gpu->draw_area, x, y, 16, 16, c, 1);
        break;
    }

    /* ---- Cópias de VRAM ------------------------------------------ */
    case 0x80: copy_vram_to_vram(gpu, cmd); break;
    case 0xa0: start_cpu_to_vram(gpu, cmd); break;
    case 0xc0: start_vram_to_cpu(gpu, cmd); break;

    default:
        break;
    }
}

/* ---- gp0_setting — registradores E1..E6 -------------------------- */

static void gp0_setting(Gpu *gpu, uint32_t val)
{
    uint32_t op = (val >> 24) & 0xffu;
    switch (op) {
    case 0xe1:
        gpu->tex_page_x  = val & 0xfu;
        gpu->tex_page_y  = (val >> 4) & 1u;
        gpu->semi_trans  = (uint8_t)((val >> 5) & 3u);
        gpu->tex_mode    = (uint8_t)((val >> 7) & 3u);
        gpu->tex_disable = (int)((val >> 11) & 1u);
        break;
    case 0xe2:
        /* Texture window — ignorado */
        break;
    case 0xe3:
        gpu->draw_area.x_min = (int32_t)(val & 0x3ffu);
        gpu->draw_area.y_min = (int32_t)((val >> 10) & 0x3ffu);
        break;
    case 0xe4:
        gpu->draw_area.x_max = (int32_t)(val & 0x3ffu);
        gpu->draw_area.y_max = (int32_t)((val >> 10) & 0x3ffu);
        break;
    case 0xe5: {
        uint32_t x = val & 0x7ffu;
        uint32_t y = (val >> 11) & 0x7ffu;
        gpu->draw_area.x_off = (x & 0x400u) ? (int32_t)(x | 0xfffff800u) : (int32_t)x;
        gpu->draw_area.y_off = (y & 0x400u) ? (int32_t)(y | 0xfffff800u) : (int32_t)y;
        break;
    }
    case 0xe6:
        gpu->mask_set_on_draw   = (int)(val & 1u);
        gpu->mask_check_enable  = (int)((val >> 1) & 1u);
        break;
    default:
        break;
    }
}

/* ---- gpu_gp0_write ----------------------------------------------- */

void gpu_gp0_write(Gpu *gpu, uint32_t val)
{
    VramTransfer *t = &gpu->vram_transfer;

    /* Transferência CPU→VRAM em andamento: consome dados */
    if (t->active && t->direction == TRANSFER_CPU_TO_VRAM) {
        uint16_t lo = (uint16_t)(val & 0xffffu);
        uint16_t hi = (uint16_t)((val >> 16) & 0xffffu);
        vram_store16(&gpu->vram, t->x, t->y, lo);
        t->x++;
        if (t->x >= t->x_start + t->width) { t->x = t->x_start; t->y++; }
        vram_store16(&gpu->vram, t->x, t->y, hi);
        t->x++;
        if (t->x >= t->x_start + t->width) { t->x = t->x_start; t->y++; }
        if (t->remaining > 0) t->remaining--;
        if (t->remaining == 0) t->active = 0;
        return;
    }

    /* Novo comando: decodifica opcode */
    if (gpu->gp0_words_left == 0) {
        uint32_t op = (val >> 24) & 0xffu;
        Gp0Handler handler;

        switch (op) {
        case 0x00: return;  /* NOP */
        case 0x01: return;  /* Clear cache */
        case 0x1f: return;  /* IRQ */
        case 0x02: handler = GP0H_FILL_VRAM;         break;
        case 0x20: case 0x21: case 0x22: case 0x23:
            handler = GP0H_POLY_FLAT3;     break;
        case 0x28: case 0x29: case 0x2a: case 0x2b:
            handler = GP0H_POLY_FLAT4;     break;
        case 0x24: case 0x25: case 0x26: case 0x27:
            handler = GP0H_POLY_FLAT_TEX3; break;
        case 0x2c: case 0x2d: case 0x2e: case 0x2f:
            handler = GP0H_POLY_FLAT_TEX4; break;
        case 0x30: case 0x31: case 0x32: case 0x33:
            handler = GP0H_POLY_GOURAUD3;  break;
        case 0x38: case 0x39: case 0x3a: case 0x3b:
            handler = GP0H_POLY_GOURAUD4;  break;
        case 0x34: case 0x35: case 0x36: case 0x37:
            handler = GP0H_POLY_GOURAUD_TEX3; break;
        case 0x3c: case 0x3d: case 0x3e: case 0x3f:
            handler = GP0H_POLY_GOURAUD_TEX4; break;
        case 0x60: case 0x61: case 0x62: case 0x63:
            handler = GP0H_RECT_VARIABLE;     break;
        case 0x64: case 0x65: case 0x66: case 0x67:
            handler = GP0H_RECT_VARIABLE_TEX; break;
        case 0x68: case 0x69: case 0x6a: case 0x6b:
            handler = GP0H_RECT_1X1;  break;
        case 0x70: case 0x71: case 0x72: case 0x73:
            handler = GP0H_RECT_8X8;  break;
        case 0x78: case 0x79: case 0x7a: case 0x7b:
            handler = GP0H_RECT_16X16; break;
        case 0x80: handler = GP0H_COPY_VRAM_VRAM; break;
        case 0xa0: handler = GP0H_COPY_CPU_VRAM;  break;
        case 0xc0: handler = GP0H_COPY_VRAM_CPU;  break;
        case 0xe0: return;
        default:
            if (op >= 0xe1u && op <= 0xe6u) {
                gp0_setting(gpu, val);
                return;
            }
            return;
        }

        if (op >= 0xe1u && op <= 0xe6u) {
            gp0_setting(gpu, val);
            return;
        }

        gpu->gp0_handler    = handler;
        gpu->gp0_words_left = handler_len(handler);
        gpu->gp0_cmd_len    = 0;
    }

    gpu->gp0_cmd_buf[gpu->gp0_cmd_len++] = val;
    gpu->gp0_words_left--;

    if (gpu->gp0_words_left == 0)
        gp0_execute(gpu);
}

/* ---- gpu_gp1_write ----------------------------------------------- */

void gpu_gp1_write(Gpu *gpu, uint32_t val)
{
    uint32_t cmd = (val >> 24) & 0xffu;
    switch (cmd) {
    case 0x00:  /* Reset GPU */
        printf("[GPU] GP1 Reset\n");
        gpu->gp0_cmd_len    = 0;
        gpu->gp0_words_left = 0;
        gpu->gp0_handler    = GP0H_COMMAND;
        if (gpu->vram_transfer.active && gpu->vram_transfer.read_buf) {
            free(gpu->vram_transfer.read_buf);
            gpu->vram_transfer.read_buf = NULL;
        }
        gpu->vram_transfer.active = 0;
        gpu->tex_page_x     = 0;
        gpu->tex_page_y     = 0;
        gpu->tex_mode       = 0;
        gpu->semi_trans     = 0;
        gpu->display_disabled = 1;
        gpu->dma_direction  = 0;
        break;
    case 0x01:  /* Reset command buffer */
        gpu->gp0_cmd_len    = 0;
        gpu->gp0_words_left = 0;
        gpu->gp0_handler    = GP0H_COMMAND;
        break;
    case 0x02:  /* Acknowledge GPU interrupt */
        break;
    case 0x03:
        gpu->display_disabled = (int)(val & 1u);
        printf("[GPU] Display %s\n", gpu->display_disabled ? "OFF" : "ON");
        break;
    case 0x04:
        gpu->dma_direction = (uint8_t)(val & 3u);
        break;
    case 0x05:
        gpu->display_vram_x = val & 0x3feu;
        gpu->display_vram_y = (val >> 10) & 0x1ffu;
        break;
    case 0x06:
        gpu->display_x1 = val & 0xfffu;
        gpu->display_x2 = (val >> 12) & 0xfffu;
        break;
    case 0x07:
        gpu->display_y1 = val & 0x3ffu;
        gpu->display_y2 = (val >> 10) & 0x3ffu;
        break;
    case 0x08: {
        uint32_t hr = val & 3u;
        gpu->display_hres = (hr == 0) ? 256 : (hr == 1) ? 320 : (hr == 2) ? 512 : 640;
        if ((val >> 6) & 1u) gpu->display_hres = 368;
        gpu->display_vres      = ((val >> 2) & 1u) ? 480u : 240u;
        gpu->display_depth     = (uint8_t)((val >> 4) & 1u);
        gpu->display_interlace = (int)((val >> 5) & 1u);
        printf("[GPU] Display mode: %ux%u %ubpp\n",
               gpu->display_hres, gpu->display_vres,
               gpu->display_depth ? 24u : 15u);
        gpu->frame_count++;
        gpu_dump_frame(gpu);
        break;
    }
    case 0x10:  /* Get GPU info — não implementado */
        break;
    default:
        break;
    }
}

/* ---- gpu_dump_frame ---------------------------------------------- */

void gpu_dump_frame(const Gpu *gpu)
{
    char path[64];
    snprintf(path, sizeof(path), "frames/frame_%04u.ppm", gpu->frame_count);
#ifdef _WIN32
    _mkdir("frames");
#else
    {
        /* mkdir -p simples */
        int r = system("mkdir -p frames");
        (void)r;
    }
#endif
    vram_dump_ppm(&gpu->vram,
                  path,
                  (uint16_t)gpu->display_vram_x,
                  (uint16_t)gpu->display_vram_y,
                  (uint16_t)gpu->display_hres,
                  (uint16_t)gpu->display_vres);
    printf("[GPU] Frame %u → %s\n", gpu->frame_count, path);
}
