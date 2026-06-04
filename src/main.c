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
            "Usage: %s [options] [file.exe | file.bin]\n"
            "  --bios <path>              BIOS ROM path (default: bios/BIOS.ROM)\n"
            "  --exe  <path>              PS-X EXE to load after BIOS init\n"
            "  --disc <path>              Disc image (.bin) to mount\n"
            "  --headless                 Run without window or audio\n"
            "  --max-instructions <N>     Exit after N instructions (headless smoke test)\n"
            "\n"
            "  Positional argument: .exe loads as PS-X EXE, .bin mounts as disc image.\n",
            prog);
}

static uint32_t debug_load32(Interconnect *inter, uint32_t addr)
{
    if (addr & 3u)
        return 0xCACACACA;
    return interconnect_load32(inter, addr);
}

static void maybe_dump_ram(Cpu *cpu)
{
    const char *spec = getenv("PS1_DUMP_RAM");
    if (!spec)
        return;

    char buf[512];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *addr_s = strtok(buf, ",");
    char *len_s = strtok(NULL, ",");
    char *path_s = strtok(NULL, "");
    if (!addr_s || !len_s || !path_s)
        return;

    uint32_t addr = (uint32_t)strtoul(addr_s, NULL, 0);
    uint32_t len = (uint32_t)strtoul(len_s, NULL, 0);
    uint32_t off = addr & 0x001FFFFFu;
    if (off >= RAM_SIZE)
        return;
    if (len > RAM_SIZE - off)
        len = RAM_SIZE - off;

    FILE *f = fopen(path_s, "wb");
    if (!f)
        return;
    fwrite(cpu->inter.ram.data + off, 1, len, f);
    fclose(f);
}

static void advance_system_quantum(Cpu *cpu)
{
    const uint32_t SYSTEM_CYCLE_QUANTUM = 300;

    dma_step(&cpu->inter.dma, &cpu->inter.irq);
    uint32_t fired = scheduler_step(&cpu->inter.scheduler, SYSTEM_CYCLE_QUANTUM, &cpu->inter.irq);

    if (fired & (1u << EVENT_CDROM_IRQ))
        cdrom_on_scheduler_event(&cpu->inter.cdrom, &cpu->inter.irq,
                                 &cpu->inter.scheduler);

    timers_step(&cpu->inter.timers, SYSTEM_CYCLE_QUANTUM, &cpu->inter.irq, &cpu->inter.scheduler);

    if (gpu_step(&cpu->inter.gpu, SYSTEM_CYCLE_QUANTUM))
    {
        irq_assert(&cpu->inter.irq, IRQ_VBLANK);
        gpu_vblank(&cpu->inter.gpu);
    }
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
        else if (argv[i][0] != '-')
        {
            /* positional: auto-detect by extension */
            const char *arg = argv[i];
            size_t len = strlen(arg);
            if (len > 4 && strcasecmp(arg + len - 4, ".exe") == 0)
                exe_path = arg;
            else if (len > 4 && strcasecmp(arg + len - 4, ".bin") == 0)
                disc_path = arg;
            else
            {
                fprintf(stderr, "Unknown file type: %s (expected .exe or .bin)\n", arg);
                usage(argv[0]);
                return 1;
            }
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
        cpu->hle_bios_vectors = true;
        fprintf(stderr, "EXE loaded: PC=0x%08X GP=0x%08X SP=0x%08X\n",
                exe.pc, exe.gp, exe.sp);
    }

    if (headless)
    {
        uint64_t count = 0;
        const char *trace_interval_env = getenv("PS1_TRACE_INTERVAL");
        uint64_t trace_interval = trace_interval_env ? strtoull(trace_interval_env, NULL, 10) : 0;
        const char *break_pc_env = getenv("PS1_BREAK_PC");
        uint32_t break_pc = break_pc_env ? (uint32_t)strtoul(break_pc_env, NULL, 0) : 0;
        bool break_pc_enabled = break_pc_env != NULL;

        /* PS1_WATCH_PC=0xADDR[,0xADDR,...] — dump regs once per unique hit, then
         * every PS1_WATCH_PC_REPEAT hits (default 1 = every hit). */
#define WATCH_PC_MAX 16
        uint32_t watch_pcs[WATCH_PC_MAX];
        uint32_t watch_pc_count = 0;
        uint64_t watch_pc_hits[WATCH_PC_MAX];
        uint64_t watch_pc_repeat = 1;
        {
            const char *e = getenv("PS1_WATCH_PC");
            if (e)
            {
                char buf[256];
                strncpy(buf, e, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                char *tok = strtok(buf, ",");
                while (tok && watch_pc_count < WATCH_PC_MAX)
                {
                    watch_pcs[watch_pc_count] = (uint32_t)strtoul(tok, NULL, 0);
                    watch_pc_hits[watch_pc_count] = 0;
                    watch_pc_count++;
                    tok = strtok(NULL, ",");
                }
            }
            const char *r = getenv("PS1_WATCH_PC_REPEAT");
            if (r)
                watch_pc_repeat = (uint64_t)strtoull(r, NULL, 10);
            if (watch_pc_repeat < 1)
                watch_pc_repeat = 1;
        }

        /* PS1_WATCH_RAM=0xADDR[,0xADDR,...] — print when word at address changes. */
#define WATCH_RAM_MAX 8
        uint32_t watch_ram_addr[WATCH_RAM_MAX];
        uint32_t watch_ram_prev[WATCH_RAM_MAX];
        uint32_t watch_ram_count = 0;
        {
            const char *e = getenv("PS1_WATCH_RAM");
            if (e)
            {
                char buf[256];
                strncpy(buf, e, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                char *tok = strtok(buf, ",");
                while (tok && watch_ram_count < WATCH_RAM_MAX)
                {
                    watch_ram_addr[watch_ram_count] = (uint32_t)strtoul(tok, NULL, 0);
                    watch_ram_prev[watch_ram_count] = 0xDEADBEEF;
                    watch_ram_count++;
                    tok = strtok(NULL, ",");
                }
            }
        }

        const uint32_t CPU_CYCLE_QUANTUM = 100;
        uint32_t cpu_quantum_cycles = 0;

        for (;;)
        {
            uint32_t cycles = cpu_run_next_instruction(cpu);
            cpu_quantum_cycles += cycles;
            while (cpu_quantum_cycles >= CPU_CYCLE_QUANTUM)
            {
                advance_system_quantum(cpu);
                cpu_quantum_cycles -= CPU_CYCLE_QUANTUM;
            }
            count++;

            /* PS1_WATCH_PC hits */
            for (uint32_t wi = 0; wi < watch_pc_count; wi++)
            {
                if (cpu->current_pc != watch_pcs[wi])
                    continue;
                watch_pc_hits[wi]++;
                if (watch_pc_hits[wi] != 1 && (watch_pc_hits[wi] % watch_pc_repeat) != 0)
                    continue;
                fprintf(stderr,
                        "WATCH_PC[%u] hit=%llu PC=0x%08X SR=0x%08X EPC=0x%08X "
                        "I_STAT=0x%04X I_MASK=0x%04X\n",
                        wi, (unsigned long long)watch_pc_hits[wi],
                        cpu->current_pc, cpu->sr, cpu->epc,
                        cpu->inter.irq.status, cpu->inter.irq.mask);
                for (int r = 0; r < 32; r += 4)
                    fprintf(stderr, "  R%02d=%08X R%02d=%08X R%02d=%08X R%02d=%08X\n",
                            r, cpu->regs[r],
                            r + 1, cpu->regs[r + 1],
                            r + 2, cpu->regs[r + 2],
                            r + 3, cpu->regs[r + 3]);
                /* Print s0 area: 8 words around cpu->regs[16] if it looks like RAM */
                uint32_t s0 = cpu->regs[16];
                if (s0 >= 0x80000000u && s0 < 0x80200000u)
                {
                    fprintf(stderr, "  s0=0x%08X context:\n", s0);
                    for (int d = -4; d <= 12; d++)
                    {
                        uint32_t a = s0 + (uint32_t)(d * 4);
                        fprintf(stderr, "    [s0%+d]=0x%08X\n", d * 4,
                                debug_load32(&cpu->inter, a));
                    }
                }
            }

            /* PS1_WATCH_RAM change detector */
            for (uint32_t wi = 0; wi < watch_ram_count; wi++)
            {
                uint32_t cur = interconnect_load32(&cpu->inter, watch_ram_addr[wi]);
                if (cur != watch_ram_prev[wi])
                {
                    fprintf(stderr,
                            "WATCH_RAM 0x%08X: 0x%08X -> 0x%08X  (instr=%llu PC=0x%08X)\n",
                            watch_ram_addr[wi], watch_ram_prev[wi], cur,
                            (unsigned long long)count, cpu->current_pc);
                    watch_ram_prev[wi] = cur;
                }
            }

            if (break_pc_enabled && cpu->current_pc == break_pc)
            {
                uint32_t op = debug_load32(&cpu->inter, cpu->current_pc);
                fprintf(stderr,
                        "BREAK_PC %llu PC=0x%08X OP=0x%08X SR=0x%08X CAUSE=0x%08X EPC=0x%08X I_STAT=0x%04X I_MASK=0x%04X\n",
                        (unsigned long long)count, cpu->current_pc, op, cpu->sr,
                        cpu->cause, cpu->epc, cpu->inter.irq.status, cpu->inter.irq.mask);
                for (int r = 0; r < 32; r += 4)
                {
                    fprintf(stderr,
                            "R%02d=0x%08X R%02d=0x%08X R%02d=0x%08X R%02d=0x%08X\n",
                            r, cpu->regs[r], r + 1, cpu->regs[r + 1],
                            r + 2, cpu->regs[r + 2], r + 3, cpu->regs[r + 3]);
                }
                for (int i = -8; i <= 8; i++)
                {
                    uint32_t addr = cpu->current_pc + (uint32_t)(i * 4);
                    fprintf(stderr, "CODE 0x%08X: 0x%08X\n",
                            addr, debug_load32(&cpu->inter, addr));
                }
                cpu_destroy(cpu);
                free(cpu);
                return 0;
            }
            if (trace_interval > 0 && (count % trace_interval) == 0)
            {
                uint32_t op = debug_load32(&cpu->inter, cpu->current_pc);
                fprintf(stderr,
                        "TRACE %llu PC=0x%08X OP=0x%08X SR=0x%08X CAUSE=0x%08X EPC=0x%08X K0=0x%08X I_STAT=0x%04X I_MASK=0x%04X\n",
                        (unsigned long long)count, cpu->current_pc, op, cpu->sr,
                        cpu->cause, cpu->epc, cpu->regs[26],
                        cpu->inter.irq.status, cpu->inter.irq.mask);
            }
            if (max_instr > 0 && count >= max_instr)
            {
                uint32_t op = debug_load32(&cpu->inter, cpu->current_pc);
                fprintf(stderr,
                        "Smoke: ran %llu instructions without abort. PC=0x%08X OP=0x%08X SR=0x%08X CAUSE=0x%08X CYC=%llu GPU_FRAMES=%llu I_STAT=0x%04X I_MASK=0x%04X\n",
                        (unsigned long long)count, cpu->current_pc, op, cpu->sr, cpu->cause,
                        (unsigned long long)cpu->inter.scheduler.current_cycle,
                        (unsigned long long)(uint64_t)cpu->inter.gpu.frames,
                        cpu->inter.irq.status, cpu->inter.irq.mask);
                maybe_dump_ram(cpu);
                cpu_destroy(cpu);
                free(cpu);
                return 0;
            }
        }
    }

    /* Run ~564k cycles per frame (33.868MHz / 60Hz), poll SDL once per frame. */
    const uint32_t CYCLES_PER_FRAME = 33868800U / 60U;
    const uint32_t CPU_CYCLE_QUANTUM = 100;
    const uint32_t SYSTEM_CYCLE_QUANTUM = 300;
    const uint64_t FRAME_NS = 1000000000ULL / 60ULL;
    const uint64_t SPU_INTERVAL = 1000000000ULL / 44100ULL;
    uint64_t frame_deadline = now_nanos() + FRAME_NS;
    uint64_t spu_now = now_nanos();
    uint64_t spu_acc = 0;

    for (;;)
    {
        /* Run one full frame's worth of CPU cycles before polling SDL. */
        uint32_t frame_cycles = 0;
        uint32_t cpu_quantum_cycles = 0;
        while (frame_cycles < CYCLES_PER_FRAME)
        {
            uint32_t cycles = cpu_run_next_instruction(cpu);
            cpu_quantum_cycles += cycles;
            while (cpu_quantum_cycles >= CPU_CYCLE_QUANTUM)
            {
                advance_system_quantum(cpu);
                frame_cycles += SYSTEM_CYCLE_QUANTUM;
                cpu_quantum_cycles -= CPU_CYCLE_QUANTUM;
            }
        }

        /* SPU clock — catch up for this frame */
        {
            uint64_t now = now_nanos();
            spu_acc += now - spu_now;
            spu_now = now;
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
                struct timespec ts = {(time_t)(wait_ns / 1000000000ULL),
                                      (long)(wait_ns % 1000000000ULL)};
                nanosleep(&ts, NULL);
            }
            frame_deadline += FRAME_NS;
            now = now_nanos();
            if (now > frame_deadline)
                frame_deadline = now + FRAME_NS;
        }
    }
}
