#include "mdec.h"
#include "log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MDEC_STATUS_DATA_OUT_EMPTY (1u << 31)
#define MDEC_STATUS_DATA_IN_FULL (1u << 30)
#define MDEC_STATUS_BUSY (1u << 29)
#define MDEC_STATUS_DATA_IN_REQ (1u << 28)
#define MDEC_STATUS_DATA_OUT_REQ (1u << 27)

static const uint8_t zagzig[64] = {
    0,
    1,
    8,
    16,
    9,
    2,
    3,
    10,
    17,
    24,
    32,
    25,
    18,
    11,
    4,
    5,
    12,
    19,
    26,
    33,
    40,
    48,
    41,
    34,
    27,
    20,
    13,
    6,
    7,
    14,
    21,
    28,
    35,
    42,
    49,
    56,
    57,
    50,
    43,
    36,
    29,
    22,
    15,
    23,
    30,
    37,
    44,
    51,
    58,
    59,
    52,
    45,
    38,
    31,
    39,
    46,
    53,
    60,
    61,
    54,
    47,
    55,
    62,
    63,
};

static const int16_t default_scale[64] = {
    0x5A82,
    0x5A82,
    0x5A82,
    0x5A82,
    0x5A82,
    0x5A82,
    0x5A82,
    0x5A82,
    0x7D8A,
    0x6A6D,
    0x471C,
    0x18F8,
    (int16_t)0xE707,
    (int16_t)0xB8E3,
    (int16_t)0x9592,
    (int16_t)0x8275,
    0x7641,
    0x30FB,
    (int16_t)0xCF04,
    (int16_t)0x89BE,
    (int16_t)0x89BE,
    (int16_t)0xCF04,
    0x30FB,
    0x7641,
    0x6A6D,
    (int16_t)0xE707,
    (int16_t)0x8275,
    (int16_t)0xB8E3,
    0x471C,
    0x7D8A,
    0x18F8,
    (int16_t)0x9592,
    0x5A82,
    (int16_t)0xA57D,
    (int16_t)0xA57D,
    0x5A82,
    0x5A82,
    (int16_t)0xA57D,
    (int16_t)0xA57D,
    0x5A82,
    0x471C,
    (int16_t)0x8275,
    0x18F8,
    0x6A6D,
    (int16_t)0x9592,
    (int16_t)0xE707,
    0x7D8A,
    (int16_t)0xB8E3,
    0x30FB,
    (int16_t)0x89BE,
    0x7641,
    (int16_t)0xCF04,
    (int16_t)0xCF04,
    0x7641,
    (int16_t)0x89BE,
    0x30FB,
    0x18F8,
    (int16_t)0xB8E3,
    0x6A6D,
    (int16_t)0x8275,
    0x7D8A,
    (int16_t)0x9592,
    0x471C,
    (int16_t)0xE707,
};

static int32_t sign10(uint16_t v)
{
    v &= 0x03FFu;
    return (v & 0x0200u) ? (int32_t)v - 0x0400 : (int32_t)v;
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static int32_t div_floor_i32(int32_t v, int32_t d)
{
    if (v >= 0)
        return v / d;
    return -(((-v) + d - 1) / d);
}

static int32_t idct_round(int64_t sum, int64_t bias)
{
    if (sum >= 0)
        return (int32_t)((sum + bias) / 0x2000);
    return -(int32_t)(((-sum) + bias) / 0x2000);
}

static void push_output(Mdec *mdec, uint32_t word)
{
    if (mdec->out_count >= MDEC_OUTPUT_WORD_CAP)
        return;
    mdec->output[mdec->out_write] = word;
    mdec->out_write = (mdec->out_write + 1u) % MDEC_OUTPUT_WORD_CAP;
    mdec->out_count++;
}

static void update_status(Mdec *mdec)
{
    uint32_t status = 0;
    if (mdec->out_count == 0)
        status |= MDEC_STATUS_DATA_OUT_EMPTY;
    else if (mdec->dma_out_enabled)
        status |= MDEC_STATUS_DATA_OUT_REQ;

    if (mdec->receiving)
        status |= MDEC_STATUS_BUSY;
    if (mdec->dma_in_enabled && (!mdec->receiving || mdec->words_remaining > 0))
        status |= MDEC_STATUS_DATA_IN_REQ;
    status |= ((uint32_t)mdec->output_depth & 3u) << 25;
    if (mdec->output_signed)
        status |= 1u << 24;
    if (mdec->output_bit15)
        status |= 1u << 23;
    status |= ((uint32_t)mdec->current_block & 7u) << 16;
    status |= mdec->receiving && mdec->words_remaining > 0
                  ? ((mdec->words_remaining - 1u) & 0xFFFFu)
                  : 0xFFFFu;
    mdec->status = status;
}

void mdec_init(Mdec *mdec)
{
    memset(mdec, 0, sizeof(*mdec));
    for (int i = 0; i < 64; i++)
    {
        mdec->quant_y[i] = 1;
        mdec->quant_uv[i] = 1;
        mdec->scale[i] = default_scale[i];
    }
    mdec->output_depth = 2;
    mdec->current_block = 4;
    mdec->status = 0x80040000u;
    update_status(mdec);
}

static void set_quant_table(Mdec *mdec)
{
    size_t byte_count = mdec->param_half_count * 2u;
    uint8_t bytes[128];
    memset(bytes, 0, sizeof(bytes));
    if (byte_count > sizeof(bytes))
        byte_count = sizeof(bytes);
    for (size_t i = 0; i < byte_count; i++)
    {
        uint16_t h = mdec->params[i / 2u];
        bytes[i] = (uint8_t)(h >> ((i & 1u) ? 8 : 0));
    }
    memcpy(mdec->quant_y, bytes, 64);
    if (mdec->command & 1u)
        memcpy(mdec->quant_uv, bytes + 64, 64);
}

static void set_scale_table(Mdec *mdec)
{
    size_t count = mdec->param_half_count < 64u ? mdec->param_half_count : 64u;
    for (size_t i = 0; i < count; i++)
        mdec->scale[i] = (int16_t)mdec->params[i];
}

static size_t decode_block(const Mdec *mdec, size_t pos, const uint8_t *qt, int16_t out[64])
{
    int32_t coeff[64];
    memset(coeff, 0, sizeof(coeff));

    while (pos < mdec->param_half_count && mdec->params[pos] == 0xFE00u)
        pos++;
    if (pos >= mdec->param_half_count)
        return pos;

    uint16_t n = mdec->params[pos++];
    uint32_t q_scale = (n >> 10) & 0x3Fu;
    int k = 0;
    int32_t val = sign10(n) * (q_scale == 0 ? 2 : qt[0]);

    for (;;)
    {
        val = clamp_i32(val, -0x400, 0x3FF);
        if (q_scale > 0)
            coeff[zagzig[k]] = val;
        else
            coeff[k] = val;

        if (k >= 63)
            break;
        if (pos >= mdec->param_half_count)
            break;
        n = mdec->params[pos++];
        if (n == 0xFE00u)
            break;
        k += (int)((n >> 10) & 0x3Fu) + 1;
        if (k > 63)
            break;
        if (q_scale == 0)
            val = sign10(n) * 2;
        else
            val = div_floor_i32(sign10(n) * (int32_t)qt[k] * (int32_t)q_scale + 4, 8);
    }

    int32_t tmp[64];
    for (int pass = 0; pass < 2; pass++)
    {
        static const int64_t pass_bias[2] = {0x0580, 0x0D00};
        const int32_t *src = pass == 0 ? coeff : tmp;
        int32_t dst[64];
        for (int x = 0; x < 8; x++)
        {
            for (int y = 0; y < 8; y++)
            {
                int64_t sum = 0;
                for (int z = 0; z < 8; z++)
                    sum += (int64_t)src[y + z * 8] *
                           div_floor_i32(mdec->scale[x + z * 8], 8);
                dst[x + y * 8] =
                    idct_round(sum, pass_bias[pass]);
            }
        }
        memcpy(tmp, dst, sizeof(tmp));
    }
    for (int i = 0; i < 64; i++)
        out[i] = (int16_t)clamp_i32(tmp[i], -256, 255);
    return pos;
}

static size_t decode_color_block(const Mdec *mdec, size_t pos,
                                 const uint8_t *qt, int16_t out[64])
{
    int32_t coeff[64] = {0};

    while (pos < mdec->param_half_count && mdec->params[pos] == 0xFE00u)
        pos++;
    if (pos >= mdec->param_half_count)
        return pos;

    uint16_t n = mdec->params[pos++];
    uint32_t q_scale = (n >> 10) & 0x3Fu;
    int k = 0;
    int32_t value = sign10(n) * (q_scale == 0 ? 2 : qt[0]);

    for (;;)
    {
        value = clamp_i32(value, -0x400, 0x3FF);
        coeff[q_scale > 0 ? zagzig[k] : k] = value;
        if (k >= 63 || pos >= mdec->param_half_count)
            break;

        n = mdec->params[pos++];
        if (n == 0xFE00u)
            break;
        k += (int)((n >> 10) & 0x3Fu) + 1;
        if (k > 63)
            break;
        value = q_scale == 0
                    ? sign10(n) * 2
                    : (sign10(n) * (int32_t)qt[k] *
                           (int32_t)q_scale +
                       4) /
                          8;
    }

    for (int y = 0; y < 8; y++)
    {
        for (int x = 0; x < 8; x++)
        {
            int64_t sum = 0;
            for (int v = 0; v < 8; v++)
            {
                for (int u = 0; u < 8; u++)
                {
                    sum += (int64_t)coeff[u + v * 8] *
                           mdec->scale[u * 8 + x] *
                           mdec->scale[v * 8 + y];
                }
            }
            int32_t sample =
                (int32_t)((sum >> 32) + ((sum >> 31) & 1));
            sample &= 0x1FF;
            if (sample & 0x100)
                sample -= 0x200;
            out[x + y * 8] =
                (int16_t)clamp_i32(sample, -128, 127);
        }
    }
    return pos;
}

static uint8_t mono_sample(const Mdec *mdec, int16_t y)
{
    int32_t v = y & 0x1FF;
    if (v & 0x100)
        v -= 0x200;
    v = clamp_i32(v, -128, 127);
    if (!mdec->output_signed)
        v ^= 0x80;
    return (uint8_t)v;
}

static int32_t signed9(int16_t v)
{
    int32_t x = v & 0x1FF;
    if (x & 0x100)
        x -= 0x200;
    return x;
}

static uint8_t mono_nibble(const Mdec *mdec, int16_t y)
{
    uint32_t v = (uint32_t)mono_sample(mdec, y) + 8u;
    if (v > 0xFFu)
        v = 0xFFu;
    return (uint8_t)(v >> 4);
}

static void output_mono(Mdec *mdec, const int16_t yblk[64])
{
    if (mdec->output_depth == 0)
    {
        for (int i = 0; i < 64; i += 8)
        {
            uint8_t b[4];
            for (int p = 0; p < 4; p++)
            {
                uint8_t a = mono_nibble(mdec, yblk[i + p * 2]);
                uint8_t c = mono_nibble(mdec, yblk[i + p * 2 + 1]);
                b[p] = (uint8_t)(a | (c << 4));
            }
            push_output(mdec, (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                  ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
        }
        return;
    }

    for (int i = 0; i < 64; i += 4)
    {
        uint8_t b0 = mono_sample(mdec, yblk[i + 0]);
        uint8_t b1 = mono_sample(mdec, yblk[i + 1]);
        uint8_t b2 = mono_sample(mdec, yblk[i + 2]);
        uint8_t b3 = mono_sample(mdec, yblk[i + 3]);
        push_output(mdec, (uint32_t)b0 | ((uint32_t)b1 << 8) |
                              ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24));
    }
}

static void yuv_to_rgb(const Mdec *mdec, int16_t y, int16_t cr, int16_t cb,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    int32_t yy = signed9(y);
    int32_t rr_chroma = signed9(cr);
    int32_t bb_chroma = signed9(cb);
    int32_t rr = yy + ((rr_chroma * 359 + 0x80) >> 8);
    int32_t gg = yy + ((((-bb_chroma * 88) & ~0x1F) + ((-rr_chroma * 183) & ~0x07) + 0x80) >> 8);
    int32_t bb = yy + ((bb_chroma * 454 + 0x80) >> 8);
    rr = clamp_i32(rr, -128, 127);
    gg = clamp_i32(gg, -128, 127);
    bb = clamp_i32(bb, -128, 127);
    if (!mdec->output_signed)
    {
        rr ^= 0x80;
        gg ^= 0x80;
        bb ^= 0x80;
    }
    *r = (uint8_t)rr;
    *g = (uint8_t)gg;
    *b = (uint8_t)bb;
}

static uint16_t rgb_to_1555_mdec(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t r5 = (uint16_t)(((uint16_t)r + 4u) >> 3);
    uint16_t g5 = (uint16_t)(((uint16_t)g + 4u) >> 3);
    uint16_t b5 = (uint16_t)(((uint16_t)b + 4u) >> 3);
    if (r5 > 31u)
        r5 = 31u;
    if (g5 > 31u)
        g5 = 31u;
    if (b5 > 31u)
        b5 = 31u;
    return (uint16_t)(r5 | (g5 << 5) | (b5 << 10));
}

static void append_rgb_pixel(Mdec *mdec, uint8_t r, uint8_t g, uint8_t b,
                             uint8_t *bytes, int *count)
{
    bytes[(*count)++] = r;
    bytes[(*count)++] = g;
    bytes[(*count)++] = b;
    while (*count >= 4)
    {
        push_output(mdec, (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                              ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24));
        memmove(bytes, bytes + 4, (size_t)(*count - 4));
        *count -= 4;
    }
}

static void output_color(Mdec *mdec, int16_t blocks[6][64])
{
    uint8_t bytes[8];
    int byte_count = 0;
    for (int py = 0; py < 16; py++)
    {
        for (int px = 0; px < 16; px++)
        {
            int block = (px >= 8 ? 1 : 0) + (py >= 8 ? 2 : 0);
            int x = px & 7;
            int y = py & 7;
            int yi = x + y * 8;
            int ci = (px / 2) + (py / 2) * 8;
            uint8_t r, g, b;
            yuv_to_rgb(mdec, blocks[2 + block][yi], blocks[0][ci], blocks[1][ci], &r, &g, &b);

            if (mdec->output_depth == 3)
            {
                uint16_t pix = rgb_to_1555_mdec(r, g, b);
                if (mdec->output_bit15)
                    pix |= 0x8000u;
                bytes[byte_count++] = (uint8_t)pix;
                bytes[byte_count++] = (uint8_t)(pix >> 8);
                if (byte_count == 4)
                {
                    push_output(mdec, (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                                          ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24));
                    byte_count = 0;
                }
            }
            else
            {
                append_rgb_pixel(mdec, r, g, b, bytes, &byte_count);
            }
        }
    }
    if (byte_count > 0)
    {
        while (byte_count < 4)
            bytes[byte_count++] = 0;
        push_output(mdec, (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                              ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24));
    }
}

static void decode_all(Mdec *mdec)
{
    size_t pos = 0;
    while (pos < mdec->param_half_count)
    {
        if (mdec->output_depth == 0 || mdec->output_depth == 1)
        {
            int16_t y[64];
            mdec->current_block = 4;
            size_t next = decode_block(mdec, pos, mdec->quant_y, y);
            if (next <= pos)
                break;
            output_mono(mdec, y);
            pos = next;
            if (pos < mdec->param_half_count &&
                mdec->params[pos] == 0xFE00u)
                break;
        }
        else
        {
            int16_t blocks[6][64];
            for (int i = 0; i < 6; i++)
            {
                mdec->current_block = (uint8_t)((i < 2) ? (4 + i) : (i - 2));
                size_t next = decode_color_block(
                    mdec, pos, i < 2 ? mdec->quant_uv : mdec->quant_y,
                    blocks[i]);
                if (next <= pos)
                    goto done;
                pos = next;
            }
            output_color(mdec, blocks);
            if (pos < mdec->param_half_count &&
                mdec->params[pos] == 0xFE00u)
                break;
        }
    }
done:
    mdec->receiving = false;
    mdec->words_remaining = 0;
    mdec->current_block = 4;
    update_status(mdec);
}

static void finish_command(Mdec *mdec)
{
    LOG(LOG_MDEC, "finish cmd=%u halves=%zu out_before=%zu", mdec->active_command,
        mdec->param_half_count, mdec->out_count);
    switch (mdec->active_command)
    {
    case 1:
        decode_all(mdec);
        break;
    case 2:
        set_quant_table(mdec);
        mdec->receiving = false;
        break;
    case 3:
        set_scale_table(mdec);
        mdec->receiving = false;
        break;
    default:
        mdec->receiving = false;
        break;
    }
    update_status(mdec);
    LOG(LOG_MDEC, "finish cmd=%u out_after=%zu status=%08X", mdec->active_command,
        mdec->out_count, mdec->status);
}

static void start_command(Mdec *mdec, uint32_t word)
{
    mdec->command = word;
    mdec->active_command = (uint8_t)(word >> 29);
    mdec->output_depth = (uint8_t)((word >> 27) & 3u);
    mdec->output_signed = (word & (1u << 26)) != 0;
    mdec->output_bit15 = (word & (1u << 25)) != 0;
    mdec->param_half_count = 0;

    switch (mdec->active_command)
    {
    case 1:
        mdec->words_remaining = word & 0xFFFFu;
        break;
    case 2:
        mdec->words_remaining = (word & 1u) ? 32u : 16u;
        break;
    case 3:
        mdec->words_remaining = 32u;
        break;
    default:
        mdec->words_remaining = 0;
        break;
    }
    mdec->receiving = mdec->words_remaining != 0;
    LOG(LOG_MDEC, "cmd word=%08X op=%u depth=%u words=%u", word, mdec->active_command,
        mdec->output_depth, mdec->words_remaining);
    if (!mdec->receiving)
        finish_command(mdec);
    update_status(mdec);
}

uint32_t mdec_load32(Mdec *mdec, uint32_t off)
{
    switch (off)
    {
    case 0:
        return mdec_dma_read(mdec);
    case 4:
        update_status(mdec);
        return mdec->status;
    default:
        return 0xFFFFFFFF;
    }
}

void mdec_store32(Mdec *mdec, uint32_t off, uint32_t val)
{
    switch (off)
    {
    case 0:
        mdec_dma_write(mdec, val);
        break;
    case 4:
        if (val & 0x80000000u)
        {
            mdec_init(mdec);
        }
        else
        {
            mdec->dma_in_enabled = (val & (1u << 30)) != 0;
            mdec->dma_out_enabled = (val & (1u << 29)) != 0;
            update_status(mdec);
        }
        break;
    default:
        break;
    }
}

void mdec_dma_write(Mdec *mdec, uint32_t word)
{
    if (!mdec->receiving)
    {
        start_command(mdec, word);
        return;
    }

    if (mdec->param_half_count + 2u <= MDEC_PARAM_HALF_CAP)
    {
        mdec->params[mdec->param_half_count++] = (uint16_t)word;
        mdec->params[mdec->param_half_count++] = (uint16_t)(word >> 16);
    }
    if (mdec->words_remaining > 0)
        mdec->words_remaining--;
    if (mdec->words_remaining == 0)
        finish_command(mdec);
    else
        update_status(mdec);
}

uint32_t mdec_dma_read(Mdec *mdec)
{
    uint32_t word = 0;
    if (mdec->out_count > 0)
    {
        word = mdec->output[mdec->out_read];
        mdec->out_read = (mdec->out_read + 1u) % MDEC_OUTPUT_WORD_CAP;
        mdec->out_count--;
    }
    update_status(mdec);
    return word;
}
