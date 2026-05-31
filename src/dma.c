#include "dma.h"
#include <stdio.h>
#include <stdlib.h>

Port port_from_index(uint32_t index)
{
    switch (index)
    {
    case 0:
        return PORT_MDEC_IN;
    case 1:
        return PORT_MDEC_OUT;
    case 2:
        return PORT_GPU;
    case 3:
        return PORT_CDROM;
    case 4:
        return PORT_SPU;
    case 5:
        return PORT_PIO;
    case 6:
        return PORT_OTC;
    default:
        fprintf(stderr, "Invalid DMA port index: %u\n", index);
        exit(1);
    }
}

void dma_init(Dma *dma)
{
    dma->control = 0x07654321;
    dma->irq_en = false;
    dma->channel_irq_en = 0;
    dma->channel_irq_flags = 0;
    dma->force_irq = false;
    dma->irq_dummy = 0;
    for (int i = 0; i < 7; i++)
        channel_init(&dma->channels[i]);
}

uint32_t dma_control(const Dma *dma)
{
    return dma->control;
}

void dma_set_control(Dma *dma, uint32_t val)
{
    dma->control = val;
}

static bool dma_irq(const Dma *dma)
{
    uint8_t channel_irq = dma->channel_irq_flags & dma->channel_irq_en;
    return dma->force_irq || (dma->irq_en && channel_irq != 0);
}

uint32_t dma_interrupt(const Dma *dma)
{
    return (uint32_t)dma->irq_dummy | ((uint32_t)dma->force_irq << 15) | ((uint32_t)dma->channel_irq_en << 16) | ((uint32_t)dma->irq_en << 23) | ((uint32_t)dma->channel_irq_flags << 24) | ((uint32_t)dma_irq(dma) << 31);
}

void dma_set_interrupt(Dma *dma, uint32_t val)
{
    dma->irq_dummy = val & 0x3F;
    dma->force_irq = (val >> 15) & 1;
    dma->channel_irq_en = (val >> 16) & 0x7F;
    dma->irq_en = (val >> 23) & 1;
    uint8_t ack = (val >> 24) & 0x3F;
    dma->channel_irq_flags = dma->channel_irq_flags & ~ack;
}

Channel *dma_channel(Dma *dma, Port port)
{
    return &dma->channels[(int)port];
}

const Channel *dma_channel_const(const Dma *dma, Port port)
{
    return &dma->channels[(int)port];
}
