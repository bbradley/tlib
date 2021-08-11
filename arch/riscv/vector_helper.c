/*
 *  RISCV vector extention helpers
 *
 *  Copyright (c) Antmicro
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
#include "cpu.h"

static inline void require_vec(CPUState *env)
{
    if (!(env->mstatus & MSTATUS_VS)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
}

#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))

/*
 * Handle configuration to vector registers
 *
 * Adapted from Spike's processor_t::vectorUnit_t::set_vl
 */
target_ulong helper_vsetvl(CPUState *env, target_ulong rd, target_ulong rs1,
                           target_ulong rs1_pass, target_ulong rs2_pass,
                           uint32_t is_rs1_imm)
{
    require_vec(env);

    target_ulong prev_csr_vl = env->vl;
    target_ulong vlen = env->vlenb * 8;

    env->vtype = rs2_pass;
    env->vsew = 1 << (GET_VTYPE_VSEW(rs2_pass) + 3);
    env->vlmul = GET_VTYPE_VLMUL(rs2_pass);
    int8_t vlmul = (int8_t)(env->vlmul << 5) >> 5;
    env->vflmul = vlmul >= 0 ? 1 << vlmul : 1.0 / (1 << -vlmul);
    env->vlmax = (target_ulong)(vlen / env->vsew * env->vflmul);
    env->vta = GET_VTYPE_VTA(rs2_pass);
    env->vma = GET_VTYPE_VMA(rs2_pass);

    float ceil_vfmul = MIN(env->vflmul, 1.0f);
    env->vill = !(env->vflmul >= 0.125 && env->vflmul <= 8)
           || env->vsew > (ceil_vfmul * env->elen)
           || (rs2_pass >> 8) != 0;

    if (env->vill) {
        env->vtype |= ((target_ulong)1) << (TARGET_LONG_BITS - 1);
        env->vlmax = 0;
    }
    if (is_rs1_imm == 1) {  // vsetivli
        env->vl = MIN(rs1_pass, env->vlmax);
    } else if (env->vlmax == 0) {  // AVL encoding for vsetvl and vsetvli
        env->vl = 0;
    } else if (rd == 0 && rs1 == 0) {
        // Keep existing VL value
        env->vl = MIN(prev_csr_vl, env->vlmax);
    } else if (rs1 == 0 && rd != 0) {
        env->vl = env->vlmax;
    } else {  // Normal stripmining (rs1 != 0)
        env->vl = MIN(rs1_pass, env->vlmax);
    }
    env->vstart = 0;
    return env->vl;
}

void helper_vmv_ivi(CPUState *env, uint32_t vd, int64_t imm)
{
    const target_ulong eew = env->vsew;
    if (V_IDX_INVALID(vd)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    for (int ei = env->vstart; ei < env->vl; ++ei) {
        switch (eew) {
        case 8:
            ((uint8_t *)V(vd))[ei] = imm;
            break;
        case 16:
            ((uint16_t *)V(vd))[ei] = imm;
            break;
        case 32:
            ((uint32_t *)V(vd))[ei] = imm;
            break;
        case 64:
            ((uint64_t *)V(vd))[ei] = imm;
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vmv_ivv(CPUState *env, uint32_t vd, int32_t vs1)
{
    const target_ulong eew = env->vsew;
    if (V_IDX_INVALID(vd) || V_IDX_INVALID(vs1)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    for (int i = env->vstart; i < env->vl; ++i) {
        switch (eew) {
        case 8:
            ((uint8_t *)V(vd))[i] = ((uint8_t *)V(vs1))[i];
            break;
        case 16:
            ((uint16_t *)V(vd))[i] = ((uint16_t *)V(vs1))[i];
            break;
        case 32:
            ((uint32_t *)V(vd))[i] = ((uint32_t *)V(vs1))[i];
            break;
        case 64:
            ((uint64_t *)V(vd))[i] = ((uint64_t *)V(vs1))[i];
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vmerge_ivv(CPUState *env, uint32_t vd, int32_t vs2, int32_t vs1)
{
    if (V_IDX_INVALID(vd) || V_IDX_INVALID(vs2) || V_IDX_INVALID(vs1)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int ei = env->vstart; ei < env->vl; ++ei) {
        uint8_t mask = !(V(0)[ei >> 3] & (1 << (ei & 0x7)));
        switch (eew) {
        case 8:
            ((int8_t *)V(vd))[ei] = mask ? ((int8_t *)V(vs2))[ei] : ((int8_t *)V(vs1))[ei];
            break;
        case 16:
            ((int16_t *)V(vd))[ei] = mask ? ((int16_t *)V(vs2))[ei] : ((int16_t *)V(vs1))[ei];
            break;
        case 32:
            ((int32_t *)V(vd))[ei] = mask ? ((int32_t *)V(vs2))[ei] : ((int32_t *)V(vs1))[ei];
            break;
        case 64:
            ((int64_t *)V(vd))[ei] = mask ? ((int64_t *)V(vs2))[ei] : ((int64_t *)V(vs1))[ei];
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vmerge_ivi(CPUState *env, uint32_t vd, int32_t vs2, target_long rs1)
{
    if (V_IDX_INVALID(vd) || V_IDX_INVALID(vs2)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int ei = env->vstart; ei < env->vl; ++ei) {
        uint8_t mask = !(V(0)[ei >> 3] & (1 << (ei & 0x7)));
        switch (eew) {
        case 8:
            ((int8_t *)V(vd))[ei] = mask ? ((int8_t *)V(vs2))[ei] : rs1;
            break;
        case 16:
            ((int16_t *)V(vd))[ei] = mask ? ((int16_t *)V(vs2))[ei] : rs1;
            break;
        case 32:
            ((int32_t *)V(vd))[ei] = mask ? ((int32_t *)V(vs2))[ei] : rs1;
            break;
        case 64:
            ((int64_t *)V(vd))[ei] = mask ? ((int64_t *)V(vs2))[ei] : rs1;
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vcompress_mvv(CPUState *env, uint32_t vd, int32_t vs2, int32_t vs1)
{
    if (env->vstart != 0 || V_IDX_INVALID(vd) || V_IDX_INVALID(vs2) || V_IDX_INVALID(vs1)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    int di = 0;
    for (int i = 0; i < env->vl; ++i) {
        if (!(V(vs1)[i >> 3] & (1 << (i & 0x7)))) {
            continue;
        }
        switch (eew) {
        case 8:
            ((uint8_t *)V(vd))[di] = ((uint8_t *)V(vs2))[i];
            break;
        case 16:
            ((uint16_t *)V(vd))[di] = ((uint16_t *)V(vs2))[i];
            break;
        case 32:
            ((uint32_t *)V(vd))[di] = ((uint32_t *)V(vs2))[i];
            break;
        case 64:
            ((uint64_t *)V(vd))[di] = ((uint64_t *)V(vs2))[i];
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
        di += 1;
    }
}

void helper_vadc_vvm(CPUState *env, uint32_t vd, int32_t vs2, int32_t vs1)
{
    if (V_IDX_INVALID(vd) || V_IDX_INVALID(vs2) || V_IDX_INVALID(vs1)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        uint8_t carry = !!(V(0)[i >> 3] & (1 << (i & 0x7)));
        switch (eew) {
        case 8:
            ((uint8_t *)V(vd))[i] = ((uint8_t *)V(vs2))[i] + ((uint8_t *)V(vs1))[i] + carry;
            break;
        case 16:
            ((uint16_t *)V(vd))[i] = ((uint16_t *)V(vs2))[i] + ((uint16_t *)V(vs1))[i] + carry;
            break;
        case 32:
            ((uint32_t *)V(vd))[i] = ((uint32_t *)V(vs2))[i] + ((uint32_t *)V(vs1))[i] + carry;
            break;
        case 64:
            ((uint64_t *)V(vd))[i] = ((uint64_t *)V(vs2))[i] + ((uint64_t *)V(vs1))[i] + carry;
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vmadc_vv(CPUState *env, uint32_t vd, int32_t vs2, int32_t vs1)
{
    if (V_IDX_INVALID(vs2) || V_IDX_INVALID(vs1)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        if (!(i & 0x7)) {
            V(vd)[i >> 3] = 0;
        }
        switch (eew) {
        case 8: {
                uint8_t a = ((uint8_t *)V(vs2))[i];
                V(vd)[i >> 3] |= (a + ((uint8_t *)V(vs1))[i] < a) << (i & 0x7);
                break;
            }
        case 16: {
                uint16_t a = ((uint16_t *)V(vs2))[i];
                V(vd)[i >> 3] |= (a + ((uint16_t *)V(vs1))[i] < a) << (i & 0x7);
                break;
            }
        case 32: {
                uint32_t a = ((uint32_t *)V(vs2))[i];
                V(vd)[i >> 3] |= (a + ((uint32_t *)V(vs1))[i] < a) << (i & 0x7);
                break;
            }
        case 64: {
                uint64_t a = ((uint64_t *)V(vs2))[i];
                V(vd)[i >> 3] |= (a + ((uint64_t *)V(vs1))[i] < a) << (i & 0x7);
                break;
            }
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vmadc_vvm(CPUState *env, uint32_t vd, int32_t vs2, int32_t vs1)
{
    if (V_IDX_INVALID(vs2) || V_IDX_INVALID(vs1)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        if (!(i & 0x7)) {
            V(vd)[i >> 3] = 0;
        }
        uint8_t carry = !!(V(0)[i >> 3] & (1 << (i & 0x7)));
        switch (eew) {
        case 8: {
                uint8_t a = ((uint8_t *)V(vs2))[i];
                uint8_t ab = a + ((uint8_t *)V(vs1))[i];
                V(vd)[i >> 3] |= (ab < a || (carry && !(ab + 1))) << (i & 0x7);
                break;
            }
        case 16: {
                uint16_t a = ((uint16_t *)V(vs2))[i];
                uint16_t ab = a + ((uint16_t *)V(vs1))[i];
                V(vd)[i >> 3] |= (ab < a || (carry && !(ab + 1))) << (i & 0x7);
                break;
            }
        case 32: {
                uint32_t a = ((uint32_t *)V(vs2))[i];
                uint32_t ab = a + ((uint32_t *)V(vs1))[i];
                V(vd)[i >> 3] |= (ab < a || (carry && !(ab + 1))) << (i & 0x7);
                break;
            }
        case 64: {
                uint64_t a = ((uint64_t *)V(vs2))[i];
                uint64_t ab = a + ((uint64_t *)V(vs1))[i];
                V(vd)[i >> 3] |= (ab < a || (carry && !(ab + 1))) << (i & 0x7);
                break;
            }
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vsbc_vvm(CPUState *env, uint32_t vd, int32_t vs2, int32_t vs1)
{
    if (V_IDX_INVALID(vd) || V_IDX_INVALID(vs2) || V_IDX_INVALID(vs1)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        uint8_t borrow = !!(V(0)[i >> 3] & (1 << (i & 0x7)));
        switch (eew) {
        case 8:
            ((uint8_t *)V(vd))[i] = ((uint8_t *)V(vs2))[i] - ((uint8_t *)V(vs1))[i] - borrow;
            break;
        case 16:
            ((uint16_t *)V(vd))[i] = ((uint16_t *)V(vs2))[i] - ((uint16_t *)V(vs1))[i] - borrow;
            break;
        case 32:
            ((uint32_t *)V(vd))[i] = ((uint32_t *)V(vs2))[i] - ((uint32_t *)V(vs1))[i] - borrow;
            break;
        case 64:
            ((uint64_t *)V(vd))[i] = ((uint64_t *)V(vs2))[i] - ((uint64_t *)V(vs1))[i] - borrow;
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vmsbc_vv(CPUState *env, uint32_t vd, int32_t vs2, int32_t vs1)
{
    if (V_IDX_INVALID(vs2) || V_IDX_INVALID(vs1)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        if (!(i & 0x7)) {
            V(vd)[i >> 3] = 0;
        }
        switch (eew) {
        case 8:
            V(vd)[i >> 3] |= (((uint8_t *)V(vs2))[i] < ((uint8_t *)V(vs1))[i]) << (i & 0x7);
            break;
        case 16:
            V(vd)[i >> 3] |= (((uint16_t *)V(vs2))[i] < ((uint16_t *)V(vs1))[i]) << (i & 0x7);
            break;
        case 32:
            V(vd)[i >> 3] |= (((uint32_t *)V(vs2))[i] < ((uint32_t *)V(vs1))[i]) << (i & 0x7);
            break;
        case 64:
            V(vd)[i >> 3] |= (((uint64_t *)V(vs2))[i] < ((uint64_t *)V(vs1))[i]) << (i & 0x7);
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vmsbc_vvm(CPUState *env, uint32_t vd, int32_t vs2, int32_t vs1)
{
    if (V_IDX_INVALID(vs2) || V_IDX_INVALID(vs1)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        if (!(i & 0x7)) {
            V(vd)[i >> 3] = 0;
        }
        uint8_t borrow = !!(V(0)[i >> 3] & (1 << (i & 0x7)));
        switch (eew) {
        case 8: {
                uint8_t b = ((uint8_t *)V(vs1))[i];
                V(vd)[i >> 3] |= (((uint8_t *)V(vs2))[i] < b || (borrow && !(b + 1))) << (i & 0x7);
                break;
            }
        case 16: {
                uint16_t b = ((uint16_t *)V(vs1))[i];
                V(vd)[i >> 3] |= (((uint16_t *)V(vs2))[i] < b || (borrow && !(b + 1))) << (i & 0x7);
                break;
            }
        case 32: {
                uint32_t b = ((uint32_t *)V(vs1))[i];
                V(vd)[i >> 3] |= (((uint32_t *)V(vs2))[i] < b || (borrow && !(b + 1))) << (i & 0x7);
                break;
            }
        case 64: {
                uint64_t b = ((uint64_t *)V(vs1))[i];
                V(vd)[i >> 3] |= (((uint64_t *)V(vs2))[i] < b || (borrow && !(b + 1))) << (i & 0x7);
                break;
            }
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vadc_vi(CPUState *env, uint32_t vd, int32_t vs2, target_ulong rs1)
{
    if (V_IDX_INVALID(vd) || V_IDX_INVALID(vs2)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        uint8_t carry = !!(V(0)[i >> 3] & (1 << (i & 0x7)));
        switch (eew) {
        case 8:
            ((uint8_t *)V(vd))[i] = ((uint8_t *)V(vs2))[i] + rs1 + carry;
            break;
        case 16:
            ((uint16_t *)V(vd))[i] = ((uint16_t *)V(vs2))[i] + rs1 + carry;
            break;
        case 32:
            ((uint32_t *)V(vd))[i] = ((uint32_t *)V(vs2))[i] + rs1 + carry;
            break;
        case 64:
            ((uint64_t *)V(vd))[i] = ((uint64_t *)V(vs2))[i] + rs1 + carry;
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vmadc_vi(CPUState *env, uint32_t vd, int32_t vs2, target_ulong rs1)
{
    if (V_IDX_INVALID(vs2)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        if (!(i & 0x7)) {
            V(vd)[i >> 3] = 0;
        }
        switch (eew) {
        case 8: {
                uint8_t a = ((uint8_t *)V(vs2))[i];
                V(vd)[i >> 3] |= (a + rs1 < a) << (i & 0x7);
                break;
            }
        case 16: {
                uint16_t a = ((uint16_t *)V(vs2))[i];
                V(vd)[i >> 3] |= (a + rs1 < a) << (i & 0x7);
                break;
            }
        case 32: {
                uint32_t a = ((uint32_t *)V(vs2))[i];
                V(vd)[i >> 3] |= (a + rs1 < a) << (i & 0x7);
                break;
            }
        case 64: {
                uint64_t a = ((uint64_t *)V(vs2))[i];
                V(vd)[i >> 3] |= (a + rs1 < a) << (i & 0x7);
                break;
            }
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vmadc_vim(CPUState *env, uint32_t vd, int32_t vs2, target_ulong rs1)
{
    if (V_IDX_INVALID(vs2)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        if (!(i & 0x7)) {
            V(vd)[i >> 3] = 0;
        }
        uint8_t carry = !!(V(0)[i >> 3] & (1 << (i & 0x7)));
        switch (eew) {
        case 8: {
                uint8_t a = ((uint8_t *)V(vs2))[i];
                uint64_t ab = a + rs1;
                V(vd)[i >> 3] |= (ab < a || (carry && !(ab + 1))) << (i & 0x7);
                break;
            }
        case 16: {
                uint16_t a = ((uint16_t *)V(vs2))[i];
                uint64_t ab = a + rs1;
                V(vd)[i >> 3] |= (ab < a || (carry && !(ab + 1))) << (i & 0x7);
                break;
            }
        case 32: {
                uint32_t a = ((uint32_t *)V(vs2))[i];
                uint32_t ab = a + rs1;
                V(vd)[i >> 3] |= (ab < a || (carry && !(ab + 1))) << (i & 0x7);
                break;
            }
        case 64: {
                uint64_t a = ((uint64_t *)V(vs2))[i];
                uint64_t ab = a + rs1;
                V(vd)[i >> 3] |= (ab < a || (carry && !(ab + 1))) << (i & 0x7);
                break;
            }
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vsbc_vi(CPUState *env, uint32_t vd, int32_t vs2, target_ulong rs1)
{
    if (V_IDX_INVALID(vd) || V_IDX_INVALID(vs2)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        uint8_t borrow = !!(V(0)[i >> 3] & (1 << (i & 0x7)));
        switch (eew) {
        case 8:
            ((uint8_t *)V(vd))[i] = ((uint8_t *)V(vs2))[i] - rs1 - borrow;
            break;
        case 16:
            ((uint16_t *)V(vd))[i] = ((uint16_t *)V(vs2))[i] - rs1 - borrow;
            break;
        case 32:
            ((uint32_t *)V(vd))[i] = ((uint32_t *)V(vs2))[i] - rs1 - borrow;
            break;
        case 64:
            ((uint64_t *)V(vd))[i] = ((uint64_t *)V(vs2))[i] - rs1 - borrow;
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vmsbc_vi(CPUState *env, uint32_t vd, int32_t vs2, target_ulong rs1)
{
    if (V_IDX_INVALID(vs2)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        if (!(i & 0x7)) {
            V(vd)[i >> 3] = 0;
        }
        switch (eew) {
        case 8:
            V(vd)[i >> 3] |= (((uint8_t *)V(vs2))[i] < rs1) << (i & 0x7);
            break;
        case 16:
            V(vd)[i >> 3] |= (((uint16_t *)V(vs2))[i] < rs1) << (i & 0x7);
            break;
        case 32:
            V(vd)[i >> 3] |= (((uint32_t *)V(vs2))[i] < rs1) << (i & 0x7);
            break;
        case 64:
            V(vd)[i >> 3] |= (((uint64_t *)V(vs2))[i] < rs1) << (i & 0x7);
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}

void helper_vmsbc_vim(CPUState *env, uint32_t vd, int32_t vs2, target_ulong rs1)
{
    if (V_IDX_INVALID(vs2)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
    const target_ulong eew = env->vsew;
    for (int i = 0; i < env->vl; ++i) {
        if (!(i & 0x7)) {
            V(vd)[i >> 3] = 0;
        }
        uint8_t borrow = !!(V(0)[i >> 3] & (1 << (i & 0x7)));
        switch (eew) {
        case 8:
            V(vd)[i >> 3] |= (((uint8_t *)V(vs2))[i] < rs1 || (borrow && !(rs1 + 1))) << (i & 0x7);
            break;
        case 16:
            V(vd)[i >> 3] |= (((uint16_t *)V(vs2))[i] < rs1 || (borrow && !(rs1 + 1))) << (i & 0x7);
            break;
        case 32:
            V(vd)[i >> 3] |= (((uint32_t *)V(vs2))[i] < rs1 || (borrow && !(rs1 + 1))) << (i & 0x7);
            break;
        case 64:
            V(vd)[i >> 3] |= (((uint64_t *)V(vs2))[i] < rs1 || (borrow && !(rs1 + 1))) << (i & 0x7);
            break;
        default:
            helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
            break;
        }
    }
}
