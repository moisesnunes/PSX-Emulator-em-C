#pragma once
#include "bios.h"
#include "ram.h"
#include "dma.h"
#include "gpu.h"
#include "spu.h"
#include "irq.h"
#include "scheduler.h"
#include "timer.h"
#include "cdrom.h"
#include "disc.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    Bios bios;
    Ram ram;
    Dma dma;
    Gpu gpu;
    Spu spu;
    Irq irq;
    Scheduler scheduler;
    Timers timers;
    Cdrom cdrom;
    Disc disc;
    bool disc_loaded;
} Interconnect;

/* window may be NULL in headless mode; headless skips audio init.
   disc_path may be NULL (no disc). */
int interconnect_init(Interconnect *inter, const char *bios_path, SDL_Window *window,
                      bool headless, const char *disc_path);
uint32_t interconnect_load32(Interconnect *inter, uint32_t addr);
uint16_t interconnect_load16(Interconnect *inter, uint32_t addr);
uint8_t interconnect_load8(Interconnect *inter, uint32_t addr);
void interconnect_store32(Interconnect *inter, uint32_t addr, uint32_t val);
void interconnect_store16(Interconnect *inter, uint32_t addr, uint16_t val);
void interconnect_store8(Interconnect *inter, uint32_t addr, uint8_t val);
void interconnect_destroy(Interconnect *inter);
