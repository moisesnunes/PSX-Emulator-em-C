#ifndef LOG_H
#define LOG_H

#include <stdbool.h>

typedef enum
{
    LOG_CPU,
    LOG_DMA,
    LOG_GPU,
    LOG_IRQ,
    LOG_CDROM,
    LOG_SPU,
    LOG_SIO,
    LOG_MDEC,
    LOG_SUBSYSTEM_COUNT
} LogSubsystem;

/* Call once at startup to parse PS1_LOG env var (e.g. PS1_LOG=CPU,IRQ,DMA). */
void log_init(void);

void log_enable(LogSubsystem s);
bool log_is_enabled(LogSubsystem s);
void log_msg(LogSubsystem s, const char *fmt, ...);

#define LOG(subsystem, ...)                  \
    do                                       \
    {                                        \
        if (log_is_enabled(subsystem))       \
            log_msg(subsystem, __VA_ARGS__); \
    } while (0)

#endif /* LOG_H */
