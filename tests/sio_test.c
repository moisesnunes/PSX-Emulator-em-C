#include "sio.h"
#include "log.h"

#include <stdio.h>

void log_msg(LogSubsystem s, const char *fmt, ...)
{
    (void)s;
    (void)fmt;
}

bool log_is_enabled(LogSubsystem s)
{
    (void)s;
    return false;
}

void irq_assert(Irq *irq, IrqFlag flag)
{
    irq->status |= (uint16_t)flag;
}

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

static uint8_t exchange(Sio *sio, uint8_t tx)
{
    sio_store8(sio, 0x00, tx, NULL);
    return sio_load8(sio, 0x00);
}

static void select_slot(Sio *sio, bool slot2)
{
    uint16_t ctrl = 0x0001 | 0x0002 | 0x0004;
    if (slot2)
        ctrl |= 0x2000;
    sio_store16(sio, 0x0A, ctrl, NULL);
}

static void deselect(Sio *sio)
{
    sio_store16(sio, 0x0A, 0x0000, NULL);
}

static void test_digital_pad_released(void)
{
    printf("test_digital_pad_released ... ");
    Sio sio;
    sio_init(&sio);
    select_slot(&sio, false);

    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x42), 0x41u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x5Au);
    EXPECT_EQ(exchange(&sio, 0x00), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x00), 0xFFu);
    deselect(&sio);
    printf("ok\n");
}

static void test_digital_pad_pressed(void)
{
    printf("test_digital_pad_pressed ... ");
    Sio sio;
    sio_init(&sio);
    sio_on_key(&sio, SDL_SCANCODE_Z, true);
    select_slot(&sio, false);

    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x42), 0x41u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x5Au);
    EXPECT_EQ(exchange(&sio, 0x00), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x00), 0xBFu);
    deselect(&sio);

    sio_on_key(&sio, SDL_SCANCODE_Z, false);
    select_slot(&sio, false);
    exchange(&sio, 0x01);
    exchange(&sio, 0x42);
    exchange(&sio, 0x00);
    EXPECT_EQ(exchange(&sio, 0x00), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x00), 0xFFu);
    printf("ok\n");
}

static void test_keyboard_and_controller_sources_merge(void)
{
    printf("test_keyboard_and_controller_sources_merge ... ");
    Sio sio;
    sio_init(&sio);

    sio_on_key(&sio, SDL_SCANCODE_Z, true);
    sio_set_button(&sio, SIO_PAD_CROSS, true);
    EXPECT_EQ((sio.pad.buttons & SIO_PAD_CROSS), 0u);

    sio_set_button(&sio, SIO_PAD_CROSS, false);
    EXPECT_EQ((sio.pad.buttons & SIO_PAD_CROSS), 0u);

    sio_on_key(&sio, SDL_SCANCODE_Z, false);
    EXPECT_EQ((sio.pad.buttons & SIO_PAD_CROSS), (uint32_t)SIO_PAD_CROSS);
    printf("ok\n");
}

static void test_forced_buttons_merge(void)
{
    printf("test_forced_buttons_merge ... ");
    Sio sio;
    sio_init(&sio);

    sio_set_forced_state(&sio, SIO_PAD_START | SIO_PAD_CROSS);
    EXPECT_EQ((sio.pad.buttons & SIO_PAD_START), 0u);
    EXPECT_EQ((sio.pad.buttons & SIO_PAD_CROSS), 0u);

    sio_set_forced_state(&sio, 0);
    EXPECT_EQ((sio.pad.buttons & SIO_PAD_START), (uint32_t)SIO_PAD_START);
    EXPECT_EQ((sio.pad.buttons & SIO_PAD_CROSS), (uint32_t)SIO_PAD_CROSS);
    printf("ok\n");
}

static void test_slot2_no_device(void)
{
    printf("test_slot2_no_device ... ");
    Sio sio;
    sio_init(&sio);
    select_slot(&sio, true);

    EXPECT_EQ(exchange(&sio, 0x81), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x52), 0xFFu);
    printf("ok\n");
}

static void test_unknown_pad_command_no_device(void)
{
    printf("test_unknown_pad_command_no_device ... ");
    Sio sio;
    sio_init(&sio);
    select_slot(&sio, false);

    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x43), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x00), 0xFFu);
    deselect(&sio);

    select_slot(&sio, false);
    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x42), 0x41u);
    printf("ok\n");
}

static void test_memcard_command_no_device(void)
{
    printf("test_memcard_command_no_device ... ");
    Sio sio;
    sio_init(&sio);
    select_slot(&sio, false);

    EXPECT_EQ(exchange(&sio, 0x81), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x52), 0xFFu);
    deselect(&sio);

    select_slot(&sio, false);
    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x42), 0x41u);
    printf("ok\n");
}

static void test_memcard_command_no_ack(void)
{
    printf("test_memcard_command_no_ack ... ");
    Sio sio;
    Irq irq = {0};
    sio_init(&sio);
    select_slot(&sio, false);

    sio_store8(&sio, 0x00, 0x81, NULL);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0080u) != 0, 0u);
    EXPECT_EQ(sio_load8(&sio, 0x00), 0xFFu);
    deselect(&sio);

    select_slot(&sio, false);
    sio_store8(&sio, 0x00, 0x01, NULL);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0080u) != 0, 0u);
    sio_step(&sio, &irq, 535);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0080u) != 0, 1u);
    EXPECT_EQ(sio_load8(&sio, 0x00), 0xFFu);
    printf("ok\n");
}

static void test_pad_ack_irq(void)
{
    printf("test_pad_ack_irq ... ");
    Sio sio;
    Irq irq = {0};
    sio_init(&sio);
    select_slot(&sio, false);

    exchange(&sio, 0x01);
    EXPECT_EQ((irq.status & IRQ_SIO) != 0, 0u);
    sio_step(&sio, &irq, 534);
    EXPECT_EQ((irq.status & IRQ_SIO) != 0, 0u);
    sio_step(&sio, &irq, 1);
    EXPECT_EQ((irq.status & IRQ_SIO) != 0, 1u);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0200u) != 0, 1u);

    sio_store16(&sio, 0x0A, (uint16_t)(sio.ctrl | 0x0010u), &irq);
    EXPECT_EQ(sio.irq_pending, 0u);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0200u) != 0, 0u);
    printf("ok\n");
}

static void test_ack_reset_keeps_selected_transaction(void)
{
    printf("test_ack_reset_keeps_selected_transaction ... ");
    Sio sio;
    Irq irq = {0};
    sio_init(&sio);
    select_slot(&sio, false);

    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    sio_step(&sio, &irq, 267);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0200u) != 0, 0u);

    sio_store16(&sio, 0x0A, 0x0010u, &irq);
    EXPECT_EQ(sio.selected, 1u);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0200u) != 0, 0u);
    sio_step(&sio, &irq, 268);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0200u) != 0, 1u);
    sio_store16(&sio, 0x0A, 0x0010u, &irq);
    EXPECT_EQ(exchange(&sio, 0x42), 0x41u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x5Au);
    deselect(&sio);
    printf("ok\n");
}

int main(void)
{
    printf("=== sio unit tests ===\n");
    test_digital_pad_released();
    test_digital_pad_pressed();
    test_keyboard_and_controller_sources_merge();
    test_forced_buttons_merge();
    test_slot2_no_device();
    test_unknown_pad_command_no_device();
    test_memcard_command_no_device();
    test_memcard_command_no_ack();
    test_pad_ack_irq();
    test_ack_reset_keeps_selected_transaction();
    printf("======================\n");
    printf("pass: %d  fail: %d\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
