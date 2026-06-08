#pragma once

#include <stdint.h>

uint32_t cpu_instruction_base_cycles(uint32_t instruction);
uint32_t cpu_multiply_cycles_signed(uint32_t operand);
uint32_t cpu_multiply_cycles_unsigned(uint32_t operand);
