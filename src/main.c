#include "cpu.h"
#include "exe.h"
#include "log.h"
#include "timer.h"
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --bios <path>              BIOS ROM path (default: bios/BIOS.ROM)\n"
        "  --exe  <path>              PS-X EXE to load after BIOS init\n"
        "  --headless                 Run without window or audio\n"
        "  --max-instructions <N>     Exit after N instructions (headless smoke test)\n",
        prog);
}

int main(int argc, char **argv) {
    const char *bios_path      = "bios/BIOS.ROM";
    const char *exe_path       = NULL;
    bool        headless       = false;
    uint64_t    max_instr      = 0; /* 0 = unlimited */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios_path = argv[++i];
        } else if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc) {
            exe_path = argv[++i];
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--max-instructions") == 0 && i + 1 < argc) {
            max_instr = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    log_init();

    SDL_Window *window = NULL;

    if (!headless) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);

        window = SDL_CreateWindow(
            "PSX",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            1024, 512,
            SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
        );
        if (!window) {
            fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            SDL_Quit();
            return 1;
        }
    }

    Cpu cpu;
    if (cpu_init(&cpu, bios_path, window, headless) != 0) {
        fprintf(stderr, "Failed to initialize CPU/BIOS\n");
        if (window) SDL_DestroyWindow(window);
        if (!headless) SDL_Quit();
        return 1;
    }

    if (exe_path) {
        PsxExe exe;
        if (exe_parse(exe_path, &exe) != 0 ||
            exe_load(exe_path, &exe, cpu.inter.ram.data, RAM_SIZE) != 0) {
            fprintf(stderr, "Failed to load EXE: %s\n", exe_path);
            cpu_destroy(&cpu);
            if (window) SDL_DestroyWindow(window);
            if (!headless) SDL_Quit();
            return 1;
        }
        /* Redirect CPU to the EXE entry point */
        cpu.pc              = exe.pc;
        cpu.next_pc         = exe.pc + 4;
        cpu.regs[28]        = exe.gp;   /* GP */
        cpu.regs[29]        = exe.sp;   /* SP */
        cpu.regs[30]        = exe.sp;   /* FP = SP */
        cpu.out_regs[28]    = exe.gp;
        cpu.out_regs[29]    = exe.sp;
        cpu.out_regs[30]    = exe.sp;
        cpu.next_instruction = 0;       /* NOP in delay slot */
        fprintf(stderr, "EXE loaded: PC=0x%08X GP=0x%08X SP=0x%08X\n",
                exe.pc, exe.gp, exe.sp);
    }

    if (headless) {
        uint64_t count = 0;
        for (;;) {
            uint32_t cycles = cpu_run_next_instruction(&cpu);
            scheduler_step(&cpu.inter.scheduler, cycles, &cpu.inter.irq);
            timers_step(&cpu.inter.timers, cycles, &cpu.inter.irq, &cpu.inter.scheduler);
            count++;
            if (max_instr > 0 && count >= max_instr) {
                fprintf(stderr, "Smoke: ran %llu instructions without abort.\n",
                        (unsigned long long)count);
                cpu_destroy(&cpu);
                return 0;
            }
        }
    }

    const uint64_t SPU_INTERVAL = 1000000000ULL / 44100;
    const uint64_t GPU_INTERVAL = 1000000000ULL / 60;

    uint64_t spu_now = now_nanos();
    uint64_t gpu_now = now_nanos();
    uint64_t nanos   = 0;

    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                cpu_destroy(&cpu);
                SDL_DestroyWindow(window);
                SDL_Quit();
                return 0;
            }
        }

        uint32_t cycles = cpu_run_next_instruction(&cpu);
        scheduler_step(&cpu.inter.scheduler, cycles, &cpu.inter.irq);
        timers_step(&cpu.inter.timers, cycles, &cpu.inter.irq, &cpu.inter.scheduler);

        uint64_t n = now_nanos() - spu_now;
        if (n > SPU_INTERVAL * 20) {
            nanos += n;
            while (nanos >= SPU_INTERVAL) {
                nanos -= SPU_INTERVAL;
                spu_clock(&cpu.inter.spu);
            }
            spu_now = now_nanos();
        }

        if (cpu.inter.gpu.frame_updated) {
            cpu.inter.gpu.frame_updated = false;

            uint64_t elapsed = now_nanos() - gpu_now;
            if (elapsed < GPU_INTERVAL) {
                uint64_t sleep_ns = GPU_INTERVAL - elapsed;
                struct timespec ts = {
                    .tv_sec  = (time_t)(sleep_ns / 1000000000ULL),
                    .tv_nsec = (long)(sleep_ns % 1000000000ULL),
                };
                nanosleep(&ts, NULL);
            }
            gpu_now = now_nanos();
        }
    }
}
