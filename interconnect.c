#include "interconnect.h"
#include "map.h"
#include <stdio.h>
#include <stdlib.h>

void inter_init(Interconnect *inter, Bios bios)
{
    inter->bios = bios;
    ram_init(&inter->ram);
    gpu_init(&inter->gpu);
}

void inter_destroy(Interconnect *inter)
{
    gpu_destroy(&inter->gpu);
}

/* ------------------------------------------------------------------ */
/* DMA stub                                                            */
/* ------------------------------------------------------------------ */

static uint32_t dma_read(uint32_t offset)
{
    uint32_t ch = (offset >> 4) & 0xfu;
    if (ch == 7 && (offset & 0xfu) == 0)
        return 0x07654321u;  /* DPCR — prioridades padrão */
    return 0;
}

static void dma_gpu_linked_list(Interconnect *inter)
{
    /* TODO Etapa 5: ler MADR do canal 2 e processar lista encadeada */
    (void)inter;
}

static void dma_write(Interconnect *inter, uint32_t offset, uint32_t val)
{
    uint32_t ch = (offset >> 4) & 0xfu;
    /* Canal 2 = GPU DMA — quando enabled+triggered em modo linked-list */
    if (ch == 2 && (offset & 0xfu) == 8) {
        uint32_t enable  = (val >> 24) & 1u;
        uint32_t trigger = (val >> 28) & 1u;
        uint32_t sync    = (val >> 9)  & 3u;
        if (enable && trigger && sync == 2)
            dma_gpu_linked_list(inter);
    }
}

/* ------------------------------------------------------------------ */
/* LOADS                                                               */
/* ------------------------------------------------------------------ */

uint32_t inter_load32(Interconnect *inter, uint32_t addr)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_BIOS, phys, &off))
        return bios_load32(&inter->bios, off);

    if (memrange_contains(MAP_RAM, phys, &off))
        return ram_load32(&inter->ram, off);

    if (memrange_contains(MAP_GPU, phys, &off)) {
        switch (off) {
        case 0: return gpu_gpuread(&inter->gpu);
        case 4: return gpu_gpustat(&inter->gpu);
        default: return 0;
        }
    }

    if (memrange_contains(MAP_IRQ_CONTROL, phys, &off))
        return 0;

    if (memrange_contains(MAP_DMA, phys, &off))
        return dma_read(off);

    if (memrange_contains(MAP_TIMERS, phys, &off))
        return 0;

    if (memrange_contains(MAP_MEM_CTRL, phys, &off))
        return 0;

    if (memrange_contains(MAP_RAM_SIZE, phys, &off))
        return 0;

    if (memrange_contains(MAP_CACHE_CTRL, phys, &off))
        return 0;

    if (memrange_contains(MAP_EXPANSION1, phys, &off))
        return 0xffffffffu;

    if (memrange_contains(MAP_SPU, phys, &off))
        return 0;

    if (memrange_contains(MAP_CDROM, phys, &off))
        return 0;

    printf("[BUS] Unhandled load32 @ 0x%08x\n", addr);
    return 0;
}

uint16_t inter_load16(Interconnect *inter, uint32_t addr)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_RAM, phys, &off))
        return ram_load16(&inter->ram, off);

    if (memrange_contains(MAP_BIOS, phys, &off))
        return bios_load16(&inter->bios, off);

    if (memrange_contains(MAP_GPU, phys, &off)) {
        uint32_t stat = gpu_gpustat(&inter->gpu);
        return (uint16_t)(stat >> (16u * ((off / 2u) & 1u)));
    }

    if (memrange_contains(MAP_SPU, phys, &off))
        return 0;

    if (memrange_contains(MAP_IRQ_CONTROL, phys, &off))
        return 0;

    if (memrange_contains(MAP_EXPANSION2, phys, &off))
        return 0xffffu;

    if (memrange_contains(MAP_TIMERS, phys, &off))
        return 0;

    printf("[BUS] Unhandled load16 @ 0x%08x\n", addr);
    return 0;
}

uint8_t inter_load8(Interconnect *inter, uint32_t addr)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_RAM, phys, &off))
        return ram_load8(&inter->ram, off);

    if (memrange_contains(MAP_BIOS, phys, &off))
        return bios_load8(&inter->bios, off);

    if (memrange_contains(MAP_EXPANSION1, phys, &off))
        return 0xffu;

    if (memrange_contains(MAP_CDROM, phys, &off))
        return 0;

    printf("[BUS] Unhandled load8 @ 0x%08x\n", addr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* STORES                                                              */
/* ------------------------------------------------------------------ */

void inter_store32(Interconnect *inter, uint32_t addr, uint32_t val)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_RAM, phys, &off)) {
        ram_store32(&inter->ram, off, val);
        return;
    }

    if (memrange_contains(MAP_GPU, phys, &off)) {
        switch (off) {
        case 0: gpu_gp0_write(&inter->gpu, val); break;
        case 4: gpu_gp1_write(&inter->gpu, val); break;
        default: break;
        }
        return;
    }

    if (memrange_contains(MAP_DMA, phys, &off)) {
        dma_write(inter, off, val);
        return;
    }

    if (memrange_contains(MAP_IRQ_CONTROL, phys, &off)) return;
    if (memrange_contains(MAP_TIMERS,      phys, &off)) return;
    if (memrange_contains(MAP_MEM_CTRL,    phys, &off)) return;
    if (memrange_contains(MAP_RAM_SIZE,    phys, &off)) return;
    if (memrange_contains(MAP_CACHE_CTRL,  phys, &off)) return;
    if (memrange_contains(MAP_SPU,         phys, &off)) return;

    printf("[BUS] Unhandled store32 0x%08x @ 0x%08x\n", val, addr);
}

void inter_store16(Interconnect *inter, uint32_t addr, uint16_t val)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_RAM, phys, &off)) {
        ram_store16(&inter->ram, off, val);
        return;
    }

    if (memrange_contains(MAP_SPU,          phys, &off)) return;
    if (memrange_contains(MAP_IRQ_CONTROL,  phys, &off)) return;
    if (memrange_contains(MAP_TIMERS,       phys, &off)) return;
    if (memrange_contains(MAP_EXPANSION2,   phys, &off)) return;

    printf("[BUS] Unhandled store16 0x%04x @ 0x%08x\n", val, addr);
}

void inter_store8(Interconnect *inter, uint32_t addr, uint8_t val)
{
    uint32_t phys = mask_region(addr);
    uint32_t off;

    if (memrange_contains(MAP_RAM, phys, &off)) {
        ram_store8(&inter->ram, off, val);
        return;
    }

    if (memrange_contains(MAP_EXPANSION2, phys, &off)) {
        if (off == 0x41u) putchar((char)val);  /* debug serial do BIOS */
        return;
    }

    if (memrange_contains(MAP_CDROM, phys, &off)) return;

    printf("[BUS] Unhandled store8 0x%02x @ 0x%08x\n", val, addr);
}
