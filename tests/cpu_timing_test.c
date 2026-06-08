#include "cpu_timing.h"
#include <stdint.h>
#include <stdio.h>

static int passed;
static int failed;

static void expect_cycles(const char *name, uint32_t instruction, uint32_t expected)
{
    uint32_t actual = cpu_instruction_base_cycles(instruction);
    if (actual != expected)
    {
        fprintf(stderr, "%s: expected %u cycles, got %u for 0x%08X\n",
                name, expected, actual, instruction);
        failed++;
        return;
    }
    passed++;
}

static void expect_value(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "%s: expected %u, got %u\n", name, expected, actual);
        failed++;
        return;
    }
    passed++;
}

int main(void)
{
    expect_cycles("sll", 0x00000000u, 2);
    expect_cycles("addiu-immediate-looks-like-load", 0x24000023u, 2);
    expect_cycles("lw", 0x8C220000u, 4);
    expect_cycles("sw", 0xAC220000u, 4);
    expect_cycles("lwl", 0x88220003u, 4);
    expect_cycles("swr", 0xB8220001u, 4);
    expect_cycles("lwc2", 0xC8220000u, 4);
    expect_cycles("swc2", 0xE8220000u, 4);
    expect_value("mult-signed-small-positive", cpu_multiply_cycles_signed(0), 7);
    expect_value("mult-signed-small-negative", cpu_multiply_cycles_signed(UINT32_MAX), 7);
    expect_value("mult-signed-large", cpu_multiply_cycles_signed(0x40000000u), 15);
    expect_value("mult-unsigned-small", cpu_multiply_cycles_unsigned(0x3FFu), 7);
    expect_value("mult-unsigned-medium", cpu_multiply_cycles_unsigned(0x400u), 11);
    expect_value("mult-unsigned-large", cpu_multiply_cycles_unsigned(0x80000000u), 15);

    printf("cpu timing tests: pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
