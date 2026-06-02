#include "sio.h"
#include "log.h"
#include <string.h>

/* STAT register bits */
#define STAT_TX_READY (1u << 0) /* TX FIFO not full — always ready */
#define STAT_RX_AVAIL (1u << 1) /* RX FIFO has data */
#define STAT_TX_IDLE (1u << 2)  /* TX finished — always idle */
#define STAT_ACK (1u << 7)      /* /ACK from device (active-low; 0=ack) */

/* CTRL register bits */
#define CTRL_TX_EN (1u << 0)
#define CTRL_SELECT (1u << 1) /* assert /CS — starts a transaction */
#define CTRL_RX_EN (1u << 2)
#define CTRL_ACK_RESET (1u << 4)  /* acknowledge/reset ACK state */
#define CTRL_RESET (1u << 6)      /* soft-reset */
#define CTRL_SLOT_MASK (1u << 13) /* 0=slot1(pad), 1=slot2(memcard) */

/* Digital pad device-id response byte */
#define PAD_ID_DIGITAL 0x41

/* Key map: SDL scancode → button bitmask */
static const struct
{
    SDL_Scancode sc;
    uint16_t mask;
} KEY_MAP[] = {
    {SDL_SCANCODE_UP, 1 << 4},     /* D-pad Up    */
    {SDL_SCANCODE_DOWN, 1 << 6},   /* D-pad Down  */
    {SDL_SCANCODE_LEFT, 1 << 7},   /* D-pad Left  */
    {SDL_SCANCODE_RIGHT, 1 << 5},  /* D-pad Right */
    {SDL_SCANCODE_RETURN, 1 << 3}, /* Start       */
    {SDL_SCANCODE_RSHIFT, 1 << 0}, /* Select      */
    {SDL_SCANCODE_Z, 1 << 14},     /* Cross       */
    {SDL_SCANCODE_X, 1 << 13},     /* Circle      */
    {SDL_SCANCODE_A, 1 << 15},     /* Square      */
    {SDL_SCANCODE_S, 1 << 12},     /* Triangle    */
    {SDL_SCANCODE_Q, 1 << 10},     /* L1          */
    {SDL_SCANCODE_W, 1 << 11},     /* R1          */
    {SDL_SCANCODE_1, 1 << 8},      /* L2          */
    {SDL_SCANCODE_2, 1 << 9},      /* R2          */
};
#define KEY_MAP_SIZE (sizeof(KEY_MAP) / sizeof(KEY_MAP[0]))

static uint8_t pad_response_byte(const Sio *sio, uint8_t index)
{
    uint8_t lo = (uint8_t)(sio->pad.buttons & 0xFF);
    uint8_t hi = (uint8_t)(sio->pad.buttons >> 8);
    switch (index)
    {
    case 0:
        return 0xFF; /* reply to address byte 0x01 */
    case 1:
        return PAD_ID_DIGITAL; /* reply to 0x42: device id   */
    case 2:
        return 0x5A; /* reply to 0x00: ACK         */
    case 3:
        return lo; /* reply to 0x00: buttons lo  */
    case 4:
        return hi; /* reply to 0x00: buttons hi  */
    default:
        return 0xFF;
    }
}

static void reset_transaction(Sio *sio)
{
    sio->rx_len = 0;
    sio->rx_pos = 0;
    sio->tx_count = 0;
    sio->selected = false;
    sio->slot2 = false;
    sio->memcard_access = false;
}

static uint32_t build_stat(const Sio *sio)
{
    uint32_t s = STAT_TX_READY | STAT_TX_IDLE;
    if (sio->rx_pos < sio->rx_len)
        s |= STAT_RX_AVAIL;
    /* /ACK is asserted (bit=0) only while a real device is responding. */
    bool device_selected = sio->selected && !sio->slot2 && !sio->memcard_access;
    if (!device_selected || sio->rx_pos >= sio->rx_len)
        s |= STAT_ACK; /* no ACK (bit=1 means not asserted) */
    return s;
}

void sio_init(Sio *sio)
{
    memset(sio, 0, sizeof(*sio));
    sio->pad.buttons = 0xFFFF; /* all buttons released */
    sio->stat = STAT_TX_READY | STAT_TX_IDLE | STAT_ACK;
    sio->baud = 0x0088; /* typical BIOS init value */
}

uint8_t sio_load8(Sio *sio, uint32_t off)
{
    switch (off)
    {
    case 0x00:
    { /* RX_DATA */
        uint8_t v = 0xFF;
        uint8_t pos = sio->rx_pos;
        if (sio->rx_pos < sio->rx_len)
            v = sio->rx_buf[sio->rx_pos++];
        if (sio->rx_pos >= sio->rx_len)
            sio->rx_pos = sio->rx_len = 0;
        sio->stat = build_stat(sio);
        LOG(LOG_SIO, "SIO RX[%u] = 0x%02X", pos, v);
        return v;
    }
    default:
        /* Fall through to 16-bit handler for wider registers */
        return (uint8_t)(sio_load16(sio, off & ~1u) >> ((off & 1) * 8));
    }
}

void sio_store8(Sio *sio, uint32_t off, uint8_t val, Irq *irq)
{
    (void)irq;
    if (off == 0x00)
    {
        /* TX_DATA — each host byte clocks exactly one response byte. */
        LOG(LOG_SIO, "SIO TX[%u] = 0x%02X", sio->tx_count, val);
        if (sio->rx_pos >= sio->rx_len)
            sio->rx_pos = sio->rx_len = 0;
        if (sio->tx_count == 0)
            sio->memcard_access = val == 0x81;
        if (sio->rx_len < sizeof(sio->rx_buf))
        {
            bool no_device = !sio->selected || sio->slot2 || sio->memcard_access;
            sio->rx_buf[sio->rx_len++] =
                no_device ? 0xFF : pad_response_byte(sio, sio->tx_count);
        }
        sio->tx_count++;
        sio->stat = build_stat(sio);
        return;
    }
    sio_store16(sio, off & ~1u,
                (sio_load16(sio, off & ~1u) & ~(0xFFu << ((off & 1) * 8))) |
                    ((uint16_t)val << ((off & 1) * 8)),
                irq);
}

uint16_t sio_load16(Sio *sio, uint32_t off)
{
    switch (off)
    {
    case 0x00:
        return (uint16_t)sio_load8(sio, 0x00);
    case 0x04:
        return (uint16_t)(sio->stat & 0xFFFF);
    case 0x06:
        return (uint16_t)(sio->stat >> 16);
    case 0x08:
        return sio->mode;
    case 0x0A:
        return sio->ctrl;
    case 0x0E:
        return sio->baud;
    default:
        return 0;
    }
}

void sio_store16(Sio *sio, uint32_t off, uint16_t val, Irq *irq)
{
    (void)irq;
    switch (off)
    {
    case 0x08:
        sio->mode = val;
        break;
    case 0x0A:
    {
        bool was_selected = sio->selected;
        sio->ctrl = val;
        if (val & CTRL_RESET)
        {
            reset_transaction(sio);
            sio->ctrl = 0;
        }
        else if (val & CTRL_SELECT)
        {
            /* /CS asserted — choose slot; TX writes will clock responses.
               Do not restart the transaction while /CS remains asserted. */
            sio->selected = true;
            sio->slot2 = (val & CTRL_SLOT_MASK) != 0;
            if (!was_selected)
            {
                sio->tx_count = 0;
                sio->rx_len = 0;
                sio->rx_pos = 0;
                sio->memcard_access = false;
            }
        }
        else if (!(val & CTRL_SELECT) && sio->selected)
        {
            /* /CS deasserted */
            reset_transaction(sio);
        }
        sio->stat = build_stat(sio);
        break;
    }
    case 0x0E:
        sio->baud = val;
        break;
    default:
        break;
    }
}

void sio_on_key(Sio *sio, SDL_Scancode sc, bool pressed)
{
    for (size_t i = 0; i < KEY_MAP_SIZE; i++)
    {
        if (KEY_MAP[i].sc == sc)
        {
            if (pressed)
                sio->pad.buttons &= ~KEY_MAP[i].mask; /* active-low */
            else
                sio->pad.buttons |= KEY_MAP[i].mask;
            return;
        }
    }
}
