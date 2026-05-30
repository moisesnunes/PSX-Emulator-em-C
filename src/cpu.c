#include "cpu.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Exception codes ---- */
typedef enum {
    EXC_INTERRUPT           = 0x00,
    EXC_LOAD_ADDRESS_ERROR  = 0x04,
    EXC_STORE_ADDRESS_ERROR = 0x05,
    EXC_SYSCALL             = 0x08,
    EXC_BREAK               = 0x09,
    EXC_ILLEGAL_INSTRUCTION = 0x0A,
    EXC_COPROCESSOR_ERROR   = 0x0B,
    EXC_OVERFLOW            = 0x0C,
} Exception;

/* ---- Instruction field helpers ---- */
static inline uint32_t instr_function(uint32_t op)    { return op >> 26; }
static inline uint32_t instr_s(uint32_t op)           { return (op >> 21) & 0x1F; }
static inline uint32_t instr_t(uint32_t op)           { return (op >> 16) & 0x1F; }
static inline uint32_t instr_d(uint32_t op)           { return (op >> 11) & 0x1F; }
static inline uint32_t instr_imm(uint32_t op)         { return op & 0xFFFF; }
static inline uint32_t instr_imm_se(uint32_t op)      { return (uint32_t)(int32_t)(int16_t)(op & 0xFFFF); }
static inline uint32_t instr_shift(uint32_t op)       { return (op >> 6) & 0x1F; }
static inline uint32_t instr_subfunction(uint32_t op) { return op & 0x3F; }
static inline uint32_t instr_imm_jump(uint32_t op)    { return op & 0x03FFFFFF; }
static inline uint32_t instr_cop_opcode(uint32_t op)  { return (op >> 21) & 0x1F; }

/* ---- CPU helpers ---- */
static inline uint32_t cpu_reg(const Cpu *cpu, uint32_t idx) { return cpu->regs[idx]; }

static inline void cpu_set_reg(Cpu *cpu, uint32_t idx, uint32_t val) {
    cpu->out_regs[idx] = val;
    cpu->out_regs[0]   = 0;
}

static uint32_t cpu_load32(Cpu *cpu, uint32_t addr) {
    return interconnect_load32(&cpu->inter, addr);
}
static uint16_t cpu_load16(Cpu *cpu, uint32_t addr) {
    return interconnect_load16(&cpu->inter, addr);
}
static uint8_t cpu_load8(Cpu *cpu, uint32_t addr) {
    return interconnect_load8(&cpu->inter, addr);
}
static void cpu_store32(Cpu *cpu, uint32_t addr, uint32_t val) {
    interconnect_store32(&cpu->inter, addr, val);
}
static void cpu_store16(Cpu *cpu, uint32_t addr, uint16_t val) {
    interconnect_store16(&cpu->inter, addr, val);
}
static void cpu_store8(Cpu *cpu, uint32_t addr, uint8_t val) {
    interconnect_store8(&cpu->inter, addr, val);
}

static void cpu_branch(Cpu *cpu, uint32_t offset) {
    cpu->next_pc = cpu->pc + (offset << 2);
    cpu->branch  = true;
}

static void cpu_exception(Cpu *cpu, Exception cause) {
    uint32_t handler = (cpu->sr & (1 << 22)) ? 0xBFC00180 : 0x80000080;
    uint32_t mode = cpu->sr & 0x3F;
    cpu->sr = (cpu->sr & ~0x3Fu) | ((mode << 2) & 0x3F);
    cpu->cause = (uint32_t)cause << 2;
    cpu->epc   = cpu->current_pc;
    if (cpu->delay_slot) {
        cpu->epc   -= 4;
        cpu->cause |= (1u << 31);
    }
    cpu->pc      = handler;
    cpu->next_pc = handler + 4;
}

/* ---- Opcode handlers (forward declarations) ---- */
static void op_lui(Cpu *cpu, uint32_t op);
static void op_ori(Cpu *cpu, uint32_t op);
static void op_sw(Cpu *cpu, uint32_t op);
static void op_sll(Cpu *cpu, uint32_t op);
static void op_addiu(Cpu *cpu, uint32_t op);
static void op_j(Cpu *cpu, uint32_t op);
static void op_or(Cpu *cpu, uint32_t op);
static void op_cop0(Cpu *cpu, uint32_t op);
static void op_mtc0(Cpu *cpu, uint32_t op);
static void op_bne(Cpu *cpu, uint32_t op);
static void op_addi(Cpu *cpu, uint32_t op);
static void op_lw(Cpu *cpu, uint32_t op);
static void op_sltu(Cpu *cpu, uint32_t op);
static void op_addu(Cpu *cpu, uint32_t op);
static void op_sh(Cpu *cpu, uint32_t op);
static void op_jal(Cpu *cpu, uint32_t op);
static void op_andi(Cpu *cpu, uint32_t op);
static void op_sb(Cpu *cpu, uint32_t op);
static void op_jr(Cpu *cpu, uint32_t op);
static void op_lb(Cpu *cpu, uint32_t op);
static void op_beq(Cpu *cpu, uint32_t op);
static void op_mfc0(Cpu *cpu, uint32_t op);
static void op_and(Cpu *cpu, uint32_t op);
static void op_add(Cpu *cpu, uint32_t op);
static void op_bgtz(Cpu *cpu, uint32_t op);
static void op_blez(Cpu *cpu, uint32_t op);
static void op_lbu(Cpu *cpu, uint32_t op);
static void op_jalr(Cpu *cpu, uint32_t op);
static void op_bxx(Cpu *cpu, uint32_t op);
static void op_slti(Cpu *cpu, uint32_t op);
static void op_subu(Cpu *cpu, uint32_t op);
static void op_sra(Cpu *cpu, uint32_t op);
static void op_div(Cpu *cpu, uint32_t op);
static void op_mflo(Cpu *cpu, uint32_t op);
static void op_srl(Cpu *cpu, uint32_t op);
static void op_sltiu(Cpu *cpu, uint32_t op);
static void op_divu(Cpu *cpu, uint32_t op);
static void op_mfhi(Cpu *cpu, uint32_t op);
static void op_slt(Cpu *cpu, uint32_t op);
static void op_syscall(Cpu *cpu, uint32_t op);
static void op_mtlo(Cpu *cpu, uint32_t op);
static void op_mthi(Cpu *cpu, uint32_t op);
static void op_rfe(Cpu *cpu, uint32_t op);
static void op_lhu(Cpu *cpu, uint32_t op);
static void op_sllv(Cpu *cpu, uint32_t op);
static void op_lh(Cpu *cpu, uint32_t op);
static void op_nor(Cpu *cpu, uint32_t op);
static void op_srav(Cpu *cpu, uint32_t op);
static void op_srlv(Cpu *cpu, uint32_t op);
static void op_multu(Cpu *cpu, uint32_t op);
static void op_xor(Cpu *cpu, uint32_t op);
static void op_break(Cpu *cpu, uint32_t op);
static void op_mult(Cpu *cpu, uint32_t op);
static void op_sub(Cpu *cpu, uint32_t op);
static void op_xori(Cpu *cpu, uint32_t op);
static void op_lwl(Cpu *cpu, uint32_t op);
static void op_lwr(Cpu *cpu, uint32_t op);
static void op_swl(Cpu *cpu, uint32_t op);
static void op_swr(Cpu *cpu, uint32_t op);
static void op_illegal(Cpu *cpu, uint32_t op);

/* ---- Decode and execute ---- */
static void decode_and_execute(Cpu *cpu, uint32_t op) {
    switch (instr_function(op)) {
        case 0x00:
            switch (instr_subfunction(op)) {
                case 0x00: op_sll(cpu, op);  break;
                case 0x02: op_srl(cpu, op);  break;
                case 0x03: op_sra(cpu, op);  break;
                case 0x04: op_sllv(cpu, op); break;
                case 0x06: op_srlv(cpu, op); break;
                case 0x07: op_srav(cpu, op); break;
                case 0x08: op_jr(cpu, op);   break;
                case 0x09: op_jalr(cpu, op); break;
                case 0x0C: op_syscall(cpu, op); break;
                case 0x0D: op_break(cpu, op); break;
                case 0x10: op_mfhi(cpu, op); break;
                case 0x11: op_mthi(cpu, op); break;
                case 0x12: op_mflo(cpu, op); break;
                case 0x13: op_mtlo(cpu, op); break;
                case 0x18: op_mult(cpu, op);  break;
                case 0x19: op_multu(cpu, op); break;
                case 0x1A: op_div(cpu, op);   break;
                case 0x1B: op_divu(cpu, op);  break;
                case 0x20: op_add(cpu, op);   break;
                case 0x21: op_addu(cpu, op);  break;
                case 0x22: op_sub(cpu, op);   break;
                case 0x23: op_subu(cpu, op);  break;
                case 0x24: op_and(cpu, op);   break;
                case 0x25: op_or(cpu, op);    break;
                case 0x26: op_xor(cpu, op);   break;
                case 0x27: op_nor(cpu, op);   break;
                case 0x2A: op_slt(cpu, op);   break;
                case 0x2B: op_sltu(cpu, op);  break;
                default:   op_illegal(cpu, op); break;
            }
            break;
        case 0x01: op_bxx(cpu, op);   break;
        case 0x02: op_j(cpu, op);     break;
        case 0x03: op_jal(cpu, op);   break;
        case 0x04: op_beq(cpu, op);   break;
        case 0x05: op_bne(cpu, op);   break;
        case 0x06: op_blez(cpu, op);  break;
        case 0x07: op_bgtz(cpu, op);  break;
        case 0x08: op_addi(cpu, op);  break;
        case 0x09: op_addiu(cpu, op); break;
        case 0x0A: op_slti(cpu, op);  break;
        case 0x0B: op_sltiu(cpu, op); break;
        case 0x0C: op_andi(cpu, op);  break;
        case 0x0D: op_ori(cpu, op);   break;
        case 0x0E: op_xori(cpu, op);  break;
        case 0x0F: op_lui(cpu, op);   break;
        case 0x10: op_cop0(cpu, op);  break;
        case 0x11: cpu_exception(cpu, EXC_COPROCESSOR_ERROR); break;
        case 0x12: /* COP2 / GTE — stub until Milestone 7 */ break;
        case 0x13: cpu_exception(cpu, EXC_COPROCESSOR_ERROR); break;
        case 0x20: op_lb(cpu, op);    break;
        case 0x21: op_lh(cpu, op);    break;
        case 0x22: op_lwl(cpu, op);   break;
        case 0x23: op_lw(cpu, op);    break;
        case 0x24: op_lbu(cpu, op);   break;
        case 0x25: op_lhu(cpu, op);   break;
        case 0x26: op_lwr(cpu, op);   break;
        case 0x28: op_sb(cpu, op);    break;
        case 0x29: op_sh(cpu, op);    break;
        case 0x2A: op_swl(cpu, op);   break;
        case 0x2B: op_sw(cpu, op);    break;
        case 0x2E: op_swr(cpu, op);   break;
        case 0x30: cpu_exception(cpu, EXC_COPROCESSOR_ERROR); break;
        case 0x31: cpu_exception(cpu, EXC_COPROCESSOR_ERROR); break;
        case 0x32: /* LWC2 / GTE — stub until Milestone 7 */ break;
        case 0x33: cpu_exception(cpu, EXC_COPROCESSOR_ERROR); break;
        case 0x38: cpu_exception(cpu, EXC_COPROCESSOR_ERROR); break;
        case 0x39: cpu_exception(cpu, EXC_COPROCESSOR_ERROR); break;
        case 0x3A: /* SWC2 / GTE — stub until Milestone 7 */ break;
        case 0x3B: cpu_exception(cpu, EXC_COPROCESSOR_ERROR); break;
        default:   op_illegal(cpu, op); break;
    }
}

/* ---- Init ---- */
int cpu_init(Cpu *cpu, const char *bios_path, SDL_Window *window, bool headless) {
    memset(cpu->regs, 0xDE, sizeof(cpu->regs));
    cpu->regs[0] = 0;
    memcpy(cpu->out_regs, cpu->regs, sizeof(cpu->regs));

    cpu->pc               = 0xBFC00000;
    cpu->next_pc          = 0xBFC00004;
    cpu->next_instruction = 0;
    cpu->sr               = 0;
    cpu->current_pc       = 0;
    cpu->cause            = 0;
    cpu->epc              = 0;
    cpu->load_reg         = 0;
    cpu->load_val         = 0;
    cpu->hi = cpu->lo     = 0xDEADBEEF;
    cpu->branch           = false;
    cpu->delay_slot       = false;

    return interconnect_init(&cpu->inter, bios_path, window, headless);
}

void cpu_destroy(Cpu *cpu) {
    interconnect_destroy(&cpu->inter);
}

/* ---- Main step ---- */
uint32_t cpu_run_next_instruction(Cpu *cpu) {
    /* Check for pending hardware interrupt before fetching the next instruction.
       SR bit 0 (IEc) enables interrupts at the CPU level. */
    if ((cpu->sr & 0x1) && irq_pending(&cpu->inter.irq)) {
        LOG(LOG_IRQ, "CPU interrupt: SR=0x%08x status=0x%04x mask=0x%04x",
            cpu->sr, cpu->inter.irq.status, cpu->inter.irq.mask);
        cpu_exception(cpu, EXC_INTERRUPT);
        return 2;
    }

    cpu->current_pc = cpu->pc;
    if (cpu->current_pc % 4 != 0) {
        cpu_exception(cpu, EXC_LOAD_ADDRESS_ERROR);
        return 2;
    }
    uint32_t op = cpu_load32(cpu, cpu->pc);
    cpu->pc      = cpu->next_pc;
    cpu->next_pc = cpu->next_pc + 4;

    cpu_set_reg(cpu, cpu->load_reg, cpu->load_val);
    cpu->load_reg = 0;
    cpu->load_val = 0;

    cpu->delay_slot = cpu->branch;
    cpu->branch     = false;

    decode_and_execute(cpu, op);
    memcpy(cpu->regs, cpu->out_regs, sizeof(cpu->regs));

    /* Approximate cycle cost: loads/stores cost more than ALU ops. */
    uint32_t fn = instr_function(op);
    if (fn == 0x20 || fn == 0x21 || fn == 0x22 || fn == 0x23 ||
        fn == 0x24 || fn == 0x25 || fn == 0x26 ||
        fn == 0x28 || fn == 0x29 || fn == 0x2A || fn == 0x2B || fn == 0x2E)
        return 4; /* loads and stores */
    return 2;     /* everything else */
}

/* ---- Opcode implementations ---- */
static void op_lui(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_t(op), instr_imm(op) << 16);
}

static void op_ori(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_t(op), cpu_reg(cpu, instr_s(op)) | instr_imm(op));
}

static void op_sw(Cpu *cpu, uint32_t op) {
    if (cpu->sr & 0x10000) return;
    uint32_t addr = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    if (addr % 4 != 0) { cpu_exception(cpu, EXC_STORE_ADDRESS_ERROR); return; }
    cpu_store32(cpu, addr, cpu_reg(cpu, instr_t(op)));
}

static void op_sll(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), cpu_reg(cpu, instr_t(op)) << instr_shift(op));
}

static void op_addiu(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_t(op), cpu_reg(cpu, instr_s(op)) + instr_imm_se(op));
}

static void op_j(Cpu *cpu, uint32_t op) {
    cpu->next_pc = (cpu->pc & 0xF0000000) | (instr_imm_jump(op) << 2);
    cpu->branch  = true;
}

static void op_or(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), cpu_reg(cpu, instr_s(op)) | cpu_reg(cpu, instr_t(op)));
}

static void op_mtc0(Cpu *cpu, uint32_t op) {
    uint32_t v    = cpu_reg(cpu, instr_t(op));
    uint32_t cop_r = instr_d(op);
    switch (cop_r) {
        case 3: case 5: case 6: case 7: case 9: case 11:
            if (v != 0) { fprintf(stderr, "Unhandled write to cop0r%u\n", cop_r); exit(1); }
            break;
        case 12: cpu->sr = v; break;
        case 13:
            if (v != 0) { fprintf(stderr, "Unhandled write to CAUSE\n"); exit(1); }
            break;
        default: fprintf(stderr, "Unhandled cop0 register: %u\n", cop_r); exit(1);
    }
}

static void op_cop0(Cpu *cpu, uint32_t op) {
    switch (instr_cop_opcode(op)) {
        case 0b00100: op_mtc0(cpu, op); break;
        case 0b00000: op_mfc0(cpu, op); break;
        case 0b10000: op_rfe(cpu, op);  break;
        default:
            fprintf(stderr, "Unhandled cop0: %08X\n", op);
            exit(1);
    }
}

static void op_bne(Cpu *cpu, uint32_t op) {
    if (cpu_reg(cpu, instr_s(op)) != cpu_reg(cpu, instr_t(op)))
        cpu_branch(cpu, instr_imm_se(op));
}

static void op_addi(Cpu *cpu, uint32_t op) {
    int32_t s = (int32_t)cpu_reg(cpu, instr_s(op));
    int32_t i = (int32_t)instr_imm_se(op);
    int32_t r;
    if (__builtin_add_overflow(s, i, &r))
        cpu_exception(cpu, EXC_OVERFLOW);
    else
        cpu_set_reg(cpu, instr_t(op), (uint32_t)r);
}

static void op_lw(Cpu *cpu, uint32_t op) {
    if (cpu->sr & 0x10000) return;
    uint32_t addr = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    if (addr % 4 != 0) { cpu_exception(cpu, EXC_LOAD_ADDRESS_ERROR); return; }
    cpu->load_reg = instr_t(op);
    cpu->load_val = cpu_load32(cpu, addr);
}

static void op_sltu(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), cpu_reg(cpu, instr_s(op)) < cpu_reg(cpu, instr_t(op)) ? 1 : 0);
}

static void op_addu(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), cpu_reg(cpu, instr_s(op)) + cpu_reg(cpu, instr_t(op)));
}

static void op_sh(Cpu *cpu, uint32_t op) {
    if (cpu->sr & 0x10000) return;
    uint32_t addr = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    if (addr % 2 != 0) { cpu_exception(cpu, EXC_STORE_ADDRESS_ERROR); return; }
    cpu_store16(cpu, addr, (uint16_t)cpu_reg(cpu, instr_t(op)));
}

static void op_jal(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, 31, cpu->next_pc);
    op_j(cpu, op);
}

static void op_andi(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_t(op), cpu_reg(cpu, instr_s(op)) & instr_imm(op));
}

static void op_sb(Cpu *cpu, uint32_t op) {
    if (cpu->sr & 0x10000) return;
    uint32_t addr = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    cpu_store8(cpu, addr, (uint8_t)cpu_reg(cpu, instr_t(op)));
}

static void op_jr(Cpu *cpu, uint32_t op) {
    cpu->next_pc = cpu_reg(cpu, instr_s(op));
    cpu->branch  = true;
}

static void op_lb(Cpu *cpu, uint32_t op) {
    uint32_t addr = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    cpu->load_reg = instr_t(op);
    cpu->load_val = (uint32_t)(int32_t)(int8_t)cpu_load8(cpu, addr);
}

static void op_beq(Cpu *cpu, uint32_t op) {
    if (cpu_reg(cpu, instr_s(op)) == cpu_reg(cpu, instr_t(op)))
        cpu_branch(cpu, instr_imm_se(op));
}

static void op_mfc0(Cpu *cpu, uint32_t op) {
    uint32_t cop_r = instr_d(op);
    uint32_t v;
    switch (cop_r) {
        case 12: v = cpu->sr;    break;
        case 13: v = cpu->cause; break;
        case 14: v = cpu->epc;   break;
        default: fprintf(stderr, "Unhandled mfc0 r%u\n", cop_r); exit(1);
    }
    cpu->load_reg = instr_t(op);
    cpu->load_val = v;
}

static void op_and(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), cpu_reg(cpu, instr_s(op)) & cpu_reg(cpu, instr_t(op)));
}

static void op_add(Cpu *cpu, uint32_t op) {
    int32_t s = (int32_t)cpu_reg(cpu, instr_s(op));
    int32_t t = (int32_t)cpu_reg(cpu, instr_t(op));
    int32_t r;
    if (__builtin_add_overflow(s, t, &r))
        cpu_exception(cpu, EXC_OVERFLOW);
    else
        cpu_set_reg(cpu, instr_d(op), (uint32_t)r);
}

static void op_bgtz(Cpu *cpu, uint32_t op) {
    if ((int32_t)cpu_reg(cpu, instr_s(op)) > 0) cpu_branch(cpu, instr_imm_se(op));
}

static void op_blez(Cpu *cpu, uint32_t op) {
    if ((int32_t)cpu_reg(cpu, instr_s(op)) <= 0) cpu_branch(cpu, instr_imm_se(op));
}

static void op_lbu(Cpu *cpu, uint32_t op) {
    uint32_t addr = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    cpu->load_reg = instr_t(op);
    cpu->load_val = cpu_load8(cpu, addr);
}

static void op_jalr(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), cpu->next_pc);
    cpu->next_pc = cpu_reg(cpu, instr_s(op));
    cpu->branch  = true;
}

static void op_bxx(Cpu *cpu, uint32_t op) {
    uint32_t is_bgez = (op >> 16) & 1;
    uint32_t is_link = ((op >> 17) & 0x0F) == 0x08;
    int32_t v        = (int32_t)cpu_reg(cpu, instr_s(op));
    uint32_t test    = ((uint32_t)(v < 0)) ^ is_bgez;
    if (is_link) cpu_set_reg(cpu, 31, cpu->pc);
    if (test) cpu_branch(cpu, instr_imm_se(op));
}

static void op_slti(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_t(op), (int32_t)cpu_reg(cpu, instr_s(op)) < (int32_t)instr_imm_se(op) ? 1 : 0);
}

static void op_subu(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), cpu_reg(cpu, instr_s(op)) - cpu_reg(cpu, instr_t(op)));
}

static void op_sra(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), (uint32_t)((int32_t)cpu_reg(cpu, instr_t(op)) >> instr_shift(op)));
}

static void op_div(Cpu *cpu, uint32_t op) {
    int32_t n = (int32_t)cpu_reg(cpu, instr_s(op));
    int32_t d = (int32_t)cpu_reg(cpu, instr_t(op));
    if (d == 0) {
        cpu->hi = (uint32_t)n;
        cpu->lo = (n >= 0) ? 0xFFFFFFFF : 1;
    } else if ((uint32_t)n == 0x80000000 && d == -1) {
        cpu->hi = 0;
        cpu->lo = 0x80000000;
    } else {
        cpu->hi = (uint32_t)(n % d);
        cpu->lo = (uint32_t)(n / d);
    }
}

static void op_mflo(Cpu *cpu, uint32_t op) { cpu_set_reg(cpu, instr_d(op), cpu->lo); }
static void op_mfhi(Cpu *cpu, uint32_t op) { cpu_set_reg(cpu, instr_d(op), cpu->hi); }

static void op_srl(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), cpu_reg(cpu, instr_t(op)) >> instr_shift(op));
}

static void op_sltiu(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_t(op), cpu_reg(cpu, instr_s(op)) < instr_imm_se(op) ? 1 : 0);
}

static void op_divu(Cpu *cpu, uint32_t op) {
    uint32_t n = cpu_reg(cpu, instr_s(op));
    uint32_t d = cpu_reg(cpu, instr_t(op));
    if (d == 0) { cpu->hi = n; cpu->lo = 0xFFFFFFFF; }
    else        { cpu->hi = n % d; cpu->lo = n / d; }
}

static void op_slt(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op),
        (int32_t)cpu_reg(cpu, instr_s(op)) < (int32_t)cpu_reg(cpu, instr_t(op)) ? 1 : 0);
}

static void op_syscall(Cpu *cpu, uint32_t op) { (void)op; cpu_exception(cpu, EXC_SYSCALL); }
static void op_break(Cpu *cpu, uint32_t op)   { (void)op; cpu_exception(cpu, EXC_BREAK); }
static void op_illegal(Cpu *cpu, uint32_t op) {
    printf("Illegal instruction: %08X\n", op);
    cpu_exception(cpu, EXC_ILLEGAL_INSTRUCTION);
}

static void op_mtlo(Cpu *cpu, uint32_t op) { cpu->lo = cpu_reg(cpu, instr_s(op)); }
static void op_mthi(Cpu *cpu, uint32_t op) { cpu->hi = cpu_reg(cpu, instr_s(op)); }

static void op_rfe(Cpu *cpu, uint32_t op) {
    if ((op & 0x3F) != 0b010000) {
        fprintf(stderr, "Invalid cop0 RFE: %08X\n", op);
        exit(1);
    }
    uint32_t mode = cpu->sr & 0x3F;
    cpu->sr = (cpu->sr & ~0x3Fu) | (mode >> 2);
}

static void op_lhu(Cpu *cpu, uint32_t op) {
    uint32_t addr = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    if (addr % 2 != 0) { cpu_exception(cpu, EXC_LOAD_ADDRESS_ERROR); return; }
    cpu->load_reg = instr_t(op);
    cpu->load_val = cpu_load16(cpu, addr);
}

static void op_sllv(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), cpu_reg(cpu, instr_t(op)) << (cpu_reg(cpu, instr_s(op)) & 0x1F));
}

static void op_lh(Cpu *cpu, uint32_t op) {
    uint32_t addr = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    cpu->load_reg = instr_t(op);
    cpu->load_val = (uint32_t)(int32_t)(int16_t)cpu_load16(cpu, addr);
}

static void op_nor(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), ~(cpu_reg(cpu, instr_s(op)) | cpu_reg(cpu, instr_t(op))));
}

static void op_srav(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op),
        (uint32_t)((int32_t)cpu_reg(cpu, instr_t(op)) >> (cpu_reg(cpu, instr_s(op)) & 0x1F)));
}

static void op_srlv(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), cpu_reg(cpu, instr_t(op)) >> (cpu_reg(cpu, instr_s(op)) & 0x1F));
}

static void op_multu(Cpu *cpu, uint32_t op) {
    uint64_t v = (uint64_t)cpu_reg(cpu, instr_s(op)) * (uint64_t)cpu_reg(cpu, instr_t(op));
    cpu->hi = (uint32_t)(v >> 32);
    cpu->lo = (uint32_t)v;
}

static void op_xor(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_d(op), cpu_reg(cpu, instr_s(op)) ^ cpu_reg(cpu, instr_t(op)));
}

static void op_mult(Cpu *cpu, uint32_t op) {
    int64_t a = (int64_t)(int32_t)cpu_reg(cpu, instr_s(op));
    int64_t b = (int64_t)(int32_t)cpu_reg(cpu, instr_t(op));
    uint64_t v = (uint64_t)(a * b);
    cpu->hi = (uint32_t)(v >> 32);
    cpu->lo = (uint32_t)v;
}

static void op_sub(Cpu *cpu, uint32_t op) {
    int32_t s = (int32_t)cpu_reg(cpu, instr_s(op));
    int32_t t = (int32_t)cpu_reg(cpu, instr_t(op));
    int32_t r;
    if (__builtin_sub_overflow(s, t, &r))
        cpu_exception(cpu, EXC_OVERFLOW);
    else
        cpu_set_reg(cpu, instr_d(op), (uint32_t)r);
}

static void op_xori(Cpu *cpu, uint32_t op) {
    cpu_set_reg(cpu, instr_t(op), cpu_reg(cpu, instr_s(op)) ^ instr_imm(op));
}

static void op_lwl(Cpu *cpu, uint32_t op) {
    uint32_t addr     = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    uint32_t cur_v    = cpu->out_regs[instr_t(op)];
    uint32_t aligned  = addr & ~3u;
    uint32_t word     = cpu_load32(cpu, aligned);
    uint32_t v;
    switch (addr & 3) {
        case 0: v = (cur_v & 0x00FFFFFF) | (word << 24); break;
        case 1: v = (cur_v & 0x0000FFFF) | (word << 16); break;
        case 2: v = (cur_v & 0x000000FF) | (word << 8);  break;
        case 3: v = (cur_v & 0x00000000) | (word << 0);  break;
        default: v = 0;
    }
    cpu->load_reg = instr_t(op);
    cpu->load_val = v;
}

static void op_lwr(Cpu *cpu, uint32_t op) {
    uint32_t addr    = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    uint32_t cur_v   = cpu->out_regs[instr_t(op)];
    uint32_t aligned = addr & ~3u;
    uint32_t word    = cpu_load32(cpu, aligned);
    uint32_t v;
    switch (addr & 3) {
        case 0: v = (cur_v & 0x00000000) | (word >> 0);  break;
        case 1: v = (cur_v & 0xFF000000) | (word >> 8);  break;
        case 2: v = (cur_v & 0xFFFF0000) | (word >> 16); break;
        case 3: v = (cur_v & 0xFFFFFF00) | (word >> 24); break;
        default: v = 0;
    }
    cpu->load_reg = instr_t(op);
    cpu->load_val = v;
}

static void op_swl(Cpu *cpu, uint32_t op) {
    uint32_t addr    = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    uint32_t v       = cpu_reg(cpu, instr_t(op));
    uint32_t aligned = addr & ~3u;
    uint32_t mem     = cpu_load32(cpu, aligned);
    uint32_t val;
    switch (addr & 3) {
        case 0: val = (mem & 0xFFFFFF00) | (v >> 24); break;
        case 1: val = (mem & 0xFFFF0000) | (v >> 16); break;
        case 2: val = (mem & 0xFF000000) | (v >> 8);  break;
        case 3: val = (mem & 0x00000000) | (v >> 0);  break;
        default: val = 0;
    }
    cpu_store32(cpu, aligned, val);
}

static void op_swr(Cpu *cpu, uint32_t op) {
    uint32_t addr    = cpu_reg(cpu, instr_s(op)) + instr_imm_se(op);
    uint32_t v       = cpu_reg(cpu, instr_t(op));
    uint32_t aligned = addr & ~3u;
    uint32_t mem     = cpu_load32(cpu, aligned);
    uint32_t val;
    switch (addr & 3) {
        case 0: val = (mem & 0x00000000) | (v << 0);  break;
        case 1: val = (mem & 0x000000FF) | (v << 8);  break;
        case 2: val = (mem & 0x0000FFFF) | (v << 16); break;
        case 3: val = (mem & 0x00FFFFFF) | (v << 24); break;
        default: val = 0;
    }
    cpu_store32(cpu, aligned, val);
}
