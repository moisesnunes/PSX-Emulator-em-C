#include "cpu.h"
#include "exe.h"
#include "gpu.h"
#include "cdrom.h"
#include "sio.h"
#include "log.h"
#include "scheduler.h"
#include "timer.h"
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Cpu is too large (~3 MB with RAM) to live on the stack — use heap. */

static uint64_t now_nanos(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --bios <path>              BIOS ROM path (default: bios/BIOS.ROM)\n"
            "  --exe  <path>              PS-X EXE to load after BIOS init\n"
            "  --disc <path>              Disc image (.bin) to mount\n"
            "  --headless                 Run without window or audio\n"
            "  --max-instructions <N>     Exit after N instructions (headless smoke test)\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *bios_path = "bios/BIOS.ROM";
    const char *exe_path = NULL;
    const char *disc_path = NULL;
    bool headless = false;
    uint64_t max_instr = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc)
        {
            bios_path = argv[++i];
        }
        else if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc)
        {
            exe_path = argv[++i];
        }
        else if (strcmp(argv[i], "--disc") == 0 && i + 1 < argc)
        {
            disc_path = argv[++i];
        }
        else if (strcmp(argv[i], "--headless") == 0)
        {
            headless = true;
        }
        else if (strcmp(argv[i], "--max-instructions") == 0 && i + 1 < argc)
        {
            max_instr = (uint64_t)strtoull(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    log_init();

    SDL_Window *window = NULL;
    if (!headless)
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
        {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        window = SDL_CreateWindow("PSX",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  1024, 512,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
        if (!window)
        {
            fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
            SDL_Quit();
            return 1;
        }
    }

    Cpu *cpu = (Cpu *)malloc(sizeof(Cpu));
    if (!cpu)
    {
        fprintf(stderr, "Failed to allocate CPU\n");
        if (window)
            SDL_DestroyWindow(window);
        if (!headless)
            SDL_Quit();
        return 1;
    }
    if (cpu_init(cpu, bios_path, window, headless, disc_path) != 0)
    {
        fprintf(stderr, "Failed to init CPU/BIOS\n");
        free(cpu);
        if (window)
            SDL_DestroyWindow(window);
        if (!headless)
            SDL_Quit();
        return 1;
    }

    if (exe_path)
    {
        PsxExe exe;
        if (exe_parse(exe_path, &exe) != 0 ||
            exe_load(exe_path, &exe, cpu->inter.ram.data, RAM_SIZE) != 0)
        {
            fprintf(stderr, "Failed to load EXE: %s\n", exe_path);
            cpu_destroy(cpu);
            free(cpu);
            if (window)
                SDL_DestroyWindow(window);
            if (!headless)
                SDL_Quit();
            return 1;
        }
        cpu->pc = exe.pc;
        cpu->next_pc = exe.pc + 4;
        memset(cpu->regs, 0, sizeof(cpu->regs));
        memset(cpu->out_regs, 0, sizeof(cpu->out_regs));
        cpu->regs[28] = exe.gp;
        cpu->regs[29] = exe.sp;
        cpu->regs[30] = exe.sp;
        cpu->out_regs[28] = exe.gp;
        cpu->out_regs[29] = exe.sp;
        cpu->out_regs[30] = exe.sp;
        cpu->next_instruction = 0;
        fprintf(stderr, "EXE loaded: PC=0x%08X GP=0x%08X SP=0x%08X\n",
                exe.pc, exe.gp, exe.sp);
    }

    if (headless)
    {
        uint64_t count = 0;
        const char *trace_interval_env = getenv("PS1_TRACE_INTERVAL");
        uint64_t trace_interval = trace_interval_env ? strtoull(trace_interval_env, NULL, 10) : 0;
        for (;;)
        {
            uint32_t cycles = cpu_run_next_instruction(cpu);
            uint32_t fired = scheduler_step(&cpu->inter.scheduler, cycles, &cpu->inter.irq);
            timers_step(&cpu->inter.timers, cycles, &cpu->inter.irq, &cpu->inter.scheduler);
            if (fired & (1u << EVENT_VBLANK))
            {
                gpu_vblank(&cpu->inter.gpu);
            }
            if (fired & (1u << EVENT_CDROM_IRQ))
            {
                cdrom_on_scheduler_event(&cpu->inter.cdrom, &cpu->inter.irq,
                                         &cpu->inter.scheduler);
            }
            count++;
            if (trace_interval > 0 && (count % trace_interval) == 0)
            {
                uint32_t op = interconnect_load32(&cpu->inter, cpu->current_pc);
                fprintf(stderr,
                        "TRACE %llu PC=0x%08X OP=0x%08X SR=0x%08X CAUSE=0x%08X EPC=0x%08X K0=0x%08X\n",
                        (unsigned long long)count, cpu->current_pc, op, cpu->sr,
                        cpu->cause, cpu->epc, cpu->regs[26]);
            }
            if (max_instr > 0 && count >= max_instr)
            {
                uint32_t op = interconnect_load32(&cpu->inter, cpu->current_pc);
                fprintf(stderr,
                        "Smoke: ran %llu instructions without abort. PC=0x%08X OP=0x%08X SR=0x%08X CAUSE=0x%08X CYC=%llu VBL=%d@%llu\n",
                        (unsigned long long)count, cpu->current_pc, op, cpu->sr, cpu->cause,
                        (unsigned long long)cpu->inter.scheduler.current_cycle,
                        cpu->inter.scheduler.events[EVENT_VBLANK].active,
                        (unsigned long long)cpu->inter.scheduler.events[EVENT_VBLANK].fire_at);
                cpu_destroy(cpu);
                free(cpu);
                return 0;
            }
        }
    }

    /* Run ~564k cycles per frame (33.868MHz / 60Hz), poll SDL once per frame. */
    const uint32_t CYCLES_PER_FRAME = 33868800U / 60U;
    const uint64_t FRAME_NS         = 1000000000ULL / 60ULL;
    const uint64_t SPU_INTERVAL     = 1000000000ULL / 44100ULL;
    uint64_t frame_deadline = now_nanos() + FRAME_NS;
    uint64_t spu_now = now_nanos();
    uint64_t spu_acc = 0;

    for (;;)
    {
        /* Run one full frame's worth of CPU cycles before polling SDL. */
        uint32_t frame_cycles = 0;
        while (frame_cycles < CYCLES_PER_FRAME)
        {
            uint32_t cycles = cpu_run_next_instruction(cpu);
            uint32_t fired  = scheduler_step(&cpu->inter.scheduler, cycles, &cpu->inter.irq);
            timers_step(&cpu->inter.timers, cycles, &cpu->inter.irq, &cpu->inter.scheduler);
            frame_cycles += cycles;

            if (fired & (1u << EVENT_VBLANK))
                gpu_vblank(&cpu->inter.gpu);

            if (fired & (1u << EVENT_CDROM_IRQ))
                cdrom_on_scheduler_event(&cpu->inter.cdrom, &cpu->inter.irq,
                                         &cpu->inter.scheduler);
        }

        /* SPU clock — catch up for this frame */
        {
            uint64_t now = now_nanos();
            spu_acc += now - spu_now;
            spu_now  = now;
            while (spu_acc >= SPU_INTERVAL)
            {
                spu_acc -= SPU_INTERVAL;
                spu_clock(&cpu->inter.spu);
            }
        }

        /* SDL events */
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
            {
                cpu_destroy(cpu);
                free(cpu);
                SDL_DestroyWindow(window);
                SDL_Quit();
                return 0;
            }
            if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
                sio_on_key(&cpu->inter.sio, event.key.keysym.scancode,
                           event.type == SDL_KEYDOWN);
        }

        /* Sleep until next frame deadline to cap at 60 Hz */
        {
            uint64_t now = now_nanos();
            if (now < frame_deadline)
            {
                uint64_t wait_ns = frame_deadline - now;
                struct timespec ts = { (time_t)(wait_ns / 1000000000ULL),
                                       (long)(wait_ns % 1000000000ULL) };
                nanosleep(&ts, NULL);
            }
            frame_deadline += FRAME_NS;
            /* Fell behind — resync so we don't try to catch up indefinitely */
            now = now_nanos();
            if (now > frame_deadline)
                frame_deadline = now + FRAME_NS;
        }

    }
}
