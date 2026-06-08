#include "sio.h"
#include "log.h"
#include <string.h>

/* STAT register bits */
#define STAT_TX_READY (1u << 0) /* TX FIFO not full — always ready */
#define STAT_RX_AVAIL (1u << 1) /* RX FIFO has data */
#define STAT_TX_IDLE (1u << 2)  /* TX finished — always idle */
#define STAT_ACK (1u << 7)      /* ACK/DSR input from device */
#define STAT_IRQ (1u << 9)      /* SIO interrupt pending */

/* CTRL register bits */
#define CTRL_TX_EN (1u << 0)
#define CTRL_SELECT (1u << 1) /* assert /CS — starts a transaction */
#define CTRL_RX_EN (1u << 2)
#define CTRL_ACK_RESET (1u << 4)  /* acknowledge/reset ACK state */
#define CTRL_RESET (1u << 6)      /* soft-reset */
#define CTRL_SLOT_MASK (1u << 13) /* 0=slot1(pad), 1=slot2(memcard) */

/* Digital pad device-id response byte */
#define PAD_ID_DIGITAL 0x41
#define ACK_DELAY_CYCLES 535u

/* Key map: SDL scancode → button bitmask */
static const struct
{
    SDL_Scancode sc;
    uint16_t mask;
} KEY_MAP[] = {
    {SDL_SCANCODE_UP, SIO_PAD_UP},
    {SDL_SCANCODE_DOWN, SIO_PAD_DOWN},
    {SDL_SCANCODE_LEFT, SIO_PAD_LEFT},
    {SDL_SCANCODE_RIGHT, SIO_PAD_RIGHT},
    {SDL_SCANCODE_RETURN, SIO_PAD_START},
    {SDL_SCANCODE_RSHIFT, SIO_PAD_SELECT},
    {SDL_SCANCODE_Z, SIO_PAD_CROSS},
    {SDL_SCANCODE_X, SIO_PAD_CIRCLE},
    {SDL_SCANCODE_A, SIO_PAD_SQUARE},
    {SDL_SCANCODE_S, SIO_PAD_TRIANGLE},
    {SDL_SCANCODE_Q, SIO_PAD_L1},
    {SDL_SCANCODE_W, SIO_PAD_R1},
    {SDL_SCANCODE_1, SIO_PAD_L2},
    {SDL_SCANCODE_2, SIO_PAD_R2},
};
#define KEY_MAP_SIZE (sizeof(KEY_MAP) / sizeof(KEY_MAP[0]))

static void refresh_pad_buttons(Sio *sio)
{
    sio->pad.buttons = (uint16_t)~(sio->keyboard_buttons |
                                   sio->controller_buttons |
                                   sio->forced_buttons);
}

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
    sio->pad_access = false;
    sio->ack_active = false;
}

static void schedule_ack_irq(Sio *sio)
{
    sio->irq_cycles_remaining = ACK_DELAY_CYCLES;
}

static uint32_t build_stat(const Sio *sio)
{
    uint32_t s = STAT_TX_READY | STAT_TX_IDLE;
    if (sio->rx_pos < sio->rx_len)
        s |= STAT_RX_AVAIL;
    if (sio->ack_active)
        s |= STAT_ACK;
    if (sio->irq_pending)
        s |= STAT_IRQ;
    return s;
}

void sio_init(Sio *sio)
{
    memset(sio, 0, sizeof(*sio));
    refresh_pad_buttons(sio);
    sio->stat = STAT_TX_READY | STAT_TX_IDLE;
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
        {
            sio->memcard_access = val == 0x81;
            sio->pad_access = val == 0x01;
        }
        else if (sio->tx_count == 1 && sio->pad_access)
        {
            sio->pad_access = val == 0x42;
        }
        if (sio->rx_len < sizeof(sio->rx_buf))
        {
            bool no_device = !sio->selected || sio->slot2 || sio->memcard_access ||
                             (sio->tx_count > 0 && !sio->pad_access);
            sio->rx_buf[sio->rx_len++] =
                no_device ? 0xFF : pad_response_byte(sio, sio->tx_count);
            if (!no_device && sio->tx_count < 4)
                schedule_ack_irq(sio);
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
    {
        uint16_t bits = (uint16_t)(build_stat(sio) & 0xFFFF);
        sio->ack_active = false;
        sio->stat = build_stat(sio);
        return bits;
    }
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
            sio->irq_pending = false;
            sio->irq_cycles_remaining = 0;
            sio->ctrl = 0;
        }
        else if (val & CTRL_ACK_RESET)
        {
            sio->irq_pending = false;
            sio->ctrl &= (uint16_t)~CTRL_ACK_RESET;
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
                sio->pad_access = false;
            }
        }
        else if (sio->selected)
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

void sio_step(Sio *sio, Irq *irq, uint32_t cycles)
{
    if (sio->irq_cycles_remaining > 0)
    {
        if (cycles >= sio->irq_cycles_remaining)
        {
            sio->irq_cycles_remaining = 0;
            sio->irq_pending = true;
            sio->ack_active = true;
            sio->stat = build_stat(sio);
        }
        else
        {
            sio->irq_cycles_remaining -= cycles;
        }
    }
    if (sio->irq_pending)
        irq_assert(irq, IRQ_SIO);
}

void sio_set_button(Sio *sio, uint16_t mask, bool pressed)
{
    if (pressed)
        sio->controller_buttons |= mask;
    else
        sio->controller_buttons &= (uint16_t)~mask;
    refresh_pad_buttons(sio);
}

void sio_set_controller_state(Sio *sio, uint16_t pressed_mask)
{
    sio->controller_buttons = pressed_mask;
    refresh_pad_buttons(sio);
}

void sio_set_forced_state(Sio *sio, uint16_t pressed_mask)
{
    sio->forced_buttons = pressed_mask;
    refresh_pad_buttons(sio);
}

void sio_on_key(Sio *sio, SDL_Scancode sc, bool pressed)
{
    for (size_t i = 0; i < KEY_MAP_SIZE; i++)
    {
        if (KEY_MAP[i].sc == sc)
        {
            if (pressed)
                sio->keyboard_buttons |= KEY_MAP[i].mask;
            else
                sio->keyboard_buttons &= (uint16_t)~KEY_MAP[i].mask;
            refresh_pad_buttons(sio);
            return;
        }
    }
}
