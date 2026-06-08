#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "irq.h"
#include <stdint.h>
#include <stdbool.h>

/* PS1 CPU runs at ~33.868 MHz */
#define PS1_CPU_HZ 33868800U

typedef enum
{
    EVENT_VBLANK = 0,
    EVENT_TIMER0,
    EVENT_TIMER1,
    EVENT_TIMER2,
    EVENT_CDROM_IRQ,
    EVENT_SPU_SAMPLE,
    EVENT_DMA0,
    EVENT_DMA1,
    EVENT_DMA2,
    EVENT_DMA3,
    EVENT_DMA4,
    EVENT_DMA5,
    EVENT_DMA6,
    EVENT_COUNT
} EventType;

typedef struct
{
    uint64_t fire_at; /* absolute cycle number when this event fires */
    EventType type;
    bool active;
} Event;

typedef struct
{
    uint64_t current_cycle;
    Event events[EVENT_COUNT];
} Scheduler;

void scheduler_init(Scheduler *s);
void scheduler_schedule(Scheduler *s, EventType type, uint32_t delta_cycles);
void scheduler_cancel(Scheduler *s, EventType type);
uint32_t scheduler_cycles_until_next_event(const Scheduler *s);

/* Advance by `cycles`, fire elapsed events, assert IRQs.
   Returns bitmask of EVENT_* that fired (1 << EventType). */
uint32_t scheduler_step(Scheduler *s, uint32_t cycles, Irq *irq);

#endif /* SCHEDULER_H */
