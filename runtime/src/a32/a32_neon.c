/* Advanced SIMD (NEON) executor for AArch32: data processing and element/structure loads and stores.
 * `w` is the ARM-form encoding (Thumb-2 forms are converted by the decoder). Floating point goes through a32_neon_f32 (a32_vfp.c), which uses the
 * "standard FPSCR" (flush-to-zero, default NaN, round to nearest) as the architecture requires for Advanced SIMD. */
#include "a32_priv.h"
#include <string.h>

#define BITS(w, hi, lo) (((w) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))
#define BIT(w, b) (((w) >> (b)) & 1u)
#define FPSCR_QC (1u << 27)

typedef union {
    uint8_t u8[16];
    int8_t s8[16];
    uint16_t u16[8];
    int16_t s16[8];
    uint32_t u32[4];
    int32_t s32[4];
    uint64_t u64[2];
    int64_t s64[2];
} V;

/* from a32_vfp.c */
enum {
    NF_ADD,
    NF_SUB,
    NF_MUL,
    NF_MLA,
    NF_MLS,
    NF_FMA,
    NF_FMS,
    NF_ABD,
    NF_MAX,
    NF_MIN,
    NF_MAXNM,
    NF_MINNM,
    NF_CEQ,
    NF_CGE,
    NF_CGT,
    NF_ACGE,
    NF_ACGT,
    NF_RECPS,
    NF_RSQRTS,
    NF_RECPE,
    NF_RSQRTE,
    NF_ABS,
    NF_NEG,
    NF_CVT_S32,
    NF_CVT_U32,
    NF_CVT_F_S32,
    NF_CVT_F_U32,
    NF_CEQ0,
    NF_CGE0,
    NF_CGT0,
    NF_CLE0,
    NF_CLT0,
    NF_RINT,
    NF_CVT_RM
};
uint32_t a32_neon_f32(A32Cpu *c, int op, uint32_t a, uint32_t b, uint32_t acc, int arg);

static inline void ldv(const A32Cpu *c, V *v, unsigned reg, int q) {
    memset(v, 0, sizeof *v);
    memcpy(v, &c->v_.d[reg], q ? 16 : 8);
}
static inline void stv(A32Cpu *c, const V *v, unsigned reg, int q) { memcpy(&c->v_.d[reg], v, q ? 16 : 8); }

static inline int64_t ls(const V *v, unsigned sz, unsigned i) {
    switch (sz) {
        case 0: return v->s8[i];
        case 1: return v->s16[i];
        case 2: return v->s32[i];
        default: return v->s64[i];
    }
}
static inline uint64_t lu(const V *v, unsigned sz, unsigned i) {
    switch (sz) {
        case 0: return v->u8[i];
        case 1: return v->u16[i];
        case 2: return v->u32[i];
        default: return v->u64[i];
    }
}
static inline void sl(V *v, unsigned sz, unsigned i, uint64_t x) {
    switch (sz) {
        case 0: v->u8[i] = (uint8_t)x; break;
        case 1: v->u16[i] = (uint16_t)x; break;
        case 2: v->u32[i] = (uint32_t)x; break;
        default: v->u64[i] = x; break;
    }
}
static inline uint64_t szmask(unsigned sz) { return sz == 3 ? ~0ull : (1ull << (8u << sz)) - 1; }

static int64_t sat_s(__int128 v, unsigned bits, A32Cpu *c) {
    __int128 hi = ((__int128)1 << (bits - 1)) - 1, lo = -hi - 1;
    if (v > hi) {
        c->fpscr |= FPSCR_QC;
        return (int64_t)hi;
    }
    if (v < lo) {
        c->fpscr |= FPSCR_QC;
        return (int64_t)lo;
    }
    return (int64_t)v;
}
static uint64_t sat_u(__int128 v, unsigned bits, A32Cpu *c) {
    __int128 hi = ((__int128)1 << bits) - 1;
    if (v > hi) {
        c->fpscr |= FPSCR_QC;
        return (uint64_t)hi;
    }
    if (v < 0) {
        c->fpscr |= FPSCR_QC;
        return 0;
    }
    return (uint64_t)v;
}

#define REGS_D(w) (BIT(w, 22) << 4 | BITS(w, 15, 12))
#define REGS_N(w) (BIT(w, 7) << 4 | BITS(w, 19, 16))
#define REGS_M(w) (BIT(w, 5) << 4 | BITS(w, 3, 0))

/* shift of value `a` by the signed amount in the low byte of `b`: VSHL / VRSHL / VQSHL / VQRSHL */
static uint64_t shift_by_reg(A32Cpu *c, int64_t a_s, uint64_t a_u, unsigned sz, int sgn, int8_t sh, int round, int sat) {
    unsigned bits = 8u << sz;
    __int128 a = sgn ? (__int128)a_s : (__int128)a_u;
    __int128 r;
    if (sh >= 0) {
        if (sh >= 127) r = a ? (a < 0 ? -((__int128)1 << 100) : ((__int128)1 << 100)) : 0;
        else r = a << (sh > 100 ? 100 : sh);
        if (sh >= (int)bits + 1 && a) r = a < 0 ? -((__int128)1 << 100) : ((__int128)1 << 100);
        if (sat) return sgn ? (uint64_t)sat_s(r, bits, c) : sat_u(r, bits, c);
        return (uint64_t)r;
    }
    int rs = -sh;
    if (rs > 120) rs = 120;
    if (round) r = (a + ((__int128)1 << (rs - 1))) >> rs;
    else r = a >> rs; /* >> on a negative __int128 is arithmetic */
    if (sat) return sgn ? (uint64_t)sat_s(r, bits, c) : sat_u(r, bits, c);
    return (uint64_t)r;
}

static uint64_t clmul8(uint8_t a, uint8_t b) {
    uint16_t r = 0;
    for (unsigned k = 0; k < 8; k++)
        if ((b >> k) & 1) r ^= (uint16_t)a << k;
    return r;
}

/* ---- three registers of the same length ----------------------------------------------------------------------------- */
static A32Exit neon_3same(A32Cpu *c, uint32_t w) {
    unsigned U = BIT(w, 24), sz = BITS(w, 21, 20), B = BITS(w, 11, 8), C4 = BIT(w, 4), q = BIT(w, 6);
    unsigned d = REGS_D(w), n = REGS_N(w), m = REGS_M(w);
    if (q && ((d | n | m) & 1)) return A32_EXIT_UNDEF;
    V vn, vm, vd, r;
    ldv(c, &vn, n, q);
    ldv(c, &vm, m, q);
    ldv(c, &vd, d, q);
    memset(&r, 0, sizeof r);
    unsigned nb = q ? 16 : 8, lanes = nb >> sz;
    int fl = 0;
    switch (B) {
        case 0:
            if (sz == 3 && !C4) return A32_EXIT_UNDEF;
            for (unsigned i = 0; i < lanes; i++) {
                if (!C4) {
                    __int128 a = U ? (__int128)lu(&vn, sz, i) : (__int128)ls(&vn, sz, i), b = U ? (__int128)lu(&vm, sz, i) : (__int128)ls(&vm, sz, i);
                    sl(&r, sz, i, (uint64_t)((a + b) >> 1));
                } else if (U) {
                    __int128 s = (__int128)lu(&vn, sz, i) + (__int128)lu(&vm, sz, i);
                    sl(&r, sz, i, sat_u(s, 8u << sz, c));
                } else {
                    __int128 s = (__int128)ls(&vn, sz, i) + (__int128)ls(&vm, sz, i);
                    sl(&r, sz, i, (uint64_t)sat_s(s, 8u << sz, c));
                }
            }
            break;
        case 1:
            if (!C4) {
                if (sz == 3) return A32_EXIT_UNDEF;
                for (unsigned i = 0; i < lanes; i++) {
                    __int128 a = U ? (__int128)lu(&vn, sz, i) : (__int128)ls(&vn, sz, i), b = U ? (__int128)lu(&vm, sz, i) : (__int128)ls(&vm, sz, i);
                    sl(&r, sz, i, (uint64_t)((a + b + 1) >> 1));
                }
                break;
            }
            for (unsigned i = 0; i < 2; i++) {
                uint64_t a = vn.u64[i], b = vm.u64[i], dd = vd.u64[i], x;
                if (!U) x = sz == 0 ? a & b : sz == 1 ? a & ~b : sz == 2 ? a | b : a | ~b;
                else x = sz == 0 ? a ^ b : sz == 1 ? (dd & a) | (~dd & b) : sz == 2 ? (b & a) | (~b & dd) : (~b & a) | (b & dd);
                r.u64[i] = x;
            }
            break;
        case 2:
            if (sz == 3 && !C4) return A32_EXIT_UNDEF;
            for (unsigned i = 0; i < lanes; i++) {
                if (!C4) {
                    __int128 a = U ? (__int128)lu(&vn, sz, i) : (__int128)ls(&vn, sz, i), b = U ? (__int128)lu(&vm, sz, i) : (__int128)ls(&vm, sz, i);
                    sl(&r, sz, i, (uint64_t)((a - b) >> 1));
                } else if (U) {
                    __int128 s = (__int128)lu(&vn, sz, i) - (__int128)lu(&vm, sz, i);
                    sl(&r, sz, i, sat_u(s, 8u << sz, c));
                } else {
                    __int128 s = (__int128)ls(&vn, sz, i) - (__int128)ls(&vm, sz, i);
                    sl(&r, sz, i, (uint64_t)sat_s(s, 8u << sz, c));
                }
            }
            break;
        case 3:
            for (unsigned i = 0; i < lanes; i++) {
                int t;
                if (U) t = C4 ? lu(&vn, sz, i) >= lu(&vm, sz, i) : lu(&vn, sz, i) > lu(&vm, sz, i);
                else t = C4 ? ls(&vn, sz, i) >= ls(&vm, sz, i) : ls(&vn, sz, i) > ls(&vm, sz, i);
                sl(&r, sz, i, t ? szmask(sz) : 0);
            }
            break;
        case 4:
        case 5:
            for (unsigned i = 0; i < lanes; i++) sl(&r, sz, i, shift_by_reg(c, ls(&vm, sz, i), lu(&vm, sz, i), sz, !U, (int8_t)vn.u8[i << sz], B == 5, C4));
            break;
        case 6:
            if (sz == 3) return A32_EXIT_UNDEF;
            for (unsigned i = 0; i < lanes; i++) {
                if (U) {
                    uint64_t a = lu(&vn, sz, i), b = lu(&vm, sz, i);
                    sl(&r, sz, i, C4 ? (a < b ? a : b) : (a > b ? a : b));
                } else {
                    int64_t a = ls(&vn, sz, i), b = ls(&vm, sz, i);
                    sl(&r, sz, i, (uint64_t)(C4 ? (a < b ? a : b) : (a > b ? a : b)));
                }
            }
            break;
        case 7:
            if (sz == 3) return A32_EXIT_UNDEF;
            for (unsigned i = 0; i < lanes; i++) {
                uint64_t ad;
                if (U) {
                    uint64_t a = lu(&vn, sz, i), b = lu(&vm, sz, i);
                    ad = a > b ? a - b : b - a;
                } else {
                    int64_t a = ls(&vn, sz, i), b = ls(&vm, sz, i);
                    ad = (uint64_t)(a > b ? a - b : b - a);
                }
                sl(&r, sz, i, C4 ? lu(&vd, sz, i) + ad : ad);
            }
            break;
        case 8:
            for (unsigned i = 0; i < lanes; i++) {
                uint64_t a = lu(&vn, sz, i), b = lu(&vm, sz, i);
                if (!C4) sl(&r, sz, i, U ? a - b : a + b);
                else sl(&r, sz, i, U ? (a == b ? szmask(sz) : 0) : ((a & b) ? szmask(sz) : 0));
            }
            break;
        case 9:
            if (sz == 3) return A32_EXIT_UNDEF;
            if (C4 && U) { /* polynomial multiply */
                if (sz != 0) return A32_EXIT_UNDEF;
                for (unsigned i = 0; i < lanes; i++) r.u8[i] = (uint8_t)clmul8(vn.u8[i], vm.u8[i]);
                break;
            }
            for (unsigned i = 0; i < lanes; i++) {
                uint64_t p = lu(&vn, sz, i) * lu(&vm, sz, i);
                if (!C4) {
                    uint64_t acc = lu(&vd, sz, i);
                    p = U ? acc - p : acc + p;
                }
                sl(&r, sz, i, p);
            }
            break;
        case 10:
            if (sz == 3 || q) return A32_EXIT_UNDEF;
            {
                unsigned h = lanes / 2;
                for (unsigned i = 0; i < h; i++)
                    for (unsigned half = 0; half < 2; half++) {
                        const V *s = half ? &vm : &vn;
                        uint64_t out;
                        if (U) {
                            uint64_t a = lu(s, sz, 2 * i), b = lu(s, sz, 2 * i + 1);
                            out = C4 ? (a < b ? a : b) : (a > b ? a : b);
                        } else {
                            int64_t a = ls(s, sz, 2 * i), b = ls(s, sz, 2 * i + 1);
                            out = (uint64_t)(C4 ? (a < b ? a : b) : (a > b ? a : b));
                        }
                        sl(&r, sz, half * h + i, out);
                    }
            }
            break;
        case 11:
            if (!C4) { /* vqdmulh / vqrdmulh */
                if (sz == 0 || sz == 3) return A32_EXIT_UNDEF;
                unsigned bits = 8u << sz;
                for (unsigned i = 0; i < lanes; i++) {
                    __int128 p = (__int128)ls(&vn, sz, i) * ls(&vm, sz, i) * 2;
                    if (U) p += (__int128)1 << (bits - 1);
                    p >>= bits;
                    sl(&r, sz, i, (uint64_t)sat_s(p, bits, c));
                }
                break;
            }
            if (U || sz == 3 || q) return A32_EXIT_UNDEF; /* vpadd */
            {
                unsigned h = lanes / 2;
                for (unsigned i = 0; i < h; i++) {
                    sl(&r, sz, i, lu(&vn, sz, 2 * i) + lu(&vn, sz, 2 * i + 1));
                    sl(&r, sz, h + i, lu(&vm, sz, 2 * i) + lu(&vm, sz, 2 * i + 1));
                }
            }
            break;
        default: fl = 1; break;
    }
    if (fl) { /* floating point (single precision only) */
        if (sz & 1) return A32_EXIT_UNDEF;
        unsigned o = BIT(w, 21);
        int op = -1, pairwise = 0;
        switch (B) {
            case 12:
                if (!C4 || U) return A32_EXIT_UNDEF;
                op = o ? NF_FMS : NF_FMA;
                break;
            case 13:
                if (!C4) {
                    if (!U) op = o ? NF_SUB : NF_ADD;
                    else if (!o) {
                        op = NF_ADD;
                        pairwise = 1;
                    } else op = NF_ABD;
                } else {
                    if (!U) op = o ? NF_MLS : NF_MLA;
                    else if (!o) op = NF_MUL;
                    else return A32_EXIT_UNDEF;
                }
                break;
            case 14:
                if (!C4) {
                    if (!U) {
                        if (o) return A32_EXIT_UNDEF;
                        op = NF_CEQ;
                    } else op = o ? NF_CGT : NF_CGE;
                } else {
                    if (!U) return A32_EXIT_UNDEF;
                    op = o ? NF_ACGT : NF_ACGE;
                }
                break;
            default:
                if (!C4) {
                    if (!U) op = o ? NF_MIN : NF_MAX;
                    else {
                        op = o ? NF_MIN : NF_MAX;
                        pairwise = 1;
                    }
                } else {
                    if (!U) op = o ? NF_RSQRTS : NF_RECPS;
                    else op = o ? NF_MINNM : NF_MAXNM;
                }
                break;
        }
        if (pairwise && q) return A32_EXIT_UNDEF;
        if (pairwise) {
            r.u32[0] = a32_neon_f32(c, op, vn.u32[0], vn.u32[1], 0, 0);
            r.u32[1] = a32_neon_f32(c, op, vm.u32[0], vm.u32[1], 0, 0);
        } else {
            for (unsigned i = 0; i < lanes; i++) r.u32[i] = a32_neon_f32(c, op, vn.u32[i], vm.u32[i], vd.u32[i], 0);
        }
    }
    stv(c, &r, d, q);
    return A32_EXIT_BUDGET;
}

/* ---- three registers of different lengths ----------------------------------------------------------------------------- */
static A32Exit neon_3diff(A32Cpu *c, uint32_t w) {
    unsigned U = BIT(w, 24), sz = BITS(w, 21, 20), B = BITS(w, 11, 8);
    unsigned d = REGS_D(w), n = REGS_N(w), m = REGS_M(w);
    if (sz == 3) return A32_EXIT_UNDEF;
    if (B == 14 && !U && sz == 2) return A32_EXIT_UNIMPL; /* vmull.p64 (crypto extension) */
    V vn, vm, r, acc;
    unsigned lanes = 8u >> sz, wsz = sz + 1;
    int narrow_dst = B == 4 || B == 6;
    int wide_n = B == 1 || B == 3 || narrow_dst;
    if ((!narrow_dst && (d & 1)) || (wide_n && (n & 1))) return A32_EXIT_UNDEF;
    ldv(c, &vm, m, 0);
    if (wide_n) ldv(c, &vn, n, 1);
    else ldv(c, &vn, n, 0);
    ldv(c, &acc, d, 1);
    memset(&r, 0, sizeof r);
    if (narrow_dst) { /* vaddhn / vsubhn (+ rounding): vm is a Q register here */
        if (m & 1) return A32_EXIT_UNDEF;
        ldv(c, &vm, m, 1);
        unsigned bits = 8u << sz; /* narrow element size */
        for (unsigned i = 0; i < lanes; i++) {
            __int128 a = lu(&vn, wsz, i), b = lu(&vm, wsz, i), s = B == 4 ? a + b : a - b;
            if (U) s += (__int128)1 << (bits - 1);
            sl(&r, sz, i, (uint64_t)(s >> bits));
        }
        stv(c, &r, d, 0);
        return A32_EXIT_BUDGET;
    }
    for (unsigned i = 0; i < lanes; i++) {
        __int128 a, b;
        if (B == 1 || B == 3) a = U ? (__int128)lu(&vn, wsz, i) : (__int128)ls(&vn, wsz, i);
        else a = U ? (__int128)lu(&vn, sz, i) : (__int128)ls(&vn, sz, i);
        b = U ? (__int128)lu(&vm, sz, i) : (__int128)ls(&vm, sz, i);
        __int128 res;
        switch (B) {
            case 0:
            case 1: res = a + b; break;
            case 2:
            case 3: res = a - b; break;
            case 5: {
                __int128 ad = a > b ? a - b : b - a;
                res = (__int128)(U ? lu(&acc, wsz, i) : lu(&acc, wsz, i)) + ad;
                break;
            }
            case 7: res = a > b ? a - b : b - a; break;
            case 8: res = (__int128)lu(&acc, wsz, i) + a * b; break;
            case 10: res = (__int128)lu(&acc, wsz, i) - a * b; break;
            case 12: res = a * b; break;
            case 9:
            case 11:
            case 13: { /* vqdmlal / vqdmlsl / vqdmull */
                if (U || sz == 0) return A32_EXIT_UNDEF;
                unsigned wb = 16u << (sz - 1);
                wb = 8u << wsz;
                __int128 p = a * b * 2;
                int64_t pp = sat_s(p, wb, c);
                if (B == 13) {
                    res = pp;
                } else {
                    __int128 ac = (__int128)ls(&acc, wsz, i);
                    res = B == 9 ? ac + pp : ac - pp;
                    res = sat_s(res, wb, c);
                }
                break;
            }
            case 14: { /* vmull.p8 */
                if (U || sz != 0) return A32_EXIT_UNDEF;
                res = clmul8(vn.u8[i], vm.u8[i]);
                break;
            }
            default: return A32_EXIT_UNDEF;
        }
        if ((B == 9 || B == 11 || B == 13) && sz == 0) return A32_EXIT_UNDEF;
        sl(&r, wsz, i, (uint64_t)res);
    }
    stv(c, &r, d, 1);
    return A32_EXIT_BUDGET;
}

/* ---- two registers and a scalar --------------------------------------------------------------------------------------- */
static A32Exit neon_scalar(A32Cpu *c, uint32_t w) {
    unsigned U = BIT(w, 24), sz = BITS(w, 21, 20), B = BITS(w, 11, 8), q = U; /* bit 24 is Q for the non-long forms */
    unsigned d = REGS_D(w), n = REGS_N(w), m;
    unsigned idx, mreg;
    if (sz == 0 || sz == 3) return A32_EXIT_UNDEF;
    if (sz == 1) {
        mreg = BITS(w, 2, 0);
        idx = (BIT(w, 5) << 1) | BIT(w, 3);
    } else {
        mreg = BITS(w, 3, 0);
        idx = BIT(w, 5);
    }
    m = mreg;
    V vn, r, acc, sc;
    ldv(c, &sc, m, 0);
    uint64_t s_u = lu(&sc, sz, idx);
    int64_t s_s = ls(&sc, sz, idx);
    int longop = B == 2 || B == 3 || B == 6 || B == 7 || B == 10 || B == 11;
    memset(&r, 0, sizeof r);
    if (!longop) {
        int fp = B == 1 || B == 5 || B == 9;
        if (fp && sz != 2) return A32_EXIT_UNDEF;
        if (q && ((d | n) & 1)) return A32_EXIT_UNDEF;
        ldv(c, &vn, n, q);
        ldv(c, &acc, d, q);
        unsigned lanes = (q ? 16 : 8) >> sz;
        for (unsigned i = 0; i < lanes; i++) {
            if (fp) {
                int op = B == 1 ? NF_MLA : B == 5 ? NF_MLS : NF_MUL;
                r.u32[i] = a32_neon_f32(c, op, vn.u32[i], (uint32_t)s_u, acc.u32[i], 0);
                continue;
            }
            switch (B) {
                case 0: sl(&r, sz, i, lu(&acc, sz, i) + lu(&vn, sz, i) * s_u); break;
                case 4: sl(&r, sz, i, lu(&acc, sz, i) - lu(&vn, sz, i) * s_u); break;
                case 8: sl(&r, sz, i, lu(&vn, sz, i) * s_u); break;
                case 12:
                case 13: {
                    unsigned bits = 8u << sz;
                    __int128 p = (__int128)ls(&vn, sz, i) * s_s * 2;
                    if (B == 13) p += (__int128)1 << (bits - 1);
                    p >>= bits;
                    sl(&r, sz, i, (uint64_t)sat_s(p, bits, c));
                    break;
                }
                default: return A32_EXIT_UNDEF;
            }
        }
        stv(c, &r, d, q);
        return A32_EXIT_BUDGET;
    }
    if (d & 1) return A32_EXIT_UNDEF;
    ldv(c, &vn, n, 0);
    ldv(c, &acc, d, 1);
    unsigned lanes = 8u >> sz, wsz = sz + 1, wb = 8u << wsz;
    for (unsigned i = 0; i < lanes; i++) {
        __int128 a = U ? (__int128)lu(&vn, sz, i) : (__int128)ls(&vn, sz, i), b = U ? (__int128)s_u : (__int128)s_s, res;
        switch (B) {
            case 2: res = (__int128)lu(&acc, wsz, i) + a * b; break;
            case 6: res = (__int128)lu(&acc, wsz, i) - a * b; break;
            case 10: res = a * b; break;
            case 3:
            case 7:
            case 11: {
                if (U) return A32_EXIT_UNDEF;
                int64_t pp = sat_s(a * b * 2, wb, c);
                if (B == 11) res = pp;
                else {
                    __int128 ac = ls(&acc, wsz, i);
                    res = sat_s(B == 3 ? ac + pp : ac - pp, wb, c);
                }
                break;
            }
            default: return A32_EXIT_UNDEF;
        }
        sl(&r, wsz, i, (uint64_t)res);
    }
    stv(c, &r, d, 1);
    return A32_EXIT_BUDGET;
}

/* ---- two registers and a shift amount; one register and a modified immediate ------------------------------------------- */
static uint64_t expand_imm(unsigned op, unsigned cmode, unsigned imm8, int *ok) {
    uint64_t imm64 = 0, v;
    *ok = 1;
    switch (cmode >> 1) {
        case 0:
            v = imm8;
            imm64 = v | v << 32;
            break;
        case 1:
            v = (uint64_t)imm8 << 8;
            imm64 = v | v << 32;
            break;
        case 2:
            v = (uint64_t)imm8 << 16;
            imm64 = v | v << 32;
            break;
        case 3:
            v = (uint64_t)imm8 << 24;
            imm64 = v | v << 32;
            break;
        case 4:
            v = imm8 | (uint64_t)imm8 << 16;
            imm64 = v | v << 32;
            break;
        case 5:
            v = (uint64_t)imm8 << 8 | (uint64_t)imm8 << 24;
            imm64 = v | v << 32;
            break;
        case 6:
            v = (cmode & 1) ? ((uint64_t)imm8 << 8 | 0xff) : ((uint64_t)imm8 << 16 | 0xffff);
            if (!(cmode & 1)) v = (uint64_t)imm8 << 8 | 0xff;
            else v = (uint64_t)imm8 << 16 | 0xffff;
            imm64 = v | v << 32;
            break;
        default:
            if (!(cmode & 1)) {
                if (!op) {
                    for (unsigned k = 0; k < 8; k++) imm64 |= (uint64_t)imm8 << (8 * k);
                } else {
                    for (unsigned k = 0; k < 8; k++)
                        if ((imm8 >> k) & 1) imm64 |= 0xffull << (8 * k);
                }
            } else {
                if (op) {
                    *ok = 0;
                    return 0;
                }
                uint32_t b6 = (imm8 >> 6) & 1;
                v = ((uint64_t)(imm8 >> 7) << 31) | ((uint64_t)(b6 ^ 1) << 30) | (b6 ? (uint64_t)0x1f << 25 : 0) | ((uint64_t)(imm8 & 0x3f) << 19);
                imm64 = v | v << 32;
            }
            break;
    }
    return imm64;
}

static A32Exit neon_modimm(A32Cpu *c, uint32_t w) {
    unsigned q = BIT(w, 6), op = BIT(w, 5), cmode = BITS(w, 11, 8);
    unsigned imm8 = (BIT(w, 24) << 7) | (BITS(w, 18, 16) << 4) | BITS(w, 3, 0);
    unsigned d = REGS_D(w);
    if (q && (d & 1)) return A32_EXIT_UNDEF;
    int ok;
    uint64_t imm = expand_imm(op, cmode, imm8, &ok);
    if (!ok) return A32_EXIT_UNDEF;
    V v;
    ldv(c, &v, d, q);
    int orr_bic = (cmode & 1) && (cmode >> 2) != 3; /* cmode 0xx1 and 10x1 */
    for (unsigned i = 0; i < (q ? 2u : 1u); i++) {
        if (orr_bic) v.u64[i] = op ? v.u64[i] & ~imm : v.u64[i] | imm;
        else v.u64[i] = op && cmode != 14 ? ~imm : imm;
    }
    stv(c, &v, d, q);
    return A32_EXIT_BUDGET;
}

static A32Exit neon_shift(A32Cpu *c, uint32_t w) {
    unsigned U = BIT(w, 24), L = BIT(w, 7), q = BIT(w, 6), op = BITS(w, 11, 8), imm6 = BITS(w, 21, 16);
    unsigned d = REGS_D(w), m = REGS_M(w);
    unsigned imm7 = L << 6 | imm6;
    unsigned sz;
    if (imm7 >= 64) sz = 3;
    else if (imm7 >= 32) sz = 2;
    else if (imm7 >= 16) sz = 1;
    else if (imm7 >= 8) sz = 0;
    else sz = 4;
    if (op == 14 || op == 15) { /* vcvt between float and fixed point */
        if (BITS(w, 21, 19) == 0 || BIT(w, 7)) return A32_EXIT_UNDEF;
        if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
        unsigned fbits = 64 - imm6;
        V vm, r;
        ldv(c, &vm, m, q);
        memset(&r, 0, sizeof r);
        for (unsigned i = 0; i < (q ? 4u : 2u); i++) {
            if (op == 14) {
                uint32_t f = a32_neon_f32(c, U ? NF_CVT_F_U32 : NF_CVT_F_S32, vm.u32[i], 0, 0, (int)fbits);
                r.u32[i] = f;
            } else r.u32[i] = a32_neon_f32(c, U ? NF_CVT_U32 : NF_CVT_S32, vm.u32[i], 0, 0, (int)fbits);
        }
        stv(c, &r, d, q);
        return A32_EXIT_BUDGET;
    }
    if (op >= 8 && op <= 10 && op != 8 + 0 && op != 9 && op != 10) return A32_EXIT_UNDEF;
    if (op == 8 || op == 9 || (op == 10 && !L)) { /* narrowing right shifts and vshll */
        if (op == 10) {                           /* vshll / vmovl: source D elements widen to Q */
            if (q || (d & 1)) return A32_EXIT_UNDEF;
            if (sz > 2) return A32_EXIT_UNDEF;
            unsigned shift = imm7 - (8u << sz);
            V vm, r;
            ldv(c, &vm, m, 0);
            memset(&r, 0, sizeof r);
            for (unsigned i = 0; i < (8u >> sz); i++) {
                __int128 x = U ? (__int128)lu(&vm, sz, i) : (__int128)ls(&vm, sz, i);
                sl(&r, sz + 1, i, (uint64_t)(x << shift));
            }
            stv(c, &r, d, 1);
            return A32_EXIT_BUDGET;
        }
        if (L || (m & 1) || sz > 2 || (op == 9 && 0)) return A32_EXIT_UNDEF;
        unsigned shift = 2 * (8u << sz) - imm7;
        V vm, r;
        ldv(c, &vm, m, 1);
        memset(&r, 0, sizeof r);
        unsigned wsz = sz + 1, bits = 8u << sz;
        int round = op == 8 ? BIT(w, 6) : BIT(w, 6); /* bit 6 selects the rounding forms */
        for (unsigned i = 0; i < (8u >> sz); i++) {
            int src_signed = op == 8 ? (U ? 0 : 1) : (U ? 0 : 1);
            __int128 x = src_signed ? (__int128)ls(&vm, wsz, i) : (__int128)lu(&vm, wsz, i);
            if (op == 8 && !U) { /* vshrn / vrshrn: plain narrowing */
                if (round) x += (__int128)1 << (shift - 1);
                sl(&r, sz, i, (uint64_t)(x >> shift));
            } else if (op == 8 && U) { /* vqshrun / vqrshrun: signed source, unsigned saturated result */
                x = (__int128)ls(&vm, wsz, i);
                if (round) x += (__int128)1 << (shift - 1);
                sl(&r, sz, i, sat_u(x >> shift, bits, c));
            } else { /* op 9: vqshrn / vqrshrn */
                x = U ? (__int128)lu(&vm, wsz, i) : (__int128)ls(&vm, wsz, i);
                if (round) x += (__int128)1 << (shift - 1);
                x >>= shift;
                sl(&r, sz, i, U ? sat_u(x, bits, c) : (uint64_t)sat_s(x, bits, c));
            }
        }
        if ((d & 1) && 0) return A32_EXIT_UNDEF;
        stv(c, &r, d, 0);
        return A32_EXIT_BUDGET;
    }
    if (sz > 3) return A32_EXIT_UNDEF;
    if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
    unsigned esize = 8u << sz;
    unsigned rshift = 2 * esize - imm7, lshift = imm7 - esize;
    V vm, vd, r;
    ldv(c, &vm, m, q);
    ldv(c, &vd, d, q);
    memset(&r, 0, sizeof r);
    unsigned lanes = (q ? 16u : 8u) >> sz;
    for (unsigned i = 0; i < lanes; i++) {
        __int128 x = U ? (__int128)lu(&vm, sz, i) : (__int128)ls(&vm, sz, i);
        uint64_t out;
        switch (op) {
            case 0: out = (uint64_t)(x >> rshift); break;                                                    /* vshr */
            case 1: out = lu(&vd, sz, i) + (uint64_t)(x >> rshift); break;                                   /* vsra */
            case 2: out = (uint64_t)((x + ((__int128)1 << (rshift - 1))) >> rshift); break;                  /* vrshr */
            case 3: out = lu(&vd, sz, i) + (uint64_t)((x + ((__int128)1 << (rshift - 1))) >> rshift); break; /* vrsra */
            case 4: {                                                                                        /* vsri (U = 1) */
                if (!U) return A32_EXIT_UNDEF;
                uint64_t mask = rshift >= esize ? 0 : szmask(sz) >> rshift;
                out = (lu(&vd, sz, i) & ~mask) | ((lu(&vm, sz, i) >> (rshift >= esize ? 0 : rshift)) & mask);
                if (rshift >= esize) out = lu(&vd, sz, i);
                break;
            }
            case 5:
                if (!U) out = lu(&vm, sz, i) << lshift; /* vshl */
                else {
                    uint64_t mask = (szmask(sz) << lshift) & szmask(sz);
                    out = (lu(&vd, sz, i) & ~mask) | ((lu(&vm, sz, i) << lshift) & mask);
                } /* vsli */
                break;
            case 6: /* vqshlu (U = 1): signed source, unsigned saturated */
                if (!U) return A32_EXIT_UNDEF;
                out = sat_u((__int128)ls(&vm, sz, i) << lshift, esize, c);
                break;
            case 7: /* vqshl immediate */ out = U ? sat_u(x << lshift, esize, c) : (uint64_t)sat_s(x << lshift, esize, c); break;
            default: return A32_EXIT_UNDEF;
        }
        sl(&r, sz, i, out);
    }
    if (op == 0 || op == 2) { /* result already shifted */
    }
    stv(c, &r, d, q);
    return A32_EXIT_BUDGET;
}

/* ---- two registers, miscellaneous ----------------------------------------------------------------------------------------- */
static void zip_unzip(V *a, V *b, unsigned sz, unsigned lanes, int kind) { /* kind 1 vtrn 2 vuzp 3 vzip; a = Vd, b = Vm; both updated */
    V ra = *a, rb = *b;
    if (kind == 1) {
        for (unsigned i = 0; i < lanes; i += 2) {
            sl(&ra, sz, i + 1, lu(b, sz, i));
            sl(&rb, sz, i, lu(a, sz, i + 1));
        }
    } else if (kind == 2) {
        for (unsigned i = 0; i < lanes / 2; i++) {
            sl(&ra, sz, i, lu(a, sz, 2 * i));
            sl(&ra, sz, lanes / 2 + i, lu(b, sz, 2 * i));
            sl(&rb, sz, i, lu(a, sz, 2 * i + 1));
            sl(&rb, sz, lanes / 2 + i, lu(b, sz, 2 * i + 1));
        }
    } else {
        for (unsigned i = 0; i < lanes / 2; i++) {
            sl(&ra, sz, 2 * i, lu(a, sz, i));
            sl(&ra, sz, 2 * i + 1, lu(b, sz, i));
            sl(&rb, sz, 2 * i, lu(a, sz, lanes / 2 + i));
            sl(&rb, sz, 2 * i + 1, lu(b, sz, lanes / 2 + i));
        }
    }
    *a = ra;
    *b = rb;
}

static A32Exit neon_misc(A32Cpu *c, uint32_t w) {
    unsigned sz = BITS(w, 19, 18), A = BITS(w, 17, 16), op = BITS(w, 10, 7), q = BIT(w, 6);
    unsigned d = REGS_D(w), m = REGS_M(w);
    V vm, vd, r;
    memset(&r, 0, sizeof r);
    if (A == 0) {
        switch (op) {
            case 0:
            case 1:
            case 2: {                  /* vrev64 / vrev32 / vrev16 */
                unsigned grp = 3 - op; /* log2 of the group size in bytes */
                if (sz + op >= 3 + 0 && sz >= 3 - op + 0 && sz > 2 - op) return A32_EXIT_UNDEF;
                if (sz >= grp) return A32_EXIT_UNDEF;
                if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
                ldv(c, &vm, m, q);
                unsigned per = (1u << grp) >> sz, lanes = (q ? 16u : 8u) >> sz;
                for (unsigned i = 0; i < lanes; i++) {
                    unsigned base = i - i % per;
                    sl(&r, sz, base + (per - 1 - i % per), lu(&vm, sz, i));
                }
                stv(c, &r, d, q);
                return A32_EXIT_BUDGET;
            }
            case 4:
            case 5:
            case 12:
            case 13: { /* vpaddl / vpadal */
                if (sz == 3) return A32_EXIT_UNDEF;
                if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
                int u = op & 1, acc = op >= 12;
                ldv(c, &vm, m, q);
                ldv(c, &vd, d, q);
                unsigned lanes = (q ? 16u : 8u) >> (sz + 1);
                for (unsigned i = 0; i < lanes; i++) {
                    __int128 s = u ? (__int128)lu(&vm, sz, 2 * i) + lu(&vm, sz, 2 * i + 1) : (__int128)ls(&vm, sz, 2 * i) + ls(&vm, sz, 2 * i + 1);
                    uint64_t x = (uint64_t)s;
                    if (acc) x += lu(&vd, sz + 1, i);
                    sl(&r, sz + 1, i, x);
                }
                stv(c, &r, d, q);
                return A32_EXIT_BUDGET;
            }
            case 8:
            case 9: { /* vcls / vclz */
                if (sz == 3) return A32_EXIT_UNDEF;
                if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
                ldv(c, &vm, m, q);
                unsigned bits = 8u << sz, lanes = (q ? 16u : 8u) >> sz;
                for (unsigned i = 0; i < lanes; i++) {
                    uint64_t x = lu(&vm, sz, i);
                    unsigned cnt = 0;
                    if (op == 9) {
                        while (cnt < bits && !((x >> (bits - 1 - cnt)) & 1)) cnt++;
                    } else {
                        unsigned top = (x >> (bits - 1)) & 1;
                        while (cnt < bits - 1 && (((x >> (bits - 2 - cnt)) & 1) == top)) cnt++;
                    }
                    sl(&r, sz, i, cnt);
                }
                stv(c, &r, d, q);
                return A32_EXIT_BUDGET;
            }
            case 10: /* vcnt */
                if (sz != 0) return A32_EXIT_UNDEF;
                if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
                ldv(c, &vm, m, q);
                for (unsigned i = 0; i < (q ? 16u : 8u); i++) r.u8[i] = (uint8_t)__builtin_popcount(vm.u8[i]);
                stv(c, &r, d, q);
                return A32_EXIT_BUDGET;
            case 11: /* vmvn */
                if (sz != 0) return A32_EXIT_UNDEF;
                if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
                ldv(c, &vm, m, q);
                r.u64[0] = ~vm.u64[0];
                r.u64[1] = ~vm.u64[1];
                stv(c, &r, d, q);
                return A32_EXIT_BUDGET;
            case 14:
            case 15: { /* vqabs / vqneg */
                if (sz == 3) return A32_EXIT_UNDEF;
                if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
                ldv(c, &vm, m, q);
                unsigned bits = 8u << sz, lanes = (q ? 16u : 8u) >> sz;
                for (unsigned i = 0; i < lanes; i++) {
                    __int128 x = ls(&vm, sz, i);
                    if (op == 14 && x < 0) x = -x;
                    else if (op == 15) x = -x;
                    sl(&r, sz, i, (uint64_t)sat_s(x, bits, c));
                }
                stv(c, &r, d, q);
                return A32_EXIT_BUDGET;
            }
            default: return A32_EXIT_UNDEF;
        }
    }
    if (A == 1) { /* compares with zero, abs, neg */
        int fp = op >= 8;
        if (fp && sz != 2) return A32_EXIT_UNDEF;
        if (!fp && sz == 3) return A32_EXIT_UNDEF;
        unsigned o = op & 7;
        if (o == 5) return A32_EXIT_UNDEF;
        if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
        ldv(c, &vm, m, q);
        unsigned lanes = (q ? 16u : 8u) >> sz;
        for (unsigned i = 0; i < lanes; i++) {
            if (fp) {
                static const int nf[8] = {NF_CGT0, NF_CGE0, NF_CEQ0, NF_CLE0, NF_CLT0, -1, NF_ABS, NF_NEG};
                r.u32[i] = a32_neon_f32(c, nf[o], vm.u32[i], 0, 0, 0);
                continue;
            }
            int64_t x = ls(&vm, sz, i);
            uint64_t out;
            switch (o) {
                case 0: out = x > 0 ? szmask(sz) : 0; break;
                case 1: out = x >= 0 ? szmask(sz) : 0; break;
                case 2: out = x == 0 ? szmask(sz) : 0; break;
                case 3: out = x <= 0 ? szmask(sz) : 0; break;
                case 4: out = x < 0 ? szmask(sz) : 0; break;
                case 6: out = (uint64_t)(x < 0 ? -x : x); break;
                default: out = (uint64_t)(-x); break;
            }
            sl(&r, sz, i, out);
        }
        stv(c, &r, d, q);
        return A32_EXIT_BUDGET;
    }
    if (A == 2) {
        switch (op) {
            case 0: /* vswp */
                if (sz != 0) return A32_EXIT_UNDEF;
                if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
                ldv(c, &vm, m, q);
                ldv(c, &vd, d, q);
                stv(c, &vm, d, q);
                stv(c, &vd, m, q);
                return A32_EXIT_BUDGET;
            case 1:
            case 2:
            case 3: { /* vtrn / vuzp / vzip */
                if (sz == 3 || (q && ((d | m) & 1))) return A32_EXIT_UNDEF;
                if (!q && sz == 2 && op != 1) return A32_EXIT_UNDEF; /* vuzp.32 / vzip.32 on D registers is undefined */
                if (d == m) return A32_EXIT_UNDEF;
                ldv(c, &vm, m, q);
                ldv(c, &vd, d, q);
                zip_unzip(&vd, &vm, sz, (q ? 16u : 8u) >> sz, op);
                stv(c, &vd, d, q);
                stv(c, &vm, m, q);
                return A32_EXIT_BUDGET;
            }
            case 4:
            case 5: { /* vmovn / vqmovun / vqmovn */
                if (sz == 3 || (m & 1)) return A32_EXIT_UNDEF;
                ldv(c, &vm, m, 1);
                unsigned wsz = sz + 1, bits = 8u << sz, lanes = 8u >> sz;
                for (unsigned i = 0; i < lanes; i++) {
                    uint64_t out;
                    if (op == 4 && !q) out = lu(&vm, wsz, i);
                    else if (op == 4) out = sat_u(ls(&vm, wsz, i), bits, c);
                    else if (!q) out = (uint64_t)sat_s(ls(&vm, wsz, i), bits, c);
                    else out = sat_u((__int128)lu(&vm, wsz, i), bits, c);
                    sl(&r, sz, i, out);
                }
                stv(c, &r, d, 0);
                return A32_EXIT_BUDGET;
            }
            case 6: { /* vshll with the maximum shift */
                if (q || sz == 3 || (d & 1)) return A32_EXIT_UNDEF;
                ldv(c, &vm, m, 0);
                unsigned lanes = 8u >> sz, shift = 8u << sz;
                for (unsigned i = 0; i < lanes; i++) sl(&r, sz + 1, i, lu(&vm, sz, i) << shift);
                stv(c, &r, d, 1);
                return A32_EXIT_BUDGET;
            }
            case 8:
            case 9:
            case 10:
            case 11:
            case 13:
            case 15: { /* vrint */
                if (sz != 2) return A32_EXIT_UNDEF;
                if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
                static const int mode[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                             0, 5, 4, 3, 0, 2, 0, 1}; /* n x a z m p: nearest-even, current(x), away, zero, -inf, +inf */
                ldv(c, &vm, m, q);
                for (unsigned i = 0; i < (q ? 4u : 2u); i++) r.u32[i] = a32_neon_f32(c, NF_RINT, vm.u32[i], 0, 0, mode[op] | (op == 9 ? 0x100 : 0));
                stv(c, &r, d, q);
                return A32_EXIT_BUDGET;
            }
            case 12:
            case 14: { /* vcvt f16 <-> f32 */
                if (sz != 1) return A32_EXIT_UNDEF;
                if (op == 12) { /* f32 -> f16: Qm -> Dd */
                    if (m & 1) return A32_EXIT_UNDEF;
                    ldv(c, &vm, m, 1);
                    for (unsigned i = 0; i < 4; i++) r.u16[i] = (uint16_t)a32_neon_f32(c, NF_CVT_RM, vm.u32[i], 0, 0, 0x200);
                    stv(c, &r, d, 0);
                } else { /* f16 -> f32: Dm -> Qd */
                    if (d & 1) return A32_EXIT_UNDEF;
                    ldv(c, &vm, m, 0);
                    for (unsigned i = 0; i < 4; i++) r.u32[i] = a32_neon_f32(c, NF_CVT_RM, vm.u16[i], 0, 0, 0x300);
                    stv(c, &r, d, 1);
                }
                return A32_EXIT_BUDGET;
            }
            default: return A32_EXIT_UNDEF;
        }
    }
    /* A == 3 */
    if (sz != 2) return A32_EXIT_UNDEF;
    if (q && ((d | m) & 1)) return A32_EXIT_UNDEF;
    ldv(c, &vm, m, q);
    unsigned lanes = q ? 4 : 2;
    if (op < 8) { /* vcvta / vcvtn / vcvtp / vcvtm */
        static const int rmode[4] = {4, 0, 1, 2};
        for (unsigned i = 0; i < lanes; i++) r.u32[i] = a32_neon_f32(c, (op & 1) ? NF_CVT_U32 : NF_CVT_S32, vm.u32[i], 0, 0, 0x1000 | rmode[op >> 1]);
    } else if (op == 8 || op == 9 || op == 10 || op == 11) { /* vrecpe / vrsqrte */
        int fp = op >= 10;
        for (unsigned i = 0; i < lanes; i++) {
            if (fp) r.u32[i] = a32_neon_f32(c, op == 10 ? NF_RECPE : NF_RSQRTE, vm.u32[i], 0, 0, 0);
            else r.u32[i] = a32_neon_f32(c, op == 8 ? NF_RECPE : NF_RSQRTE, vm.u32[i], 0, 0, 1);
        }
    } else { /* vcvt between float and integer */
        static const int nf[4] = {NF_CVT_F_S32, NF_CVT_F_U32, NF_CVT_S32, NF_CVT_U32};
        for (unsigned i = 0; i < lanes; i++) r.u32[i] = a32_neon_f32(c, nf[op - 12], vm.u32[i], 0, 0, 0);
    }
    stv(c, &r, d, q);
    return A32_EXIT_BUDGET;
}

/* ---- vext, vtbl / vtbx, vdup (scalar) ---------------------------------------------------------------------------------------- */
static A32Exit neon_ext_tbl_dup(A32Cpu *c, uint32_t w) {
    unsigned d = REGS_D(w), n = REGS_N(w), m = REGS_M(w), q = BIT(w, 6);
    if (!BIT(w, 24)) { /* vext */
        unsigned imm4 = BITS(w, 11, 8);
        if (q && ((d | n | m) & 1)) return A32_EXIT_UNDEF;
        if (!q && imm4 > 7) return A32_EXIT_UNDEF;
        V vn, vm, r;
        ldv(c, &vn, n, q);
        ldv(c, &vm, m, q);
        memset(&r, 0, sizeof r);
        uint8_t cat[32];
        unsigned nb = q ? 16 : 8;
        memcpy(cat, vn.u8, nb);
        memcpy(cat + nb, vm.u8, nb);
        memcpy(r.u8, cat + imm4, nb);
        stv(c, &r, d, q);
        return A32_EXIT_BUDGET;
    }
    if (BITS(w, 11, 10) == 2) { /* vtbl / vtbx */
        unsigned len = BITS(w, 9, 8), ext = BIT(w, 6);
        if (n + len + 1 > 32) return A32_EXIT_UNDEF;
        V vm, vd, r;
        ldv(c, &vm, m, 0);
        ldv(c, &vd, d, 0);
        uint8_t table[32];
        memcpy(table, &c->v_.d[n], 8 * (len + 1) > 32 ? 32 : 8 * (len + 1));
        for (unsigned i = 0; i < 8; i++) {
            unsigned idx = vm.u8[i];
            r.u8[i] = idx < 8 * (len + 1) ? table[idx] : (ext ? vd.u8[i] : 0);
        }
        stv(c, &r, d, 0);
        return A32_EXIT_BUDGET;
    }
    if (BITS(w, 11, 7) == 0x18) { /* vdup scalar */
        unsigned imm4 = BITS(w, 19, 16), sz, idx;
        if (imm4 & 1) {
            sz = 0;
            idx = imm4 >> 1;
        } else if (imm4 & 2) {
            sz = 1;
            idx = imm4 >> 2;
        } else if (imm4 & 4) {
            sz = 2;
            idx = imm4 >> 3;
        } else return A32_EXIT_UNDEF;
        if (q && (d & 1)) return A32_EXIT_UNDEF;
        V vm, r;
        ldv(c, &vm, m, 0);
        memset(&r, 0, sizeof r);
        uint64_t x = lu(&vm, sz, idx);
        for (unsigned i = 0; i < (q ? 16u : 8u) >> sz; i++) sl(&r, sz, i, x);
        stv(c, &r, d, q);
        return A32_EXIT_BUDGET;
    }
    return A32_EXIT_UNDEF;
}

/* ---- element and structure loads and stores ---------------------------------------------------------------------------------- */
static inline uint32_t gaddr(const A32Cpu *c, unsigned rn) { return c->r[rn]; }

/* VLDn / VSTn: multiple structures, single lane, all lanes (ARM ARM A8.8.321 ff.). Alignment hints are ignored (the guest memory has no alignment faults). */
static A32Exit neon_ldst(A32Cpu *c, uint32_t w) {
    unsigned L = BIT(w, 21), rn = BITS(w, 19, 16), rm = w & 15, d = REGS_D(w);
    uint32_t addr = gaddr(c, rn), start = addr;
    unsigned bytes = 0;
    if (!BIT(w, 23)) { /* multiple structures */
        unsigned type = BITS(w, 11, 8), size = BITS(w, 7, 6);
        unsigned regs, inc, n;
        switch (type) {
            case 7:
                regs = 1;
                inc = 1;
                n = 1;
                break;
            case 10:
                regs = 2;
                inc = 1;
                n = 1;
                break;
            case 6:
                regs = 3;
                inc = 1;
                n = 1;
                break;
            case 2:
                regs = 4;
                inc = 1;
                n = 1;
                break;
            case 8:
                regs = 1;
                inc = 1;
                n = 2;
                break;
            case 9:
                regs = 1;
                inc = 2;
                n = 2;
                break;
            case 3:
                regs = 2;
                inc = 2;
                n = 2;
                break;
            case 0:
                regs = 1;
                inc = 1;
                n = 4;
                break;
            case 1:
                regs = 1;
                inc = 2;
                n = 4;
                break;
            case 4:
                regs = 1;
                inc = 1;
                n = 3;
                break;
            case 5:
                regs = 1;
                inc = 2;
                n = 3;
                break;
            default: return A32_EXIT_UNDEF;
        }
        if (n > 1 && size == 3) return A32_EXIT_UNDEF;
        unsigned ebytes = 1u << size, elems = 8u / ebytes;
        if (d + (n - 1) * inc + regs - 1 > 31) return A32_EXIT_UNDEF;
        for (unsigned r = 0; r < regs; r++)
            for (unsigned e = 0; e < elems; e++)
                for (unsigned i = 0; i < n; i++) {
                    uint8_t *reg = (uint8_t *)&c->v_.d[d + i * inc + r] + e * ebytes;
                    if (L) memcpy(reg, c->mem + addr, ebytes);
                    else memcpy(c->mem + addr, reg, ebytes);
                    addr += ebytes;
                }
        bytes = addr - start;
    } else {
        unsigned size = BITS(w, 11, 10), n = BITS(w, 9, 8) + 1;
        if (size == 3) { /* all lanes: load one structure and replicate (VLDn only) */
            if (!L) return A32_EXIT_UNDEF;
            unsigned T = BIT(w, 5), esz = BITS(w, 7, 6), ebytes = 1u << esz;
            unsigned regs = n == 1 ? T + 1 : 1, inc = n == 1 ? 1 : T + 1;
            if (n == 1 && esz == 3) return A32_EXIT_UNDEF;
            if (n > 1 && esz == 3 && n != 4) return A32_EXIT_UNDEF;
            if (d + (n - 1) * inc + regs - 1 > 31) return A32_EXIT_UNDEF;
            if (esz == 3) ebytes = 8;
            for (unsigned i = 0; i < n; i++) {
                uint64_t v = 0;
                memcpy(&v, c->mem + addr, ebytes);
                addr += ebytes;
                for (unsigned r = 0; r < regs; r++) {
                    uint8_t *reg = (uint8_t *)&c->v_.d[d + i * inc + r];
                    for (unsigned e = 0; e < 8u / ebytes; e++) memcpy(reg + e * ebytes, &v, ebytes);
                }
            }
            bytes = addr - start;
        } else { /* single lane */
            unsigned ia = BITS(w, 7, 4), index, inc;
            if (size == 0) {
                index = ia >> 1 & 7;
                inc = 1;
            } else if (size == 1) {
                index = ia >> 2 & 3;
                inc = (ia & 2) ? 2 : 1;
            } else {
                index = ia >> 3 & 1;
                inc = (ia & 4) ? 2 : 1;
            }
            unsigned ebytes = 1u << size;
            if (d + (n - 1) * inc > 31) return A32_EXIT_UNDEF;
            for (unsigned i = 0; i < n; i++) {
                uint8_t *reg = (uint8_t *)&c->v_.d[d + i * inc] + index * ebytes;
                if (L) memcpy(reg, c->mem + addr, ebytes);
                else memcpy(c->mem + addr, reg, ebytes);
                addr += ebytes;
            }
            bytes = addr - start;
        }
    }
    if (rm != 15) c->r[rn] = rm == 13 ? start + bytes : start + c->r[rm];
    return A32_EXIT_BUDGET;
}

A32Exit a32_exec_neon(A32Cpu *c, uint32_t w) {
    if ((w & 0xff000000) == 0xf4000000) return neon_ldst(c, w);
    if ((w & 0xfe000000) != 0xf2000000) return A32_EXIT_UNDEF;
    unsigned size = BITS(w, 21, 20);
    if (!BIT(w, 23)) return neon_3same(c, w);
    if (BIT(w, 4)) {
        if (BITS(w, 21, 19) == 0 && !BIT(w, 7)) return neon_modimm(c, w);
        return neon_shift(c, w);
    }
    if (size != 3) return BIT(w, 6) ? neon_scalar(c, w) : neon_3diff(c, w);
    if (!BIT(w, 24)) return neon_ext_tbl_dup(c, w);
    if (!BIT(w, 11)) return neon_misc(c, w);
    return neon_ext_tbl_dup(c, w);
}
