#pragma once
#include "disc.h"
#include "irq.h"
#include "scheduler.h"
#include "ram.h"
#include "spu.h"
#include <stdint.h>
#include <stdbool.h>

/* PS1 CD-ROM controller — registers 0x1F801800–0x1F801803 */

/* INT flags (bits 2:0 of interrupt flag register) */
#define CDROM_INT1 1 /* data ready */
#define CDROM_INT2 2 /* acknowledge */
#define CDROM_INT3 3 /* complete */
#define CDROM_INT5 5 /* error */

#define CDROM_PARAM_FIFO_SIZE 16
#define CDROM_RESP_FIFO_SIZE 16
#define CDROM_DATA_FIFO_SIZE 2340 /* one sector's raw data */

typedef enum
{
    CDROM_STATE_IDLE = 0,
    CDROM_STATE_READING,
    CDROM_STATE_SEEKING,
} CdromState;

/* What the event handler should do when it fires. */
typedef enum
{
    CDROM_PHASE_ACK = 0,      /* deliver irq_type from queue, done */
    CDROM_PHASE_INIT_ACK,     /* deliver INT3 init ack, enqueue complete */
    CDROM_PHASE_INIT_DONE,    /* prepare idle stat and deliver INT2 */
    CDROM_PHASE_SEEK_ACK,     /* deliver INT3 seek ack, enqueue seek-done */
    CDROM_PHASE_SEEK_DONE,    /* prepare idle stat and deliver INT2 */
    CDROM_PHASE_GETID_ACK,    /* deliver INT3 ID ack, enqueue ID result */
    CDROM_PHASE_GETID_DONE,   /* prepare ID bytes and deliver INT2/INT5 */
    CDROM_PHASE_READTOC_ACK,  /* deliver INT3 TOC ack, enqueue complete */
    CDROM_PHASE_READTOC_DONE, /* prepare idle stat and deliver INT2 */
    CDROM_PHASE_PAUSE_ACK,    /* deliver INT3 pause ack, enqueue complete */
    CDROM_PHASE_PAUSE_DONE,   /* prepare idle stat and deliver INT2 */
    CDROM_PHASE_READ_DATA,    /* deliver INT3 ack, enqueue first SECTOR event */
    CDROM_PHASE_SECTOR,       /* load + deliver one sector (INT1), re-enqueue self */
} CdromPhase;

/* One pending event: an INT type and what the handler should do with it. */
typedef struct
{
    uint8_t int_type;
    CdromPhase phase;
    uint32_t delay; /* scheduler delay in CPU cycles */
} CdromEvent;

#define CDROM_QUEUE_SIZE 4

/* Small FIFO of pending CD-ROM events.  Commands enqueue; the scheduler
   event handler dequeues one slot per firing and re-arms itself for the
   next slot if the queue is non-empty. */
typedef struct
{
    CdromEvent slots[CDROM_QUEUE_SIZE];
    uint8_t head, tail, count;
} CdromEventQueue;

typedef struct
{
    /* ---- Register index (bits 1:0 of reg 0) ---- */
    uint8_t index;

    /* ---- FIFOs ---- */
    uint8_t param_fifo[CDROM_PARAM_FIFO_SIZE];
    uint8_t param_len;
    uint8_t resp_fifo[CDROM_RESP_FIFO_SIZE];
    uint8_t resp_len;
    uint8_t resp_pos;
    uint8_t data_fifo[CDROM_DATA_FIFO_SIZE];
    uint16_t data_len;
    uint16_t data_pos;

    /* ---- Interrupt state ---- */
    uint8_t irq_flag; /* bits 2:0 — currently asserted INT type (read by SW) */
    uint8_t irq_en;   /* interrupt enable mask */

    /* ---- Pending event queue ---- */
    CdromEventQueue evq; /* commands enqueue; scheduler event handler dequeues */

    /* ---- Command waiting for the current response IRQ to be acknowledged ---- */
    bool pending_cmd_valid;
    uint8_t pending_cmd;
    uint8_t pending_param_fifo[CDROM_PARAM_FIFO_SIZE];
    uint8_t pending_param_len;

    /* ---- Drive state ---- */
    CdromState state;
    bool double_speed;
    bool xa_adpcm_en;
    bool xa_filter_en;
    bool report_mode;
    bool auto_pause;
    bool cdda_en;
    bool playing_cdda;
    bool muted;
    bool adpcm_muted;
    uint8_t mode; /* raw setmode byte */
    uint8_t xa_filter_file;
    uint8_t xa_filter_channel;
    bool xa_current_set;
    uint8_t xa_current_file;
    uint8_t xa_current_channel;
    int32_t xa_last_samples[4];
    uint32_t xa_resample_accum;
    uint8_t cd_audio_volume[2][2];
    uint8_t next_cd_audio_volume[2][2];

    /* ---- Seek target (from Setloc) ---- */
    Msf seek_target;
    bool seek_pending;
    uint32_t seek_lba;
    uint32_t seek_delay;
    uint32_t read_seek_delay;

    /* ---- Current position ---- */
    uint32_t cur_lba;

    /* ---- Disc image (may be NULL = no disc) ---- */
    Disc *disc;
    Spu *spu;

    /* ---- Sector buffer (filled by ReadN) ---- */
    uint8_t sector_buf[DISC_SECTOR_SIZE];
} Cdrom;

void cdrom_init(Cdrom *cd, Disc *disc);
void cdrom_set_spu(Cdrom *cd, Spu *spu);
uint8_t cdrom_load8(Cdrom *cd, uint32_t offset);
void cdrom_store8(Cdrom *cd, uint32_t offset, uint8_t val,
                  Irq *irq, Scheduler *sched);

/* Called by DMA channel 3 (CDROM→RAM) to drain data FIFO. */
uint32_t cdrom_dma_read(Cdrom *cd);
bool cdrom_dma_ready(const Cdrom *cd);

/* Called by scheduler when EVENT_CDROM_IRQ fires. */
void cdrom_on_scheduler_event(Cdrom *cd, Irq *irq, Scheduler *sched);

/* Structured IRQ event log — for unit tests.
   Records the int_type of every IRQ delivered (INT1/INT2/INT3/INT5). */
void cdrom_event_log_reset(void);
uint32_t cdrom_event_log_count(void);
uint8_t cdrom_event_log_get(uint32_t i);
