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
#include <strings.h>
#include <time.h>

/* Cpu is too large (~3 MB with RAM) to live on the stack — use heap. */

typedef struct
{
    SDL_GameController *pad;
    SDL_JoystickID instance_id;
    unsigned port;
    uint16_t button_state;
    uint16_t axis_state;
    uint8_t left_x;
    uint8_t left_y;
    uint8_t right_x;
    uint8_t right_y;
} InputController;

#define INPUT_CONTROLLER_COUNT 2
#define PAD_SCRIPT_MAX_EVENTS 64
typedef struct
{
    uint32_t frame;
    uint16_t buttons;
} PadScriptEvent;

static PadScriptEvent g_pad_script[PAD_SCRIPT_MAX_EVENTS];
static uint32_t g_pad_script_count = 0;
static uint32_t g_pad_script_pos = 0;
static uint16_t g_pad_forced_base = 0;
static uint16_t g_pad_script_buttons = 0;

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
            "  --memcard <path>           Slot 1 raw 128 KiB memory card\n"
            "  --headless                 Run without window or audio\n"
            "  --max-instructions <N>     Exit after N instructions (headless smoke test)\n"
            "\n"
            "Debug env:\n"
            "  PS1_PAD_HELD=START,CROSS   Hold digital pad buttons\n"
            "  PS1_PAD_SCRIPT=60:START;66:;4500:CROSS;4510:\n"
            "                              Change held buttons at GPU frames\n"
            "\n"
            "  Positional argument: .exe loads as PS-X EXE, .bin mounts as disc image.\n",
            prog);
}

static void input_controller_close(InputController *ctl)
{
    if (!ctl->pad)
        return;
    SDL_GameControllerClose(ctl->pad);
    ctl->pad = NULL;
    ctl->instance_id = -1;
    ctl->button_state = 0;
    ctl->axis_state = 0;
    ctl->left_x = ctl->left_y = 0x80;
    ctl->right_x = ctl->right_y = 0x80;
}

static void input_controller_apply(InputController *ctl, Sio *sio)
{
    sio_set_port_controller_state(sio, ctl->port,
                                  ctl->button_state | ctl->axis_state);
    sio_set_port_analog_state(sio, ctl->port, ctl->left_x, ctl->left_y,
                              ctl->right_x, ctl->right_y);
}

static bool input_controller_instance_open(const InputController *ctls,
                                           SDL_JoystickID instance_id)
{
    for (unsigned port = 0; port < INPUT_CONTROLLER_COUNT; port++)
    {
        if (ctls[port].pad && ctls[port].instance_id == instance_id)
            return true;
    }
    return false;
}

static void input_controllers_open_available(InputController *ctls)
{
    int count = SDL_NumJoysticks();
    for (int i = 0; i < count; i++)
    {
        if (!SDL_IsGameController(i))
            continue;
        SDL_JoystickID instance_id = SDL_JoystickGetDeviceInstanceID(i);
        if (input_controller_instance_open(ctls, instance_id))
            continue;
        for (unsigned port = 0; port < INPUT_CONTROLLER_COUNT; port++)
        {
            InputController *ctl = &ctls[port];
            if (ctl->pad)
                continue;
            ctl->pad = SDL_GameControllerOpen(i);
            if (!ctl->pad)
                break;
            SDL_Joystick *joy = SDL_GameControllerGetJoystick(ctl->pad);
            ctl->instance_id = joy ? SDL_JoystickInstanceID(joy) : -1;
            fprintf(stderr, "Using controller on port %u: %s\n", port + 1,
                    SDL_GameControllerName(ctl->pad));
            break;
        }
    }
}

static void input_controllers_close(InputController *ctls)
{
    for (unsigned port = 0; port < INPUT_CONTROLLER_COUNT; port++)
        input_controller_close(&ctls[port]);
}

static void input_clear_live_state(InputController *ctls, Sio *sio)
{
    sio_clear_keyboard_state(sio);
    for (unsigned port = 0; port < INPUT_CONTROLLER_COUNT; port++)
    {
        InputController *ctl = &ctls[port];
        ctl->button_state = 0;
        ctl->axis_state = 0;
        ctl->left_x = ctl->left_y = 0x80;
        ctl->right_x = ctl->right_y = 0x80;
        input_controller_apply(ctl, sio);
    }
}

static uint16_t input_controller_button_mask(SDL_GameControllerButton button)
{
    switch (button)
    {
    case SDL_CONTROLLER_BUTTON_A:
        return SIO_PAD_CROSS;
    case SDL_CONTROLLER_BUTTON_B:
        return SIO_PAD_CIRCLE;
    case SDL_CONTROLLER_BUTTON_X:
        return SIO_PAD_SQUARE;
    case SDL_CONTROLLER_BUTTON_Y:
        return SIO_PAD_TRIANGLE;
    case SDL_CONTROLLER_BUTTON_BACK:
        return SIO_PAD_SELECT;
    case SDL_CONTROLLER_BUTTON_START:
        return SIO_PAD_START;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        return SIO_PAD_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        return SIO_PAD_R1;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return SIO_PAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return SIO_PAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return SIO_PAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return SIO_PAD_RIGHT;
    default:
        return 0;
    }
}

static uint8_t input_axis_byte(int16_t value)
{
    return (uint8_t)(((int32_t)value + 32768) >> 8);
}

static void input_controller_handle_button(InputController *ctl, Sio *sio,
                                           SDL_GameControllerButton button,
                                           bool pressed)
{
    uint16_t mask = input_controller_button_mask(button);
    if (!mask)
        return;
    if (pressed)
        ctl->button_state |= mask;
    else
        ctl->button_state &= (uint16_t)~mask;
    input_controller_apply(ctl, sio);
}

static void input_controller_handle_axis(InputController *ctl, Sio *sio,
                                         SDL_GameControllerAxis axis,
                                         int16_t value)
{
    const int16_t deadzone = 12000;
    uint16_t clear = 0;
    uint16_t set = 0;

    switch (axis)
    {
    case SDL_CONTROLLER_AXIS_LEFTX:
        ctl->left_x = input_axis_byte(value);
        clear = SIO_PAD_LEFT | SIO_PAD_RIGHT;
        if (value <= -deadzone)
            set = SIO_PAD_LEFT;
        else if (value >= deadzone)
            set = SIO_PAD_RIGHT;
        break;
    case SDL_CONTROLLER_AXIS_LEFTY:
        ctl->left_y = input_axis_byte(value);
        clear = SIO_PAD_UP | SIO_PAD_DOWN;
        if (value <= -deadzone)
            set = SIO_PAD_UP;
        else if (value >= deadzone)
            set = SIO_PAD_DOWN;
        break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        clear = SIO_PAD_L2;
        if (value >= deadzone)
            set = SIO_PAD_L2;
        break;
    case SDL_CONTROLLER_AXIS_RIGHTX:
        ctl->right_x = input_axis_byte(value);
        input_controller_apply(ctl, sio);
        return;
    case SDL_CONTROLLER_AXIS_RIGHTY:
        ctl->right_y = input_axis_byte(value);
        input_controller_apply(ctl, sio);
        return;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        clear = SIO_PAD_R2;
        if (value >= deadzone)
            set = SIO_PAD_R2;
        break;
    default:
        return;
    }

    ctl->axis_state = (uint16_t)((ctl->axis_state & ~clear) | set);
    input_controller_apply(ctl, sio);
}

static void input_controller_poll_state(InputController *ctl, Sio *sio)
{
    if (!ctl->pad)
        return;

    uint16_t buttons = 0;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
    {
        if (SDL_GameControllerGetButton(ctl->pad, (SDL_GameControllerButton)b))
            buttons |= input_controller_button_mask((SDL_GameControllerButton)b);
    }

    uint16_t axes = 0;
    const int16_t deadzone = 12000;
    int16_t lx = SDL_GameControllerGetAxis(ctl->pad, SDL_CONTROLLER_AXIS_LEFTX);
    int16_t ly = SDL_GameControllerGetAxis(ctl->pad, SDL_CONTROLLER_AXIS_LEFTY);
    int16_t rx = SDL_GameControllerGetAxis(ctl->pad, SDL_CONTROLLER_AXIS_RIGHTX);
    int16_t ry = SDL_GameControllerGetAxis(ctl->pad, SDL_CONTROLLER_AXIS_RIGHTY);
    int16_t lt = SDL_GameControllerGetAxis(ctl->pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    int16_t rt = SDL_GameControllerGetAxis(ctl->pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

    if (lx <= -deadzone)
        axes |= SIO_PAD_LEFT;
    else if (lx >= deadzone)
        axes |= SIO_PAD_RIGHT;
    if (ly <= -deadzone)
        axes |= SIO_PAD_UP;
    else if (ly >= deadzone)
        axes |= SIO_PAD_DOWN;
    if (lt >= deadzone)
        axes |= SIO_PAD_L2;
    if (rt >= deadzone)
        axes |= SIO_PAD_R2;

    ctl->button_state = buttons;
    ctl->axis_state = axes;
    ctl->left_x = input_axis_byte(lx);
    ctl->left_y = input_axis_byte(ly);
    ctl->right_x = input_axis_byte(rx);
    ctl->right_y = input_axis_byte(ry);
    input_controller_apply(ctl, sio);
}

static bool process_sdl_events(Cpu *cpu, InputController *input_ctls)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
            return false;
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            input_clear_live_state(input_ctls, &cpu->inter.sio);
        if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
            sio_on_key(&cpu->inter.sio, event.key.keysym.scancode,
                       event.type == SDL_KEYDOWN);
        else if (event.type == SDL_CONTROLLERDEVICEADDED)
        {
            input_controllers_open_available(input_ctls);
            for (unsigned port = 0; port < INPUT_CONTROLLER_COUNT; port++)
                input_controller_apply(&input_ctls[port], &cpu->inter.sio);
        }
        else if (event.type == SDL_CONTROLLERDEVICEREMOVED)
        {
            for (unsigned port = 0; port < INPUT_CONTROLLER_COUNT; port++)
            {
                InputController *ctl = &input_ctls[port];
                if (!ctl->pad || ctl->instance_id != event.cdevice.which)
                    continue;
                input_controller_close(ctl);
                input_controller_apply(ctl, &cpu->inter.sio);
            }
            input_controllers_open_available(input_ctls);
        }
        else if (event.type == SDL_CONTROLLERBUTTONDOWN ||
                 event.type == SDL_CONTROLLERBUTTONUP)
        {
            for (unsigned port = 0; port < INPUT_CONTROLLER_COUNT; port++)
            {
                InputController *ctl = &input_ctls[port];
                if (ctl->pad && ctl->instance_id == event.cbutton.which)
                    input_controller_handle_button(
                        ctl, &cpu->inter.sio,
                        (SDL_GameControllerButton)event.cbutton.button,
                        event.type == SDL_CONTROLLERBUTTONDOWN);
            }
        }
        else if (event.type == SDL_CONTROLLERAXISMOTION)
        {
            for (unsigned port = 0; port < INPUT_CONTROLLER_COUNT; port++)
            {
                InputController *ctl = &input_ctls[port];
                if (ctl->pad && ctl->instance_id == event.caxis.which)
                    input_controller_handle_axis(
                        ctl, &cpu->inter.sio,
                        (SDL_GameControllerAxis)event.caxis.axis,
                        event.caxis.value);
            }
        }
    }

    for (unsigned port = 0; port < INPUT_CONTROLLER_COUNT; port++)
        input_controller_poll_state(&input_ctls[port], &cpu->inter.sio);
    return true;
}

static bool sleep_until_frame_deadline(Cpu *cpu, InputController *input_ctls,
                                       uint64_t deadline)
{
    for (;;)
    {
        uint64_t now = now_nanos();
        if (now >= deadline)
            return true;

        if (!process_sdl_events(cpu, input_ctls))
            return false;

        uint64_t wait_ns = deadline - now;
        if (wait_ns > 1000000ULL)
            wait_ns = 1000000ULL;
        struct timespec ts = {(time_t)(wait_ns / 1000000000ULL),
                              (long)(wait_ns % 1000000000ULL)};
        nanosleep(&ts, NULL);
    }
}

static uint16_t input_button_name_mask(const char *name)
{
    if (strcasecmp(name, "SELECT") == 0)
        return SIO_PAD_SELECT;
    if (strcasecmp(name, "START") == 0)
        return SIO_PAD_START;
    if (strcasecmp(name, "UP") == 0)
        return SIO_PAD_UP;
    if (strcasecmp(name, "RIGHT") == 0)
        return SIO_PAD_RIGHT;
    if (strcasecmp(name, "DOWN") == 0)
        return SIO_PAD_DOWN;
    if (strcasecmp(name, "LEFT") == 0)
        return SIO_PAD_LEFT;
    if (strcasecmp(name, "L2") == 0)
        return SIO_PAD_L2;
    if (strcasecmp(name, "R2") == 0)
        return SIO_PAD_R2;
    if (strcasecmp(name, "L1") == 0)
        return SIO_PAD_L1;
    if (strcasecmp(name, "R1") == 0)
        return SIO_PAD_R1;
    if (strcasecmp(name, "TRIANGLE") == 0)
        return SIO_PAD_TRIANGLE;
    if (strcasecmp(name, "CIRCLE") == 0)
        return SIO_PAD_CIRCLE;
    if (strcasecmp(name, "CROSS") == 0 || strcasecmp(name, "X") == 0)
        return SIO_PAD_CROSS;
    if (strcasecmp(name, "SQUARE") == 0)
        return SIO_PAD_SQUARE;
    return 0;
}

static uint16_t input_parse_button_list(const char *list)
{
    char buf[256];
    strncpy(buf, list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uint16_t pressed = 0;
    char *save = NULL;
    char *token = strtok_r(buf, ",+| ", &save);
    while (token)
    {
        uint16_t mask = input_button_name_mask(token);
        if (mask)
            pressed |= mask;
        else
            fprintf(stderr, "Unknown PS1 pad button: %s\n", token);
        token = strtok_r(NULL, ",+| ", &save);
    }

    return pressed;
}

static void input_apply_forced_state(Sio *sio)
{
    sio_set_forced_state(sio, g_pad_forced_base | g_pad_script_buttons);
}

static void input_apply_forced_env(Sio *sio)
{
    const char *env = getenv("PS1_PAD_HELD");
    if (!env || !*env)
        return;

    uint16_t pressed = input_parse_button_list(env);
    g_pad_forced_base = pressed;
    input_apply_forced_state(sio);
    if (pressed)
        fprintf(stderr, "Holding PS1 pad buttons from PS1_PAD_HELD=0x%04X\n", pressed);
}

static void input_parse_pad_script(void)
{
    const char *env = getenv("PS1_PAD_SCRIPT");
    if (!env || !*env)
        return;

    const char *entry = env;
    while (*entry && g_pad_script_count < PAD_SCRIPT_MAX_EVENTS)
    {
        const char *end = strchr(entry, ';');
        size_t len = end ? (size_t)(end - entry) : strlen(entry);
        char item[256];
        if (len >= sizeof(item))
            len = sizeof(item) - 1;
        memcpy(item, entry, len);
        item[len] = '\0';

        char *colon = strchr(item, ':');
        if (colon)
        {
            *colon = '\0';
            g_pad_script[g_pad_script_count].frame = (uint32_t)strtoul(item, NULL, 0);
            g_pad_script[g_pad_script_count].buttons = input_parse_button_list(colon + 1);
            g_pad_script_count++;
        }
        else
        {
            fprintf(stderr, "Ignoring malformed PS1_PAD_SCRIPT entry: %s\n", item);
        }
        if (!end)
            break;
        entry = end + 1;
    }

    if (g_pad_script_count)
        fprintf(stderr, "Loaded %u PS1_PAD_SCRIPT events\n", g_pad_script_count);
}

static void input_update_pad_script(Sio *sio, uint32_t frame)
{
    while (g_pad_script_pos < g_pad_script_count &&
           frame >= g_pad_script[g_pad_script_pos].frame)
    {
        g_pad_script_buttons = g_pad_script[g_pad_script_pos].buttons;
        input_apply_forced_state(sio);
        fprintf(stderr, "PS1_PAD_SCRIPT frame=%u buttons=0x%04X\n",
                g_pad_script[g_pad_script_pos].frame, g_pad_script_buttons);
        g_pad_script_pos++;
    }
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

static void advance_system_cycles(Cpu *cpu, uint32_t cycles)
{
    dma_step(&cpu->inter.dma, &cpu->inter.irq);
    uint32_t remaining = cycles;
    while (remaining > 0)
    {
        uint32_t slice = remaining;
        uint32_t until_event = scheduler_cycles_until_next_event(&cpu->inter.scheduler);
        if (until_event < slice)
            slice = until_event;
        uint32_t until_gpu =
            gpu_cycles_until_timing_boundary(&cpu->inter.gpu);
        if (until_gpu < slice)
            slice = until_gpu;
        uint32_t until_sio = sio_cycles_until_event(&cpu->inter.sio);
        if (until_sio < slice)
            slice = until_sio;

        uint32_t fired = scheduler_step(&cpu->inter.scheduler, slice, &cpu->inter.irq);
        if (slice > 0)
        {
            bool in_hblank = gpu_in_hblank(&cpu->inter.gpu);
            bool in_vblank = gpu_in_vblank(&cpu->inter.gpu);
            GpuTimingEvents gpu_events = gpu_step(&cpu->inter.gpu, slice);
            timers_step(&cpu->inter.timers, slice, gpu_events.dotclock_ticks,
                        gpu_events.hblank_count, in_hblank,
                        gpu_events.vblank_started, in_vblank,
                        &cpu->inter.irq, &cpu->inter.scheduler);
            sio_step(&cpu->inter.sio, &cpu->inter.irq, slice);
            spu_step(&cpu->inter.spu, slice);

            if (gpu_events.vblank_started)
            {
                irq_assert(&cpu->inter.irq, IRQ_VBLANK);
                gpu_vblank_start(&cpu->inter.gpu);
            }
            if (gpu_events.frame_ended)
            {
                input_update_pad_script(&cpu->inter.sio, cpu->inter.gpu.frames);
                gpu_vblank(&cpu->inter.gpu);
            }
            remaining -= slice;
        }

        if (fired & (1u << EVENT_CDROM_IRQ))
            cdrom_on_scheduler_event(&cpu->inter.cdrom, &cpu->inter.irq,
                                     &cpu->inter.scheduler);
        interconnect_on_scheduler_events(&cpu->inter, fired);
    }
}

int main(int argc, char **argv)
{
    const char *bios_path = "bios/BIOS.ROM";
    const char *exe_path = NULL;
    const char *disc_path = NULL;
    const char *memcard_path = NULL;
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
        else if (strcmp(argv[i], "--memcard") == 0 && i + 1 < argc)
        {
            memcard_path = argv[++i];
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
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
        {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        window = SDL_CreateWindow("PSX",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  960, 720,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
                                      SDL_WINDOW_RESIZABLE |
                                      SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window)
        {
            fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
            SDL_Quit();
            return 1;
        }
    }

    InputController input_ctl[INPUT_CONTROLLER_COUNT] = {0};
    for (unsigned port = 0; port < INPUT_CONTROLLER_COUNT; port++)
    {
        input_ctl[port].instance_id = -1;
        input_ctl[port].port = port;
        input_ctl[port].left_x = input_ctl[port].left_y = 0x80;
        input_ctl[port].right_x = input_ctl[port].right_y = 0x80;
    }
    if (!headless)
        input_controllers_open_available(input_ctl);

    Cpu *cpu = (Cpu *)malloc(sizeof(Cpu));
    if (!cpu)
    {
        fprintf(stderr, "Failed to allocate CPU\n");
        input_controllers_close(input_ctl);
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
        input_controllers_close(input_ctl);
        if (window)
            SDL_DestroyWindow(window);
        if (!headless)
            SDL_Quit();
        return 1;
    }
    input_apply_forced_env(&cpu->inter.sio);
    input_parse_pad_script();
    if (memcard_path && sio_memory_card_load(&cpu->inter.sio, memcard_path) < 0)
    {
        fprintf(stderr, "Memory card unavailable: %s\n", memcard_path);
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
            input_controllers_close(input_ctl);
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
                advance_system_cycles(cpu, CPU_CYCLE_QUANTUM);
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
                input_controllers_close(input_ctl);
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
                input_controllers_close(input_ctl);
                return 0;
            }
        }
    }

    const uint32_t CPU_CYCLE_QUANTUM = 100;
    const uint32_t INPUT_POLL_CYCLES = 3000;
    const uint64_t FRAME_NS = 1000000000ULL / 60ULL;
    const uint64_t MAX_FRAME_LAG_NS = FRAME_NS * 5ULL;
    uint64_t frame_deadline = now_nanos() + FRAME_NS;
    uint32_t cpu_quantum_cycles = 0;

    for (;;)
    {
        uint32_t frame_start = cpu->inter.gpu.frames;
        uint32_t input_poll_cycles = 0;

        if (!process_sdl_events(cpu, input_ctl))
            break;

        /* Run in small slices until the GPU produces the next VBlank.  This
           keeps SDL input fresh and avoids pacing against a second frame clock. */
        while (cpu->inter.gpu.frames == frame_start)
        {
            uint32_t cycles = cpu_run_next_instruction(cpu);
            cpu_quantum_cycles += cycles;
            input_poll_cycles += cycles;
            while (cpu_quantum_cycles >= CPU_CYCLE_QUANTUM)
            {
                advance_system_cycles(cpu, CPU_CYCLE_QUANTUM);
                cpu_quantum_cycles -= CPU_CYCLE_QUANTUM;
            }

            if (input_poll_cycles >= INPUT_POLL_CYCLES)
            {
                input_poll_cycles = 0;
                if (!process_sdl_events(cpu, input_ctl))
                    goto quit_normal_loop;
            }
        }

        if (!process_sdl_events(cpu, input_ctl))
            break;

        if (!sleep_until_frame_deadline(cpu, input_ctl, frame_deadline))
            break;
        frame_deadline += FRAME_NS;
        uint64_t now = now_nanos();
        if (now > frame_deadline + MAX_FRAME_LAG_NS)
            frame_deadline = now + FRAME_NS;
    }

quit_normal_loop:
    cpu_destroy(cpu);
    free(cpu);
    input_controllers_close(input_ctl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
