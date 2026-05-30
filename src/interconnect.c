#include "interconnect.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- Memory map ---- */
typedef struct { uint32_t start, length; } Range;

static int range_contains(Range r, uint32_t addr, uint32_t *offset) {
    if (addr >= r.start && addr < r.start + r.length) {
        *offset = addr - r.start;
        return 1;
    }
    return 0;
}

static const Range MAP_RAM         = { 0x00000000, 2 * 1024 * 1024 };
static const Range MAP_BIOS        = { 0x1FC00000, 512 * 1024 };
static const Range MAP_MEM_CONTROL = { 0x1F801000, 36 };
static const Range MAP_RAM_SIZE    = { 0x1F801060, 4 };
static const Range MAP_CACHE_CTRL  = { 0xFFFE0130, 4 };
static const Range MAP_SPU         = { 0x1F801C00, 640 };
static const Range MAP_EXPANSION2  = { 0x1F802000, 66 };
static const Range MAP_EXPANSION1  = { 0x1F000000, 512 * 1024 };
static const Range MAP_IRQ_CONTROL = { 0x1F801070, 8 };
static const Range MAP_TIMERS      = { 0x1F801100, 16 * 3 };
static const Range MAP_DMA         = { 0x1F801080, 0x80 };
static const Range MAP_GPU         = { 0x1F801810, 8 };

static const uint32_t REGION_MASK[8] = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0x7FFFFFFF,
    0x1FFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF,
};

static uint32_t mask_region(uint32_t addr) {
    return addr & REGION_MASK[addr >> 29];
}

/* ---- DMA register helpers ---- */
static uint32_t dma_reg_read(Interconnect *inter, uint32_t offset) {
    uint32_t major = (offset & 0x70) >> 4;
    uint32_t minor = offset & 0x0F;

    if (major <= 6) {
        const Channel *ch = dma_channel_const(&inter->dma, port_from_index(major));
        switch (minor) {
            case 0: return channel_base(ch);
            case 4: return channel_block_control(ch);
            case 8: return channel_control(ch);
            default:
                fprintf(stderr, "Unhandled DMA read at %08X\n", offset);
                exit(1);
        }
    } else if (major == 7) {
        switch (minor) {
            case 0: return dma_control(&inter->dma);
            case 4: return dma_interrupt(&inter->dma);
            default:
                fprintf(stderr, "Unhandled DMA read at %08X\n", offset);
                exit(1);
        }
    }
    fprintf(stderr, "Unhandled DMA read at %08X\n", offset);
    exit(1);
}

static void do_dma(Interconnect *inter, Port port);

static void dma_reg_write(Interconnect *inter, uint32_t offset, uint32_t val) {
    uint32_t major = (offset & 0x70) >> 4;
    uint32_t minor = offset & 0x0F;
    Port active_port = (Port)-1;
    int has_active = 0;

    if (major <= 6) {
        Port port = port_from_index(major);
        Channel *ch = dma_channel(&inter->dma, port);
        switch (minor) {
            case 0: channel_set_base(ch, val);          break;
            case 4: channel_set_block_control(ch, val); break;
            case 8: channel_set_control(ch, val);       break;
            default:
                fprintf(stderr, "Unhandled DMA write %08X = %08X\n", offset, val);
                exit(1);
        }
        if (channel_active(ch)) {
            active_port = port;
            has_active = 1;
        }
    } else if (major == 7) {
        switch (minor) {
            case 0: dma_set_control(&inter->dma, val);    break;
            case 4: dma_set_interrupt(&inter->dma, val);  break;
            default:
                fprintf(stderr, "Unhandled DMA write %08X = %08X\n", offset, val);
                exit(1);
        }
    } else {
        fprintf(stderr, "Unhandled DMA write %08X = %08X\n", offset, val);
        exit(1);
    }

    if (has_active) do_dma(inter, active_port);
}

static void do_dma_block(Interconnect *inter, Port port) {
    Channel *ch = dma_channel(&inter->dma, port);
    Step increment = channel_step(ch);
    uint32_t addr = channel_base(ch);
    uint32_t remsz;
    if (!channel_transfer_size(ch, &remsz)) {
        fprintf(stderr, "Couldn't figure out DMA block transfer size\n");
        exit(1);
    }
    while (remsz > 0) {
        uint32_t cur_addr = addr & 0x001FFFFC;
        switch (channel_direction(ch)) {
            case DIRECTION_FROM_RAM: {
                uint32_t src = ram_load32(&inter->ram, cur_addr);
                switch (port) {
                    case PORT_GPU: gpu_gp0(&inter->gpu, src); break;
                    default:
                        fprintf(stderr, "Unhandled DMA dest port %d\n", port);
                        exit(1);
                }
                break;
            }
            case DIRECTION_TO_RAM: {
                uint32_t src;
                switch (port) {
                    case PORT_OTC:
                        src = (remsz == 1) ? 0x00FFFFFF
                                           : (addr - 4) & 0x001FFFFF;
                        break;
                    default:
                        fprintf(stderr, "Unhandled DMA src port %d\n", port);
                        exit(1);
                }
                ram_store32(&inter->ram, cur_addr, src);
                break;
            }
        }
        addr = (increment == STEP_INCREMENT) ? addr + 4 : addr - 4;
        remsz--;
    }
    channel_done(ch);
}

static void do_dma_linked_list(Interconnect *inter, Port port) {
    Channel *ch = dma_channel(&inter->dma, port);
    uint32_t addr = channel_base(ch) & 0x001FFFFC;
    if (channel_direction(ch) == DIRECTION_TO_RAM) {
        fprintf(stderr, "Invalid DMA direction for linked list\n");
        exit(1);
    }
    if (port != PORT_GPU) {
        fprintf(stderr, "Linked list DMA on non-GPU port\n");
        exit(1);
    }
    for (;;) {
        uint32_t header = ram_load32(&inter->ram, addr);
        uint32_t remsz  = header >> 24;
        while (remsz > 0) {
            addr = (addr + 4) & 0x001FFFFC;
            gpu_gp0(&inter->gpu, ram_load32(&inter->ram, addr));
            remsz--;
        }
        if (header & 0x00800000) break;
        addr = header & 0x001FFFFC;
    }
    channel_done(ch);
}

static void do_dma(Interconnect *inter, Port port) {
    if (channel_sync(dma_channel(&inter->dma, port)) == SYNC_LINKED_LIST)
        do_dma_linked_list(inter, port);
    else
        do_dma_block(inter, port);
}

/* ---- Init / destroy ---- */
int interconnect_init(Interconnect *inter, const char *bios_path, SDL_Window *window, bool headless) {
    if (bios_init(&inter->bios, bios_path) != 0) return -1;
    ram_init(&inter->ram);
    dma_init(&inter->dma);
    gpu_init(&inter->gpu, window);
    if (!headless && spu_init(&inter->spu) != 0) return -1;
    irq_init(&inter->irq);
    scheduler_init(&inter->scheduler);
    return 0;
}

void interconnect_destroy(Interconnect *inter) {
    gpu_destroy(&inter->gpu);
    spu_destroy(&inter->spu);
}

/* ---- Load32 ---- */
uint32_t interconnect_load32(Interconnect *inter, uint32_t addr) {
    if (addr % 4 != 0) { fprintf(stderr, "Unaligned load32: %08X\n", addr); exit(1); }
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_BIOS, abs, &off)) return bios_load32(&inter->bios, off);
    if (range_contains(MAP_RAM,  abs, &off)) return ram_load32(&inter->ram, off);
    if (range_contains(MAP_IRQ_CONTROL, abs, &off)) return irq_load32(&inter->irq, off);
    if (range_contains(MAP_DMA, abs, &off)) return dma_reg_read(inter, off);
    if (range_contains(MAP_GPU, abs, &off)) {
        switch (off) {
            case 0: return gpu_read(&inter->gpu);
            case 4: return gpu_status(&inter->gpu);
            default: fprintf(stderr, "GPU read: %08X\n", off); exit(1);
        }
    }
    if (range_contains(MAP_TIMERS, abs, &off)) return 0;

    fprintf(stderr, "Unhandled load32: %08X\n", addr);
    exit(1);
}

/* ---- Load16 ---- */
uint16_t interconnect_load16(Interconnect *inter, uint32_t addr) {
    if (addr % 2 != 0) { fprintf(stderr, "Unaligned load16: %08X\n", addr); exit(1); }
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_SPU, abs, &off)) return spu_load(&inter->spu, abs, off);
    if (range_contains(MAP_RAM, abs, &off)) return ram_load16(&inter->ram, off);
    if (range_contains(MAP_IRQ_CONTROL, abs, &off)) return irq_load16(&inter->irq, off);

    fprintf(stderr, "Unhandled load16: %08X\n", addr);
    exit(1);
}

/* ---- Load8 ---- */
uint8_t interconnect_load8(Interconnect *inter, uint32_t addr) {
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_BIOS, abs, &off)) return bios_load8(&inter->bios, off);
    if (range_contains(MAP_RAM,  abs, &off)) return ram_load8(&inter->ram, off);
    if (range_contains(MAP_EXPANSION1, abs, &off)) return 0xFF;

    fprintf(stderr, "Unhandled load8: %08X\n", addr);
    exit(1);
}

/* ---- Store32 ---- */
void interconnect_store32(Interconnect *inter, uint32_t addr, uint32_t val) {
    if (addr % 4 != 0) { fprintf(stderr, "Unaligned store32: %08X\n", addr); exit(1); }
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_MEM_CONTROL, abs, &off)) {
        switch (off) {
            case 0: if (val != 0x1F000000) { fprintf(stderr, "Bad expansion 1 base: %08X\n", val); exit(1); } break;
            case 4: if (val != 0x1F802000) { fprintf(stderr, "Bad expansion 2 base: %08X\n", val); exit(1); } break;
            default: break;
        }
        return;
    }
    if (range_contains(MAP_RAM_SIZE,   abs, &off)) return;
    if (range_contains(MAP_CACHE_CTRL, abs, &off)) return;
    if (range_contains(MAP_RAM,  abs, &off)) { ram_store32(&inter->ram, off, val); return; }
    if (range_contains(MAP_IRQ_CONTROL, abs, &off)) { irq_store32(&inter->irq, off, val); return; }
    if (range_contains(MAP_DMA, abs, &off)) { dma_reg_write(inter, off, val); return; }
    if (range_contains(MAP_GPU, abs, &off)) {
        switch (off) {
            case 0: gpu_gp0(&inter->gpu, val); return;
            case 4: gpu_gp1(&inter->gpu, val); return;
            default: fprintf(stderr, "GPU write: %08X = %08X\n", off, val); exit(1);
        }
    }
    if (range_contains(MAP_TIMERS, abs, &off)) return;

    fprintf(stderr, "Unhandled store32: %08X = %08X\n", addr, val);
    exit(1);
}

/* ---- Store16 ---- */
void interconnect_store16(Interconnect *inter, uint32_t addr, uint16_t val) {
    if (addr % 2 != 0) { fprintf(stderr, "Unaligned store16: %08X\n", addr); exit(1); }
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_RAM,  abs, &off)) { ram_store16(&inter->ram, off, val); return; }
    if (range_contains(MAP_SPU,  abs, &off)) { spu_store(&inter->spu, abs, off, val); return; }
    if (range_contains(MAP_TIMERS, abs, &off)) return;
    if (range_contains(MAP_IRQ_CONTROL, abs, &off)) { irq_store16(&inter->irq, off, val); return; }

    fprintf(stderr, "Unhandled store16: %08X = %04X\n", addr, val);
    exit(1);
}

/* ---- Store8 ---- */
void interconnect_store8(Interconnect *inter, uint32_t addr, uint8_t val) {
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_EXPANSION2, abs, &off)) return;
    if (range_contains(MAP_RAM,        abs, &off)) { ram_store8(&inter->ram, off, val); return; }

    fprintf(stderr, "Unhandled store8: %08X = %02X\n", addr, val);
    exit(1);
}
