#ifndef TIMER_H
#define TIMER_H

#include "irq.h"
#include "scheduler.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * PS1 Root Counters (timers) — three identical units at:
 *   Timer 0: 0x1F801100  (clock source: system or dotclock)
 *   Timer 1: 0x1F801110  (clock source: system or hblank)
 *   Timer 2: 0x1F801120  (clock source: system or system/8)
 *
 * Each unit has three 16-bit registers at offsets 0, 4, 8:
 *   +0  Counter Value (R/W — resets to 0 on write)
 *   +4  Counter Mode  (R/W — see TimerMode bits)
 *   +8  Counter Target
 */

/* TimerMode bit fields (write to +4) */
#define TMODE_SYNC_ENABLE (1u << 0)       /* 0=free-run, 1=sync to external */
#define TMODE_SYNC_MODE (3u << 1)         /* sync mode (timer-specific) */
#define TMODE_RESET_TARGET (1u << 3)      /* 1=reset counter on target match */
#define TMODE_IRQ_TARGET (1u << 4)        /* 1=IRQ on target match */
#define TMODE_IRQ_OVERFLOW (1u << 5)      /* 1=IRQ on overflow (0xFFFF→0) */
#define TMODE_IRQ_REPEAT (1u << 6)        /* 0=one-shot, 1=repeat */
#define TMODE_IRQ_TOGGLE (1u << 7)        /* 0=pulse IRQ bit, 1=toggle */
#define TMODE_CLK_SRC (3u << 8)           /* clock source select */
#define TMODE_IRQ_STATUS (1u << 10)       /* current IRQ status (R) */
#define TMODE_TARGET_REACHED (1u << 11)   /* sticky: target reached since last read */
#define TMODE_OVERFLOW_REACHED (1u << 12) /* sticky: overflow since last read */

typedef struct
{
    uint16_t value;   /* current counter value */
    uint16_t mode;    /* mode register */
    uint16_t target;  /* target compare value */
    uint8_t frac_rem; /* sub-cycle remainder for Timer2 /8 divider */
    bool fired;       /* true after first IRQ in one-shot mode */
    bool sync_started;
} TimerUnit;

typedef struct
{
    TimerUnit units[3];
} Timers;

void timers_init(Timers *t);

uint32_t timers_load32(Timers *t, uint32_t offset);
uint16_t timers_load16(Timers *t, uint32_t offset);
void timers_store32(Timers *t, uint32_t offset, uint32_t val, Scheduler *sched);
void timers_store16(Timers *t, uint32_t offset, uint16_t val, Scheduler *sched);

/* Advance all timers by `cycles` system-clock cycles.
   Fires IRQs via `irq` and reschedules via `sched` when needed. */
void timers_step(Timers *t, uint32_t cycles, uint32_t dotclock_ticks,
                 uint32_t hblank_count, bool vblank_started, bool in_vblank,
                 Irq *irq, Scheduler *sched);

#endif /* TIMER_H */
