#include "irq.h"
#include "log.h"
#include <stdio.h>

void irq_init(Irq *irq) {
    irq->status = 0;
    irq->mask   = 0;
}

void irq_assert(Irq *irq, IrqFlag flag) {
    irq->status |= (uint16_t)flag;
    LOG(LOG_IRQ, "assert flag=0x%04x status=0x%04x mask=0x%04x pending=%d",
        (unsigned)flag, (unsigned)irq->status, (unsigned)irq->mask, irq_pending(irq));
}

bool irq_pending(const Irq *irq) {
    return (irq->status & irq->mask) != 0;
}

uint32_t irq_load32(const Irq *irq, uint32_t offset) {
    switch (offset) {
    case 0: return irq->status;
    case 4: return irq->mask;
    default:
        LOG(LOG_IRQ, "unhandled load32 offset=0x%x", offset);
        return 0;
    }
}

uint16_t irq_load16(const Irq *irq, uint32_t offset) {
    return (uint16_t)irq_load32(irq, offset);
}

void irq_store32(Irq *irq, uint32_t offset, uint32_t val) {
    switch (offset) {
    case 0:
        /* Writing 0 to a bit acknowledges (clears) it; writing 1 has no effect */
        irq->status &= (uint16_t)val;
        LOG(LOG_IRQ, "I_STAT ack write=0x%04x status=0x%04x", (unsigned)val, (unsigned)irq->status);
        break;
    case 4:
        irq->mask = (uint16_t)val;
        LOG(LOG_IRQ, "I_MASK write=0x%04x", (unsigned)irq->mask);
        break;
    default:
        LOG(LOG_IRQ, "unhandled store32 offset=0x%x val=0x%x", offset, val);
        break;
    }
}

void irq_store16(Irq *irq, uint32_t offset, uint16_t val) {
    irq_store32(irq, offset, val);
}
