/* AArch32 executor: runs decoded instructions against an A32Cpu. Integer, branch, load/store and system instructions live here;
 * VFP is in a32_vfp.c and NEON in a32_neon.c. */
#include "a32_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BITS(w, hi, lo) (((w) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))
#define BIT(w, b) (((w) >> (b)) & 1u)

#define CACHE_BITS 15
#define CACHE_SIZE (1u << CACHE_BITS)
typedef struct {
    uint32_t key, raw, valid;
    A32Insn insn;
} CacheEntry;

/* ---- helpers ------------------------------------------------------------------------------------------------------ */
static inline uint32_t ld8(const A32Cpu *c, uint32_t a) { return c->mem[a]; }
static inline uint32_t ld16(const A32Cpu *c, uint32_t a) {
    uint16_t v;
    memcpy(&v, c->mem + a, 2);
    return v;
}
static inline uint32_t ld32(const A32Cpu *c, uint32_t a) {
    uint32_t v;
    memcpy(&v, c->mem + a, 4);
    return v;
}
static inline void st8(A32Cpu *c, uint32_t a, uint32_t v) { c->mem[a] = (uint8_t)v; }
static inline void st16(A32Cpu *c, uint32_t a, uint32_t v) {
    uint16_t x = (uint16_t)v;
    memcpy(c->mem + a, &x, 2);
}
static inline void st32(A32Cpu *c, uint32_t a, uint32_t v) { memcpy(c->mem + a, &v, 4); }

static inline uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

uint32_t a32_get_cpsr(const A32Cpu *c) { return c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28 | c->q << 27 | (c->ge & 15) << 16 | c->thumb << 5 | 0x10; }
void a32_set_flags(A32Cpu *c, uint32_t b) {
    c->n = b >> 31 & 1;
    c->z = b >> 30 & 1;
    c->c = b >> 29 & 1;
    c->v = b >> 28 & 1;
    c->q = b >> 27 & 1;
}

void a32_reset(A32Cpu *c) {
    void *mem = c->mem;
    const A32Hooks *h = c->hooks;
    void *user = c->user;
    memset(c, 0, sizeof *c);
    c->mem = mem;
    c->hooks = h;
    c->user = user;
    c->fpexc_en = 1 << 30;
}

const char *a32_exit_name(A32Exit e) {
    switch (e) {
        case A32_EXIT_BUDGET: return "budget";
        case A32_EXIT_STOP: return "stop";
        case A32_EXIT_UNDEF: return "undefined";
        case A32_EXIT_UNIMPL: return "unimplemented";
        case A32_EXIT_BKPT: return "bkpt";
        case A32_EXIT_ALIGN: return "alignment";
        case A32_EXIT_NOBLOCK: return "no lifted code at this pc";
    }
    return "?";
}

__attribute__((unused)) static int cond_pass(const A32Cpu *c, unsigned cond) {
    int r;
    switch (cond >> 1) {
        case 0: r = c->z; break;
        case 1: r = c->c; break;
        case 2: r = c->n; break;
        case 3: r = c->v; break;
        case 4: r = c->c && !c->z; break;
        case 5: r = c->n == c->v; break;
        case 6: r = c->n == c->v && !c->z; break;
        default: return 1;
    }
    return (cond & 1) ? !r : r;
}

/* shifts: returns the result and the carry out */
static uint32_t shift_c(unsigned type, uint32_t v, unsigned amt, unsigned cin, unsigned *cout) {
    *cout = cin;
    switch (type) {
        case 0: /* lsl */
            if (amt == 0) return v;
            if (amt < 32) {
                *cout = (v >> (32 - amt)) & 1;
                return v << amt;
            }
            *cout = amt == 32 ? (v & 1) : 0;
            return 0;
        case 1: /* lsr */
            if (amt == 0) return v;
            if (amt < 32) {
                *cout = (v >> (amt - 1)) & 1;
                return v >> amt;
            }
            *cout = amt == 32 ? (v >> 31) : 0;
            return 0;
        case 2: /* asr */
            if (amt == 0) return v;
            if (amt < 32) {
                *cout = (v >> (amt - 1)) & 1;
                return (uint32_t)((int32_t)v >> amt);
            }
            *cout = v >> 31;
            return (uint32_t)((int32_t)v >> 31);
        default: /* ror */
            if (amt == 0) return v;
            amt &= 31;
            if (amt == 0) {
                *cout = v >> 31;
                return v;
            }
            *cout = (v >> (amt - 1)) & 1;
            return ror32(v, amt);
    }
}
/* immediate shift as encoded (type, imm5): lsr/asr #0 mean 32, ror #0 means rrx */
static uint32_t shift_imm(unsigned type, uint32_t v, unsigned imm5, unsigned cin, unsigned *cout) {
    if (type == 3 && imm5 == 0) {
        *cout = v & 1;
        return (v >> 1) | (cin << 31);
    }
    if ((type == 1 || type == 2) && imm5 == 0) imm5 = 32;
    return shift_c(type, v, imm5, cin, cout);
}

static inline uint32_t add_c(uint32_t a, uint32_t b, unsigned cin, unsigned *cout, unsigned *vout) {
    uint64_t u = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)u;
    *cout = (unsigned)(u >> 32);
    *vout = (((a ^ r) & (b ^ r)) >> 31) & 1;
    return r;
}

/* saturation */
static int32_t ssat32(int64_t v, unsigned n, int *sat) { /* n in 1..32 */
    int64_t hi = (n == 32) ? 0x7fffffffLL : (1LL << (n - 1)) - 1, lo = -hi - 1;
    if (v > hi) {
        *sat = 1;
        return (int32_t)hi;
    }
    if (v < lo) {
        *sat = 1;
        return (int32_t)lo;
    }
    return (int32_t)v;
}
static uint32_t usat32(int64_t v, unsigned n, int *sat) { /* n in 0..31 */
    int64_t hi = (1LL << n) - 1;
    if (v > hi) {
        *sat = 1;
        return (uint32_t)hi;
    }
    if (v < 0) {
        *sat = 1;
        return 0;
    }
    return (uint32_t)v;
}

static inline unsigned popcount16(uint32_t v) { return (unsigned)__builtin_popcount(v & 0xffff); }

/* ---- the step ----------------------------------------------------------------------------------------------------- */
typedef struct {
    A32Cpu *c;
    uint32_t pc, pcv, npc;
    int in_it;
    A32Exit exit;
} Ctx;

static inline uint32_t R(const Ctx *x, unsigned n) { return n == 15 ? x->pcv : x->c->r[n]; }

/* BXWritePC / LoadWritePC semantics */
static void bx_write(Ctx *x, uint32_t addr) {
    if (addr & 1) {
        x->c->thumb = 1;
        x->npc = addr & ~1u;
    } else {
        x->c->thumb = 0;
        x->npc = addr & ~3u;
    }
}
/* ALUWritePC: interworking in ARM state, plain branch in Thumb */
static void alu_write_pc(Ctx *x, uint32_t addr) {
    if (x->c->thumb) x->npc = addr & ~1u;
    else bx_write(x, addr);
}

static void exec_dp(Ctx *x, const A32Insn *i) {
    A32Cpu *c = x->c;
    unsigned sh_carry = c->c;
    uint32_t b;
    if (i->k == K_IMM) {
        b = i->imm;
        if (i->fl & FL_ICARRY) sh_carry = b >> 31;
    } else if (i->k == K_RSI) b = shift_imm(i->sh, R(x, i->rm), i->sa, c->c, &sh_carry);
    else b = shift_c(i->sh, R(x, i->rm), R(x, i->ra) & 0xff, c->c, &sh_carry);
    uint32_t a = (i->k == K_IMM && i->rn == 15) ? (x->pcv & ~3u) : R(x, i->rn);
    int setf = (i->fl & FL_S) || ((i->fl & FL_SIT) && !x->in_it);
    uint32_t res = 0;
    unsigned cf = c->c, vf = c->v;
    int logical = 0, wr = 1;
    switch (i->sub) {
        case 0:
            res = a & b;
            logical = 1;
            break;
        case 1:
            res = a ^ b;
            logical = 1;
            break;
        case 2: res = add_c(a, ~b, 1, &cf, &vf); break;
        case 3: res = add_c(b, ~a, 1, &cf, &vf); break;
        case 4: res = add_c(a, b, 0, &cf, &vf); break;
        case 5: res = add_c(a, b, c->c, &cf, &vf); break;
        case 6: res = add_c(a, ~b, c->c, &cf, &vf); break;
        case 7: res = add_c(b, ~a, c->c, &cf, &vf); break;
        case 8:
            res = a & b;
            logical = 1;
            wr = 0;
            break;
        case 9:
            res = a ^ b;
            logical = 1;
            wr = 0;
            break;
        case 10:
            res = add_c(a, ~b, 1, &cf, &vf);
            wr = 0;
            break;
        case 11:
            res = add_c(a, b, 0, &cf, &vf);
            wr = 0;
            break;
        case 12:
            res = a | b;
            logical = 1;
            break;
        case 13:
            res = b;
            logical = 1;
            break;
        case 14:
            res = a & ~b;
            logical = 1;
            break;
        case 15:
            res = ~b;
            logical = 1;
            break;
        default:
            res = a | ~b;
            logical = 1;
            break; /* 16: orn (Thumb-2) */
    }
    if (logical) {
        cf = sh_carry;
        vf = c->v;
    }
    if (wr && i->rd == 15) {
        if (setf && !c->thumb) {
            x->exit = A32_EXIT_UNIMPL;
            return;
        } /* exception return forms (movs pc, lr) are privileged */
        alu_write_pc(x, res);
        return;
    }
    if (setf) {
        c->n = res >> 31;
        c->z = res == 0;
        c->c = cf;
        c->v = vf;
    }
    if (wr) c->r[i->rd] = res;
}

static void exec_par(Ctx *x, const A32Insn *i) {
    A32Cpu *c = x->c;
    unsigned prefix = i->sub >> 3, op = i->sub & 7;
    uint32_t a = R(x, i->rn), b = R(x, i->rm), res = 0, ge = 0;
    int is_signed = prefix == 0 || prefix == 1 || prefix == 2;
    if (op <= 3) { /* 16-bit lanes: add16 asx sax sub16 */
        int64_t lane[2];
        for (unsigned k = 0; k < 2; k++) {
            int64_t av = is_signed ? (int16_t)(a >> (16 * k)) : (uint16_t)(a >> (16 * k));
            int64_t bv;
            int add;
            if (op == 0) {
                bv = is_signed ? (int16_t)(b >> (16 * k)) : (uint16_t)(b >> (16 * k));
                add = 1;
            } else if (op == 3) {
                bv = is_signed ? (int16_t)(b >> (16 * k)) : (uint16_t)(b >> (16 * k));
                add = 0;
            } else { /* asx: lo = a.lo - b.hi, hi = a.hi + b.lo ; sax: lo = a.lo + b.hi, hi = a.hi - b.lo */
                unsigned bk = 1 - k;
                bv = is_signed ? (int16_t)(b >> (16 * bk)) : (uint16_t)(b >> (16 * bk));
                add = (op == 1) ? (k == 1) : (k == 0);
            }
            int64_t r = add ? av + bv : av - bv;
            int g = is_signed ? r >= 0 : (add ? r >= 0x10000 : r >= 0);
            if (prefix == 1) {
                int s = 0;
                r = ssat32(r, 16, &s);
            } else if (prefix == 4) {
                int s = 0;
                r = (int64_t)usat32(r, 16, &s);
                if (!add && r < 0) r = 0;
            } else if (prefix == 2 || prefix == 5) r >>= 1;
            lane[k] = r;
            if (g && (prefix == 0 || prefix == 3)) ge |= 3u << (2 * k);
        }
        res = ((uint32_t)lane[0] & 0xffff) | ((uint32_t)lane[1] << 16);
    } else { /* 8-bit lanes: add8 sub8 */
        for (unsigned k = 0; k < 4; k++) {
            int64_t av = is_signed ? (int8_t)(a >> (8 * k)) : (uint8_t)(a >> (8 * k));
            int64_t bv = is_signed ? (int8_t)(b >> (8 * k)) : (uint8_t)(b >> (8 * k));
            int add = op == 4;
            int64_t r = add ? av + bv : av - bv;
            int g = is_signed ? r >= 0 : (add ? r >= 0x100 : r >= 0);
            if (prefix == 1) {
                int s = 0;
                r = ssat32(r, 8, &s);
            } else if (prefix == 4) {
                int s = 0;
                r = (int64_t)usat32(r, 8, &s);
                if (!add && r < 0) r = 0;
            } else if (prefix == 2 || prefix == 5) r >>= 1;
            res |= ((uint32_t)r & 0xff) << (8 * k);
            if (g && (prefix == 0 || prefix == 3)) ge |= 1u << k;
        }
    }
    if (prefix == 0 || prefix == 3) c->ge = ge;
    c->r[i->rd] = res;
}

static int64_t sat_signed64(A32Cpu *c, int64_t v, unsigned bits) {
    int s = 0;
    int32_t r = ssat32(v, bits, &s);
    if (s) c->q = 1;
    return r;
}

static void exec_mul_group(Ctx *x, const A32Insn *i) {
    A32Cpu *c = x->c;
    uint32_t rn = R(x, i->rn), rm = R(x, i->rm);
    int setf = (i->fl & FL_S) || ((i->fl & FL_SIT) && !x->in_it);
    switch (i->op) {
        case OP_MUL: {
            uint32_t r = rn * rm;
            if (i->sub == 1) r += R(x, i->ra);
            else if (i->sub == 2) r = R(x, i->ra) - r;
            c->r[i->rd] = r;
            if (setf) {
                c->n = r >> 31;
                c->z = r == 0;
            }
            break;
        }
        case OP_MULL: {
            uint64_t r;
            switch (i->sub) {
                case 0: r = (uint64_t)rn * rm; break;
                case 1: r = (uint64_t)rn * rm + (((uint64_t)c->r[i->ra] << 32) | c->r[i->rd]); break;
                case 2: r = (uint64_t)((int64_t)(int32_t)rn * (int32_t)rm); break;
                case 3: r = (uint64_t)((int64_t)(int32_t)rn * (int32_t)rm) + (((uint64_t)c->r[i->ra] << 32) | c->r[i->rd]); break;
                default: r = (uint64_t)rn * rm + c->r[i->ra] + c->r[i->rd]; break; /* umaal */
            }
            c->r[i->rd] = (uint32_t)r;
            c->r[i->ra] = (uint32_t)(r >> 32);
            if (setf) {
                c->n = (uint32_t)(r >> 63);
                c->z = r == 0;
            }
            break;
        }
        case OP_SMULH: {
            int32_t xv = (i->imm & 1) ? (int16_t)(rn >> 16) : (int16_t)rn;
            int32_t yv = (i->imm & 2) ? (int16_t)(rm >> 16) : (int16_t)rm;
            switch (i->sub) {
                case 0: { /* smla<x><y> */
                    int64_t p = (int64_t)xv * yv, s = p + (int32_t)R(x, i->ra);
                    if (s != (int32_t)s) c->q = 1;
                    c->r[i->rd] = (uint32_t)s;
                    break;
                }
                case 1: { /* smulw<y> / smlaw<y>: the multiplicand is the whole Rn */
                    int64_t p = ((int64_t)(int32_t)rn * yv) >> 16;
                    if (i->imm2) {
                        int64_t s = p + (int32_t)R(x, i->ra);
                        if (s != (int32_t)s) c->q = 1;
                        p = s;
                    }
                    c->r[i->rd] = (uint32_t)p;
                    break;
                }
                case 2: { /* smlal<x><y> */
                    int64_t acc = (int64_t)(((uint64_t)c->r[i->ra] << 32) | c->r[i->rd]) + (int64_t)xv * yv;
                    c->r[i->rd] = (uint32_t)acc;
                    c->r[i->ra] = (uint32_t)((uint64_t)acc >> 32);
                    break;
                }
                default: c->r[i->rd] = (uint32_t)((int64_t)xv * yv); break;
            }
            break;
        }
        case OP_SMMUL: {
            int64_t p = (int64_t)(int32_t)rn * (int32_t)rm;
            int64_t acc = (i->ra == 15 && i->sub == 0) ? 0 : (int64_t)((uint64_t)R(x, i->ra) << 32);
            int64_t r = i->sub == 0 ? acc + p : acc - p;
            if (i->imm) r += 0x80000000LL;
            c->r[i->rd] = (uint32_t)((uint64_t)r >> 32);
            break;
        }
        case OP_SMUAD: {
            uint32_t m = i->imm ? ror32(rm, 16) : rm;
            int64_t p1 = (int64_t)(int16_t)rn * (int16_t)m, p2 = (int64_t)(int16_t)(rn >> 16) * (int16_t)(m >> 16);
            int64_t v = (i->sub & 1) ? p1 - p2 : p1 + p2;
            if (i->sub < 2) {
                if (i->ra != 15) { v += (int32_t)c->r[i->ra]; }
                if (v != (int32_t)v) c->q = 1;
                c->r[i->rd] = (uint32_t)v;
            } else {
                int64_t acc = (int64_t)(((uint64_t)c->r[i->ra] << 32) | c->r[i->rd]) + v;
                c->r[i->rd] = (uint32_t)acc;
                c->r[i->ra] = (uint32_t)((uint64_t)acc >> 32);
            }
            break;
        }
        default: break;
    }
}

static void exec_misc_alu(Ctx *x, const A32Insn *i) {
    A32Cpu *c = x->c;
    switch (i->op) {
        case OP_DIV: {
            uint32_t n = R(x, i->rn), m = R(x, i->rm), r;
            if (m == 0) r = 0;
            else if (i->sub == 1) r = n / m;
            else if (n == 0x80000000u && m == 0xffffffffu) r = 0x80000000u;
            else r = (uint32_t)((int32_t)n / (int32_t)m);
            c->r[i->rd] = r;
            break;
        }
        case OP_CLZ: {
            uint32_t v = R(x, i->rm);
            c->r[i->rd] = v ? (uint32_t)__builtin_clz(v) : 32;
            break;
        }
        case OP_RBIT: {
            uint32_t v = R(x, i->rm), r = 0;
            for (unsigned k = 0; k < 32; k++) r |= ((v >> k) & 1) << (31 - k);
            c->r[i->rd] = r;
            break;
        }
        case OP_REV: c->r[i->rd] = __builtin_bswap32(R(x, i->rm)); break;
        case OP_REV16: {
            uint32_t v = R(x, i->rm);
            c->r[i->rd] = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
            break;
        }
        case OP_REVSH: {
            uint32_t v = R(x, i->rm);
            c->r[i->rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff));
            break;
        }
        case OP_SEL: {
            uint32_t a = R(x, i->rn), b = R(x, i->rm), r = 0;
            for (unsigned k = 0; k < 4; k++) r |= (((c->ge >> k) & 1) ? (a >> (8 * k)) & 0xff : (b >> (8 * k)) & 0xff) << (8 * k);
            c->r[i->rd] = r;
            break;
        }
        case OP_USAD8: {
            uint32_t a = R(x, i->rn), b = R(x, i->rm), s = 0;
            for (unsigned k = 0; k < 4; k++) {
                int d = (int)((a >> (8 * k)) & 0xff) - (int)((b >> (8 * k)) & 0xff);
                s += (uint32_t)(d < 0 ? -d : d);
            }
            if (i->ra != 15) s += R(x, i->ra);
            c->r[i->rd] = s;
            break;
        }
        case OP_EXT: {
            uint32_t v = ror32(R(x, i->rm), i->sa), add = i->rn == 15 ? 0 : R(x, i->rn), r;
            switch (i->sub) {
                case 0: r = (uint32_t)(int32_t)(int8_t)v + add; break;
                case 1: r = (uint32_t)(int32_t)(int16_t)v + add; break;
                case 2: r = (v & 0xff) + add; break;
                case 3: r = (v & 0xffff) + add; break;
                case 4: r = (((uint32_t)(int32_t)(int8_t)v + (add & 0xffff)) & 0xffff) | ((((uint32_t)(int32_t)(int8_t)(v >> 16)) + (add >> 16)) << 16); break;
                default: r = (((v & 0xff) + (add & 0xffff)) & 0xffff) | ((((v >> 16) & 0xff) + (add >> 16)) << 16); break;
            }
            c->r[i->rd] = r;
            break;
        }
        case OP_BFX: {
            uint32_t v = R(x, i->rn), w = i->imm2, lsb = i->imm;
            uint32_t f = (v >> lsb) & (w == 32 ? 0xffffffffu : ((1u << w) - 1));
            if (i->sub == 1) f = (uint32_t)((int32_t)(f << (32 - w)) >> (32 - w));
            c->r[i->rd] = f;
            break;
        }
        case OP_BFI: {
            uint32_t w = i->imm2, lsb = i->imm, mask = (w == 32 ? 0xffffffffu : ((1u << w) - 1)) << lsb;
            uint32_t src = i->rn == 15 ? 0 : R(x, i->rn);
            c->r[i->rd] = (c->r[i->rd] & ~mask) | ((src << lsb) & mask);
            break;
        }
        case OP_SAT: {
            int s = 0;
            if (i->sub >= 2) { /* ssat16 / usat16 */
                uint32_t v = R(x, i->rn), r = 0;
                for (unsigned k = 0; k < 2; k++) {
                    int64_t h = (int16_t)(v >> (16 * k));
                    uint32_t q = i->sub == 2 ? (uint32_t)ssat32(h, i->imm, &s) : usat32(h, i->imm, &s);
                    r |= (q & 0xffff) << (16 * k);
                }
                if (s) c->q = 1;
                c->r[i->rd] = r;
                break;
            }
            uint32_t v = R(x, i->rn);
            unsigned dummy;
            v = i->sh == 2 ? shift_imm(2, v, i->sa, 0, &dummy) : shift_imm(0, v, i->sa, 0, &dummy);
            uint32_t r = i->sub == 0 ? (uint32_t)ssat32((int32_t)v, i->imm, &s) : usat32((int32_t)v, i->imm, &s);
            if (s) c->q = 1;
            c->r[i->rd] = r;
            break;
        }
        case OP_PKH: {
            uint32_t a = R(x, i->rn), b = R(x, i->rm), r;
            unsigned dummy;
            if (i->sub == 0) {
                uint32_t sb = shift_imm(0, b, i->sa, 0, &dummy);
                r = (sb & 0xffff0000u) | (a & 0xffff);
            } else {
                uint32_t sb = shift_imm(2, b, i->sa, 0, &dummy);
                r = (a & 0xffff0000u) | (sb & 0xffff);
            }
            c->r[i->rd] = r;
            break;
        }
        case OP_QARITH: {
            int64_t m = (int32_t)R(x, i->rm), n = (int32_t)R(x, i->rn);
            int s = 0;
            if (i->sub >= 2) { n = ssat32(n * 2, 32, &s); }
            if (s) c->q = 1;
            int64_t r = (i->sub & 1) ? m - n : m + n;
            c->r[i->rd] = (uint32_t)sat_signed64(c, r, 32);
            break;
        }
        case OP_PAR: exec_par(x, i); break;
        default: break;
    }
}

/* ---- loads and stores --------------------------------------------------------------------------------------------- */
static uint32_t ld_sub(A32Cpu *c, unsigned sub, uint32_t addr, unsigned fl) {
    uint32_t v;
    switch (sub) {
        case 1: v = ld32(c, addr); break;
        case 3: v = ld8(c, addr); break;
        case 5: v = ld16(c, addr); break;
        case 6: v = (uint32_t)(int32_t)(int8_t)ld8(c, addr); break;
        default: v = (uint32_t)(int32_t)(int16_t)ld16(c, addr); break;
    }
    if (fl & FL_ACQ) __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return v;
}

static void exec_ldst(Ctx *x, const A32Insn *i) {
    A32Cpu *c = x->c;
    uint32_t base = i->rn == 15 ? (x->pcv & ~3u) : c->r[i->rn];
    uint32_t off;
    if (i->fl & FL_REG) {
        unsigned cf;
        off = shift_imm(i->sh, R(x, i->rm), i->sa, c->c, &cf);
    } else off = i->imm;
    uint32_t off_addr = (i->fl & FL_U) ? base + off : base - off;
    uint32_t addr = (i->fl & FL_P) ? off_addr : base;
    unsigned sub = i->sub;
    int wb = (i->fl & FL_W) != 0;
    if (sub >= 8) { /* ldrd / strd */
        if (sub == 9) {
            uint32_t lo = ld32(c, addr), hi = ld32(c, addr + 4);
            if (wb && i->rn != 15) c->r[i->rn] = off_addr;
            c->r[i->rd] = lo;
            c->r[i->ra] = hi;
        } else {
            st32(c, addr, R(x, i->rd));
            st32(c, addr + 4, R(x, i->ra));
            if (wb) c->r[i->rn] = off_addr;
        }
        return;
    }
    int load = sub & 1 || sub >= 6;
    if (!load) {
        uint32_t v = R(x, i->rd);
        if (i->fl & FL_ACQ) __atomic_thread_fence(__ATOMIC_RELEASE);
        if (sub == 0) st32(c, addr, v);
        else if (sub == 2) st8(c, addr, v);
        else st16(c, addr, v);
        if (wb) c->r[i->rn] = off_addr;
        return;
    }
    uint32_t v = ld_sub(c, sub, addr, i->fl);
    if (wb && i->rn != 15) c->r[i->rn] = off_addr;
    if (i->rd == 15) {
        if (sub == 1) bx_write(x, v);
        else x->exit = A32_EXIT_UNDEF;
        return;
    }
    c->r[i->rd] = v;
}

static void exec_ldm(Ctx *x, const A32Insn *i) {
    A32Cpu *c = x->c;
    uint32_t list = i->imm, base = c->r[i->rn];
    unsigned n = popcount16(list);
    uint32_t start;
    if (i->fl & FL_U) start = (i->fl & FL_P) ? base + 4 : base;
    else start = (i->fl & FL_P) ? base - 4 * n : base - 4 * n + 4;
    uint32_t newbase = (i->fl & FL_U) ? base + 4 * n : base - 4 * n;
    uint32_t addr = start;
    if (i->sub) { /* load */
        uint32_t vals[16];
        for (unsigned r = 0; r < 16; r++)
            if ((list >> r) & 1) {
                vals[r] = ld32(c, addr);
                addr += 4;
            }
        if ((i->fl & FL_W) && !((list >> i->rn) & 1)) c->r[i->rn] = newbase;
        for (unsigned r = 0; r < 15; r++)
            if ((list >> r) & 1) c->r[r] = vals[r];
        if ((list >> 15) & 1) bx_write(x, vals[15]);
    } else {
        for (unsigned r = 0; r < 16; r++)
            if ((list >> r) & 1) {
                st32(c, addr, r == 15 ? x->pcv : c->r[r]);
                addr += 4;
            }
        if (i->fl & FL_W) c->r[i->rn] = newbase;
    }
}

static void exec_exclusive(Ctx *x, const A32Insn *i) {
    A32Cpu *c = x->c;
    unsigned size = 1u << i->sub;
    uint32_t addr = R(x, i->rn) + ((i->op == OP_LDREX || i->op == OP_STREX) ? i->imm : 0);
    /* a strex that no ldrex preceded just fails (like QEMU, which checks the monitor before the alignment) */
    if (i->op == OP_STREX && !(c->ex_valid && c->ex_addr == addr && c->ex_size == size)) {
        c->ex_valid = 0;
        c->r[i->rd] = 1;
        return;
    }
    if (addr & (size - 1)) {
        c->fault_addr = addr;
        x->exit = A32_EXIT_ALIGN;
        return;
    }
    if (i->op == OP_LDREX) {
        uint64_t v;
        switch (size) {
            case 1: v = __atomic_load_n((uint8_t *)(c->mem + addr), __ATOMIC_ACQUIRE); break;
            case 2: v = __atomic_load_n((uint16_t *)(c->mem + addr), __ATOMIC_ACQUIRE); break;
            case 4: v = __atomic_load_n((uint32_t *)(c->mem + addr), __ATOMIC_ACQUIRE); break;
            default: v = __atomic_load_n((uint64_t *)(c->mem + addr), __ATOMIC_ACQUIRE); break;
        }
        c->ex_addr = addr;
        c->ex_size = size;
        c->ex_val = v;
        c->ex_valid = 1;
        c->r[i->rd] = (uint32_t)v;
        if (size == 8) c->r[i->ra] = (uint32_t)(v >> 32);
        return;
    }
    /* strex: succeeds when the monitor still covers the address and the memory still holds the loaded value */
    uint64_t nv = R(x, i->rm);
    if (size == 8) nv |= (uint64_t)R(x, i->ra) << 32;
    int ok = 0;
    if (c->ex_valid && c->ex_addr == addr && c->ex_size == size) {
        uint64_t exp = c->ex_val;
        switch (size) {
            case 1: {
                uint8_t e = (uint8_t)exp;
                ok = __atomic_compare_exchange_n((uint8_t *)(c->mem + addr), &e, (uint8_t)nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                break;
            }
            case 2: {
                uint16_t e = (uint16_t)exp;
                ok = __atomic_compare_exchange_n((uint16_t *)(c->mem + addr), &e, (uint16_t)nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                break;
            }
            case 4: {
                uint32_t e = (uint32_t)exp;
                ok = __atomic_compare_exchange_n((uint32_t *)(c->mem + addr), &e, (uint32_t)nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                break;
            }
            default: {
                uint64_t e = exp;
                ok = __atomic_compare_exchange_n((uint64_t *)(c->mem + addr), &e, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                break;
            }
        }
    }
    c->ex_valid = 0;
    c->r[i->rd] = ok ? 0 : 1;
}

/* ---- system -------------------------------------------------------------------------------------------------------- */
static void exec_cp(Ctx *x, const A32Insn *i) {
    A32Cpu *c = x->c;
    unsigned cp = i->imm & 15, opc1 = (i->imm >> 16) & 7, crn = (i->imm >> 12) & 15, crm = (i->imm >> 8) & 15, opc2 = (i->imm >> 4) & 7;
    if (i->op == OP_MRRC) {
        unsigned op1 = (i->imm >> 4) & 15;
        if (cp == 15 && (op1 == 0 || op1 == 1) && ((i->imm >> 8) & 15) == 14 && c->hooks && c->hooks->counter) {
            uint64_t v = c->hooks->counter(c, 0);
            c->r[i->rd] = (uint32_t)v;
            c->r[i->ra] = (uint32_t)(v >> 32);
            return;
        }
        x->exit = A32_EXIT_UNIMPL;
        return;
    }
    if (cp != 15) {
        x->exit = A32_EXIT_UNDEF;
        return;
    }
    if (i->op == OP_MRC) {
        uint32_t v = 0;
        if (crn == 13 && crm == 0 && opc1 == 0 && opc2 == 3) v = c->tpidruro;
        else if (crn == 13 && crm == 0 && opc1 == 0 && opc2 == 2) v = c->tpidrurw;
        else if (crn == 14 && crm == 0 && opc1 == 0 && opc2 == 0 && c->hooks && c->hooks->counter) v = (uint32_t)c->hooks->counter(c, 1);
        else {
            x->exit = A32_EXIT_UNIMPL;
            return;
        }
        if (i->rd == 15) {
            c->n = v >> 31;
            c->z = (v >> 30) & 1;
            c->c = (v >> 29) & 1;
            c->v = (v >> 28) & 1;
        } else c->r[i->rd] = v;
        return;
    }
    uint32_t v = R(x, i->rd);
    if (crn == 13 && crm == 0 && opc1 == 0 && opc2 == 2) {
        c->tpidrurw = v;
        return;
    }
    if (crn == 7 && (crm == 10 || crm == 5 || crm == 14 || crm == 11)) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return;
    } /* cache maintenance / v6 barriers */
    x->exit = A32_EXIT_UNIMPL;
}

static void exec_branch(Ctx *x, const A32Insn *i) {
    A32Cpu *c = x->c;
    uint32_t ret = x->pc + i->len;
    switch (i->op) {
        case OP_B: x->npc = x->pcv + i->imm; break;
        case OP_BL:
            c->r[14] = ret | c->thumb;
            x->npc = x->pcv + i->imm;
            if (c->call_ring) c->call_ring[c->call_n++ & 1023] = x->npc | c->thumb;
            break;
        case OP_BLX_IMM: {
            c->r[14] = ret | c->thumb;
            uint32_t base = c->thumb ? (x->pcv & ~3u) : x->pcv;
            x->npc = base + i->imm;
            c->thumb = !c->thumb;
            if (c->call_ring) c->call_ring[c->call_n++ & 1023] = x->npc | c->thumb;
            break;
        }
        case OP_BX: bx_write(x, R(x, i->rm)); break;
        case OP_BLX_REG: {
            uint32_t t = R(x, i->rm);
            c->r[14] = ret | c->thumb;
            if (c->call_ring) c->call_ring[c->call_n++ & 1023] = t;
            bx_write(x, t);
            break;
        }
        case OP_CBZ:
            if ((R(x, i->rn) == 0) == (i->sub == 0)) x->npc = x->pcv + i->imm;
            break;
        case OP_TB: {
            uint32_t base = R(x, i->rn), idx = R(x, i->rm);
            uint32_t off = i->sub == 0 ? ld8(c, base + idx) : ld16(c, base + idx * 2);
            x->npc = x->pcv + off * 2;
            break;
        }
        default: break;
    }
}

static void exec_sys(Ctx *x, const A32Insn *i) {
    A32Cpu *c = x->c;
    switch (i->op) {
        case OP_MRS: c->r[i->rd] = a32_get_cpsr(c) & ~(1u << 5); break;
        case OP_MSR: {
            uint32_t v = (i->fl & FL_REG) && i->op == OP_MSR && (i->fl & FL_ICARRY) ? i->imm2 : R(x, i->rm);
            if (i->imm & 8) a32_set_flags(c, v);
            if (i->imm & 4) c->ge = (v >> 16) & 15;
            break;
        }
        case OP_CLREX: c->ex_valid = 0; break;
        case OP_NOP: break;
        case OP_BKPT:
            x->exit = A32_EXIT_BKPT;
            c->fault_word = i->imm;
            break;
        case OP_SVC: {
            c->r[15] = x->npc;
            if (c->hooks && c->hooks->svc && c->hooks->svc(c, i->imm)) x->exit = A32_EXIT_STOP;
            x->npc = c->r[15];
            break;
        }
        case OP_MRC:
        case OP_MCR:
        case OP_MRRC: exec_cp(x, i); break;
        default: break;
    }
}

static int exec_one(Ctx *x, const A32Insn *i) {
    switch (i->op) {
        case OP_DP: exec_dp(x, i); break;
        case OP_MOVW: x->c->r[i->rd] = i->imm & 0xffff; break;
        case OP_MOVT: x->c->r[i->rd] = (x->c->r[i->rd] & 0xffff) | (i->imm << 16); break;
        case OP_MUL:
        case OP_MULL:
        case OP_SMULH:
        case OP_SMMUL:
        case OP_SMUAD: exec_mul_group(x, i); break;
        case OP_DIV:
        case OP_CLZ:
        case OP_RBIT:
        case OP_REV:
        case OP_REV16:
        case OP_REVSH:
        case OP_SEL:
        case OP_USAD8:
        case OP_EXT:
        case OP_BFX:
        case OP_BFI:
        case OP_SAT:
        case OP_PKH:
        case OP_QARITH:
        case OP_PAR: exec_misc_alu(x, i); break;
        case OP_LDST: exec_ldst(x, i); break;
        case OP_LDM: exec_ldm(x, i); break;
        case OP_LDREX:
        case OP_STREX: exec_exclusive(x, i); break;
        case OP_B:
        case OP_BL:
        case OP_BLX_IMM:
        case OP_BX:
        case OP_BLX_REG:
        case OP_CBZ:
        case OP_TB: exec_branch(x, i); break;
        case OP_IT: x->c->itstate = i->imm; return 1;
        case OP_SVC:
        case OP_BKPT:
        case OP_NOP:
        case OP_MRS:
        case OP_MSR:
        case OP_MRC:
        case OP_MCR:
        case OP_MRRC:
        case OP_CLREX: exec_sys(x, i); break;
        case OP_VFP:
            x->exit = a32_exec_vfp(x->c, i->imm, x->pcv);
            if (x->exit == A32_EXIT_BUDGET) x->exit = A32_EXIT_BUDGET;
            break;
        case OP_NEON: x->exit = a32_exec_neon(x->c, i->imm); break;
        case OP_UNIMPL: x->exit = A32_EXIT_UNIMPL; break;
        default: x->exit = A32_EXIT_UNDEF; break;
    }
    return 0;
}

void a32_free(A32Cpu *c) {
    free(c->cache);
    c->cache = NULL;
}

/* Executes one instruction that was decoded at build time (the lifter embeds the A32Insn in the image): the semantics of an operation the lifter does not
 * inline. The condition has already been checked by the caller. Leaves the next pc in c->r[15] (and CPSR.T). No guest code is fetched or decoded here. */
A32Exit a32_op_exec(A32Cpu *c, const A32Insn *i, uint32_t pc, uint32_t raw) {
    Ctx x = {.c = c, .pc = pc, .pcv = pc + (c->thumb ? 4 : 8), .npc = pc + i->len, .exit = A32_EXIT_BUDGET};
    exec_one(&x, i);
    if (x.exit != A32_EXIT_BUDGET) {
        c->fault_pc = pc;
        c->fault_word = raw;
        if (x.exit == A32_EXIT_STOP) c->r[15] = x.npc;
        return x.exit;
    }
    c->r[15] = x.npc;
    return A32_EXIT_BUDGET;
}

#ifdef NAPPLE_A32_INTERP /* the fetch-decode-execute loop: a test oracle and a diagnostics flag, never part of the shipped runtime */

/* Decode (through the per-cpu cache) the instruction at pc. Returns the instruction or NULL when the fetch itself is not possible. */
static const A32Insn *fetch_decode(A32Cpu *c, uint32_t pc, uint32_t *raw_out) {
    CacheEntry *cache = c->cache;
    uint32_t raw, key;
    if (c->thumb) {
        uint32_t h1 = ld16(c, pc);
        raw = h1;
        key = pc | 1;
        if (a32_thumb_is32((uint16_t)h1)) raw |= ld16(c, pc + 2) << 16;
        else raw |= 0xffff0000u; /* the second halfword of a 16-bit instruction is not part of it */
    } else {
        raw = ld32(c, pc);
        key = pc;
    }
    *raw_out = raw;
    CacheEntry *e = &cache[(key >> 1) & (CACHE_SIZE - 1)];
    if (e->valid && e->key == key && e->raw == raw) return &e->insn;
    if (c->thumb) a32_decode_thumb((uint16_t)raw, (uint16_t)(raw >> 16), &e->insn);
    else a32_decode_arm(raw, &e->insn);
    e->key = key;
    e->raw = raw;
    e->valid = 1;
    return &e->insn;
}

#ifdef A32_WATCH
#include <pthread.h>
static uint32_t watch_pc, watch_off, watch_addr, watch_last;
static int watch_init;
static void watch_check(A32Cpu *c, uint32_t pc) {
    if (!watch_init) {
        watch_init = 1;
        const char *e = getenv("A32_WATCH_PC");
        if (e) watch_pc = (uint32_t)strtoul(e, 0, 16);
        e = getenv("A32_WATCH_ADDR");
        if (e) {
            watch_addr = (uint32_t)strtoul(e, 0, 16);
            watch_last = 0xdeadbeef;
        }
        e = getenv("A32_WATCH_OFF");
        watch_off = e ? (uint32_t)strtoul(e, 0, 16) : 0;
    }
    if (watch_pc && pc == watch_pc) {
        watch_addr = c->r[0] + watch_off;
        watch_last = ld32(c, watch_addr);
        fprintf(stderr, "[watch] entry pc %#x r0=%#x watch %#x = %#x (cpu %p)\n", pc, c->r[0], watch_addr, watch_last, (void *)c);
    }
    static int armed;
    if (pc == 0x50b420) armed = 1;
    if (watch_addr && armed) {
        uint32_t v = ld32(c, watch_addr);
        if (v != watch_last) {
            fprintf(stderr, "[watch] %#x: %#x -> %#x after pc %#x lr %#x (cpu %p)\n", watch_addr, watch_last, v, pc, c->r[14], (void *)c);
            watch_last = v;
        }
    }
}
#endif
A32Exit a32_run(A32Cpu *c, uint64_t budget) {
    if (!c->cache) {
        c->cache = calloc(CACHE_SIZE, sizeof(CacheEntry));
        if (!c->cache) return A32_EXIT_STOP;
    }
    Ctx x = {.c = c};
    while (budget--) {
        uint32_t pc = c->r[15];
        if (c->hle_hi != c->hle_lo && (pc & ~1u) - c->hle_lo < c->hle_hi - c->hle_lo) {
            int stop = c->hooks->hle(c, pc & ~1u);
            uint32_t lr = c->r[14];
            if (lr & 1) {
                c->thumb = 1;
                c->r[15] = lr & ~1u;
            } else {
                c->thumb = 0;
                c->r[15] = lr & ~3u;
            }
            if (stop) return A32_EXIT_STOP;
            continue;
        }
#ifdef A32_WATCH
        watch_check(c, pc);
#endif
        uint32_t raw;
        const A32Insn *i = fetch_decode(c, pc, &raw);
        unsigned it = c->thumb ? c->itstate : 0;
        x.pc = pc;
        x.pcv = pc + (c->thumb ? 4 : 8);
        x.npc = pc + i->len;
        x.in_it = it != 0;
        x.exit = A32_EXIT_BUDGET;
        unsigned cond = i->cond != 15 ? i->cond : (it ? it >> 4 : 15);
        int skip_advance = 0;
        if (cond == 15 || cond_pass(c, cond)) skip_advance = exec_one(&x, i);
        c->insns++;
        if (x.exit != A32_EXIT_BUDGET) {
            c->fault_pc = pc;
            c->fault_word = raw;
            if (x.exit == A32_EXIT_STOP) c->r[15] = x.npc;
            return x.exit;
        }
        if (it && !skip_advance) c->itstate = (it & 7) == 0 ? 0 : (it & 0xe0) | ((it << 1) & 0x1f);
        c->r[15] = x.npc;
    }
    return A32_EXIT_BUDGET;
}
#endif /* NAPPLE_A32_INTERP */
