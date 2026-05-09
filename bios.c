#include "bios.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* Carrega o arquivo de BIOS do caminho especificado para o buffer em memória.
 * Retorna 0 em sucesso, -1 em falha. */
int bios_load(Bios *bios, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "Cannot open BIOS '%s': %s\n", path, strerror(errno));
        return -1;
    }
    size_t n = fread(bios->data, 1, BIOS_SIZE, f);
    fclose(f);
    /* O arquivo deve ter exatamente BIOS_SIZE bytes; qualquer diferença indica
     * um dump inválido ou corrompido. */
    if (n != BIOS_SIZE)
    {
        fprintf(stderr, "BIOS size mismatch: expected %d bytes, got %zu\n", BIOS_SIZE, n);
        return -1;
    }
    return 0;
}

/* Lê uma word de 32 bits em little-endian a partir do offset dado. */
uint32_t bios_load32(const Bios *bios, uint32_t offset)
{
    return (uint32_t)bios->data[offset] | ((uint32_t)bios->data[offset + 1] << 8) | ((uint32_t)bios->data[offset + 2] << 16) | ((uint32_t)bios->data[offset + 3] << 24);
}

/* Lê uma half-word de 16 bits em little-endian a partir do offset dado. */
uint16_t bios_load16(const Bios *bios, uint32_t offset)
{
    return (uint16_t)bios->data[offset] | ((uint16_t)bios->data[offset + 1] << 8);
}

/* Lê um byte a partir do offset dado. */
uint8_t bios_load8(const Bios *bios, uint32_t offset)
{
    return bios->data[offset];
}
