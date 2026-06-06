#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

uint8_t to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
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

/* ---- Raw .bin single-track open ---- */
static int disc_open_bin(Disc *disc, const char *path)
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

    disc->track_count = 1;
    disc->tracks[1].start_lba    = 0;
    disc->tracks[1].fp           = disc->fp;
    disc->tracks[1].file_offset  = 0;
    disc->tracks[1].file_size    = size;
    disc->tracks[1].sector_size  = DISC_SECTOR_SIZE;
    disc->tracks[1].data_offset  = DISC_DATA_OFFSET;
    disc->tracks[1].type         = DISC_TRACK_DATA;
    disc->sector_count = (uint32_t)(size / DISC_SECTOR_SIZE);
    return 0;
}

/* ---- CUE parser ---- */

static void str_trim(char *s)
{
    /* left */
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i]))
        i++;
    if (i)
        memmove(s, s + i, strlen(s + i) + 1);
    /* right */
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

/* Extract a quoted or unquoted token from *p, write into buf, advance *p. */
static bool next_token(const char **p, char *buf, size_t bufsz)
{
    while (**p && isspace((unsigned char)**p))
        (*p)++;
    if (!**p)
        return false;
    size_t i = 0;
    if (**p == '"')
    {
        (*p)++;
        while (**p && **p != '"' && i + 1 < bufsz)
            buf[i++] = *(*p)++;
        if (**p == '"')
            (*p)++;
    }
    else
    {
        while (**p && !isspace((unsigned char)**p) && i + 1 < bufsz)
            buf[i++] = *(*p)++;
    }
    buf[i] = '\0';
    return i > 0;
}

static int disc_open_cue(Disc *disc, const char *cue_path)
{
    FILE *cue = fopen(cue_path, "r");
    if (!cue)
    {
        fprintf(stderr, "disc_open: cannot open cue '%s'\n", cue_path);
        return -1;
    }

    /* Directory of the .cue file for resolving relative bin paths. */
    char cue_dir[4096] = "";
    {
        const char *slash = strrchr(cue_path, '/');
        if (!slash)
            slash = strrchr(cue_path, '\\');
        if (slash)
        {
            size_t dlen = (size_t)(slash - cue_path + 1);
            if (dlen < sizeof(cue_dir))
            {
                memcpy(cue_dir, cue_path, dlen);
                cue_dir[dlen] = '\0';
            }
        }
    }

    memset(disc, 0, sizeof(*disc));
    disc->fp = NULL;
    disc->track_count = 0;

    char line[1024];
    uint8_t cur_track = 0;
    uint32_t running_lba = 0;  /* LBA where the next track will start */
    long bin_offset = 0;       /* byte offset in the bin file for current track */
    FILE *cur_fp = NULL;
    long cur_file_size = 0;

    while (fgets(line, sizeof(line), cue))
    {
        str_trim(line);
        const char *p = line;
        char kw[64];
        if (!next_token(&p, kw, sizeof(kw)))
            continue;

        /* FILE <name> BINARY */
        if (strcasecmp(kw, "FILE") == 0)
        {
            char fname[4096], ftype[32];
            next_token(&p, fname, sizeof(fname));
            next_token(&p, ftype, sizeof(ftype));

            char full[8192];
            snprintf(full, sizeof(full), "%s%s", cue_dir, fname);
            cur_fp = fopen(full, "rb");
            if (!cur_fp)
            {
                fprintf(stderr, "disc_open: cannot open bin '%s'\n", full);
                fclose(cue);
                return -1;
            }
            if (!disc->fp)
                disc->fp = cur_fp;
            fseek(cur_fp, 0, SEEK_END);
            cur_file_size = ftell(cur_fp);
            rewind(cur_fp);
            bin_offset = 0;
        }
        /* TRACK <n> <mode> */
        else if (strcasecmp(kw, "TRACK") == 0)
        {
            char num_s[8], mode_s[32];
            next_token(&p, num_s, sizeof(num_s));
            next_token(&p, mode_s, sizeof(mode_s));

            cur_track = (uint8_t)atoi(num_s);
            if (cur_track < 1 || cur_track > DISC_MAX_TRACKS)
                continue;

            if (cur_track > disc->track_count)
                disc->track_count = cur_track;

            DiscTrack *t = &disc->tracks[cur_track];
            t->start_lba   = running_lba;
            t->fp          = cur_fp ? cur_fp : disc->fp;
            t->file_offset = bin_offset;
            t->file_size   = cur_file_size;

            /* Determine sector geometry from mode string. */
            if (strcasecmp(mode_s, "AUDIO") == 0)
            {
                t->sector_size = 2352;
                t->data_offset = 0;
                t->type        = DISC_TRACK_AUDIO;
            }
            else if (strcasecmp(mode_s, "MODE1/2048") == 0)
            {
                t->sector_size = 2048;
                t->data_offset = 16;
                t->type        = DISC_TRACK_DATA;
            }
            else if (strcasecmp(mode_s, "MODE1/2352") == 0)
            {
                t->sector_size = 2352;
                t->data_offset = 16;
                t->type        = DISC_TRACK_DATA;
            }
            else /* MODE2/2352 and variants */
            {
                t->sector_size = 2352;
                t->data_offset = DISC_DATA_OFFSET; /* 24 */
                t->type        = DISC_TRACK_DATA;
            }
        }
        /* INDEX <n> <mm:ss:ff> */
        else if (strcasecmp(kw, "INDEX") == 0)
        {
            char idx_s[8], msf_s[16];
            next_token(&p, idx_s, sizeof(idx_s));
            next_token(&p, msf_s, sizeof(msf_s));

            if (atoi(idx_s) != 1 || cur_track < 1)
                continue;

            /* Parse mm:ss:ff (not BCD, plain decimal in cue files). */
            unsigned mm = 0, ss = 0, ff = 0;
            sscanf(msf_s, "%u:%u:%u", &mm, &ss, &ff);
            uint32_t lba = (mm * 60 + ss) * 75 + ff;

            DiscTrack *t = &disc->tracks[cur_track];
            t->start_lba   = lba;
            /* File byte offset = lba * sector_size for single-file images. */
            t->file_offset = (long)lba * (long)t->sector_size;
            running_lba    = lba;
        }
    }
    fclose(cue);

    if (!disc->fp || disc->track_count == 0)
    {
        fprintf(stderr, "disc_open: invalid or empty cue '%s'\n", cue_path);
        if (disc->fp) fclose(disc->fp);
        return -1;
    }

    bool single_file = true;
    for (uint8_t t = 2; t <= disc->track_count; t++)
    {
        if (disc->tracks[t].fp != disc->tracks[1].fp)
        {
            single_file = false;
            break;
        }
    }

    if (single_file)
    {
        uint32_t sz = disc->tracks[1].sector_size;
        disc->sector_count = sz > 0 ? (uint32_t)(disc->tracks[1].file_size / sz) : 0;
    }
    else
    {
        uint32_t next_lba = 0;
        for (uint8_t t = 1; t <= disc->track_count; t++)
        {
            DiscTrack *track = &disc->tracks[t];
            track->start_lba = next_lba;
            uint32_t sectors = 0;
            if (track->sector_size > 0 && track->file_size > track->file_offset)
                sectors = (uint32_t)((track->file_size - track->file_offset) /
                                     (long)track->sector_size);
            next_lba += sectors;
        }
        disc->sector_count = next_lba;
    }

    return 0;
}

static const DiscTrack *disc_track_for_lba(const Disc *disc, uint32_t lba)
{
    const DiscTrack *track = &disc->tracks[1];
    for (uint8_t t = 2; t <= disc->track_count; t++)
    {
        if (disc->tracks[t].start_lba <= lba)
            track = &disc->tracks[t];
    }
    return track;
}

int disc_open(Disc *disc, const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext && (strcasecmp(ext, ".cue") == 0))
        return disc_open_cue(disc, path);
    return disc_open_bin(disc, path);
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
    FILE *closed[DISC_MAX_TRACKS] = {0};
    uint8_t closed_count = 0;
    for (uint8_t t = 1; t <= disc->track_count; t++)
    {
        FILE *fp = disc->tracks[t].fp;
        if (!fp)
            continue;
        bool seen = false;
        for (uint8_t i = 0; i < closed_count; i++)
        {
            if (closed[i] == fp)
            {
                seen = true;
                break;
            }
        }
        if (!seen)
        {
            closed[closed_count++] = fp;
            fclose(fp);
        }
    }
    for (uint8_t t = 1; t <= disc->track_count; t++)
        disc->tracks[t].fp = NULL;
    disc->fp = NULL;
}

int disc_read_sector(Disc *disc, uint32_t lba, uint8_t buf[DISC_SECTOR_SIZE])
{
    if (!disc->fp || lba >= disc->sector_count)
    {
        memset(buf, 0, DISC_SECTOR_SIZE);
        return -1;
    }

    const DiscTrack *track = disc_track_for_lba(disc, lba);
    if (!track->fp)
    {
        memset(buf, 0, DISC_SECTOR_SIZE);
        return -1;
    }

    long offset = track->file_offset + (long)(lba - track->start_lba) * (long)track->sector_size;

    if (fseek(track->fp, offset, SEEK_SET) != 0)
    {
        memset(buf, 0, DISC_SECTOR_SIZE);
        return -1;
    }

    /* Read the raw sector from the file, then place it in a 2352-byte buffer. */
    if (track->sector_size == DISC_SECTOR_SIZE)
    {
        size_t got = fread(buf, 1, DISC_SECTOR_SIZE, track->fp);
        if (got < DISC_SECTOR_SIZE)
            memset(buf + got, 0, DISC_SECTOR_SIZE - got);
    }
    else
    {
        /* Smaller sector (e.g. MODE1/2048): build a synthetic 2352-byte sector. */
        memset(buf, 0, DISC_SECTOR_SIZE);
        /* Sync pattern */
        buf[0] = 0x00;
        memset(buf + 1, 0xFF, 10);
        buf[11] = 0x00;
        /* Header: MSF + mode */
        Msf hdr = msf_from_lba(lba);
        buf[12] = hdr.m; buf[13] = hdr.s; buf[14] = hdr.f;
        buf[15] = 0x01; /* Mode 1 */
        size_t got = fread(buf + 16, 1, track->sector_size, track->fp);
        if (got < track->sector_size)
            memset(buf + 16 + got, 0, track->sector_size - got);
    }

    return 0;
}

uint32_t disc_data_offset_for_lba(const Disc *disc, uint32_t lba)
{
    if (!disc || disc->track_count == 0)
        return DISC_DATA_OFFSET;
    return disc_track_for_lba(disc, lba)->data_offset;
}
