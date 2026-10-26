/* t_softdouble.c - verifies the binary64 emulation (src/shader/soft_double_core.inc, the code the translator emits into Metal shaders) against the
 * host CPU's native double on an edge-case corpus plus millions of random vectors, for all four Maxwell rounding modes.
 * Pure C, runs on Linux and macOS: gcc -O1 -I src tests/t_softdouble.c -lm  (tests/t_softdouble.sh). Exit 0 = zero mismatches.
 * NaN results must be the canonical 0x7FFFFFFFFFFFFFFF whenever the CPU produces any NaN. */
#include <fenv.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader/soft_double.h"

static const int fe_mode[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
static const char *const mode_name[4] = {"RN", "RM", "RP", "RZ"};

static uint64_t rng_s[2] = {0x9E3779B97F4A7C15ull, 0xD1B54A32D192ED03ull};
static uint64_t rnd(void) {   // xoroshiro128+
    uint64_t s0 = rng_s[0], s1 = rng_s[1], r = s0 + s1;
    s1 ^= s0;
    rng_s[0] = ((s0 << 24) | (s0 >> 40)) ^ s1 ^ (s1 << 16);
    rng_s[1] = (s1 << 37) | (s1 >> 27);
    return r;
}

static double d_of(uint64_t b) {
    double d;
    memcpy(&d, &b, 8);
    return d;
}
static uint64_t b_of(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}
static float f_of(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}
static uint32_t fb_of(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

static uint64_t g_checked, g_bad;
static uint64_t g_op_checked[32], g_op_bad[32];
static const char *g_opname[32];
static int g_nops;
static int op_id(const char *name) {
    for (int i = 0; i < g_nops; i++)
        if (!strcmp(g_opname[i], name)) return i;
    g_opname[g_nops] = name;
    return g_nops++;
}

static void report(const char *op, int mode, uint64_t a, uint64_t b, uint64_t c, uint64_t got, uint64_t want) {
    static int shown;
    if (shown++ < 40)
        printf("MISMATCH %s %s a=%016" PRIx64 " b=%016" PRIx64 " c=%016" PRIx64 " got=%016" PRIx64 " want=%016" PRIx64 "\n", op, mode_name[mode & 3], a, b, c,
               got, want);
}

/* compare a 64-bit result with the CPU's: NaN must be canonical */
static void check64(const char *op, int mode, uint64_t a, uint64_t b, uint64_t c, uint64_t got, double want) {
    uint64_t w = isnan(want) ? 0x7FFFFFFFFFFFFFFFull : b_of(want);
    int id = op_id(op);
    g_checked++;
    g_op_checked[id]++;
    if (got != w) {
        g_bad++;
        g_op_bad[id]++;
        report(op, mode, a, b, c, got, w);
    }
}
static void check_u(const char *op, int mode, uint64_t a, uint64_t b, uint64_t got, uint64_t want) {
    int id = op_id(op);
    g_checked++;
    g_op_checked[id]++;
    if (got != want) {
        g_bad++;
        g_op_bad[id]++;
        report(op, mode, a, b, 0, got, want);
    }
}

static uint64_t corpus[512];
static int ncorpus;
static void add(uint64_t v) { corpus[ncorpus++] = v; }
static void build_corpus(void) {
    static const uint64_t base[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x8000000000000001ull, 0x0000000000000002ull, 0x0000000000000003ull,
        0x000FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x0008000000000000ull, 0x0010000000000000ull, 0x8010000000000000ull, 0x0010000000000001ull,
        0x001FFFFFFFFFFFFFull, 0x0020000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x3FF0000000000001ull, 0x3FEFFFFFFFFFFFFFull,
        0x3FE0000000000000ull, 0x3FF8000000000000ull, 0x4000000000000000ull, 0x4008000000000000ull, 0x4010000000000000ull, 0x4014000000000000ull,
        0x3FD5555555555555ull, 0x3FE5555555555555ull, 0x4330000000000000ull, 0x4330000000000001ull, 0x432FFFFFFFFFFFFFull, 0x4340000000000000ull,
        0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull, 0x7FE0000000000000ull, 0x7FDFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
        0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull, 0x7FFFFFFFFFFFFFFFull, 0xFFF4000000000000ull, 0x7FF123456789ABCDull,
        0x41DFFFFFFFC00000ull, 0xC1E0000000000000ull, 0x41E0000000000000ull, 0x43DFFFFFFFFFFFFFull, 0x43E0000000000000ull, 0xC3E0000000000000ull,
        0x43F0000000000000ull, 0x3CA0000000000000ull, 0x3C90000000000000ull, 0x3CB0000000000000ull, 0x3FB999999999999Aull, 0x400921FB54442D18ull,
        0x3C9FFFFFFFFFFFFFull, 0x7FD0000000000000ull, 0x0360000000000000ull, 0x7C90000000000000ull, 0x2B2BFF2EE48E0530ull, 0x0000000100000000ull,
        0x3FE0000000000001ull, 0xBFE0000000000000ull, 0x3FF4000000000000ull, 0x4004000000000000ull, 0x41CFFFFFFFFFFFFFull, 0x7FE8000000000000ull,
    };
    for (size_t i = 0; i < sizeof base / sizeof *base; i++) {
        add(base[i]);
        if (base[i] != 0 && (base[i] & 0x7FF0000000000000ull) != 0x7FF0000000000000ull) add(base[i] ^ 0x8000000000000000ull);
    }
}

static uint64_t gen(int kind, uint64_t ref) {
    uint64_t r = rnd();
    switch (kind) {
        case 0: return r;                                                                              // anything
        case 1: return ((r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(1023 - 40 + (rnd() % 80)) << 52));   // moderate exponents
        case 2: {                                                                                      // exponent close to ref, random mantissa
            int e = (int)((ref >> 52) & 0x7FF) + (int)(rnd() % 13) - 6;
            if (e < 1) e = 1;
            if (e > 2046) e = 2046;
            return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)e << 52);
        }
        case 3: {   // few mantissa bits: exact sums, ties
            uint64_t m = 0;
            int n = (int)(rnd() % 5);
            for (int i = 0; i < n; i++) m |= 1ull << (rnd() % 52);
            if (rnd() & 1) m |= 1ull << (rnd() % 6);
            return (r & 0x8000000000000000ull) | ((uint64_t)(1023 - 30 + (rnd() % 60)) << 52) | m;
        }
        case 4: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(rnd() % 4) << 52);            // subnormal / tiny
        case 5: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(2046 - (rnd() % 4)) << 52);   // huge
        case 6: {                                                                              // same exponent and top mantissa: cancellation
            uint64_t v = (ref & 0xFFFFFFFFFFFFF000ull) | (r & 0xFFF);
            return v ^ ((rnd() & 1) ? 0x8000000000000000ull : 0);
        }
        default: return corpus[r % (uint64_t)ncorpus];
    }
}

static double h_add(double a, double b) {
    volatile double x = a, y = b, r = x + y;
    return r;
}
static double h_mul(double a, double b) {
    volatile double x = a, y = b, r = x * y;
    return r;
}
static double h_fma(double a, double b, double c) {
    volatile double x = a, y = b, z = c;
    double r = fma(x, y, z);
    return r;
}

static int host_cmp(uint64_t ab, uint64_t bb, int op) {
    double a = d_of(ab), b = d_of(bb);
    int un = isnan(a) || isnan(b);
    int lt = a<b, eq = a == b, gt = a> b;
    switch (op) {
        case 0: return 0;
        case 1: return lt;
        case 2: return eq;
        case 3: return lt || eq;
        case 4: return gt;
        case 5: return lt || gt;
        case 6: return gt || eq;
        case 7: return !un;
        case 8: return un;
        case 9: return un || lt;
        case 10: return un || eq;
        case 11: return un || lt || eq;
        case 12: return un || gt;
        case 13: return un || !eq;
        case 14: return un || gt || eq;
        default: return 1;
    }
}

static double host_rint(double d, int mode) {
    switch (mode) {
        case 0: {
            int old = fegetround();
            fesetround(FE_TONEAREST);
            double r = nearbyint(d);
            fesetround(old);
            return r;
        }
        case 1: return floor(d);
        case 2: return ceil(d);
        default: return trunc(d);
    }
}

static uint64_t host_to_int(double d, int mode, int sgn, int nbits) {
    if (isnan(d)) return 0;
    double r = host_rint(d, mode);
    double lim = ldexp(1.0, sgn ? nbits - 1 : nbits);
    if (sgn) {
        if (r >= lim) return nbits == 64 ? 0x7FFFFFFFFFFFFFFFull : ((1ull << (nbits - 1)) - 1);
        if (r <= -lim) return (uint64_t)(0 - (1ull << (nbits - 1)));
        return (uint64_t)(int64_t)r;
    }
    if (r <= 0) return 0;
    if (r >= lim) return nbits == 64 ? ~0ull : ((1ull << nbits) - 1);
    if (r >= 9223372036854775808.0) return (uint64_t)(r - 9223372036854775808.0) + 0x8000000000000000ull;
    return (uint64_t)r;
}

static void test_pair(uint64_t a, uint64_t b) {
    for (int m = 0; m < 4; m++) {
        fesetround(fe_mode[m]);
        check64("DADD", m, a, b, 0, sd_add(a, b, (uint32_t)m), h_add(d_of(a), d_of(b)));
        check64("DSUB", m, a, b, 0, sd_sub(a, b, (uint32_t)m), h_add(d_of(a), -d_of(b)));
        check64("DMUL", m, a, b, 0, sd_mul(a, b, (uint32_t)m), h_mul(d_of(a), d_of(b)));
        fesetround(FE_TONEAREST);
    }
    for (int op = 0; op < 16; op++) check_u("DSETP", op, a, b, sd_cmp(a, b, (uint32_t)op), (uint64_t)host_cmp(a, b, op));
    {   // min/max: NaN operand loses; equal zeros max -> +0, min -> -0
        double x = d_of(a), y = d_of(b);
        for (int mx = 0; mx < 2; mx++) {
            double want;
            if (isnan(x)) want = isnan(y) ? NAN : y;
            else if (isnan(y)) want = x;
            else if (x == y) want = mx ? d_of(a & b) : d_of(a | b);
            else want = mx ? (x > y ? x : y) : (x < y ? x : y);
            check64(mx ? "DMNMX.max" : "DMNMX.min", 0, a, b, 0, sd_minmax(a, b, (uint32_t)mx), want);
        }
    }
}

static void test_triple(uint64_t a, uint64_t b, uint64_t c) {
    for (int m = 0; m < 4; m++) {
        fesetround(fe_mode[m]);
        check64("DFMA", m, a, b, c, sd_fma(a, b, c, (uint32_t)m), h_fma(d_of(a), d_of(b), d_of(c)));
        fesetround(FE_TONEAREST);
    }
}

static void test_single(uint64_t a) {
    for (int m = 0; m < 4; m++) {
        check64("RINT", m, a, 0, 0, sd_rint(a, (uint32_t)m), host_rint(d_of(a), m));
        for (int sgn = 0; sgn < 2; sgn++)
            for (int nb = 0; nb < 5; nb++) {
                static const int nbs[5] = {8, 16, 32, 64, 64};
                int n = nbs[nb];
                if (nb == 4 && !sgn) continue;
                uint64_t got = sd_to_int(a, (uint32_t)m, (uint32_t)sgn, (uint32_t)n);
                check_u(sgn ? "F2I.signed" : "F2I.unsigned", m, a, (uint64_t)n, got, host_to_int(d_of(a), m, sgn, n));
            }
        fesetround(fe_mode[m]);
        {
            volatile double dd = d_of(a);
            volatile float ff = (float)dd;
            uint32_t want = fb_of(ff);
            if (isnan(dd)) want = 0x7FFFFFFFu;
            uint32_t got = sd_to_f32(a, (uint32_t)m);
            check_u("F2F.f64->f32", m, a, 0, got, want);
        }
        {   // integer sources: the 64-bit patterns a (signed and unsigned)
            volatile int64_t si = (int64_t)a;
            volatile uint64_t ui = a;
            volatile double d1 = (double)si, d2 = (double)ui;
            volatile float f1 = (float)si, f2 = (float)ui;
            check64("I2F.s64->f64", m, a, 0, 0, sd_from_int(a, 1, (uint32_t)m), d1);
            check64("I2F.u64->f64", m, a, 0, 0, sd_from_int(a, 0, (uint32_t)m), d2);
            check_u("I2F.s64->f32", m, a, 0, sd_int_to_f32(a, 1, (uint32_t)m), fb_of(f1));
            check_u("I2F.u64->f32", m, a, 0, sd_int_to_f32(a, 0, (uint32_t)m), fb_of(f2));
            uint64_t sm = a >> (rnd() % 64);   // smaller magnitudes: exact and tie cases
            volatile int64_t si2 = (int64_t)sm;
            volatile double d3 = (double)si2;
            check64("I2F.small", m, sm, 0, 0, sd_from_int(sm, 1, (uint32_t)m), d3);
        }
        fesetround(FE_TONEAREST);
    }
}

static void test_f32(uint32_t x) {
    float f = f_of(x);
    double want = (double)f;
    check64("F2F.f32->f64", 0, x, 0, 0, sd_from_f32(x), want);
}

int main(void) {
    build_corpus();
    long n_rand = 1500000;
    if (getenv("T_SD_N")) n_rand = atol(getenv("T_SD_N"));

    // edge corpus: all pairs, all triples
    for (int i = 0; i < ncorpus; i++) {
        test_single(corpus[i]);
        for (int j = 0; j < ncorpus; j++) {
            test_pair(corpus[i], corpus[j]);
            if ((i % 3) == 0 && (j % 3) == 0)
                for (int k = 0; k < ncorpus; k++) test_triple(corpus[i], corpus[j], corpus[k]);
        }
    }
    printf("edge corpus: %d values, %" PRIu64 " checks, %" PRIu64 " mismatches\n", ncorpus, g_checked, g_bad);

    uint64_t edge_checked = g_checked;
    for (long i = 0; i < n_rand; i++) {
        int ka = (int)(rnd() % 8), kb = (int)(rnd() % 8);
        uint64_t a = gen(ka, 0);
        uint64_t b = gen(kb == 2 || kb == 6 ? (kb) : kb, a);
        if (kb == 2 || kb == 6) b = gen(kb, a);
        uint64_t c = gen((int)(rnd() % 8), a);
        test_pair(a, b);
        test_single(a);
        test_triple(a, b, c);
        // fma exactness / cancellation: c = -(a*b) rounded, then nudged by a few ulps
        {
            volatile double p = d_of(a) * d_of(b);
            uint64_t cc = b_of(-p) + (uint64_t)((int64_t)(rnd() % 7) - 3);
            test_triple(a, b, cc);
            test_triple(a, b, (b_of(-p)) ^ (rnd() & 0xFFF));
        }
        // sums of nearby values and sparse values hit the tie and cancellation paths
        test_pair(gen(3, 0), gen(3, 0));
        test_pair(a, gen(6, a));
        uint32_t x = (uint32_t)rnd();
        test_f32(x);
        test_f32(x & 0x807FFFFFu);   // subnormals
    }
    printf("random: %ld iterations, %" PRIu64 " checks, %" PRIu64 " mismatches\n", n_rand, g_checked - edge_checked, g_bad);

    // exhaustive f32 -> f64 over every exponent class and a stride of mantissas
    for (uint64_t x = 0; x < (1ull << 32); x += 1237) test_f32((uint32_t)x);
    for (uint32_t x = 0; x < (1u << 23); x += 1) {
        test_f32(x);
        test_f32(0x80000000u | x);
        test_f32(0x7F800000u | x);
    }

    printf("%-16s %14s %10s\n", "op", "checks", "mismatches");
    for (int i = 0; i < g_nops; i++) printf("%-16s %14" PRIu64 " %10" PRIu64 "\n", g_opname[i], g_op_checked[i], g_op_bad[i]);
    printf("TOTAL %" PRIu64 " checks, %" PRIu64 " mismatches\n", g_checked, g_bad);
    puts(g_bad ? "FAILED" : "ALL SOFT-DOUBLE TESTS PASSED");
    return g_bad != 0;
}
