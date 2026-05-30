#include "exe.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static uint32_t read_u32le(const uint8_t *buf, int offset) {
    return (uint32_t)buf[offset + 0]
         | (uint32_t)buf[offset + 1] << 8
         | (uint32_t)buf[offset + 2] << 16
         | (uint32_t)buf[offset + 3] << 24;
}

int exe_parse(const char *path, PsxExe *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "exe: cannot open '%s'\n", path);
        return -1;
    }

    uint8_t header[PSX_EXE_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fprintf(stderr, "exe: file too short (< 2048 bytes): %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (memcmp(header, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "exe: bad magic in '%s'\n", path);
        return -1;
    }

    out->pc        = read_u32le(header, 0x10);
    out->gp        = read_u32le(header, 0x14);
    out->load_addr = read_u32le(header, 0x18);
    out->load_size = read_u32le(header, 0x1C);

    uint32_t sp_base   = read_u32le(header, 0x30);
    uint32_t sp_offset = read_u32le(header, 0x34);

    if (sp_base == 0) {
        /* "No Stack!" executables (ps1-tests built with PSn00bSDK) */
        out->sp = PSX_DEFAULT_SP;
    } else {
        out->sp = sp_base + sp_offset;
    }

    return 0;
}

int exe_load(const char *path, const PsxExe *info, uint8_t *ram, uint32_t ram_size) {
    /* load_addr is a KSEG0/KUSEG virtual address; strip upper bits */
    uint32_t dest = info->load_addr & 0x1FFFFF;

    if ((uint64_t)dest + info->load_size > ram_size) {
        fprintf(stderr, "exe: load region [0x%08X, +0x%X] exceeds RAM size\n",
                info->load_addr, info->load_size);
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "exe: cannot open '%s' for loading\n", path);
        return -1;
    }

    if (fseek(f, PSX_EXE_HEADER_SIZE, SEEK_SET) != 0) {
        fprintf(stderr, "exe: seek failed\n");
        fclose(f);
        return -1;
    }

    size_t n = fread(ram + dest, 1, info->load_size, f);
    fclose(f);

    if (n != info->load_size) {
        fprintf(stderr, "exe: expected %u bytes, got %zu\n", info->load_size, n);
        return -1;
    }

    return 0;
}
