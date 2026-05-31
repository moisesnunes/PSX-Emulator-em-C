#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Raw 2352-byte sector layout for Mode 2 Form 1 (most PSX data) */
#define DISC_SECTOR_SIZE 2352
#define DISC_DATA_OFFSET 24      /* byte offset to user data within sector */
#define DISC_DATA_SIZE 2048      /* user data bytes per sector */
#define DISC_LEAD_IN_SECTORS 150 /* 2 seconds of pregap before track 1 */

typedef struct
{
    uint8_t m, s, f; /* BCD minutes, seconds, frames */
} Msf;

Msf msf_from_lba(uint32_t lba);
uint32_t msf_to_lba(Msf msf);
uint8_t to_bcd(uint8_t v);
uint8_t from_bcd(uint8_t v);

typedef struct
{
    FILE *fp;
    uint32_t sector_count; /* total sectors (including lead-in) */
} Disc;

/* Returns 0 on success, -1 on failure. */
int disc_open(Disc *disc, const char *path);
void disc_close(Disc *disc);

/* Read one raw 2352-byte sector at LBA `lba` into `buf`. */
int disc_read_sector(Disc *disc, uint32_t lba, uint8_t buf[DISC_SECTOR_SIZE]);
