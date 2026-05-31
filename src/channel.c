#include "channel.h"
#include <stdio.h>
#include <stdlib.h>

void channel_init(Channel *ch)
{
    ch->enable = false;
    ch->direction = DIRECTION_TO_RAM;
    ch->step = STEP_INCREMENT;
    ch->sync = SYNC_MANUAL;
    ch->trigger = false;
    ch->chop = false;
    ch->chop_dma_sz = 0;
    ch->chop_cpu_sz = 0;
    ch->dummy = 0;
    ch->base = 0;
    ch->block_size = 0;
    ch->block_count = 0;
}

uint32_t channel_control(const Channel *ch)
{
    return ((uint32_t)ch->direction << 0) | ((uint32_t)ch->step << 1) | ((uint32_t)ch->chop << 8) | ((uint32_t)ch->sync << 9) | ((uint32_t)ch->chop_dma_sz << 16) | ((uint32_t)ch->chop_cpu_sz << 20) | ((uint32_t)ch->enable << 24) | ((uint32_t)ch->trigger << 28) | ((uint32_t)ch->dummy << 29);
}

void channel_set_control(Channel *ch, uint32_t val)
{
    ch->direction = (val & 1) ? DIRECTION_FROM_RAM : DIRECTION_TO_RAM;
    ch->step = ((val >> 1) & 1) ? STEP_DECREMENT : STEP_INCREMENT;
    ch->chop = (val >> 8) & 1;
    switch ((val >> 9) & 3)
    {
    case 0:
        ch->sync = SYNC_MANUAL;
        break;
    case 1:
        ch->sync = SYNC_REQUEST;
        break;
    case 2:
        ch->sync = SYNC_LINKED_LIST;
        break;
    default:
        fprintf(stderr, "Unknown DMA sync mode: %u\n", (val >> 9) & 3);
        exit(1);
    }
    ch->chop_dma_sz = (val >> 16) & 7;
    ch->chop_cpu_sz = (val >> 20) & 7;
    ch->enable = (val >> 24) & 1;
    ch->trigger = (val >> 28) & 1;
    ch->dummy = (val >> 29) & 3;
}

uint32_t channel_base(const Channel *ch)
{
    return ch->base;
}

void channel_set_base(Channel *ch, uint32_t val)
{
    ch->base = val & 0x00FFFFFF;
}

uint32_t channel_block_control(const Channel *ch)
{
    return ((uint32_t)ch->block_count << 16) | ch->block_size;
}

void channel_set_block_control(Channel *ch, uint32_t val)
{
    ch->block_size = (uint16_t)(val);
    ch->block_count = (uint16_t)(val >> 16);
}

bool channel_active(const Channel *ch)
{
    bool trigger = (ch->sync == SYNC_MANUAL) ? ch->trigger : true;
    return ch->enable && trigger;
}

Direction channel_direction(const Channel *ch) { return ch->direction; }
Step channel_step(const Channel *ch) { return ch->step; }
Sync channel_sync(const Channel *ch) { return ch->sync; }

int channel_transfer_size(const Channel *ch, uint32_t *out)
{
    uint32_t bs = ch->block_size;
    uint32_t bc = ch->block_count;
    switch (ch->sync)
    {
    case SYNC_MANUAL:
        *out = bs;
        return 1;
    case SYNC_REQUEST:
        *out = bc * bs;
        return 1;
    default:
        return 0;
    }
}

void channel_done(Channel *ch)
{
    ch->enable = false;
    ch->trigger = false;
}
