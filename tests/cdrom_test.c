/*
 * tests/cdrom_test.c — unit tests for the CD-ROM controller.
 *
 * Compiled standalone: links cdrom.c + disc.c + stub implementations below.
 * Does NOT link the full emulator (no SDL, no OpenGL, no BIOS).
 *
 * Build:  make test-cdrom
 */

/* Pull in the real types from the project headers. */
#include "log.h"       /* LogSubsystem — must come before cdrom.h */
#include "scheduler.h" /* EventType, Scheduler, PS1_CPU_HZ */
#include "irq.h"       /* Irq, IrqFlag */
#include "disc.h"      /* Disc, Msf */
#include "cdrom.h"     /* Cdrom, CDROM_INT*, cdrom_event_log_* */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Stub implementations (linked instead of the real .o files)
 * ========================================================================= */

/* ---- irq.c stub ---- */
void irq_init(Irq *irq) { memset(irq, 0, sizeof(*irq)); }
void irq_assert(Irq *irq, IrqFlag f) { irq->status |= (uint16_t)f; }
bool irq_pending(const Irq *irq) { return (irq->status & irq->mask) != 0; }
uint32_t irq_load32(const Irq *irq, uint32_t offset)
{
    (void)irq;
    (void)offset;
    return 0;
}
uint16_t irq_load16(const Irq *irq, uint32_t offset)
{
    (void)irq;
    (void)offset;
    return 0;
}
void irq_store32(Irq *irq, uint32_t offset, uint32_t val)
{
    (void)irq;
    (void)offset;
    (void)val;
}
void irq_store16(Irq *irq, uint32_t offset, uint16_t val)
{
    (void)irq;
    (void)offset;
    (void)val;
}

/* ---- scheduler.c stub ---- */
void scheduler_init(Scheduler *s) { memset(s, 0, sizeof(*s)); }

void scheduler_schedule(Scheduler *s, EventType type, uint32_t delta)
{
    s->events[type].fire_at = s->current_cycle + delta;
    s->events[type].type = type;
    s->events[type].active = true;
}

void scheduler_cancel(Scheduler *s, EventType type)
{
    s->events[type].active = false;
}

uint32_t scheduler_step(Scheduler *s, uint32_t cycles, Irq *irq)
{
    (void)irq;
    s->current_cycle += cycles;
    uint32_t fired = 0;
    for (int i = 0; i < EVENT_COUNT; i++)
    {
        if (s->events[i].active && s->current_cycle >= s->events[i].fire_at)
        {
            s->events[i].active = false;
            fired |= 1u << i;
        }
    }
    return fired;
}

/* ---- log.c stub ---- */
void log_init(void) {}
void log_enable(LogSubsystem s) { (void)s; }
bool log_is_enabled(LogSubsystem s)
{
    (void)s;
    return false;
}
void log_msg(LogSubsystem s, const char *fmt, ...)
{
    (void)s;
    (void)fmt;
}

/* ---- disc.c stub (no real file I/O needed in unit tests) ---- */
uint8_t to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
uint8_t from_bcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0xF)); }

Msf msf_from_lba(uint32_t lba)
{
    lba += 150;
    return (Msf){to_bcd((uint8_t)(lba / 75 / 60)),
                 to_bcd((uint8_t)((lba / 75) % 60)),
                 to_bcd((uint8_t)(lba % 75))};
}

uint32_t msf_to_lba(Msf msf)
{
    uint32_t m = from_bcd(msf.m), s = from_bcd(msf.s), f = from_bcd(msf.f);
    return (m * 60 + s) * 75 + f - 150;
}

int disc_open(Disc *disc, const char *path)
{
    (void)path;
    disc->fp = (FILE *)1; /* non-NULL sentinel */
    disc->sector_count = 300;
    return 0;
}

void disc_close(Disc *disc) { disc->fp = NULL; }

int disc_read_sector(Disc *disc, uint32_t lba, uint8_t buf[DISC_SECTOR_SIZE])
{
    (void)disc;
    (void)lba;
    memset(buf, 0xAA, DISC_SECTOR_SIZE);
    return 0;
}

/* =========================================================================
 * Test harness
 * ========================================================================= */

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_EQ(a, b)                                                \
    do                                                                 \
    {                                                                  \
        if ((uint32_t)(a) != (uint32_t)(b))                            \
        {                                                              \
            fprintf(stderr, "  FAIL %s:%d  expected %u got %u\n",      \
                    __FILE__, __LINE__, (unsigned)(b), (unsigned)(a)); \
            g_fail++;                                                  \
        }                                                              \
        else                                                           \
        {                                                              \
            g_pass++;                                                  \
        }                                                              \
    } while (0)

#define EXPECT_TRUE(x) EXPECT_EQ(!!(x), 1u)

/* Advance the scheduler until EVENT_CDROM_IRQ is fully drained. */
static void run_until_quiet(Cdrom *cd, Irq *irq, Scheduler *sched,
                            uint64_t max_cycles)
{
    const uint32_t step = PS1_CPU_HZ / 500; /* ~2 ms */
    for (uint64_t e = 0; e < max_cycles; e += step)
    {
        uint32_t fired = scheduler_step(sched, step, irq);
        if (fired & (1u << EVENT_CDROM_IRQ))
            cdrom_on_scheduler_event(cd, irq, sched);
        if (cd->evq.count == 0 && !sched->events[EVENT_CDROM_IRQ].active)
            break;
    }
}

static void setup(Cdrom *cd, Irq *irq, Scheduler *sched, Disc *disc)
{
    irq_init(irq);
    scheduler_init(sched);
    disc_open(disc, "fake.bin");
    cdrom_init(cd, disc);
    cdrom_event_log_reset();
}

static void reg_write(Cdrom *cd, uint32_t offset, uint8_t val,
                      Irq *irq, Scheduler *sched)
{
    cdrom_store8(cd, offset, val, irq, sched);
}

static void push_param(Cdrom *cd, uint8_t v, Irq *irq, Scheduler *sched)
{
    reg_write(cd, 0, 0, irq, sched); /* index = 0 */
    reg_write(cd, 2, v, irq, sched); /* param FIFO */
}

static void send_cmd(Cdrom *cd, uint8_t cmd, Irq *irq, Scheduler *sched)
{
    reg_write(cd, 0, 0, irq, sched); /* index = 0 */
    reg_write(cd, 1, cmd, irq, sched);
}

/* =========================================================================
 * Tests
 * ========================================================================= */

static void test_getstat(void)
{
    printf("test_getstat ... ");
    Cdrom cd;
    Irq irq;
    Scheduler sched;
    Disc disc;
    setup(&cd, &irq, &sched, &disc);

    send_cmd(&cd, 0x01, &irq, &sched);
    run_until_quiet(&cd, &irq, &sched, (uint64_t)PS1_CPU_HZ);

    EXPECT_EQ(cdrom_event_log_count(), 1u);
    EXPECT_EQ(cdrom_event_log_get(0), CDROM_INT3);
    EXPECT_TRUE(irq.status & IRQ_CDROM);
    printf("ok\n");
}

static void test_seekl(void)
{
    printf("test_seekl ... ");
    Cdrom cd;
    Irq irq;
    Scheduler sched;
    Disc disc;
    setup(&cd, &irq, &sched, &disc);

    push_param(&cd, 0x00, &irq, &sched);
    push_param(&cd, 0x02, &irq, &sched);
    push_param(&cd, 0x00, &irq, &sched);
    send_cmd(&cd, 0x02, &irq, &sched); /* Setloc */
    run_until_quiet(&cd, &irq, &sched, (uint64_t)PS1_CPU_HZ);
    cdrom_event_log_reset();
    irq.status = 0;

    send_cmd(&cd, 0x15, &irq, &sched); /* SeekL */

    const uint32_t step = PS1_CPU_HZ / 500;
    for (uint64_t e = 0; e < (uint64_t)PS1_CPU_HZ; e += step)
    {
        uint32_t fired = scheduler_step(&sched, step, &irq);
        if (fired & (1u << EVENT_CDROM_IRQ))
            cdrom_on_scheduler_event(&cd, &irq, &sched);
        if (cdrom_event_log_count() >= 1)
            break;
    }

    EXPECT_EQ(cdrom_event_log_count(), 1u);
    EXPECT_EQ(cdrom_event_log_get(0), CDROM_INT3);
    EXPECT_EQ(cdrom_load8(&cd, 1), 0x42u); /* motor on + seeking */

    run_until_quiet(&cd, &irq, &sched, 2ULL * PS1_CPU_HZ);

    EXPECT_EQ(cdrom_event_log_count(), 2u);
    EXPECT_EQ(cdrom_event_log_get(0), CDROM_INT3);
    EXPECT_EQ(cdrom_event_log_get(1), CDROM_INT2);
    EXPECT_EQ((uint32_t)cd.state, (uint32_t)CDROM_STATE_IDLE);
    EXPECT_EQ(cdrom_load8(&cd, 1), 0x02u); /* motor on + idle */
    printf("ok\n");
}

static void test_readn(void)
{
    printf("test_readn ... ");
    Cdrom cd;
    Irq irq;
    Scheduler sched;
    Disc disc;
    setup(&cd, &irq, &sched, &disc);

    push_param(&cd, 0x00, &irq, &sched);
    push_param(&cd, 0x02, &irq, &sched);
    push_param(&cd, 0x00, &irq, &sched);
    send_cmd(&cd, 0x02, &irq, &sched); /* Setloc */
    run_until_quiet(&cd, &irq, &sched, (uint64_t)PS1_CPU_HZ);
    cdrom_event_log_reset();
    irq.status = 0;

    send_cmd(&cd, 0x06, &irq, &sched); /* ReadN */

    /* Run until we have INT3 + at least 3 INT1s */
    const uint32_t step = PS1_CPU_HZ / 500;
    for (uint64_t e = 0; e < 4ULL * PS1_CPU_HZ; e += step)
    {
        uint32_t fired = scheduler_step(&sched, step, &irq);
        if (fired & (1u << EVENT_CDROM_IRQ))
            cdrom_on_scheduler_event(&cd, &irq, &sched);
        /* Count INT1 sectors after the initial INT3 */
        uint32_t int1s = 0;
        for (uint32_t i = 1; i < cdrom_event_log_count(); i++)
            if (cdrom_event_log_get(i) == CDROM_INT1)
                int1s++;
        if (int1s >= 3)
            break;
    }

    EXPECT_TRUE(cdrom_event_log_count() >= 4u); /* INT3 + 3×INT1 */
    EXPECT_EQ(cdrom_event_log_get(0), CDROM_INT3);
    uint32_t int1s = 0;
    for (uint32_t i = 1; i < cdrom_event_log_count(); i++)
        if (cdrom_event_log_get(i) == CDROM_INT1)
            int1s++;
    EXPECT_TRUE(int1s >= 3u);
    EXPECT_EQ((uint32_t)cd.state, (uint32_t)CDROM_STATE_READING);

    /* Pause → IDLE */
    send_cmd(&cd, 0x09, &irq, &sched);
    run_until_quiet(&cd, &irq, &sched, (uint64_t)PS1_CPU_HZ);
    EXPECT_EQ((uint32_t)cd.state, (uint32_t)CDROM_STATE_IDLE);

    uint32_t log_after_pause = cdrom_event_log_count();
    run_until_quiet(&cd, &irq, &sched, (uint64_t)PS1_CPU_HZ);
    EXPECT_EQ(cdrom_event_log_count(), log_after_pause);
    printf("ok\n");
}

static void test_getid_no_disc(void)
{
    printf("test_getid_no_disc ... ");
    Cdrom cd;
    Irq irq;
    Scheduler sched;
    irq_init(&irq);
    scheduler_init(&sched);
    cdrom_init(&cd, NULL);
    cdrom_event_log_reset();

    send_cmd(&cd, 0x1A, &irq, &sched); /* GetID */
    run_until_quiet(&cd, &irq, &sched, (uint64_t)PS1_CPU_HZ);

    EXPECT_EQ(cdrom_event_log_count(), 1u);
    EXPECT_EQ(cdrom_event_log_get(0), CDROM_INT5);
    printf("ok\n");
}

/* Queue must not overflow when a command arrives while ReadN is in flight. */
static void test_overlap_cmd_during_readn(void)
{
    printf("test_overlap_cmd_during_readn ... ");
    Cdrom cd;
    Irq irq;
    Scheduler sched;
    Disc disc;
    setup(&cd, &irq, &sched, &disc);

    push_param(&cd, 0x00, &irq, &sched);
    push_param(&cd, 0x02, &irq, &sched);
    push_param(&cd, 0x00, &irq, &sched);
    send_cmd(&cd, 0x02, &irq, &sched); /* Setloc */
    run_until_quiet(&cd, &irq, &sched, (uint64_t)PS1_CPU_HZ);

    send_cmd(&cd, 0x06, &irq, &sched); /* ReadN */

    /* Fire only the INT3 ack event, stop before any sector */
    const uint32_t step = PS1_CPU_HZ / 500;
    bool got_ack = false;
    for (uint64_t e = 0; e < (uint64_t)PS1_CPU_HZ && !got_ack; e += step)
    {
        uint32_t fired = scheduler_step(&sched, step, &irq);
        if (fired & (1u << EVENT_CDROM_IRQ))
        {
            cdrom_on_scheduler_event(&cd, &irq, &sched);
            if (cdrom_event_log_count() >= 1)
                got_ack = true;
        }
    }

    /* Issue Setloc while a sector event is pending in the queue */
    push_param(&cd, 0x00, &irq, &sched);
    push_param(&cd, 0x04, &irq, &sched);
    push_param(&cd, 0x00, &irq, &sched);
    send_cmd(&cd, 0x02, &irq, &sched); /* Setloc mid-flight */

    EXPECT_TRUE(cd.evq.count <= CDROM_QUEUE_SIZE);
    EXPECT_EQ(cdrom_event_log_get(0), CDROM_INT3); /* ReadN ack was first */
    printf("ok\n");
}

/* =========================================================================
 * Main
 * ========================================================================= */
int main(void)
{
    printf("=== cdrom unit tests ===\n");
    test_getstat();
    test_seekl();
    test_readn();
    test_getid_no_disc();
    test_overlap_cmd_during_readn();
    printf("========================\n");
    printf("pass: %d  fail: %d\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
