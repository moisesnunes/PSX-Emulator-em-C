#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Raw 2352-byte sector layout for Mode 2 Form 1 (most PSX data) */
#define DISC_SECTOR_SIZE 2352
#define DISC_DATA_OFFSET 24      /* byte offset to user data within sector */
#define DISC_DATA_SIZE 2048      /* user data bytes per sector */
#define DISC_LEAD_IN_SECTORS 150 /* 2 seconds of pregap before track 1 */
#define DISC_MAX_TRACKS 99

typedef struct
{
    uint8_t m, s, f; /* BCD minutes, seconds, frames */
} Msf;

Msf msf_from_lba(uint32_t lba);
uint32_t msf_to_lba(Msf msf);
uint8_t to_bcd(uint8_t v);
uint8_t from_bcd(uint8_t v);

typedef enum
{
    DISC_TRACK_DATA,  /* MODE1/2048, MODE2/2352, etc. */
    DISC_TRACK_AUDIO, /* AUDIO */
} DiscTrackType;

typedef struct
{
    uint32_t start_lba; /* logical disc LBA of track start; MSF adds lead-in */
    FILE *fp;
    long file_offset; /* byte offset within fp where this track begins */
    long file_size;
    uint32_t sector_size; /* bytes per sector in the image file */
    uint32_t data_offset; /* byte offset to user data within a sector */
    DiscTrackType type;
} DiscTrack;

typedef struct
{
    FILE *fp;
    uint32_t sector_count;                 /* total sectors (including lead-in) */
    DiscTrack tracks[DISC_MAX_TRACKS + 1]; /* tracks[1..track_count]; tracks[0] unused */
    uint8_t track_count;
} Disc;

/* Returns 0 on success, -1 on failure. */
int disc_open(Disc *disc, const char *path);
void disc_close(Disc *disc);

/* Read one raw 2352-byte sector at LBA `lba` into `buf`. */
int disc_read_sector(Disc *disc, uint32_t lba, uint8_t buf[DISC_SECTOR_SIZE]);
uint32_t disc_data_offset_for_lba(const Disc *disc, uint32_t lba);

/* Track table helpers (used by CDROM GetTN/GetTD). */
uint8_t disc_track_count_bcd(const Disc *disc);
Msf disc_track_start_msf(const Disc *disc, uint8_t track_bcd);
