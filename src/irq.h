#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t status; /* I_STAT @ 0x1F801070 — set when IRQ fires        */
    uint16_t mask;   /* I_MASK @ 0x1F801074 — which IRQs reach the CPU  */
} Irq;

typedef enum {
    IRQ_VBLANK = 1 << 0,
    IRQ_GPU    = 1 << 1,
    IRQ_CDROM  = 1 << 2,
    IRQ_DMA    = 1 << 3,
    IRQ_TIMER0 = 1 << 4,
    IRQ_TIMER1 = 1 << 5,
    IRQ_TIMER2 = 1 << 6,
    IRQ_SIO    = 1 << 7,
    IRQ_SPU    = 1 << 9,
} IrqFlag;

void     irq_init(Irq *irq);
void     irq_assert(Irq *irq, IrqFlag flag);
bool     irq_pending(const Irq *irq);
uint32_t irq_load32(const Irq *irq, uint32_t offset);
uint16_t irq_load16(const Irq *irq, uint32_t offset);
void     irq_store32(Irq *irq, uint32_t offset, uint32_t val);
void     irq_store16(Irq *irq, uint32_t offset, uint16_t val);

#endif /* IRQ_H */
