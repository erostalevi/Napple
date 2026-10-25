/* ARM (A32) instruction decoder: 32-bit word -> A32Insn. Encodings follow the ARMv8-A AArch32 architecture reference. */
#include "a32_priv.h"
#include <string.h>

#define BITS(w, hi, lo) (((w) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))
#define BIT(w, b) (((w) >> (b)) & 1u)

static uint32_t sext(uint32_t v, unsigned bits) { return (uint32_t)((int32_t)(v << (32 - bits)) >> (32 - bits)); }

static void set_dp_imm(A32Insn *o, uint32_t w) {
    uint32_t imm8 = w & 0xff, rot = BITS(w, 11, 8) * 2;
    o->imm = rot ? (imm8 >> rot) | (imm8 << (32 - rot)) : imm8;
    o->k = K_IMM; if (rot) o->fl |= FL_ICARRY;
}

static void arm_dp(uint32_t w, A32Insn *o, int imm_form) {
    o->op = OP_DP; o->sub = BITS(w, 24, 21); o->rn = BITS(w, 19, 16); o->rd = BITS(w, 15, 12);
    if (BIT(w, 20)) o->fl |= FL_S;
    if (imm_form) { set_dp_imm(o, w); return; }
    o->rm = w & 15; o->sh = BITS(w, 6, 5);
    if (BIT(w, 4)) { o->k = K_RSR; o->ra = BITS(w, 11, 8); }
    else { o->k = K_RSI; o->sa = BITS(w, 11, 7); }
}

static void arm_misc(uint32_t w, A32Insn *o) {   /* 0001 0xx0 with bit 7 = 0 (bit 4 = 0 or not a reg shift) */
    unsigned op2 = BITS(w, 6, 4), op = BITS(w, 22, 21);
    if (BIT(w, 7)) {   /* halfword multiplies */
        unsigned x = BIT(w, 5), y = BIT(w, 6);
        o->rd = BITS(w, 19, 16); o->ra = BITS(w, 15, 12); o->rm = BITS(w, 11, 8); o->rn = w & 15;
        o->op = OP_SMULH; o->imm = x | y << 1;
        switch (op) {
        case 0: o->sub = 0; break;                                  /* smla<x><y> */
        case 1: o->sub = 1; o->imm = y << 1 | 0; o->imm2 = x ? 0 : 1; break;   /* x=0 smlaw<y> (acc), x=1 smulw<y> */
        case 2: o->sub = 2; o->rd = BITS(w, 15, 12); o->ra = BITS(w, 19, 16); break;   /* smlal<x><y>: RdLo = 15:12, RdHi = 19:16 */
        default: o->sub = 3; break;                                 /* smul<x><y> */
        }
        return;
    }
    switch (op2) {
    case 0:
        if (op & 1) { /* MSR register */
            if (BIT(w, 22)) { o->op = OP_UNDEF; return; }          /* spsr */
            o->op = OP_MSR; o->imm = BITS(w, 19, 16); o->rm = w & 15;
        } else {
            if (BIT(w, 22)) { o->op = OP_UNDEF; return; }
            o->op = OP_MRS; o->rd = BITS(w, 15, 12);
        }
        return;
    case 1:
        if (op == 1) { o->op = OP_BX; o->rm = w & 15; }
        else if (op == 3) { o->op = OP_CLZ; o->rd = BITS(w, 15, 12); o->rm = w & 15; }
        else o->op = OP_UNDEF;
        return;
    case 2: if (op == 1) { o->op = OP_BX; o->rm = w & 15; } else o->op = OP_UNDEF; return;   /* bxj behaves as bx */
    case 3: if (op == 1) { o->op = OP_BLX_REG; o->rm = w & 15; } else o->op = OP_UNDEF; return;
    case 5: o->op = OP_QARITH; o->sub = op; o->rd = BITS(w, 15, 12); o->rn = BITS(w, 19, 16); o->rm = w & 15; return;
    case 6: o->op = OP_UNIMPL; return;   /* eret */
    case 7:
        if (op == 1) { o->op = OP_BKPT; o->imm = BITS(w, 19, 8) << 4 | (w & 15); return; }
        o->op = OP_UNIMPL; return;      /* hvc, smc */
    default: o->op = OP_UNDEF; return;
    }
}

static void arm_mul(uint32_t w, A32Insn *o) {   /* bits 27:24 = 0000, 7:4 = 1001 */
    unsigned op = BITS(w, 23, 20);
    o->rm = BITS(w, 11, 8); o->rn = w & 15;
    switch (op >> 1) {
    case 0: o->op = OP_MUL; o->sub = 0; o->rd = BITS(w, 19, 16); o->ra = BITS(w, 15, 12); break;
    case 1: o->op = OP_MUL; o->sub = 1; o->rd = BITS(w, 19, 16); o->ra = BITS(w, 15, 12); break;
    case 2: if (op & 1) { o->op = OP_UNDEF; return; } o->op = OP_MULL; o->sub = 4; o->rd = BITS(w, 15, 12); o->ra = BITS(w, 19, 16); return;   /* umaal */
    case 3: if (op & 1) { o->op = OP_UNDEF; return; } o->op = OP_MUL; o->sub = 2; o->rd = BITS(w, 19, 16); o->ra = BITS(w, 15, 12); return;     /* mls */
    default:
        o->op = OP_MULL; o->sub = (op >> 1) - 4;   /* 0 umull 1 umlal 2 smull 3 smlal */
        o->rd = BITS(w, 15, 12); o->ra = BITS(w, 19, 16); break;
    }
    if (op & 1) o->fl |= FL_S;
}

static void arm_sync(uint32_t w, A32Insn *o) {   /* 0001 xxxx ... 1001 */
    unsigned op = BITS(w, 23, 20);
    o->rn = BITS(w, 19, 16);
    if (!BIT(w, 23)) {   /* swp / swpb */
        if (op & 3) { o->op = OP_UNDEF; return; }
        o->op = OP_UNIMPL; return;
    }
    unsigned size = BITS(w, 22, 21);             /* 00 word, 01 dword, 10 byte, 11 half */
    unsigned s = size == 0 ? 2 : size == 1 ? 3 : size == 2 ? 0 : 1;
    unsigned kind = BITS(w, 11, 8);              /* 1111 exclusive, 1110 acquire/release exclusive, 1100 acquire/release */
    int load = BIT(w, 20);
    o->sub = s;
    if (kind == 0xf || kind == 0xe) {
        if (load) { o->op = OP_LDREX; o->rd = BITS(w, 15, 12); o->ra = o->rd + 1; if (kind == 0xe) o->fl |= FL_ACQ; }
        else { o->op = OP_STREX; o->rd = BITS(w, 15, 12); o->rm = w & 15; o->ra = o->rm + 1; if (kind == 0xe) o->fl |= FL_ACQ; }
        return;
    }
    if (kind == 0xc) {   /* lda / stl: plain accesses with ordering */
        if (s == 3) { o->op = OP_UNDEF; return; }
        o->op = OP_LDST; o->fl |= FL_P | FL_U | FL_ACQ;
        static const uint8_t ld[3] = {3, 5, 1}, st[3] = {2, 4, 0};   /* byte half word */
        if (load) { o->sub = ld[s]; o->rd = BITS(w, 15, 12); }
        else { o->sub = st[s]; o->rd = w & 15; }
        o->imm = 0; return;
    }
    o->op = OP_UNDEF;
}

static void arm_extra_ldst(uint32_t w, A32Insn *o) {   /* 000 P U I W L ... 1SH1 */
    unsigned sh = BITS(w, 6, 5), l = BIT(w, 20), p = BIT(w, 24), wb = BIT(w, 21);
    o->op = OP_LDST; o->rn = BITS(w, 19, 16); o->rd = BITS(w, 15, 12);
    if (p) o->fl |= FL_P;
    if (BIT(w, 23)) o->fl |= FL_U;
    if (!p) o->fl |= FL_W; else if (wb) o->fl |= FL_W;
    if (BIT(w, 22)) o->imm = BITS(w, 11, 8) << 4 | (w & 15);
    else { if (BITS(w, 11, 8)) { o->op = OP_UNDEF; return; } o->fl |= FL_REG; o->rm = w & 15; o->sh = 0; o->sa = 0; }
    switch (sh) {
    case 1: o->sub = l ? 5 : 4; break;
    case 2: if (l) o->sub = 6; else { o->sub = 9; o->ra = o->rd + 1; if (o->rd & 1) o->op = OP_UNDEF; } break;   /* ldrsb | ldrd */
    case 3: if (l) o->sub = 7; else { o->sub = 8; o->ra = o->rd + 1; if (o->rd & 1) o->op = OP_UNDEF; } break;   /* ldrsh | strd */
    }
    if ((sh == 2 || sh == 3) && !l) {
        /* ldrd/strd: for ldrd (sh == 2, l == 0) store is "strd" when sh == 3 and l == 0: ARM: L=0,SH=10 -> LDRD; L=0,SH=11 -> STRD */
        o->sub = sh == 2 ? 9 : 8;
    }
}

static void arm_ldst(uint32_t w, A32Insn *o, int reg) {
    unsigned p = BIT(w, 24), b = BIT(w, 22), wb = BIT(w, 21), l = BIT(w, 20);
    o->op = OP_LDST; o->rn = BITS(w, 19, 16); o->rd = BITS(w, 15, 12);
    o->sub = (b ? 2 : 0) + (l ? 1 : 0);
    if (p) o->fl |= FL_P;
    if (BIT(w, 23)) o->fl |= FL_U;
    if (!p) o->fl |= FL_W; else if (wb) o->fl |= FL_W;
    if (reg) { o->fl |= FL_REG; o->rm = w & 15; o->sh = BITS(w, 6, 5); o->sa = BITS(w, 11, 7); }
    else o->imm = w & 0xfff;
}

static void arm_media(uint32_t w, A32Insn *o) {   /* 011x xxxx ... xxx1 */
    unsigned op1 = BITS(w, 24, 20), op2 = BITS(w, 7, 5);
    unsigned rd = BITS(w, 15, 12), rn = BITS(w, 19, 16), rm = w & 15;
    if (BITS(w, 24, 23) == 0) {   /* parallel add / subtract */
        unsigned pre = BITS(w, 22, 20);
        static const int8_t map[8] = {-1, 0, 1, 2, -1, 3, 4, 5};
        if (map[pre] < 0 || op2 == 5 || op2 == 6) { o->op = OP_UNDEF; return; }
        o->op = OP_PAR; o->sub = map[pre] * 8 + op2; o->rd = rd; o->rn = rn; o->rm = rm; return;
    }
    if (BITS(w, 24, 23) == 1) {   /* pack, saturate, reverse, extend, select */
        if ((w & 0x30) == 0x10) {                 /* pkh, ssat, usat: bits 5:4 = 01 (bit 6 is the shift type / tb) */
            if (BITS(w, 22, 20) == 0) { o->op = OP_PKH; o->sub = BIT(w, 6); o->rd = rd; o->rn = rn; o->rm = rm; o->sa = BITS(w, 11, 7); return; }
            /* ssat / usat: 0110 101x / 0110 111x with bits 6:4 = sh01 */
            if (BITS(w, 22, 21) == 1 || BITS(w, 22, 21) == 3) {
                o->op = OP_SAT; o->sub = BIT(w, 22); o->rd = rd; o->rn = rm; o->sh = BIT(w, 6) ? 2 : 0; o->sa = BITS(w, 11, 7);
                o->imm = o->sub ? BITS(w, 20, 16) : BITS(w, 20, 16) + 1;
                return;
            }
            o->op = OP_UNDEF; return;
        }
        if ((w & 0xf0) == 0xb0 && BITS(w, 22, 20) == 0 && BITS(w, 11, 8) == 0xf) { o->op = OP_SEL; o->rd = rd; o->rn = rn; o->rm = rm; return; }
        if ((w & 0xf0) == 0x30 && BITS(w, 11, 8) == 0xf) {   /* ssat16 / usat16 / rev / rbit */
            switch (BITS(w, 22, 20)) {
            case 2: o->op = OP_SAT; o->sub = 2; o->rd = rd; o->rn = rm; o->imm = BITS(w, 19, 16) + 1; return;
            case 6: o->op = OP_SAT; o->sub = 3; o->rd = rd; o->rn = rm; o->imm = BITS(w, 19, 16); return;
            case 3: if (rn == 15) { o->op = OP_REV; o->rd = rd; o->rm = rm; return; } break;
            case 7: if (rn == 15) { o->op = OP_RBIT; o->rd = rd; o->rm = rm; return; } break;
            }
            o->op = OP_UNDEF; return;
        }
        if ((w & 0xf0) == 0xb0 && BITS(w, 11, 8) == 0xf && rn == 15) {
            if (BITS(w, 22, 20) == 3) { o->op = OP_REV16; o->rd = rd; o->rm = rm; return; }
            if (BITS(w, 22, 20) == 7) { o->op = OP_REVSH; o->rd = rd; o->rm = rm; return; }
        }
        if ((w & 0xf0) == 0x70 && BITS(w, 9, 8) == 0) {   /* extend */
            static const int8_t kind[8] = {4, -1, 0, 1, 5, -1, 2, 3};
            int k = kind[BITS(w, 22, 20)];
            if (k < 0) { o->op = OP_UNDEF; return; }
            o->op = OP_EXT; o->sub = k; o->rd = rd; o->rn = rn; o->rm = rm; o->sa = BITS(w, 11, 10) * 8; return;
        }
        o->op = OP_UNDEF; return;
    }
    if (BITS(w, 24, 23) == 2) {   /* multiplies, divide, usad8 */
        unsigned rdd = BITS(w, 19, 16), ra = BITS(w, 15, 12), rmm = BITS(w, 11, 8), rnn = w & 15;
        if (!BIT(w, 4)) { o->op = OP_UNDEF; return; }
        if (op1 == 0x10 || op1 == 0x14) {   /* smuad/smlad/smusd/smlsd (0x10) ; smlald/smlsld (0x14) */
            if (BIT(w, 7)) { o->op = OP_UNDEF; return; }
            unsigned sel = BITS(w, 7, 5);
            if (op1 == 0x10) {
                if (sel & 4) { o->op = OP_UNDEF; return; }
                o->op = OP_SMUAD; o->sub = (sel >> 1) & 1; o->imm = sel & 1; o->rd = rdd; o->ra = ra; o->rn = rnn; o->rm = rmm;
                if (sel >> 1 > 1) o->op = OP_UNDEF;
            } else {
                if ((sel >> 1) > 1) { o->op = OP_UNDEF; return; }
                o->op = OP_SMUAD; o->sub = 2 + ((sel >> 1) & 1); o->imm = sel & 1; o->rd = ra; o->ra = rdd; o->rn = rnn; o->rm = rmm;
            }
            return;
        }
        if (op1 == 0x11 && BITS(w, 7, 5) == 0) { o->op = OP_DIV; o->sub = 0; o->rd = rdd; o->rn = rnn; o->rm = rmm; if (ra != 15) o->op = OP_UNDEF; return; }
        if (op1 == 0x13 && BITS(w, 7, 5) == 0) { o->op = OP_DIV; o->sub = 1; o->rd = rdd; o->rn = rnn; o->rm = rmm; if (ra != 15) o->op = OP_UNDEF; return; }
        if (op1 == 0x15) {   /* smmla/smmul/smmls */
            unsigned sel = BITS(w, 7, 5);
            if (sel == 0 || sel == 1) { o->op = OP_SMMUL; o->sub = 0; o->imm = sel & 1; }
            else if (sel == 6 || sel == 7) { o->op = OP_SMMUL; o->sub = 1; o->imm = sel & 1; }
            else { o->op = OP_UNDEF; return; }
            o->rd = rdd; o->ra = ra; o->rn = rnn; o->rm = rmm; return;
        }
        if (op1 == 0x18 && BITS(w, 7, 5) == 0) { o->op = OP_USAD8; o->rd = rdd; o->ra = ra; o->rn = rnn; o->rm = rmm; return; }
        o->op = OP_UNDEF; return;
    }
    /* BITS(24,23) == 3: bitfield */
    if ((op1 & 0x1e) == 0x1c && BITS(w, 6, 4) == 1) {   /* bfc / bfi */
        unsigned lsb = BITS(w, 11, 7), msb = BITS(w, 20, 16);
        if (msb < lsb) { o->op = OP_UNDEF; return; }
        o->op = OP_BFI; o->rd = rd; o->rn = rm; o->imm = lsb; o->imm2 = msb - lsb + 1;
        return;
    }
    if (((op1 & 0x1e) == 0x1a || (op1 & 0x1e) == 0x1e) && BITS(w, 6, 4) == 5) {   /* sbfx 1101x / ubfx 1111x (bit 20 is the top bit of widthm1) */
        o->op = OP_BFX; o->sub = BIT(w, 22) ? 0 : 1; o->rd = rd; o->rn = rm; o->imm = BITS(w, 11, 7); o->imm2 = BITS(w, 20, 16) + 1;
        if (o->imm + o->imm2 > 32) o->op = OP_UNDEF;
        return;
    }
    o->op = OP_UNDEF;
}

static void arm_uncond(uint32_t w, A32Insn *o) {
    if ((w & 0xfe000000) == 0xfa000000) {   /* blx imm */
        o->op = OP_BLX_IMM; o->imm = (sext(w & 0xffffff, 24) << 2) | (BIT(w, 24) << 1); return;
    }
    if ((w & 0xfffffff0) == 0xf57ff010) { o->op = OP_CLREX; return; }
    if ((w & 0xffffff00) == 0xf57ff000) {
        unsigned k = BITS(w, 7, 4);
        if (k == 4 || k == 5 || k == 6) { o->op = OP_NOP; return; }   /* dsb dmb isb */
        o->op = OP_UNDEF; return;
    }
    if ((w & 0xfc000000) == 0xf4000000) {
        if (BIT(w, 24)) { o->op = OP_NOP; return; }   /* pld / pldw / pli: hints */
        o->op = OP_NEON; o->imm = w; return;           /* NEON element / structure load-store */
    }
    if ((w & 0xfe000000) == 0xf2000000) { o->op = OP_NEON; o->imm = w; return; }
    if ((w & 0xff000000) == 0xfe000000 || (w & 0xfe000000) == 0xfc000000) {   /* v8 unconditional VFP: vsel, vmaxnm, vrint, vcvt<r>; mcr2/mrc2 */
        o->op = OP_VFP; o->imm = w; return;
    }
    o->op = OP_UNDEF;
}

void a32_decode_arm(uint32_t w, A32Insn *o) {
    memset(o, 0, sizeof *o);
    o->len = 4; o->cond = BITS(w, 31, 28);
    if (o->cond == 15) { o->cond = 15; arm_uncond(w, o); return; }
    unsigned top = BITS(w, 27, 25);
    switch (top) {
    case 0: {
        if (BIT(w, 4) && BIT(w, 7)) {
            if (BITS(w, 6, 5) == 0) {
                if (!BIT(w, 24)) arm_mul(w, o);
                else arm_sync(w, o);
                return;
            }
            arm_extra_ldst(w, o); return;
        }
        unsigned opc = BITS(w, 24, 21);
        if (opc >= 8 && opc <= 11 && !BIT(w, 20)) { arm_misc(w, o); return; }
        arm_dp(w, o, 0); return;
    }
    case 1: {
        unsigned opc = BITS(w, 24, 21);
        if (opc >= 8 && opc <= 11 && !BIT(w, 20)) {
            if (opc == 8) { o->op = OP_MOVW; o->rd = BITS(w, 15, 12); o->imm = BITS(w, 19, 16) << 12 | (w & 0xfff); return; }
            if (opc == 10) { o->op = OP_MOVT; o->rd = BITS(w, 15, 12); o->imm = BITS(w, 19, 16) << 12 | (w & 0xfff); return; }
            /* msr immediate / hints */
            if (opc == 9) {
                if (BIT(w, 22) == 0 && BITS(w, 19, 18) == 0 && BITS(w, 15, 12) == 0xf) {
                    unsigned mask = BITS(w, 19, 16);
                    if (mask == 0 && BITS(w, 7, 0) <= 0x14 ) { o->op = OP_NOP; return; }   /* nop / yield / wfe / wfi / sev / sevl / csdb */
                    if (mask == 0) { o->op = OP_NOP; return; }
                }
                if (!BIT(w, 22) && BITS(w, 15, 12) == 0xf) {
                    uint32_t imm8 = w & 0xff, rot = BITS(w, 11, 8) * 2;
                    o->op = OP_MSR; o->imm = BITS(w, 19, 16); o->fl |= FL_ICARRY | FL_REG;
                    o->imm2 = rot ? (imm8 >> rot) | (imm8 << (32 - rot)) : imm8; return;
                }
            }
            o->op = OP_UNDEF; return;
        }
        arm_dp(w, o, 1); return;
    }
    case 2: arm_ldst(w, o, 0); return;
    case 3:
        if (BIT(w, 4)) { arm_media(w, o); return; }
        arm_ldst(w, o, 1); return;
    case 4: {
        o->op = OP_LDM; o->rn = BITS(w, 19, 16); o->imm = w & 0xffff; o->sub = BIT(w, 20);
        if (BIT(w, 24)) o->fl |= FL_P;
        if (BIT(w, 23)) o->fl |= FL_U;
        if (BIT(w, 21)) o->fl |= FL_W;
        if (BIT(w, 22)) o->op = OP_UNIMPL;   /* user-mode register transfer / exception return: privileged */
        return;
    }
    case 5:
        o->op = BIT(w, 24) ? OP_BL : OP_B; o->imm = sext(w & 0xffffff, 24) << 2; return;
    case 6: case 7: {
        if (top == 7 && BIT(w, 24)) { o->op = OP_SVC; o->imm = w & 0xffffff; return; }
        unsigned cp = BITS(w, 11, 8);
        if (cp == 10 || cp == 11) { o->op = OP_VFP; o->imm = w; return; }
        if (top == 7 && BIT(w, 4)) {   /* mcr / mrc */
            o->op = BIT(w, 20) ? OP_MRC : OP_MCR; o->rd = BITS(w, 15, 12);
            o->imm = BITS(w, 23, 21) << 16 | BITS(w, 19, 16) << 12 | (w & 15) << 8 | BITS(w, 7, 5) << 4 | cp; return;
        }
        if (top == 6 && (w & 0x0fe00000) == 0x0c400000) {   /* mcrr / mrrc */
            o->op = BIT(w, 20) ? OP_MRRC : OP_UNIMPL; o->rd = BITS(w, 15, 12); o->ra = BITS(w, 19, 16);
            o->imm = BITS(w, 7, 4) << 4 | (w & 15) << 8 | cp; return;
        }
        o->op = OP_UNIMPL; return;
    }
    }
}
