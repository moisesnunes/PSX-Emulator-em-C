#include "cpu_timing.h"

static uint32_t count_leading_zeroes(uint32_t value)
{
    return value == 0 ? 32u : (uint32_t)__builtin_clz(value);
}

uint32_t cpu_instruction_base_cycles(uint32_t instruction)
{
    (void)instruction;
    return 2;
}

uint32_t cpu_multiply_cycles_signed(uint32_t operand)
{
    uint32_t sign_extension = (uint32_t)((int32_t)operand >> 21);
    uint32_t leading_zeroes = count_leading_zeroes((operand ^ sign_extension) | 1u);
    return 7u + (2u - leading_zeroes / 11u) * 4u;
}

uint32_t cpu_multiply_cycles_unsigned(uint32_t operand)
{
    uint32_t leading_zeroes = count_leading_zeroes(operand | 1u);
    return 7u + (2u - leading_zeroes / 11u) * 4u;
}
