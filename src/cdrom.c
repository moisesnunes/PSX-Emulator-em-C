#include "cdrom.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

/* ---- Timing constants (approximate CPU cycles) ---- */
#define CDROM_CYCLES_PER_SECTOR_1X (PS1_CPU_HZ / 75)
#define CDROM_CYCLES_PER_SECTOR_2X (PS1_CPU_HZ / 150)
/* Short delay for command ack responses (~1 ms) */
#define CDROM_ACK_DELAY (PS1_CPU_HZ / 1000)
/* GetID / Init take a bit longer (~5 ms) */
#define CDROM_CMD_DELAY (PS1_CPU_HZ / 200)
/* SeekL completes after a nominal seek time (~20 ms) */
#define CDROM_SEEK_DELAY (PS1_CPU_HZ / 50)

/* ---- FIFO helpers ---- */
static void resp_clear(Cdrom *cd)
{
    cd->resp_len = 0;
    cd->resp_pos = 0;
}
static void resp_push(Cdrom *cd, uint8_t v)
{
    if (cd->resp_len < CDROM_RESP_FIFO_SIZE)
        cd->resp_fifo[cd->resp_len++] = v;
}

static uint8_t param_pop(Cdrom *cd)
{
    if (cd->param_len == 0)
        return 0;
    uint8_t v = cd->param_fifo[0];
    memmove(cd->param_fifo, cd->param_fifo + 1, --cd->param_len);
    return v;
}

/* ---- Stat byte ---- */
static uint8_t make_stat(const Cdrom *cd)
{
    uint8_t s = 0;
    if (cd->disc)
        s |= 0x02; /* motor on */
    if (cd->state == CDROM_STATE_READING)
        s |= 0x20; /* reading  */
    if (cd->state == CDROM_STATE_SEEKING)
        s |= 0x40; /* seeking  */
    return s;
}

/* ---- Event queue helpers ---- */

static void evq_push(CdromEventQueue *q, uint8_t int_type, CdromPhase phase, uint32_t delay)
{
    if (q->count >= CDROM_QUEUE_SIZE)
        return; /* full — shouldn't happen */
    q->slots[q->tail] = (CdromEvent){int_type, phase, delay};
    q->tail = (q->tail + 1) % CDROM_QUEUE_SIZE;
    q->count++;
}

static bool evq_pop(CdromEventQueue *q, CdromEvent *out)
{
    if (q->count == 0)
        return false;
    *out = q->slots[q->head];
    q->head = (q->head + 1) % CDROM_QUEUE_SIZE;
    q->count--;
    return true;
}

static void evq_clear(CdromEventQueue *q)
{
    q->head = q->tail = q->count = 0;
}

/* Enqueue one event and arm the scheduler (only if this is the first
   pending slot — subsequent slots are armed by the event handler). */
static void queue_event(Cdrom *cd, Scheduler *sched,
                        uint8_t int_type, CdromPhase phase, uint32_t delay)
{
    bool was_empty = (cd->evq.count == 0);
    evq_push(&cd->evq, int_type, phase, delay);
    if (was_empty)
        scheduler_schedule(sched, EVENT_CDROM_IRQ, delay);
}

/* ---- Commit the front event's IRQ to the CPU line ---- */
static void commit_irq(Cdrom *cd, Irq *irq, uint8_t int_type)
{
    cd->irq_flag = int_type;
    if (int_type & cd->irq_en)
        irq_assert(irq, IRQ_CDROM);
}

/* ---- Structured event log (for tests) ---- */
#define CDROM_LOG_SIZE 64
static uint8_t cdrom_event_log[CDROM_LOG_SIZE];
static uint32_t cdrom_event_log_len = 0;

void cdrom_event_log_reset(void) { cdrom_event_log_len = 0; }
uint32_t cdrom_event_log_count(void) { return cdrom_event_log_len; }
uint8_t cdrom_event_log_get(uint32_t i)
{
    return (i < cdrom_event_log_len) ? cdrom_event_log[i] : 0;
}

static void log_irq_event(uint8_t int_type)
{
    if (cdrom_event_log_len < CDROM_LOG_SIZE)
        cdrom_event_log[cdrom_event_log_len++] = int_type;
}

/* ---- Command handlers ---- */

/* GetTN (0x13) — first and last track number */
static void cmd_gettn(Cdrom *cd, Scheduler *sched)
{
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    if (!cd->disc)
    {
        resp_push(cd, 0x01);
        resp_push(cd, 0x01);
    }
    else
    {
        resp_push(cd, to_bcd(1));
        resp_push(cd, disc_track_count_bcd(cd->disc));
    }
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "GetTN last=%02X", cd->disc ? disc_track_count_bcd(cd->disc) : 1);
}

/* GetTD (0x14) — track start MSF position */
static void cmd_gettd(Cdrom *cd, Scheduler *sched)
{
    uint8_t track = param_pop(cd);
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    if (cd->disc)
    {
        Msf pos = disc_track_start_msf(cd->disc, track);
        resp_push(cd, pos.m);
        resp_push(cd, pos.s);
    }
    else
    {
        resp_push(cd, 0x00);
        resp_push(cd, 0x00);
    }
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "GetTD track=%02X m=%02X s=%02X",
        track,
        cd->resp_fifo[1], cd->resp_fifo[2]);
}

/* Test (0x19) — CD-ROM controller subcommands.
   The retail BIOS commonly uses 19h,20h to query controller version. */
static void cmd_test(Cdrom *cd, Scheduler *sched)
{
    uint8_t sub = param_pop(cd);
    resp_clear(cd);
    switch (sub)
    {
    case 0x20: /* version: 10 Jan 1997, vC2 (US/EUR PU-18) */
        resp_push(cd, 0x97);
        resp_push(cd, 0x01);
        resp_push(cd, 0x10);
        resp_push(cd, 0xC2);
        queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
        break;
    case 0x21: /* switches: head not at POS0, door closed */
        resp_push(cd, 0x00);
        queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
        break;
    case 0x22:
    { /* region string */
        static const uint8_t region[] = {'f', 'o', 'r', ' ', 'U', '/', 'C'};
        for (size_t i = 0; i < sizeof(region); i++)
            resp_push(cd, region[i]);
        queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
        break;
    }
    case 0x04: /* start/read SCEx string */
        resp_push(cd, make_stat(cd));
        queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
        break;
    case 0x05: /* SCEx counters: licensed disc */
        resp_push(cd, cd->disc ? 0x01 : 0x00);
        resp_push(cd, cd->disc ? 0x01 : 0x00);
        queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
        break;
    default:
        resp_push(cd, 0x11);
        resp_push(cd, 0x10);
        queue_event(cd, sched, CDROM_INT5, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
        break;
    }
    LOG(LOG_CDROM, "Test sub=0x%02X", sub);
}

/* Getstat (0x01) */
static void cmd_getstat(Cdrom *cd, Scheduler *sched)
{
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "Getstat stat=0x%02X", cd->resp_fifo[0]);
}

/* Init (0x0A) */
static void cmd_init(Cdrom *cd, Scheduler *sched)
{
    evq_clear(&cd->evq);
    cd->double_speed = false;
    cd->xa_adpcm_en = false;
    cd->report_mode = false;
    cd->auto_pause = false;
    cd->mode = 0;
    cd->state = CDROM_STATE_IDLE;
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_INIT_ACK, CDROM_CMD_DELAY);
    LOG(LOG_CDROM, "Init");
}

/* Setmode (0x0E) */
static void cmd_setmode(Cdrom *cd, Scheduler *sched)
{
    cd->mode = param_pop(cd);
    cd->double_speed = (cd->mode >> 7) & 1;
    cd->xa_adpcm_en = (cd->mode >> 6) & 1;
    cd->report_mode = (cd->mode >> 2) & 1;
    cd->auto_pause = (cd->mode >> 1) & 1;
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "Setmode mode=0x%02X", cd->mode);
}

/* GetID (0x1A) */
static void cmd_getid(Cdrom *cd, Scheduler *sched)
{
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_GETID_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "GetID disc=%s", cd->disc ? "present" : "absent");
}

/* Setloc (0x02) */
static void cmd_setloc(Cdrom *cd, Scheduler *sched)
{
    cd->seek_target.m = param_pop(cd);
    cd->seek_target.s = param_pop(cd);
    cd->seek_target.f = param_pop(cd);
    cd->seek_pending = true;
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "Setloc %02X:%02X:%02X",
        cd->seek_target.m, cd->seek_target.s, cd->seek_target.f);
}

/* SeekL (0x15):
   event 1 (SEEK_ACK phase)  → INT3 + seeking stat
   event 2 (SEEK_DONE phase) → INT2 + idle stat */
static void cmd_seek(Cdrom *cd, Scheduler *sched, const char *name)
{
    if (cd->seek_pending)
    {
        cd->cur_lba = msf_to_lba(cd->seek_target);
        cd->seek_pending = false;
    }
    cd->state = CDROM_STATE_SEEKING;
    resp_clear(cd);
    resp_push(cd, make_stat(cd)); /* stat with SEEKING bit set */
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_SEEK_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "%s LBA=%u", name, cd->cur_lba);
}

static void cmd_seekl(Cdrom *cd, Scheduler *sched)
{
    cmd_seek(cd, sched, "SeekL");
}

static void cmd_seekp(Cdrom *cd, Scheduler *sched)
{
    cmd_seek(cd, sched, "SeekP");
}

/* ReadN (0x06) / ReadS (0x1B):
   event 1 (ACK phase)       → INT3 + reading stat
   event 2 (READ_DATA phase) → INT1 + sector data
   ReadS skips sub-header checking; behaviour is identical for Mode 1 data. */
static void cmd_read(Cdrom *cd, uint8_t cmd, Scheduler *sched)
{
    if (cd->seek_pending)
    {
        cd->cur_lba = msf_to_lba(cd->seek_target);
        cd->seek_pending = false;
    }
    cd->state = CDROM_STATE_READING;
    resp_clear(cd);
    resp_push(cd, make_stat(cd)); /* stat with READING bit set */
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_READ_DATA, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "Read%c LBA=%u", cmd == 0x06 ? 'N' : 'S', cd->cur_lba);
}

/* Pause (0x09) */
static void cmd_pause(Cdrom *cd, Scheduler *sched)
{
    cd->state = CDROM_STATE_IDLE;
    evq_clear(&cd->evq);
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_PAUSE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "Pause");
}

/* Stop (0x08) */
static void cmd_stop(Cdrom *cd, Scheduler *sched)
{
    cd->state = CDROM_STATE_IDLE;
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "Stop");
}

/* Mute (0x0B) / Demute (0x0C).
   Audio output is not implemented yet, but BIOS/game code expects the command
   to acknowledge successfully while configuring CD-DA playback state. */
static void cmd_audio_mute(Cdrom *cd, Scheduler *sched, bool muted)
{
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "%s", muted ? "Mute" : "Demute");
}

/* ReadTOC (0x1E) — reload table of contents.
   Minimal single-track images already have TOC data in Disc. */
static void cmd_readtoc(Cdrom *cd, Scheduler *sched)
{
    cd->state = CDROM_STATE_SEEKING;
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_READTOC_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "ReadTOC");
}

/* ---- Dispatch ---- */
static void execute_command(Cdrom *cd, uint8_t cmd, Scheduler *sched)
{
    switch (cmd)
    {
    case 0x01:
        cmd_getstat(cd, sched);
        break;
    case 0x02:
        cmd_setloc(cd, sched);
        break;
    case 0x06:
    case 0x1B:
        cmd_read(cd, cmd, sched);
        break;
    case 0x08:
        cmd_stop(cd, sched);
        break;
    case 0x09:
        cmd_pause(cd, sched);
        break;
    case 0x0A:
        cmd_init(cd, sched);
        break;
    case 0x0B:
        cmd_audio_mute(cd, sched, true);
        break;
    case 0x0C:
        cmd_audio_mute(cd, sched, false);
        break;
    case 0x0E:
        cmd_setmode(cd, sched);
        break;
    case 0x13:
        cmd_gettn(cd, sched);
        break;
    case 0x14:
        cmd_gettd(cd, sched);
        break;
    case 0x15:
        cmd_seekl(cd, sched);
        break;
    case 0x16:
        cmd_seekp(cd, sched);
        break;
    case 0x19:
        cmd_test(cd, sched);
        break;
    case 0x1A:
        cmd_getid(cd, sched);
        break;
    case 0x1E:
        cmd_readtoc(cd, sched);
        break;
    default:
        resp_clear(cd);
        resp_push(cd, 0x11);
        resp_push(cd, 0x40);
        queue_event(cd, sched, CDROM_INT5, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
        LOG(LOG_CDROM, "Unknown command 0x%02X", cmd);
        break;
    }
}

static void execute_or_defer_command(Cdrom *cd, uint8_t cmd, Scheduler *sched)
{
    if (cd->irq_flag != 0)
    {
        cd->pending_cmd_valid = true;
        cd->pending_cmd = cmd;
        cd->pending_param_len = cd->param_len;
        memcpy(cd->pending_param_fifo, cd->param_fifo, cd->param_len);
        cd->param_len = 0;
        LOG(LOG_CDROM, "Command 0x%02X deferred while INT%d pending", cmd, cd->irq_flag);
        return;
    }

    execute_command(cd, cmd, sched);
}

static void execute_pending_command(Cdrom *cd, Scheduler *sched)
{
    if (!cd->pending_cmd_valid || cd->irq_flag != 0)
        return;

    uint8_t cmd = cd->pending_cmd;
    cd->pending_cmd_valid = false;
    cd->param_len = cd->pending_param_len;
    memcpy(cd->param_fifo, cd->pending_param_fifo, cd->pending_param_len);
    cd->pending_param_len = 0;

    LOG(LOG_CDROM, "Command 0x%02X released after IRQ ack", cmd);
    execute_command(cd, cmd, sched);
}

/* ---- Public API ---- */

void cdrom_init(Cdrom *cd, Disc *disc)
{
    memset(cd, 0, sizeof(*cd));
    cd->disc = disc;
    cd->state = CDROM_STATE_IDLE;
    cd->irq_en = 0x1F;
    evq_clear(&cd->evq);
}

uint8_t cdrom_load8(Cdrom *cd, uint32_t offset)
{
    switch (offset)
    {
    case 0:
    {
        /* Status register (not index-sensitive) */
        uint8_t stat = cd->index & 0x03;
        if (cd->param_len == 0)
            stat |= 0x08; /* PRMEMPT */
        if (cd->param_len < CDROM_PARAM_FIFO_SIZE)
            stat |= 0x10; /* PRMWRDY */
        if (cd->resp_pos < cd->resp_len)
            stat |= 0x20; /* RSLRRDY */
        if (cd->data_pos < cd->data_len)
            stat |= 0x40; /* DRQSTS  */
        return stat;
    }
    case 1:
        /* Response FIFO */
        if (cd->resp_pos < cd->resp_len)
            return cd->resp_fifo[cd->resp_pos++];
        return 0;
    case 2:
        /* Data FIFO (8-bit) */
        if (cd->data_pos < cd->data_len)
            return cd->data_fifo[cd->data_pos++];
        return 0;
    case 3:
        /* PS1 layout: index 0 → irq_en, index 1 → irq_flag */
        switch (cd->index & 1)
        {
        case 0:
            return cd->irq_en | 0xE0;
        case 1:
            return cd->irq_flag | 0xE0;
        }
        break;
    }
    return 0;
}

void cdrom_store8(Cdrom *cd, uint32_t offset, uint8_t val,
                  Irq *irq, Scheduler *sched)
{
    switch (offset)
    {
    case 0:
        cd->index = val & 0x03;
        return;
    case 1:
        switch (cd->index)
        {
        case 0:
            execute_or_defer_command(cd, val, sched);
            return;
        default:
            return; /* sound map / other — ignore */
        }
    case 2:
        switch (cd->index)
        {
        case 0:
            if (cd->param_len < CDROM_PARAM_FIFO_SIZE)
                cd->param_fifo[cd->param_len++] = val;
            return;
        case 1:
            cd->irq_en = val & 0x1F;
            return;
        default:
            return;
        }
    case 3:
        switch (cd->index)
        {
        case 0:
            /* Request register — bit 7: want data into FIFO */
            (void)irq; /* no side-effect needed here */
            return;
        case 1:
            /* Interrupt flag acknowledge: writing 1 clears the bit */
            cd->irq_flag &= ~(val & 0x1F);
            if (val & 0x1F)
                resp_clear(cd);
            if (cd->irq_flag == 0)
                irq_deassert(irq, IRQ_CDROM);
            if (val & 0x40)
                cd->param_len = 0; /* reset param FIFO */
            execute_pending_command(cd, sched);
            return;
        default:
            return;
        }
    }
    LOG(LOG_CDROM, "cdrom_store8 unhandled offset=%u idx=%u val=0x%02X",
        offset, cd->index, val);
}

/* ---- DMA channel 3 interface ---- */

bool cdrom_dma_ready(const Cdrom *cd)
{
    return cd->data_pos < cd->data_len;
}

uint32_t cdrom_dma_read(Cdrom *cd)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
    {
        uint8_t b = 0;
        if (cd->data_pos < cd->data_len)
            b = cd->data_fifo[cd->data_pos++];
        v |= (uint32_t)b << (i * 8);
    }
    return v;
}

/* ---- deliver_sector: load one sector into data FIFO, queue INT1 ---- */
static void deliver_sector(Cdrom *cd, Irq *irq, Scheduler *sched)
{
    (void)sched;

    if (cd->state != CDROM_STATE_READING)
    {
        LOG(LOG_CDROM, "ReadN sector ignored: state=%d", cd->state);
        return;
    }

    if (!cd->disc)
    {
        cd->state = CDROM_STATE_IDLE;
        resp_clear(cd);
        resp_push(cd, 0x09); /* error stat */
        resp_push(cd, 0x80); /* unreadable */
        evq_push(&cd->evq, CDROM_INT5, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
        LOG(LOG_CDROM, "ReadN sector: no disc");
        return;
    }

    uint8_t raw[DISC_SECTOR_SIZE];
    disc_read_sector(cd->disc, cd->cur_lba, raw);
    cd->cur_lba++;

    bool raw_mode = (cd->mode & 0x20) != 0;
    if (raw_mode)
    {
        cd->data_len = 2340;
        cd->data_pos = 0;
        memcpy(cd->data_fifo, raw + 12, 2340);
    }
    else
    {
        cd->data_len = DISC_DATA_SIZE;
        cd->data_pos = 0;
        memcpy(cd->data_fifo, raw + DISC_DATA_OFFSET, DISC_DATA_SIZE);
    }

    resp_clear(cd);
    resp_push(cd, make_stat(cd));

    /* Deliver INT1 immediately, then enqueue the next sector event. */
    commit_irq(cd, irq, CDROM_INT1);
    log_irq_event(CDROM_INT1);

    uint32_t period = cd->double_speed ? CDROM_CYCLES_PER_SECTOR_2X
                                       : CDROM_CYCLES_PER_SECTOR_1X;
    /* Enqueue next sector; the scheduler handler rearms after this phase. */
    evq_push(&cd->evq, CDROM_INT1, CDROM_PHASE_SECTOR, period);

    LOG(LOG_CDROM, "Sector LBA=%u delivered (%s)", cd->cur_lba - 1,
        raw_mode ? "raw" : "data");
}

/* ---- Scheduler event handler ----
   Dequeues the front event, executes its phase, then re-arms the scheduler
   for the next queued event (if any).  Commands never deliver IRQs directly;
   they only enqueue slots here. */
void cdrom_on_scheduler_event(Cdrom *cd, Irq *irq, Scheduler *sched)
{
    CdromEvent ev;
    if (!evq_pop(&cd->evq, &ev))
        return; /* spurious fire */

    switch (ev.phase)
    {

    case CDROM_PHASE_ACK:
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        break;

    case CDROM_PHASE_INIT_ACK:
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        evq_push(&cd->evq, CDROM_INT2, CDROM_PHASE_INIT_DONE, CDROM_CMD_DELAY);
        break;

    case CDROM_PHASE_INIT_DONE:
        cd->state = CDROM_STATE_IDLE;
        resp_clear(cd);
        resp_push(cd, make_stat(cd));
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        LOG(LOG_CDROM, "Init complete");
        break;

    case CDROM_PHASE_SEEK_ACK:
        /* Deliver INT3 ack (seeking stat already in resp FIFO). */
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        evq_push(&cd->evq, CDROM_INT2, CDROM_PHASE_SEEK_DONE, CDROM_SEEK_DELAY);
        LOG(LOG_CDROM, "SeekL ack INT3, queued INT2 LBA=%u", cd->cur_lba);
        break;

    case CDROM_PHASE_SEEK_DONE:
        /* Prepare and deliver INT2 seek-complete. */
        cd->state = CDROM_STATE_IDLE;
        resp_clear(cd);
        resp_push(cd, make_stat(cd));
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        LOG(LOG_CDROM, "SeekL complete INT2 LBA=%u", cd->cur_lba);
        break;

    case CDROM_PHASE_GETID_ACK:
        /* Deliver INT3 ack (stat already in resp FIFO), then queue ID result. */
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        evq_push(&cd->evq, cd->disc ? CDROM_INT2 : CDROM_INT5,
                 CDROM_PHASE_GETID_DONE, CDROM_CMD_DELAY);
        break;

    case CDROM_PHASE_GETID_DONE:
        /* Prepare and deliver the second GetID response. */
        resp_clear(cd);
        if (!cd->disc)
        {
            resp_push(cd, 0x08); /* error stat: motor off */
            resp_push(cd, 0x40); /* no disc */
            resp_push(cd, 0x00);
            resp_push(cd, 0x00);
            resp_push(cd, 0x00);
            resp_push(cd, 0x00);
            resp_push(cd, 0x00);
            resp_push(cd, 0x00);
        }
        else
        {
            resp_push(cd, make_stat(cd));
            resp_push(cd, 0x00); /* licensed */
            resp_push(cd, 0x20); /* mode 2 data */
            resp_push(cd, 0x00);
            resp_push(cd, 'S');
            resp_push(cd, 'C');
            resp_push(cd, 'E');
            resp_push(cd, 'A');
        }
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        break;

    case CDROM_PHASE_READTOC_ACK:
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        evq_push(&cd->evq, CDROM_INT2, CDROM_PHASE_READTOC_DONE, CDROM_SEEK_DELAY);
        break;

    case CDROM_PHASE_READTOC_DONE:
        cd->state = CDROM_STATE_IDLE;
        resp_clear(cd);
        resp_push(cd, make_stat(cd));
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        LOG(LOG_CDROM, "ReadTOC complete");
        break;

    case CDROM_PHASE_PAUSE_ACK:
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        evq_push(&cd->evq, CDROM_INT2, CDROM_PHASE_PAUSE_DONE, CDROM_CMD_DELAY);
        break;

    case CDROM_PHASE_PAUSE_DONE:
        cd->state = CDROM_STATE_IDLE;
        resp_clear(cd);
        resp_push(cd, make_stat(cd));
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        LOG(LOG_CDROM, "Pause complete");
        break;

    case CDROM_PHASE_READ_DATA:
        /* Deliver INT3 ack (reading stat already in resp FIFO). */
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        /* Enqueue first sector event. */
        {
            uint32_t period = cd->double_speed ? CDROM_CYCLES_PER_SECTOR_2X
                                               : CDROM_CYCLES_PER_SECTOR_1X;
            evq_push(&cd->evq, CDROM_INT1, CDROM_PHASE_SECTOR, period);
        }
        LOG(LOG_CDROM, "ReadN ack INT3, queued sector LBA=%u", cd->cur_lba);
        break;

    case CDROM_PHASE_SECTOR:
        deliver_sector(cd, irq, sched);
        break;
    }

    /* Re-arm scheduler for the next queued event, if any. */
    if (cd->evq.count > 0)
        scheduler_schedule(sched, EVENT_CDROM_IRQ,
                           cd->evq.slots[cd->evq.head].delay);
}
