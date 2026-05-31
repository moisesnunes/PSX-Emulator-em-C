#ifndef EXE_H
#define EXE_H

#include <stdint.h>

/*
 * PS-X EXE format layout (little-endian):
 *   0x000  magic[8]    "PS-X EXE"
 *   0x010  pc          initial PC
 *   0x014  gp          initial GP (r28)
 *   0x018  load_addr   destination in RAM
 *   0x01C  load_size   bytes to copy (must be multiple of 2048)
 *   0x030  sp_base     initial SP base (r29/r30); 0 means "not set"
 *   0x034  sp_offset   added to sp_base for final SP value
 *   0x800  payload...
 *
 * sp_base == 0 with sp_offset == 0 → "No Stack!" executables from ps1-tests.
 * We default SP to 0x801FFFF0 in that case (top of 2 MiB RAM, 16-byte aligned).
 */

#define PSX_EXE_HEADER_SIZE 0x800
#define PSX_DEFAULT_SP 0x801FFFF0u

typedef struct
{
   uint32_t pc;
   uint32_t gp;
   uint32_t load_addr;
   uint32_t load_size;
   uint32_t sp; /* final SP value, already computed */
} PsxExe;

/* Parse header and return info. Does NOT load payload into RAM.
   Returns 0 on success, -1 on error (bad magic, short file). */
int exe_parse(const char *path, PsxExe *out);

/* Load payload bytes into `ram` (uint8_t array of at least 2 MiB).
   Caller must have called exe_parse first.
   Returns 0 on success, -1 on error. */
int exe_load(const char *path, const PsxExe *info, uint8_t *ram, uint32_t ram_size);

#endif /* EXE_H */
