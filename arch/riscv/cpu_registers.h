#include "cpu-defs.h"

// REMARK: we use #ifdef/#endif, #ifdef/#endif instead of #ifdef/#else/#endif notation due to the limitation of `RegisterEnumParser.cs`
typedef enum {
#ifdef TARGET_RISCV64
    ZERO_64 = 0,
    X_0_64 = 0,
    RA_64 = 1,
    X_1_64 = 1,
    SP_64 = 2,
    X_2_64 = 2,
    GP_64 = 3,
    X_3_64 = 3,
    TP_64 = 4,
    X_4_64 = 4,
    T_0_64 = 5,
    X_5_64 = 5,
    T_1_64 = 6,
    X_6_64 = 6,
    T_2_64 = 7,
    X_7_64 = 7,
    FP_64 = 8,
    S_0_64 = 8,
    X_8_64 = 8,
    S_1_64 = 9,
    X_9_64 = 9,
    A_0_64 = 10,
    X_10_64 = 10,
    A_1_64 = 11,
    X_11_64 = 11,
    A_2_64 = 12,
    X_12_64 = 12,
    A_3_64 = 13,
    X_13_64 = 13,
    A_4_64 = 14,
    X_14_64 = 14,
    A_5_64 = 15,
    X_15_64 = 15,
    A_6_64 = 16,
    X_16_64 = 16,
    A_7_64 = 17,
    X_17_64 = 17,
    S_2_64 = 18,
    X_18_64 = 18,
    S_3_64 = 19,
    X_19_64 = 19,
    S_4_64 = 20,
    X_20_64 = 20,
    S_5_64 = 21,
    X_21_64 = 21,
    S_6_64 = 22,
    X_22_64 = 22,
    S_7_64 = 23,
    X_23_64 = 23,
    S_8_64 = 24,
    X_24_64 = 24,
    S_9_64 = 25,
    X_25_64 = 25,
    S_10_64 = 26,
    X_26_64 = 26,
    S_11_64 = 27,
    X_27_64 = 27,
    T_3_64 = 28,
    X_28_64 = 28,
    T_4_64 = 29,
    X_29_64 = 29,
    T_5_64 = 30,
    X_30_64 = 30,
    T_6_64 = 31,
    X_31_64 = 31,
    PC_64 = 32,
    SSTATUS_64 = 0x141,
    SIE_64 = 0x145,
    STVEC_64 = 0x146,
    SSCRATCH_64 = 0x181,
    SEPC_64 = 0x182,
    SCAUSE_64 = 0x183,
    STVAL_64 = 0x184,
    SIP_64 = 0x185,
    MSTATUS_64 = 0x341,
    MISA_64 = 0x342,
    MEDELEG_64 = 0x343,
    MIDELEG_64 = 0x344,
    MIE_64 = 0x345,
    MTVEC_64 = 0x346,
    MSCRATCH_64 = 0x381,
    MEPC_64 = 0x382,
    MCAUSE_64 = 0x383,
    MTVAL_64 = 0x384,
    MIP_64 = 0x385,
    PRIV_64 = 4161,
    F_0_64 = 33,
    F_1_64 = 34,
    F_2_64 = 35,
    F_3_64 = 36,
    F_4_64 = 37,
    F_5_64 = 38,
    F_6_64 = 39,
    F_7_64 = 40,
    F_8_64 = 41,
    F_9_64 = 42,
    F_10_64 = 43,
    F_11_64 = 44,
    F_12_64 = 45,
    F_13_64 = 46,
    F_14_64 = 47,
    F_15_64 = 48,
    F_16_64 = 49,
    F_17_64 = 50,
    F_18_64 = 51,
    F_19_64 = 52,
    F_20_64 = 53,
    F_21_64 = 54,
    F_22_64 = 55,
    F_23_64 = 56,
    F_24_64 = 57,
    F_25_64 = 58,
    F_26_64 = 59,
    F_27_64 = 60,
    F_28_64 = 61,
    F_29_64 = 62,
    F_30_64 = 63,
    F_31_64 = 64,
#endif
#ifdef TARGET_RISCV32
    ZERO_32 = 0,
    X_0_32 = 0,
    RA_32 = 1,
    X_1_32 = 1,
    SP_32 = 2,
    X_2_32 = 2,
    GP_32 = 3,
    X_3_32 = 3,
    TP_32 = 4,
    X_4_32 = 4,
    T_0_32 = 5,
    X_5_32 = 5,
    T_1_32 = 6,
    X_6_32 = 6,
    T_2_32 = 7,
    X_7_32 = 7,
    S_0_32 = 8,
    FP_32 = 8,
    X_8_32 = 8,
    S_1_32 = 9,
    X_9_32 = 9,
    A_0_32 = 10,
    X_10_32 = 10,
    A_1_32 = 11,
    X_11_32 = 11,
    A_2_32 = 12,
    X_12_32 = 12,
    A_3_32 = 13,
    X_13_32 = 13,
    A_4_32 = 14,
    X_14_32 = 14,
    A_5_32 = 15,
    X_15_32 = 15,
    A_6_32 = 16,
    X_16_32 = 16,
    A_7_32 = 17,
    X_17_32 = 17,
    S_2_32 = 18,
    X_18_32 = 18,
    S_3_32 = 19,
    X_19_32 = 19,
    S_4_32 = 20,
    X_20_32 = 20,
    S_5_32 = 21,
    X_21_32 = 21,
    S_6_32 = 22,
    X_22_32 = 22,
    S_7_32 = 23,
    X_23_32 = 23,
    S_8_32 = 24,
    X_24_32 = 24,
    S_9_32 = 25,
    X_25_32 = 25,
    S_10_32 = 26,
    X_26_32 = 26,
    S_11_32 = 27,
    X_27_32 = 27,
    T_3_32 = 28,
    X_28_32 = 28,
    T_4_32 = 29,
    X_29_32 = 29,
    T_5_32 = 30,
    X_30_32 = 30,
    T_6_32 = 31,
    X_31_32 = 31,
    PC_32 = 32,
    SSTATUS_32 = 0x141,
    SIE_32 = 0x145,
    STVEC_32 = 0x146,
    SSCRATCH_32 = 0x181,
    SEPC_32 = 0x182,
    SCAUSE_32 = 0x183,
    STVAL_32 = 0x184,
    SIP_32 = 0x185,
    MSTATUS_32 = 0x341,
    MISA_32 = 0x342,
    MEDELEG_32 = 0x343,
    MIDELEG_32 = 0x344,
    MIE_32 = 0x345,
    MTVEC_32 = 0x346,
    MSCRATCH_32 = 0x381,
    MEPC_32 = 0x382,
    MCAUSE_32 = 0x383,
    MTVAL_32 = 0x384,
    MIP_32 = 0x385,
    PRIV_32 = 4161,
    F_0_32 = 33,
    F_1_32 = 34,
    F_2_32 = 35,
    F_3_32 = 36,
    F_4_32 = 37,
    F_5_32 = 38,
    F_6_32 = 39,
    F_7_32 = 40,
    F_8_32 = 41,
    F_9_32 = 42,
    F_10_32 = 43,
    F_11_32 = 44,
    F_12_32 = 45,
    F_13_32 = 46,
    F_14_32 = 47,
    F_15_32 = 48,
    F_16_32 = 49,
    F_17_32 = 50,
    F_18_32 = 51,
    F_19_32 = 52,
    F_20_32 = 53,
    F_21_32 = 54,
    F_22_32 = 55,
    F_23_32 = 56,
    F_24_32 = 57,
    F_25_32 = 58,
    F_26_32 = 59,
    F_27_32 = 60,
    F_28_32 = 61,
    F_29_32 = 62,
    F_30_32 = 63,
    F_31_32 = 64,
#endif
} Registers;
