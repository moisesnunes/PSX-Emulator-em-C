#include "timer.h"
#include "log.h"
#include <string.h>

#define OVERFLOW_TICKS 0x10000u

/*
 * Clock source encoding per timer (TMODE_CLK_SRC bits 9:8):
 *
 *  Timer 0:  0/2 = system clock   1/3 = dotclock  (no /8; treat as 1:1)
 *  Timer 1:  0/2 = system clock   1/3 = hblank    (hblank not counted here; treat as 1:1)
 *  Timer 2:  0/1 = system clock   2/3 = system/8
 *
 * For Timer 0/1 external sources (dotclock, hblank) we still use system
 * cycles but at 1:1 — accurate emulation would require pixel/line tracking.
 * For Timer 2 /8 we accumulate a remainder to avoid losing sub-cycle ticks.
 */
static bool timer2_div8(const TimerUnit *u) {
    uint32_t src = (u->mode & TMODE_CLK_SRC) >> 8;
    return src == 2 || src == 3;
}

static IrqFlag timer_irq_flag(int idx) {
    switch (idx) {
        case 0: return IRQ_TIMER0;
        case 1: return IRQ_TIMER1;
        default: return IRQ_TIMER2;
    }
}

void timers_init(Timers *t) {
    memset(t, 0, sizeof(*t));
}

/* ---------- load/store ---------- */

static int      unit_index(uint32_t offset) { return (int)(offset >> 4); }
static uint32_t reg_offset(uint32_t offset) { return offset & 0x0F; }

uint32_t timers_load32(Timers *t, uint32_t offset) {
    int idx = unit_index(offset);
    if (idx > 2) return 0;
    TimerUnit *u = &t->units[idx];
    uint32_t val;
    switch (reg_offset(offset)) {
        case 0: val = u->value; break;
        case 4:
            val = u->mode;
            /* reading mode register clears sticky bits */
            u->mode &= (uint16_t)~(TMODE_TARGET_REACHED | TMODE_OVERFLOW_REACHED);
            break;
        case 8:  val = u->target; break;
        default: val = 0;         break;
    }
    return val;
}

uint16_t timers_load16(Timers *t, uint32_t offset) {
    return (uint16_t)timers_load32(t, offset);
}

void timers_store32(Timers *t, uint32_t offset, uint32_t val, Scheduler *sched) {
    int idx = unit_index(offset);
    if (idx > 2) return;
    TimerUnit *u = &t->units[idx];

    switch (reg_offset(offset)) {
        case 0:
            u->value    = (uint16_t)val;
            u->frac_rem = 0;
            break;
        case 4:
            /* Writing mode resets counter, clears sticky flags, resets one-shot state */
            u->value    = 0;
            u->frac_rem = 0;
            u->fired    = false;
            u->mode     = (uint16_t)(val & 0x1FFF);
            u->mode    &= (uint16_t)~(TMODE_TARGET_REACHED | TMODE_OVERFLOW_REACHED);
            /* IRQ bit starts high (not firing) */
            u->mode    |= TMODE_IRQ_STATUS;
            LOG(LOG_IRQ, "Timer%d mode=%04X clksrc=%u div8=%d",
                idx, u->mode, (u->mode & TMODE_CLK_SRC) >> 8, timer2_div8(u));
            /* cancel any stale scheduler event for this timer */
            scheduler_cancel(sched, idx == 0 ? EVENT_TIMER0 :
                                    idx == 1 ? EVENT_TIMER1 : EVENT_TIMER2);
            break;
        case 8:
            u->target = (uint16_t)val;
            break;
        default:
            break;
    }
}

void timers_store16(Timers *t, uint32_t offset, uint16_t val, Scheduler *sched) {
    timers_store32(t, offset, (uint32_t)val, sched);
}

/* ---------- step ---------- */

static void fire_irq(TimerUnit *u, int idx, Irq *irq) {
    bool repeat = (u->mode & TMODE_IRQ_REPEAT) != 0;
    bool toggle = (u->mode & TMODE_IRQ_TOGGLE) != 0;

    if (!repeat && u->fired) return; /* one-shot already done */

    if (toggle) {
        /* toggle IRQ_STATUS bit; IRQ fires on 0→1 transition ... actually
           the bit is inverted: 0 = IRQ asserted.  Toggle flips it each event. */
        u->mode ^= TMODE_IRQ_STATUS;
        if (!(u->mode & TMODE_IRQ_STATUS)) {
            /* bit went low → IRQ asserted */
            irq_assert(irq, timer_irq_flag(idx));
            u->fired = true;
        }
    } else {
        /* pulse mode: always assert */
        u->mode &= (uint16_t)~TMODE_IRQ_STATUS; /* pulse low briefly */
        irq_assert(irq, timer_irq_flag(idx));
        u->mode |= TMODE_IRQ_STATUS;             /* back high */
        u->fired = true;
    }
}

void timers_step(Timers *t, uint32_t cycles, Irq *irq, Scheduler *sched) {
    (void)sched; /* scheduler not used for per-tick timer events */

    for (int idx = 0; idx < 3; idx++) {
        TimerUnit *u = &t->units[idx];

        /* Apply /8 divider for Timer 2 when CLK_SRC selects system/8 */
        uint32_t ticks;
        if (idx == 2 && timer2_div8(u)) {
            uint32_t total = u->frac_rem + cycles;
            ticks          = total >> 3; /* divide by 8 */
            u->frac_rem    = (uint8_t)(total & 7);
        } else {
            ticks       = cycles;
            u->frac_rem = 0;
        }

        if (ticks == 0) continue;

        /* Counters always advance regardless of IRQ enable bits */
        uint32_t new_val = (uint32_t)u->value + ticks;

        /* --- Target match --- */
        bool target_hit = false;
        if (u->target > 0 && u->value < u->target && new_val >= u->target) {
            target_hit = true;
            u->mode   |= TMODE_TARGET_REACHED;
            if (u->mode & TMODE_RESET_TARGET) {
                /* reset counter to 0 after target */
                new_val = new_val - u->target;
            }
        }

        /* --- Overflow --- */
        bool overflow_hit = false;
        if (new_val >= OVERFLOW_TICKS) {
            overflow_hit = true;
            u->mode     |= TMODE_OVERFLOW_REACHED;
            new_val      &= 0xFFFF;
        }

        u->value = (uint16_t)new_val;

        /* --- Fire IRQ if enabled --- */
        if (target_hit && (u->mode & TMODE_IRQ_TARGET)) {
            LOG(LOG_IRQ, "Timer%d target IRQ val=%04X", idx, u->value);
            fire_irq(u, idx, irq);
        }
        if (overflow_hit && (u->mode & TMODE_IRQ_OVERFLOW)) {
            LOG(LOG_IRQ, "Timer%d overflow IRQ", idx);
            fire_irq(u, idx, irq);
        }
    }
}
