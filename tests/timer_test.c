#include "timer.h"
#include <stdint.h>
#include <stdio.h>

static int passed;
static int failed;

#define EXPECT_EQ(actual, expected)                                      \
    do                                                                   \
    {                                                                    \
        uint32_t got_ = (uint32_t)(actual);                              \
        uint32_t expected_ = (uint32_t)(expected);                       \
        if (got_ != expected_)                                           \
        {                                                                \
            fprintf(stderr, "%s:%d expected %u got %u\n",                \
                    __FILE__, __LINE__, expected_, got_);                 \
            failed++;                                                    \
        }                                                                \
        else                                                             \
        {                                                                \
            passed++;                                                    \
        }                                                                \
    } while (0)

static void step(Timers *timers, Scheduler *scheduler, Irq *irq,
                 uint32_t cycles, uint32_t dotclock, uint32_t hblank,
                 bool vblank_started, bool in_vblank)
{
    timers_step(timers, cycles, dotclock, hblank, vblank_started, in_vblank,
                irq, scheduler);
}

static void test_external_clocks(void)
{
    Timers timers;
    Scheduler scheduler;
    Irq irq = {0};
    timers_init(&timers);
    scheduler_init(&scheduler);

    timers_store16(&timers, 0x04, 0x0100, &scheduler);
    timers_store16(&timers, 0x14, 0x0100, &scheduler);
    timers_store16(&timers, 0x24, 0x0200, &scheduler);

    step(&timers, &scheduler, &irq, 7, 13, 2, false, false);
    EXPECT_EQ(timers.units[0].value, 13);
    EXPECT_EQ(timers.units[1].value, 2);
    EXPECT_EQ(timers.units[2].value, 0);

    step(&timers, &scheduler, &irq, 1, 2, 1, false, false);
    EXPECT_EQ(timers.units[0].value, 15);
    EXPECT_EQ(timers.units[1].value, 3);
    EXPECT_EQ(timers.units[2].value, 1);
}

static void test_sync_mode_three_releases_once(void)
{
    Timers timers;
    Scheduler scheduler;
    Irq irq = {0};
    timers_init(&timers);
    scheduler_init(&scheduler);

    timers_store16(&timers, 0x04, TMODE_SYNC_ENABLE | (3u << 1), &scheduler);
    step(&timers, &scheduler, &irq, 10, 0, 0, false, false);
    EXPECT_EQ(timers.units[0].value, 0);
    step(&timers, &scheduler, &irq, 10, 0, 1, false, false);
    EXPECT_EQ(timers.units[0].value, 10);
    step(&timers, &scheduler, &irq, 10, 0, 0, false, false);
    EXPECT_EQ(timers.units[0].value, 20);
}

static void test_timer1_resets_only_at_vblank_entry(void)
{
    Timers timers;
    Scheduler scheduler;
    Irq irq = {0};
    timers_init(&timers);
    scheduler_init(&scheduler);

    timers_store16(&timers, 0x14, TMODE_SYNC_ENABLE | (1u << 1), &scheduler);
    step(&timers, &scheduler, &irq, 20, 0, 0, true, true);
    EXPECT_EQ(timers.units[1].value, 20);
    step(&timers, &scheduler, &irq, 20, 0, 0, false, true);
    EXPECT_EQ(timers.units[1].value, 40);
}

int main(void)
{
    test_external_clocks();
    test_sync_mode_three_releases_once();
    test_timer1_resets_only_at_vblank_entry();
    printf("timer tests: pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
