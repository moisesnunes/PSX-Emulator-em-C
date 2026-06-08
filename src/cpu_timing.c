#include "cpu_timing.h"

static uint32_t count_leading_zeroes(uint32_t value)
{
    return value == 0 ? 32u : (uint32_t)__builtin_clz(value);
}

uint32_t cpu_instruction_base_cycles(uint32_t instruction)
{
    uint32_t opcode = instruction >> 26;

    switch (opcode)
    {
    case 0x20: /* LB */
    case 0x21: /* LH */
    case 0x22: /* LWL */
    case 0x23: /* LW */
    case 0x24: /* LBU */
    case 0x25: /* LHU */
    case 0x26: /* LWR */
    case 0x28: /* SB */
    case 0x29: /* SH */
    case 0x2A: /* SWL */
    case 0x2B: /* SW */
    case 0x2E: /* SWR */
    case 0x32: /* LWC2 */
    case 0x3A: /* SWC2 */
        return 4;
    default:
        return 2;
    }
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
