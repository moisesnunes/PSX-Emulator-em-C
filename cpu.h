#ifndef PSX_CPU_H
#define PSX_CPU_H

#include "interconnect.h"
#include "cop0.h"
#include <stdint.h>

#define NUM_REGS 32

/* 32-register file — $0 is always 0 */
typedef struct
{
    uint32_t r[NUM_REGS];
} RegisterFile;

typedef struct
{
    uint32_t pc;
    uint32_t next_pc;
    uint32_t current_pc;
    RegisterFile regs;
    uint32_t hi;
    uint32_t lo;
    Cop0 cop0;
    Interconnect inter;
    uint32_t load_delay_reg; /* pending load-delay register index */
    uint32_t load_delay_val; /* pending load-delay value          */
    int branch;              /* next instruction is a delay slot  */
    int delay_slot;          /* current instruction is delay slot */
} Cpu;

void cpu_init(Cpu *cpu, Bios bios);
uint32_t cpu_pc(const Cpu *cpu);
void cpu_run_next_instruction(Cpu *cpu);

#endif /* PSX_CPU_H */
