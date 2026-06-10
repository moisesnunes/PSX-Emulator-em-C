#include "timer.h"
#include "log.h"
#include <string.h>

#define OVERFLOW_TICKS 0x10000u

/* Timer0 uses system/dotclock, Timer1 system/HBlank, Timer2 system/system/8. */
static bool timer2_div8(const TimerUnit *u)
{
    uint32_t src = (u->mode & TMODE_CLK_SRC) >> 8;
    return src == 2 || src == 3;
}

static IrqFlag timer_irq_flag(int idx)
{
    switch (idx)
    {
    case 0:
        return IRQ_TIMER0;
    case 1:
        return IRQ_TIMER1;
    default:
        return IRQ_TIMER2;
    }
}

void timers_init(Timers *t)
{
    memset(t, 0, sizeof(*t));
}

/* ---------- load/store ---------- */

static int unit_index(uint32_t offset) { return (int)(offset >> 4); }
static uint32_t reg_offset(uint32_t offset) { return offset & 0x0F; }

uint32_t timers_load32(Timers *t, uint32_t offset)
{
    int idx = unit_index(offset);
    if (idx > 2)
        return 0;
    TimerUnit *u = &t->units[idx];
    uint32_t val;
    switch (reg_offset(offset))
    {
    case 0:
        val = u->value;
        break;
    case 4:
        val = u->mode;
        /* reading mode register clears sticky bits */
        u->mode &= (uint16_t)~(TMODE_TARGET_REACHED | TMODE_OVERFLOW_REACHED);
        break;
    case 8:
        val = u->target;
        break;
    default:
        val = 0;
        break;
    }
    return val;
}

uint16_t timers_load16(Timers *t, uint32_t offset)
{
    return (uint16_t)timers_load32(t, offset);
}

void timers_store32(Timers *t, uint32_t offset, uint32_t val, Scheduler *sched)
{
    int idx = unit_index(offset);
    if (idx > 2)
        return;
    TimerUnit *u = &t->units[idx];

    switch (reg_offset(offset))
    {
    case 0:
        u->value = (uint16_t)val;
        u->frac_rem = 0;
        break;
    case 4:
        /* Writing mode resets counter, clears sticky flags, resets one-shot state */
        u->value = 0;
        u->frac_rem = 0;
        u->fired = false;
        u->sync_started = false;
        u->reset_pending = false;
        u->mode = (uint16_t)(val & 0x1FFF);
        u->mode &= (uint16_t)~(TMODE_TARGET_REACHED | TMODE_OVERFLOW_REACHED);
        /* IRQ bit starts high (not firing) */
        u->mode |= TMODE_IRQ_STATUS;
        LOG(LOG_IRQ, "Timer%d mode=%04X clksrc=%u div8=%d",
            idx, u->mode, (u->mode & TMODE_CLK_SRC) >> 8, timer2_div8(u));
        /* cancel any stale scheduler event for this timer */
        scheduler_cancel(sched, idx == 0 ? EVENT_TIMER0 : idx == 1 ? EVENT_TIMER1
                                                                   : EVENT_TIMER2);
        break;
    case 8:
        u->target = (uint16_t)val;
        break;
    default:
        break;
    }
}

void timers_store16(Timers *t, uint32_t offset, uint16_t val, Scheduler *sched)
{
    timers_store32(t, offset, (uint32_t)val, sched);
}

/* ---------- step ---------- */

static void fire_irq(TimerUnit *u, int idx, Irq *irq)
{
    bool repeat = (u->mode & TMODE_IRQ_REPEAT) != 0;
    bool toggle = (u->mode & TMODE_IRQ_TOGGLE) != 0;

    if (!repeat && u->fired)
        return; /* one-shot already done */

    if (toggle)
    {
        /* toggle IRQ_STATUS bit; IRQ fires on 0→1 transition ... actually
           the bit is inverted: 0 = IRQ asserted.  Toggle flips it each event. */
        u->mode ^= TMODE_IRQ_STATUS;
        if (!(u->mode & TMODE_IRQ_STATUS))
        {
            /* bit went low → IRQ asserted */
            irq_assert(irq, timer_irq_flag(idx));
            u->fired = true;
        }
    }
    else
    {
        /* pulse mode: always assert */
        u->mode &= (uint16_t)~TMODE_IRQ_STATUS; /* pulse low briefly */
        irq_assert(irq, timer_irq_flag(idx));
        u->mode |= TMODE_IRQ_STATUS; /* back high */
        u->fired = true;
    }
}

static bool external_clock_selected(const TimerUnit *u, int idx)
{
    uint32_t src = (u->mode & TMODE_CLK_SRC) >> 8;
    if (idx == 0 || idx == 1)
        return (src & 1u) != 0;
    return false;
}

static uint32_t timer_ticks(TimerUnit *u, int idx, uint32_t cycles,
                            uint32_t dotclock_ticks, uint32_t hblank_count)
{
    if (idx == 0 && external_clock_selected(u, idx))
        return dotclock_ticks;
    if (idx == 1 && external_clock_selected(u, idx))
        return hblank_count;
    if (idx == 2 && timer2_div8(u))
    {
        uint32_t total = u->frac_rem + cycles;
        u->frac_rem = (uint8_t)(total & 7);
        return total >> 3;
    }
    u->frac_rem = 0;
    return cycles;
}

static uint32_t apply_sync_period(TimerUnit *u, int idx, uint32_t ticks,
                                  bool in_hblank, bool in_vblank)
{
    if (!(u->mode & TMODE_SYNC_ENABLE))
        return ticks;

    uint32_t sync_mode = (u->mode & TMODE_SYNC_MODE) >> 1;
    if (idx == 2)
    {
        if (sync_mode == 0 || sync_mode == 3)
            return 0;
        return ticks;
    }

    bool in_sync_period = idx == 0 ? in_hblank : in_vblank;
    switch (sync_mode)
    {
    case 0: /* pause during HBlank/VBlank */
        return in_sync_period ? 0 : ticks;
    case 1: /* reset at HBlank/VBlank */
        return ticks;
    case 2: /* count only during HBlank/VBlank and reset at entry */
        return in_sync_period ? ticks : 0;
    case 3: /* pause until first HBlank/VBlank, then free run */
        return u->sync_started ? ticks : 0;
    default:
        return ticks;
    }
}

static void apply_sync_edge(TimerUnit *u, int idx, bool sync_started)
{
    if (!(u->mode & TMODE_SYNC_ENABLE) || idx == 2 || !sync_started)
        return;

    uint32_t sync_mode = (u->mode & TMODE_SYNC_MODE) >> 1;
    if (sync_mode == 1 || sync_mode == 2)
    {
        u->value = 0;
        u->reset_pending = false;
    }
    else if (sync_mode == 3)
    {
        u->sync_started = true;
    }
}

static void timer_advance(TimerUnit *u, int idx, uint32_t ticks, Irq *irq)
{
    while (ticks-- > 0)
    {
        if (u->reset_pending)
        {
            u->value = 0;
            u->reset_pending = false;
            continue;
        }

        uint16_t old_value = u->value;
        u->value++;

        bool target_hit = u->value == u->target;
        if (target_hit)
        {
            u->mode |= TMODE_TARGET_REACHED;
            if (u->mode & TMODE_IRQ_TARGET)
                fire_irq(u, idx, irq);
            if (u->mode & TMODE_RESET_TARGET)
                u->reset_pending = true;
        }

        if (old_value == 0xFFFFu)
        {
            u->mode |= TMODE_OVERFLOW_REACHED;
            if (u->mode & TMODE_IRQ_OVERFLOW)
                fire_irq(u, idx, irq);
        }
    }
}

void timers_step(Timers *t, uint32_t cycles, uint32_t dotclock_ticks,
                 uint32_t hblank_count, bool in_hblank,
                 bool vblank_started, bool in_vblank,
                 Irq *irq, Scheduler *sched)
{
    (void)sched; /* scheduler not used for per-tick timer events */

    for (int idx = 0; idx < 3; idx++)
    {
        TimerUnit *u = &t->units[idx];

        uint32_t ticks = timer_ticks(u, idx, cycles, dotclock_ticks, hblank_count);
        ticks = apply_sync_period(u, idx, ticks, in_hblank, in_vblank);
        timer_advance(u, idx, ticks, irq);

        bool sync_edge = idx == 0 ? hblank_count != 0 : vblank_started;
        apply_sync_edge(u, idx, sync_edge);
    }
}
