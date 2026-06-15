#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MDEC_PARAM_HALF_CAP 131072u
#define MDEC_OUTPUT_WORD_CAP 1048576u

typedef struct
{
    uint32_t command;
    uint32_t status;

    uint8_t quant_y[64];
    uint8_t quant_uv[64];
    int16_t scale[64];

    uint16_t params[MDEC_PARAM_HALF_CAP];
    size_t param_half_count;
    size_t decode_half_pos;
    uint32_t words_remaining;
    uint8_t active_command;
    bool receiving;

    uint32_t output[MDEC_OUTPUT_WORD_CAP];
    size_t out_read;
    size_t out_write;
    size_t out_count;
    uint32_t dma_output[192];
    size_t dma_out_read;
    size_t dma_out_count;
    uint8_t data_out_empty_delay;

    bool dma_in_enabled;
    bool dma_out_enabled;
    bool output_signed;
    bool output_bit15;
    uint8_t output_depth;
    uint8_t current_block;
} Mdec;

void mdec_init(Mdec *mdec);
uint32_t mdec_load32(Mdec *mdec, uint32_t off);
void mdec_store32(Mdec *mdec, uint32_t off, uint32_t val);
void mdec_dma_write(Mdec *mdec, uint32_t word);
uint32_t mdec_cpu_read(Mdec *mdec);
uint32_t mdec_dma_read(Mdec *mdec);
