/* Internal to the AArch32 core: the decoded instruction form shared by the ARM and Thumb decoders and the executor. */
#pragma once
#include "a32.h"

/* Decoded operations. Field use per op is in the comment; unused fields are zero. */
typedef enum {
    OP_UNDEF = 0,
    OP_UNIMPL,
    OP_DP, /* sub = ARM data-processing opcode 0..15; rd, rn, operand2: k_imm -> imm | k_rsi -> rm,sh,sa | k_rsr -> rm,sh,ra(shift reg) */
    OP_MOVW,
    OP_MOVT,  /* rd, imm */
    OP_MUL,   /* sub 0 mul, 1 mla, 2 mls: rd, rn, rm, ra */
    OP_MULL,  /* sub 0 umull 1 umlal 2 smull 3 smlal 4 umaal: rd = RdLo, ra = RdHi, rn, rm */
    OP_SMULH, /* halfword multiplies: sub 0 smla<xy> 1 smulw<y> / smlaw<y> (imm2 bit0 = acc) 2 smlal<xy> 3 smul<xy>; imm = x | y << 1; rd, rn, rm, ra */
    OP_SMMUL, /* sub 0 smmul/smmla 1 smmls; imm = round; ra == 15 -> no accumulate */
    OP_SMUAD, /* sub 0 smuad/smlad 1 smusd/smlsd 2 smlald 3 smlsld; imm = swap(X); ra == 15 -> no accumulate; for 2,3: rd = lo, ra = hi */
    OP_DIV,   /* sub 0 sdiv 1 udiv */
    OP_CLZ,
    OP_RBIT,
    OP_REV,
    OP_REV16,
    OP_REVSH,
    OP_SEL,
    OP_USAD8,  /* ra == 15 -> usad8 */
    OP_EXT,    /* sub: 0 sxtb 1 sxth 2 uxtb 3 uxth 4 sxtb16 5 uxtb16; rd, rm, rn (15 = no add), sa = rotate (0,8,16,24) */
    OP_BFX,    /* sub 0 ubfx 1 sbfx: rd, rn, imm = lsb, imm2 = width */
    OP_BFI,    /* rd, rn (15 = bfc), imm = lsb, imm2 = width */
    OP_SAT,    /* sub 0 ssat 1 usat 2 ssat16 3 usat16; rd, rn, imm = sat position (bit count for ssat, bit index for usat), sh, sa */
    OP_PKH,    /* sub 0 pkhbt 1 pkhtb: rd, rn, rm, sa */
    OP_QARITH, /* sub 0 qadd 1 qsub 2 qdadd 3 qdsub: rd, rm (value), rn (other) */
    OP_PAR,    /* parallel add/sub: sub = prefix * 8 + op; prefix 0 s 1 q 2 sh 3 u 4 uq 5 uh; op 0 add16 1 asx 2 sax 3 sub16 4 add8 7 sub8; rd, rn, rm */
    OP_LDST,   /* sub 0 str 1 ldr 2 strb 3 ldrb 4 strh 5 ldrh 6 ldrsb 7 ldrsh 8 strd 9 ldrd; rd (rt), ra (rt2 for 8/9), rn, offset imm or (rm,sh,sa) if FL_REG;
                       FL_P pre-index/offset, FL_U add, FL_W writeback (for post-index the decoder sets FL_W, FL_P clear), FL_ACQ acquire/release (ordering only) */
    OP_LDM,    /* rn, imm = register list, FL_P before, FL_U increment, FL_W writeback, sub 1 = load */
    OP_LDREX,  /* sub = size 0 byte 1 half 2 word 3 dword; rd (rt), ra (rt2), rn */
    OP_STREX,  /* sub = size; rd = status, rm = rt, ra = rt2, rn */
    OP_CLREX,
    OP_B,       /* imm = byte offset from the pc operand (pc + 8 / + 4) */
    OP_BL,      /* same, writes lr */
    OP_BLX_IMM, /* same, switches state */
    OP_BX,      /* rm */
    OP_BLX_REG, /* rm */
    OP_CBZ,     /* sub 0 cbz 1 cbnz; rn, imm */
    OP_TB,      /* sub 0 tbb 1 tbh; rn, rm */
    OP_IT,      /* imm = firstcond << 4 | mask */
    OP_SVC,     /* imm */
    OP_BKPT,    /* imm */
    OP_NOP,
    OP_MRS, /* rd */
    OP_MSR, /* imm = mask (bit 3 = nzcvq, bit 2 = ge), rm or imm2 with FL_IMM */
    OP_MRC, /* rd = rt, imm = opc1 << 16 | crn << 12 | crm << 8 | opc2 << 4 | cp */
    OP_MCR,
    OP_MRRC, /* rd = rt, ra = rt2, imm as above (opc1 in bits 7:4, crm in 11:8) */
    OP_VFP,  /* VFP / NEON / coprocessor 10/11: imm = the instruction as an A32 word (Thumb-2 forms are converted to it) */
    OP_NEON,
    OP_COUNT
} A32Op;

enum { FL_S = 1, FL_SIT = 2, FL_P = 4, FL_U = 8, FL_W = 16, FL_REG = 32, FL_ICARRY = 64, FL_ACQ = 128 };
/* operand2 kind of OP_DP, in `k` */
enum { K_IMM = 0, K_RSI = 1, K_RSR = 2 };

typedef struct {
    uint16_t op;
    uint8_t cond, len; /* cond 0..14, 15 = always (Thumb: the IT state decides at run time); len 2 or 4 */
    uint8_t rd, rn, rm, ra;
    uint8_t sh, sa, sub, fl;
    uint8_t k, pad[3];
    uint32_t imm, imm2;
} A32Insn;

/* Decoders: fill *out for the instruction whose first word / halfwords are given. Always set a valid op (OP_UNDEF / OP_UNIMPL included). */
void a32_decode_arm(uint32_t w, A32Insn *out);
/* hw1 is the first halfword, hw2 the next one (only used when the first says 32 bit). Returns the instruction length in bytes. */
unsigned a32_decode_thumb(uint16_t hw1, uint16_t hw2, A32Insn *out);
int a32_thumb_is32(uint16_t hw1);

/* VFP / NEON executor (a32_vfp.c, a32_neon.c). `w` is the ARM-form encoding. Returns A32_EXIT_BUDGET for "continue" or an exit reason. */
A32Exit a32_exec_vfp(A32Cpu *c, uint32_t w, uint32_t pc_operand);
A32Exit a32_exec_neon(A32Cpu *c, uint32_t w);
