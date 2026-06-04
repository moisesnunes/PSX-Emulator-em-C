#include "scheduler.h"
#include "log.h"

void scheduler_init(Scheduler *s) {
    s->current_cycle = 0;
    for (int i = 0; i < EVENT_COUNT; i++) {
        s->events[i].active  = false;
        s->events[i].fire_at = 0;
        s->events[i].type    = (EventType)i;
    }
}

void scheduler_schedule(Scheduler *s, EventType type, uint32_t delta_cycles) {
    s->events[type].fire_at = s->current_cycle + delta_cycles;
    s->events[type].active  = true;
}

void scheduler_cancel(Scheduler *s, EventType type) {
    s->events[type].active = false;
}

uint32_t scheduler_step(Scheduler *s, uint32_t cycles, Irq *irq) {
    s->current_cycle += cycles;
    uint32_t fired = 0;

    for (int i = 0; i < EVENT_COUNT; i++) {
        Event *e = &s->events[i];
        if (!e->active || s->current_cycle < e->fire_at) continue;

        e->active = false;
        fired |= (1u << i);

        switch ((EventType)i) {
        case EVENT_VBLANK: break;
        case EVENT_TIMER0: irq_assert(irq, IRQ_TIMER0); break;
        case EVENT_TIMER1: irq_assert(irq, IRQ_TIMER1); break;
        case EVENT_TIMER2: irq_assert(irq, IRQ_TIMER2); break;
        case EVENT_CDROM_IRQ: /* cdrom_on_scheduler_event called by caller */ break;
        case EVENT_SPU_SAMPLE: irq_assert(irq, IRQ_SPU); break;
        default: break;
        }
    }
    return fired;
}
