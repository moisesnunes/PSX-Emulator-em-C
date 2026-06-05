#pragma once
#include "irq.h"
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdbool.h>

/* PS1 SIO0 controller — registers 0x1F801040–0x1F80104F
   Models a digital pad in slot 1 and "no device" in slot 2 (memory card). */

/* Digital pad button bitmask — bit=0 means pressed (PS1 active-low). */
enum
{
    SIO_PAD_SELECT = 1u << 0,
    SIO_PAD_START = 1u << 3,
    SIO_PAD_UP = 1u << 4,
    SIO_PAD_RIGHT = 1u << 5,
    SIO_PAD_DOWN = 1u << 6,
    SIO_PAD_LEFT = 1u << 7,
    SIO_PAD_L2 = 1u << 8,
    SIO_PAD_R2 = 1u << 9,
    SIO_PAD_L1 = 1u << 10,
    SIO_PAD_R1 = 1u << 11,
    SIO_PAD_TRIANGLE = 1u << 12,
    SIO_PAD_CIRCLE = 1u << 13,
    SIO_PAD_CROSS = 1u << 14,
    SIO_PAD_SQUARE = 1u << 15,
};

typedef struct
{
    uint16_t buttons; /* bit0=Select bit3=Start bit4=Up bit5=Right
                         bit6=Down bit7=Left bit8=L2 bit9=R2
                         bit10=L1 bit11=R1 bit12=Tri bit13=Circ
                         bit14=Cross bit15=Square                  */
} PadState;

typedef struct
{
    /* Registers (offsets from 0x1F801040) */
    uint8_t rx_data; /* 0x00 R  last received byte */
    uint32_t stat;   /* 0x04 R  status */
    uint16_t mode;   /* 0x08 RW mode */
    uint16_t ctrl;   /* 0x0A RW control */
    uint16_t baud;   /* 0x0E RW baud rate */

    /* Transaction state */
    uint8_t rx_buf[16]; /* bytes the device will send back */
    uint8_t rx_len;
    uint8_t rx_pos;
    uint8_t tx_count; /* bytes received from host this transaction */
    bool selected;
    bool slot2;
    bool memcard_access;
    bool pad_access;
    bool ack_active;
    bool irq_pending;
    uint8_t irq_timer;

    uint16_t keyboard_buttons;
    uint16_t controller_buttons;
    uint16_t forced_buttons;
    PadState pad;
} Sio;

void sio_init(Sio *sio);
uint8_t sio_load8(Sio *sio, uint32_t off);
void sio_store8(Sio *sio, uint32_t off, uint8_t val, Irq *irq);
uint16_t sio_load16(Sio *sio, uint32_t off);
void sio_store16(Sio *sio, uint32_t off, uint16_t val, Irq *irq);
void sio_step(Sio *sio, Irq *irq);

void sio_set_button(Sio *sio, uint16_t mask, bool pressed);
void sio_set_controller_state(Sio *sio, uint16_t pressed_mask);
void sio_set_forced_state(Sio *sio, uint16_t pressed_mask);

/* Called by main.c on every SDL keyboard event. */
void sio_on_key(Sio *sio, SDL_Scancode sc, bool pressed);
