/* Thumb / Thumb-2 instruction decoder: halfwords -> A32Insn (same form as the ARM decoder produces). */
#include "a32_priv.h"
#include <string.h>

#define BITS(w, hi, lo) (((w) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))
#define BIT(w, b) (((w) >> (b)) & 1u)
#define CC_ALWAYS 15 /* the IT state (or nothing) decides */

static uint32_t sext(uint32_t v, unsigned bits) { return (uint32_t)((int32_t)(v << (32 - bits)) >> (32 - bits)); }

int a32_thumb_is32(uint16_t hw1) {
    unsigned t = hw1 >> 11;
    return t == 0x1d || t == 0x1e || t == 0x1f;
}

/* ---- 16-bit ---------------------------------------------------------------------------------------------------- */
static void dp_imm(A32Insn *o, unsigned sub, unsigned rd, unsigned rn, uint32_t imm, int flags_if_not_it) {
    o->op = OP_DP;
    o->sub = sub;
    o->rd = rd;
    o->rn = rn;
    o->imm = imm;
    o->k = K_IMM;
    if (flags_if_not_it) o->fl |= FL_SIT;
}
static void dp_rsi(A32Insn *o, unsigned sub, unsigned rd, unsigned rn, unsigned rm, unsigned sh, unsigned sa, int flags_if_not_it) {
    o->op = OP_DP;
    o->sub = sub;
    o->rd = rd;
    o->rn = rn;
    o->rm = rm;
    o->sh = sh;
    o->sa = sa;
    o->k = K_RSI;
    if (flags_if_not_it) o->fl |= FL_SIT;
}

static void thumb16(uint16_t h, A32Insn *o) {
    unsigned t5 = h >> 11;
    o->len = 2;
    o->cond = CC_ALWAYS;
    switch (t5) {
        case 0:
        case 1:
        case 2: /* lsl / lsr / asr (immediate): movs */ dp_rsi(o, 13, h & 7, 0, BITS(h, 5, 3), t5, BITS(h, 10, 6), 1); return;
        case 3: { /* add / sub (register or imm3) */
            unsigned sub = BIT(h, 9) ? 2 : 4, rd = h & 7, rn = BITS(h, 5, 3), v = BITS(h, 8, 6);
            if (BIT(h, 10)) dp_imm(o, sub, rd, rn, v, 1);
            else dp_rsi(o, sub, rd, rn, v, 0, 0, 1);
            return;
        }
        case 4:
        case 5:
        case 6:
        case 7: { /* mov / cmp / add / sub with imm8 */
            static const uint8_t map[4] = {13, 10, 4, 2};
            unsigned op = t5 - 4, rd = BITS(h, 10, 8);
            dp_imm(o, map[op], rd, rd, h & 0xff, 1);
            if (op == 1) {
                o->fl &= ~FL_SIT;
                o->fl |= FL_S;
            }
            return;
        }
        case 8:
            if (!BIT(h, 10)) { /* data processing (register) */
                unsigned op = BITS(h, 9, 6), rd = h & 7, rm = BITS(h, 5, 3);
                switch (op) {
                    case 0: dp_rsi(o, 0, rd, rd, rm, 0, 0, 1); break; /* ands */
                    case 1: dp_rsi(o, 1, rd, rd, rm, 0, 0, 1); break; /* eors */
                    case 2:
                    case 3:
                    case 4:
                    case 7: /* lsls lsrs asrs rors (register) */
                        o->op = OP_DP;
                        o->sub = 13;
                        o->rd = rd;
                        o->rm = rd;
                        o->ra = rm;
                        o->k = K_RSR;
                        o->fl |= FL_SIT;
                        o->sh = op == 2 ? 0 : op == 3 ? 1 : op == 4 ? 2 : 3;
                        break;
                    case 5: dp_rsi(o, 5, rd, rd, rm, 0, 0, 1); break; /* adcs */
                    case 6: dp_rsi(o, 6, rd, rd, rm, 0, 0, 1); break; /* sbcs */
                    case 8:
                        dp_rsi(o, 8, 0, rd, rm, 0, 0, 0);
                        o->fl |= FL_S;
                        break;                                 /* tst */
                    case 9: dp_imm(o, 3, rd, rm, 0, 1); break; /* rsbs rd, rm, #0 */
                    case 10:
                        dp_rsi(o, 10, 0, rd, rm, 0, 0, 0);
                        o->fl |= FL_S;
                        break; /* cmp */
                    case 11:
                        dp_rsi(o, 11, 0, rd, rm, 0, 0, 0);
                        o->fl |= FL_S;
                        break;                                          /* cmn */
                    case 12: dp_rsi(o, 12, rd, rd, rm, 0, 0, 1); break; /* orrs */
                    case 13:
                        o->op = OP_MUL;
                        o->sub = 0;
                        o->rd = rd;
                        o->rn = rm;
                        o->rm = rd;
                        o->fl |= FL_SIT;
                        break;                                          /* muls rd, rm, rd */
                    case 14: dp_rsi(o, 14, rd, rd, rm, 0, 0, 1); break; /* bics */
                    default: dp_rsi(o, 15, rd, 0, rm, 0, 0, 1); break;  /* mvns */
                }
                return;
            } else { /* special data / branch exchange */
                unsigned op = BITS(h, 9, 8), rm = BITS(h, 6, 3), rdn = (BIT(h, 7) << 3) | (h & 7);
                switch (op) {
                    case 0: dp_rsi(o, 4, rdn, rdn, rm, 0, 0, 0); break; /* add */
                    case 1:
                        dp_rsi(o, 10, 0, rdn, rm, 0, 0, 0);
                        o->fl |= FL_S;
                        break;                                         /* cmp */
                    case 2: dp_rsi(o, 13, rdn, 0, rm, 0, 0, 0); break; /* mov */
                    default:
                        o->op = BIT(h, 7) ? OP_BLX_REG : OP_BX;
                        o->rm = rm;
                        break;
                }
                return;
            }
        case 9: /* ldr (literal) */
            o->op = OP_LDST;
            o->sub = 1;
            o->rd = BITS(h, 10, 8);
            o->rn = 15;
            o->imm = (h & 0xff) << 2;
            o->fl = FL_P | FL_U;
            return;
        case 10:
        case 11: { /* load / store (register offset) */
            static const uint8_t sub[8] = {0, 4, 2, 6, 1, 5, 3, 7};
            o->op = OP_LDST;
            o->sub = sub[BITS(h, 11, 9)];
            o->rd = h & 7;
            o->rn = BITS(h, 5, 3);
            o->rm = BITS(h, 8, 6);
            o->fl = FL_P | FL_U | FL_REG;
            return;
        }
    }
    o->op = OP_UNDEF;
}

/* The remaining 16-bit groups (kept out of the switch above so each group reads on its own). */
static void thumb16_b(uint16_t h, A32Insn *o) {
    unsigned t5 = BITS(h, 15, 11);
    o->len = 2;
    o->cond = CC_ALWAYS;
    switch (t5) {
        case 0x0c:
        case 0x0d:
        case 0x0e:
        case 0x0f: { /* 011 B L imm5: str/ldr/strb/ldrb */
            static const uint8_t sub[4] = {0, 1, 2, 3};
            o->op = OP_LDST;
            o->sub = sub[t5 - 0x0c];
            o->rd = h & 7;
            o->rn = BITS(h, 5, 3);
            o->fl = FL_P | FL_U;
            o->imm = BITS(h, 10, 6) << (t5 < 0x0e ? 2 : 0);
            return;
        }
        case 0x10:
        case 0x11: /* strh / ldrh imm5 */
            o->op = OP_LDST;
            o->sub = t5 == 0x10 ? 4 : 5;
            o->rd = h & 7;
            o->rn = BITS(h, 5, 3);
            o->imm = BITS(h, 10, 6) << 1;
            o->fl = FL_P | FL_U;
            return;
        case 0x12:
        case 0x13: /* sp-relative str / ldr */
            o->op = OP_LDST;
            o->sub = t5 == 0x12 ? 0 : 1;
            o->rd = BITS(h, 10, 8);
            o->rn = 13;
            o->imm = (h & 0xff) << 2;
            o->fl = FL_P | FL_U;
            return;
        case 0x14:
        case 0x15: /* adr / add rd, sp, #imm */ dp_imm(o, 4, BITS(h, 10, 8), t5 == 0x14 ? 15 : 13, (h & 0xff) << 2, 0); return;
        case 0x16:
        case 0x17: { /* misc */
            if ((h & 0xff00) == 0xb000) {
                dp_imm(o, BIT(h, 7) ? 2 : 4, 13, 13, (h & 0x7f) << 2, 0);
                return;
            }
            if ((h & 0xf500) == 0xb100) { /* cbz / cbnz */
                o->op = OP_CBZ;
                o->sub = BIT(h, 11);
                o->rn = h & 7;
                o->imm = (BIT(h, 9) << 6) | (BITS(h, 7, 3) << 1);
                o->rn = h & 7;
                return;
            }
            if ((h & 0xff00) == 0xb200) { /* sxth sxtb uxth uxtb */
                static const uint8_t k[4] = {1, 0, 3, 2};
                o->op = OP_EXT;
                o->sub = k[BITS(h, 7, 6)];
                o->rd = h & 7;
                o->rm = BITS(h, 5, 3);
                o->rn = 15;
                return;
            }
            if ((h & 0xfe00) == 0xb400) {
                o->op = OP_LDM;
                o->rn = 13;
                o->imm = (h & 0xff) | (BIT(h, 8) << 14);
                o->fl = FL_P | FL_W;
                return;
            } /* push: stmdb sp! */
            if ((h & 0xfe00) == 0xbc00) {
                o->op = OP_LDM;
                o->sub = 1;
                o->rn = 13;
                o->imm = (h & 0xff) | (BIT(h, 8) << 15);
                o->fl = FL_W | FL_U;
                return;
            } /* pop: ldmia sp! */
            if ((h & 0xff00) == 0xba00) {
                unsigned k = BITS(h, 7, 6);
                if (k == 0) o->op = OP_REV;
                else if (k == 1) o->op = OP_REV16;
                else if (k == 3) o->op = OP_REVSH;
                else {
                    o->op = OP_UNDEF;
                    return;
                }
                o->rd = h & 7;
                o->rm = BITS(h, 5, 3);
                return;
            }
            if ((h & 0xff00) == 0xbe00) {
                o->op = OP_BKPT;
                o->imm = h & 0xff;
                return;
            }
            if ((h & 0xff00) == 0xbf00) {
                if ((h & 0xf) != 0) {
                    o->op = OP_IT;
                    o->imm = h & 0xff;
                    return;
                }
                if (BITS(h, 7, 4) <= 5) {
                    o->op = OP_NOP;
                    return;
                } /* nop yield wfe wfi sev sevl */
                o->op = OP_UNDEF;
                return;
            }
            if ((h & 0xffe8) == 0xb660) {
                o->op = OP_NOP;
                return;
            } /* cps */
            o->op = OP_UNDEF;
            return;
        }
        case 0x18:
        case 0x19: { /* stmia / ldmia rn!, list */
            o->op = OP_LDM;
            o->rn = BITS(h, 10, 8);
            o->imm = h & 0xff;
            o->sub = BIT(h, 11);
            o->fl = FL_U;
            if (!BIT(h, 11) || !((o->imm >> o->rn) & 1)) o->fl |= FL_W;
            return;
        }
        case 0x1a:
        case 0x1b: { /* conditional branch / svc / udf */
            unsigned cond = BITS(h, 11, 8);
            if (cond == 15) {
                o->op = OP_SVC;
                o->imm = h & 0xff;
                return;
            }
            if (cond == 14) {
                o->op = OP_UNDEF;
                return;
            }
            o->op = OP_B;
            o->cond = cond;
            o->imm = sext(h & 0xff, 8) << 1;
            return;
        }
        case 0x1c:
            o->op = OP_B;
            o->imm = sext(h & 0x7ff, 11) << 1;
            return;
    }
    o->op = OP_UNDEF;
}

/* ---- 32-bit ---------------------------------------------------------------------------------------------------- */
static uint32_t thumb_expand_imm(uint32_t imm12, int *carry) {
    *carry = 0;
    if (BITS(imm12, 11, 10) == 0) {
        uint32_t b = imm12 & 0xff;
        switch (BITS(imm12, 9, 8)) {
            case 0: return b;
            case 1: return b | b << 16;
            case 2: return b << 8 | b << 24;
            default: return b | b << 8 | b << 16 | b << 24;
        }
    }
    uint32_t v = 0x80 | (imm12 & 0x7f), rot = BITS(imm12, 11, 7);
    *carry = 1;
    return (v >> rot) | (v << (32 - rot));
}

/* Thumb op4 (data processing, modified immediate / shifted register) -> ARM data-processing opcode; -1 undefined, 16 = orn */
static int dp_map(unsigned op4, unsigned rd, unsigned rn, int s, unsigned *special) {
    *special = 0;
    switch (op4) {
        case 0:
            if (rd == 15 && s) return 8;
            return 0;      /* and / tst */
        case 1: return 14; /* bic */
        case 2:
            if (rn == 15) return 13;
            return 12; /* orr / mov */
        case 3:
            if (rn == 15) return 15;
            return 16; /* orn / mvn */
        case 4:
            if (rd == 15 && s) return 9;
            return 1;                    /* eor / teq */
        case 6: *special = 1; return -1; /* pkh */
        case 8:
            if (rd == 15 && s) return 11;
            return 4; /* add / cmn */
        case 10: return 5;
        case 11: return 6;
        case 13:
            if (rd == 15 && s) return 10;
            return 2; /* sub / cmp */
        case 14: return 3;
        default: return -1;
    }
}

static void t32_dp_modified(uint16_t h1, uint16_t h2, A32Insn *o) {
    unsigned op4 = BITS(h1, 8, 5), s = BIT(h1, 4), rn = h1 & 15, rd = BITS(h2, 11, 8);
    unsigned special;
    int m = dp_map(op4, rd, rn, s, &special);
    if (m < 0) {
        o->op = OP_UNDEF;
        return;
    }
    uint32_t imm12 = (BIT(h1, 10) << 11) | (BITS(h2, 14, 12) << 8) | (h2 & 0xff);
    int carry;
    o->op = OP_DP;
    o->sub = m;
    o->rd = rd;
    o->rn = rn;
    o->k = K_IMM;
    o->imm = thumb_expand_imm(imm12, &carry);
    if (carry) o->fl |= FL_ICARRY;
    if (s) o->fl |= FL_S;
    if (m == 8 || m == 9 || m == 10 || m == 11) o->rd = 0;
}

static void t32_dp_shifted(uint16_t h1, uint16_t h2, A32Insn *o) {
    unsigned op4 = BITS(h1, 8, 5), s = BIT(h1, 4), rn = h1 & 15, rd = BITS(h2, 11, 8), rm = h2 & 15;
    unsigned sh = BITS(h2, 5, 4), sa = (BITS(h2, 14, 12) << 2) | BITS(h2, 7, 6);
    unsigned special;
    int m = dp_map(op4, rd, rn, s, &special);
    if (special) { /* pkhbt / pkhtb */
        if (s) {
            o->op = OP_UNDEF;
            return;
        }
        o->op = OP_PKH;
        o->sub = BIT(h2, 5);
        o->rd = rd;
        o->rn = rn;
        o->rm = rm;
        o->sa = sa;
        return;
    }
    if (m < 0) {
        o->op = OP_UNDEF;
        return;
    }
    o->op = OP_DP;
    o->sub = m;
    o->rd = rd;
    o->rn = rn;
    o->rm = rm;
    o->sh = sh;
    o->sa = sa;
    o->k = K_RSI;
    if (s) o->fl |= FL_S;
    if (m == 8 || m == 9 || m == 10 || m == 11) o->rd = 0;
}

static void t32_plain_imm(uint16_t h1, uint16_t h2, A32Insn *o) {
    unsigned op5 = BITS(h1, 8, 4), rn = h1 & 15, rd = BITS(h2, 11, 8);
    uint32_t imm12 = (BIT(h1, 10) << 11) | (BITS(h2, 14, 12) << 8) | (h2 & 0xff);
    unsigned imm5 = (BITS(h2, 14, 12) << 2) | BITS(h2, 7, 6);
    o->rd = rd;
    o->rn = rn;
    switch (op5) {
        case 0x00:
            o->op = OP_DP;
            o->sub = 4;
            o->k = K_IMM;
            o->imm = imm12;
            return;
        case 0x0a:
            o->op = OP_DP;
            o->sub = 2;
            o->k = K_IMM;
            o->imm = imm12;
            return;
        case 0x04:
            o->op = OP_MOVW;
            o->imm = (rn << 12) | imm12;
            return;
        case 0x0c:
            o->op = OP_MOVT;
            o->imm = (rn << 12) | imm12;
            return;
        case 0x10:
        case 0x12:
            o->op = OP_SAT;
            o->sub = 0;
            o->rn = rn;
            o->sh = BIT(h1, 5) ? 2 : 0;
            o->sa = imm5;
            o->imm = (h2 & 0x1f) + 1;
            if (BIT(h1, 5) && imm5 == 0) {
                o->op = OP_SAT;
                o->sub = 2;
                o->imm = (h2 & 0xf) + 1;
            } /* ssat16 */
            return;
        case 0x18:
        case 0x1a:
            o->op = OP_SAT;
            o->sub = 1;
            o->rn = rn;
            o->sh = BIT(h1, 5) ? 2 : 0;
            o->sa = imm5;
            o->imm = h2 & 0x1f;
            if (BIT(h1, 5) && imm5 == 0) {
                o->sub = 3;
                o->imm = h2 & 0xf;
            } /* usat16 */
            return;
        case 0x14:
            o->op = OP_BFX;
            o->sub = 1;
            o->imm = imm5;
            o->imm2 = (h2 & 0x1f) + 1;
            if (o->imm + o->imm2 > 32) o->op = OP_UNDEF;
            return;
        case 0x1c:
            o->op = OP_BFX;
            o->sub = 0;
            o->imm = imm5;
            o->imm2 = (h2 & 0x1f) + 1;
            if (o->imm + o->imm2 > 32) o->op = OP_UNDEF;
            return;
        case 0x16: {
            unsigned msb = h2 & 0x1f;
            if (msb < imm5) {
                o->op = OP_UNDEF;
                return;
            }
            o->op = OP_BFI;
            o->imm = imm5;
            o->imm2 = msb - imm5 + 1;
            return;
        }
    }
    o->op = OP_UNDEF;
}

static void t32_branch_misc(uint16_t h1, uint16_t h2, A32Insn *o) {
    unsigned s = BIT(h1, 10), j1 = BIT(h2, 13), j2 = BIT(h2, 11);
    if (BIT(h2, 14) == 0 && BIT(h2, 12) == 0) {
        if (BITS(h1, 9, 7) != 7) { /* conditional branch T3 */
            o->op = OP_B;
            o->cond = BITS(h1, 9, 6);
            uint32_t imm = (s << 20) | (j2 << 19) | (j1 << 18) | (BITS(h1, 5, 0) << 12) | (BITS(h2, 10, 0) << 1);
            o->imm = sext(imm, 21);
            return;
        }
        /* misc control */
        if ((h1 & 0xffe0) == 0xf380 && (h2 & 0xff00) == 0x8800) {
            o->op = OP_MSR;
            o->imm = BITS(h2, 11, 8);
            o->rm = h1 & 15;
            return;
        }
        if ((h1 & 0xffe0) == 0xf380 && (h2 & 0xf300) == 0x8000) {
            o->op = OP_MSR;
            o->imm = BITS(h2, 11, 10) == 2 ? 8 : BITS(h2, 11, 10) == 1 ? 4 : BITS(h2, 11, 10) == 3 ? 12 : 0;
            o->rm = h1 & 15;
            return;
        }
        if (h1 == 0xf3af && (h2 & 0xff00) == 0x8000) {
            o->op = OP_NOP;
            return;
        }
        if (h1 == 0xf3bf && h2 == 0x8f2f) {
            o->op = OP_CLREX;
            return;
        }
        if (h1 == 0xf3bf && ((h2 & 0xfff0) == 0x8f40 || (h2 & 0xfff0) == 0x8f50 || (h2 & 0xfff0) == 0x8f60)) {
            o->op = OP_NOP;
            return;
        }
        if (h1 == 0xf3ef && (h2 & 0xf000) == 0x8000 && (h2 & 0xff) == 0) {
            o->op = OP_MRS;
            o->rd = BITS(h2, 11, 8);
            return;
        }
        o->op = OP_UNDEF;
        return;
    }
    if ((h1 & 0xfff0) == 0xf7f0 && (h2 & 0xf000) == 0xa000) {
        o->op = OP_UNDEF;
        return;
    } /* udf.w */
    unsigned i1 = (~(j1 ^ s)) & 1, i2 = (~(j2 ^ s)) & 1;
    uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | (BITS(h1, 9, 0) << 12) | (BITS(h2, 10, 0) << 1);
    imm = sext(imm, 25);
    if (BIT(h2, 14) == 0) {
        o->op = OP_B;
        o->imm = imm;
        return;
    } /* b.w T4 */
    if (BIT(h2, 12)) {
        o->op = OP_BL;
        o->imm = imm;
        return;
    }
    if (BIT(h2, 0)) {
        o->op = OP_UNDEF;
        return;
    }
    o->op = OP_BLX_IMM;
    o->imm = imm;
}

static void t32_ldst_single(uint16_t h1, uint16_t h2, A32Insn *o) {
    unsigned size = BITS(h1, 6, 5), load = BIT(h1, 4), sign = BIT(h1, 8), rn = h1 & 15, rt = BITS(h2, 15, 12);
    if (size == 3) {
        o->op = OP_UNDEF;
        return;
    }
    if (sign && !load) {
        o->op = OP_UNDEF;
        return;
    }
    static const uint8_t st[3] = {2, 4, 0}, ldu[3] = {3, 5, 1}, lds[3] = {6, 7, 1};
    unsigned sub = load ? (sign ? lds[size] : ldu[size]) : st[size];
    if (sign && size == 2) {
        o->op = OP_UNDEF;
        return;
    }
    o->op = OP_LDST;
    o->sub = sub;
    o->rd = rt;
    o->rn = rn;
    if (load && rt == 15 && size < 2) {
        o->op = OP_NOP;
        return;
    } /* pld / pli hints */
    if (load && rn == 15) {
        o->imm = BITS(h2, 11, 0);
        o->fl = FL_P | (BIT(h1, 7) ? FL_U : 0);
        return;
    } /* literal */
    if (BIT(h1, 7)) {
        o->imm = BITS(h2, 11, 0);
        o->fl = FL_P | FL_U;
        return;
    }
    if (BIT(h2, 11)) {
        unsigned p = BIT(h2, 10), u = BIT(h2, 9), w = BIT(h2, 8);
        if (!p && !w) {
            o->op = BITS(h2, 11, 8) == 14 ? OP_LDST : OP_UNDEF;
            if (o->op == OP_UNDEF) return;
        }
        o->imm = h2 & 0xff;
        if (BITS(h2, 11, 8) == 14) {
            o->fl = FL_P | FL_U;
            return;
        } /* ldrt / strt: treated as a plain access */
        if (p) o->fl |= FL_P;
        if (u) o->fl |= FL_U;
        if (w || !p) o->fl |= FL_W;
        return;
    }
    if (BITS(h2, 11, 6) == 0) {
        o->fl = FL_P | FL_U | FL_REG;
        o->rm = h2 & 15;
        o->sh = 0;
        o->sa = BITS(h2, 5, 4);
        return;
    }
    o->op = OP_UNDEF;
}

static void t32_ldm_dual(uint16_t h1, uint16_t h2, A32Insn *o) { /* 1110 100x xxxx */
    unsigned rn = h1 & 15, load = BIT(h1, 4), w = BIT(h1, 5);
    if (!BIT(h1, 6)) { /* ldm / stm */
        unsigned op = BITS(h1, 8, 7);
        if (op != 1 && op != 2) {
            o->op = OP_UNDEF;
            return;
        }
        o->op = OP_LDM;
        o->rn = rn;
        o->imm = h2;
        o->sub = load;
        if (op == 1) o->fl |= FL_U;
        else o->fl |= FL_P;
        if (w) o->fl |= FL_W;
        return;
    }
    unsigned p = BIT(h1, 8), u = BIT(h1, 7);
    if (!p && !w) { /* exclusive, acquire/release, table branch */
        unsigned rt = BITS(h2, 15, 12), hi = BITS(h1, 7, 4), lo = BITS(h2, 7, 4);
        if (hi == 4) {
            o->op = OP_STREX;
            o->sub = 2;
            o->rd = BITS(h2, 11, 8);
            o->rm = rt;
            o->rn = rn;
            o->imm = (h2 & 0xff) << 2;
            return;
        }
        if (hi == 5) {
            o->op = OP_LDREX;
            o->sub = 2;
            o->rd = rt;
            o->rn = rn;
            o->imm = (h2 & 0xff) << 2;
            return;
        }
        o->rn = rn;
        if (hi == 12) {
            static const int8_t sz[16] = {-1, -1, -1, -1, 0, 1, -1, 3, 0, 1, 2, -1, 0, 1, 2, 3};
            int s = sz[lo];
            if (s < 0) {
                o->op = OP_UNDEF;
                return;
            }
            if (lo >= 8 && lo <= 10) { /* stlb stlh stl */
                static const uint8_t stl[3] = {2, 4, 0};
                o->op = OP_LDST;
                o->sub = stl[s];
                o->rd = rt;
                o->fl = FL_P | FL_U | FL_ACQ;
                return;
            }
            o->op = OP_STREX;
            o->sub = s;
            o->rd = h2 & 15;
            o->rm = rt;
            o->ra = BITS(h2, 11, 8);
            o->fl = lo >= 12 ? FL_ACQ : 0;
            return;
        }
        if (hi == 13) {
            if (lo <= 1) {
                o->op = OP_TB;
                o->sub = lo;
                o->rm = h2 & 15;
                return;
            }
            static const int8_t sz[16] = {-1, -1, -1, -1, 0, 1, -1, 3, 0, 1, 2, -1, 0, 1, 2, 3};
            int s = sz[lo];
            if (s < 0) {
                o->op = OP_UNDEF;
                return;
            }
            if (lo >= 8 && lo <= 10) { /* ldab ldah lda */
                static const uint8_t lda[3] = {3, 5, 1};
                o->op = OP_LDST;
                o->sub = lda[s];
                o->rd = rt;
                o->fl = FL_P | FL_U | FL_ACQ;
                return;
            }
            o->op = OP_LDREX;
            o->sub = s;
            o->rd = rt;
            o->ra = BITS(h2, 11, 8);
            o->fl = lo >= 12 ? FL_ACQ : 0;
            return;
        }
        o->op = OP_UNDEF;
        return;
    }
    /* ldrd / strd (immediate) */
    o->op = OP_LDST;
    o->sub = load ? 9 : 8;
    o->rn = rn;
    o->rd = BITS(h2, 15, 12);
    o->ra = BITS(h2, 11, 8);
    o->imm = (h2 & 0xff) << 2;
    if (p) o->fl |= FL_P;
    if (u) o->fl |= FL_U;
    if (w || !p) o->fl |= FL_W;
    if (load && rn == 15) o->fl &= ~FL_W;
}

static void t32_reg_group(uint16_t h1, uint16_t h2, A32Insn *o) { /* 1111 1010 xxxx: shifts, extend, parallel, misc */
    unsigned rn = h1 & 15, rd = BITS(h2, 11, 8), rm = h2 & 15;
    if (!BIT(h1, 7)) { /* register shifts and extends */
        if (BITS(h2, 15, 12) != 15) {
            o->op = OP_UNDEF;
            return;
        }
        if (BIT(h2, 7)) {
            static const int8_t kind[8] = {1, 3, 4, 5, 0, 2, -1, -1}; /* sxth uxth sxtb16 uxtb16 sxtb uxtb */
            int k = kind[BITS(h1, 6, 4)];
            if (k < 0 || BIT(h2, 6)) {
                o->op = OP_UNDEF;
                return;
            }
            o->op = OP_EXT;
            o->sub = k;
            o->rd = rd;
            o->rn = rn;
            o->rm = rm;
            o->sa = BITS(h2, 5, 4) * 8;
            return;
        }
        if (BITS(h2, 7, 4) != 0) {
            o->op = OP_UNDEF;
            return;
        }
        o->op = OP_DP;
        o->sub = 13;
        o->rd = rd;
        o->rm = rn;
        o->ra = rm;
        o->k = K_RSR;
        o->sh = BITS(h1, 6, 5);
        if (BIT(h1, 4)) o->fl |= FL_S;
        return;
    }
    if (BITS(h2, 15, 12) != 15) {
        o->op = OP_UNDEF;
        return;
    }
    if (!BIT(h2, 7)) { /* parallel add / subtract */
        static const int8_t opmap[8] = {4, 0, 1, -1, 7, 3, 2, -1};
        static const int8_t pre[8] = {0, 1, 2, -1, 3, 4, 5, -1};
        int op = opmap[BITS(h1, 6, 4)], p = pre[BITS(h2, 6, 4)];
        if (op < 0 || p < 0) {
            o->op = OP_UNDEF;
            return;
        }
        o->op = OP_PAR;
        o->sub = p * 8 + op;
        o->rd = rd;
        o->rn = rn;
        o->rm = rm;
        return;
    }
    unsigned op1 = BITS(h1, 5, 4), op2 = BITS(h2, 5, 4);
    if (BIT(h1, 6) || BIT(h2, 6)) {
        o->op = OP_UNDEF;
        return;
    }
    switch (op1) {
        case 0: {
            static const uint8_t qmap[4] = {0, 2, 1, 3};
            o->op = OP_QARITH;
            o->sub = qmap[op2];
            o->rd = rd;
            o->rm = rm;
            o->rn = rn;
            break;
        } /* qadd qdadd qsub qdsub (Thumb order) -> ARM order; Rd, Rm, Rn */
        case 1:
            if (rn != rm) {
                o->op = OP_UNDEF;
                return;
            }
            o->rd = rd;
            o->rm = rm;
            o->op = op2 == 0 ? OP_REV : op2 == 1 ? OP_REV16 : op2 == 2 ? OP_RBIT : OP_REVSH;
            break;
        case 2:
            if (op2) {
                o->op = OP_UNDEF;
                return;
            }
            o->op = OP_SEL;
            o->rd = rd;
            o->rn = rn;
            o->rm = rm;
            break;
        default:
            if (op2 || rn != rm) {
                o->op = OP_UNDEF;
                return;
            }
            o->op = OP_CLZ;
            o->rd = rd;
            o->rm = rm;
            break;
    }
}

static void t32_mul_group(uint16_t h1, uint16_t h2, A32Insn *o) { /* 1111 1011 xxxx */
    unsigned rn = h1 & 15, ra = BITS(h2, 15, 12), rd = BITS(h2, 11, 8), rm = h2 & 15, op1 = BITS(h1, 6, 4), op2 = BITS(h2, 7, 4);
    if (!BIT(h1, 7)) {
        o->rd = rd;
        o->rn = rn;
        o->rm = rm;
        o->ra = ra;
        if (BITS(h2, 7, 6)) {
            o->op = OP_UNDEF;
            return;
        }
        switch (op1) {
            case 0:
                if (BITS(h2, 5, 4) == 0) {
                    o->op = OP_MUL;
                    o->sub = ra == 15 ? 0 : 1;
                    return;
                }
                if (BITS(h2, 5, 4) == 1) {
                    o->op = OP_MUL;
                    o->sub = 2;
                    return;
                }
                break;
            case 1:
                o->op = OP_SMULH;
                o->sub = ra == 15 ? 3 : 0;
                o->imm = BIT(h2, 5) | BIT(h2, 4) << 1; /* x = N (Rn half), y = M (Rm half) */
                return;
            case 2:
            case 4:
                if (BITS(h2, 5, 5)) {
                    o->op = OP_UNDEF;
                    return;
                }
                o->op = OP_SMUAD;
                o->sub = op1 == 2 ? 0 : 1;
                o->imm = BIT(h2, 4);
                return;
            case 3:
                o->op = OP_SMULH;
                o->sub = 1;
                o->imm = BIT(h2, 4) << 1;
                o->imm2 = ra != 15;
                if (BIT(h2, 5)) o->op = OP_UNDEF;
                return;
            case 5:
                o->op = OP_SMMUL;
                o->sub = 0;
                o->imm = BIT(h2, 4);
                return;
            case 6:
                o->op = OP_SMMUL;
                o->sub = 1;
                o->imm = BIT(h2, 4);
                return;
            case 7:
                if (BITS(h2, 5, 4)) {
                    o->op = OP_UNDEF;
                    return;
                }
                o->op = OP_USAD8;
                return;
        }
        o->op = OP_UNDEF;
        return;
    }
    /* long multiply and divide */
    unsigned rdlo = BITS(h2, 15, 12), rdhi = BITS(h2, 11, 8);
    o->rn = rn;
    o->rm = rm;
    switch (op1) {
        case 0:
            if (op2 == 0) {
                o->op = OP_MULL;
                o->sub = 2;
                o->rd = rdlo;
                o->ra = rdhi;
                return;
            }
            break;
        case 1:
            if (op2 == 15) {
                o->op = OP_DIV;
                o->sub = 0;
                o->rd = BITS(h2, 11, 8);
                if (rdlo != 15) o->op = OP_UNDEF;
                return;
            }
            break;
        case 2:
            if (op2 == 0) {
                o->op = OP_MULL;
                o->sub = 0;
                o->rd = rdlo;
                o->ra = rdhi;
                return;
            }
            break;
        case 3:
            if (op2 == 15) {
                o->op = OP_DIV;
                o->sub = 1;
                o->rd = BITS(h2, 11, 8);
                if (rdlo != 15) o->op = OP_UNDEF;
                return;
            }
            break;
        case 4:
            if (op2 == 0) {
                o->op = OP_MULL;
                o->sub = 3;
                o->rd = rdlo;
                o->ra = rdhi;
                return;
            }
            if ((op2 & 0xc) == 0x8) { /* smlal<x><y> */
                o->op = OP_SMULH;
                o->sub = 2;
                o->rd = rdlo;
                o->ra = rdhi;
                o->imm = BIT(h2, 5) | BIT(h2, 4) << 1;
                return;
            }
            if ((op2 & 0xe) == 0xc) {
                o->op = OP_SMUAD;
                o->sub = 2;
                o->rd = rdlo;
                o->ra = rdhi;
                o->imm = BIT(h2, 4);
                return;
            }
            break;
        case 5:
            if ((op2 & 0xe) == 0xc) {
                o->op = OP_SMUAD;
                o->sub = 3;
                o->rd = rdlo;
                o->ra = rdhi;
                o->imm = BIT(h2, 4);
                return;
            }
            break;
        case 6:
            if (op2 == 0) {
                o->op = OP_MULL;
                o->sub = 1;
                o->rd = rdlo;
                o->ra = rdhi;
                return;
            }
            if (op2 == 6) {
                o->op = OP_MULL;
                o->sub = 4;
                o->rd = rdlo;
                o->ra = rdhi;
                return;
            }
            break;
    }
    o->op = OP_UNDEF;
}

static void vfp_word(uint16_t h1, uint16_t h2, A32Insn *o) {
    uint32_t hi = h1;
    if ((h1 & 0xef00) == 0xef00) {
        o->op = OP_NEON;
        o->imm = 0xf2000000u | ((uint32_t)BIT(h1, 12) << 24) | ((hi & 0xff) << 16) | h2;
        return;
    }
    o->op = OP_VFP;
    o->imm = (hi << 16) | h2;
}

static void t32_coproc(uint16_t h1, uint16_t h2, A32Insn *o) { /* 1110 11xx, 1111 11xx, 1110 1111 / 1111 1111 */
    unsigned cp = BITS(h2, 11, 8);
    if ((h1 & 0xef00) == 0xef00) {
        vfp_word(h1, h2, o);
        return;
    }
    if (cp == 10 || cp == 11) {
        vfp_word(h1, h2, o);
        return;
    }
    if ((h1 & 0xff00) == 0xfe00 || (h1 & 0xfe00) == 0xfc00) {
        vfp_word(h1, h2, o);
        return;
    }                                            /* v8 vsel / vrint / vmaxnm / vcvt<r> */
    if ((h1 & 0xff00) == 0xee00 && BIT(h2, 4)) { /* mcr / mrc */
        o->op = BIT(h1, 4) ? OP_MRC : OP_MCR;
        o->rd = BITS(h2, 15, 12);
        o->imm = BITS(h1, 7, 5) << 16 | (h1 & 15) << 12 | (h2 & 15) << 8 | BITS(h2, 7, 5) << 4 | cp;
        return;
    }
    if ((h1 & 0xffe0) == 0xec40) { /* mcrr / mrrc */
        o->op = BIT(h1, 4) ? OP_MRRC : OP_UNIMPL;
        o->rd = BITS(h2, 15, 12);
        o->ra = h1 & 15;
        o->imm = BITS(h2, 7, 4) << 4 | (h2 & 15) << 8 | cp;
        return;
    }
    o->op = OP_UNIMPL;
}

static void thumb32(uint16_t h1, uint16_t h2, A32Insn *o) {
    o->len = 4;
    o->cond = CC_ALWAYS;
    switch (h1 >> 8) {
        case 0xe8:
        case 0xe9: t32_ldm_dual(h1, h2, o); return;
        case 0xea:
        case 0xeb: t32_dp_shifted(h1, h2, o); return;
        case 0xec:
        case 0xed:
        case 0xee:
        case 0xef: t32_coproc(h1, h2, o); return;
        case 0xf0:
        case 0xf1:
        case 0xf2:
        case 0xf3:
        case 0xf4:
        case 0xf5:
        case 0xf6:
        case 0xf7:
            if (BIT(h2, 15)) t32_branch_misc(h1, h2, o);
            else if (!BIT(h1, 9)) t32_dp_modified(h1, h2, o);
            else t32_plain_imm(h1, h2, o);
            return;
        case 0xf8:
        case 0xf9:
            if ((h1 & 0xff10) == 0xf900) {
                o->op = OP_NEON;
                o->imm = 0xf4000000u | ((uint32_t)(h1 & 0xff) << 16) | h2;
                return;
            } /* element / structure load-store */
            t32_ldst_single(h1, h2, o);
            return;
        case 0xfa: t32_reg_group(h1, h2, o); return;
        case 0xfb: t32_mul_group(h1, h2, o); return;
        default: t32_coproc(h1, h2, o); return; /* 0xfc..0xff */
    }
}

unsigned a32_decode_thumb(uint16_t h1, uint16_t h2, A32Insn *o) {
    memset(o, 0, sizeof *o);
    if (a32_thumb_is32(h1)) {
        thumb32(h1, h2, o);
        return 4;
    }
    if ((h1 >> 11) < 12) thumb16(h1, o);
    else thumb16_b(h1, o);
    return 2;
}
