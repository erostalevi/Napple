// t_f64_msl_cxx.cpp - runs the *emitted MSL text* of the double-precision library (soft_double_msl.h, dumped to msl_dump.inc by t_f64_msl_cxx.sh) as C++
// and compares every function with the C build of the same source (soft_double.h) on random and edge vectors. MSL is C++14-based, so this catches
// stringification, macro and typing mistakes in what the shader compiler receives; it does not replace the Metal run (t_f64.m on the Mac).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t uint;   // the Metal scalar names
typedef uint64_t ulong;
static inline uint clz(uint x) { return x ? (uint)__builtin_clz(x) : 32u; }
namespace msl {
#include "msl_dump.inc"
}
namespace cc {
#include "shader/soft_double.h"
}

static uint64_t s0 = 88172645463325252ull;
static uint64_t rnd() {
    s0 ^= s0 << 13;
    s0 ^= s0 >> 7;
    s0 ^= s0 << 17;
    return s0;
}
static uint64_t gen() {
    uint64_t r = rnd();
    switch (rnd() % 6) {
        case 0: return r;
        case 1: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(1023 - 30 + rnd() % 60) << 52);
        case 2: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(rnd() % 3) << 52);
        case 3: return (r & 0x8000000000000000ull) | 0x7FF0000000000000ull | (rnd() % 3 ? 0 : (r & 0xF));
        case 4: return r & 0x8000000000000001ull;
        default: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(2040 + rnd() % 7) << 52);
    }
}
int main() {
    long bad = 0, n = 0;
#define EQ(call_msl, call_cc, what)                                                                                                                            \
    do {                                                                                                                                                       \
        n++;                                                                                                                                                   \
        uint64_t x = (call_msl), y = (call_cc);                                                                                                                \
        if (x != y && bad++ < 20) printf("MISMATCH %s msl=%016llx cc=%016llx\n", what, (unsigned long long)x, (unsigned long long)y);                          \
    } while (0)
    for (long i = 0; i < 400000; i++) {
        uint64_t a = gen(), b = gen(), c = gen();
        uint32_t m = (uint32_t)(rnd() & 3), op = (uint32_t)(rnd() & 15), nb = 8u << (rnd() & 3), sg = (uint32_t)(rnd() & 1);
        EQ(msl::sd_add(a, b, m), cc::sd_add(a, b, m), "add");
        EQ(msl::sd_sub(a, b, m), cc::sd_sub(a, b, m), "sub");
        EQ(msl::sd_mul(a, b, m), cc::sd_mul(a, b, m), "mul");
        EQ(msl::sd_fma(a, b, c, m), cc::sd_fma(a, b, c, m), "fma");
        EQ(msl::sd_cmp(a, b, op), cc::sd_cmp(a, b, op), "cmp");
        EQ(msl::sd_minmax(a, b, (uint32_t)(i & 1)), cc::sd_minmax(a, b, (uint32_t)(i & 1)), "minmax");
        EQ(msl::sd_rint(a, m), cc::sd_rint(a, m), "rint");
        EQ(msl::sd_from_int(a, sg, m), cc::sd_from_int(a, sg, m), "from_int");
        EQ(msl::sd_to_int(a, m, sg, nb), cc::sd_to_int(a, m, sg, nb), "to_int");
        EQ(msl::sd_to_f32(a, m), cc::sd_to_f32(a, m), "to_f32");
        EQ(msl::sd_int_to_f32(a, sg, m), cc::sd_int_to_f32(a, sg, m), "int_to_f32");
        EQ(msl::sd_from_f32((uint32_t)b), cc::sd_from_f32((uint32_t)b), "from_f32");
    }
    printf("t_f64_msl_cxx: %ld comparisons, %ld mismatches\n", n, bad);
    return bad != 0;
}
