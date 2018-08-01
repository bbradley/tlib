/*
 *  RISC-V main translation routines
 *
 *  Author: Sagar Karandikar, sagark@eecs.berkeley.edu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "infrastructure.h"

#include "cpu.h"

#include "tcg-op.h"

#include "instmap.h"
#include "debug.h"
#include "arch_callbacks.h"

/* global register indices */
static TCGv_ptr cpu_env;
static TCGv cpu_gpr[32], cpu_pc;
static TCGv_i64 cpu_fpr[32]; /* assume F and D extensions */
static TCGv load_res;


#include "tb-helper.h"

typedef struct DisasContext {
    struct TranslationBlock *tb;
    target_ulong pc;
    target_ulong next_pc;
    uint32_t opcode;
    int singlestep_enabled;
    int mem_idx;
    int bstate;
} DisasContext;

static inline void kill_unknown(DisasContext *ctx, int excp);

enum {
    BS_NONE     = 0, /* When seen outside of translation while loop, indicates
                     need to exit tb due to end of page. */
    BS_STOP     = 1, /* Need to exit tb for syscall, sret, etc. */
    BS_BRANCH   = 2, /* Need to exit tb for branch, jal, etc. */
};


static const char * const regnames[] = {
  "zero", "ra  ", "sp  ", "gp  ", "tp  ", "t0  ",  "t1  ",  "t2  ",
  "s0  ", "s1  ", "a0  ", "a1  ", "a2  ", "a3  ",  "a4  ",  "a5  ",
  "a6  ", "a7  ", "s2  ", "s3  ", "s4  ", "s5  ",  "s6  ",  "s7  ",
  "s8  ", "s9  ", "s10 ", "s11 ", "t3  ", "t4  ",  "t5  ",  "t6  "
};

static const char * const fpr_regnames[] = {
  "ft0", "ft1", "ft2",  "ft3",  "ft4", "ft5", "ft6",  "ft7",
  "fs0", "fs1", "fa0",  "fa1",  "fa2", "fa3", "fa4",  "fa5",
  "fa6", "fa7", "fs2",  "fs3",  "fs4", "fs5", "fs6",  "fs7",
  "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"
};

#ifdef TARGET_RISCV64
#define CASE_OP_32_64(X) case X: case glue(X, W)
#else
#define CASE_OP_32_64(X) case X
#endif

static inline uint32_t extract32(uint32_t value, uint8_t start, uint8_t length)
{
    return (value >> start) & ((((uint32_t)1) << length) - 1);
}

static inline uint64_t sextract64(uint64_t value, uint8_t start, uint8_t length)
{
    uint64_t result = (value >> start) & ((1 << length) - 1);
    if(result >> (length - 1))
    {
        result |= ~((((uint64_t)1) << length) - 1);
    }
    return result;
}

static inline void generate_exception(DisasContext *ctx, int excp)
{
    tcg_gen_movi_tl(cpu_pc, ctx->pc);
    TCGv_i32 helper_tmp = tcg_const_i32(excp);
    gen_helper_raise_exception(cpu_env, helper_tmp);
    tcg_temp_free_i32(helper_tmp);
}

static inline void generate_exception_mbadaddr(DisasContext *ctx, int excp)
{
    generate_log(ctx->pc, "exception_badaddr");
    tcg_gen_movi_tl(cpu_pc, ctx->pc);
    TCGv_i32 helper_tmp = tcg_const_i32(excp);
    gen_helper_raise_exception_mbadaddr(cpu_env, helper_tmp, cpu_pc);
    tcg_temp_free_i32(helper_tmp);
}

/* unknown instruction */
static inline void kill_unknown(DisasContext *ctx, int excp)
{
    generate_exception(ctx, excp);
    ctx->bstate = BS_STOP;
}

static inline bool use_goto_tb(DisasContext *ctx, target_ulong dest)
{
    if (unlikely(ctx->singlestep_enabled)) {
        return false;
    }

    return (ctx->tb->pc & TARGET_PAGE_MASK) == (dest & TARGET_PAGE_MASK);
}

static inline void gen_goto_tb(DisasContext *ctx, int n, target_ulong dest)
{
    if (use_goto_tb(ctx, dest)) {
        /* chaining is only allowed when the jump is to the same page */
        tcg_gen_goto_tb(n);
        tcg_gen_movi_tl(cpu_pc, dest);
        gen_exit_tb((uintptr_t)ctx->tb + n, ctx->tb);
    } else {
        tcg_gen_movi_tl(cpu_pc, dest);
        if (ctx->singlestep_enabled) {
            gen_helper_raise_exception_debug(cpu_env);
        }
        gen_exit_tb(0, ctx->tb);
    }
}

/* Wrapper for getting reg values - need to check of reg is zero since
 * cpu_gpr[0] is not actually allocated
 */
static inline void gen_get_gpr(TCGv t, int reg_num)
{
    if (reg_num == 0) {
        tcg_gen_movi_tl(t, 0);
    } else {
        tcg_gen_mov_tl(t, cpu_gpr[reg_num]);
    }
}

/* Wrapper for setting reg values - need to check of reg is zero since
 * cpu_gpr[0] is not actually allocated. this is more for safety purposes,
 * since we usually avoid calling the OP_TYPE_gen function if we see a write to
 * $zero
 */
static inline void gen_set_gpr(int reg_num_dst, TCGv t)
{
    if (reg_num_dst != 0) {
        tcg_gen_mov_tl(cpu_gpr[reg_num_dst], t);
    }
}

static void gen_mulhsu(TCGv ret, TCGv arg1, TCGv arg2)
{
    TCGv rl = tcg_temp_new();
    TCGv rh = tcg_temp_new();

    tcg_gen_mulu2_tl(rl, rh, arg1, arg2);
    /* fix up for one negative */
    tcg_gen_sari_tl(rl, arg1, TARGET_LONG_BITS - 1);
    tcg_gen_and_tl(rl, rl, arg2);
    tcg_gen_sub_tl(ret, rh, rl);

    tcg_temp_free(rl);
    tcg_temp_free(rh);
}

static void gen_fsgnj(DisasContext *ctx, uint32_t rd, uint32_t rs1, uint32_t rs2,
                     int rm, uint64_t min)
{
    TCGv t0 = tcg_temp_new();
    int fp_ok = gen_new_label();
    int done = gen_new_label();

generate_log(ctx->pc, "gen_fsgnj has some loads, so we should check if it maybe fails!");

    // check MSTATUS.FS
    tcg_gen_ld_tl(t0, cpu_env, offsetof(CPUState, mstatus));
    tcg_gen_andi_tl(t0, t0, MSTATUS_FS);
    tcg_gen_brcondi_tl(TCG_COND_NE, t0, 0x0, fp_ok);
    // MSTATUS_FS field was zero:
    kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
    tcg_gen_br(done);

    // proceed with operation
    gen_set_label(fp_ok);
    TCGv_i64 src1 = tcg_temp_new_i64();
    TCGv_i64 src2 = tcg_temp_new_i64();

    tcg_gen_mov_i64(src1, cpu_fpr[rs1]);
    tcg_gen_mov_i64(src2, cpu_fpr[rs2]);

    switch (rm) {
    case 0: /* fsgnj */

        if (rs1 == rs2) { /* FMOV */
            tcg_gen_mov_i64(cpu_fpr[rd], src1);
        }

        tcg_gen_andi_i64(src1, src1, ~min);
        tcg_gen_andi_i64(src2, src2, min);
        tcg_gen_or_i64(cpu_fpr[rd], src1, src2);
        break;
    case 1: /* fsgnjn */
        tcg_gen_andi_i64(src1, src1, ~min);
        tcg_gen_not_i64(src2, src2);
        tcg_gen_andi_i64(src2, src2, min);
        tcg_gen_or_i64(cpu_fpr[rd], src1, src2);
        break;
    case 2: /* fsgnjx */
        tcg_gen_andi_i64(src2, src2, min);
        tcg_gen_xor_i64(cpu_fpr[rd], src1, src2);
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
    }
    tcg_temp_free_i64(src1);
    tcg_temp_free_i64(src2);
    gen_set_label(done);
    tcg_temp_free(t0);
}

static void gen_arith(DisasContext *ctx, uint32_t opc, int rd, int rs1,
        int rs2)
{
    TCGv source1, source2, cond1, cond2, zeroreg, resultopt1;
    source1 = tcg_temp_new();
    source2 = tcg_temp_new();
    gen_get_gpr(source1, rs1);
    gen_get_gpr(source2, rs2);

    switch (opc) {
    CASE_OP_32_64(OPC_RISC_ADD):
        tcg_gen_add_tl(source1, source1, source2);
        break;
    CASE_OP_32_64(OPC_RISC_SUB):
        tcg_gen_sub_tl(source1, source1, source2);
        break;
#if defined(TARGET_RISCV64)
    case OPC_RISC_SLLW:
        tcg_gen_andi_tl(source2, source2, 0x1F);
        tcg_gen_shl_tl(source1, source1, source2);
        break;
#endif
    case OPC_RISC_SLL:
        tcg_gen_andi_tl(source2, source2, TARGET_LONG_BITS - 1);
        tcg_gen_shl_tl(source1, source1, source2);
        break;
    case OPC_RISC_SLT:
        tcg_gen_setcond_tl(TCG_COND_LT, source1, source1, source2);
        break;
    case OPC_RISC_SLTU:
        tcg_gen_setcond_tl(TCG_COND_LTU, source1, source1, source2);
        break;
    case OPC_RISC_XOR:
        tcg_gen_xor_tl(source1, source1, source2);
        break;
#if defined(TARGET_RISCV64)
    case OPC_RISC_SRLW:
        /* clear upper 32 */
        tcg_gen_ext32u_tl(source1, source1);
        tcg_gen_andi_tl(source2, source2, 0x1F);
        tcg_gen_shr_tl(source1, source1, source2);
        break;
#endif
    case OPC_RISC_SRL:
        tcg_gen_andi_tl(source2, source2, TARGET_LONG_BITS - 1);
        tcg_gen_shr_tl(source1, source1, source2);
        break;
#if defined(TARGET_RISCV64)
    case OPC_RISC_SRAW:
        /* first, trick to get it to act like working on 32 bits (get rid of
        upper 32, sign extend to fill space) */
        tcg_gen_ext32s_tl(source1, source1);
        tcg_gen_andi_tl(source2, source2, 0x1F);
        tcg_gen_sar_tl(source1, source1, source2);
        break;
        /* fall through to SRA */
#endif
    case OPC_RISC_SRA:
        tcg_gen_andi_tl(source2, source2, TARGET_LONG_BITS - 1);
        tcg_gen_sar_tl(source1, source1, source2);
        break;
    case OPC_RISC_OR:
        tcg_gen_or_tl(source1, source1, source2);
        break;
    case OPC_RISC_AND:
        tcg_gen_and_tl(source1, source1, source2);
        break;
    CASE_OP_32_64(OPC_RISC_MUL):
        tcg_gen_mul_tl(source1, source1, source2);
        break;
    case OPC_RISC_MULH:
        tcg_gen_muls2_tl(source2, source1, source1, source2);
        break;
    case OPC_RISC_MULHSU:
        gen_mulhsu(source1, source1, source2);
        break;
    case OPC_RISC_MULHU:
        tcg_gen_mulu2_tl(source2, source1, source1, source2);
        break;
#if defined(TARGET_RISCV64)
    case OPC_RISC_DIVW:
        tcg_gen_ext32s_tl(source1, source1);
        tcg_gen_ext32s_tl(source2, source2);
        /* fall through to DIV */
#endif
        /* fallthrough */
    case OPC_RISC_DIV:
        /* Handle by altering args to tcg_gen_div to produce req'd results:
         * For overflow: want source1 in source1 and 1 in source2
         * For div by zero: want -1 in source1 and 1 in source2 -> -1 result */
        cond1 = tcg_temp_new();
        cond2 = tcg_temp_new();
        zeroreg = tcg_const_tl(0);
        resultopt1 = tcg_temp_new();

        tcg_gen_movi_tl(resultopt1, (target_ulong)-1);
        tcg_gen_setcondi_tl(TCG_COND_EQ, cond2, source2, (target_ulong)(~0L));
        tcg_gen_setcondi_tl(TCG_COND_EQ, cond1, source1,
                            ((target_ulong)1) << (TARGET_LONG_BITS - 1));
        tcg_gen_and_tl(cond1, cond1, cond2); /* cond1 = overflow */
        tcg_gen_setcondi_tl(TCG_COND_EQ, cond2, source2, 0); /* cond2 = div 0 */
        /* if div by zero, set source1 to -1, otherwise don't change */
        tcg_gen_movcond_tl(TCG_COND_EQ, source1, cond2, zeroreg, source1,
                resultopt1);
        /* if overflow or div by zero, set source2 to 1, else don't change */
        tcg_gen_or_tl(cond1, cond1, cond2);
        tcg_gen_movi_tl(resultopt1, (target_ulong)1);
        tcg_gen_movcond_tl(TCG_COND_EQ, source2, cond1, zeroreg, source2,
                resultopt1);
        tcg_gen_div_tl(source1, source1, source2);

        tcg_temp_free(cond1);
        tcg_temp_free(cond2);
        tcg_temp_free(zeroreg);
        tcg_temp_free(resultopt1);
        break;
#if defined(TARGET_RISCV64)
    case OPC_RISC_DIVUW:
        tcg_gen_ext32u_tl(source1, source1);
        tcg_gen_ext32u_tl(source2, source2);
        /* fall through to DIVU */
#endif
        /* fallthrough */
    case OPC_RISC_DIVU:
        cond1 = tcg_temp_new();
        zeroreg = tcg_const_tl(0);
        resultopt1 = tcg_temp_new();

        tcg_gen_setcondi_tl(TCG_COND_EQ, cond1, source2, 0);
        tcg_gen_movi_tl(resultopt1, (target_ulong)-1);
        tcg_gen_movcond_tl(TCG_COND_EQ, source1, cond1, zeroreg, source1,
                resultopt1);
        tcg_gen_movi_tl(resultopt1, (target_ulong)1);
        tcg_gen_movcond_tl(TCG_COND_EQ, source2, cond1, zeroreg, source2,
                resultopt1);
        tcg_gen_divu_tl(source1, source1, source2);

        tcg_temp_free(cond1);
        tcg_temp_free(zeroreg);
        tcg_temp_free(resultopt1);
        break;
#if defined(TARGET_RISCV64)
    case OPC_RISC_REMW:
        tcg_gen_ext32s_tl(source1, source1);
        tcg_gen_ext32s_tl(source2, source2);
        /* fall through to REM */
#endif
        /* fallthrough */
    case OPC_RISC_REM:
        cond1 = tcg_temp_new();
        cond2 = tcg_temp_new();
        zeroreg = tcg_const_tl(0);
        resultopt1 = tcg_temp_new();

        tcg_gen_movi_tl(resultopt1, 1L);
        tcg_gen_setcondi_tl(TCG_COND_EQ, cond2, source2, (target_ulong)-1);
        tcg_gen_setcondi_tl(TCG_COND_EQ, cond1, source1,
                            (target_ulong)1 << (TARGET_LONG_BITS - 1));
        tcg_gen_and_tl(cond2, cond1, cond2); /* cond1 = overflow */
        tcg_gen_setcondi_tl(TCG_COND_EQ, cond1, source2, 0); /* cond2 = div 0 */
        /* if overflow or div by zero, set source2 to 1, else don't change */
        tcg_gen_or_tl(cond2, cond1, cond2);
        tcg_gen_movcond_tl(TCG_COND_EQ, source2, cond2, zeroreg, source2,
                resultopt1);
        tcg_gen_rem_tl(resultopt1, source1, source2);
        /* if div by zero, just return the original dividend */
        tcg_gen_movcond_tl(TCG_COND_EQ, source1, cond1, zeroreg, resultopt1,
                source1);

        tcg_temp_free(cond1);
        tcg_temp_free(cond2);
        tcg_temp_free(zeroreg);
        tcg_temp_free(resultopt1);
        break;
#if defined(TARGET_RISCV64)
    case OPC_RISC_REMUW:
        tcg_gen_ext32u_tl(source1, source1);
        tcg_gen_ext32u_tl(source2, source2);
        /* fall through to REMU */
#endif
        /* fallthrough */
    case OPC_RISC_REMU:
        cond1 = tcg_temp_new();
        zeroreg = tcg_const_tl(0);
        resultopt1 = tcg_temp_new();

        tcg_gen_movi_tl(resultopt1, (target_ulong)1);
        tcg_gen_setcondi_tl(TCG_COND_EQ, cond1, source2, 0);
        tcg_gen_movcond_tl(TCG_COND_EQ, source2, cond1, zeroreg, source2,
                resultopt1);
        tcg_gen_remu_tl(resultopt1, source1, source2);
        /* if div by zero, just return the original dividend */
        tcg_gen_movcond_tl(TCG_COND_EQ, source1, cond1, zeroreg, resultopt1,
                source1);

        tcg_temp_free(cond1);
        tcg_temp_free(zeroreg);
        tcg_temp_free(resultopt1);
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }

    if (opc & 0x8) { /* sign extend for W instructions */
        tcg_gen_ext32s_tl(source1, source1);
    }

    gen_set_gpr(rd, source1);
    tcg_temp_free(source1);
    tcg_temp_free(source2);
}

static void gen_arith_imm(DisasContext *ctx, uint32_t opc, int rd,
        int rs1, target_long imm)
{
generate_log(ctx->pc, "we are in gen_arith_imm");
    TCGv source1;
    source1 = tcg_temp_new();
    gen_get_gpr(source1, rs1);
    target_long extra_shamt = 0;

    switch (opc) {
    case OPC_RISC_ADDI:
#if defined(TARGET_RISCV64)
    case OPC_RISC_ADDIW:
#endif
        tcg_gen_addi_tl(source1, source1, imm);
        break;
    case OPC_RISC_SLTI:
        tcg_gen_setcondi_tl(TCG_COND_LT, source1, source1, imm);
        break;
    case OPC_RISC_SLTIU:
        tcg_gen_setcondi_tl(TCG_COND_LTU, source1, source1, imm);
        break;
    case OPC_RISC_XORI:
        tcg_gen_xori_tl(source1, source1, imm);
        break;
    case OPC_RISC_ORI:
        tcg_gen_ori_tl(source1, source1, imm);
        break;
    case OPC_RISC_ANDI:
        tcg_gen_andi_tl(source1, source1, imm);
        break;
#if defined(TARGET_RISCV64)
    case OPC_RISC_SLLIW:
         if ((imm >= 32)) {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            break;
         }
        /* fall through to SLLI */
#endif
        /* fallthrough */
    case OPC_RISC_SLLI:
        if (imm < TARGET_LONG_BITS) {
            tcg_gen_shli_tl(source1, source1, imm);
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        break;
#if defined(TARGET_RISCV64)
    case OPC_RISC_SHIFT_RIGHT_IW:
        if ((imm & 0x3ff) >= 32) {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        tcg_gen_shli_tl(source1, source1, 32);
        extra_shamt = 32;
        /* fall through to SHIFT_RIGHT_I */
#endif
        /* fallthrough */
    case OPC_RISC_SHIFT_RIGHT_I:
        /* differentiate on IMM */
        if ((imm & 0x3ff) < TARGET_LONG_BITS) {
            if (imm & 0x400) {
                /* SRAI[W] */
                tcg_gen_sari_tl(source1, source1, (imm ^ 0x400) + extra_shamt);
            } else {
                /* SRLI[W] */
                tcg_gen_shri_tl(source1, source1, imm + extra_shamt);
            }
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }

    if (opc & 0x8) { /* sign-extend for W instructions */
        tcg_gen_ext32s_tl(source1, source1);
    }

    gen_set_gpr(rd, source1);
    tcg_temp_free(source1);
}

static void gen_jal(CPUState *env, DisasContext *ctx, int rd,
                    target_ulong imm)
{
    target_ulong next_pc;

    /* check misaligned: */
    next_pc = ctx->pc + imm;
    if (!riscv_has_ext(env, RISCV_FEATURE_RVC)) {
        if ((next_pc & 0x3) != 0) {
            generate_exception_mbadaddr(ctx, RISCV_EXCP_INST_ADDR_MIS);
        }
    }
    if (rd != 0) {
        tcg_gen_movi_tl(cpu_gpr[rd], ctx->next_pc);
    }

    gen_goto_tb(ctx, 0, ctx->pc + imm); /* must use this for safety */
    ctx->bstate = BS_BRANCH;

}

static void gen_jalr(CPUState *env, DisasContext *ctx, uint32_t opc, int rd, int rs1,
        target_long imm)
{
    /* no chaining with JALR */
    int misaligned = gen_new_label();
    TCGv t0;
    t0 = tcg_temp_new();

    switch (opc) {
    case OPC_RISC_JALR:
        gen_get_gpr(cpu_pc, rs1);
        tcg_gen_addi_tl(cpu_pc, cpu_pc, imm);
        tcg_gen_andi_tl(cpu_pc, cpu_pc, (target_ulong)-2);

        if (!riscv_has_ext(env, RISCV_FEATURE_RVC)) {
            tcg_gen_andi_tl(t0, cpu_pc, 0x2);
            tcg_gen_brcondi_tl(TCG_COND_NE, t0, 0x0, misaligned);
        }

        if (rd != 0) {
            tcg_gen_movi_tl(cpu_gpr[rd], ctx->next_pc);
        }
        gen_exit_tb(0, ctx->tb);

        gen_set_label(misaligned);
        generate_exception_mbadaddr(ctx, RISCV_EXCP_INST_ADDR_MIS);
        gen_exit_tb(0, ctx->tb);
        ctx->bstate = BS_BRANCH;
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }
    tcg_temp_free(t0);
}

static void gen_branch(CPUState *env, DisasContext *ctx, uint32_t opc, int rs1, int rs2,
        target_long bimm)
{
    int l = gen_new_label();
    TCGv source1, source2;
    source1 = tcg_temp_new();
    source2 = tcg_temp_new();
    gen_get_gpr(source1, rs1);
    gen_get_gpr(source2, rs2);

    switch (opc) {
    case OPC_RISC_BEQ:
        tcg_gen_brcond_tl(TCG_COND_EQ, source1, source2, l);
        break;
    case OPC_RISC_BNE:
        tcg_gen_brcond_tl(TCG_COND_NE, source1, source2, l);
        break;
    case OPC_RISC_BLT:
        tcg_gen_brcond_tl(TCG_COND_LT, source1, source2, l);
        break;
    case OPC_RISC_BGE:
        tcg_gen_brcond_tl(TCG_COND_GE, source1, source2, l);
        break;
    case OPC_RISC_BLTU:
        tcg_gen_brcond_tl(TCG_COND_LTU, source1, source2, l);
        break;
    case OPC_RISC_BGEU:
        tcg_gen_brcond_tl(TCG_COND_GEU, source1, source2, l);
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }

    gen_goto_tb(ctx, 1, ctx->next_pc);
    gen_set_label(l); /* branch taken */
    if (!riscv_has_ext(env, RISCV_FEATURE_RVC) && ((ctx->pc + bimm) & 0x3)) {
        /* misaligned */
        generate_exception_mbadaddr(ctx, RISCV_EXCP_INST_ADDR_MIS);
        gen_exit_tb(0, ctx->tb);
    } else {
        gen_goto_tb(ctx, 0, ctx->pc + bimm);
    }
    tcg_temp_free(source1);
    tcg_temp_free(source2);
    ctx->bstate = BS_BRANCH;
}

static void gen_load(DisasContext *ctx, uint32_t opc, int rd, int rs1,
        target_long imm)
{
    TCGv t0 = tcg_temp_new();
    TCGv t1 = tcg_temp_new();

    generate_log(ctx->pc, "gen_load");

    gen_get_gpr(t0, rs1);

    tcg_gen_addi_tl(t0, t0, imm);

    tcg_gen_movi_tl(cpu_pc, ctx->pc);
    switch (opc) {

    case OPC_RISC_LB:
        tcg_gen_qemu_ld8s(t1, t0, ctx->mem_idx);
        break;
    case OPC_RISC_LH:
        tcg_gen_qemu_ld16s(t1, t0, ctx->mem_idx);
        break;
    case OPC_RISC_LW:
        tcg_gen_qemu_ld32s(t1, t0, ctx->mem_idx);
        break;
    case OPC_RISC_LD:
        tcg_gen_qemu_ld64(t1, t0, ctx->mem_idx);
        break;
    case OPC_RISC_LBU:
        tcg_gen_qemu_ld8u(t1, t0, ctx->mem_idx);
        break;
    case OPC_RISC_LHU:
        tcg_gen_qemu_ld16u(t1, t0, ctx->mem_idx);
        break;
    case OPC_RISC_LWU:
        tcg_gen_qemu_ld32u(t1, t0, ctx->mem_idx);
        break;
    default:
	 kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;

    }

    gen_set_gpr(rd, t1);
    tcg_temp_free(t0);
    tcg_temp_free(t1);
    generate_log(ctx->pc, "after gen_load");
}

static void gen_store(DisasContext *ctx, uint32_t opc, int rs1, int rs2,
        target_long imm)
{
    tcg_gen_movi_tl(cpu_pc, ctx->pc);

    TCGv t0 = tcg_temp_new();
    TCGv dat = tcg_temp_new();
    gen_get_gpr(t0, rs1);
    tcg_gen_addi_tl(t0, t0, imm);
    gen_get_gpr(dat, rs2);
 generate_log(ctx->pc, "gen_store");

    switch (opc) {
    case OPC_RISC_SB:
        tcg_gen_qemu_st8(dat, t0, ctx->mem_idx);
        break;
    case OPC_RISC_SH:
        tcg_gen_qemu_st16(dat, t0, ctx->mem_idx);
        break;
    case OPC_RISC_SW:
        tcg_gen_qemu_st32(dat, t0, ctx->mem_idx);
        break;
    case OPC_RISC_SD:
        tcg_gen_qemu_st64(dat, t0, ctx->mem_idx);
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }

 generate_log(ctx->pc, "after gen_store");

    tcg_temp_free(t0);
    tcg_temp_free(dat);
}

static void gen_fp_load(DisasContext *ctx, uint32_t opc, int rd,
        int rs1, target_long imm)
{
    TCGv t0 = tcg_temp_new();
    int fp_ok = gen_new_label();
    int done = gen_new_label();

    // check MSTATUS.FS
    generate_log(ctx->pc, "gen_fp_load has some loads so we should check it");
    tcg_gen_ld_tl(t0, cpu_env, offsetof(CPUState, mstatus));
    tcg_gen_andi_tl(t0, t0, MSTATUS_FS);
    tcg_gen_brcondi_tl(TCG_COND_NE, t0, 0x0, fp_ok);
    // MSTATUS_FS field was zero:
    kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
    tcg_gen_br(done);

    // proceed with operation
    gen_set_label(fp_ok);
    gen_get_gpr(t0, rs1);
    tcg_gen_addi_tl(t0, t0, imm);

    switch (opc) {
    case OPC_RISC_FLW:
        tcg_gen_qemu_ld32u(cpu_fpr[rd], t0, ctx->mem_idx);
        break;
    case OPC_RISC_FLD:
        tcg_gen_qemu_ld64(cpu_fpr[rd], t0, ctx->mem_idx);
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }
    gen_set_label(done);
    tcg_temp_free(t0);
}

static void gen_fp_store(DisasContext *ctx, uint32_t opc, int rs1,
        int rs2, target_long imm)
{
    TCGv t0 = tcg_temp_new();
    TCGv t1 = tcg_temp_new();
    int fp_ok = gen_new_label();
    int done = gen_new_label();

    // check MSTATUS.FS
    tcg_gen_ld_tl(t0, cpu_env, offsetof(CPUState, mstatus));
    tcg_gen_andi_tl(t0, t0, MSTATUS_FS);
    tcg_gen_brcondi_tl(TCG_COND_NE, t0, 0x0, fp_ok);
    // MSTATUS_FS field was zero:
    kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
    tcg_gen_br(done);

    // proceed with operation
    gen_set_label(fp_ok);
    gen_get_gpr(t0, rs1);
    tcg_gen_addi_tl(t0, t0, imm);

    switch (opc) {
    case OPC_RISC_FSW:
        tcg_gen_qemu_st32(cpu_fpr[rs2], t0, ctx->mem_idx);
        break;
    case OPC_RISC_FSD:
        tcg_gen_qemu_st64(cpu_fpr[rs2], t0, ctx->mem_idx);
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }

    gen_set_label(done);
    tcg_temp_free(t0);
    tcg_temp_free(t1);
}

static void gen_atomic(DisasContext *ctx, uint32_t opc,
                      int rd, int rs1, int rs2)
{
    generate_log(ctx->pc, "gen_atomic");
    /* TODO: handle aq, rl bits? - for now just get rid of them: */
    opc = MASK_OP_ATOMIC_NO_AQ_RL(opc);
    TCGv source1, source2, dat;
    int done;
    source1 = tcg_temp_local_new();
    source2 = tcg_temp_local_new();
    done = gen_new_label();
    dat = tcg_temp_local_new();
    gen_get_gpr(source1, rs1);
    gen_get_gpr(source2, rs2);
    switch (opc) {
        /* all currently implemented as non-atomics */
    case OPC_RISC_LR_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        break;
    case OPC_RISC_SC_W:
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_movi_tl(dat, 0);
        break;
    case OPC_RISC_AMOSWAP_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOADD_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_add_tl(source2, dat, source2);
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOXOR_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_xor_tl(source2, dat, source2);
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOAND_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_and_tl(source2, dat, source2);
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOOR_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_or_tl(source2, dat, source2);
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOMIN_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_brcond_tl(TCG_COND_LT, dat, source2, done);
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        break;
    case OPC_RISC_AMOMAX_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_brcond_tl(TCG_COND_GT, dat, source2, done);
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        break;
    case OPC_RISC_AMOMINU_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_brcond_tl(TCG_COND_LTU, dat, source2, done);
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        break;
    case OPC_RISC_AMOMAXU_W:
        tcg_gen_qemu_ld32s(dat, source1, ctx->mem_idx);
        tcg_gen_brcond_tl(TCG_COND_GTU, dat, source2, done);
        tcg_gen_qemu_st32(source2, source1, ctx->mem_idx);
        break;
#if defined(TARGET_RISCV64)
    case OPC_RISC_LR_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        break;
    case OPC_RISC_SC_D:
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_movi_tl(dat, 0); // assume always success
        break;
    case OPC_RISC_AMOSWAP_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOADD_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_add_tl(source2, dat, source2);
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOXOR_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_xor_tl(source2, dat, source2);
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOAND_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_and_tl(source2, dat, source2);
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOOR_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_or_tl(source2, dat, source2);
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        tcg_gen_mov_tl(source1, dat);
        break;
    case OPC_RISC_AMOMIN_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_brcond_tl(TCG_COND_LT, dat, source2, done);
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        break;
    case OPC_RISC_AMOMAX_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_brcond_tl(TCG_COND_GT, dat, source2, done);
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        break;
    case OPC_RISC_AMOMINU_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_brcond_tl(TCG_COND_LTU, dat, source2, done);
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        break;
    case OPC_RISC_AMOMAXU_D:
        tcg_gen_qemu_ld64(dat, source1, ctx->mem_idx);
        tcg_gen_brcond_tl(TCG_COND_GTU, dat, source2, done);
        tcg_gen_qemu_st64(source2, source1, ctx->mem_idx);
        break;
#endif
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }
    gen_set_label(done);
    generate_log(ctx->pc, "after gen_atomic");
    gen_set_gpr(rd, dat);
    tcg_temp_free(source1);
    tcg_temp_free(source2);
    tcg_temp_free(dat);
}

static void gen_fp_fmadd(DisasContext *ctx, uint32_t opc, int rd,
        int rs1, int rs2, int rs3, int rm)
{
    generate_log(ctx->pc,"we are in a floating point op, might be wrong!");

    TCGv_i64 rm_reg = tcg_temp_new_i64();
    tcg_gen_movi_i64(rm_reg, rm);

    switch (opc) {
    case OPC_RISC_FMADD_S:
        gen_helper_fmadd_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                           cpu_fpr[rs3], rm_reg);
        break;
    case OPC_RISC_FMADD_D:
        gen_helper_fmadd_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                           cpu_fpr[rs3], rm_reg);
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }
    tcg_temp_free_i64(rm_reg);
}

static void gen_fp_fmsub(DisasContext *ctx, uint32_t opc, int rd,
        int rs1, int rs2, int rs3, int rm)
{
    generate_log(ctx->pc, "we are in a floating point op, might be wrong!");
    TCGv_i64 rm_reg = tcg_temp_new_i64();
    tcg_gen_movi_i64(rm_reg, rm);

    switch (opc) {
    case OPC_RISC_FMSUB_S:
        gen_helper_fmsub_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                           cpu_fpr[rs3], rm_reg);
        break;
    case OPC_RISC_FMSUB_D:
        gen_helper_fmsub_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                           cpu_fpr[rs3], rm_reg);
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }
    tcg_temp_free_i64(rm_reg);
}

static void gen_fp_fnmsub(DisasContext *ctx, uint32_t opc, int rd,
        int rs1, int rs2, int rs3, int rm)
{
    generate_log(ctx->pc, "we are in a floating point op, might be wrong!");

    TCGv_i64 rm_reg = tcg_temp_new_i64();
    tcg_gen_movi_i64(rm_reg, rm);

    switch (opc) {
    case OPC_RISC_FNMSUB_S:
        gen_helper_fnmsub_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                            cpu_fpr[rs3], rm_reg);
        break;
    case OPC_RISC_FNMSUB_D:
        gen_helper_fnmsub_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                            cpu_fpr[rs3], rm_reg);
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }
    tcg_temp_free_i64(rm_reg);
}

static void gen_fp_fnmadd(DisasContext *ctx, uint32_t opc, int rd,
        int rs1, int rs2, int rs3, int rm)
{
    generate_log(ctx->pc, "we are in a floating point op, might be wrong!");

    TCGv_i64 rm_reg = tcg_temp_new_i64();
    tcg_gen_movi_i64(rm_reg, rm);

    switch (opc) {
    case OPC_RISC_FNMADD_S:
        gen_helper_fnmadd_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                            cpu_fpr[rs3], rm_reg);
        break;
    case OPC_RISC_FNMADD_D:
        gen_helper_fnmadd_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                            cpu_fpr[rs3], rm_reg);
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }
    tcg_temp_free_i64(rm_reg);
}

static void gen_fp_arith(DisasContext *ctx, uint32_t opc, int rd,
        int rs1, int rs2, int rm)
{
    TCGv_i64 rm_reg = tcg_temp_new_i64();
    TCGv write_int_rd = tcg_temp_new();
    tcg_gen_movi_i64(rm_reg, rm);
    generate_log(ctx->pc, "gen_fp_arith: most probably something bad might happen for floats!!!!");
    switch (opc) {
    case OPC_RISC_FADD_S:
        gen_helper_fadd_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                          rm_reg);
        break;
    case OPC_RISC_FSUB_S:
        gen_helper_fsub_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                          rm_reg);
        break;
    case OPC_RISC_FMUL_S:
        gen_helper_fmul_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                          rm_reg);
        break;
    case OPC_RISC_FDIV_S:
        gen_helper_fdiv_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                          rm_reg);
        break;
    case OPC_RISC_FSGNJ_S:
        gen_fsgnj(ctx, rd, rs1, rs2, rm, INT32_MIN);
        break;
    case OPC_RISC_FMIN_S:
        /* also handles: OPC_RISC_FMAX_S */
        if (rm == 0x0) {
            gen_helper_fmin_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2]);
        } else if (rm == 0x1) {
            gen_helper_fmax_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2]);
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        break;
    case OPC_RISC_FSQRT_S:
        gen_helper_fsqrt_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], rm_reg);
        break;
    case OPC_RISC_FEQ_S:
        /* also handles: OPC_RISC_FLT_S, OPC_RISC_FLE_S */
        if (rm == 0x0) {
            gen_helper_fle_s(write_int_rd, cpu_env, cpu_fpr[rs1], cpu_fpr[rs2]);
        } else if (rm == 0x1) {
            gen_helper_flt_s(write_int_rd, cpu_env, cpu_fpr[rs1], cpu_fpr[rs2]);
        } else if (rm == 0x2) {
            gen_helper_feq_s(write_int_rd, cpu_env, cpu_fpr[rs1], cpu_fpr[rs2]);
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        gen_set_gpr(rd, write_int_rd);
        break;
    case OPC_RISC_FCVT_W_S:
        /* also OPC_RISC_FCVT_WU_S, OPC_RISC_FCVT_L_S, OPC_RISC_FCVT_LU_S */
        if (rs2 == 0x0) { /* FCVT_W_S */
            gen_helper_fcvt_w_s(write_int_rd, cpu_env, cpu_fpr[rs1], rm_reg);
        } else if (rs2 == 0x1) { /* FCVT_WU_S */
            gen_helper_fcvt_wu_s(write_int_rd, cpu_env, cpu_fpr[rs1], rm_reg);
        } else if (rs2 == 0x2) { /* FCVT_L_S */
#if defined(TARGET_RISCV64)
            gen_helper_fcvt_l_s(write_int_rd, cpu_env, cpu_fpr[rs1], rm_reg);
#else
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
#endif
        } else if (rs2 == 0x3) { /* FCVT_LU_S */
#if defined(TARGET_RISCV64)
            gen_helper_fcvt_lu_s(write_int_rd, cpu_env, cpu_fpr[rs1], rm_reg);
#else
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
#endif
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        gen_set_gpr(rd, write_int_rd);
        break;
    case OPC_RISC_FCVT_S_W:
        /* also OPC_RISC_FCVT_S_WU, OPC_RISC_FCVT_S_L, OPC_RISC_FCVT_S_LU */
        gen_get_gpr(write_int_rd, rs1);
        if (rs2 == 0) { /* FCVT_S_W */
            gen_helper_fcvt_s_w(cpu_fpr[rd], cpu_env, write_int_rd, rm_reg);
        } else if (rs2 == 0x1) { /* FCVT_S_WU */
            gen_helper_fcvt_s_wu(cpu_fpr[rd], cpu_env, write_int_rd, rm_reg);
        } else if (rs2 == 0x2) { /* FCVT_S_L */
#if defined(TARGET_RISCV64)
            gen_helper_fcvt_s_l(cpu_fpr[rd], cpu_env, write_int_rd, rm_reg);
#else
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
#endif
        } else if (rs2 == 0x3) { /* FCVT_S_LU */
#if defined(TARGET_RISCV64)
            gen_helper_fcvt_s_lu(cpu_fpr[rd], cpu_env, write_int_rd, rm_reg);
#else
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
#endif
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        break;
    case OPC_RISC_FMV_X_S: {
            int fp_ok = gen_new_label();
            int done = gen_new_label();

            // check MSTATUS.FS
            tcg_gen_ld_tl(write_int_rd, cpu_env, offsetof(CPUState, mstatus));
            tcg_gen_andi_tl(write_int_rd, write_int_rd, MSTATUS_FS);
            tcg_gen_brcondi_tl(TCG_COND_NE, write_int_rd, 0x0, fp_ok);
            // MSTATUS_FS field was zero:
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            tcg_gen_br(done);

            // proceed with operation
            gen_set_label(fp_ok);
            /* also OPC_RISC_FCLASS_S */
            if (rm == 0x0) { /* FMV */
#if defined(TARGET_RISCV64)
                tcg_gen_ext32s_tl(write_int_rd, cpu_fpr[rs1]);
#else
                tcg_gen_trunc_i64_i32(write_int_rd, cpu_fpr[rs1]);
#endif
            } else if (rm == 0x1) {
                gen_helper_fclass_s(write_int_rd, cpu_env, cpu_fpr[rs1]);
            } else {
                kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            }
            gen_set_gpr(rd, write_int_rd);
            gen_set_label(done);
            break;
        }
    case OPC_RISC_FMV_S_X:
        {
            int fp_ok = gen_new_label();
            int done = gen_new_label();

            // check MSTATUS.FS
            tcg_gen_ld_tl(write_int_rd, cpu_env, offsetof(CPUState, mstatus));
            tcg_gen_andi_tl(write_int_rd, write_int_rd, MSTATUS_FS);
            tcg_gen_brcondi_tl(TCG_COND_NE, write_int_rd, 0x0, fp_ok);
            // MSTATUS_FS field was zero:
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            tcg_gen_br(done);

            // proceed with operation
            gen_set_label(fp_ok);
            gen_get_gpr(write_int_rd, rs1);
#if defined(TARGET_RISCV64)
            tcg_gen_mov_tl(cpu_fpr[rd], write_int_rd);
#else
            tcg_gen_extu_i32_i64(cpu_fpr[rd], write_int_rd);
#endif
            gen_set_label(done);
            break;
        }
    /* double */
    case OPC_RISC_FADD_D:
        gen_helper_fadd_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                          rm_reg);
        break;
    case OPC_RISC_FSUB_D:
        gen_helper_fsub_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                          rm_reg);
        break;
    case OPC_RISC_FMUL_D:
        gen_helper_fmul_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                          rm_reg);
        break;
    case OPC_RISC_FDIV_D:
        gen_helper_fdiv_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2],
                          rm_reg);
        break;
    case OPC_RISC_FSGNJ_D:
        gen_fsgnj(ctx, rd, rs1, rs2, rm, INT64_MIN);
        break;
    case OPC_RISC_FMIN_D:
        /* also OPC_RISC_FMAX_D */
        if (rm == 0x0) {
            gen_helper_fmin_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2]);
        } else if (rm == 0x1) {
            gen_helper_fmax_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], cpu_fpr[rs2]);
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        break;
    case OPC_RISC_FCVT_S_D:
        if (rs2 == 0x1) {
            gen_helper_fcvt_s_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], rm_reg);
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        break;
    case OPC_RISC_FCVT_D_S:
        if (rs2 == 0x0) {
            gen_helper_fcvt_d_s(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], rm_reg);
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        break;
    case OPC_RISC_FSQRT_D:
        gen_helper_fsqrt_d(cpu_fpr[rd], cpu_env, cpu_fpr[rs1], rm_reg);
        break;
    case OPC_RISC_FEQ_D:
        /* also OPC_RISC_FLT_D, OPC_RISC_FLE_D */
        if (rm == 0x0) {
            gen_helper_fle_d(write_int_rd, cpu_env, cpu_fpr[rs1], cpu_fpr[rs2]);
        } else if (rm == 0x1) {
            gen_helper_flt_d(write_int_rd, cpu_env, cpu_fpr[rs1], cpu_fpr[rs2]);
        } else if (rm == 0x2) {
            gen_helper_feq_d(write_int_rd, cpu_env, cpu_fpr[rs1], cpu_fpr[rs2]);
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        gen_set_gpr(rd, write_int_rd);
        break;
    case OPC_RISC_FCVT_W_D:
        /* also OPC_RISC_FCVT_WU_D, OPC_RISC_FCVT_L_D, OPC_RISC_FCVT_LU_D */
        if (rs2 == 0x0) {
            gen_helper_fcvt_w_d(write_int_rd, cpu_env, cpu_fpr[rs1], rm_reg);
        } else if (rs2 == 0x1) {
            gen_helper_fcvt_wu_d(write_int_rd, cpu_env, cpu_fpr[rs1], rm_reg);
        } else if (rs2 == 0x2) {
#if defined(TARGET_RISCV64)
            gen_helper_fcvt_l_d(write_int_rd, cpu_env, cpu_fpr[rs1], rm_reg);
#else
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
#endif
        } else if (rs2 == 0x3) {
#if defined(TARGET_RISCV64)
            gen_helper_fcvt_lu_d(write_int_rd, cpu_env, cpu_fpr[rs1], rm_reg);
#else
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
#endif
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        gen_set_gpr(rd, write_int_rd);
        break;
    case OPC_RISC_FCVT_D_W:
        /* also OPC_RISC_FCVT_D_WU, OPC_RISC_FCVT_D_L, OPC_RISC_FCVT_D_LU */
        gen_get_gpr(write_int_rd, rs1);
        if (rs2 == 0x0) {
            gen_helper_fcvt_d_w(cpu_fpr[rd], cpu_env, write_int_rd, rm_reg);
        } else if (rs2 == 0x1) {
            gen_helper_fcvt_d_wu(cpu_fpr[rd], cpu_env, write_int_rd, rm_reg);
        } else if (rs2 == 0x2) {
#if defined(TARGET_RISCV64)
            gen_helper_fcvt_d_l(cpu_fpr[rd], cpu_env, write_int_rd, rm_reg);
#else
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
#endif
        } else if (rs2 == 0x3) {
#if defined(TARGET_RISCV64)
            gen_helper_fcvt_d_lu(cpu_fpr[rd], cpu_env, write_int_rd, rm_reg);
#else
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
#endif
        } else {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        }
        break;
#if defined(TARGET_RISCV64)
    case OPC_RISC_FMV_X_D:
        {
            int fp_ok = gen_new_label();
            int done = gen_new_label();

            // check MSTATUS.FS
            tcg_gen_ld_tl(write_int_rd, cpu_env, offsetof(CPUState, mstatus));
            tcg_gen_andi_tl(write_int_rd, write_int_rd, MSTATUS_FS);
            tcg_gen_brcondi_tl(TCG_COND_NE, write_int_rd, 0x0, fp_ok);
            // MSTATUS_FS field was zero:
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            tcg_gen_br(done);

            // proceed with operation
            gen_set_label(fp_ok);
            /* also OPC_RISC_FCLASS_D */
            if (rm == 0x0) { /* FMV */
                tcg_gen_mov_tl(write_int_rd, cpu_fpr[rs1]);
            } else if (rm == 0x1) {
                gen_helper_fclass_d(write_int_rd, cpu_env, cpu_fpr[rs1]);
            } else {
                kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            }
            gen_set_gpr(rd, write_int_rd);
            gen_set_label(done);
            break;
        }
    case OPC_RISC_FMV_D_X:
        {
            int fp_ok = gen_new_label();
            int done = gen_new_label();

            // check MSTATUS.FS
            tcg_gen_ld_tl(write_int_rd, cpu_env, offsetof(CPUState, mstatus));
            tcg_gen_andi_tl(write_int_rd, write_int_rd, MSTATUS_FS);
            tcg_gen_brcondi_tl(TCG_COND_NE, write_int_rd, 0x0, fp_ok);
            // MSTATUS_FS field was zero:
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            tcg_gen_br(done);

            // proceed with operation
            gen_set_label(fp_ok);
            gen_get_gpr(write_int_rd, rs1);
            tcg_gen_mov_tl(cpu_fpr[rd], write_int_rd);
            gen_set_label(done);
            break;
        }
#endif
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }
    tcg_temp_free_i64(rm_reg);
    tcg_temp_free(write_int_rd);
}

static void gen_system(DisasContext *ctx, uint32_t opc,
                      int rd, int rs1, int csr)
{
    TCGv source1, csr_store, dest, rs1_pass, imm_rs1;
    source1 = tcg_temp_new();
    csr_store = tcg_temp_new();
    dest = tcg_temp_new();
    rs1_pass = tcg_temp_new();
    imm_rs1 = tcg_temp_new();
    gen_get_gpr(source1, rs1);
    tcg_gen_movi_tl(cpu_pc, ctx->pc);
    tcg_gen_movi_tl(rs1_pass, rs1);
    tcg_gen_movi_tl(csr_store, csr); /* copy into temp reg to feed to helper */

    switch (opc) {
    case OPC_RISC_ECALL:
        switch (csr) {
        case 0x0: /* ECALL */
            /* always generates U-level ECALL, fixed in do_interrupt handler */
            generate_exception(ctx, RISCV_EXCP_U_ECALL);
            gen_exit_tb(0, ctx->tb); /* no chaining */
            ctx->bstate = BS_BRANCH;
            break;
        case 0x1: /* EBREAK */
            generate_exception(ctx, RISCV_EXCP_BREAKPOINT);
            gen_exit_tb(0, ctx->tb); /* no chaining */
            ctx->bstate = BS_BRANCH;
            break;
        case 0x002: /* URET */
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            break;
        case 0x102: /* SRET */
            gen_helper_sret(cpu_pc, cpu_env, cpu_pc);
            gen_exit_tb(0, ctx->tb); /* no chaining */
            ctx->bstate = BS_BRANCH;
            break;
        case 0x202: /* HRET */
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            break;
        case 0x302: /* MRET */
            gen_helper_mret(cpu_pc, cpu_env, cpu_pc);
            gen_exit_tb(0, ctx->tb); /* no chaining */
            ctx->bstate = BS_BRANCH;
            break;
        case 0x7b2: /* DRET */
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            break;
        case 0x105: /* WFI */
            tcg_gen_movi_tl(cpu_pc, ctx->next_pc);
            gen_helper_wfi(cpu_env);
            break;
        case 0x104: /* SFENCE.VM */
            gen_helper_tlb_flush(cpu_env);
            break;
        case 0x120: /* SFENCE.VMA */
            /* TODO: handle ASID specific fences */
            gen_helper_tlb_flush(cpu_env);
            break;
        default:
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
        break;
    default:
        tcg_gen_movi_tl(imm_rs1, rs1);
        switch (opc) {
        case OPC_RISC_CSRRW:
            gen_helper_csrrw(dest, cpu_env, source1, csr_store);
            break;
        case OPC_RISC_CSRRS:
            gen_helper_csrrs(dest, cpu_env, source1, csr_store, rs1_pass);
            break;
        case OPC_RISC_CSRRC:
            gen_helper_csrrc(dest, cpu_env, source1, csr_store, rs1_pass);
            break;
        case OPC_RISC_CSRRWI:
            gen_helper_csrrw(dest, cpu_env, imm_rs1, csr_store);
            break;
        case OPC_RISC_CSRRSI:
            gen_helper_csrrs(dest, cpu_env, imm_rs1, csr_store, rs1_pass);
            break;
        case OPC_RISC_CSRRCI:
            gen_helper_csrrc(dest, cpu_env, imm_rs1, csr_store, rs1_pass);
            break;
        default:
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
        gen_set_gpr(rd, dest);
        /* end tb since we may be changing priv modes, to get mmu_index right */
        tcg_gen_movi_tl(cpu_pc, ctx->next_pc);
        gen_exit_tb(0, ctx->tb); /* no chaining */
        ctx->bstate = BS_BRANCH;
        break;
    }
    tcg_temp_free(source1);
    tcg_temp_free(csr_store);
    tcg_temp_free(dest);
    tcg_temp_free(rs1_pass);
    tcg_temp_free(imm_rs1);
}

static void decode_RV32_64C0(DisasContext *ctx)
{
    uint8_t funct3 = extract32(ctx->opcode, 13, 3);
    uint8_t rd_rs2 = GET_C_RS2S(ctx->opcode);
    uint8_t rs1s = GET_C_RS1S(ctx->opcode);

    switch (funct3) {
    case 0:
        /* illegal */
        if (ctx->opcode == 0) {
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        } else {
            /* C.ADDI4SPN -> addi rd', x2, zimm[9:2]*/
            gen_arith_imm(ctx, OPC_RISC_ADDI, rd_rs2, 2,
                          GET_C_ADDI4SPN_IMM(ctx->opcode));
        }
        break;
    case 1:
        /* C.FLD -> fld rd', offset[7:3](rs1')*/
        gen_fp_load(ctx, OPC_RISC_FLD, rd_rs2, rs1s,
                    GET_C_LD_IMM(ctx->opcode));
        /* C.LQ(RV128) */
        break;
    case 2:
        /* C.LW -> lw rd', offset[6:2](rs1') */
        gen_load(ctx, OPC_RISC_LW, rd_rs2, rs1s,
                 GET_C_LW_IMM(ctx->opcode));
        break;
    case 3:
#if defined(TARGET_RISCV64)
        /* C.LD(RV64/128) -> ld rd', offset[7:3](rs1')*/
        gen_load(ctx, OPC_RISC_LD, rd_rs2, rs1s,
                 GET_C_LD_IMM(ctx->opcode));
#else
        /* C.FLW (RV32) -> flw rd', offset[6:2](rs1')*/
        gen_fp_load(ctx, OPC_RISC_FLW, rd_rs2, rs1s,
                    GET_C_LW_IMM(ctx->opcode));
#endif
        break;
    case 4:
        /* reserved */
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    case 5:
        /* C.FSD(RV32/64) -> fsd rs2', offset[7:3](rs1') */
        gen_fp_store(ctx, OPC_RISC_FSD, rs1s, rd_rs2,
                     GET_C_LD_IMM(ctx->opcode));
        /* C.SQ (RV128) */
        break;
    case 6:
        /* C.SW -> sw rs2', offset[6:2](rs1')*/
        gen_store(ctx, OPC_RISC_SW, rs1s, rd_rs2,
                  GET_C_LW_IMM(ctx->opcode));
        break;
    case 7:
#if defined(TARGET_RISCV64)
        /* C.SD (RV64/128) -> sd rs2', offset[7:3](rs1')*/
        gen_store(ctx, OPC_RISC_SD, rs1s, rd_rs2,
                  GET_C_LD_IMM(ctx->opcode));
#else
        /* C.FSW (RV32) -> fsw rs2', offset[6:2](rs1')*/
        gen_fp_store(ctx, OPC_RISC_FSW, rs1s, rd_rs2,
                     GET_C_LW_IMM(ctx->opcode));
#endif
        break;
    }
}

static void decode_RV32_64C1(CPUState *env, DisasContext *ctx)
{
    uint8_t funct3 = extract32(ctx->opcode, 13, 3);
    uint8_t rd_rs1 = GET_C_RS1(ctx->opcode);
    uint8_t rs1s, rs2s;
    uint8_t funct2;

    switch (funct3) {
    case 0:
        /* C.ADDI -> addi rd, rd, nzimm[5:0] */
        gen_arith_imm(ctx, OPC_RISC_ADDI, rd_rs1, rd_rs1,
                      GET_C_IMM(ctx->opcode));
        break;
    case 1:
#if defined(TARGET_RISCV64)
        /* C.ADDIW (RV64/128) -> addiw rd, rd, imm[5:0]*/
        gen_arith_imm(ctx, OPC_RISC_ADDIW, rd_rs1, rd_rs1,
                      GET_C_IMM(ctx->opcode));
#else
        /* C.JAL(RV32) -> jal x1, offset[11:1] */
        gen_jal(env, ctx, 1, GET_C_J_IMM(ctx->opcode));
#endif
        break;
    case 2:
        /* C.LI -> addi rd, x0, imm[5:0]*/
        gen_arith_imm(ctx, OPC_RISC_ADDI, rd_rs1, 0, GET_C_IMM(ctx->opcode));
        break;
    case 3:
        if (rd_rs1 == 2) {
            /* C.ADDI16SP -> addi x2, x2, nzimm[9:4]*/
            gen_arith_imm(ctx, OPC_RISC_ADDI, 2, 2,
                          GET_C_ADDI16SP_IMM(ctx->opcode));
        } else if (rd_rs1 != 0) {
            /* C.LUI (rs1/rd =/= {0,2}) -> lui rd, nzimm[17:12]*/
            tcg_gen_movi_tl(cpu_gpr[rd_rs1],
                            GET_C_IMM(ctx->opcode) << 12);
        }
        break;
    case 4:
        funct2 = extract32(ctx->opcode, 10, 2);
        rs1s = GET_C_RS1S(ctx->opcode);
        switch (funct2) {
        case 0: /* C.SRLI(RV32) -> srli rd', rd', shamt[5:0] */
            gen_arith_imm(ctx, OPC_RISC_SHIFT_RIGHT_I, rs1s, rs1s,
                               GET_C_ZIMM(ctx->opcode));
            /* C.SRLI64(RV128) */
            break;
        case 1:
            /* C.SRAI -> srai rd', rd', shamt[5:0]*/
            gen_arith_imm(ctx, OPC_RISC_SHIFT_RIGHT_I, rs1s, rs1s,
                            GET_C_ZIMM(ctx->opcode) | 0x400);
            /* C.SRAI64(RV128) */
            break;
        case 2:
            /* C.ANDI -> andi rd', rd', imm[5:0]*/
            gen_arith_imm(ctx, OPC_RISC_ANDI, rs1s, rs1s,
                          GET_C_IMM(ctx->opcode));
            break;
        case 3:
            funct2 = extract32(ctx->opcode, 5, 2);
            rs2s = GET_C_RS2S(ctx->opcode);
            switch (funct2) {
            case 0:
                /* C.SUB -> sub rd', rd', rs2' */
                if (extract32(ctx->opcode, 12, 1) == 0) {
                    gen_arith(ctx, OPC_RISC_SUB, rs1s, rs1s, rs2s);
                }
#if defined(TARGET_RISCV64)
                else {
                    gen_arith(ctx, OPC_RISC_SUBW, rs1s, rs1s, rs2s);
                }
#endif
                break;
            case 1:
                /* C.XOR -> xor rs1', rs1', rs2' */
                if (extract32(ctx->opcode, 12, 1) == 0) {
                    gen_arith(ctx, OPC_RISC_XOR, rs1s, rs1s, rs2s);
                }
#if defined(TARGET_RISCV64)
                else {
                    /* C.ADDW (RV64/128) */
                    gen_arith(ctx, OPC_RISC_ADDW, rs1s, rs1s, rs2s);
                }
#endif
                break;
            case 2:
                /* C.OR -> or rs1', rs1', rs2' */
                gen_arith(ctx, OPC_RISC_OR, rs1s, rs1s, rs2s);
                break;
            case 3:
                /* C.AND -> and rs1', rs1', rs2' */
                gen_arith(ctx, OPC_RISC_AND, rs1s, rs1s, rs2s);
                break;
            }
            break;
        }
        break;
    case 5:
        /* C.J -> jal x0, offset[11:1]*/
        gen_jal(env, ctx, 0, GET_C_J_IMM(ctx->opcode));
        break;
    case 6:
        /* C.BEQZ -> beq rs1', x0, offset[8:1]*/
        rs1s = GET_C_RS1S(ctx->opcode);
        gen_branch(env, ctx, OPC_RISC_BEQ, rs1s, 0, GET_C_B_IMM(ctx->opcode));
        break;
    case 7:
        /* C.BNEZ -> bne rs1', x0, offset[8:1]*/
        rs1s = GET_C_RS1S(ctx->opcode);
        gen_branch(env, ctx, OPC_RISC_BNE, rs1s, 0, GET_C_B_IMM(ctx->opcode));
        break;
    }
}

static void decode_RV32_64C2(CPUState *env, DisasContext *ctx)
{
    uint8_t rd, rs2;
    uint8_t funct3 = extract32(ctx->opcode, 13, 3);

    rd = GET_RD(ctx->opcode);

    switch (funct3) {
    case 0: /* C.SLLI -> slli rd, rd, shamt[5:0]
               C.SLLI64 -> */
        gen_arith_imm(ctx, OPC_RISC_SLLI, rd, rd, GET_C_ZIMM(ctx->opcode));
        break;
    case 1: /* C.FLDSP(RV32/64DC) -> fld rd, offset[8:3](x2) */
        gen_fp_load(ctx, OPC_RISC_FLD, rd, 2, GET_C_LDSP_IMM(ctx->opcode));
        break;
    case 2: /* C.LWSP -> lw rd, offset[7:2](x2) */
        gen_load(ctx, OPC_RISC_LW, rd, 2, GET_C_LWSP_IMM(ctx->opcode));
        break;
    case 3:
#if defined(TARGET_RISCV64)
        /* C.LDSP(RVC64) -> ld rd, offset[8:3](x2) */
        gen_load(ctx, OPC_RISC_LD, rd, 2, GET_C_LDSP_IMM(ctx->opcode));
#else
        /* C.FLWSP(RV32FC) -> flw rd, offset[7:2](x2) */
        gen_fp_load(ctx, OPC_RISC_FLW, rd, 2, GET_C_LWSP_IMM(ctx->opcode));
#endif
        break;
    case 4:
        rs2 = GET_C_RS2(ctx->opcode);

        if (extract32(ctx->opcode, 12, 1) == 0) {
            if (rs2 == 0) {
                /* C.JR -> jalr x0, rs1, 0*/
                gen_jalr(env, ctx, OPC_RISC_JALR, 0, rd, 0);
            } else {
                /* C.MV -> add rd, x0, rs2 */
                gen_arith(ctx, OPC_RISC_ADD, rd, 0, rs2);
            }
        } else {
            if (rd == 0) {
                /* C.EBREAK -> ebreak*/
                gen_system(ctx, OPC_RISC_ECALL, 0, 0, 0x1);
            } else {
                if (rs2 == 0) {
                    /* C.JALR -> jalr x1, rs1, 0*/
                    gen_jalr(env, ctx, OPC_RISC_JALR, 1, rd, 0);
                } else {
                    /* C.ADD -> add rd, rd, rs2 */
                    gen_arith(ctx, OPC_RISC_ADD, rd, rd, rs2);
                }
            }
        }
        break;
    case 5:
        /* C.FSDSP -> fsd rs2, offset[8:3](x2)*/
        gen_fp_store(ctx, OPC_RISC_FSD, 2, GET_C_RS2(ctx->opcode),
                     GET_C_SDSP_IMM(ctx->opcode));
        /* C.SQSP */
        break;
    case 6: /* C.SWSP -> sw rs2, offset[7:2](x2)*/
        gen_store(ctx, OPC_RISC_SW, 2, GET_C_RS2(ctx->opcode),
                  GET_C_SWSP_IMM(ctx->opcode));
        break;
    case 7:
#if defined(TARGET_RISCV64)
        /* C.SDSP(Rv64/128) -> sd rs2, offset[8:3](x2)*/
        gen_store(ctx, OPC_RISC_SD, 2, GET_C_RS2(ctx->opcode),
                  GET_C_SDSP_IMM(ctx->opcode));
#else
        /* C.FSWSP(RV32) -> fsw rs2, offset[7:2](x2) */
        gen_fp_store(ctx, OPC_RISC_FSW, 2, GET_C_RS2(ctx->opcode),
                     GET_C_SWSP_IMM(ctx->opcode));
#endif
        break;
    }
}

static void decode_RV32_64C(CPUState *env, DisasContext *ctx)
{
    uint8_t op = extract32(ctx->opcode, 0, 2);

    switch (op) {
    case 0:
        decode_RV32_64C0(ctx);
        break;
    case 1:
        decode_RV32_64C1(env, ctx);
        break;
    case 2:
        decode_RV32_64C2(env, ctx);
        break;
    }
}

static void decode_RV32_64G(CPUState *env, DisasContext *ctx)
{
    int rs1;
    int rs2;
    int rd;
    uint32_t op;
    target_long imm;
    generate_log(ctx->pc, "decode_RV32_64G, ctx->opcode = 0x%08X at pc=0x%08X", ctx->opcode, ctx->pc);

    /* We do not do misaligned address check here: the address should never be
     * misaligned at this point. Instructions that set PC must do the check,
     * since epc must be the address of the instruction that caused us to
     * perform the misaligned instruction fetch */

    op = MASK_OP_MAJOR(ctx->opcode);
    rs1 = GET_RS1(ctx->opcode);
    rs2 = GET_RS2(ctx->opcode);
    rd = GET_RD(ctx->opcode);
    imm = GET_IMM(ctx->opcode);

    generate_log(ctx->pc, "decode_RV32_64G op=0x%X", op);

    switch (op) {
    case OPC_RISC_LUI:
        if (rd == 0) {
            break; /* NOP */
        }
        tcg_gen_movi_tl(cpu_gpr[rd], sextract64(ctx->opcode, 12, 20) << 12);
        break;
    case OPC_RISC_AUIPC:
        if (rd == 0) {
            break; /* NOP */
        }
        tcg_gen_movi_tl(cpu_gpr[rd], (sextract64(ctx->opcode, 12, 20) << 12) +
               ctx->pc);
        break;
    case OPC_RISC_JAL:
        imm = GET_JAL_IMM(ctx->opcode);
        gen_jal(env, ctx, rd, imm);
        break;
    case OPC_RISC_JALR:
        generate_log(ctx->pc, "going to jalr");
        gen_jalr(env, ctx, MASK_OP_JALR(ctx->opcode), rd, rs1, imm);
        break;
    case OPC_RISC_BRANCH:
        generate_log(ctx->pc, "going to branch, rs1=%X rs2=%X imm=%lX", rs1, rs2, GET_B_IMM(ctx->opcode));
        gen_branch(env, ctx, MASK_OP_BRANCH(ctx->opcode), rs1, rs2,
                   GET_B_IMM(ctx->opcode));
        break;
    case OPC_RISC_LOAD:
        gen_load(ctx, MASK_OP_LOAD(ctx->opcode), rd, rs1, imm);
        break;
    case OPC_RISC_STORE:
        gen_store(ctx, MASK_OP_STORE(ctx->opcode), rs1, rs2,
                  GET_STORE_IMM(ctx->opcode));
        break;
    case OPC_RISC_ARITH_IMM:
#if defined(TARGET_RISCV64)
    case OPC_RISC_ARITH_IMM_W:
#endif
        if (rd == 0) {
            break; /* NOP */
        }
        gen_arith_imm(ctx, MASK_OP_ARITH_IMM(ctx->opcode), rd, rs1, imm);
        break;
    case OPC_RISC_ARITH:
#if defined(TARGET_RISCV64)
    case OPC_RISC_ARITH_W:
#endif
        if (rd == 0) {
            break; /* NOP */
        }
        gen_arith(ctx, MASK_OP_ARITH(ctx->opcode), rd, rs1, rs2);
        break;
    case OPC_RISC_FP_LOAD:
        gen_fp_load(ctx, MASK_OP_FP_LOAD(ctx->opcode), rd, rs1, imm);
        break;
    case OPC_RISC_FP_STORE:
        gen_fp_store(ctx, MASK_OP_FP_STORE(ctx->opcode), rs1, rs2,
                     GET_STORE_IMM(ctx->opcode));
        break;
    case OPC_RISC_ATOMIC:
        gen_atomic(ctx, MASK_OP_ATOMIC(ctx->opcode), rd, rs1, rs2);
        break;
    case OPC_RISC_FMADD:
        gen_fp_fmadd(ctx, MASK_OP_FP_FMADD(ctx->opcode), rd, rs1, rs2,
                     GET_RS3(ctx->opcode), GET_RM(ctx->opcode));
        break;
    case OPC_RISC_FMSUB:
        gen_fp_fmsub(ctx, MASK_OP_FP_FMSUB(ctx->opcode), rd, rs1, rs2,
                     GET_RS3(ctx->opcode), GET_RM(ctx->opcode));
        break;
    case OPC_RISC_FNMSUB:
        gen_fp_fnmsub(ctx, MASK_OP_FP_FNMSUB(ctx->opcode), rd, rs1, rs2,
                      GET_RS3(ctx->opcode), GET_RM(ctx->opcode));
        break;
    case OPC_RISC_FNMADD:
        gen_fp_fnmadd(ctx, MASK_OP_FP_FNMADD(ctx->opcode), rd, rs1, rs2,
                      GET_RS3(ctx->opcode), GET_RM(ctx->opcode));
        break;
    case OPC_RISC_FP_ARITH:
        gen_fp_arith(ctx, MASK_OP_FP_ARITH(ctx->opcode), rd, rs1, rs2,
                     GET_RM(ctx->opcode));
        break;
    case OPC_RISC_FENCE:
        /* standard fence is nop, fence_i flushes TB (like an icache): */
        if (ctx->opcode & 0x1000) { /* FENCE_I */
            gen_helper_fence_i(cpu_env);
            tcg_gen_movi_tl(cpu_pc, ctx->next_pc);
            gen_exit_tb(0, ctx->tb); /* no chaining */
            ctx->bstate = BS_BRANCH;
        }
        break;
    case OPC_RISC_SYSTEM:
        gen_system(ctx, MASK_OP_SYSTEM(ctx->opcode), rd, rs1,
                   (ctx->opcode & 0xFFF00000) >> 20);
        break;
    default:
        kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
        break;
    }
    generate_log(ctx->pc, "after RV32_64G");
}

static int disas_insn(CPUState *env, DisasContext *ctx)
{
    /* check for compressed insn */
    if (extract32(ctx->opcode, 0, 2) != 3) {
        if (!riscv_has_ext(env, RISCV_FEATURE_RVC)) {
            tlib_log(LOG_LEVEL_ERROR, "RISC-V C instruction set is not enabled for this CPU!");
            kill_unknown(ctx, RISCV_EXCP_ILLEGAL_INST);
	        return 0;
        } else {
            ctx->next_pc = ctx->pc + 2;
            decode_RV32_64C(env, ctx);
	        ctx->pc = ctx->next_pc;
	        return 2;
        }
    } else {
        ctx->next_pc = ctx->pc + 4;
        decode_RV32_64G(env, ctx);
	    ctx->pc = ctx->next_pc;
	    return 4;
    }
}

uint32_t get_disas_flags(CPUState *env, DisasContext *dc) {
    return 0;
}

void create_disas_context(DisasContext *dc, CPUState *env, TranslationBlock *tb) {
    dc->pc = tb->pc;

    /* once we have GDB, the rest of the translate.c implementation should be
       ready for singlestep */
    dc->singlestep_enabled = env->singlestep_enabled;

    dc->tb = tb;
    dc->bstate = BS_NONE;

    dc->mem_idx = cpu_mmu_index(env);
}

int gen_breakpoint(DisasContext *dc, CPUBreakpoint *bp) {
    generate_exception(dc, EXCP_DEBUG);
    /* Advance PC so that clearing the breakpoint will
      invalidate this TB.  */
    dc->pc += 4;
    return 1;
}

void gen_intermediate_code(CPUState *env,
                           TranslationBlock *tb, int max_insns)
{
    DisasContext ctx;

    create_disas_context(&ctx, env, tb);

   tcg_clear_temp_count();

    while (1) {
        if (unlikely(!QTAILQ_EMPTY(&env->breakpoints))) {
            CPUBreakpoint* bp = process_breakpoints(env, ctx.pc);
            if (bp != NULL && gen_breakpoint(&ctx, bp)) {
                break;
            }
        }

        if (tb->search_pc) {
            tcg->gen_opc_pc[gen_opc_ptr - tcg->gen_opc_buf] = ctx.pc;
            tcg->gen_opc_instr_start[gen_opc_ptr - tcg->gen_opc_buf] = 1;
        }

        ctx.opcode = ldl_code(ctx.pc);

        char msg[1024];
        sprintf(msg, "opcode 0x%08X at pc=0x" TARGET_FMT_plx " [tcg id=%d]", ctx.opcode, ctx.pc, (int)(gen_opc_ptr - tcg->gen_opc_buf));
        generate_log(ctx.pc, "---> tcg: we are executing %s", msg);

        tb->prev_size = tb->size;
        tb->size += disas_insn(env, &ctx);
        tb->icount++;

        if (!tb->search_pc)
        {
            // it looks like `search_pc` is set to 1 only when restoring the state;
            // the intention here is to set `original_size` value only during the first block generation
            // so it can be used later when restoring the block
            tb->original_size = tb->size;
        }

    	generate_log(ctx.pc, "<--- tcg: we are done executing %s", msg);

        if (tcg_check_temp_count()) {
            tlib_abortf("TCG temps leak detected at PC %08x", ctx.pc);
        }

        if (ctx.bstate != BS_NONE) {
            break;
        }
        if (ctx.singlestep_enabled) {
            break;
        }
        if ((ctx.pc - (tb->pc & TARGET_PAGE_MASK)) >= TARGET_PAGE_SIZE) {
            break;
        }
        if (tb->icount >= max_insns) {
            ctx.bstate = BS_STOP;
            break;
        }
        if ((gen_opc_ptr - tcg->gen_opc_buf) >= OPC_MAX_SIZE) {
                break;
        }
        if (tb->search_pc && tb->size == tb->original_size)
        {
            // `search_pc` is set to 1 only when restoring the block;
            // this is to ensure that the size of restored block is not bigger than the size of the original one
            ctx.bstate = BS_STOP;
            break;
        }
    }
    if (env->singlestep_enabled && ctx.bstate != BS_BRANCH) {
        if (ctx.bstate == BS_NONE) {
            tcg_gen_movi_tl(cpu_pc, ctx.pc);
        }
        gen_helper_raise_exception_debug(cpu_env);
    } else {
        switch (ctx.bstate) {
        case BS_STOP:
            gen_goto_tb(&ctx, 0, ctx.pc);
            break;
        case BS_NONE: /* handle end of page - DO NOT CHAIN. See gen_goto_tb. */
            tcg_gen_movi_tl(cpu_pc, ctx.pc);
            gen_exit_tb(0, ctx.tb);
            break;
        case BS_BRANCH: /* ops using BS_BRANCH generate own exit seq */
        default:
            break;
        }
    }

    tb->disas_flags = get_disas_flags(env, &ctx);
}

void translate_init(void)
{
    int i;
    gen_helpers();

    cpu_env = tcg_global_reg_new_ptr(TCG_AREG0, "env");

    /* cpu_gpr[0] is a placeholder for the zero register. Do not use it. */
    /* Use the gen_set_gpr and gen_get_gpr helper functions when accessing */
    /* registers, unless you specifically block reads/writes to reg 0 */
    TCGV_UNUSED(cpu_gpr[0]);
    for (i = 1; i < 32; i++) {
        cpu_gpr[i] = tcg_global_mem_new(TCG_AREG0,
                                        offsetof(CPUState, gpr[i]), regnames[i]);
    }

    for (i = 0; i < 32; i++) {
        cpu_fpr[i] = tcg_global_mem_new_i64(TCG_AREG0,
                                        offsetof(CPUState, fpr[i]), fpr_regnames[i]);
    }

    cpu_pc = tcg_global_mem_new(TCG_AREG0, offsetof(CPUState, pc), "pc");

    load_res = tcg_global_mem_new(cpu_env, offsetof(CPUState, load_res),
                             "load_res");
}

void cpu_state_reset(CPUState *env)
{
    bool privilege = env->privilege_architecture_1_10;
    target_ulong mhartid = env->mhartid;
    target_ulong misa_mask = env->misa_mask;
    memset(env, 0, offsetof(CPUState, breakpoints));

    env->mhartid = mhartid;
    env->privilege_architecture_1_10 = privilege;
    env->pc = RISCV_START_PC; // STARTING PC VALUE def'd in cpu.h
    env->exception_index = EXCP_NONE;
    env->priv = PRV_M;
    env->misa = misa_mask;
    env->misa_mask = misa_mask;
}

void restore_state_to_opc(CPUState *env, TranslationBlock *tb, int pc_pos)
{
    env->pc = tcg->gen_opc_pc[pc_pos];
}

int process_interrupt(int interrupt_request, CPUState *env)
{
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        int interruptno = riscv_cpu_hw_interrupts_pending(env);
	    if (interruptno + 1) {
             env->exception_index = RISCV_EXCP_INT_FLAG | interruptno;
             do_interrupt(env);
	     return 1;
	}
    }
    return 0;
}

//TODO: These empty implementations are required due to problems with weak attribute.
//Remove this after #7035.
void cpu_exec_epilogue(CPUState *env)
{
}

void cpu_exec_prologue(CPUState *env)
{
}

