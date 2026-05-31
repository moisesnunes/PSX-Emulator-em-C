#pragma once
#include "interconnect.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint32_t pc;
    uint32_t next_pc;
    uint32_t regs[32];
    uint32_t out_regs[32];
    Interconnect inter;
    uint32_t next_instruction;

    /* COP0 */
    uint32_t sr;
    uint32_t current_pc;
    uint32_t cause;
    uint32_t epc;

    uint8_t load_reg;
    uint32_t load_val;

    uint32_t hi;
    uint32_t lo;

    bool branch;
    bool delay_slot;
} Cpu;

int cpu_init(Cpu *cpu, const char *bios_path, SDL_Window *window, bool headless, const char *disc_path);
uint32_t cpu_run_next_instruction(Cpu *cpu); /* returns approx. cycles consumed */
void cpu_destroy(Cpu *cpu);
