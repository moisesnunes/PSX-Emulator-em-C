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
    timers_step(timers, cycles, dotclock, hblank, false,
                vblank_started, in_vblank,
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
    EXPECT_EQ(timers.units[0].value, 0);
    step(&timers, &scheduler, &irq, 10, 0, 0, false, false);
    EXPECT_EQ(timers.units[0].value, 10);
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
    EXPECT_EQ(timers.units[1].value, 0);
    step(&timers, &scheduler, &irq, 20, 0, 0, false, true);
    EXPECT_EQ(timers.units[1].value, 20);
}

static void test_one_shot_and_repeat_irq(void)
{
    Timers timers;
    Scheduler scheduler;
    Irq irq = {0};
    timers_init(&timers);
    scheduler_init(&scheduler);

    timers_store16(&timers, 0x08, 3, &scheduler);
    timers_store16(&timers, 0x04,
                   TMODE_RESET_TARGET | TMODE_IRQ_TARGET, &scheduler);
    step(&timers, &scheduler, &irq, 3, 0, 0, false, false);
    EXPECT_EQ((irq.status & IRQ_TIMER0) != 0, 1);
    EXPECT_EQ(timers.units[0].value, 3);
    irq.status = 0;
    step(&timers, &scheduler, &irq, 1, 0, 0, false, false);
    EXPECT_EQ(timers.units[0].value, 0);
    step(&timers, &scheduler, &irq, 3, 0, 0, false, false);
    EXPECT_EQ((irq.status & IRQ_TIMER0) != 0, 0);

    timers_store16(&timers, 0x04,
                   TMODE_RESET_TARGET | TMODE_IRQ_TARGET | TMODE_IRQ_REPEAT,
                   &scheduler);
    step(&timers, &scheduler, &irq, 3, 0, 0, false, false);
    EXPECT_EQ((irq.status & IRQ_TIMER0) != 0, 1);
    irq.status = 0;
    step(&timers, &scheduler, &irq, 4, 0, 0, false, false);
    EXPECT_EQ((irq.status & IRQ_TIMER0) != 0, 1);
}

static void test_toggle_irq_asserts_every_other_target(void)
{
    Timers timers;
    Scheduler scheduler;
    Irq irq = {0};
    timers_init(&timers);
    scheduler_init(&scheduler);

    timers_store16(&timers, 0x08, 2, &scheduler);
    timers_store16(&timers, 0x04,
                   TMODE_RESET_TARGET | TMODE_IRQ_TARGET |
                       TMODE_IRQ_REPEAT | TMODE_IRQ_TOGGLE,
                   &scheduler);

    step(&timers, &scheduler, &irq, 2, 0, 0, false, false);
    EXPECT_EQ((irq.status & IRQ_TIMER0) != 0, 1);
    EXPECT_EQ((timers.units[0].mode & TMODE_IRQ_STATUS) != 0, 0);
    irq.status = 0;

    step(&timers, &scheduler, &irq, 3, 0, 0, false, false);
    EXPECT_EQ((irq.status & IRQ_TIMER0) != 0, 0);
    EXPECT_EQ((timers.units[0].mode & TMODE_IRQ_STATUS) != 0, 1);

    step(&timers, &scheduler, &irq, 3, 0, 0, false, false);
    EXPECT_EQ((irq.status & IRQ_TIMER0) != 0, 1);
}

static void test_hblank_sync_periods(void)
{
    Timers timers;
    Scheduler scheduler;
    Irq irq = {0};
    timers_init(&timers);
    scheduler_init(&scheduler);

    timers_store16(&timers, 0x04, TMODE_SYNC_ENABLE, &scheduler);
    timers_step(&timers, 30, 6, 0, false, false, false,
                &irq, &scheduler);
    timers_step(&timers, 10, 2, 0, true, false, false,
                &irq, &scheduler);
    EXPECT_EQ(timers.units[0].value, 30);

    timers_store16(&timers, 0x04,
                   TMODE_SYNC_ENABLE | (2u << 1) | 0x0100, &scheduler);
    timers_step(&timers, 30, 6, 1, false, false, false,
                &irq, &scheduler);
    EXPECT_EQ(timers.units[0].value, 0);
    timers_step(&timers, 10, 2, 0, true, false, false,
                &irq, &scheduler);
    EXPECT_EQ(timers.units[0].value, 2);
}

static void test_multiple_target_events_in_one_slice(void)
{
    Timers timers;
    Scheduler scheduler;
    Irq irq = {0};
    timers_init(&timers);
    scheduler_init(&scheduler);

    timers_store16(&timers, 0x08, 2, &scheduler);
    timers_store16(&timers, 0x04,
                   TMODE_RESET_TARGET | TMODE_IRQ_TARGET |
                       TMODE_IRQ_REPEAT | TMODE_IRQ_TOGGLE,
                   &scheduler);
    step(&timers, &scheduler, &irq, 8, 0, 0, false, false);
    EXPECT_EQ(timers.units[0].value, 2);
    EXPECT_EQ((timers.units[0].mode & TMODE_IRQ_STATUS) != 0, 0);
    EXPECT_EQ((irq.status & IRQ_TIMER0) != 0, 1);
}

static void test_hblank_clock_counts_lines(void)
{
    Timers timers;
    Scheduler scheduler;
    Irq irq = {0};
    timers_init(&timers);
    scheduler_init(&scheduler);

    timers_store16(&timers, 0x14, 0x0100, &scheduler);
    for (uint32_t line = 0; line < 263; line++)
    {
        timers_step(&timers, 1605, 252, 1, false, false, false,
                    &irq, &scheduler);
        timers_step(&timers, 542, 85, 0, true, false, false,
                    &irq, &scheduler);
    }
    EXPECT_EQ(timers.units[1].value, 263);
}

int main(void)
{
    test_external_clocks();
    test_sync_mode_three_releases_once();
    test_timer1_resets_only_at_vblank_entry();
    test_one_shot_and_repeat_irq();
    test_toggle_irq_asserts_every_other_target();
    test_hblank_sync_periods();
    test_multiple_target_events_in_one_slice();
    test_hblank_clock_counts_lines();
    printf("timer tests: pass=%d fail=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
