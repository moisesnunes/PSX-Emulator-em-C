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

int main(void)
{
    printf("=== sio unit tests ===\n");
    test_digital_pad_released();
    test_digital_pad_pressed();
    test_slot2_no_device();
    printf("======================\n");
    printf("pass: %d  fail: %d\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
