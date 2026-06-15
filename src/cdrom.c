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
#define CDROM_MIN_SEEK_DELAY 20000u
#define CDROM_LONG_SEEK_SECTORS 7200u

static uint32_t seek_delay_for_lba(const Cdrom *cd, uint32_t target_lba);

#define XA_SECTOR_SUBHEADER_OFFSET 16u
#define XA_SECTOR_DATA_OFFSET 24u
#define XA_CHUNKS_PER_SECTOR 18u
#define XA_CHUNK_SIZE 128u
#define XA_WORDS_PER_CHUNK 28u

static int16_t clamp16_cdrom(int32_t v)
{
    if (v < -0x8000)
        return -0x8000;
    if (v > 0x7FFF)
        return 0x7FFF;
    return (int16_t)v;
}

static int16_t le_i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

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
    if (cd->playing_cdda)
        s |= 0x80; /* playing CD-DA */
    return s;
}

static void reset_xa_stream(Cdrom *cd)
{
    cd->xa_current_set = false;
    cd->xa_current_file = 0;
    cd->xa_current_channel = 0;
    memset(cd->xa_last_samples, 0, sizeof(cd->xa_last_samples));
    cd->xa_resample_accum = 0;
    if (cd->spu)
        spu_clear_cd_audio(cd->spu);
}

static int16_t cd_apply_volume(int16_t sample, uint8_t volume)
{
    return clamp16_cdrom(((int32_t)sample * (int32_t)volume) >> 7);
}

static void cd_push_audio_frame(Cdrom *cd, int16_t left, int16_t right)
{
    if (!cd->spu || cd->muted || cd->adpcm_muted)
        return;

    int32_t out_l = cd_apply_volume(left, cd->cd_audio_volume[0][0]) +
                    cd_apply_volume(right, cd->cd_audio_volume[1][0]);
    int32_t out_r = cd_apply_volume(left, cd->cd_audio_volume[0][1]) +
                    cd_apply_volume(right, cd->cd_audio_volume[1][1]);
    spu_push_cd_audio_frame(cd->spu, clamp16_cdrom(out_l), clamp16_cdrom(out_r));
}

static void cd_push_resampled_frame(Cdrom *cd, int16_t left, int16_t right, uint32_t source_rate)
{
    cd->xa_resample_accum += 44100u;
    while (cd->xa_resample_accum >= source_rate)
    {
        cd->xa_resample_accum -= source_rate;
        cd_push_audio_frame(cd, left, right);
    }
}

static bool xa_sector_is_audio(const uint8_t raw[DISC_SECTOR_SIZE])
{
    uint8_t submode = raw[XA_SECTOR_SUBHEADER_OFFSET + 2u];
    uint8_t coding = raw[XA_SECTOR_SUBHEADER_OFFSET + 3u];
    return (submode & 0x04u) != 0 && (submode & 0x20u) != 0 &&
           (coding & 0x03u) == 0;
}

static void decode_xa_sector(Cdrom *cd, const uint8_t raw[DISC_SECTOR_SIZE])
{
    if (!cd->xa_adpcm_en || !xa_sector_is_audio(raw))
        return;

    uint8_t file = raw[XA_SECTOR_SUBHEADER_OFFSET + 0u];
    uint8_t channel = raw[XA_SECTOR_SUBHEADER_OFFSET + 1u];
    uint8_t submode = raw[XA_SECTOR_SUBHEADER_OFFSET + 2u];
    uint8_t coding = raw[XA_SECTOR_SUBHEADER_OFFSET + 3u];

    if (cd->xa_filter_en &&
        (file != cd->xa_filter_file || channel != cd->xa_filter_channel))
        return;
    if (channel == 0xFFu && (!cd->xa_filter_en || cd->xa_filter_channel != 0xFFu))
        return;

    if (!cd->xa_current_set)
    {
        cd->xa_current_set = true;
        cd->xa_current_file = file;
        cd->xa_current_channel = channel;
    }
    else if (file != cd->xa_current_file || channel != cd->xa_current_channel)
    {
        return;
    }

    bool stereo = (coding & 0x01u) != 0;
    bool half_rate = (coding & 0x04u) != 0;
    bool eight_bit = (coding & 0x10u) != 0;
    uint32_t source_rate = half_rate ? 18900u : 37800u;
    uint32_t blocks = eight_bit ? 4u : 8u;
    static const int8_t filter_pos[16] = {0, 60, 115, 98};
    static const int8_t filter_neg[16] = {0, 0, -52, -55};

    const uint8_t *chunk = raw + XA_SECTOR_DATA_OFFSET;
    for (uint32_t ci = 0; ci < XA_CHUNKS_PER_SECTOR; ci++, chunk += XA_CHUNK_SIZE)
    {
        const uint8_t *headers = chunk + 4u;
        const uint8_t *words = chunk + 16u;
        int16_t decoded[8][XA_WORDS_PER_CHUNK];

        for (uint32_t block = 0; block < blocks; block++)
        {
            uint8_t header = headers[block];
            uint8_t shift = header & 0x0Fu;
            uint8_t filter = (header >> 4) & 0x0Fu;
            if (shift > 12)
                shift = 9;
            int32_t fp = filter_pos[filter];
            int32_t fn = filter_neg[filter];
            int32_t *prev = stereo ? &cd->xa_last_samples[(block & 1u) * 2u]
                                   : &cd->xa_last_samples[0];

            for (uint32_t word = 0; word < XA_WORDS_PER_CHUNK; word++)
            {
                uint32_t word_data = (uint32_t)words[word * 4u + 0u] |
                                     ((uint32_t)words[word * 4u + 1u] << 8) |
                                     ((uint32_t)words[word * 4u + 2u] << 16) |
                                     ((uint32_t)words[word * 4u + 3u] << 24);
                int32_t sample;
                if (eight_bit)
                {
                    uint8_t b = (uint8_t)((word_data >> (block * 8u)) & 0xFFu);
                    sample = ((int16_t)((uint16_t)b << 8)) >> shift;
                }
                else
                {
                    uint8_t nibble = (uint8_t)((word_data >> (block * 4u)) & 0x0Fu);
                    sample = ((int16_t)((uint16_t)nibble << 12)) >> shift;
                }
                sample += (prev[0] * fp) >> 6;
                sample += (prev[1] * fn) >> 6;
                sample = clamp16_cdrom(sample);
                prev[1] = prev[0];
                prev[0] = sample;
                decoded[block][word] = (int16_t)sample;
            }
        }

        if (stereo)
        {
            uint32_t pairs = blocks / 2u;
            for (uint32_t pair = 0; pair < pairs; pair++)
                for (uint32_t word = 0; word < XA_WORDS_PER_CHUNK; word++)
                    cd_push_resampled_frame(cd, decoded[pair * 2u][word],
                                            decoded[pair * 2u + 1u][word], source_rate);
        }
        else
        {
            for (uint32_t block = 0; block < blocks; block++)
                for (uint32_t word = 0; word < XA_WORDS_PER_CHUNK; word++)
                    cd_push_resampled_frame(cd, decoded[block][word],
                                            decoded[block][word], source_rate);
        }
    }

    if (submode & 0x80u)
        reset_xa_stream(cd);
}

static void process_cdda_sector(Cdrom *cd, const uint8_t raw[DISC_SECTOR_SIZE])
{
    if (!cd->cdda_en || cd->muted)
        return;
    for (uint32_t i = 0; i + 3u < DISC_SECTOR_SIZE; i += 4u)
        cd_push_audio_frame(cd, le_i16(raw + i), le_i16(raw + i + 2u));
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

/* GetlocL (0x10) — current position as absolute MSF + sub-header bytes.
   Returns 8 bytes: amm, ass, asect, amode, file, channel, submode, coding. */
static void cmd_getlocl(Cdrom *cd, Scheduler *sched)
{
    resp_clear(cd);
    Msf pos = msf_from_lba(cd->cur_lba);
    resp_push(cd, pos.m); /* amm */
    resp_push(cd, pos.s); /* ass */
    resp_push(cd, pos.f); /* asect */
    resp_push(cd, 0x02);  /* amode — Mode 2 */
    resp_push(cd, cd->xa_current_set ? cd->xa_current_file : 0x01);
    resp_push(cd, cd->xa_current_set ? cd->xa_current_channel : 0x00);
    resp_push(cd, 0x08); /* submode: data */
    resp_push(cd, 0x00); /* coding info */
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "GetlocL LBA=%u %02X:%02X:%02X", cd->cur_lba, pos.m, pos.s, pos.f);
}

/* GetlocP (0x11) — current position in TOC format.
   Returns 8 bytes: track, index, mm, ss, sect, amm, ass, asect. */
static void cmd_getlocp(Cdrom *cd, Scheduler *sched)
{
    resp_clear(cd);
    Msf abs_msf = msf_from_lba(cd->cur_lba);

    /* Find which track contains cur_lba. */
    uint8_t track_num = 1;
    uint32_t track_start = 0;
    if (cd->disc)
    {
        uint8_t tc = cd->disc->track_count;
        for (uint8_t t = 1; t <= tc; t++)
        {
            uint32_t ts = cd->disc->tracks[t].start_lba;
            if (ts <= cd->cur_lba)
            {
                track_num = t;
                track_start = ts;
            }
        }
    }

    uint32_t rel_lba = cd->cur_lba >= track_start ? cd->cur_lba - track_start : 0;
    Msf rel_msf = {
        to_bcd((uint8_t)(rel_lba / 75 / 60)),
        to_bcd((uint8_t)((rel_lba / 75) % 60)),
        to_bcd((uint8_t)(rel_lba % 75)),
    };

    resp_push(cd, to_bcd(track_num)); /* track (BCD) */
    resp_push(cd, 0x01);              /* index 1 (BCD) */
    resp_push(cd, rel_msf.m);
    resp_push(cd, rel_msf.s);
    resp_push(cd, rel_msf.f);
    resp_push(cd, abs_msf.m);
    resp_push(cd, abs_msf.s);
    resp_push(cd, abs_msf.f);
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "GetlocP LBA=%u track=%02X abs=%02X:%02X:%02X rel=%02X:%02X:%02X",
        cd->cur_lba, to_bcd(track_num),
        abs_msf.m, abs_msf.s, abs_msf.f,
        rel_msf.m, rel_msf.s, rel_msf.f);
}

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
    cd->xa_filter_en = false;
    cd->report_mode = false;
    cd->auto_pause = false;
    cd->cdda_en = false;
    cd->playing_cdda = false;
    cd->muted = false;
    cd->adpcm_muted = false;
    cd->mode = 0;
    cd->state = CDROM_STATE_IDLE;
    reset_xa_stream(cd);
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_INIT_ACK, CDROM_CMD_DELAY);
    LOG(LOG_CDROM, "Init");
}

static void cmd_play(Cdrom *cd, Scheduler *sched)
{
    if (cd->seek_pending)
    {
        uint32_t target_lba = msf_to_lba(cd->seek_target);
        cd->read_seek_delay = seek_delay_for_lba(cd, target_lba);
        cd->cur_lba = target_lba;
        cd->seek_pending = false;
    }
    else
    {
        cd->read_seek_delay = 0;
    }
    cd->state = CDROM_STATE_READING;
    cd->playing_cdda = true;
    cd->cdda_en = true;
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_READ_DATA, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "Play LBA=%u", cd->cur_lba);
}

/* Setmode (0x0E) */
static void cmd_setmode(Cdrom *cd, Scheduler *sched)
{
    cd->mode = param_pop(cd);
    cd->cdda_en = (cd->mode >> 0) & 1;
    cd->double_speed = (cd->mode >> 7) & 1;
    cd->xa_adpcm_en = (cd->mode >> 6) & 1;
    cd->xa_filter_en = (cd->mode >> 3) & 1;
    cd->report_mode = (cd->mode >> 2) & 1;
    cd->auto_pause = (cd->mode >> 1) & 1;
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "Setmode mode=0x%02X", cd->mode);
}

static void cmd_setfilter(Cdrom *cd, Scheduler *sched)
{
    cd->xa_filter_file = param_pop(cd);
    cd->xa_filter_channel = param_pop(cd);
    cd->xa_current_set = false;
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "Setfilter file=%u channel=%u", cd->xa_filter_file, cd->xa_filter_channel);
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

static uint32_t seek_delay_for_lba(const Cdrom *cd, uint32_t target_lba)
{
    uint32_t distance = cd->cur_lba > target_lba
                            ? cd->cur_lba - target_lba
                            : target_lba - cd->cur_lba;
    uint64_t delay = (uint64_t)distance *
                     (CDROM_CYCLES_PER_SECTOR_1X / 2000u);

    if (distance >= CDROM_LONG_SEEK_SECTORS)
        delay = PS1_CPU_HZ / 7u + (uint64_t)distance * 64u;
    else if (delay < CDROM_MIN_SEEK_DELAY)
        delay = CDROM_MIN_SEEK_DELAY;

    uint32_t rotation = cd->double_speed ? CDROM_CYCLES_PER_SECTOR_2X
                                         : CDROM_CYCLES_PER_SECTOR_1X;
    delay += cd->state == CDROM_STATE_READING ? rotation / 2u : rotation;
    return delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
}

/* SeekL (0x15):
   event 1 (SEEK_ACK phase)  → INT3 + seeking stat
   event 2 (SEEK_DONE phase) → INT2 + idle stat */
static void cmd_seek(Cdrom *cd, Scheduler *sched, const char *name)
{
    cd->seek_lba = cd->seek_pending ? msf_to_lba(cd->seek_target)
                                    : cd->cur_lba;
    cd->seek_delay = seek_delay_for_lba(cd, cd->seek_lba);
    cd->seek_pending = false;
    cd->state = CDROM_STATE_SEEKING;
    resp_clear(cd);
    resp_push(cd, make_stat(cd)); /* stat with SEEKING bit set */
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_SEEK_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "%s LBA=%u->%u delay=%u", name, cd->cur_lba,
        cd->seek_lba, cd->seek_delay);
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
        uint32_t target_lba = msf_to_lba(cd->seek_target);
        cd->read_seek_delay = seek_delay_for_lba(cd, target_lba);
        cd->cur_lba = target_lba;
        cd->seek_pending = false;
    }
    else
    {
        cd->read_seek_delay = 0;
    }
    cd->state = CDROM_STATE_READING;
    cd->playing_cdda = false;
    resp_clear(cd);
    resp_push(cd, make_stat(cd)); /* stat with READING bit set */
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_READ_DATA, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "Read%c LBA=%u", cmd == 0x06 ? 'N' : 'S', cd->cur_lba);
}

/* Pause (0x09) */
static void cmd_pause(Cdrom *cd, Scheduler *sched)
{
    cd->state = CDROM_STATE_IDLE;
    cd->playing_cdda = false;
    evq_clear(&cd->evq);
    if (cd->spu)
        spu_clear_cd_audio(cd->spu);
    resp_clear(cd);
    resp_push(cd, make_stat(cd));
    queue_event(cd, sched, CDROM_INT3, CDROM_PHASE_PAUSE_ACK, CDROM_ACK_DELAY);
    LOG(LOG_CDROM, "Pause");
}

/* Stop (0x08) */
static void cmd_stop(Cdrom *cd, Scheduler *sched)
{
    cd->state = CDROM_STATE_IDLE;
    cd->playing_cdda = false;
    if (cd->spu)
        spu_clear_cd_audio(cd->spu);
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
    cd->muted = muted;
    if (muted && cd->spu)
        spu_clear_cd_audio(cd->spu);
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
    case 0x10:
        cmd_getlocl(cd, sched);
        break;
    case 0x11:
        cmd_getlocp(cd, sched);
        break;
    case 0x02:
        cmd_setloc(cd, sched);
        break;
    case 0x03:
        cmd_play(cd, sched);
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
    case 0x0D:
        cmd_setfilter(cd, sched);
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
    cd->cd_audio_volume[0][0] = 0x80;
    cd->cd_audio_volume[1][1] = 0x80;
    cd->next_cd_audio_volume[0][0] = 0x80;
    cd->next_cd_audio_volume[1][1] = 0x80;
    evq_clear(&cd->evq);
}

void cdrom_set_spu(Cdrom *cd, Spu *spu)
{
    cd->spu = spu;
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
        case 3:
            cd->next_cd_audio_volume[1][1] = val;
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
        case 2:
            cd->next_cd_audio_volume[0][0] = val;
            return;
        case 3:
            cd->next_cd_audio_volume[1][0] = val;
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
        case 2:
            cd->next_cd_audio_volume[0][1] = val;
            return;
        case 3:
            cd->adpcm_muted = (val & 0x01u) != 0;
            if (val & 0x20u)
                memcpy(cd->cd_audio_volume, cd->next_cd_audio_volume,
                       sizeof(cd->cd_audio_volume));
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

    if (cd->irq_flag != 0 || cd->pending_cmd_valid)
    {
        evq_push(&cd->evq, CDROM_INT1, CDROM_PHASE_SECTOR, CDROM_ACK_DELAY);
        LOG(LOG_CDROM, "Sector deferred while INT%d pending", cd->irq_flag);
        return;
    }

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
    uint32_t sector_lba = cd->cur_lba;
    uint32_t data_offset = disc_data_offset_for_lba(cd->disc, sector_lba);
    disc_read_sector(cd->disc, sector_lba, raw);
    cd->cur_lba++;
    process_cdda_sector(cd, raw);
    decode_xa_sector(cd, raw);

    bool raw_mode = (cd->mode & 0x20) != 0;
    if (cd->playing_cdda)
    {
        cd->data_len = 0;
        cd->data_pos = 0;
    }
    else if (raw_mode)
    {
        cd->data_len = 2340;
        cd->data_pos = 0;
        memcpy(cd->data_fifo, raw + 12, 2340);
    }
    else
    {
        cd->data_len = DISC_DATA_SIZE;
        cd->data_pos = 0;
        memcpy(cd->data_fifo, raw + data_offset, DISC_DATA_SIZE);
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
        evq_push(&cd->evq, CDROM_INT2, CDROM_PHASE_SEEK_DONE, cd->seek_delay);
        LOG(LOG_CDROM, "Seek ack INT3, queued INT2 LBA=%u delay=%u",
            cd->seek_lba, cd->seek_delay);
        break;

    case CDROM_PHASE_SEEK_DONE:
        /* Prepare and deliver INT2 seek-complete. */
        cd->cur_lba = cd->seek_lba;
        cd->state = CDROM_STATE_IDLE;
        resp_clear(cd);
        resp_push(cd, make_stat(cd));
        commit_irq(cd, irq, ev.int_type);
        log_irq_event(ev.int_type);
        LOG(LOG_CDROM, "Seek complete INT2 LBA=%u", cd->cur_lba);
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
        evq_push(&cd->evq, CDROM_INT2, CDROM_PHASE_READTOC_DONE,
                 PS1_CPU_HZ * 3u / 5u);
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
            uint64_t first_sector_delay = (uint64_t)period + cd->read_seek_delay;
            evq_push(&cd->evq, CDROM_INT1, CDROM_PHASE_SECTOR,
                     first_sector_delay > UINT32_MAX
                         ? UINT32_MAX
                         : (uint32_t)first_sector_delay);
            cd->read_seek_delay = 0;
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
