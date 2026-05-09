#ifndef PSX_COP0_H
#define PSX_COP0_H

#include <stdint.h>

typedef struct
{
    uint32_t sr;        /* Status Register   ($12) */
    uint32_t cause;     /* Cause Register    ($13) */
    uint32_t epc;       /* Exception PC      ($14) */
    uint32_t bad_vaddr; /* Bad Virtual Addr  ($8)  */
} Cop0;

void cop0_init(Cop0 *cop0);
uint32_t cop0_read(const Cop0 *cop0, uint32_t reg);
void cop0_write(Cop0 *cop0, uint32_t reg, uint32_t val);
int cop0_status_bev(const Cop0 *cop0);
int cop0_cache_isolated(const Cop0 *cop0);
void cop0_enter_exception(Cop0 *cop0, uint32_t cause_code,
                          uint32_t current_pc, int in_delay_slot);
void cop0_return_from_exception(Cop0 *cop0);

#endif /* PSX_COP0_H */
