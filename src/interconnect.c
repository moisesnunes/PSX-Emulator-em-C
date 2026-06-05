#include "interconnect.h"
#include "timer.h"
#include "cdrom.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Memory map ---- */
typedef struct
{
    uint32_t start, length;
} Range;

static int range_contains(Range r, uint32_t addr, uint32_t *offset)
{
    if (addr >= r.start && addr < r.start + r.length)
    {
        *offset = addr - r.start;
        return 1;
    }
    return 0;
}

static const Range MAP_RAM = {0x00000000, 8 * 1024 * 1024};
static const Range MAP_BIOS = {0x1FC00000, 512 * 1024};
static const Range MAP_SCRATCHPAD = {0x1F800000, 1024};
static const Range MAP_MEM_CONTROL = {0x1F801000, 36};
static const Range MAP_RAM_SIZE = {0x1F801060, 4};
static const Range MAP_CACHE_CTRL = {0xFFFE0130, 4};
static const Range MAP_SPU = {0x1F801C00, 640};
static const Range MAP_EXPANSION2 = {0x1F802000, 66};
static const Range MAP_EXPANSION1 = {0x1F000000, 512 * 1024};
static const Range MAP_EXPANSION3 = {0x1FA00000, 2 * 1024 * 1024};
static const Range MAP_IRQ_CONTROL = {0x1F801070, 8};
static const Range MAP_TIMERS = {0x1F801100, 16 * 3};
static const Range MAP_DMA = {0x1F801080, 0x80};
static const Range MAP_GPU = {0x1F801810, 8};
static const Range MAP_MDEC = {0x1F801820, 8};
static const Range MAP_CDROM = {0x1F801800, 4};
static const Range MAP_SIO = {0x1F801040, 32};

static const uint32_t REGION_MASK[8] = {
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0x7FFFFFFF,
    0x1FFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
};

static uint32_t mask_region(uint32_t addr)
{
    return addr & REGION_MASK[addr >> 29];
}

static uint32_t scratch_load32(const Interconnect *inter, uint32_t offset)
{
    return (uint32_t)inter->scratchpad[offset] |
           ((uint32_t)inter->scratchpad[offset + 1] << 8) |
           ((uint32_t)inter->scratchpad[offset + 2] << 16) |
           ((uint32_t)inter->scratchpad[offset + 3] << 24);
}

static uint16_t scratch_load16(const Interconnect *inter, uint32_t offset)
{
    return (uint16_t)inter->scratchpad[offset] |
           ((uint16_t)inter->scratchpad[offset + 1] << 8);
}

static void scratch_store32(Interconnect *inter, uint32_t offset, uint32_t val)
{
    inter->scratchpad[offset] = (uint8_t)val;
    inter->scratchpad[offset + 1] = (uint8_t)(val >> 8);
    inter->scratchpad[offset + 2] = (uint8_t)(val >> 16);
    inter->scratchpad[offset + 3] = (uint8_t)(val >> 24);
}

static void scratch_store16(Interconnect *inter, uint32_t offset, uint16_t val)
{
    inter->scratchpad[offset] = (uint8_t)val;
    inter->scratchpad[offset + 1] = (uint8_t)(val >> 8);
}

static uint32_t mem_control_load32(const Interconnect *inter, uint32_t offset)
{
    uint32_t index = offset >> 2;
    if (index < 9)
        return inter->mem_control[index];
    return 0;
}

static uint16_t mem_control_load16(const Interconnect *inter, uint32_t offset)
{
    return (uint16_t)(mem_control_load32(inter, offset & ~3u) >> ((offset & 2u) * 8u));
}

static uint8_t mem_control_load8(const Interconnect *inter, uint32_t offset)
{
    return (uint8_t)(mem_control_load32(inter, offset & ~3u) >> ((offset & 3u) * 8u));
}

static void mem_control_store32(Interconnect *inter, uint32_t offset, uint32_t val)
{
    uint32_t index = offset >> 2;
    if (index < 9)
        inter->mem_control[index] = val;
}

static void mem_control_store16(Interconnect *inter, uint32_t offset, uint16_t val)
{
    uint32_t aligned = offset & ~3u;
    uint32_t shift = (offset & 2u) * 8u;
    uint32_t word = mem_control_load32(inter, aligned);
    word = (word & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    mem_control_store32(inter, aligned, word);
}

static void mem_control_store8(Interconnect *inter, uint32_t offset, uint8_t val)
{
    uint32_t aligned = offset & ~3u;
    uint32_t shift = (offset & 3u) * 8u;
    uint32_t word = mem_control_load32(inter, aligned);
    word = (word & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    mem_control_store32(inter, aligned, word);
}

/* ---- DMA register helpers ---- */
static uint32_t dma_reg_read(Interconnect *inter, uint32_t offset)
{
    uint32_t major = (offset & 0x70) >> 4;
    uint32_t minor = offset & 0x0F;

    if (major <= 6)
    {
        const Channel *ch = dma_channel_const(&inter->dma, port_from_index(major));
        switch (minor)
        {
        case 0:
            return channel_base(ch);
        case 4:
            return channel_block_control(ch);
        case 8:
            return channel_control(ch);
        default:
            fprintf(stderr, "Unhandled DMA read at %08X\n", offset);
            exit(1);
        }
    }
    else if (major == 7)
    {
        switch (minor)
        {
        case 0:
            return dma_control(&inter->dma);
        case 4:
            return dma_interrupt(&inter->dma);
        default:
            fprintf(stderr, "Unhandled DMA read at %08X\n", offset);
            exit(1);
        }
    }
    fprintf(stderr, "Unhandled DMA read at %08X\n", offset);
    exit(1);
}

static uint16_t dma_reg_read16(Interconnect *inter, uint32_t offset)
{
    uint32_t aligned = offset & ~3u;
    uint32_t word = dma_reg_read(inter, aligned);
    return (uint16_t)(word >> ((offset & 2u) * 8u));
}

static uint8_t dma_reg_read8(Interconnect *inter, uint32_t offset)
{
    uint32_t word = dma_reg_read(inter, offset & ~3u);
    return (uint8_t)(word >> ((offset & 3u) * 8u));
}

static void do_dma(Interconnect *inter, Port port);

static void dma_update_irq(Interconnect *inter)
{
    bool pending = dma_irq_pending(&inter->dma);
    if (pending && !inter->dma.irq_line)
        inter->dma.irq_pending = true;
    inter->dma.irq_line = pending;
}

static void dma_finish(Interconnect *inter, Channel *ch, Port port)
{
    channel_done(ch);
    dma_mark_channel_done(&inter->dma, port);
    dma_update_irq(inter);
    LOG(LOG_DMA, "DMA done port=%d", port);
}

static void dma_reg_write(Interconnect *inter, uint32_t offset, uint32_t val)
{
    uint32_t major = (offset & 0x70) >> 4;
    uint32_t minor = offset & 0x0F;
    Port active_port = (Port)-1;
    int has_active = 0;

    if (major <= 6)
    {
        Port port = port_from_index(major);
        Channel *ch = dma_channel(&inter->dma, port);
        switch (minor)
        {
        case 0:
            channel_set_base(ch, val);
            break;
        case 4:
            channel_set_block_control(ch, val);
            break;
        case 8:
            channel_set_control(ch, val);
            break;
        default:
            fprintf(stderr, "Unhandled DMA write %08X = %08X\n", offset, val);
            exit(1);
        }
        if (channel_active(ch))
        {
            active_port = port;
            has_active = 1;
        }
    }
    else if (major == 7)
    {
        switch (minor)
        {
        case 0:
            dma_set_control(&inter->dma, val);
            break;
        case 4:
            dma_set_interrupt(&inter->dma, val);
            dma_update_irq(inter);
            break;
        default:
            fprintf(stderr, "Unhandled DMA write %08X = %08X\n", offset, val);
            exit(1);
        }
    }
    else
    {
        fprintf(stderr, "Unhandled DMA write %08X = %08X\n", offset, val);
        exit(1);
    }

    if (has_active)
        do_dma(inter, active_port);
}

static void dma_reg_write16(Interconnect *inter, uint32_t offset, uint16_t val)
{
    uint32_t aligned = offset & ~3u;
    uint32_t shift = (offset & 2u) * 8u;
    uint32_t word = dma_reg_read(inter, aligned);
    word = (word & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    dma_reg_write(inter, aligned, word);
}

static void dma_reg_write8(Interconnect *inter, uint32_t offset, uint8_t val)
{
    uint32_t aligned = offset & ~3u;
    uint32_t shift = (offset & 3u) * 8u;
    uint32_t word = dma_reg_read(inter, aligned);
    word = (word & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    dma_reg_write(inter, aligned, word);
}

static uint8_t irq_load8(const Irq *irq, uint32_t offset)
{
    uint32_t word = irq_load32(irq, offset & ~3u);
    return (uint8_t)(word >> ((offset & 3u) * 8u));
}

static void irq_store8(Irq *irq, uint32_t offset, uint8_t val)
{
    uint32_t aligned = offset & ~3u;
    uint32_t shift = (offset & 3u) * 8u;
    uint32_t word = irq_load32(irq, aligned);
    word = (word & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    irq_store32(irq, aligned, word);
}

static uint8_t timers_load8(Timers *timers, uint32_t offset)
{
    uint16_t half = timers_load16(timers, offset & ~1u);
    return (uint8_t)(half >> ((offset & 1u) * 8u));
}

static void timers_store8(Timers *timers, uint32_t offset, uint8_t val, Scheduler *sched)
{
    uint32_t aligned = offset & ~1u;
    uint32_t shift = (offset & 1u) * 8u;
    uint16_t half = timers_load16(timers, aligned);
    half = (uint16_t)((half & ~(0xFFu << shift)) | ((uint16_t)val << shift));
    timers_store16(timers, aligned, half, sched);
}

static uint16_t cdrom_load16(Cdrom *cdrom, uint32_t offset)
{
    return (uint16_t)cdrom_load8(cdrom, offset) |
           ((uint16_t)cdrom_load8(cdrom, offset + 1) << 8);
}

static uint32_t cdrom_load32(Cdrom *cdrom, uint32_t offset)
{
    return (uint32_t)cdrom_load8(cdrom, offset) |
           ((uint32_t)cdrom_load8(cdrom, offset + 1) << 8) |
           ((uint32_t)cdrom_load8(cdrom, offset + 2) << 16) |
           ((uint32_t)cdrom_load8(cdrom, offset + 3) << 24);
}

static void cdrom_store16(Cdrom *cdrom, uint32_t offset, uint16_t val,
                          Irq *irq, Scheduler *sched)
{
    cdrom_store8(cdrom, offset, (uint8_t)val, irq, sched);
    cdrom_store8(cdrom, offset + 1, (uint8_t)(val >> 8), irq, sched);
}

static void cdrom_store32(Cdrom *cdrom, uint32_t offset, uint32_t val,
                          Irq *irq, Scheduler *sched)
{
    cdrom_store8(cdrom, offset, (uint8_t)val, irq, sched);
    cdrom_store8(cdrom, offset + 1, (uint8_t)(val >> 8), irq, sched);
    cdrom_store8(cdrom, offset + 2, (uint8_t)(val >> 16), irq, sched);
    cdrom_store8(cdrom, offset + 3, (uint8_t)(val >> 24), irq, sched);
}

static uint32_t gpu_reg_read(Interconnect *inter, uint32_t offset)
{
    switch (offset)
    {
    case 0:
        return gpu_read(&inter->gpu);
    case 4:
        return gpu_status(&inter->gpu);
    default:
        fprintf(stderr, "GPU read: %08X\n", offset);
        exit(1);
    }
}

static void gpu_reg_write(Interconnect *inter, uint32_t offset, uint32_t val)
{
    switch (offset)
    {
    case 0:
        gpu_gp0(&inter->gpu, val);
        return;
    case 4:
        gpu_gp1(&inter->gpu, val);
        return;
    default:
        fprintf(stderr, "GPU write: %08X = %08X\n", offset, val);
        exit(1);
    }
}

static uint16_t gpu_reg_read16(Interconnect *inter, uint32_t offset)
{
    uint32_t word = gpu_reg_read(inter, offset & ~3u);
    return (uint16_t)(word >> ((offset & 2u) * 8u));
}

static uint8_t gpu_reg_read8(Interconnect *inter, uint32_t offset)
{
    uint32_t word = gpu_reg_read(inter, offset & ~3u);
    return (uint8_t)(word >> ((offset & 3u) * 8u));
}

static void gpu_reg_write16(Interconnect *inter, uint32_t offset, uint16_t val)
{
    gpu_reg_write(inter, offset & ~3u, (uint32_t)val << ((offset & 2u) * 8u));
}

static void gpu_reg_write8(Interconnect *inter, uint32_t offset, uint8_t val)
{
    gpu_reg_write(inter, offset & ~3u, (uint32_t)val << ((offset & 3u) * 8u));
}

static uint16_t mdec_reg_read16(Interconnect *inter, uint32_t offset)
{
    uint32_t word = mdec_load32(&inter->mdec, offset & ~3u);
    return (uint16_t)(word >> ((offset & 2u) * 8u));
}

static uint8_t mdec_reg_read8(Interconnect *inter, uint32_t offset)
{
    uint32_t word = mdec_load32(&inter->mdec, offset & ~3u);
    return (uint8_t)(word >> ((offset & 3u) * 8u));
}

static void mdec_reg_write16(Interconnect *inter, uint32_t offset, uint16_t val)
{
    mdec_store32(&inter->mdec, offset & ~3u, (uint32_t)val << ((offset & 2u) * 8u));
}

static void mdec_reg_write8(Interconnect *inter, uint32_t offset, uint8_t val)
{
    mdec_store32(&inter->mdec, offset & ~3u, (uint32_t)val << ((offset & 3u) * 8u));
}

static uint8_t spu_load8(Interconnect *inter, uint32_t abs, uint32_t offset)
{
    uint16_t half = spu_load(&inter->spu, abs & ~1u, offset & ~1u);
    return (uint8_t)(half >> ((offset & 1u) * 8u));
}

static uint32_t spu_load32(Interconnect *inter, uint32_t abs, uint32_t offset)
{
    return (uint32_t)spu_load(&inter->spu, abs, offset) |
           ((uint32_t)spu_load(&inter->spu, abs + 2, offset + 2) << 16);
}

static void spu_store8(Interconnect *inter, uint32_t abs, uint32_t offset, uint8_t val)
{
    uint32_t aligned_abs = abs & ~1u;
    uint32_t aligned_off = offset & ~1u;
    uint32_t shift = (offset & 1u) * 8u;
    uint16_t half = spu_load(&inter->spu, aligned_abs, aligned_off);
    half = (uint16_t)((half & ~(0xFFu << shift)) | ((uint16_t)val << shift));
    spu_store(&inter->spu, aligned_abs, aligned_off, half);
}

static void spu_store32(Interconnect *inter, uint32_t abs, uint32_t offset, uint32_t val)
{
    spu_store(&inter->spu, abs, offset, (uint16_t)val);
    spu_store(&inter->spu, abs + 2, offset + 2, (uint16_t)(val >> 16));
}

static void dma_mdec_out_reorder_color(Interconnect *inter, uint32_t addr, uint32_t remsz)
{
    uint32_t words_per_macroblock = inter->mdec.output_depth == 3 ? 128u : 192u;
    uint32_t bytes_per_pixel = inter->mdec.output_depth == 3 ? 2u : 3u;
    uint8_t raw[192u * 4u];
    uint8_t ordered[192u * 4u];
    uint32_t macroblock = 0;

    while (remsz >= words_per_macroblock)
    {
        memset(raw, 0, sizeof(raw));
        memset(ordered, 0, sizeof(ordered));

        for (uint32_t i = 0; i < words_per_macroblock; i++)
        {
            uint32_t word = mdec_dma_read(&inter->mdec);
            raw[i * 4u + 0u] = (uint8_t)word;
            raw[i * 4u + 1u] = (uint8_t)(word >> 8);
            raw[i * 4u + 2u] = (uint8_t)(word >> 16);
            raw[i * 4u + 3u] = (uint8_t)(word >> 24);
        }

        for (uint32_t block = 0; block < 4; block++)
        {
            uint32_t xx = (block & 1u) ? 8u : 0u;
            uint32_t yy = (block & 2u) ? 8u : 0u;
            uint32_t block_base = block * 8u * 8u * bytes_per_pixel;
            for (uint32_t y = 0; y < 8; y++)
            {
                for (uint32_t x = 0; x < 8; x++)
                {
                    uint32_t src = block_base + (y * 8u + x) * bytes_per_pixel;
                    uint32_t dst = ((yy + y) * 16u + (xx + x)) * bytes_per_pixel;
                    for (uint32_t b = 0; b < bytes_per_pixel; b++)
                        ordered[dst + b] = raw[src + b];
                }
            }
        }

        uint32_t dst_addr = (addr + macroblock * words_per_macroblock * 4u) & 0x001FFFFC;
        for (uint32_t i = 0; i < words_per_macroblock; i++)
        {
            uint32_t word = (uint32_t)ordered[i * 4u + 0u] |
                            ((uint32_t)ordered[i * 4u + 1u] << 8) |
                            ((uint32_t)ordered[i * 4u + 2u] << 16) |
                            ((uint32_t)ordered[i * 4u + 3u] << 24);
            ram_store32(&inter->ram, (dst_addr + i * 4u) & 0x001FFFFC, word);
        }

        macroblock++;
        remsz -= words_per_macroblock;
    }
}

static void do_dma_block(Interconnect *inter, Port port)
{
    Channel *ch = dma_channel(&inter->dma, port);
    Step increment = channel_step(ch);
    uint32_t addr = channel_base(ch);
    uint32_t remsz;
    if (!channel_transfer_size(ch, &remsz))
    {
        if (port == PORT_OTC)
        {
            dma_finish(inter, ch, port);
            return;
        }
        fprintf(stderr, "Couldn't figure out DMA block transfer size\n");
        exit(1);
    }
    if (remsz == 0 && port == PORT_MDEC_OUT)
        remsz = channel_block_count(ch) ? channel_block_count(ch) : channel_block_size(ch);
    LOG(LOG_DMA, "DMA block port=%d dir=%d addr=0x%06X bs=%u bc=%u words=%u", port,
        channel_direction(ch), addr, channel_block_size(ch), channel_block_count(ch), remsz);
    if (port == PORT_MDEC_OUT && channel_direction(ch) == DIRECTION_TO_RAM &&
        increment == STEP_INCREMENT && (inter->mdec.output_depth == 2 || inter->mdec.output_depth == 3))
    {
        dma_mdec_out_reorder_color(inter, addr, remsz);
        dma_finish(inter, ch, port);
        return;
    }
    while (remsz > 0)
    {
        uint32_t cur_addr = addr & 0x001FFFFC;
        switch (channel_direction(ch))
        {
        case DIRECTION_FROM_RAM:
        {
            uint32_t src = ram_load32(&inter->ram, cur_addr);
            switch (port)
            {
            case PORT_MDEC_IN:
                mdec_dma_write(&inter->mdec, src);
                break;
            case PORT_GPU:
                gpu_gp0(&inter->gpu, src);
                break;
            case PORT_SPU:
                spu_dma_write(&inter->spu, src);
                break;
            case PORT_OTC:
                break;
            default:
                fprintf(stderr, "Unhandled DMA dest port %d\n", port);
                exit(1);
            }
            break;
        }
        case DIRECTION_TO_RAM:
        {
            uint32_t src;
            switch (port)
            {
            case PORT_MDEC_OUT:
                src = mdec_dma_read(&inter->mdec);
                break;
            case PORT_OTC:
                src = (remsz == 1) ? 0x00FFFFFF
                                   : (addr - 4) & 0x001FFFFF;
                break;
            case PORT_CDROM:
                src = cdrom_dma_read(&inter->cdrom);
                break;
            case PORT_GPU:
                src = gpu_read(&inter->gpu);
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
    dma_finish(inter, ch, port);
}

static void do_dma_linked_list(Interconnect *inter, Port port)
{
    Channel *ch = dma_channel(&inter->dma, port);
    uint32_t addr = channel_base(ch) & 0x001FFFFC;
    if (channel_direction(ch) == DIRECTION_TO_RAM)
    {
        fprintf(stderr, "Invalid DMA direction for linked list\n");
        exit(1);
    }
    if (port != PORT_GPU)
    {
        fprintf(stderr, "Linked list DMA on non-GPU port\n");
        exit(1);
    }
    for (;;)
    {
        uint32_t header = ram_load32(&inter->ram, addr);
        uint32_t remsz = header >> 24;
        while (remsz > 0)
        {
            addr = (addr + 4) & 0x001FFFFC;
            gpu_gp0(&inter->gpu, ram_load32(&inter->ram, addr));
            remsz--;
        }
        if (header & 0x00800000)
            break;
        addr = header & 0x001FFFFC;
    }
    dma_finish(inter, ch, port);
}

static void do_dma(Interconnect *inter, Port port)
{
    if (port != PORT_OTC && channel_sync(dma_channel(&inter->dma, port)) == SYNC_LINKED_LIST)
        do_dma_linked_list(inter, port);
    else
        do_dma_block(inter, port);
}

/* ---- Init / destroy ---- */
int interconnect_init(Interconnect *inter, const char *bios_path, SDL_Window *window,
                      bool headless, const char *disc_path)
{
    memset(inter, 0, sizeof(*inter));
    if (bios_init(&inter->bios, bios_path) != 0)
        return -1;
    inter->mem_control[0] = 0x1F000000;
    inter->mem_control[1] = 0x1F802000;
    ram_init(&inter->ram);
    dma_init(&inter->dma);
    gpu_init(&inter->gpu, window);
    mdec_init(&inter->mdec);
    if (spu_init(&inter->spu, !headless) != 0)
        return -1;
    irq_init(&inter->irq);
    scheduler_init(&inter->scheduler);
    timers_init(&inter->timers);
    inter->disc_loaded = false;
    if (disc_path)
    {
        if (disc_open(&inter->disc, disc_path) == 0)
            inter->disc_loaded = true;
        else
            fprintf(stderr, "Warning: could not open disc '%s'\n", disc_path);
    }
    cdrom_init(&inter->cdrom, inter->disc_loaded ? &inter->disc : NULL);
    cdrom_set_spu(&inter->cdrom, &inter->spu);
    sio_init(&inter->sio);
    return 0;
}

void interconnect_destroy(Interconnect *inter)
{
    gpu_destroy(&inter->gpu);
    spu_destroy(&inter->spu);
    if (inter->disc_loaded)
        disc_close(&inter->disc);
}

/* ---- Load32 ---- */
uint32_t interconnect_load32(Interconnect *inter, uint32_t addr)
{
    if (addr % 4 != 0)
    {
        fprintf(stderr, "Unaligned load32: %08X\n", addr);
        exit(1);
    }
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_MEM_CONTROL, abs, &off))
        return mem_control_load32(inter, off);
    if (range_contains(MAP_BIOS, abs, &off))
        return bios_load32(&inter->bios, off);
    if (range_contains(MAP_SPU, abs, &off))
        return spu_load32(inter, abs, off);
    if (range_contains(MAP_SCRATCHPAD, abs, &off))
        return scratch_load32(inter, off);
    if (range_contains(MAP_RAM, abs, &off))
        return ram_load32(&inter->ram, off & 0x001FFFFF);
    if (range_contains(MAP_IRQ_CONTROL, abs, &off))
        return irq_load32(&inter->irq, off);
    if (range_contains(MAP_RAM_SIZE, abs, &off))
        return 0;
    if (range_contains(MAP_CACHE_CTRL, abs, &off))
        return 0;
    if (range_contains(MAP_DMA, abs, &off))
        return dma_reg_read(inter, off);
    if (range_contains(MAP_CDROM, abs, &off))
        return cdrom_load32(&inter->cdrom, off);
    if (range_contains(MAP_SIO, abs, &off))
        return (uint32_t)sio_load16(&inter->sio, off) |
               ((uint32_t)sio_load16(&inter->sio, off + 2) << 16);
    if (range_contains(MAP_MDEC, abs, &off))
        return mdec_load32(&inter->mdec, off);
    if (range_contains(MAP_GPU, abs, &off))
        return gpu_reg_read(inter, off);
    if (range_contains(MAP_TIMERS, abs, &off))
        return timers_load32(&inter->timers, off);
    if (range_contains(MAP_EXPANSION1, abs, &off))
        return 0xFFFFFFFF;
    if (range_contains(MAP_EXPANSION2, abs, &off))
        return 0xFFFFFFFF;
    if (range_contains(MAP_EXPANSION3, abs, &off))
        return 0xFFFFFFFF;

    fprintf(stderr, "Unhandled load32: %08X\n", addr);
    exit(1);
}

/* ---- Load16 ---- */
uint16_t interconnect_load16(Interconnect *inter, uint32_t addr)
{
    if (addr % 2 != 0)
    {
        fprintf(stderr, "Unaligned load16: %08X\n", addr);
        exit(1);
    }
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_MEM_CONTROL, abs, &off))
        return mem_control_load16(inter, off);
    if (range_contains(MAP_SPU, abs, &off))
        return spu_load(&inter->spu, abs, off);
    if (range_contains(MAP_BIOS, abs, &off))
        return bios_load16(&inter->bios, off);
    if (range_contains(MAP_SCRATCHPAD, abs, &off))
        return scratch_load16(inter, off);
    if (range_contains(MAP_RAM, abs, &off))
        return ram_load16(&inter->ram, off & 0x001FFFFF);
    if (range_contains(MAP_IRQ_CONTROL, abs, &off))
        return irq_load16(&inter->irq, off);
    if (range_contains(MAP_RAM_SIZE, abs, &off))
        return 0;
    if (range_contains(MAP_CACHE_CTRL, abs, &off))
        return 0;
    if (range_contains(MAP_DMA, abs, &off))
        return dma_reg_read16(inter, off);
    if (range_contains(MAP_CDROM, abs, &off))
        return cdrom_load16(&inter->cdrom, off);
    if (range_contains(MAP_GPU, abs, &off))
        return gpu_reg_read16(inter, off);
    if (range_contains(MAP_MDEC, abs, &off))
        return mdec_reg_read16(inter, off);
    if (range_contains(MAP_TIMERS, abs, &off))
        return timers_load16(&inter->timers, off);
    if (range_contains(MAP_SIO, abs, &off))
        return sio_load16(&inter->sio, off);
    if (range_contains(MAP_EXPANSION1, abs, &off))
        return 0xFFFF;
    if (range_contains(MAP_EXPANSION2, abs, &off))
        return 0xFFFF;
    if (range_contains(MAP_EXPANSION3, abs, &off))
        return 0xFFFF;

    fprintf(stderr, "Unhandled load16: %08X\n", addr);
    exit(1);
}

/* ---- Load8 ---- */
uint8_t interconnect_load8(Interconnect *inter, uint32_t addr)
{
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_MEM_CONTROL, abs, &off))
        return mem_control_load8(inter, off);
    if (range_contains(MAP_BIOS, abs, &off))
        return bios_load8(&inter->bios, off);
    if (range_contains(MAP_SCRATCHPAD, abs, &off))
        return inter->scratchpad[off];
    if (range_contains(MAP_RAM, abs, &off))
        return ram_load8(&inter->ram, off & 0x001FFFFF);
    if (range_contains(MAP_IRQ_CONTROL, abs, &off))
        return irq_load8(&inter->irq, off);
    if (range_contains(MAP_SPU, abs, &off))
        return spu_load8(inter, abs, off);
    if (range_contains(MAP_TIMERS, abs, &off))
        return timers_load8(&inter->timers, off);
    if (range_contains(MAP_GPU, abs, &off))
        return gpu_reg_read8(inter, off);
    if (range_contains(MAP_MDEC, abs, &off))
        return mdec_reg_read8(inter, off);
    if (range_contains(MAP_CDROM, abs, &off))
        return cdrom_load8(&inter->cdrom, off);
    if (range_contains(MAP_RAM_SIZE, abs, &off))
        return 0;
    if (range_contains(MAP_CACHE_CTRL, abs, &off))
        return 0;
    if (range_contains(MAP_DMA, abs, &off))
        return dma_reg_read8(inter, off);
    if (range_contains(MAP_SIO, abs, &off))
        return sio_load8(&inter->sio, off);
    if (range_contains(MAP_EXPANSION1, abs, &off))
        return 0xFF;
    if (range_contains(MAP_EXPANSION2, abs, &off))
        return 0xFF;
    if (range_contains(MAP_EXPANSION3, abs, &off))
        return 0xFF;

    fprintf(stderr, "Unhandled load8: %08X\n", addr);
    exit(1);
}

/* ---- Store32 ---- */
void interconnect_store32(Interconnect *inter, uint32_t addr, uint32_t val)
{
    if (addr % 4 != 0)
    {
        fprintf(stderr, "Unaligned store32: %08X\n", addr);
        exit(1);
    }
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_MEM_CONTROL, abs, &off))
    {
        switch (off)
        {
        case 0:
            if (val != 0x1F000000)
            {
                fprintf(stderr, "Bad expansion 1 base: %08X\n", val);
                exit(1);
            }
            break;
        case 4:
            if (val != 0x1F802000)
            {
                fprintf(stderr, "Bad expansion 2 base: %08X\n", val);
                exit(1);
            }
            break;
        default:
            break;
        }
        mem_control_store32(inter, off, val);
        return;
    }
    if (range_contains(MAP_RAM_SIZE, abs, &off))
        return;
    if (range_contains(MAP_CACHE_CTRL, abs, &off))
        return;
    if (range_contains(MAP_RAM, abs, &off))
    {
        ram_store32(&inter->ram, off & 0x001FFFFF, val);
        return;
    }
    if (range_contains(MAP_SCRATCHPAD, abs, &off))
    {
        scratch_store32(inter, off, val);
        return;
    }
    if (range_contains(MAP_IRQ_CONTROL, abs, &off))
    {
        irq_store32(&inter->irq, off, val);
        return;
    }
    if (range_contains(MAP_DMA, abs, &off))
    {
        dma_reg_write(inter, off, val);
        return;
    }
    if (range_contains(MAP_MDEC, abs, &off))
    {
        mdec_store32(&inter->mdec, off, val);
        return;
    }
    if (range_contains(MAP_SPU, abs, &off))
    {
        spu_store32(inter, abs, off, val);
        return;
    }
    if (range_contains(MAP_CDROM, abs, &off))
    {
        cdrom_store32(&inter->cdrom, off, val, &inter->irq, &inter->scheduler);
        return;
    }
    if (range_contains(MAP_SIO, abs, &off))
    {
        sio_store16(&inter->sio, off, (uint16_t)val, &inter->irq);
        sio_store16(&inter->sio, off + 2, (uint16_t)(val >> 16), &inter->irq);
        return;
    }
    if (range_contains(MAP_GPU, abs, &off))
    {
        gpu_reg_write(inter, off, val);
        return;
    }
    if (range_contains(MAP_TIMERS, abs, &off))
    {
        timers_store32(&inter->timers, off, val, &inter->scheduler);
        return;
    }

    fprintf(stderr, "Unhandled store32: %08X = %08X\n", addr, val);
    exit(1);
}

/* ---- Store16 ---- */
void interconnect_store16(Interconnect *inter, uint32_t addr, uint16_t val)
{
    if (addr % 2 != 0)
    {
        fprintf(stderr, "Unaligned store16: %08X\n", addr);
        exit(1);
    }
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_MEM_CONTROL, abs, &off))
    {
        mem_control_store16(inter, off, val);
        return;
    }
    if (range_contains(MAP_RAM, abs, &off))
    {
        ram_store16(&inter->ram, off & 0x001FFFFF, val);
        return;
    }
    if (range_contains(MAP_SCRATCHPAD, abs, &off))
    {
        scratch_store16(inter, off, val);
        return;
    }
    if (range_contains(MAP_SPU, abs, &off))
    {
        spu_store(&inter->spu, abs, off, val);
        return;
    }
    if (range_contains(MAP_RAM_SIZE, abs, &off))
        return;
    if (range_contains(MAP_CACHE_CTRL, abs, &off))
        return;
    if (range_contains(MAP_TIMERS, abs, &off))
    {
        timers_store16(&inter->timers, off, val, &inter->scheduler);
        return;
    }
    if (range_contains(MAP_IRQ_CONTROL, abs, &off))
    {
        irq_store16(&inter->irq, off, val);
        return;
    }
    if (range_contains(MAP_DMA, abs, &off))
    {
        dma_reg_write16(inter, off, val);
        return;
    }
    if (range_contains(MAP_SIO, abs, &off))
    {
        sio_store16(&inter->sio, off, val, &inter->irq);
        return;
    }
    if (range_contains(MAP_CDROM, abs, &off))
    {
        cdrom_store16(&inter->cdrom, off, val, &inter->irq, &inter->scheduler);
        return;
    }
    if (range_contains(MAP_GPU, abs, &off))
    {
        gpu_reg_write16(inter, off, val);
        return;
    }
    if (range_contains(MAP_MDEC, abs, &off))
    {
        mdec_reg_write16(inter, off, val);
        return;
    }

    fprintf(stderr, "Unhandled store16: %08X = %04X\n", addr, val);
    exit(1);
}

/* ---- Store8 ---- */
void interconnect_store8(Interconnect *inter, uint32_t addr, uint8_t val)
{
    uint32_t abs = mask_region(addr);
    uint32_t off;

    if (range_contains(MAP_MEM_CONTROL, abs, &off))
    {
        mem_control_store8(inter, off, val);
        return;
    }
    if (range_contains(MAP_EXPANSION2, abs, &off))
        return;
    if (range_contains(MAP_RAM, abs, &off))
    {
        ram_store8(&inter->ram, off & 0x001FFFFF, val);
        return;
    }
    if (range_contains(MAP_SCRATCHPAD, abs, &off))
    {
        inter->scratchpad[off] = val;
        return;
    }
    if (range_contains(MAP_SPU, abs, &off))
    {
        spu_store8(inter, abs, off, val);
        return;
    }
    if (range_contains(MAP_CDROM, abs, &off))
    {
        cdrom_store8(&inter->cdrom, off, val, &inter->irq, &inter->scheduler);
        return;
    }
    if (range_contains(MAP_SIO, abs, &off))
    {
        sio_store8(&inter->sio, off, val, &inter->irq);
        return;
    }
    if (range_contains(MAP_IRQ_CONTROL, abs, &off))
    {
        irq_store8(&inter->irq, off, val);
        return;
    }
    if (range_contains(MAP_TIMERS, abs, &off))
    {
        timers_store8(&inter->timers, off, val, &inter->scheduler);
        return;
    }
    if (range_contains(MAP_GPU, abs, &off))
    {
        gpu_reg_write8(inter, off, val);
        return;
    }
    if (range_contains(MAP_MDEC, abs, &off))
    {
        mdec_reg_write8(inter, off, val);
        return;
    }
    if (range_contains(MAP_RAM_SIZE, abs, &off))
        return;
    if (range_contains(MAP_CACHE_CTRL, abs, &off))
        return;
    if (range_contains(MAP_DMA, abs, &off))
    {
        dma_reg_write8(inter, off, val);
        return;
    }

    fprintf(stderr, "Unhandled store8: %08X = %02X\n", addr, val);
    exit(1);
}
