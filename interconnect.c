#include "interconnect.h"
#include "map.h"
#include <stdio.h>
#include <stdlib.h>

void inter_init(Interconnect *inter, Bios bios)
{
    inter->bios = bios;
    ram_init(&inter->ram);
}

/* ------------------------------------------------------------------ */
/* GPU stubs (Etapa 4 — implementar gpu.c quando chegar lá)           */
/* ------------------------------------------------------------------ */

static uint32_t gpu_read(uint32_t offset)
{
    switch (offset)
    {
    case 0:
        return 0; /* GPUREAD  */
    case 4:
        return 0x1c000000; /* GPUSTAT — ready bits */
    default:
        printf("[GPU] Read at unknown offset 0x%x\n", offset);
        return 0;
    }
}

static void gpu_write(uint32_t offset, uint32_t val)
{
    switch (offset)
    {
    case 0:
        printf("[GPU] GP0 command: 0x%08x\n", val);
        break;
    case 4:
        printf("[GPU] GP1 command: 0x%08x\n", val);
        break;
    default:
        printf("[GPU] Write 0x%08x at unknown offset 0x%x\n", val, offset);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* LOADS                                                               */
/* ------------------------------------------------------------------ */

uint32_t inter_load32(const Interconnect *inter, uint32_t addr)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_BIOS, phys, &off))
        return bios_load32(&inter->bios, off);

    if (memrange_contains(MAP_RAM, phys, &off))
        return ram_load32(&inter->ram, off);

    if (memrange_contains(MAP_IRQ_CONTROL, phys, &off))
    {
        printf("[IRQ] Load32 at offset 0x%x (stub)\n", off);
        return 0;
    }

    if (memrange_contains(MAP_DMA, phys, &off))
    {
        printf("[DMA] Load32 at offset 0x%x (stub)\n", off);
        return 0;
    }

    if (memrange_contains(MAP_GPU, phys, &off))
        return gpu_read(off);

    if (memrange_contains(MAP_TIMERS, phys, &off))
    {
        printf("[TIM] Load32 at offset 0x%x (stub)\n", off);
        return 0;
    }

    if (memrange_contains(MAP_CDROM, phys, &off))
    {
        printf("[CDR] Load32 at offset 0x%x (stub)\n", off);
        return 0;
    }

    if (memrange_contains(MAP_SPU, phys, &off))
    {
        printf("[SPU] Load32 at offset 0x%x (stub)\n", off);
        return 0;
    }

    if (memrange_contains(MAP_MEM_CTRL, phys, &off))
        return 0;

    if (memrange_contains(MAP_RAM_SIZE, phys, &off))
        return 0;

    if (memrange_contains(MAP_CACHE_CTRL, phys, &off))
        return 0;

    if (memrange_contains(MAP_EXPANSION1, phys, &off))
    {
        printf("[EXP1] Load32 at offset 0x%x\n", off);
        return 0xffffffff; /* nothing connected → pulls high */
    }

    fprintf(stderr, "[BUS] Unhandled load32 at 0x%08x (phys: 0x%08x)\n", addr, phys);
    abort();
}

uint16_t inter_load16(const Interconnect *inter, uint32_t addr)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_RAM, phys, &off))
        return ram_load16(&inter->ram, off);

    if (memrange_contains(MAP_BIOS, phys, &off))
        return bios_load16(&inter->bios, off);

    if (memrange_contains(MAP_SPU, phys, &off))
    {
        printf("[SPU] Load16 at offset 0x%x (stub)\n", off);
        return 0;
    }

    if (memrange_contains(MAP_IRQ_CONTROL, phys, &off))
    {
        printf("[IRQ] Load16 at offset 0x%x (stub)\n", off);
        return 0;
    }

    if (memrange_contains(MAP_EXPANSION2, phys, &off))
    {
        printf("[EXP2] Load16 at offset 0x%x\n", off);
        return 0xffff;
    }

    fprintf(stderr, "[BUS] Unhandled load16 at 0x%08x (phys: 0x%08x)\n", addr, phys);
    abort();
}

uint8_t inter_load8(const Interconnect *inter, uint32_t addr)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_RAM, phys, &off))
        return ram_load8(&inter->ram, off);

    if (memrange_contains(MAP_BIOS, phys, &off))
        return bios_load8(&inter->bios, off);

    if (memrange_contains(MAP_EXPANSION1, phys, &off))
    {
        printf("[EXP1] Load8 at offset 0x%x\n", off);
        return 0xff;
    }

    if (memrange_contains(MAP_CDROM, phys, &off))
    {
        printf("[CDR] Load8 at offset 0x%x (stub)\n", off);
        return 0;
    }

    fprintf(stderr, "[BUS] Unhandled load8 at 0x%08x (phys: 0x%08x)\n", addr, phys);
    abort();
}

/* ------------------------------------------------------------------ */
/* STORES                                                              */
/* ------------------------------------------------------------------ */

void inter_store32(Interconnect *inter, uint32_t addr, uint32_t val)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_RAM, phys, &off))
    {
        ram_store32(&inter->ram, off, val);
        return;
    }

    if (memrange_contains(MAP_IRQ_CONTROL, phys, &off))
    {
        printf("[IRQ] Store32 0x%08x at offset 0x%x (stub)\n", val, off);
        return;
    }

    if (memrange_contains(MAP_DMA, phys, &off))
    {
        printf("[DMA] Store32 0x%08x at offset 0x%x (stub)\n", val, off);
        return;
    }

    if (memrange_contains(MAP_GPU, phys, &off))
    {
        gpu_write(off, val);
        return;
    }

    if (memrange_contains(MAP_TIMERS, phys, &off))
    {
        printf("[TIM] Store32 0x%08x at offset 0x%x (stub)\n", val, off);
        return;
    }

    if (memrange_contains(MAP_MEM_CTRL, phys, &off))
        return;

    if (memrange_contains(MAP_RAM_SIZE, phys, &off))
        return;

    if (memrange_contains(MAP_CACHE_CTRL, phys, &off))
        return;

    fprintf(stderr, "[BUS] Unhandled store32 0x%08x at 0x%08x (phys: 0x%08x)\n",
            val, addr, phys);
    abort();
}

void inter_store16(Interconnect *inter, uint32_t addr, uint16_t val)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_RAM, phys, &off))
    {
        ram_store16(&inter->ram, off, val);
        return;
    }

    if (memrange_contains(MAP_SPU, phys, &off))
    {
        printf("[SPU] Store16 0x%04x at offset 0x%x (stub)\n", val, off);
        return;
    }

    if (memrange_contains(MAP_IRQ_CONTROL, phys, &off))
    {
        printf("[IRQ] Store16 0x%04x at offset 0x%x (stub)\n", val, off);
        return;
    }

    if (memrange_contains(MAP_EXPANSION2, phys, &off))
    {
        printf("[EXP2] Store16 0x%04x at offset 0x%x (stub)\n", val, off);
        return;
    }

    if (memrange_contains(MAP_TIMERS, phys, &off))
    {
        printf("[TIM] Store16 0x%04x at offset 0x%x (stub)\n", val, off);
        return;
    }

    fprintf(stderr, "[BUS] Unhandled store16 0x%04x at 0x%08x (phys: 0x%08x)\n",
            val, addr, phys);
    abort();
}

void inter_store8(Interconnect *inter, uint32_t addr, uint8_t val)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_RAM, phys, &off))
    {
        ram_store8(&inter->ram, off, val);
        return;
    }

    if (memrange_contains(MAP_EXPANSION2, phys, &off))
    {
        /* BIOS debug serial port — many games write here */
        if (off == 0x41)
            putchar((char)val);
        return;
    }

    if (memrange_contains(MAP_CDROM, phys, &off))
    {
        printf("[CDR] Store8 0x%02x at offset 0x%x (stub)\n", val, off);
        return;
    }

    fprintf(stderr, "[BUS] Unhandled store8 0x%02x at 0x%08x (phys: 0x%08x)\n",
            val, addr, phys);
    abort();
}
