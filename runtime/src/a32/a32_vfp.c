/* VFP executor (VFPv4 plus the ARMv8 additions: vsel, vmaxnm/vminnm, vrint*, vcvt{a,n,p,m}, half precision conversions).
 * Register transfers between the core and the NEON scalar/duplicate forms that live in the coprocessor 10/11 space are here too.
 * Arithmetic runs on the host FPU with the guest's rounding mode; see a32_vfp_t.h for the ARM-specific parts. */
#include "a32_priv.h"
#include <fenv.h>
#include <math.h>
#include <string.h>

#define BITS(w, hi, lo) (((w) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))
#define BIT(w, b) (((w) >> (b)) & 1u)

enum {
    FPSCR_IOC = 1u << 0,
    FPSCR_DZC = 1u << 1,
    FPSCR_OFC = 1u << 2,
    FPSCR_UFC = 1u << 3,
    FPSCR_IXC = 1u << 4,
    FPSCR_IDC = 1u << 7,
    FPSCR_DN = 1u << 25,
    FPSCR_FZ = 1u << 24,
    FPSCR_AHP = 1u << 26,
};

/* host helpers by precision */
static inline float sqrt_hf(float x) { return sqrtf(x); }
static inline double sqrt_hd(double x) { return sqrt(x); }
static inline float fma_hf(float a, float b, float c) { return fmaf(a, b, c); }
static inline double fma_hd(double a, double b, double c) { return fma(a, b, c); }
static inline float ceil_hf(float x) { return ceilf(x); }
static inline double ceil_hd(double x) { return ceil(x); }
static inline float floor_hf(float x) { return floorf(x); }
static inline double floor_hd(double x) { return floor(x); }
static inline float trunc_hf(float x) { return truncf(x); }
static inline double trunc_hd(double x) { return trunc(x); }
static inline float round_hf(float x) { return roundf(x); }
static inline double round_hd(double x) { return round(x); }
static inline float rint_nf(float x) {
    float r = roundf(x);
    if (fabsf(x - truncf(x)) == 0.5f) r = 2.0f * roundf(x * 0.5f);
    return r;
}
static inline double rint_nd(double x) {
    double r = round(x);
    if (fabs(x - trunc(x)) == 0.5) r = 2.0 * round(x * 0.5);
    return r;
}

#define FT float
#define UT uint32_t
#define PFX(n) f32_##n
#define FRAC_BITS 23
#define H_SQRT sqrt_hf
#define H_FMA fma_hf
#define H_CEIL ceil_hf
#define H_FLOOR floor_hf
#define H_TRUNC trunc_hf
#define H_ROUND round_hf
#define H_RINTN rint_nf
#include "a32_vfp_t.h"
#undef FT
#undef UT
#undef PFX
#undef FRAC_BITS

#define FT double
#define UT uint64_t
#define PFX(n) f64_##n
#define FRAC_BITS 52
#undef H_SQRT
#undef H_FMA
#undef H_CEIL
#undef H_FLOOR
#undef H_TRUNC
#undef H_ROUND
#undef H_RINTN
#define H_SQRT sqrt_hd
#define H_FMA fma_hd
#define H_CEIL ceil_hd
#define H_FLOOR floor_hd
#define H_TRUNC trunc_hd
#define H_ROUND round_hd
#define H_RINTN rint_nd
#include "a32_vfp_t.h"
#undef FT
#undef UT
#undef PFX
#undef FRAC_BITS

/* ---- memory ------------------------------------------------------------------------------------------------------- */
static inline uint32_t ld32(const A32Cpu *c, uint32_t a) {
    uint32_t v;
    memcpy(&v, c->mem + a, 4);
    return v;
}
static inline void st32(A32Cpu *c, uint32_t a, uint32_t v) { memcpy(c->mem + a, &v, 4); }

/* ---- half precision ----------------------------------------------------------------------------------------------- */
static uint32_t half_to_single(A32Cpu *c, uint16_t h, int ahp) {
    uint32_t sign = (uint32_t)(h >> 15) << 31, exp = (h >> 10) & 0x1f, frac = h & 0x3ff;
    if (exp == 0x1f && !ahp) {
        if (frac == 0) return sign | 0x7f800000u;
        if (!(frac & 0x200)) c->fpscr |= FPSCR_IOC;
        return (c->fpscr & FPSCR_DN) ? 0x7fc00000u : sign | 0x7fc00000u | (frac << 13);
    }
    if (exp == 0) {
        if (frac == 0) return sign;
        if (c->fpscr & FPSCR_FZ) {
            c->fpscr |= FPSCR_IDC;
            return sign;
        }
        unsigned k = 0; /* normalise the denormal: value = 2^(-14-k) * 1.frac */
        while (!(frac & 0x400)) {
            frac <<= 1;
            k++;
        }
        return sign | (uint32_t)(113 - k) << 23 | ((frac & 0x3ff) << 13);
    }
    return sign | (exp + 112) << 23 | frac << 13;
}

/* single (dbl = 0) or double (dbl = 1) bit pattern -> IEEE half, with FPSCR rounding, flush-to-zero and exception bits */
static uint16_t to_half(A32Cpu *c, uint64_t bits, int dbl) {
    unsigned ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = bits >> (ebits + fbits), exp = (bits >> fbits) & ((1u << ebits) - 1), frac = bits & ((1ull << fbits) - 1);
    uint16_t s = (uint16_t)(sign << 15);
    if (exp == (1u << ebits) - 1) {
        if (frac == 0) return (uint16_t)(s | 0x7c00);
        if (!((frac >> (fbits - 1)) & 1)) c->fpscr |= FPSCR_IOC;
        if (c->fpscr & FPSCR_DN) return 0x7e00;
        return (uint16_t)(s | 0x7e00 | ((frac >> (fbits - 10)) & 0x3ff));
    }
    if (exp == 0) {
        if (frac && (c->fpscr & FPSCR_FZ)) c->fpscr |= FPSCR_IDC;
        if (!frac || (c->fpscr & FPSCR_FZ)) return s;
    }
    int bias = dbl ? 1023 : 127, e = (int)exp - bias + 15;
    uint64_t mant = frac | (exp ? 1ull << fbits : 0);
    int shift = (int)fbits - 10;
    if (e <= 0) shift += 1 - e;
    int rm = (c->fpscr >> 22) & 3;
    uint32_t half, up = 0;
    int inexact;
    if (shift > 63) {
        half = 0;
        inexact = 1;
        up = (rm == 1 && !sign) || (rm == 2 && sign);
    } else {
        uint64_t rem = mant & ((1ull << shift) - 1), mid = 1ull << (shift - 1);
        half = (uint32_t)(mant >> shift);
        inexact = rem != 0;
        if (rem) {
            if (rm == 0) up = rem > mid || (rem == mid && (half & 1));
            else if (rm == 1) up = !sign;
            else if (rm == 2) up = (uint32_t)sign;
        }
    }
    if (inexact) c->fpscr |= FPSCR_IXC;
    if (e <= 0) {
        if (inexact) c->fpscr |= FPSCR_UFC;
        return (uint16_t)(s | (half + up));
    }
    uint32_t v = ((uint32_t)e << 10) + (half & 0x3ff) + up;
    if (e >= 31 || v >= 0x7c00) {
        c->fpscr |= FPSCR_OFC | FPSCR_IXC;
        int to_max = rm == 3 || (rm == 1 && sign) || (rm == 2 && !sign);
        return (uint16_t)(s | (to_max ? 0x7bff : 0x7c00));
    }
    return (uint16_t)(s | v);
}

static uint32_t vfp_expand_imm32(unsigned imm8) {
    uint32_t sign = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
    uint32_t exp = ((b6 ^ 1) << 7) | (b6 ? 0x7c : 0) | ((imm8 >> 4) & 3);
    return sign << 31 | exp << 23 | (imm8 & 15) << 19;
}
static uint64_t vfp_expand_imm64(unsigned imm8) {
    uint64_t sign = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
    uint64_t exp = ((b6 ^ 1) << 10) | (b6 ? 0x3fc : 0) | ((imm8 >> 4) & 3);
    return sign << 63 | exp << 52 | (uint64_t)(imm8 & 15) << 48;
}

/* register indices */
static inline unsigned sidx(unsigned v, unsigned b) { return (v << 1) | b; }
static inline unsigned didx(unsigned v, unsigned b) { return (b << 4) | v; }

static const int host_modes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
static void set_host_mode(const A32Cpu *c) {
    unsigned rm = (c->fpscr >> 22) & 3;
    if (rm) fesetround(host_modes[rm]);
}
static void reset_host_mode(const A32Cpu *c) {
    if ((c->fpscr >> 22) & 3) fesetround(FE_TONEAREST);
}

/* ---- data processing ------------------------------------------------------------------------------------------------ */
static A32Exit vfp_dp(A32Cpu *c, uint32_t w) {
    unsigned dbl = BIT(w, 8), D = BIT(w, 22), N = BIT(w, 7), M = BIT(w, 5), s = BIT(w, 6);
    unsigned vn = BITS(w, 19, 16), vd = BITS(w, 15, 12), vm = BITS(w, 3, 0);
    unsigned p = BIT(w, 23), q = BIT(w, 21), r = BIT(w, 20);
    unsigned opc = p << 2 | q << 1 | r;
    unsigned d, n, m;
    if (dbl) {
        d = didx(vd, D);
        n = didx(vn, N);
        m = didx(vm, M);
    } else {
        d = sidx(vd, D);
        n = sidx(vn, N);
        m = sidx(vm, M);
    }
    set_host_mode(c);
#define OP32(expr)                                                                                                                                             \
    do {                                                                                                                                                       \
        uint32_t X = VS(c, n), Y = VS(c, m), Z = VS(c, d);                                                                                                     \
        (void)X;                                                                                                                                               \
        (void)Y;                                                                                                                                               \
        (void)Z;                                                                                                                                               \
        VS(c, d) = (expr);                                                                                                                                     \
    } while (0)
#define OP64(expr)                                                                                                                                             \
    do {                                                                                                                                                       \
        uint64_t X = VD(c, n), Y = VD(c, m), Z = VD(c, d);                                                                                                     \
        (void)X;                                                                                                                                               \
        (void)Y;                                                                                                                                               \
        (void)Z;                                                                                                                                               \
        VD(c, d) = (expr);                                                                                                                                     \
    } while (0)
#define NEG32(x) ((x) ^ 0x80000000u)
#define NEG64(x) ((x) ^ 0x8000000000000000ull)
    switch (opc) {
        case 0:
            if (dbl) OP64(f64_add(c, Z, s ? NEG64(f64_mul(c, X, Y)) : f64_mul(c, X, Y)));
            else OP32(f32_add(c, Z, s ? NEG32(f32_mul(c, X, Y)) : f32_mul(c, X, Y)));
            break;
        case 1:
            if (dbl) OP64(f64_add(c, NEG64(Z), s ? NEG64(f64_mul(c, X, Y)) : f64_mul(c, X, Y)));
            else OP32(f32_add(c, NEG32(Z), s ? NEG32(f32_mul(c, X, Y)) : f32_mul(c, X, Y)));
            break;
        case 2:
            if (dbl) OP64(s ? NEG64(f64_mul(c, X, Y)) : f64_mul(c, X, Y));
            else OP32(s ? NEG32(f32_mul(c, X, Y)) : f32_mul(c, X, Y));
            break;
        case 3:
            if (dbl) OP64(s ? f64_sub(c, X, Y) : f64_add(c, X, Y));
            else OP32(s ? f32_sub(c, X, Y) : f32_add(c, X, Y));
            break;
        case 4:
            if (s) {
                reset_host_mode(c);
                return A32_EXIT_UNDEF;
            }
            if (dbl) OP64(f64_div(c, X, Y));
            else OP32(f32_div(c, X, Y));
            break;
        case 5: /* vfnms (s = 0) / vfnma (s = 1) */
            if (dbl) OP64(s ? f64_fma(c, NEG64(Z), NEG64(X), Y) : f64_fma(c, NEG64(Z), X, Y));
            else OP32(s ? f32_fma(c, NEG32(Z), NEG32(X), Y) : f32_fma(c, NEG32(Z), X, Y));
            break;
        case 6: /* vfma (s = 0) / vfms (s = 1) */
            if (dbl) OP64(s ? f64_fma(c, Z, NEG64(X), Y) : f64_fma(c, Z, X, Y));
            else OP32(s ? f32_fma(c, Z, NEG32(X), Y) : f32_fma(c, Z, X, Y));
            break;
        default: {            /* 7: the extension space */
            unsigned op = vn; /* opc2 */
            unsigned n7 = N;
            if (!s && !n7 && BITS(w, 7, 4) == 0) { /* vmov immediate */
                unsigned imm8 = (vn << 4) | vm;
                if (dbl) VD(c, d) = vfp_expand_imm64(imm8);
                else VS(c, d) = vfp_expand_imm32(imm8);
                break;
            }
            if (!s) {
                reset_host_mode(c);
                return A32_EXIT_UNDEF;
            }
            switch (op) {
                case 0:
                    if (!n7) {
                        if (dbl) VD(c, d) = VD(c, m);
                        else VS(c, d) = VS(c, m);
                    } else {
                        if (dbl) VD(c, d) = VD(c, m) & ~0x8000000000000000ull;
                        else VS(c, d) = VS(c, m) & ~0x80000000u;
                    }
                    break;
                case 1:
                    if (!n7) {
                        if (dbl) VD(c, d) = NEG64(VD(c, m));
                        else VS(c, d) = NEG32(VS(c, m));
                    } else {
                        if (dbl) VD(c, d) = f64_sqrt(c, VD(c, m));
                        else VS(c, d) = f32_sqrt(c, VS(c, m));
                    }
                    break;
                case 2:
                case 3: { /* vcvtb / vcvtt */
                    unsigned top = n7, ahp = !!(c->fpscr & FPSCR_AHP);
                    if (ahp) {
                        reset_host_mode(c);
                        return A32_EXIT_UNIMPL;
                    }
                    unsigned sm = sidx(vm, M), sd = sidx(vd, D);
                    if (op == 2) { /* half -> single/double */
                        uint16_t h = (uint16_t)(VS(c, sm) >> (top ? 16 : 0));
                        uint32_t sv = half_to_single(c, h, ahp);
                        if (dbl) {
                            double dv = (double)f32_f(sv);
                            uint64_t dbits;
                            memcpy(&dbits, &dv, 8);
                            if (f32_isnan(sv)) dbits = 0x7ff8000000000000ull | ((uint64_t)(sv & 0x80000000u) << 32) | ((uint64_t)(sv & 0x3fffff) << 29);
                            VD(c, didx(vd, D)) = dbits;
                        } else VS(c, sd) = sv;
                    } else { /* single/double -> half */
                        uint16_t h = to_half(c, dbl ? VD(c, didx(vm, M)) : VS(c, sm), dbl);
                        uint32_t cur = VS(c, sd);
                        VS(c, sd) = top ? (cur & 0xffffu) | ((uint32_t)h << 16) : (cur & 0xffff0000u) | h;
                    }
                    break;
                }
                case 4:
                case 5: { /* vcmp / vcmpe */
                    if (op == 4) {
                        if (dbl) f64_cmp(c, VD(c, d), VD(c, m), n7);
                        else f32_cmp(c, VS(c, d), VS(c, m), n7);
                    } else {
                        if (dbl) f64_cmp(c, VD(c, d), 0, n7);
                        else f32_cmp(c, VS(c, d), 0, n7);
                    }
                    break;
                }
                case 6:
                    if (dbl) VD(c, d) = f64_rint(c, VD(c, m), n7 ? 3 : 5, 0);
                    else VS(c, d) = f32_rint(c, VS(c, m), n7 ? 3 : 5, 0);
                    break;
                case 7:
                    if (!n7) {
                        if (dbl) VD(c, d) = f64_rint(c, VD(c, m), 5, 1);
                        else VS(c, d) = f32_rint(c, VS(c, m), 5, 1);
                    } else if (dbl) { /* double -> single */
                        uint64_t x = VD(c, m);
                        uint32_t res;
                        if (f64_isnan(x)) {
                            if (f64_issnan(x)) c->fpscr |= FPSCR_IOC;
                            res = (c->fpscr & FPSCR_DN) ? 0x7fc00000u : (uint32_t)((x >> 32) & 0x80000000u) | 0x7fc00000u | (uint32_t)((x >> 29) & 0x3fffff);
                        } else {
                            x = f64_in(c, x);
                            feclearexcept(FE_ALL_EXCEPT);
                            volatile double dv = f64_f(x);
                            res = f32_fin(c, (float)dv);
                        }
                        VS(c, sidx(vd, D)) = res;
                    } else { /* single -> double */
                        uint32_t x = VS(c, sidx(vm, M));
                        uint64_t res;
                        if (f32_isnan(x)) {
                            if (f32_issnan(x)) c->fpscr |= FPSCR_IOC;
                            res = (c->fpscr & FPSCR_DN) ? 0x7ff8000000000000ull
                                                        : (uint64_t)(x & 0x80000000u) << 32 | 0x7ff8000000000000ull | (uint64_t)(x & 0x3fffff) << 29;
                        } else {
                            x = f32_in(c, x);
                            feclearexcept(FE_ALL_EXCEPT);
                            volatile float fv = f32_f(x);
                            res = f64_fin(c, (double)fv);
                        }
                        VD(c, didx(vd, D)) = res;
                    }
                    break;
                case 8: { /* integer -> float */
                    uint32_t v = VS(c, sidx(vm, M));
                    if (dbl) VD(c, d) = f64_from_int(c, v, n7);
                    else VS(c, d) = f32_from_int(c, v, n7);
                    break;
                }
                case 12:
                case 13: { /* float -> integer */
                    unsigned sd = sidx(vd, D);
                    int rm = n7 ? 3 : 5;
                    if (dbl) VS(c, sd) = f64_to_int(c, VD(c, m), op & 1, rm);
                    else VS(c, sd) = f32_to_int(c, VS(c, m), op & 1, rm);
                    break;
                }
                case 10:
                case 11:
                case 14:
                case 15: { /* fixed point */
                    unsigned size = n7 ? 32 : 16, fb = size - ((BITS(w, 3, 0) << 1) | BIT(w, 5));
                    int uns = op & 1;
                    if (fb > size) {
                        reset_host_mode(c);
                        return A32_EXIT_UNDEF;
                    }
                    unsigned dd = dbl ? didx(vd, D) : sidx(vd, D);
                    if (op < 12) { /* fixed -> float */
                        uint32_t v = dbl ? (uint32_t)VD(c, dd) : VS(c, dd);
                        if (dbl) VD(c, dd) = f64_from_fixed(c, v, !uns, size, fb);
                        else VS(c, dd) = f32_from_fixed(c, v, !uns, size, fb);
                    } else { /* float -> fixed */
                        uint32_t v;
                        if (dbl) v = f64_to_fixed(c, VD(c, dd), !uns, size, fb);
                        else v = f32_to_fixed(c, VS(c, dd), !uns, size, fb);
                        if (dbl) VD(c, dd) = uns ? (uint64_t)v : (uint64_t)(int64_t)(int32_t)v;
                        else VS(c, dd) = v; /* the whole register is written: extended */
                    }
                    break;
                }
                default: reset_host_mode(c); return A32_EXIT_UNDEF;
            }
            break;
        }
    }
    reset_host_mode(c);
    return A32_EXIT_BUDGET;
}

/* ---- load / store ---------------------------------------------------------------------------------------------------- */
static A32Exit vfp_ldst(A32Cpu *c, uint32_t w, uint32_t pcv) {
    unsigned dbl = BIT(w, 8), P = BIT(w, 24), U = BIT(w, 23), D = BIT(w, 22), W = BIT(w, 21), L = BIT(w, 20), rn = BITS(w, 19, 16), vd = BITS(w, 15, 12),
             imm8 = w & 0xff;
    if (BITS(w, 27, 21) == 0x62) { /* 64-bit transfers between core registers and extension registers */
        unsigned rt = BITS(w, 15, 12), rt2 = BITS(w, 19, 16), vm = BITS(w, 3, 0), M = BIT(w, 5);
        if (rt == 15 || rt2 == 15) return A32_EXIT_UNDEF;
        if (dbl) {
            unsigned dm = didx(vm, M);
            if (L) {
                c->r[rt] = (uint32_t)VD(c, dm);
                c->r[rt2] = (uint32_t)(VD(c, dm) >> 32);
            } else VD(c, dm) = (uint64_t)c->r[rt2] << 32 | c->r[rt];
        } else {
            unsigned sm = sidx(vm, M);
            if (sm == 63) return A32_EXIT_UNDEF;
            if (L) {
                c->r[rt] = VS(c, sm);
                c->r[rt2] = VS(c, sm + 1);
            } else {
                VS(c, sm) = c->r[rt];
                VS(c, sm + 1) = c->r[rt2];
            }
        }
        return A32_EXIT_BUDGET;
    }
    uint32_t base = rn == 15 ? (pcv & ~3u) : c->r[rn];
    if (P && !W) { /* vldr / vstr */
        uint32_t addr = U ? base + imm8 * 4 : base - imm8 * 4;
        if (dbl) {
            unsigned dd = didx(vd, D);
            if (L) {
                uint64_t v = ld32(c, addr) | (uint64_t)ld32(c, addr + 4) << 32;
                VD(c, dd) = v;
            } else {
                st32(c, addr, (uint32_t)VD(c, dd));
                st32(c, addr + 4, (uint32_t)(VD(c, dd) >> 32));
            }
        } else {
            unsigned sd = sidx(vd, D);
            if (L) VS(c, sd) = ld32(c, addr);
            else st32(c, addr, VS(c, sd));
        }
        return A32_EXIT_BUDGET;
    }
    if (P == U && W) return A32_EXIT_UNDEF;
    if (P == U) return A32_EXIT_UNDEF;
    unsigned words = imm8; /* vldm / vstm / vpush / vpop */
    unsigned first = dbl ? didx(vd, D) : sidx(vd, D);
    unsigned regs = dbl ? words / 2 : words;
    if (regs == 0 || first + regs > (dbl ? 32u : 32u) || (!dbl && first + regs > 32)) return A32_EXIT_UNDEF;
    if (rn == 15 && W) return A32_EXIT_UNDEF;
    uint32_t addr = U ? base : base - words * 4;
    uint32_t newbase = U ? base + words * 4 : base - words * 4;
    if (dbl) {
        for (unsigned k = 0; k < regs; k++) {
            if (L) VD(c, first + k) = ld32(c, addr) | (uint64_t)ld32(c, addr + 4) << 32;
            else {
                st32(c, addr, (uint32_t)VD(c, first + k));
                st32(c, addr + 4, (uint32_t)(VD(c, first + k) >> 32));
            }
            addr += 8;
        }
    } else {
        for (unsigned k = 0; k < regs; k++) {
            if (L) VS(c, first + k) = ld32(c, addr);
            else st32(c, addr, VS(c, first + k));
            addr += 4;
        }
    }
    if (W) c->r[rn] = newbase;
    return A32_EXIT_BUDGET;
}

/* ---- register transfers (cp10 / cp11, bit 4 set) ---------------------------------------------------------------------- */
static A32Exit vfp_xfer(A32Cpu *c, uint32_t w) {
    unsigned dbl = BIT(w, 8), L = BIT(w, 20), rt = BITS(w, 15, 12), vn = BITS(w, 19, 16), N = BIT(w, 7);
    unsigned opc1 = BITS(w, 23, 21), opc2 = BITS(w, 6, 5);
    if (BITS(w, 3, 0) != 0) return A32_EXIT_UNDEF;
    if (!dbl) {
        if (opc1 == 0 && opc2 == 0) {
            unsigned sn = sidx(vn, N);
            if (L) {
                if (rt == 15) return A32_EXIT_UNDEF;
                c->r[rt] = VS(c, sn);
            } else VS(c, sn) = c->r[rt];
            return A32_EXIT_BUDGET;
        }
        if (opc1 == 7 && opc2 == 0 && N == 0) {
            if (L) { /* vmrs */
                uint32_t v;
                switch (vn) {
                    case 0: v = 0x41034000u; break; /* FPSID (Cortex-A53 style value) */
                    case 1: v = c->fpscr; break;
                    case 5: v = 0x00000043u; break; /* MVFR2 */
                    case 6: v = 0x11111111u; break; /* MVFR1 */
                    case 7: v = 0x10110222u; break; /* MVFR0 */
                    case 8: v = c->fpexc_en; break;
                    default: return A32_EXIT_UNDEF;
                }
                if (rt == 15) {
                    if (vn != 1) return A32_EXIT_UNDEF;
                    c->n = v >> 31;
                    c->z = (v >> 30) & 1;
                    c->c = (v >> 29) & 1;
                    c->v = (v >> 28) & 1;
                } else c->r[rt] = v;
            } else {
                uint32_t v = c->r[rt];
                if (vn == 1) c->fpscr = v & 0xffc8ffffu; /* N Z C V QC AHP DN FZ RMode stride len and the enable and flag bits */
                else if (vn == 8) c->fpexc_en = v;
                else if (vn != 0) return A32_EXIT_UNDEF;
            }
            return A32_EXIT_BUDGET;
        }
        return A32_EXIT_UNDEF;
    }
    /* cp11: scalar <-> core and vdup */
    unsigned reg = didx(vn, N);
    if (BIT(w, 23) && !L) { /* vdup from core register: 1110 1 B Q 0 Vd Rt 1011 D 0 E 1 0000 */
        unsigned B = BIT(w, 22), Q = BIT(w, 21), E = BIT(w, 5), D = BIT(w, 7), vd = BITS(w, 19, 16);
        unsigned dd = didx(vd, D);
        if (BIT(w, 6) || rt == 15) return A32_EXIT_UNDEF;
        unsigned size = B ? 8 : E ? 16 : 32;
        if (B && E) return A32_EXIT_UNDEF;
        if (Q && (dd & 1)) return A32_EXIT_UNDEF;
        uint32_t v = c->r[rt];
        uint64_t pat = size == 32   ? (uint64_t)v << 32 | v
                       : size == 16 ? (uint64_t)((v & 0xffff) * 0x0001000100010001ull)
                                    : (uint64_t)((v & 0xff) * 0x0101010101010101ull);
        VD(c, dd) = pat;
        if (Q) VD(c, dd + 1) = pat;
        return A32_EXIT_BUDGET;
    }
    if (BITS(w, 3, 0) || (BIT(w, 4) == 0)) return A32_EXIT_UNDEF;
    unsigned U = BIT(w, 23), o1 = BITS(w, 22, 21);
    unsigned size, idx;
    if (o1 & 2) {
        size = 8;
        idx = (BIT(w, 21) << 2) | opc2;
    } else if (opc2 & 1) {
        size = 16;
        idx = (BIT(w, 21) << 1) | (opc2 >> 1);
    } else if (opc2 == 0) {
        size = 32;
        idx = BIT(w, 21);
    } else return A32_EXIT_UNDEF;
    if (rt == 15) return A32_EXIT_UNDEF;
    uint8_t *b = &c->v_.b[reg * 8];
    if (L) {
        uint32_t v;
        if (size == 8) {
            v = b[idx];
            if (!U) v = (uint32_t)(int32_t)(int8_t)v;
        } else if (size == 16) {
            uint16_t h;
            memcpy(&h, b + idx * 2, 2);
            v = h;
            if (!U) v = (uint32_t)(int32_t)(int16_t)v;
        } else memcpy(&v, b + idx * 4, 4);
        c->r[rt] = v;
    } else {
        if (U) return A32_EXIT_UNDEF;
        uint32_t v = c->r[rt];
        if (size == 8) b[idx] = (uint8_t)v;
        else if (size == 16) {
            uint16_t h = (uint16_t)v;
            memcpy(b + idx * 2, &h, 2);
        } else memcpy(b + idx * 4, &v, 4);
    }
    return A32_EXIT_BUDGET;
}

/* ---- ARMv8 unconditional instructions: vsel, vmaxnm/vminnm, vrint{a,n,p,m}, vcvt{a,n,p,m} ------------------------------- */
static A32Exit vfp_uncond(A32Cpu *c, uint32_t w) {
    unsigned dbl = BIT(w, 8), D = BIT(w, 22), N = BIT(w, 7), M = BIT(w, 5), vn = BITS(w, 19, 16), vd = BITS(w, 15, 12), vm = BITS(w, 3, 0);
    if (BITS(w, 11, 9) != 5) return A32_EXIT_UNDEF;
    if (BITS(w, 27, 24) != 0xe) return A32_EXIT_UNDEF;
    unsigned d, n, m;
    if (dbl) {
        d = didx(vd, D);
        n = didx(vn, N);
        m = didx(vm, M);
    } else {
        d = sidx(vd, D);
        n = sidx(vn, N);
        m = sidx(vm, M);
    }
    if (!BIT(w, 23) && !BIT(w, 4) && !BIT(w, 6)) { /* vsel */
        unsigned cc = BITS(w, 21, 20);
        int t = cc == 0 ? c->z : cc == 1 ? c->v : cc == 2 ? c->n == c->v : (c->n == c->v && !c->z);
        if (dbl) VD(c, d) = t ? VD(c, n) : VD(c, m);
        else VS(c, d) = t ? VS(c, n) : VS(c, m);
        return A32_EXIT_BUDGET;
    }
    if (BIT(w, 23) && BITS(w, 21, 20) == 0 && !BIT(w, 4)) { /* vmaxnm / vminnm */
        int is_max = !BIT(w, 6);
        if (dbl) VD(c, d) = f64_maxmin_num(c, VD(c, n), VD(c, m), is_max);
        else VS(c, d) = f32_maxmin_num(c, VS(c, n), VS(c, m), is_max);
        return A32_EXIT_BUDGET;
    }
    static const int rmmap[4] = {4, 0, 1, 2};                                                        /* a n p m -> away, nearest-even, +inf, -inf */
    if (BIT(w, 23) && BITS(w, 21, 19) == 7 && BITS(w, 7, 6) == 1 && !BIT(w, 4) && BIT(w, 18) == 0) { /* vrint<rm> */
        int rm = rmmap[BITS(w, 17, 16)];
        set_host_mode(c);
        if (dbl) VD(c, d) = f64_rint(c, VD(c, m), rm, 0);
        else VS(c, d) = f32_rint(c, VS(c, m), rm, 0);
        reset_host_mode(c);
        return A32_EXIT_BUDGET;
    }
    if (BIT(w, 23) && BITS(w, 21, 19) == 7 && BIT(w, 18) && BIT(w, 6) && !BIT(w, 4)) { /* vcvt<rm> float -> integer */
        int rm = rmmap[BITS(w, 17, 16)], sgn = BIT(w, 7);
        unsigned sd = sidx(vd, D);
        if (dbl) VS(c, sd) = f64_to_int(c, VD(c, m), sgn, rm);
        else VS(c, sd) = f32_to_int(c, VS(c, m), sgn, rm);
        return A32_EXIT_BUDGET;
    }
    return A32_EXIT_UNDEF;
}

A32Exit a32_exec_vfp(A32Cpu *c, uint32_t w, uint32_t pc_operand) {
    unsigned cond = w >> 28, cp = BITS(w, 11, 8);
    if (cond == 15) return vfp_uncond(c, w);
    if (cp != 10 && cp != 11) return A32_EXIT_UNIMPL;
    switch (BITS(w, 27, 24)) {
        case 0xc:
        case 0xd: return vfp_ldst(c, w, pc_operand);
        case 0xe: return BIT(w, 4) ? vfp_xfer(c, w) : vfp_dp(c, w);
        default: return A32_EXIT_UNDEF;
    }
}

/* ---- Advanced SIMD single precision ---------------------------------------------------------------------------------
 * NEON always runs with the "standard FPSCR value": flush-to-zero, default NaN, round to nearest even. Ops are NF_* in a32_neon.c. Integer<->float
 * conversions take the number of fraction bits in `arg`, or (arg & 0x1000) with the rounding mode in the low byte (0 even, 1 +inf, 2 -inf, 3 zero, 4 away). */
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
static inline float nf_f(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static inline uint32_t nf_u(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static inline int nf_isnan(uint32_t u) { return (u & 0x7fffffffu) > 0x7f800000u; }
static inline uint32_t nf_in(uint32_t u) { return (u & 0x7f800000u) == 0 ? (u & 0x80000000u) : u; } /* flush a denormal input */
static inline uint32_t nf_out(float f) {
    uint32_t u = nf_u(f);
    if (nf_isnan(u)) return 0x7fc00000u;
    return (u & 0x7f800000u) == 0 ? (u & 0x80000000u) : u;
}
static uint32_t nf_recip_est(uint32_t a) { /* a in [256, 512) -> [256, 511] */
    a = a * 2 + 1;
    unsigned b = (1u << 19) / a;
    return (b + 1) / 2;
}
static uint32_t nf_rsqrt_est(uint32_t a) { /* a in [128, 512) -> [256, 511] */
    if (a < 256) a = a * 2 + 1;
    else {
        a = (a >> 1) << 1;
        a = (a + 1) * 2;
    }
    uint64_t b = 512;
    while ((uint64_t)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (uint32_t)((b + 1) / 2);
}
static float nf_round(double x, int mode) { /* round to integral in float */
    switch (mode) {
        case 1: return (float)ceil(x);
        case 2: return (float)floor(x);
        case 3: return (float)trunc(x);
        case 4: return (float)round(x);
        default: {
            double r = nearbyint(x);
            return (float)r;
        } /* even (host mode is nearest) */
    }
}
uint32_t a32_neon_f32(A32Cpu *c, int op, uint32_t a, uint32_t b, uint32_t acc, int arg) {
    const uint32_t ones = 0xffffffffu;
    uint32_t ra = nf_in(a), rb = nf_in(b), racc = nf_in(acc);
    float fa = nf_f(ra), fb = nf_f(rb), fc = nf_f(racc);
    int an = nf_isnan(ra), bn = nf_isnan(rb);
    switch (op) {
        case NF_ADD: return nf_out(fa + fb);
        case NF_SUB: return nf_out(fa - fb);
        case NF_MUL: return nf_out(fa * fb);
        case NF_MLA: {
            uint32_t p = nf_out(fa * fb);
            return nf_out(fc + nf_f(p));
        }
        case NF_MLS: {
            uint32_t p = nf_out(fa * fb);
            return nf_out(fc - nf_f(p));
        }
        case NF_FMA: return nf_out(fmaf(fa, fb, fc));
        case NF_FMS: return nf_out(fmaf(-fa, fb, fc));
        case NF_ABD: {
            float d = fa - fb;
            return nf_out(fabsf(d));
        }
        case NF_MAX:
        case NF_MIN:
        case NF_MAXNM:
        case NF_MINNM: {
            int mx = op == NF_MAX || op == NF_MAXNM;
            if (op >= NF_MAXNM) {
                if (an && !bn) return rb;
                if (bn && !an) return ra;
            }
            if (an || bn) return 0x7fc00000u;
            if (fa == fb) return mx ? ((ra & 0x80000000u) ? rb : ra) : ((ra & 0x80000000u) ? ra : rb); /* +0 beats -0 for max, -0 beats +0 for min */
            return mx ? (fa > fb ? ra : rb) : (fa < fb ? ra : rb);
        }
        case NF_CEQ: return !an && !bn && fa == fb ? ones : 0;
        case NF_CGE: return !an && !bn && fa >= fb ? ones : 0;
        case NF_CGT: return !an && !bn && fa > fb ? ones : 0;
        case NF_ACGE: return !an && !bn && fabsf(fa) >= fabsf(fb) ? ones : 0;
        case NF_ACGT: return !an && !bn && fabsf(fa) > fabsf(fb) ? ones : 0;
        case NF_CEQ0: return !an && fa == 0 ? ones : 0;
        case NF_CGE0: return !an && fa >= 0 ? ones : 0;
        case NF_CGT0: return !an && fa > 0 ? ones : 0;
        case NF_CLE0: return !an && fa <= 0 ? ones : 0;
        case NF_CLT0: return !an && fa < 0 ? ones : 0;
        case NF_RECPS: {
            if (an || bn) return 0x7fc00000u;
            if ((isinf(fa) && fb == 0) || (fa == 0 && isinf(fb))) return nf_u(2.0f);
            return nf_out(fmaf(-fa, fb, 2.0f));
        }
        case NF_RSQRTS: {
            if (an || bn) return 0x7fc00000u;
            if ((isinf(fa) && fb == 0) || (fa == 0 && isinf(fb))) return nf_u(1.5f);
            return nf_out(fmaf(-fa, fb, 3.0f) / 2.0f);
        }
        case NF_RECPE: {
            if (arg == 1) { /* unsigned fixed point estimate (vrecpe.u32) */
                if (!(a & 0x80000000u)) return 0xffffffffu;
                uint32_t est = nf_recip_est((a >> 23) & 0x1ff);
                return (est << 23) | 0;
            }
            uint32_t sign = ra & 0x80000000u, exp = (ra >> 23) & 0xff, frac = ra & 0x7fffff;
            if (an) return 0x7fc00000u;
            if (exp == 0xff) return sign;            /* inf -> 0 */
            if (exp == 0) return sign | 0x7f800000u; /* zero (or flushed denormal) -> inf */
            int rexp = 253 - (int)exp;
            if (rexp <= 0) return sign; /* result denormal: flushed */
            if (rexp >= 255) return sign | 0x7f800000u;
            uint32_t est = nf_recip_est(0x100 | (frac >> 15));
            return sign | ((uint32_t)rexp << 23) | ((est & 0xff) << 15);
        }
        case NF_RSQRTE: {
            if (arg == 1) { /* unsigned fixed point */
                if (!(a & 0xc0000000u)) return 0xffffffffu;
                uint32_t est = nf_rsqrt_est((a >> 23) & 0x1ff);
                return est << 23;
            }
            uint32_t sign = ra & 0x80000000u, exp = (ra >> 23) & 0xff, frac = ra & 0x7fffff;
            if (an) return 0x7fc00000u;
            if (exp == 0) return sign | 0x7f800000u;
            if (sign) return 0x7fc00000u;
            if (exp == 0xff) return 0;
            uint32_t scaled = (exp & 1) ? (0x80 | (frac >> 16)) : (0x100 | (frac >> 15));
            int rexp = (380 - (int)exp) / 2;
            uint32_t est = nf_rsqrt_est(scaled);
            return ((uint32_t)rexp << 23) | ((est & 0xff) << 15);
        }
        case NF_ABS: return a & 0x7fffffffu;
        case NF_NEG: return a ^ 0x80000000u;
        case NF_CVT_S32:
        case NF_CVT_U32: {
            if (an) return 0;
            double x = (double)fa;
            if (arg & 0x1000) x = (double)nf_round(x, arg & 0xff);
            else x = trunc(x * ldexp(1.0, arg & 0x3f));
            if (op == NF_CVT_S32) return x >= 2147483647.0 ? 0x7fffffffu : x <= -2147483648.0 ? 0x80000000u : (uint32_t)(int32_t)x;
            return x >= 4294967295.0 ? 0xffffffffu : x <= 0 ? 0 : (uint32_t)x;
        }
        case NF_CVT_F_S32: return nf_out((float)((double)(int32_t)a / ldexp(1.0, arg & 0x3f)));
        case NF_CVT_F_U32: return nf_out((float)((double)a / ldexp(1.0, arg & 0x3f)));
        case NF_RINT:
            if (an) return 0x7fc00000u;
            return nf_out(nf_round((double)fa, arg & 0xff));
        case NF_CVT_RM:
            if ((arg & 0xff00) == 0x200) return to_half(c, a, 0);
            if ((arg & 0xff00) == 0x300) return half_to_single(c, (uint16_t)a, 0);
            return 0;
    }
    return 0;
}
