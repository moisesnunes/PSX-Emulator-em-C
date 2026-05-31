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
    cd->double_speed = false;
    cd->xa_adpcm_en = false;
    cd->report_mode = false;
    cd->auto_pause = false;
    cd->mode = 0;
    cd->state = CDROM_STATE_IDLE;
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_CMD_DELAY);
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
        queue_event(cd, sched, CDROM_INT5, CDROM_PHASE_ACK, CDROM_CMD_DELAY);
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
        resp_push(cd, 'I');
        queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_CMD_DELAY);
    }
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
static void cmd_seekl(Cdrom *cd, Scheduler *sched)
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
    LOG(LOG_CDROM, "SeekL LBA=%u", cd->cur_lba);
}

/* ReadN (0x06):
   event 1 (ACK phase)       → INT3 + reading stat
   event 2 (READ_DATA phase) → INT1 + sector data  */
static void cmd_readn(Cdrom *cd, Scheduler *sched)
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
    LOG(LOG_CDROM, "ReadN LBA=%u", cd->cur_lba);
}

/* Pause (0x09) */
static void cmd_pause(Cdrom *cd, Scheduler *sched)
{
    cd->state = CDROM_STATE_IDLE;
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
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
        cmd_readn(cd, sched);
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
    case 0x0E:
        cmd_setmode(cd, sched);
        break;
    case 0x15:
        cmd_seekl(cd, sched);
        break;
    case 0x1A:
        cmd_getid(cd, sched);
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
            execute_command(cd, val, sched);
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
            if (val & 0x40)
                cd->param_len = 0; /* reset param FIFO */
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
