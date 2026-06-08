#include "scheduler.h"
#include <stdint.h>
#include <stdio.h>

static int passed;
static int failed;

#define EXPECT_EQ(actual, expected)                                      \
    do                                                                   \
    {                                                                    \
        uint64_t got_ = (uint64_t)(actual);                              \
        uint64_t expected_ = (uint64_t)(expected);                       \
        if (got_ != expected_)                                           \
        {                                                                \
            fprintf(stderr, "%s:%d expected %llu got %llu\n",            \
                    __FILE__, __LINE__,                                  \
                    (unsigned long long)expected_,                        \
                    (unsigned long long)got_);                            \
            failed++;                                                    \
        }                                                                \
        else                                                             \
        {                                                                \
            passed++;                                                    \
        }                                                                \
    } while (0)

static void test_next_event_delta(void)
{
    Scheduler scheduler;
    scheduler_init(&scheduler);

    EXPECT_EQ(scheduler_cycles_until_next_event(&scheduler), UINT32_MAX);
    scheduler_schedule(&scheduler, EVENT_CDROM_IRQ, 75);
    scheduler_schedule(&scheduler, EVENT_TIMER0, 25);
    EXPECT_EQ(scheduler_cycles_until_next_event(&scheduler), 25);

    Irq irq = {0};
    EXPECT_EQ(scheduler_step(&scheduler, 24, &irq), 0);
    EXPECT_EQ(scheduler_cycles_until_next_event(&scheduler), 1);
    EXPECT_EQ(scheduler_step(&scheduler, 1, &irq), 1u << EVENT_TIMER0);
    EXPECT_EQ(scheduler.current_cycle, 25);
    EXPECT_EQ(scheduler_cycles_until_next_event(&scheduler), 50);
}

static void test_simultaneous_events(void)
{
    Scheduler scheduler;
    scheduler_init(&scheduler);
    scheduler_schedule(&scheduler, EVENT_TIMER1, 40);
    scheduler_schedule(&scheduler, EVENT_CDROM_IRQ, 40);

    Irq irq = {0};
    uint32_t fired = scheduler_step(&scheduler, 40, &irq);
    EXPECT_EQ(fired, (1u << EVENT_TIMER1) | (1u << EVENT_CDROM_IRQ));
    EXPECT_EQ((irq.status & IRQ_TIMER1) != 0, 1);
    EXPECT_EQ(scheduler_cycles_until_next_event(&scheduler), UINT32_MAX);
}

static void test_due_event_has_zero_delta(void)
{
    Scheduler scheduler;
    scheduler_init(&scheduler);
    scheduler_schedule(&scheduler, EVENT_CDROM_IRQ, 0);

    EXPECT_EQ(scheduler_cycles_until_next_event(&scheduler), 0);
    Irq irq = {0};
    EXPECT_EQ(scheduler_step(&scheduler, 0, &irq), 1u << EVENT_CDROM_IRQ);
}

static void test_dma_event_has_no_early_irq(void)
{
    Scheduler scheduler;
    scheduler_init(&scheduler);
    scheduler_schedule(&scheduler, EVENT_DMA2, 12);

    Irq irq = {0};
    EXPECT_EQ(scheduler_step(&scheduler, 11, &irq), 0);
    EXPECT_EQ(irq.status & IRQ_DMA, 0);
    EXPECT_EQ(scheduler_step(&scheduler, 1, &irq), 1u << EVENT_DMA2);
    EXPECT_EQ(irq.status & IRQ_DMA, 0);
}

int main(void)
{
    test_next_event_delta();
    test_simultaneous_events();
    test_due_event_has_zero_delta();
    test_dma_event_has_no_early_irq();
    printf("scheduler tests: pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
