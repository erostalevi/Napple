/* t_f64.m - GPU execution of the double-precision emulation (Mac only; written in a Linux cloud session and NOT yet run: see integrators/ for the check).
 *
 * Compiles sd_msl_source (the exact text the Maxwell translator appends to a shader that uses DADD/DMUL/DFMA/DSETP/DMNMX/DSET or a 64-bit conversion)
 * inside a compute kernel, runs random and edge vectors through every function for all four rounding modes, and compares each result bit for bit
 * with (a) the C build of the same source (soft_double.h, itself proven against the CPU's double by t_softdouble) and (b) the CPU's native double
 * for add/sub/mul/fma/compare. Any difference means the Metal compiler or GPU disagrees with the reference (fast-math reassociation does not apply to
 * integer code, so a mismatch would be a 64-bit integer codegen issue).
 * Build with the other tests: make -C gfx tests ; run: MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 build/tests/t_f64 */
#import <Metal/Metal.h>
#include <fenv.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader/soft_double.h"
#include "shader/soft_double_msl.h"

static int g_fail;
#define CHECK(c, ...)                                                                                                                                          \
    do {                                                                                                                                                       \
        if (!(c)) {                                                                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                                                        \
            printf(__VA_ARGS__);                                                                                                                               \
            printf("\n");                                                                                                                                      \
            g_fail++;                                                                                                                                          \
        }                                                                                                                                                      \
    } while (0)

enum {
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_FMA,
    OP_CMP,
    OP_MIN,
    OP_MAX,
    OP_RINT,
    OP_FROM_S,
    OP_FROM_U,
    OP_TO_S32,
    OP_TO_U32,
    OP_TO_S64,
    OP_TO_U64,
    OP_TO_F32,
    OP_FROM_F32,
    OP_S64_F32,
    OP_U64_F32,
    OP_COUNT
};
static const char *const op_name[OP_COUNT] = {"DADD",      "DSUB",    "DMUL",        "DFMA",        "DSETP",       "DMNMX.min",
                                              "DMNMX.max", "RINT",    "I2F.s64",     "I2F.u64",     "F2I.s32",     "F2I.u32",
                                              "F2I.s64",   "F2I.u64", "F2F.f64>f32", "F2F.f32>f64", "I2F.s64>f32", "I2F.u64>f32"};

static NSString *kernel_source(void) {
    NSString *lib = [NSString stringWithUTF8String:sd_msl_source];
    return [NSString
        stringWithFormat:
            @"#include <metal_stdlib>\nusing namespace metal;\n%@\n"
             "kernel void f64k(device const ulong *A [[buffer(0)]], device const ulong *B [[buffer(1)]], device const ulong *C [[buffer(2)]], device ulong *R "
             "[[buffer(3)]],\n"
             "                 constant uint &op [[buffer(4)]], constant uint &rm [[buffer(5)]], uint i [[thread_position_in_grid]]) {\n"
             "  ulong a = A[i], b = B[i], c = C[i], r = 0ul;\n"
             "  switch (op) {\n"
             "  case 0u: r = sd_add(a, b, rm); break;\n  case 1u: r = sd_sub(a, b, rm); break;\n  case 2u: r = sd_mul(a, b, rm); break;\n  case 3u: r = "
             "sd_fma(a, b, c, rm); break;\n"
             "  case 4u: r = ulong(sd_cmp(a, b, rm)); break;\n  case 5u: r = sd_minmax(a, b, 0u); break;\n  case 6u: r = sd_minmax(a, b, 1u); break;\n"
             "  case 7u: r = sd_rint(a, rm); break;\n  case 8u: r = sd_from_int(a, 1u, rm); break;\n  case 9u: r = sd_from_int(a, 0u, rm); break;\n"
             "  case 10u: r = sd_to_int(a, rm, 1u, 32u); break;\n  case 11u: r = sd_to_int(a, rm, 0u, 32u); break;\n  case 12u: r = sd_to_int(a, rm, 1u, 64u); "
             "break;\n"
             "  case 13u: r = sd_to_int(a, rm, 0u, 64u); break;\n  case 14u: r = ulong(sd_to_f32(a, rm)); break;\n  case 15u: r = sd_from_f32(uint(a)); "
             "break;\n"
             "  case 16u: r = ulong(sd_int_to_f32(a, 1u, rm)); break;\n  default: r = ulong(sd_int_to_f32(a, 0u, rm)); break;\n"
             "  }\n  R[i] = r;\n}\n",
            lib];
}

static uint64_t s0 = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) {
    s0 ^= s0 << 13;
    s0 ^= s0 >> 7;
    s0 ^= s0 << 17;
    return s0;
}
static uint64_t gen(void) {
    static const uint64_t edge[] = {0,
                                    0x8000000000000000ull,
                                    1,
                                    0x000FFFFFFFFFFFFFull,
                                    0x0010000000000000ull,
                                    0x3FF0000000000000ull,
                                    0xBFF0000000000000ull,
                                    0x3FE0000000000000ull,
                                    0x4330000000000000ull,
                                    0x7FEFFFFFFFFFFFFFull,
                                    0x7FF0000000000000ull,
                                    0xFFF0000000000000ull,
                                    0x7FF8000000000000ull,
                                    0x7FF0000000000001ull,
                                    0x41E0000000000000ull,
                                    0xC1E0000000000000ull,
                                    0x43E0000000000000ull,
                                    0xC3E0000000000000ull,
                                    0x3CA0000000000000ull,
                                    0x3C90000000000000ull};
    uint64_t r = rnd();
    switch (rnd() % 7) {
        case 0: return r;
        case 1: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(1023 - 40 + rnd() % 80) << 52);
        case 2: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(rnd() % 3) << 52);
        case 3: return edge[r % (sizeof edge / sizeof *edge)];
        case 4: return (r & 0x8000000000000000ull) | ((uint64_t)(1023 - 20 + rnd() % 40) << 52) | (1ull << (rnd() % 52)) | (1ull << (rnd() % 52));
        case 5: return r >> (rnd() % 64); // integers of every magnitude (the int->float ops read the raw value)
        default: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(2040 + rnd() % 7) << 52);
    }
}

static uint64_t ref_op(int op, uint64_t a, uint64_t b, uint64_t c, uint32_t rm) {
    switch (op) {
        case OP_ADD: return sd_add(a, b, rm);
        case OP_SUB: return sd_sub(a, b, rm);
        case OP_MUL: return sd_mul(a, b, rm);
        case OP_FMA: return sd_fma(a, b, c, rm);
        case OP_CMP: return sd_cmp(a, b, rm);
        case OP_MIN: return sd_minmax(a, b, 0);
        case OP_MAX: return sd_minmax(a, b, 1);
        case OP_RINT: return sd_rint(a, rm);
        case OP_FROM_S: return sd_from_int(a, 1, rm);
        case OP_FROM_U: return sd_from_int(a, 0, rm);
        case OP_TO_S32: return sd_to_int(a, rm, 1, 32);
        case OP_TO_U32: return sd_to_int(a, rm, 0, 32);
        case OP_TO_S64: return sd_to_int(a, rm, 1, 64);
        case OP_TO_U64: return sd_to_int(a, rm, 0, 64);
        case OP_TO_F32: return sd_to_f32(a, rm);
        case OP_FROM_F32: return sd_from_f32((uint32_t)a);
        case OP_S64_F32: return sd_int_to_f32(a, 1, rm);
        default: return sd_int_to_f32(a, 0, rm);
    }
}

/* the CPU's own double for the ops it has: add/sub/mul/fma under the matching rounding mode */
static int native_op(int op, uint64_t a, uint64_t b, uint64_t c, uint32_t rm, uint64_t *out) {
    static const int fe[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    double x, y, z;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    memcpy(&z, &c, 8);
    volatile double vx = x, vy = y, vz = z, r;
    fesetround(fe[rm & 3]);
    switch (op) {
        case OP_ADD: r = vx + vy; break;
        case OP_SUB: r = vx - vy; break;
        case OP_MUL: r = vx * vy; break;
        case OP_FMA: r = fma(vx, vy, vz); break;
        default: fesetround(FE_TONEAREST); return 0;
    }
    fesetround(FE_TONEAREST);
    double rr = r;
    *out = isnan(rr) ? 0x7FFFFFFFFFFFFFFFull : ({
        uint64_t t;
        memcpy(&t, &rr, 8);
        t;
    });
    return 1;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            printf("t_f64: no Metal device\n");
            return 1;
        }
        NSError *err = nil;
        MTLCompileOptions *o = [MTLCompileOptions new];
        o.languageVersion = MTLLanguageVersion3_0;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kernel_source() options:o error:&err];
        CHECK(lib != nil, "the double library compiles as MSL: %s", err.localizedDescription.UTF8String);
        if (!lib) return 1;
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"f64k"] error:&err];
        CHECK(pso != nil, "pipeline: %s", err.localizedDescription.UTF8String);
        if (!pso) return 1;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        enum { N = 1 << 17 };
        id<MTLBuffer> bA = [dev newBufferWithLength:N * 8 options:MTLResourceStorageModeShared],
                      bB = [dev newBufferWithLength:N * 8 options:MTLResourceStorageModeShared],
                      bC = [dev newBufferWithLength:N * 8 options:MTLResourceStorageModeShared],
                      bR = [dev newBufferWithLength:N * 8 options:MTLResourceStorageModeShared];
        uint64_t *A = bA.contents, *B = bB.contents, *C = bC.contents, *R = bR.contents;
        long total = 0, bad = 0, nbad_native = 0;
        for (int pass = 0; pass < 8; pass++) { // fresh vectors each pass: 8 x 131072 x ops x modes
            for (int i = 0; i < N; i++) {
                A[i] = gen();
                B[i] = gen();
                C[i] = gen();
                if (i % 5 == 0) B[i] = A[i] ^ (rnd() & 0xFFF); // near-equal operands: cancellation
                if (i % 7 == 0) {
                    double p, x, y;
                    memcpy(&x, &A[i], 8);
                    memcpy(&y, &B[i], 8);
                    volatile double vp = x * y;
                    p = -vp;
                    memcpy(&C[i], &p, 8);
                    C[i] += rnd() % 5;
                } // fma exactness
            }
            for (int op = 0; op < OP_COUNT; op++) {
                int nmodes = (op == OP_CMP) ? 16 : (op == OP_MIN || op == OP_MAX || op == OP_FROM_F32) ? 1 : 4;
                for (uint32_t rm = 0; rm < (uint32_t)nmodes; rm++) {
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
                    uint32_t opv = (uint32_t)op;
                    [e setComputePipelineState:pso];
                    [e setBuffer:bA offset:0 atIndex:0];
                    [e setBuffer:bB offset:0 atIndex:1];
                    [e setBuffer:bC offset:0 atIndex:2];
                    [e setBuffer:bR offset:0 atIndex:3];
                    [e setBytes:&opv length:4 atIndex:4];
                    [e setBytes:&rm length:4 atIndex:5];
                    [e dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                    [e endEncoding];
                    [cb commit];
                    [cb waitUntilCompleted];
                    CHECK(cb.status == MTLCommandBufferStatusCompleted, "%s rm=%u command buffer: %s", op_name[op], rm,
                          cb.error.localizedDescription.UTF8String);
                    long b0 = bad;
                    for (int i = 0; i < N; i++) {
                        uint64_t want = ref_op(op, A[i], B[i], C[i], rm), nat;
                        total++;
                        if (R[i] != want) {
                            if (bad++ < 24)
                                printf("MISMATCH %s rm=%u a=%016" PRIx64 " b=%016" PRIx64 " c=%016" PRIx64 " gpu=%016" PRIx64 " ref=%016" PRIx64 "\n",
                                       op_name[op], rm, A[i], B[i], C[i], R[i], want);
                        }
                        if (native_op(op, A[i], B[i], C[i], rm, &nat) && R[i] != nat) {
                            if (nbad_native++ < 24)
                                printf("NATIVE MISMATCH %s rm=%u a=%016" PRIx64 " b=%016" PRIx64 " c=%016" PRIx64 " gpu=%016" PRIx64 " cpu=%016" PRIx64 "\n",
                                       op_name[op], rm, A[i], B[i], C[i], R[i], nat);
                        }
                    }
                    if (pass == 0 && bad == b0) printf("ok   %-12s rm=%u\n", op_name[op], rm);
                }
            }
        }
        printf("t_f64: %ld GPU results compared with the C reference (%ld mismatches) and the CPU double (%ld mismatches)\n", total, bad, nbad_native);
        CHECK(bad == 0 && nbad_native == 0, "GPU results differ from the reference");
    }
    printf(g_fail ? "t_f64: %d FAILED\n" : "t_f64: ok\n", g_fail);
    return g_fail != 0;
}
