#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t to_bcd(uint8_t v) { return (uint8_t)((v / 10) << 4) | (v % 10); }
uint8_t from_bcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0xF)); }

Msf msf_from_lba(uint32_t lba)
{
    lba += DISC_LEAD_IN_SECTORS;
    Msf msf;
    msf.f = to_bcd((uint8_t)(lba % 75));
    msf.s = to_bcd((uint8_t)((lba / 75) % 60));
    msf.m = to_bcd((uint8_t)(lba / 75 / 60));
    return msf;
}

uint32_t msf_to_lba(Msf msf)
{
    uint32_t m = from_bcd(msf.m);
    uint32_t s = from_bcd(msf.s);
    uint32_t f = from_bcd(msf.f);
    return (m * 60 + s) * 75 + f - DISC_LEAD_IN_SECTORS;
}

int disc_open(Disc *disc, const char *path)
{
    disc->fp = fopen(path, "rb");
    if (!disc->fp)
    {
        fprintf(stderr, "disc_open: cannot open '%s'\n", path);
        return -1;
    }
    fseek(disc->fp, 0, SEEK_END);
    long size = ftell(disc->fp);
    rewind(disc->fp);
    disc->sector_count = (uint32_t)(size / DISC_SECTOR_SIZE);

    /* Build a minimal single-track TOC.
       PS1 data discs are almost always single-track (track 1 = data).
       Track 1 starts at LBA 0 (absolute sector DISC_LEAD_IN_SECTORS for MSF).
       A proper CUE parser would populate all tracks; for now one track suffices
       for GetTN/GetTD to work with standard data discs. */
    disc->track_count = 1;
    disc->tracks[1].start_lba = 0; /* LBA 0 = first user sector */

    return 0;
}

uint8_t disc_track_count_bcd(const Disc *disc)
{
    return to_bcd(disc->track_count);
}

Msf disc_track_start_msf(const Disc *disc, uint8_t track_bcd)
{
    uint8_t t = from_bcd(track_bcd);
    /* Track 0 = lead-out: report end of disc */
    if (t == 0)
        return msf_from_lba(disc->sector_count > DISC_LEAD_IN_SECTORS
                            ? disc->sector_count - DISC_LEAD_IN_SECTORS
                            : 0);
    if (t < 1 || t > disc->track_count)
        return msf_from_lba(0);
    return msf_from_lba(disc->tracks[t].start_lba);
}

void disc_close(Disc *disc)
{
    if (disc->fp)
    {
        fclose(disc->fp);
        disc->fp = NULL;
    }
}

int disc_read_sector(Disc *disc, uint32_t lba, uint8_t buf[DISC_SECTOR_SIZE])
{
    if (!disc->fp || lba >= disc->sector_count)
    {
        memset(buf, 0, DISC_SECTOR_SIZE);
        return -1;
    }
    long offset = (long)lba * DISC_SECTOR_SIZE;
    if (fseek(disc->fp, offset, SEEK_SET) != 0)
        return -1;
    size_t got = fread(buf, 1, DISC_SECTOR_SIZE, disc->fp);
    if (got < DISC_SECTOR_SIZE)
        memset(buf + got, 0, DISC_SECTOR_SIZE - got);
    return 0;
}
