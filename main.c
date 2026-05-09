#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "bios.h"

int main(int argc, char *argv[])
{
    const char *bios_path = (argc > 1) ? argv[1] : "roms/SCPH1001.BIN";

    printf("PSX Emulator - Starting\n");
    printf("Loading BIOS: %s\n", bios_path);

    Bios bios;
    if (bios_load(&bios, bios_path) != 0)
    {
        fprintf(stderr, "Failed to load BIOS\n");
        return 1;
    }

    Cpu cpu;
    cpu_init(&cpu, bios);

    printf("CPU initialized. PC = 0x%08x\n", cpu_pc(&cpu));
    printf("Starting emulation loop...\n\n");

    for (;;)
        cpu_run_next_instruction(&cpu);

    return 0;
}
