#include "disc.h"

#include <stdio.h>
#include <string.h>

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

static int write_bytes(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t wrote = fwrite(data, 1, len, f);
    fclose(f);
    return wrote == len ? 0 : -1;
}

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

static void test_cue_mode1_2048(void)
{
    printf("test_cue_mode1_2048 ... ");
    const char *bin_path = "/tmp/psx_disc_mode1_2048.bin";
    const char *cue_path = "/tmp/psx_disc_mode1_2048.cue";
    uint8_t data[2048 * 2];
    memset(data, 0, sizeof(data));
    data[0] = 0x12;
    data[1] = 0x34;
    data[2048] = 0x56;
    EXPECT_EQ(write_bytes(bin_path, data, sizeof(data)), 0u);
    EXPECT_EQ(write_text(cue_path,
                         "FILE \"psx_disc_mode1_2048.bin\" BINARY\n"
                         "  TRACK 01 MODE1/2048\n"
                         "    INDEX 01 00:00:00\n"),
              0u);

    Disc disc;
    EXPECT_EQ(disc_open(&disc, cue_path), 0u);
    EXPECT_EQ(disc.track_count, 1u);
    EXPECT_EQ(disc.tracks[1].sector_size, 2048u);
    EXPECT_EQ(disc_data_offset_for_lba(&disc, 0), 16u);

    uint8_t sector[DISC_SECTOR_SIZE];
    EXPECT_EQ(disc_read_sector(&disc, 0, sector), 0u);
    EXPECT_EQ(sector[15], 0x01u);
    EXPECT_EQ(sector[16], 0x12u);
    EXPECT_EQ(sector[17], 0x34u);
    EXPECT_EQ(disc_read_sector(&disc, 1, sector), 0u);
    EXPECT_EQ(sector[16], 0x56u);
    disc_close(&disc);
    remove(bin_path);
    remove(cue_path);
    printf("ok\n");
}

static void test_cue_multitrack_audio(void)
{
    printf("test_cue_multitrack_audio ... ");
    const char *bin_path = "/tmp/psx_disc_multitrack.bin";
    const char *cue_path = "/tmp/psx_disc_multitrack.cue";
    uint8_t data[DISC_SECTOR_SIZE * 3];
    memset(data, 0, sizeof(data));
    data[DISC_SECTOR_SIZE * 2 + 0] = 0xA5;
    data[DISC_SECTOR_SIZE * 2 + 1] = 0x5A;
    EXPECT_EQ(write_bytes(bin_path, data, sizeof(data)), 0u);
    EXPECT_EQ(write_text(cue_path,
                         "FILE \"psx_disc_multitrack.bin\" BINARY\n"
                         "  TRACK 01 MODE2/2352\n"
                         "    INDEX 01 00:00:00\n"
                         "  TRACK 02 AUDIO\n"
                         "    INDEX 01 00:00:02\n"),
              0u);

    Disc disc;
    EXPECT_EQ(disc_open(&disc, cue_path), 0u);
    EXPECT_EQ(disc.track_count, 2u);
    EXPECT_EQ(disc.tracks[2].start_lba, 2u);
    EXPECT_EQ(disc_data_offset_for_lba(&disc, 2), 0u);

    uint8_t sector[DISC_SECTOR_SIZE];
    EXPECT_EQ(disc_read_sector(&disc, 2, sector), 0u);
    EXPECT_EQ(sector[0], 0xA5u);
    EXPECT_EQ(sector[1], 0x5Au);
    disc_close(&disc);
    remove(bin_path);
    remove(cue_path);
    printf("ok\n");
}

static void test_cue_multifile_audio(void)
{
    printf("test_cue_multifile_audio ... ");
    const char *track1_path = "/tmp/psx_disc_multi_file_track1.bin";
    const char *track2_path = "/tmp/psx_disc_multi_file_track2.bin";
    const char *cue_path = "/tmp/psx_disc_multi_file.cue";
    uint8_t track1[DISC_SECTOR_SIZE * 2];
    uint8_t track2[DISC_SECTOR_SIZE * 152];
    memset(track1, 0, sizeof(track1));
    memset(track2, 0, sizeof(track2));
    track1[0] = 0x11;
    track2[DISC_SECTOR_SIZE * 150 + 0] = 0x22;
    track2[DISC_SECTOR_SIZE * 150 + 1] = 0x33;
    EXPECT_EQ(write_bytes(track1_path, track1, sizeof(track1)), 0u);
    EXPECT_EQ(write_bytes(track2_path, track2, sizeof(track2)), 0u);
    EXPECT_EQ(write_text(cue_path,
                         "FILE \"psx_disc_multi_file_track1.bin\" BINARY\n"
                         "  TRACK 01 MODE2/2352\n"
                         "    INDEX 01 00:00:00\n"
                         "FILE \"psx_disc_multi_file_track2.bin\" BINARY\n"
                         "  TRACK 02 AUDIO\n"
                         "    INDEX 00 00:00:00\n"
                         "    INDEX 01 00:02:00\n"),
              0u);

    Disc disc;
    EXPECT_EQ(disc_open(&disc, cue_path), 0u);
    EXPECT_EQ(disc.track_count, 2u);
    EXPECT_EQ(disc.tracks[1].start_lba, 0u);
    EXPECT_EQ(disc.tracks[2].start_lba, 2u);
    EXPECT_EQ(disc.tracks[2].file_offset, (long)(DISC_SECTOR_SIZE * 150));
    EXPECT_EQ(disc.sector_count, 4u);

    uint8_t sector[DISC_SECTOR_SIZE];
    EXPECT_EQ(disc_read_sector(&disc, 2, sector), 0u);
    EXPECT_EQ(sector[0], 0x22u);
    EXPECT_EQ(sector[1], 0x33u);
    disc_close(&disc);
    remove(track1_path);
    remove(track2_path);
    remove(cue_path);
    printf("ok\n");
}

int main(void)
{
    printf("=== disc unit tests ===\n");
    test_cue_mode1_2048();
    test_cue_multitrack_audio();
    test_cue_multifile_audio();
    printf("=======================\n");
    printf("pass: %d  fail: %d\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
