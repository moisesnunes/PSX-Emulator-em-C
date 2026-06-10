#include "sio.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static void test_card_path(char *path, size_t size, const char *suffix)
{
    snprintf(path, size, "/tmp/psx-sio-%ld-%s.mcr", (long)getpid(), suffix);
    unlink(path);
}

static uint8_t memcard_begin(Sio *sio, uint8_t command, uint16_t sector)
{
    select_slot(sio, false);
    EXPECT_EQ(exchange(sio, 0x81), 0xFFu);
    uint8_t flag = exchange(sio, command);
    EXPECT_EQ(exchange(sio, 0x00), 0x5Au);
    EXPECT_EQ(exchange(sio, 0x00), 0x5Du);
    EXPECT_EQ(exchange(sio, (uint8_t)(sector >> 8)), 0x00u);
    EXPECT_EQ(exchange(sio, (uint8_t)sector), (uint8_t)(sector >> 8));
    return flag;
}

static uint8_t memcard_write_sector(Sio *sio, uint16_t sector,
                                    const uint8_t *data, bool valid_checksum)
{
    memcard_begin(sio, 0x57, sector);
    uint8_t checksum = (uint8_t)(sector >> 8) ^ (uint8_t)sector;
    for (uint32_t i = 0; i < SIO_MEMCARD_SECTOR_SIZE; i++)
    {
        exchange(sio, data[i]);
        checksum ^= data[i];
    }
    exchange(sio, valid_checksum ? checksum : (uint8_t)(checksum ^ 0xFFu));
    EXPECT_EQ(exchange(sio, 0x00), 0x5Cu);
    EXPECT_EQ(exchange(sio, 0x00), 0x5Du);
    uint8_t status = exchange(sio, 0x00);
    deselect(sio);
    return status;
}

static void memcard_read_sector(Sio *sio, uint16_t sector, uint8_t *data)
{
    memcard_begin(sio, 0x52, sector);
    EXPECT_EQ(exchange(sio, 0x00), 0x5Cu);
    EXPECT_EQ(exchange(sio, 0x00), 0x5Du);
    EXPECT_EQ(exchange(sio, 0x00), (uint8_t)(sector >> 8));
    EXPECT_EQ(exchange(sio, 0x00), (uint8_t)sector);
    uint8_t checksum = (uint8_t)(sector >> 8) ^ (uint8_t)sector;
    for (uint32_t i = 0; i < SIO_MEMCARD_SECTOR_SIZE; i++)
    {
        data[i] = exchange(sio, 0x00);
        checksum ^= data[i];
    }
    EXPECT_EQ(exchange(sio, 0x00), checksum);
    EXPECT_EQ(exchange(sio, 0x00), 0x47u);
    deselect(sio);
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

static void test_port2_digital_pad(void)
{
    printf("test_port2_digital_pad ... ");
    Sio sio;
    sio_init(&sio);
    sio_set_port_controller_state(&sio, 1, SIO_PAD_CIRCLE);
    select_slot(&sio, true);

    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x42), 0x41u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x5Au);
    EXPECT_EQ(exchange(&sio, 0x00), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x00), 0xDFu);
    deselect(&sio);

    select_slot(&sio, false);
    exchange(&sio, 0x01);
    exchange(&sio, 0x42);
    exchange(&sio, 0x00);
    EXPECT_EQ(exchange(&sio, 0x00), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x00), 0xFFu);
    printf("ok\n");
}

static void test_port2_memcard_absent(void)
{
    printf("test_port2_memcard_absent ... ");
    Sio sio;
    sio_init(&sio);
    select_slot(&sio, true);

    EXPECT_EQ(exchange(&sio, 0x81), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x52), 0xFFu);
    printf("ok\n");
}

static void test_dualshock_analog_mode(void)
{
    printf("test_dualshock_analog_mode ... ");
    Sio sio;
    sio_init(&sio);
    sio_set_port_analog_state(&sio, 0, 0x10, 0x20, 0xE0, 0xF0);

    select_slot(&sio, false);
    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x43), 0x41u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x5Au);
    exchange(&sio, 0x01);
    for (int i = 0; i < 5; i++)
        exchange(&sio, 0x00);
    deselect(&sio);
    EXPECT_EQ(sio.pad.config_mode, 1u);

    select_slot(&sio, false);
    exchange(&sio, 0x01);
    EXPECT_EQ(exchange(&sio, 0x44), 0xF3u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x5Au);
    exchange(&sio, 0x01);
    exchange(&sio, 0x00);
    for (int i = 0; i < 4; i++)
        exchange(&sio, 0x00);
    deselect(&sio);
    EXPECT_EQ(sio.pad.analog_enabled, 1u);

    select_slot(&sio, false);
    exchange(&sio, 0x01);
    EXPECT_EQ(exchange(&sio, 0x45), 0xF3u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x5Au);
    EXPECT_EQ(exchange(&sio, 0x00), 0x01u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x02u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x01u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x02u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x01u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x00u);
    deselect(&sio);

    select_slot(&sio, false);
    exchange(&sio, 0x01);
    EXPECT_EQ(exchange(&sio, 0x4C), 0xF3u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x5Au);
    EXPECT_EQ(exchange(&sio, 0x01), 0x00u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x00u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x00u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x07u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x00u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x00u);
    deselect(&sio);

    select_slot(&sio, false);
    exchange(&sio, 0x01);
    exchange(&sio, 0x43);
    exchange(&sio, 0x00);
    exchange(&sio, 0x00);
    for (int i = 0; i < 5; i++)
        exchange(&sio, 0x00);
    deselect(&sio);
    EXPECT_EQ(sio.pad.config_mode, 0u);

    select_slot(&sio, false);
    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x42), 0x73u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x5Au);
    EXPECT_EQ(exchange(&sio, 0x00), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x00), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x00), 0xE0u);
    EXPECT_EQ(exchange(&sio, 0x00), 0xF0u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x10u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x20u);
    deselect(&sio);
    printf("ok\n");
}

static void test_unknown_pad_command_no_device(void)
{
    printf("test_unknown_pad_command_no_device ... ");
    Sio sio;
    sio_init(&sio);
    select_slot(&sio, false);

    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x99), 0xFFu);
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
    sio_step(&sio, &irq, 1088);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0080u) != 0, 1u);
    EXPECT_EQ(sio_load8(&sio, 0x00), 0xFFu);
    printf("ok\n");
}

static void test_memcard_create_and_format(void)
{
    printf("test_memcard_create_and_format ... ");
    char path[128];
    test_card_path(path, sizeof(path), "create");
    Sio sio;
    sio_init(&sio);

    EXPECT_EQ(sio_memory_card_load(&sio, path), 1u);
    EXPECT_EQ(sio.memcard_present, 1u);
    EXPECT_EQ(sio.memcard_new, 1u);
    EXPECT_EQ(sio.memcard_data[0], 'M');
    EXPECT_EQ(sio.memcard_data[1], 'C');

    FILE *file = fopen(path, "rb");
    EXPECT_EQ(file != NULL, 1u);
    if (file)
    {
        EXPECT_EQ(fseek(file, 0, SEEK_END), 0u);
        EXPECT_EQ(ftell(file), SIO_MEMCARD_SIZE);
        fclose(file);
    }
    unlink(path);
    printf("ok\n");
}

static void test_memcard_corrupt_size(void)
{
    printf("test_memcard_corrupt_size ... ");
    char path[128];
    test_card_path(path, sizeof(path), "corrupt");
    FILE *file = fopen(path, "wb");
    EXPECT_EQ(file != NULL, 1u);
    if (file)
    {
        fputs("not a card", file);
        fclose(file);
    }

    Sio sio;
    sio_init(&sio);
    EXPECT_EQ(sio_memory_card_load(&sio, path), (uint32_t)-1);
    EXPECT_EQ(sio.memcard_present, 0u);
    EXPECT_EQ(sio.memcard_corrupt, 1u);
    unlink(path);
    printf("ok\n");
}

static void test_memcard_corrupt_metadata(void)
{
    printf("test_memcard_corrupt_metadata ... ");
    char path[128];
    test_card_path(path, sizeof(path), "metadata");

    Sio created;
    sio_init(&created);
    EXPECT_EQ(sio_memory_card_load(&created, path), 1u);
    FILE *file = fopen(path, "r+b");
    EXPECT_EQ(file != NULL, 1u);
    if (file)
    {
        EXPECT_EQ(fseek(file, 1, SEEK_SET), 0u);
        EXPECT_EQ(fputc('X', file), 'X');
        fclose(file);
    }

    Sio loaded;
    sio_init(&loaded);
    EXPECT_EQ(sio_memory_card_load(&loaded, path), (uint32_t)-1);
    EXPECT_EQ(loaded.memcard_present, 0u);
    EXPECT_EQ(loaded.memcard_corrupt, 1u);
    unlink(path);
    printf("ok\n");
}

static void test_memcard_read_write_persistence(void)
{
    printf("test_memcard_read_write_persistence ... ");
    char path[128];
    test_card_path(path, sizeof(path), "rw");
    Sio sio;
    sio_init(&sio);
    EXPECT_EQ(sio_memory_card_load(&sio, path), 1u);

    uint8_t written[SIO_MEMCARD_SECTOR_SIZE];
    uint8_t read_back[SIO_MEMCARD_SECTOR_SIZE];
    for (uint32_t i = 0; i < sizeof(written); i++)
        written[i] = (uint8_t)(i ^ 0xA5u);

    EXPECT_EQ(memcard_write_sector(&sio, 42, written, true), 0x47u);
    memset(read_back, 0, sizeof(read_back));
    memcard_read_sector(&sio, 42, read_back);
    EXPECT_EQ(memcmp(written, read_back, sizeof(written)), 0u);
    EXPECT_EQ(sio.memcard_new, 0u);

    Sio reloaded;
    sio_init(&reloaded);
    EXPECT_EQ(sio_memory_card_load(&reloaded, path), 0u);
    memset(read_back, 0, sizeof(read_back));
    memcard_read_sector(&reloaded, 42, read_back);
    EXPECT_EQ(memcmp(written, read_back, sizeof(written)), 0u);
    unlink(path);
    printf("ok\n");
}

static void test_memcard_rejects_bad_checksum(void)
{
    printf("test_memcard_rejects_bad_checksum ... ");
    char path[128];
    test_card_path(path, sizeof(path), "checksum");
    Sio sio;
    sio_init(&sio);
    EXPECT_EQ(sio_memory_card_load(&sio, path), 1u);

    uint8_t original[SIO_MEMCARD_SECTOR_SIZE];
    uint8_t replacement[SIO_MEMCARD_SECTOR_SIZE];
    uint8_t read_back[SIO_MEMCARD_SECTOR_SIZE];
    memcard_read_sector(&sio, 7, original);
    memset(replacement, 0x5A, sizeof(replacement));
    EXPECT_EQ(memcard_write_sector(&sio, 7, replacement, false), 0x4Eu);
    memcard_read_sector(&sio, 7, read_back);
    EXPECT_EQ(memcmp(original, read_back, sizeof(original)), 0u);
    unlink(path);
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
    EXPECT_EQ(sio_cycles_until_event(&sio), 1088u);
    EXPECT_EQ((irq.status & IRQ_SIO) != 0, 0u);
    sio_step(&sio, &irq, 1087);
    EXPECT_EQ((irq.status & IRQ_SIO) != 0, 0u);
    sio_step(&sio, &irq, 1);
    EXPECT_EQ((irq.status & IRQ_SIO) != 0, 1u);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0200u) != 0, 1u);

    sio_store16(&sio, 0x0A, (uint16_t)(sio.ctrl | 0x0010u), &irq);
    EXPECT_EQ(sio.irq_pending, 0u);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0200u) != 0, 0u);
    printf("ok\n");
}

static void test_ack_timing_uses_mode_and_baud(void)
{
    printf("test_ack_timing_uses_mode_and_baud ... ");
    Sio sio;
    sio_init(&sio);
    select_slot(&sio, false);

    sio_store16(&sio, 0x0E, 10, NULL);
    sio_store16(&sio, 0x08, 0, NULL);
    exchange(&sio, 0x01);
    EXPECT_EQ(sio_cycles_until_event(&sio), 80u);

    sio_store16(&sio, 0x08, 2, NULL);
    exchange(&sio, 0x42);
    EXPECT_EQ(sio_cycles_until_event(&sio), 1280u);

    sio_store16(&sio, 0x08, 3, NULL);
    exchange(&sio, 0x00);
    EXPECT_EQ(sio_cycles_until_event(&sio), 5120u);

    sio_store16(&sio, 0x0E, 0, NULL);
    sio_store16(&sio, 0x08, 1, NULL);
    exchange(&sio, 0x00);
    EXPECT_EQ(sio_cycles_until_event(&sio), 8u);
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
    sio_step(&sio, &irq, 544);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0200u) != 0, 0u);

    sio_store16(&sio, 0x0A, 0x0010u, &irq);
    EXPECT_EQ(sio.selected, 1u);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0200u) != 0, 0u);
    sio_step(&sio, &irq, 544);
    EXPECT_EQ((sio_load16(&sio, 0x04) & 0x0200u) != 0, 1u);
    sio_store16(&sio, 0x0A, 0x0010u, &irq);
    EXPECT_EQ(exchange(&sio, 0x42), 0x41u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x5Au);
    deselect(&sio);
    printf("ok\n");
}

static void test_ack_reset_can_select_pad(void)
{
    printf("test_ack_reset_can_select_pad ... ");
    Sio sio;
    sio_init(&sio);

    sio_store16(&sio, 0x0A, 0x0013u, NULL);
    EXPECT_EQ(sio.selected, 1u);
    EXPECT_EQ(exchange(&sio, 0x01), 0xFFu);
    EXPECT_EQ(exchange(&sio, 0x42), 0x41u);
    EXPECT_EQ(exchange(&sio, 0x00), 0x5Au);
    deselect(&sio);
    printf("ok\n");
}

static void test_clear_keyboard_state(void)
{
    printf("test_clear_keyboard_state ... ");
    Sio sio;
    sio_init(&sio);

    sio_on_key(&sio, SDL_SCANCODE_Z, true);
    sio_set_controller_state(&sio, SIO_PAD_CIRCLE);
    sio_clear_keyboard_state(&sio);
    EXPECT_EQ((sio.pad.buttons & SIO_PAD_CROSS), (uint32_t)SIO_PAD_CROSS);
    EXPECT_EQ((sio.pad.buttons & SIO_PAD_CIRCLE), 0u);
    printf("ok\n");
}

int main(void)
{
    printf("=== sio unit tests ===\n");
    test_digital_pad_released();
    test_digital_pad_pressed();
    test_keyboard_and_controller_sources_merge();
    test_forced_buttons_merge();
    test_port2_digital_pad();
    test_port2_memcard_absent();
    test_dualshock_analog_mode();
    test_unknown_pad_command_no_device();
    test_memcard_command_no_device();
    test_memcard_command_no_ack();
    test_memcard_create_and_format();
    test_memcard_corrupt_size();
    test_memcard_corrupt_metadata();
    test_memcard_read_write_persistence();
    test_memcard_rejects_bad_checksum();
    test_pad_ack_irq();
    test_ack_timing_uses_mode_and_baud();
    test_ack_reset_keeps_selected_transaction();
    test_ack_reset_can_select_pad();
    test_clear_keyboard_state();
    printf("======================\n");
    printf("pass: %d  fail: %d\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
