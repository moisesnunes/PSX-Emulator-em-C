#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    DIRECTION_TO_RAM   = 0,
    DIRECTION_FROM_RAM = 1,
} Direction;

typedef enum {
    STEP_INCREMENT = 0,
    STEP_DECREMENT = 1,
} Step;

typedef enum {
    SYNC_MANUAL      = 0,
    SYNC_REQUEST     = 1,
    SYNC_LINKED_LIST = 2,
} Sync;

typedef struct {
    bool      enable;
    Direction direction;
    Step      step;
    Sync      sync;
    bool      trigger;
    bool      chop;
    uint8_t   chop_dma_sz;
    uint8_t   chop_cpu_sz;
    uint8_t   dummy;

    uint32_t  base;
    uint16_t  block_size;
    uint16_t  block_count;
} Channel;

void     channel_init(Channel *ch);
uint32_t channel_control(const Channel *ch);
void     channel_set_control(Channel *ch, uint32_t val);
uint32_t channel_base(const Channel *ch);
void     channel_set_base(Channel *ch, uint32_t val);
uint32_t channel_block_control(const Channel *ch);
void     channel_set_block_control(Channel *ch, uint32_t val);
bool     channel_active(const Channel *ch);
Direction channel_direction(const Channel *ch);
Step     channel_step(const Channel *ch);
Sync     channel_sync(const Channel *ch);
/* Returns 0 on LinkedList (no size). Sets *out and returns 1 on success. */
int      channel_transfer_size(const Channel *ch, uint32_t *out);
void     channel_done(Channel *ch);
