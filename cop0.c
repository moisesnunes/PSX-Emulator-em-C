#include "cop0.h"
#include <stdio.h>

/* Inicializa todos os registradores do COP0 com zero (estado de reset). */
void cop0_init(Cop0 *cop0)
{
    cop0->sr = 0;
    cop0->cause = 0;
    cop0->epc = 0;
    cop0->bad_vaddr = 0;
}

/* Lê um registrador do COP0 pelo número (MFC0).
 * Apenas os registradores implementados são retornados; demais emitem aviso. */
uint32_t cop0_read(const Cop0 *cop0, uint32_t reg)
{
    switch (reg)
    {
    case 8:
        return cop0->bad_vaddr; /* BadVAddr — endereço que causou a exceção de memória */
    case 12:
        return cop0->sr; /* Status Register */
    case 13:
        return cop0->cause; /* Cause Register */
    case 14:
        return cop0->epc; /* Exception Program Counter */
    default:
        printf("[COP0] Read from unhandled register %u\n", reg);
        return 0;
    }
}

/* Escreve em um registrador do COP0 pelo número (MTC0).
 * Registradores não utilizados pelo PSX geram aviso se recebem valor não-zero. */
void cop0_write(Cop0 *cop0, uint32_t reg, uint32_t val)
{
    switch (reg)
    {
    case 12:
        cop0->sr = val;
        break; /* Status Register */
    case 13:
        cop0->cause = val;
        break; /* Cause Register */
    /* Registers that should be zero — warn if non-zero */
    case 3:
    case 5:
    case 6:
    case 7:
    case 9:
    case 11:
        if (val != 0)
            printf("[COP0] Write 0x%08x to N/A register %u\n", val, reg);
        break;
    default:
        printf("[COP0] Write 0x%08x to unhandled register %u\n", val, reg);
        break;
    }
}

/* Retorna o bit BEV (Boot Exception Vectors) do Status Register (SR[22]).
 * Quando 1, vetores de exceção apontam para a ROM de boot (0xBFC00180). */
int cop0_status_bev(const Cop0 *cop0)
{
    return (cop0->sr >> 22) & 1;
}

/* Retorna o bit IsC (Isolate Cache) do Status Register (SR[16]).
 * Quando 1, acessos à memória atingem o cache de dados em vez da RAM/ROM. */
int cop0_cache_isolated(const Cop0 *cop0)
{
    return (cop0->sr >> 16) & 1;
}

void cop0_enter_exception(Cop0 *cop0, uint32_t cause_code,
                          uint32_t current_pc, int in_delay_slot)
{
    /* Shift mode stack: KUo|IEo ← KUp|IEp ← KUc|IEc ← 0 (exception mode) */
    uint32_t mode = cop0->sr & 0x3f;
    cop0->sr &= ~(uint32_t)0x3f;
    cop0->sr |= (mode << 2) & 0x3f;

    /* Store exception code in Cause[6:2] */
    cop0->cause = (cause_code << 2) & 0x7c;

    if (in_delay_slot)
    {
        /* EPC points to the branch instruction, not the delay slot */
        cop0->epc = current_pc - 4;
        cop0->cause |= (uint32_t)1 << 31; /* BD (Branch Delay) bit */
    }
    else
    {
        cop0->epc = current_pc;
    }
}

void cop0_return_from_exception(Cop0 *cop0)
{
    /* Pop mode stack: KUc|IEc ← KUp|IEp ← KUo|IEo */
    uint32_t mode = cop0->sr & 0x3f;
    cop0->sr &= ~(uint32_t)0x3f;
    cop0->sr |= mode >> 2;
}
