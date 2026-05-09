#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Exception codes (COP0 Cause ExcCode field)                          */
/* ------------------------------------------------------------------ */
typedef enum
{
    EXC_INTERRUPT = 0x00,
    EXC_LOAD_ADDRESS_ERROR = 0x04,
    EXC_STORE_ADDRESS_ERROR = 0x05,
    EXC_SYSCALL = 0x08,
    EXC_BREAK = 0x09,
    EXC_ILLEGAL_INSTRUCTION = 0x0a,
    EXC_COPROCESSOR_ERROR = 0x0b,
    EXC_OVERFLOW = 0x0c,
} Exception;

/* ------------------------------------------------------------------ */
/* Instruction field accessors                                          */
/* ------------------------------------------------------------------ */
static inline uint32_t ins_opcode(uint32_t ins) { return ins >> 26; }
static inline uint32_t ins_rs(uint32_t ins) { return (ins >> 21) & 0x1f; }
static inline uint32_t ins_rt(uint32_t ins) { return (ins >> 16) & 0x1f; }
static inline uint32_t ins_rd(uint32_t ins) { return (ins >> 11) & 0x1f; }
static inline uint32_t ins_shamt(uint32_t ins) { return (ins >> 6) & 0x1f; }
static inline uint32_t ins_funct(uint32_t ins) { return ins & 0x3f; }
static inline uint32_t ins_imm_u(uint32_t ins) { return ins & 0xffff; }
static inline uint32_t ins_imm_se(uint32_t ins) { return (uint32_t)(int32_t)(int16_t)(ins & 0xffff); }
static inline uint32_t ins_jump(uint32_t ins) { return ins & 0x3ffffff; }
static inline uint32_t ins_cop_op(uint32_t ins) { return (ins >> 21) & 0x1f; }

/* ------------------------------------------------------------------ */
/* Register file helpers                                               */
/* ------------------------------------------------------------------ */
static inline uint32_t reg_get(const RegisterFile *rf, uint32_t idx)
{
    return rf->r[idx];
}

static inline void reg_set(RegisterFile *rf, uint32_t idx, uint32_t val)
{
    rf->r[idx] = val;
    rf->r[0] = 0; /* $zero is always 0 */
}

/* ------------------------------------------------------------------ */
/* CPU helpers                                                         */
/* ------------------------------------------------------------------ */
static inline uint32_t cpu_reg(const Cpu *cpu, uint32_t idx)
{
    return reg_get(&cpu->regs, idx);
}

static inline void cpu_set_reg(Cpu *cpu, uint32_t idx, uint32_t val)
{
    reg_set(&cpu->regs, idx, val);
}

/* Returns register value accounting for any pending load-delay slot */
static inline uint32_t cpu_load_delay_val(const Cpu *cpu, uint32_t idx)
{
    if (cpu->load_delay_reg == idx)
        return cpu->load_delay_val;
    return reg_get(&cpu->regs, idx);
}

static inline uint32_t cpu_load32(const Cpu *cpu, uint32_t addr)
{
    return inter_load32(&((Cpu *)cpu)->inter, addr);
}
static inline uint16_t cpu_load16(const Cpu *cpu, uint32_t addr)
{
    return inter_load16(&((Cpu *)cpu)->inter, addr);
}
static inline uint8_t cpu_load8(const Cpu *cpu, uint32_t addr)
{
    return inter_load8(&((Cpu *)cpu)->inter, addr);
}

static void cpu_store32(Cpu *cpu, uint32_t addr, uint32_t val)
{
    if (cop0_cache_isolated(&cpu->cop0))
        return;
    inter_store32(&cpu->inter, addr, val);
}
static void cpu_store16(Cpu *cpu, uint32_t addr, uint16_t val)
{
    if (cop0_cache_isolated(&cpu->cop0))
        return;
    inter_store16(&cpu->inter, addr, val);
}
static void cpu_store8(Cpu *cpu, uint32_t addr, uint8_t val)
{
    if (cop0_cache_isolated(&cpu->cop0))
        return;
    inter_store8(&cpu->inter, addr, val);
}

/* ------------------------------------------------------------------ */
/* Exception entry                                                     */
/* ------------------------------------------------------------------ */
static void cpu_exception(Cpu *cpu, Exception cause)
{
    uint32_t handler = cop0_status_bev(&cpu->cop0)
                           ? 0xbfc00180  /* boot exception vector */
                           : 0x80000080; /* normal exception vector */

    cop0_enter_exception(&cpu->cop0, (uint32_t)cause,
                         cpu->current_pc, cpu->delay_slot);
    cpu->pc = handler;
    cpu->next_pc = handler + 4;
    cpu->branch = 0;
    cpu->delay_slot = 0;
}

/* ------------------------------------------------------------------ */
/* Branch helper                                                       */
/* ------------------------------------------------------------------ */
static void cpu_branch(Cpu *cpu, uint32_t ins)
{
    uint32_t offset = ins_imm_se(ins) << 2;
    cpu->branch = 1;
    cpu->next_pc = cpu->pc + offset; /* pc already advanced past branch */
}

/* ================================================================== */
/* INSTRUCTION IMPLEMENTATIONS                                         */
/* ================================================================== */

/* LUI */
static void op_lui(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rt(ins), ins_imm_u(ins) << 16);
}

/* ORI */
static void op_ori(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rt(ins), cpu_reg(cpu, ins_rs(ins)) | ins_imm_u(ins));
}

/* ANDI */
static void op_andi(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rt(ins), cpu_reg(cpu, ins_rs(ins)) & ins_imm_u(ins));
}

/* XORI */
static void op_xori(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rt(ins), cpu_reg(cpu, ins_rs(ins)) ^ ins_imm_u(ins));
}

/* ADDIU — no overflow exception */
static void op_addiu(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rt(ins), cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins));
}

/* ADDI — overflow causes exception */
static void op_addi(Cpu *cpu, uint32_t ins)
{
    int32_t a = (int32_t)cpu_reg(cpu, ins_rs(ins));
    int32_t b = (int32_t)ins_imm_se(ins);
    int32_t result;
    if (__builtin_add_overflow(a, b, &result))
        cpu_exception(cpu, EXC_OVERFLOW);
    else
        cpu_set_reg(cpu, ins_rt(ins), (uint32_t)result);
}

/* SLTI */
static void op_slti(Cpu *cpu, uint32_t ins)
{
    int32_t a = (int32_t)cpu_reg(cpu, ins_rs(ins));
    int32_t b = (int32_t)ins_imm_se(ins);
    cpu_set_reg(cpu, ins_rt(ins), a < b ? 1 : 0);
}

/* SLTIU */
static void op_sltiu(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rt(ins),
                cpu_reg(cpu, ins_rs(ins)) < ins_imm_se(ins) ? 1 : 0);
}

/* LW */
static void op_lw(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    if (addr % 4 != 0)
    {
        cpu_exception(cpu, EXC_LOAD_ADDRESS_ERROR);
        return;
    }
    uint32_t v = cpu_load32(cpu, addr);
    cpu->load_delay_reg = ins_rt(ins);
    cpu->load_delay_val = v;
}

/* LB */
static void op_lb(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    uint32_t v = (uint32_t)(int32_t)(int8_t)cpu_load8(cpu, addr);
    cpu->load_delay_reg = ins_rt(ins);
    cpu->load_delay_val = v;
}

/* LBU */
static void op_lbu(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    cpu->load_delay_reg = ins_rt(ins);
    cpu->load_delay_val = cpu_load8(cpu, addr);
}

/* LH */
static void op_lh(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    if (addr % 2 != 0)
    {
        cpu_exception(cpu, EXC_LOAD_ADDRESS_ERROR);
        return;
    }
    uint32_t v = (uint32_t)(int32_t)(int16_t)cpu_load16(cpu, addr);
    cpu->load_delay_reg = ins_rt(ins);
    cpu->load_delay_val = v;
}

/* LHU */
static void op_lhu(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    if (addr % 2 != 0)
    {
        cpu_exception(cpu, EXC_LOAD_ADDRESS_ERROR);
        return;
    }
    cpu->load_delay_reg = ins_rt(ins);
    cpu->load_delay_val = cpu_load16(cpu, addr);
}

/* LWL */
static void op_lwl(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    uint32_t cur = cpu_load_delay_val(cpu, ins_rt(ins));
    uint32_t aligned = addr & ~(uint32_t)3;
    uint32_t word = cpu_load32(cpu, aligned);
    uint32_t v;
    switch (addr & 3)
    {
    case 0:
        v = (cur & 0x00ffffff) | (word << 24);
        break;
    case 1:
        v = (cur & 0x0000ffff) | (word << 16);
        break;
    case 2:
        v = (cur & 0x000000ff) | (word << 8);
        break;
    case 3:
        v = word;
        break;
    default:
        abort();
    }
    cpu->load_delay_reg = ins_rt(ins);
    cpu->load_delay_val = v;
}

/* LWR */
static void op_lwr(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    uint32_t cur = cpu_load_delay_val(cpu, ins_rt(ins));
    uint32_t aligned = addr & ~(uint32_t)3;
    uint32_t word = cpu_load32(cpu, aligned);
    uint32_t v;
    switch (addr & 3)
    {
    case 0:
        v = word;
        break;
    case 1:
        v = (cur & 0xff000000) | (word >> 8);
        break;
    case 2:
        v = (cur & 0xffff0000) | (word >> 16);
        break;
    case 3:
        v = (cur & 0xffffff00) | (word >> 24);
        break;
    default:
        abort();
    }
    cpu->load_delay_reg = ins_rt(ins);
    cpu->load_delay_val = v;
}

/* SW */
static void op_sw(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    if (addr % 4 != 0)
    {
        cpu_exception(cpu, EXC_STORE_ADDRESS_ERROR);
        return;
    }
    cpu_store32(cpu, addr, cpu_reg(cpu, ins_rt(ins)));
}

/* SH */
static void op_sh(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    if (addr % 2 != 0)
    {
        cpu_exception(cpu, EXC_STORE_ADDRESS_ERROR);
        return;
    }
    cpu_store16(cpu, addr, (uint16_t)cpu_reg(cpu, ins_rt(ins)));
}

/* SB */
static void op_sb(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    cpu_store8(cpu, addr, (uint8_t)cpu_reg(cpu, ins_rt(ins)));
}

/* SWL */
static void op_swl(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    uint32_t val = cpu_reg(cpu, ins_rt(ins));
    uint32_t aligned = addr & ~(uint32_t)3;
    uint32_t mem = cpu_load32(cpu, aligned);
    uint32_t v;
    switch (addr & 3)
    {
    case 0:
        v = (mem & 0xffffff00) | (val >> 24);
        break;
    case 1:
        v = (mem & 0xffff0000) | (val >> 16);
        break;
    case 2:
        v = (mem & 0xff000000) | (val >> 8);
        break;
    case 3:
        v = val;
        break;
    default:
        abort();
    }
    cpu_store32(cpu, aligned, v);
}

/* SWR */
static void op_swr(Cpu *cpu, uint32_t ins)
{
    uint32_t addr = cpu_reg(cpu, ins_rs(ins)) + ins_imm_se(ins);
    uint32_t val = cpu_reg(cpu, ins_rt(ins));
    uint32_t aligned = addr & ~(uint32_t)3;
    uint32_t mem = cpu_load32(cpu, aligned);
    uint32_t v;
    switch (addr & 3)
    {
    case 0:
        v = val;
        break;
    case 1:
        v = (mem & 0x000000ff) | (val << 8);
        break;
    case 2:
        v = (mem & 0x0000ffff) | (val << 16);
        break;
    case 3:
        v = (mem & 0x00ffffff) | (val << 24);
        break;
    default:
        abort();
    }
    cpu_store32(cpu, aligned, v);
}

/* SLL */
static void op_sll(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rd(ins), cpu_reg(cpu, ins_rt(ins)) << ins_shamt(ins));
}

/* SRL */
static void op_srl(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rd(ins), cpu_reg(cpu, ins_rt(ins)) >> ins_shamt(ins));
}

/* SRA */
static void op_sra(Cpu *cpu, uint32_t ins)
{
    int32_t v = (int32_t)cpu_reg(cpu, ins_rt(ins)) >> ins_shamt(ins);
    cpu_set_reg(cpu, ins_rd(ins), (uint32_t)v);
}

/* SLLV */
static void op_sllv(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rd(ins),
                cpu_reg(cpu, ins_rt(ins)) << (cpu_reg(cpu, ins_rs(ins)) & 0x1f));
}

/* SRLV */
static void op_srlv(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rd(ins),
                cpu_reg(cpu, ins_rt(ins)) >> (cpu_reg(cpu, ins_rs(ins)) & 0x1f));
}

/* SRAV */
static void op_srav(Cpu *cpu, uint32_t ins)
{
    int32_t v = (int32_t)cpu_reg(cpu, ins_rt(ins)) >> (cpu_reg(cpu, ins_rs(ins)) & 0x1f);
    cpu_set_reg(cpu, ins_rd(ins), (uint32_t)v);
}

/* ADD */
static void op_add(Cpu *cpu, uint32_t ins)
{
    int32_t a = (int32_t)cpu_reg(cpu, ins_rs(ins));
    int32_t b = (int32_t)cpu_reg(cpu, ins_rt(ins));
    int32_t r;
    if (__builtin_add_overflow(a, b, &r))
        cpu_exception(cpu, EXC_OVERFLOW);
    else
        cpu_set_reg(cpu, ins_rd(ins), (uint32_t)r);
}

/* ADDU */
static void op_addu(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rd(ins),
                cpu_reg(cpu, ins_rs(ins)) + cpu_reg(cpu, ins_rt(ins)));
}

/* SUB */
static void op_sub(Cpu *cpu, uint32_t ins)
{
    int32_t a = (int32_t)cpu_reg(cpu, ins_rs(ins));
    int32_t b = (int32_t)cpu_reg(cpu, ins_rt(ins));
    int32_t r;
    if (__builtin_sub_overflow(a, b, &r))
        cpu_exception(cpu, EXC_OVERFLOW);
    else
        cpu_set_reg(cpu, ins_rd(ins), (uint32_t)r);
}

/* SUBU */
static void op_subu(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rd(ins),
                cpu_reg(cpu, ins_rs(ins)) - cpu_reg(cpu, ins_rt(ins)));
}

/* AND */
static void op_and(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rd(ins),
                cpu_reg(cpu, ins_rs(ins)) & cpu_reg(cpu, ins_rt(ins)));
}

/* OR */
static void op_or(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rd(ins),
                cpu_reg(cpu, ins_rs(ins)) | cpu_reg(cpu, ins_rt(ins)));
}

/* XOR */
static void op_xor(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rd(ins),
                cpu_reg(cpu, ins_rs(ins)) ^ cpu_reg(cpu, ins_rt(ins)));
}

/* NOR */
static void op_nor(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rd(ins),
                ~(cpu_reg(cpu, ins_rs(ins)) | cpu_reg(cpu, ins_rt(ins))));
}

/* SLT */
static void op_slt(Cpu *cpu, uint32_t ins)
{
    int32_t a = (int32_t)cpu_reg(cpu, ins_rs(ins));
    int32_t b = (int32_t)cpu_reg(cpu, ins_rt(ins));
    cpu_set_reg(cpu, ins_rd(ins), a < b ? 1 : 0);
}

/* SLTU */
static void op_sltu(Cpu *cpu, uint32_t ins)
{
    cpu_set_reg(cpu, ins_rd(ins),
                cpu_reg(cpu, ins_rs(ins)) < cpu_reg(cpu, ins_rt(ins)) ? 1 : 0);
}

/* MULT */
static void op_mult(Cpu *cpu, uint32_t ins)
{
    int64_t a = (int64_t)(int32_t)cpu_reg(cpu, ins_rs(ins));
    int64_t b = (int64_t)(int32_t)cpu_reg(cpu, ins_rt(ins));
    uint64_t v = (uint64_t)(a * b);
    cpu->hi = (uint32_t)(v >> 32);
    cpu->lo = (uint32_t)v;
}

/* MULTU */
static void op_multu(Cpu *cpu, uint32_t ins)
{
    uint64_t a = cpu_reg(cpu, ins_rs(ins));
    uint64_t b = cpu_reg(cpu, ins_rt(ins));
    uint64_t v = a * b;
    cpu->hi = (uint32_t)(v >> 32);
    cpu->lo = (uint32_t)v;
}

/* DIV */
static void op_div(Cpu *cpu, uint32_t ins)
{
    int32_t n = (int32_t)cpu_reg(cpu, ins_rs(ins));
    int32_t d = (int32_t)cpu_reg(cpu, ins_rt(ins));
    if (d == 0)
    {
        cpu->hi = (uint32_t)n;
        cpu->lo = (n >= 0) ? 0xffffffff : 1;
    }
    else if (n == INT32_MIN && d == -1)
    {
        cpu->hi = 0;
        cpu->lo = (uint32_t)n;
    }
    else
    {
        cpu->hi = (uint32_t)(n % d);
        cpu->lo = (uint32_t)(n / d);
    }
}

/* DIVU */
static void op_divu(Cpu *cpu, uint32_t ins)
{
    uint32_t n = cpu_reg(cpu, ins_rs(ins));
    uint32_t d = cpu_reg(cpu, ins_rt(ins));
    if (d == 0)
    {
        cpu->hi = n;
        cpu->lo = 0xffffffff;
    }
    else
    {
        cpu->hi = n % d;
        cpu->lo = n / d;
    }
}

/* MFHI / MTHI / MFLO / MTLO */
static void op_mfhi(Cpu *cpu, uint32_t ins) { cpu_set_reg(cpu, ins_rd(ins), cpu->hi); }
static void op_mthi(Cpu *cpu, uint32_t ins) { cpu->hi = cpu_reg(cpu, ins_rs(ins)); }
static void op_mflo(Cpu *cpu, uint32_t ins) { cpu_set_reg(cpu, ins_rd(ins), cpu->lo); }
static void op_mtlo(Cpu *cpu, uint32_t ins) { cpu->lo = cpu_reg(cpu, ins_rs(ins)); }

/* J */
static void op_j(Cpu *cpu, uint32_t ins)
{
    cpu->branch = 1;
    cpu->next_pc = (cpu->pc & 0xf0000000) | (ins_jump(ins) << 2);
}

/* JAL */
static void op_jal(Cpu *cpu, uint32_t ins)
{
    uint32_t ra = cpu->next_pc;
    op_j(cpu, ins);
    cpu_set_reg(cpu, 31, ra);
}

/* JR */
static void op_jr(Cpu *cpu, uint32_t ins)
{
    cpu->branch = 1;
    cpu->next_pc = cpu_reg(cpu, ins_rs(ins));
}

/* JALR */
static void op_jalr(Cpu *cpu, uint32_t ins)
{
    uint32_t ra = cpu->next_pc;
    cpu->branch = 1;
    cpu->next_pc = cpu_reg(cpu, ins_rs(ins));
    cpu_set_reg(cpu, ins_rd(ins), ra);
}

/* BEQ */
static void op_beq(Cpu *cpu, uint32_t ins)
{
    if (cpu_reg(cpu, ins_rs(ins)) == cpu_reg(cpu, ins_rt(ins)))
        cpu_branch(cpu, ins);
}

/* BNE */
static void op_bne(Cpu *cpu, uint32_t ins)
{
    if (cpu_reg(cpu, ins_rs(ins)) != cpu_reg(cpu, ins_rt(ins)))
        cpu_branch(cpu, ins);
}

/* BLEZ */
static void op_blez(Cpu *cpu, uint32_t ins)
{
    if ((int32_t)cpu_reg(cpu, ins_rs(ins)) <= 0)
        cpu_branch(cpu, ins);
}

/* BGTZ */
static void op_bgtz(Cpu *cpu, uint32_t ins)
{
    if ((int32_t)cpu_reg(cpu, ins_rs(ins)) > 0)
        cpu_branch(cpu, ins);
}

/* BcondZ — covers BLTZ / BGEZ / BLTZAL / BGEZAL */
static void op_bcond(Cpu *cpu, uint32_t ins)
{
    uint32_t cond = ins_rt(ins);
    int bltz = (cond & 0x01) == 0;
    int link = (cond & 0x10) != 0;
    int32_t val = (int32_t)cpu_reg(cpu, ins_rs(ins));
    int test = bltz ? (val < 0) : (val >= 0);

    if (link)
    {
        uint32_t ra = cpu->next_pc;
        cpu_set_reg(cpu, 31, ra);
    }
    if (test)
        cpu_branch(cpu, ins);
}

/* ------------------------------------------------------------------ */
/* COP0                                                                */
/* ------------------------------------------------------------------ */

static void op_mfc0(Cpu *cpu, uint32_t ins)
{
    uint32_t v = cop0_read(&cpu->cop0, ins_rd(ins));
    cpu->load_delay_reg = ins_rt(ins);
    cpu->load_delay_val = v;
}

static void op_mtc0(Cpu *cpu, uint32_t ins)
{
    cop0_write(&cpu->cop0, ins_rd(ins), cpu_reg(cpu, ins_rt(ins)));
}

static void op_rfe(Cpu *cpu, uint32_t ins)
{
    if ((ins & 0x3f) != 0x10)
    {
        fprintf(stderr, "[CPU] Invalid RFE encoding: %08x\n", ins);
        abort();
    }
    cop0_return_from_exception(&cpu->cop0);
}

static void op_cop0(Cpu *cpu, uint32_t ins)
{
    switch (ins_cop_op(ins))
    {
    case 0x00:
        op_mfc0(cpu, ins);
        break;
    case 0x04:
        op_mtc0(cpu, ins);
        break;
    case 0x10:
        op_rfe(cpu, ins);
        break;
    default:
        fprintf(stderr, "[CPU] Unhandled COP0 instruction: %08x\n", ins);
        abort();
    }
}

static void op_cop2(Cpu *cpu, uint32_t ins)
{
    (void)cpu;
    fprintf(stderr, "[CPU] GTE (COP2) not implemented: %08x\n", ins);
    abort();
}

/* ------------------------------------------------------------------ */
/* SPECIAL sub-decode (opcode == 0x00)                                 */
/* ------------------------------------------------------------------ */
static void op_special(Cpu *cpu, uint32_t ins)
{
    switch (ins_funct(ins))
    {
    case 0x00:
        op_sll(cpu, ins);
        break;
    case 0x02:
        op_srl(cpu, ins);
        break;
    case 0x03:
        op_sra(cpu, ins);
        break;
    case 0x04:
        op_sllv(cpu, ins);
        break;
    case 0x06:
        op_srlv(cpu, ins);
        break;
    case 0x07:
        op_srav(cpu, ins);
        break;
    case 0x08:
        op_jr(cpu, ins);
        break;
    case 0x09:
        op_jalr(cpu, ins);
        break;
    case 0x0c:
        cpu_exception(cpu, EXC_SYSCALL);
        break;
    case 0x0d:
        cpu_exception(cpu, EXC_BREAK);
        break;
    case 0x10:
        op_mfhi(cpu, ins);
        break;
    case 0x11:
        op_mthi(cpu, ins);
        break;
    case 0x12:
        op_mflo(cpu, ins);
        break;
    case 0x13:
        op_mtlo(cpu, ins);
        break;
    case 0x18:
        op_mult(cpu, ins);
        break;
    case 0x19:
        op_multu(cpu, ins);
        break;
    case 0x1a:
        op_div(cpu, ins);
        break;
    case 0x1b:
        op_divu(cpu, ins);
        break;
    case 0x20:
        op_add(cpu, ins);
        break;
    case 0x21:
        op_addu(cpu, ins);
        break;
    case 0x22:
        op_sub(cpu, ins);
        break;
    case 0x23:
        op_subu(cpu, ins);
        break;
    case 0x24:
        op_and(cpu, ins);
        break;
    case 0x25:
        op_or(cpu, ins);
        break;
    case 0x26:
        op_xor(cpu, ins);
        break;
    case 0x27:
        op_nor(cpu, ins);
        break;
    case 0x2a:
        op_slt(cpu, ins);
        break;
    case 0x2b:
        op_sltu(cpu, ins);
        break;
    default:
        printf("[CPU] Illegal SPECIAL %08x at PC %08x\n",
               ins, cpu->current_pc);
        cpu_exception(cpu, EXC_ILLEGAL_INSTRUCTION);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Main decode/execute                                                 */
/* ------------------------------------------------------------------ */
static void decode_and_execute(Cpu *cpu, uint32_t ins)
{
    switch (ins_opcode(ins))
    {
    case 0x00:
        op_special(cpu, ins);
        break;
    case 0x01:
        op_bcond(cpu, ins);
        break;
    case 0x02:
        op_j(cpu, ins);
        break;
    case 0x03:
        op_jal(cpu, ins);
        break;
    case 0x04:
        op_beq(cpu, ins);
        break;
    case 0x05:
        op_bne(cpu, ins);
        break;
    case 0x06:
        op_blez(cpu, ins);
        break;
    case 0x07:
        op_bgtz(cpu, ins);
        break;
    case 0x08:
        op_addi(cpu, ins);
        break;
    case 0x09:
        op_addiu(cpu, ins);
        break;
    case 0x0a:
        op_slti(cpu, ins);
        break;
    case 0x0b:
        op_sltiu(cpu, ins);
        break;
    case 0x0c:
        op_andi(cpu, ins);
        break;
    case 0x0d:
        op_ori(cpu, ins);
        break;
    case 0x0e:
        op_xori(cpu, ins);
        break;
    case 0x0f:
        op_lui(cpu, ins);
        break;
    case 0x10:
        op_cop0(cpu, ins);
        break;
    case 0x12:
        op_cop2(cpu, ins);
        break;
    case 0x20:
        op_lb(cpu, ins);
        break;
    case 0x21:
        op_lh(cpu, ins);
        break;
    case 0x22:
        op_lwl(cpu, ins);
        break;
    case 0x23:
        op_lw(cpu, ins);
        break;
    case 0x24:
        op_lbu(cpu, ins);
        break;
    case 0x25:
        op_lhu(cpu, ins);
        break;
    case 0x26:
        op_lwr(cpu, ins);
        break;
    case 0x28:
        op_sb(cpu, ins);
        break;
    case 0x29:
        op_sh(cpu, ins);
        break;
    case 0x2a:
        op_swl(cpu, ins);
        break;
    case 0x2b:
        op_sw(cpu, ins);
        break;
    case 0x2e:
        op_swr(cpu, ins);
        break;
    /* COP0/COP2 load-store — COP0 always generates CoprocessorError,
       COP2 (GTE) is a stub for now                                  */
    case 0x30:
        cpu_exception(cpu, EXC_COPROCESSOR_ERROR);
        break;
    case 0x32: /* LWC2 — GTE, stub */
        break;
    case 0x38:
        cpu_exception(cpu, EXC_COPROCESSOR_ERROR);
        break;
    case 0x3a: /* SWC2 — GTE, stub */
        break;
    default:
        printf("[CPU] Illegal opcode %08x at PC %08x\n",
               ins, cpu->current_pc);
        cpu_exception(cpu, EXC_ILLEGAL_INSTRUCTION);
        break;
    }
}

/* ================================================================== */
/* PUBLIC API                                                          */
/* ================================================================== */

void cpu_init(Cpu *cpu, Bios bios)
{
    uint32_t i;
    for (i = 0; i < NUM_REGS; i++)
        cpu->regs.r[i] = 0xdeadbeef;
    cpu->regs.r[0] = 0;

    cpu->pc = 0xbfc00000;
    cpu->next_pc = 0xbfc00004;
    cpu->current_pc = 0xbfc00000;
    cpu->hi = 0xdeadbeef;
    cpu->lo = 0xdeadbeef;
    cpu->load_delay_reg = 0;
    cpu->load_delay_val = 0;
    cpu->branch = 0;
    cpu->delay_slot = 0;

    cop0_init(&cpu->cop0);
    inter_init(&cpu->inter, bios);
}

uint32_t cpu_pc(const Cpu *cpu)
{
    return cpu->pc;
}

void cpu_run_next_instruction(Cpu *cpu)
{
    cpu->current_pc = cpu->pc;

    /* PC must be word-aligned */
    if (cpu->current_pc % 4 != 0)
    {
        cpu_exception(cpu, EXC_LOAD_ADDRESS_ERROR);
        return;
    }

    /* Fetch */
    uint32_t ins = inter_load32(&cpu->inter, cpu->pc);

    /* Advance PC */
    cpu->pc = cpu->next_pc;
    cpu->next_pc = cpu->next_pc + 4;

    /* Commit pending load-delay slot value */
    reg_set(&cpu->regs, cpu->load_delay_reg, cpu->load_delay_val);
    cpu->load_delay_reg = 0;
    cpu->load_delay_val = 0;

    /* Track delay slot flag */
    cpu->delay_slot = cpu->branch;
    cpu->branch = 0;

    decode_and_execute(cpu, ins);
}
