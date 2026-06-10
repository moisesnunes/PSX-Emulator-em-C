#define _POSIX_C_SOURCE 200809L

#include "sio.h"
#include "log.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
#define CTRL_SLOT_MASK (1u << 13) /* 0=port 1, 1=port 2 */

/* Digital pad device-id response byte */
#define PAD_ID_DIGITAL 0x41
#define PAD_ID_ANALOG 0x73
#define PAD_ID_CONFIG 0xF3
#define MEMCARD_FLAG_NEW 0x08u

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
    sio->pad_port2.buttons = (uint16_t)~sio->controller_buttons_port2;
}

static uint8_t pad_response_byte(Sio *sio, uint8_t index, uint8_t value)
{
    PadState *pad = sio->slot2 ? &sio->pad_port2 : &sio->pad;
    uint8_t lo = (uint8_t)(pad->buttons & 0xFF);
    uint8_t hi = (uint8_t)(pad->buttons >> 8);
    uint8_t id = pad->config_mode
                     ? PAD_ID_CONFIG
                     : (pad->analog_enabled ? PAD_ID_ANALOG : PAD_ID_DIGITAL);
    switch (index)
    {
    case 0:
        return 0xFF; /* reply to address byte 0x01 */
    case 1:
        return id;
    case 2:
        return 0x5A;
    case 3:
        if (sio->pad_command == 0x45)
            return 0x01;
        if (sio->pad_command == 0x46 || sio->pad_command == 0x4C)
        {
            pad->config_query = value & 1u;
            return 0x00;
        }
        if (sio->pad_command == 0x47 || sio->pad_command == 0x4D)
            return 0x00;
        if (sio->pad_command == 0x43)
            pad->config_mode = value == 0x01;
        else if (sio->pad_command == 0x44 && pad->config_mode &&
                 !pad->mode_locked)
            pad->analog_enabled = value == 0x01;
        if (sio->pad_command == 0x42 && pad->buttons != 0xFFFFu)
            LOG(LOG_SIO, "pad port=%u buttons=0x%04X",
                sio->slot2 ? 2u : 1u, pad->buttons);
        return sio->pad_command == 0x42 ? lo : 0x00;
    case 4:
        if (sio->pad_command == 0x45)
            return 0x02;
        if (sio->pad_command == 0x46 || sio->pad_command == 0x47 ||
            sio->pad_command == 0x4C)
            return 0x00;
        if (sio->pad_command == 0x4D)
            return 0x01;
        if (sio->pad_command == 0x44 && pad->config_mode)
            pad->mode_locked = value == 0x03;
        return sio->pad_command == 0x42 ? hi : 0x00;
    case 5:
        if (sio->pad_command == 0x45)
            return pad->analog_enabled ? 0x01 : 0x00;
        if (sio->pad_command == 0x46)
            return 0x01;
        if (sio->pad_command == 0x47)
            return 0x02;
        if (sio->pad_command == 0x4C)
            return 0x00;
        if (sio->pad_command == 0x4D)
            return 0xFF;
        return sio->pad_command == 0x42 && pad->analog_enabled
                   ? pad->right_x
                   : 0x00;
    case 6:
        if (sio->pad_command == 0x45)
            return 0x02;
        if (sio->pad_command == 0x46)
            return pad->config_query ? 0x01 : 0x02;
        if (sio->pad_command == 0x47)
            return 0x00;
        if (sio->pad_command == 0x4C)
            return pad->config_query ? 0x07 : 0x04;
        if (sio->pad_command == 0x4D)
            return 0xFF;
        return sio->pad_command == 0x42 && pad->analog_enabled
                   ? pad->right_y
                   : 0x00;
    case 7:
        if (sio->pad_command == 0x45)
            return 0x01;
        if (sio->pad_command == 0x46)
            return pad->config_query ? 0x01 : 0x00;
        if (sio->pad_command == 0x47)
            return 0x01;
        if (sio->pad_command == 0x4C)
            return 0x00;
        if (sio->pad_command == 0x4D)
            return 0xFF;
        return sio->pad_command == 0x42 && pad->analog_enabled
                   ? pad->left_x
                   : 0x00;
    case 8:
        if (sio->pad_command == 0x45)
            return 0x00;
        if (sio->pad_command == 0x46)
            return pad->config_query ? 0x14 : 0x0A;
        if (sio->pad_command == 0x47 || sio->pad_command == 0x4C)
            return 0x00;
        if (sio->pad_command == 0x4D)
            return 0xFF;
        return sio->pad_command == 0x42 && pad->analog_enabled
                   ? pad->left_y
                   : 0x00;
    default:
        return 0xFF;
    }
}

static uint8_t pad_response_length(const Sio *sio)
{
    const PadState *pad = sio->slot2 ? &sio->pad_port2 : &sio->pad;
    if (sio->pad_command == 0x42)
        return pad->analog_enabled ? 9u : 5u;
    return 9u;
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
    sio->pad_command = 0;
    sio->ack_active = false;
    sio->memcard_pos = 0;
    sio->memcard_command = 0;
    sio->memcard_checksum = 0;
    sio->memcard_received_checksum = 0;
    sio->memcard_write_valid = false;
}

static uint32_t serial_byte_cycles(const Sio *sio)
{
    static const uint8_t reload_factors[4] = {1, 1, 16, 64};
    uint32_t reload = sio->baud ? sio->baud : 1u;
    uint32_t factor = reload_factors[sio->mode & 3u];
    uint64_t cycles = (uint64_t)reload * factor * 8u;
    return cycles > UINT32_MAX ? UINT32_MAX : (uint32_t)cycles;
}

static void schedule_ack_irq(Sio *sio)
{
    sio->irq_cycles_remaining = serial_byte_cycles(sio);
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
    sio->pad.right_x = sio->pad.right_y = 0x80;
    sio->pad.left_x = sio->pad.left_y = 0x80;
    sio->pad_port2.right_x = sio->pad_port2.right_y = 0x80;
    sio->pad_port2.left_x = sio->pad_port2.left_y = 0x80;
    sio->stat = STAT_TX_READY | STAT_TX_IDLE;
    sio->baud = 0x0088; /* typical BIOS init value */
}

static void format_memory_card(uint8_t *data)
{
    memset(data, 0, SIO_MEMCARD_SIZE);
    data[0] = 'M';
    data[1] = 'C';
    data[127] = 0x0E;

    for (uint32_t block = 0; block < 15; block++)
    {
        uint8_t *frame = data + (block + 1u) * SIO_MEMCARD_SECTOR_SIZE;
        frame[0] = 0xA0;
        frame[8] = 0x00;
        frame[9] = 0xFF;
        frame[10] = 0xFF;
        frame[127] = 0xA0;
    }

    for (uint32_t frame_index = 16; frame_index < 36; frame_index++)
    {
        uint8_t *frame = data + frame_index * SIO_MEMCARD_SECTOR_SIZE;
        frame[0] = frame[1] = frame[2] = frame[3] = 0xFF;
        frame[8] = frame[9] = 0xFF;
    }
}

static bool memory_card_metadata_valid(const uint8_t *data)
{
    if (data[0] != 'M' || data[1] != 'C')
        return false;

    for (uint32_t frame_index = 0; frame_index < 36; frame_index++)
    {
        const uint8_t *frame =
            data + frame_index * SIO_MEMCARD_SECTOR_SIZE;
        uint8_t checksum = 0;
        for (uint32_t i = 0; i < SIO_MEMCARD_SECTOR_SIZE - 1u; i++)
            checksum ^= frame[i];
        if (checksum != frame[SIO_MEMCARD_SECTOR_SIZE - 1u])
            return false;
    }
    return true;
}

int sio_memory_card_flush(Sio *sio)
{
    if (!sio->memcard_present || sio->memcard_path[0] == '\0')
        return -1;

    char tmp_path[sizeof(sio->memcard_path) + 5];
    int len = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", sio->memcard_path);
    if (len < 0 || (size_t)len >= sizeof(tmp_path))
        return -1;

    FILE *file = fopen(tmp_path, "wb");
    if (!file)
        return -1;
    bool ok = fwrite(sio->memcard_data, 1, SIO_MEMCARD_SIZE, file) ==
              SIO_MEMCARD_SIZE;
    if (ok)
        ok = fflush(file) == 0;
    if (ok)
        ok = fsync(fileno(file)) == 0;
    if (fclose(file) != 0)
        ok = false;
    if (!ok || rename(tmp_path, sio->memcard_path) != 0)
    {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

int sio_memory_card_load(Sio *sio, const char *path)
{
    if (!path || path[0] == '\0' || strlen(path) >= sizeof(sio->memcard_path))
        return -1;

    FILE *file = fopen(path, "rb");
    if (!file)
    {
        if (errno != ENOENT)
            return -1;
        format_memory_card(sio->memcard_data);
        strcpy(sio->memcard_path, path);
        sio->memcard_present = true;
        sio->memcard_new = true;
        sio->memcard_corrupt = false;
        if (sio_memory_card_flush(sio) != 0)
        {
            sio_memory_card_eject(sio);
            return -1;
        }
        fprintf(stderr, "Memory card created: %s\n", path);
        return 1;
    }

    size_t size = fread(sio->memcard_data, 1, SIO_MEMCARD_SIZE, file);
    int extra = fgetc(file);
    bool read_error = ferror(file) != 0;
    fclose(file);
    strcpy(sio->memcard_path, path);
    bool valid_size = size == SIO_MEMCARD_SIZE && extra == EOF;
    if (read_error || !valid_size ||
        !memory_card_metadata_valid(sio->memcard_data))
    {
        sio->memcard_present = false;
        sio->memcard_corrupt = true;
        fprintf(stderr, "Memory card corrupt (invalid size or metadata): %s\n",
                path);
        return -1;
    }

    sio->memcard_present = true;
    sio->memcard_new = false;
    sio->memcard_corrupt = false;
    fprintf(stderr, "Memory card loaded: %s\n", path);
    return 0;
}

void sio_memory_card_eject(Sio *sio)
{
    sio->memcard_present = false;
    sio->memcard_new = false;
    sio->memcard_path[0] = '\0';
    reset_transaction(sio);
}

static uint8_t memcard_response(Sio *sio, uint8_t value)
{
    uint16_t pos = sio->memcard_pos++;
    if (pos == 0)
        return 0xFF;
    if (pos == 1)
    {
        sio->memcard_command = value;
        return sio->memcard_new ? MEMCARD_FLAG_NEW : 0x00;
    }
    if (pos == 2)
        return 0x5A;
    if (pos == 3)
        return 0x5D;
    if (pos == 4)
    {
        sio->memcard_sector = (uint16_t)value << 8;
        sio->memcard_checksum = value;
        return 0x00;
    }
    if (pos == 5)
    {
        sio->memcard_sector |= value;
        sio->memcard_checksum ^= value;
        return (uint8_t)(sio->memcard_sector >> 8);
    }

    uint32_t sector_offset =
        (uint32_t)(sio->memcard_sector & 0x03FFu) * SIO_MEMCARD_SECTOR_SIZE;
    if (sio->memcard_command == 0x52)
    {
        if (pos == 6)
            return 0x5C;
        if (pos == 7)
            return 0x5D;
        if (pos == 8)
            return (uint8_t)(sio->memcard_sector >> 8);
        if (pos == 9)
            return (uint8_t)sio->memcard_sector;
        if (pos >= 10 && pos < 138)
        {
            uint8_t data = sio->memcard_data[sector_offset + pos - 10u];
            sio->memcard_checksum ^= data;
            return data;
        }
        if (pos == 138)
            return sio->memcard_checksum;
        if (pos == 139)
        {
            sio->memcard_new = false;
            return 0x47;
        }
        return 0xFF;
    }

    if (sio->memcard_command == 0x57)
    {
        if (pos >= 6 && pos < 134)
        {
            uint16_t index = pos - 6u;
            sio->memcard_write[index] = value;
            sio->memcard_checksum ^= value;
            return index == 0 ? (uint8_t)sio->memcard_sector
                              : sio->memcard_write[index - 1u];
        }
        if (pos == 134)
        {
            sio->memcard_received_checksum = value;
            sio->memcard_write_valid =
                value == sio->memcard_checksum && sio->memcard_sector < 1024u;
            return sio->memcard_write[127];
        }
        if (pos == 135)
            return 0x5C;
        if (pos == 136)
            return 0x5D;
        if (pos == 137)
        {
            if (sio->memcard_write_valid)
            {
                memcpy(sio->memcard_data + sector_offset, sio->memcard_write,
                       SIO_MEMCARD_SECTOR_SIZE);
                sio->memcard_new = false;
                if (sio_memory_card_flush(sio) != 0)
                {
                    fprintf(stderr, "Memory card write failed: %s\n",
                            sio->memcard_path);
                    return 0x4E;
                }
                return 0x47;
            }
            return 0x4E;
        }
        return 0xFF;
    }

    return 0xFF;
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
            sio->memcard_access =
                val == 0x81 && sio->memcard_present && !sio->slot2;
            sio->pad_access = val == 0x01;
        }
        else if (sio->tx_count == 1 && sio->pad_access)
        {
            PadState *pad = sio->slot2 ? &sio->pad_port2 : &sio->pad;
            sio->pad_command = val;
            sio->pad_access =
                val == 0x42 || val == 0x43 ||
                (pad->config_mode &&
                 (val == 0x44 || val == 0x45 || val == 0x46 ||
                  val == 0x47 || val == 0x4C || val == 0x4D));
        }
        if (sio->rx_len < sizeof(sio->rx_buf))
        {
            bool active_memcard = sio->selected && sio->memcard_access;
            bool active_pad = sio->selected && sio->pad_access;
            uint8_t response = 0xFF;
            if (active_memcard)
                response = memcard_response(sio, val);
            else if (active_pad)
                response = pad_response_byte(sio, sio->tx_count, val);
            sio->rx_buf[sio->rx_len++] = response;
            bool memcard_ack =
                active_memcard &&
                ((sio->memcard_command == 0x52 && sio->memcard_pos <= 139u) ||
                 (sio->memcard_command == 0x57 && sio->memcard_pos <= 137u) ||
                 sio->memcard_pos <= 5u);
            if (memcard_ack ||
                (active_pad &&
                 sio->tx_count + 1u < pad_response_length(sio)))
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
        bool ack_only = (val & CTRL_ACK_RESET) != 0 &&
                        (val & (uint16_t)~CTRL_ACK_RESET) == 0;
        if (ack_only)
            val = (uint16_t)(sio->ctrl | CTRL_ACK_RESET);
        sio->ctrl = val;
        if (val & CTRL_RESET)
        {
            reset_transaction(sio);
            sio->irq_pending = false;
            sio->irq_cycles_remaining = 0;
            sio->ctrl = 0;
        }
        else
        {
            if (val & CTRL_ACK_RESET)
            {
                sio->irq_pending = false;
                sio->ctrl &= (uint16_t)~CTRL_ACK_RESET;
            }

            if (val & CTRL_SELECT)
            {
                /* ACK reset and /CS selection are independent control bits. */
                sio->selected = true;
                sio->slot2 = (val & CTRL_SLOT_MASK) != 0;
                if (!was_selected)
                {
                    sio->tx_count = 0;
                    sio->rx_len = 0;
                    sio->rx_pos = 0;
                    sio->memcard_access = false;
                    sio->pad_access = false;
                    sio->pad_command = 0;
                }
            }
            else if (was_selected)
            {
                /* /CS deasserted */
                reset_transaction(sio);
            }
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

uint32_t sio_cycles_until_event(const Sio *sio)
{
    return sio->irq_cycles_remaining ? sio->irq_cycles_remaining : UINT32_MAX;
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
    sio_set_port_controller_state(sio, 0, pressed_mask);
}

void sio_set_port_controller_state(Sio *sio, unsigned port,
                                   uint16_t pressed_mask)
{
    if (port == 0)
        sio->controller_buttons = pressed_mask;
    else if (port == 1)
        sio->controller_buttons_port2 = pressed_mask;
    else
        return;
    refresh_pad_buttons(sio);
}

void sio_set_port_analog_state(Sio *sio, unsigned port, uint8_t left_x,
                               uint8_t left_y, uint8_t right_x,
                               uint8_t right_y)
{
    PadState *pad;
    if (port == 0)
        pad = &sio->pad;
    else if (port == 1)
        pad = &sio->pad_port2;
    else
        return;
    pad->left_x = left_x;
    pad->left_y = left_y;
    pad->right_x = right_x;
    pad->right_y = right_y;
}

void sio_set_forced_state(Sio *sio, uint16_t pressed_mask)
{
    sio->forced_buttons = pressed_mask;
    refresh_pad_buttons(sio);
}

void sio_clear_keyboard_state(Sio *sio)
{
    sio->keyboard_buttons = 0;
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
