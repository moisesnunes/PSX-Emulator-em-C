#pragma once
#include "irq.h"
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdbool.h>

/* PS1 SIO0 controller — registers 0x1F801040–0x1F80104F
   Models a digital pad in slot 1 and "no device" in slot 2 (memory card). */

/* Digital pad button bitmask — bit=0 means pressed (PS1 active-low). */
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

    PadState pad;
} Sio;

void sio_init(Sio *sio);
uint8_t sio_load8(Sio *sio, uint32_t off);
void sio_store8(Sio *sio, uint32_t off, uint8_t val, Irq *irq);
uint16_t sio_load16(Sio *sio, uint32_t off);
void sio_store16(Sio *sio, uint32_t off, uint16_t val, Irq *irq);

/* Called by main.c on every SDL keyboard event. */
void sio_on_key(Sio *sio, SDL_Scancode sc, bool pressed);
