/* Template for the VFP arithmetic of one precision; included twice by a32_vfp.c (FT = float / double). The operands are bit patterns (UT),
 * arithmetic runs on the host FPU with the host rounding mode set from FPSCR.RMode, and the ARM-specific parts (NaN propagation, default NaN,
 * flush-to-zero, cumulative exception bits, saturating conversions) are done here. */
#ifndef FT
#error "define FT, UT, PFX, FRAC_BITS before including"
#endif

#define EXP_BITS_OF(UT_) (sizeof(UT_) == 4 ? 8 : 11)
#define NB (sizeof(UT) * 8)
#define SIGN_BIT ((UT)1 << (NB - 1))
#define FRAC_MASK (((UT)1 << FRAC_BITS) - 1)
#define EXP_MASK (((UT)1 << (NB - 1)) - 1 - FRAC_MASK)
#define QUIET_BIT ((UT)1 << (FRAC_BITS - 1))
#define DEFAULT_NAN (EXP_MASK | QUIET_BIT)

static inline FT PFX(f)(UT u) {
    FT f;
    memcpy(&f, &u, sizeof f);
    return f;
}
static inline UT PFX(u)(FT f) {
    UT u;
    memcpy(&u, &f, sizeof u);
    return u;
}
static inline int PFX(isnan)(UT u) { return (u & EXP_MASK) == EXP_MASK && (u & FRAC_MASK) != 0; }
static inline int PFX(issnan)(UT u) { return PFX(isnan)(u) && !(u & QUIET_BIT); }
static inline int PFX(isden)(UT u) { return (u & EXP_MASK) == 0 && (u & FRAC_MASK) != 0; }

/* flush a denormal input to zero when FPSCR.FZ is set */
static UT PFX(in)(A32Cpu *c, UT u) {
    if ((c->fpscr & FPSCR_FZ) && PFX(isden)(u)) {
        c->fpscr |= FPSCR_IDC;
        return u & SIGN_BIT;
    }
    return u;
}

/* Collects the host exception flags into FPSCR and converts the host result to the ARM bit pattern. */
static UT PFX(fin)(A32Cpu *c, FT r) {
    int e = fetestexcept(FE_ALL_EXCEPT);
    UT u = PFX(u)(r);
    if (PFX(isnan)(u)) {
        c->fpscr |= FPSCR_IOC;
        return DEFAULT_NAN;
    } /* an invalid operation generates the default NaN (the host's sign differs) */
    if ((c->fpscr & FPSCR_FZ) && PFX(isden)(u)) {
        c->fpscr |= FPSCR_UFC;
        return u & SIGN_BIT;
    } /* flushed: underflow only, not inexact */
    if (e & FE_INVALID) c->fpscr |= FPSCR_IOC;
    if (e & FE_DIVBYZERO) c->fpscr |= FPSCR_DZC;
    if (e & FE_OVERFLOW) c->fpscr |= FPSCR_OFC;
    if (e & FE_UNDERFLOW) c->fpscr |= FPSCR_UFC;
    if (e & FE_INEXACT) c->fpscr |= FPSCR_IXC;
    return u;
}

/* FPProcessNaNs: if any operand is a NaN, the result is the first signalling NaN (quietened) else the first quiet NaN. */
static int PFX(nans)(A32Cpu *c, const UT *op, unsigned n, UT *out) {
    for (unsigned k = 0; k < n; k++)
        if (PFX(issnan)(op[k])) {
            c->fpscr |= FPSCR_IOC;
            *out = (c->fpscr & FPSCR_DN) ? DEFAULT_NAN : (op[k] | QUIET_BIT);
            return 1;
        }
    for (unsigned k = 0; k < n; k++)
        if (PFX(isnan)(op[k])) {
            *out = (c->fpscr & FPSCR_DN) ? DEFAULT_NAN : op[k];
            return 1;
        }
    return 0;
}

#define BEGIN_OP() feclearexcept(FE_ALL_EXCEPT)

static UT PFX(add)(A32Cpu *c, UT a, UT b) {
    UT n, ops[2] = {a, b};
    if (PFX(nans)(c, ops, 2, &n)) return n;
    a = PFX(in)(c, a);
    b = PFX(in)(c, b);
    BEGIN_OP();
    volatile FT x = PFX(f)(a), y = PFX(f)(b);
    return PFX(fin)(c, x + y);
}
static UT PFX(sub)(A32Cpu *c, UT a, UT b) {
    UT n, ops[2] = {a, b};
    if (PFX(nans)(c, ops, 2, &n)) return n;
    a = PFX(in)(c, a);
    b = PFX(in)(c, b);
    BEGIN_OP();
    volatile FT x = PFX(f)(a), y = PFX(f)(b);
    return PFX(fin)(c, x - y);
}
static UT PFX(mul)(A32Cpu *c, UT a, UT b) {
    UT n, ops[2] = {a, b};
    if (PFX(nans)(c, ops, 2, &n)) return n;
    a = PFX(in)(c, a);
    b = PFX(in)(c, b);
    BEGIN_OP();
    volatile FT x = PFX(f)(a), y = PFX(f)(b);
    return PFX(fin)(c, x * y);
}
static UT PFX(div)(A32Cpu *c, UT a, UT b) {
    UT n, ops[2] = {a, b};
    if (PFX(nans)(c, ops, 2, &n)) return n;
    a = PFX(in)(c, a);
    b = PFX(in)(c, b);
    BEGIN_OP();
    volatile FT x = PFX(f)(a), y = PFX(f)(b);
    return PFX(fin)(c, x / y);
}
static UT PFX(sqrt)(A32Cpu *c, UT a) {
    UT n;
    if (PFX(nans)(c, &a, 1, &n)) return n;
    a = PFX(in)(c, a);
    BEGIN_OP();
    volatile FT x = PFX(f)(a);
    return PFX(fin)(c, H_SQRT(x));
}
/* fused a*b + addend */
static UT PFX(fma)(A32Cpu *c, UT addend, UT a, UT b) {
    UT n, ops[3] = {addend, a, b};
    int inf_zero = ((a & ~SIGN_BIT) == EXP_MASK && (b & ~SIGN_BIT) == 0) || ((b & ~SIGN_BIT) == EXP_MASK && (a & ~SIGN_BIT) == 0);
    if (PFX(isnan)(addend) && !PFX(issnan)(addend) && inf_zero && !PFX(isnan)(a) && !PFX(isnan)(b)) {
        c->fpscr |= FPSCR_IOC;
        return DEFAULT_NAN;
    }
    if (PFX(nans)(c, ops, 3, &n)) return n;
    addend = PFX(in)(c, addend);
    a = PFX(in)(c, a);
    b = PFX(in)(c, b);
    BEGIN_OP();
    volatile FT x = PFX(f)(a), y = PFX(f)(b), z = PFX(f)(addend);
    return PFX(fin)(c, H_FMA(x, y, z));
}

/* round to an integral value: mode 0 nearest-even 1 +inf 2 -inf 3 zero 4 nearest-away 5 current FPSCR mode; exact: raise inexact */
static UT PFX(rint)(A32Cpu *c, UT a, int mode, int exact) {
    UT n;
    if (PFX(nans)(c, &a, 1, &n)) return n;
    a = PFX(in)(c, a);
    FT x = PFX(f)(a), r;
    if (mode == 5) mode = (c->fpscr >> 22) & 3;
    switch (mode) {
        case 0: r = H_RINTN(x); break;
        case 1: r = H_CEIL(x); break;
        case 2: r = H_FLOOR(x); break;
        case 3: r = H_TRUNC(x); break;
        default: r = H_ROUND(x); break;
    }
    if (exact && r != x) c->fpscr |= FPSCR_IXC;
    UT u = PFX(u)(r);
    if (r == 0) u = (u & ~SIGN_BIT) | (a & SIGN_BIT); /* keeps the sign of the operand */
    return u;
}

/* VCMP: sets the FPSCR flags */
static void PFX(cmp)(A32Cpu *c, UT a, UT b, int quiet_nan_signals) {
    uint32_t f;
    if (PFX(isnan)(a) || PFX(isnan)(b)) {
        f = 0x3;
        if (PFX(issnan)(a) || PFX(issnan)(b) || quiet_nan_signals) c->fpscr |= FPSCR_IOC;
    } else {
        a = PFX(in)(c, a);
        b = PFX(in)(c, b);
        FT x = PFX(f)(a), y = PFX(f)(b);
        f = x == y ? 0x6 : x < y ? 0x8 : 0x2;
    }
    c->fpscr = (c->fpscr & 0x0fffffffu) | (f << 28);
}

/* float -> integer with ARM saturation; rm as in rint */
static uint32_t PFX(to_int)(A32Cpu *c, UT a, int is_signed, int rm) {
    if (PFX(isnan)(a)) {
        c->fpscr |= FPSCR_IOC;
        return 0;
    }
    a = PFX(in)(c, a);
    UT r = PFX(rint)(c, a, rm, 0);
    double x = (double)PFX(f)(r), orig = (double)PFX(f)(a);
    uint32_t out;
    if (is_signed) {
        if (x >= 2147483648.0) {
            c->fpscr |= FPSCR_IOC;
            return 0x7fffffffu;
        }
        if (x < -2147483648.0) {
            c->fpscr |= FPSCR_IOC;
            return 0x80000000u;
        }
        out = (uint32_t)(int32_t)x;
    } else {
        if (x >= 4294967296.0) {
            c->fpscr |= FPSCR_IOC;
            return 0xffffffffu;
        }
        if (x <= -1.0) {
            c->fpscr |= FPSCR_IOC;
            return 0;
        }
        out = (uint32_t)x;
    }
    if (x != orig) c->fpscr |= FPSCR_IXC;
    return out;
}
static UT PFX(from_int)(A32Cpu *c, uint32_t v, int is_signed) {
    BEGIN_OP();
    volatile FT r = is_signed ? (FT)(int32_t)v : (FT)v;
    return PFX(fin)(c, r);
}
/* fixed point: result bits <-> value with `frac` fraction bits, 16 or 32 bit container */
static uint32_t PFX(to_fixed)(A32Cpu *c, UT a, int is_signed, unsigned size, unsigned frac) {
    if (PFX(isnan)(a)) {
        c->fpscr |= FPSCR_IOC;
        return 0;
    }
    a = PFX(in)(c, a);
    double x = (double)PFX(f)(a) * (double)(1ull << frac), orig = x;
    x = trunc(x);
    double lo = is_signed ? -(double)(1ull << (size - 1)) : 0, hi = is_signed ? (double)(1ull << (size - 1)) - 1 : (double)((1ull << size) - 1);
    if (x > hi) {
        c->fpscr |= FPSCR_IOC;
        x = hi;
    } else if (x < lo) {
        c->fpscr |= FPSCR_IOC;
        x = lo;
    } else if (x != orig) c->fpscr |= FPSCR_IXC;
    return is_signed ? (uint32_t)(int64_t)x : (uint32_t)(uint64_t)x;
}
static UT PFX(from_fixed)(A32Cpu *c, uint32_t v, int is_signed, unsigned size, unsigned frac) {
    int64_t iv = is_signed ? (size == 16 ? (int64_t)(int16_t)v : (int64_t)(int32_t)v) : (size == 16 ? (int64_t)(uint16_t)v : (int64_t)(uint32_t)v);
    BEGIN_OP();
    volatile FT r = (FT)iv;
    volatile FT s = r / (FT)(double)(1ull << frac);
    return PFX(fin)(c, s);
}

/* vmaxnm / vminnm: a quiet NaN loses to a number */
static UT PFX(maxmin_num)(A32Cpu *c, UT a, UT b, int is_max) {
    int an = PFX(isnan)(a), bn = PFX(isnan)(b);
    if (PFX(issnan)(a) || PFX(issnan)(b)) {
        UT ops[2] = {a, b}, n;
        PFX(nans)(c, ops, 2, &n);
        return n;
    }
    if (an && !bn) return PFX(in)(c, b); /* the number that wins is still a flushed input when FZ is set */
    if (bn && !an) return PFX(in)(c, a);
    if (an && bn) return (c->fpscr & FPSCR_DN) ? DEFAULT_NAN : a;
    a = PFX(in)(c, a);
    b = PFX(in)(c, b);
    FT x = PFX(f)(a), y = PFX(f)(b);
    if (x == y) { /* +0 vs -0 */
        return is_max ? (a & b) : (a | b);
    }
    return (is_max ? x > y : x < y) ? a : b;
}

#undef EXP_BITS_OF
#undef NB
#undef SIGN_BIT
#undef FRAC_MASK
#undef EXP_MASK
#undef QUIET_BIT
#undef DEFAULT_NAN
#undef BEGIN_OP
