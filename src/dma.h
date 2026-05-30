#pragma once
#include "channel.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PORT_MDEC_IN  = 0,
    PORT_MDEC_OUT = 1,
    PORT_GPU      = 2,
    PORT_CDROM    = 3,
    PORT_SPU      = 4,
    PORT_PIO      = 5,
    PORT_OTC      = 6,
} Port;

Port port_from_index(uint32_t index);

typedef struct {
    uint32_t  control;
    bool      irq_en;
    uint8_t   channel_irq_en;
    uint8_t   channel_irq_flags;
    bool      force_irq;
    uint8_t   irq_dummy;
    Channel   channels[7];
} Dma;

void     dma_init(Dma *dma);
uint32_t dma_control(const Dma *dma);
void     dma_set_control(Dma *dma, uint32_t val);
uint32_t dma_interrupt(const Dma *dma);
void     dma_set_interrupt(Dma *dma, uint32_t val);
Channel *dma_channel(Dma *dma, Port port);
const Channel *dma_channel_const(const Dma *dma, Port port);
