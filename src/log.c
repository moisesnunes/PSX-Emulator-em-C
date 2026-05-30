#include "log.h"

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static bool enabled[LOG_SUBSYSTEM_COUNT];

static const struct { const char *name; LogSubsystem id; } subsystem_names[] = {
    { "CPU",   LOG_CPU   },
    { "DMA",   LOG_DMA   },
    { "GPU",   LOG_GPU   },
    { "IRQ",   LOG_IRQ   },
    { "CDROM", LOG_CDROM },
    { "SPU",   LOG_SPU   },
    { "SIO",   LOG_SIO   },
};

static const char *subsystem_prefix[] = {
    [LOG_CPU]   = "CPU",
    [LOG_DMA]   = "DMA",
    [LOG_GPU]   = "GPU",
    [LOG_IRQ]   = "IRQ",
    [LOG_CDROM] = "CDROM",
    [LOG_SPU]   = "SPU",
    [LOG_SIO]   = "SIO",
};

void log_init(void) {
    memset(enabled, 0, sizeof(enabled));

    const char *env = getenv("PS1_LOG");
    if (!env) return;

    char buf[256];
    strncpy(buf, env, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = strtok(buf, ",");
    while (token) {
        for (size_t i = 0; i < sizeof(subsystem_names) / sizeof(subsystem_names[0]); i++) {
            if (strcasecmp(token, subsystem_names[i].name) == 0) {
                enabled[subsystem_names[i].id] = true;
                break;
            }
        }
        token = strtok(NULL, ",");
    }
}

void log_enable(LogSubsystem s) {
    if (s < LOG_SUBSYSTEM_COUNT) enabled[s] = true;
}

bool log_is_enabled(LogSubsystem s) {
    return s < LOG_SUBSYSTEM_COUNT && enabled[s];
}

void log_msg(LogSubsystem s, const char *fmt, ...) {
    if (!log_is_enabled(s)) return;

    fprintf(stderr, "[%s] ", subsystem_prefix[s]);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}
