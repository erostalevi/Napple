/* t_xlate.m - Maxwell -> MSL translator: golden checks on hand-assembled shaders and, when NAPPLE_SHADER_CORPUS points to a directory of
 * extracted shaders (<name>.bin code + <name>.ctl control, stage from the _vs/_ps/_cs suffix; kept outside the repo), translation rate and
 * Metal compilation of every result. */
/* Portable: on a host without Metal (the Linux CI box) the Metal compile steps are skipped and counted as "unverified"; every translation and
 * text assertion still runs, so the translator can be checked off-Mac. Build there with tests/t_xlate_linux.sh. */
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <dirent.h>
#include "shader/maxwell.h"
#include "nvn_test.h"

#ifdef __APPLE__
static id<MTLDevice> g_dev;
static int compiles(const char *msl, NSString **err) {
    NSError *e = nil;
    MTLCompileOptions *o = [MTLCompileOptions new];
    o.languageVersion = MTLLanguageVersion3_0;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(msl) options:o error:&e];
    if (!lib && err) *err = e.localizedDescription;
    return lib != nil;
}
#define MSL_COMPILES(msl, what)                                                                                                                                \
    do {                                                                                                                                                       \
        NSString *e_ = nil;                                                                                                                                    \
        CHECK(compiles((msl), &e_), "%s MSL compiles: %s", (what), e_.UTF8String);                                                                           \
    } while (0)
#define MSL_COMPILES31(msl, what)                                                                                                                              \
    do {                                                                                                                                                       \
        NSError *e_ = nil;                                                                                                                                     \
        MTLCompileOptions *o_ = [MTLCompileOptions new];                                                                                                       \
        o_.languageVersion = MTLLanguageVersion3_1;                                                                                                            \
        id<MTLLibrary> l_ = [g_dev newLibraryWithSource:@(msl) options:o_ error:&e_];                                                                          \
        CHECK(l_ != nil, "%s MSL 3.1 compiles: %s", (what), e_.localizedDescription.UTF8String);                                                              \
    } while (0)
#define POOL @autoreleasepool
#else
static int g_unverified; /* Metal compile checks skipped on this host */
#define MSL_COMPILES(msl, what)                                                                                                                                \
    do {                                                                                                                                                       \
        (void)(msl);                                                                                                                                           \
        g_unverified++;                                                                                                                                        \
    } while (0)
#define MSL_COMPILES31(msl, what) MSL_COMPILES(msl, what)
#define POOL
#endif
static ShaderInfo *xl(uint8_t *blob, size_t n, int stage, ShaderKey key) { return maxwell_translate(blob, n, stage, &key); }

/* ---- ISA-gap regression helpers: a tiny assembler for the ops added in the shader-isa round (field layouts as the translator decodes them) ---- */
static uint64_t i_lds(int d, int a, int off, int size) {
    return 0xef48000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)(off & 0xffffff) << 20) | ((uint64_t)size << 48) | PT_BITS;
}
static uint64_t i_sts(int d, int a, int off, int size) {
    return 0xef58000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)(off & 0xffffff) << 20) | ((uint64_t)size << 48) | PT_BITS;
}
static uint64_t i_bar(void) { return 0xf0a8000000000000ull | PT_BITS; }
static uint64_t i_atoms(int d, int a, int b, int off, int size, int op) {
    return 0xec00000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)size << 28) | ((uint64_t)(off & 0x3fffff) << 30) |
           ((uint64_t)op << 52) | PT_BITS;
}
static uint64_t i_atom(int d, int a, int b, int off, int e, int size, int op) {
    return 0xed00000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)(off & 0xfffff) << 28) | ((uint64_t)e << 48) |
           ((uint64_t)size << 49) | ((uint64_t)op << 52) | PT_BITS;
}
static uint64_t i_red(int data, int a, int off, int e, int size, int op) {
    return 0xebf8000000000000ull | (uint64_t)data | ((uint64_t)a << 8) | ((uint64_t)size << 20) | ((uint64_t)op << 23) | ((uint64_t)(off & 0xfffff) << 28) |
           ((uint64_t)e << 48) | PT_BITS;
}
static uint64_t i_vote(int d, int pd, int vop, int psrc) {
    return 0x50d8000000000000ull | (uint64_t)d | ((uint64_t)psrc << 39) | ((uint64_t)pd << 45) | ((uint64_t)vop << 48) | PT_BITS;
}
static uint64_t i_cs2r(int d, int sr) { return 0x50c8000000000000ull | (uint64_t)d | ((uint64_t)sr << 20) | PT_BITS; }
/* TLD: target 0 1D, 1 2D, 2 3D, 3 cube */
static uint64_t i_tld(int d, int a, int b, int index, int target, int array, int mask, int aoffi, int ms, int lod) {
    return 0xdc38000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)array << 28) | ((uint64_t)target << 29) |
           ((uint64_t)mask << 31) | ((uint64_t)aoffi << 35) | ((uint64_t)index << 36) | ((uint64_t)ms << 50) | ((uint64_t)lod << 55) | PT_BITS;
}
static uint64_t i_txd(int d, int a, int b, int index, int target, int array, int mask, int aoffi) {
    return 0xde38000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)array << 28) | ((uint64_t)target << 29) |
           ((uint64_t)mask << 31) | ((uint64_t)aoffi << 35) | ((uint64_t)index << 36) | PT_BITS;
}
static uint64_t i_tld4(int d, int a, int b, int index, int aoff, int comp, int dc) {
    return 0xc838000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | (1ull << 29) | (0xfull << 31) | ((uint64_t)index << 36) |
           ((uint64_t)dc << 50) | ((uint64_t)aoff << 54) | ((uint64_t)comp << 56) | PT_BITS;
}
static uint64_t i_tld4s(int d0, int d1, int a, int b, int index, int dc, int aoffi, int comp) {
    return 0xdf00000000000000ull | (uint64_t)d0 | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)d1 << 28) | ((uint64_t)index << 36) |
           ((uint64_t)dc << 50) | ((uint64_t)aoffi << 51) | ((uint64_t)comp << 52) | PT_BITS;
}
static uint64_t i_bfe(int d, int a, int b, int brev, int sgn) {
    return 0x5c00000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)brev << 40) | ((uint64_t)sgn << 48) | PT_BITS;
}
static uint64_t i_prmt(int d, int a, int b, int c_, int mode) {
    return 0x5bc0000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)c_ << 39) | ((uint64_t)mode << 48) | PT_BITS;
}
static uint64_t i_r2p(int a, int b, int cc, int byte) {
    return 0x5cf0000000000000ull | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)cc << 40) | ((uint64_t)byte << 41) | PT_BITS;
}
static uint64_t i_ald_idx(int d, int a, int addr, int n) {
    return 0xefd8000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)addr << 20) | ((uint64_t)(n - 1) << 47) | PT_BITS;
}

/* IMAD / LEA: layouts from memory (see maxwell_translate.c), so these tests pin the translator's reading of them, not the hardware's */
static uint64_t i_imad(uint64_t cls, int d, int a, int b, int c_, uint64_t flags) {
    return (cls << 56) | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)c_ << 39) | flags | PT_BITS;
}
#define IMAD_S (1ull << 48)
#define IMAD_HI (1ull << 54)
static uint64_t i_lea_lo(uint64_t cls, int d, int a, int b, int scale, uint64_t flags) {
    return (cls << 56) | (0xdull << 52) | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)scale << 39) | flags | PT_BITS;
}
static uint64_t i_lea_hi(int d, int a, int b, int c_, int scale, uint64_t flags) {
    return (0x5bull << 56) | (0xdull << 52) | (1ull << 51) | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)scale << 28) |
           ((uint64_t)c_ << 39) | flags | PT_BITS;
}

#include "t_surface_asm.inc"

/* translate `n` instructions as `stage` (compute programs have no header: the code starts at the blob) */
static ShaderInfo *xl_ins(int stage, const uint64_t *ins, int n, ShaderKey key, uint32_t omap) {
    static uint8_t cs[0x1000], ctl[0x880];
    if (stage != STAGE_COMPUTE) {
        uint64_t pad[64];
        int m = (n + 2) / 3 * 3;
        for (int i = 0; i < m; i++) pad[i] = i < n ? ins[i] : ins_exit();
        build_shader(cs, ctl, stage, pad, m, omap);
        return xl(cs, 0x400, stage, key);
    }
    memset(cs, 0, sizeof cs);
    uint64_t *w = (uint64_t *)cs;
    int o = 1;
    for (int i = 0; i < (n + 2) / 3 * 3; i++) {
        w[o++] = i < n ? ins[i] : ins_exit();
        if (o % 4 == 0) w[o++] = 0;
    } /* pad the last group with EXIT */
    return maxwell_translate(cs, 0x400, STAGE_COMPUTE, &key);
}
#define HAS(si, str) ((si)->msl && strstr((si)->msl, (str)))
#define REFUSED(si, str) (!(si)->msl && strstr((si)->error, (str)))
/* translate, then run the Metal compile check on the result */
#define XL_OK(var, stage, key, omap, label, ...)                                                                                                               \
    uint64_t var##_i[] = {__VA_ARGS__};                                                                                                                        \
    ShaderInfo *var = xl_ins((stage), var##_i, (int)(sizeof var##_i / 8), (key), (omap));                                                                      \
    CHECK(var->msl != NULL, label " translation: %s", var->error);                                                                                             \
    if (var->msl) MSL_COMPILES(var->msl, label)
#define XL_REFUSE(var, stage, key, omap, why, label, ...)                                                                                                      \
    uint64_t var##_i[] = {__VA_ARGS__};                                                                                                                        \
    ShaderInfo *var = xl_ins((stage), var##_i, (int)(sizeof var##_i / 8), (key), (omap));                                                                      \
    CHECK(REFUSED(var, why), label " must be refused with \"%s\" (msl %s, error \"%s\")", why, var->msl ? "present" : "null", var->error);                     \
    maxwell_free(var)


#ifdef __APPLE__
/* Runs a translated fragment shader on a 1x1 RGBA32Uint target and returns its colour output (registers r0..r3 at EXIT): numeric checks of ALU translations. */
static int run_fs_u4(const char *msl, uint32_t out[4], int math_mode) {
    NSError *e = nil;
    MTLCompileOptions *o = [MTLCompileOptions new];
    o.languageVersion = MTLLanguageVersion3_0;
    o.preserveInvariance = YES;
    o.fastMathEnabled = NO;
    if (@available(macOS 15.0, *)) {
        if (math_mode) o.mathMode = math_mode == 1 ? MTLMathModeRelaxed : MTLMathModeFast;
    }
    id<MTLLibrary> fl = [g_dev newLibraryWithSource:@(msl) options:o error:&e];
    if (!fl) {
        printf("  fs compile: %s\n", e.localizedDescription.UTF8String);
        return 0;
    }
    static id<MTLFunction> vfn;
    if (!vfn) {
        id<MTLLibrary> vl = [g_dev
            newLibraryWithSource:
                @"#include <metal_stdlib>\nusing namespace metal;\nstruct VO { float4 pos [[position]]; };\n"
                 "vertex VO vmain(uint vid [[vertex_id]]) { VO o; float2 p = float2((vid << 1) & 2, vid & 2); o.pos = float4(p * 2.0 - 1.0, 0, 1); return o; }"
                         options:nil
                           error:&e];
        vfn = [vl newFunctionWithName:@"vmain"];
    }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vfn;
    d.fragmentFunction = [fl newFunctionWithName:@"main0"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Uint;
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) {
        printf("  pipeline: %s\n", e.localizedDescription.UTF8String);
        return 0;
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> rt = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
    rp.colorAttachments[0].texture = rt;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandQueue> q = [g_dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    [rt getBytes:out bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
    return 1;
}
#endif
static uint64_t i_s2r(int d, int sr) { return 0xf0c8000000000000ull | (uint64_t)d | ((uint64_t)sr << 20) | PT_BITS; }
static uint64_t i_fmnmx(int d, int a, int b, int pred, int pneg) {
    return 0x5c60000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)pred << 39) | ((uint64_t)pneg << 42) | PT_BITS;
}
/* FSETP P0 = (Ra <cmp> Rb) && PT;  SEL d = P0 ? Ra(imm 1) : RZ via FSET would need more ops: use SEL (reg) d = p ? a : b */
static uint64_t i_fsetp(int pd, int a, int b, int cmp) {
    return 0x5bb0000000000000ull | ((uint64_t)pd << 3) | 7ull | ((uint64_t)a << 8) | ((uint64_t)b << 20) | (7ull << 39) | ((uint64_t)cmp << 48) | PT_BITS;
}
static uint64_t i_sel(int d, int a, int b, int p) {
    return 0x5ca0000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)p << 39) | PT_BITS;
}
static uint64_t i_vmnmx(int d, int a, int b, int c_, int asel, int asg, int bsel, int bsg, int bvid, int mx, int vop, int dfmt, int sat) {
    return 0x3a00000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)bsel << 28) | ((uint64_t)asel << 36) |
           ((uint64_t)c_ << 39) | ((uint64_t)asg << 48) | ((uint64_t)bsg << 49) | ((uint64_t)bvid << 50) | ((uint64_t)vop << 51) | ((uint64_t)dfmt << 54) |
           ((uint64_t)sat << 55) | ((uint64_t)mx << 56) | PT_BITS;
}

int main(void) {
    POOL {
#ifdef __APPLE__
        g_dev = MTLCreateSystemDefaultDevice();
#endif
        static uint8_t code[0x1000], ctl[0x880];
        /* vertex: pass attribute 0 to the position */
        uint64_t vs[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
        build_shader(code, ctl, 0, vs, 3, 0);
        ShaderKey k = {0};
        ShaderInfo *s = xl(code, 0x400, STAGE_VERTEX, k);
        CHECK(s->msl != NULL, "vertex translation: %s", s->error);
        if (s->msl) {
            CHECK(strstr(s->msl, "float4 a0 [[attribute(0)]]"), "vertex input declared");
            CHECK(strstr(s->msl, "out.pos[0] = as_type<float>(r0)"), "position store");
            CHECK(!strstr(s->msl, "out.pos.y = -out.pos.y"), "no flip without the key");
            CHECK(s->attr_mask == 1, "attr mask %x", s->attr_mask);
            MSL_COMPILES(s->msl, "vertex");
        }
        maxwell_free(s);
        k.flip_y = true;
        k.depth_minus_one = true;
        s = xl(code, 0x400, STAGE_VERTEX, k);
        CHECK(s->msl && strstr(s->msl, "out.pos.y = -out.pos.y") && strstr(s->msl, "(out.pos.z + out.pos.w) * 0.5f"),
              "clip space epilogue (flip_y, depth -w..w)");
        maxwell_free(s);

        /* global memory (LDG/STG, SSBO access): in GPU-address mode (size ~0) the address is a Metal GPU virtual address and the shader needs no gmem buffer argument */
        {
            uint64_t ldg = 0xeed0000000000000ull | 0 | (2ull << 8) | (0x10ull << 20) | (1ull << 45) | (4ull << 48) | PT_BITS; /* LDG.E R0, [R2.64+0x10] */
            uint64_t gs[] = {ldg, ins_ast(0, 0x70, 4), ins_exit()};
            build_shader(code, ctl, 0, gs, 3, 0);
            extern uint64_t maxwell_gmem_base, maxwell_gmem_size;
            maxwell_gmem_base = 0;
            maxwell_gmem_size = ~0ull;
            ShaderKey gk = {0};
            ShaderInfo *g = xl(code, 0x400, STAGE_VERTEX, gk);
            CHECK(g->msl != NULL, "LDG translation: %s", g->error);
            if (g->msl) {
                CHECK(g->uses_gmem, "LDG marks the shader as using global memory");
                CHECK(strstr(g->msl, "device uchar *gmem = (device uchar *)ga0"), "GPU-address mode dereferences the address directly");
                CHECK(!strstr(g->msl, "device uchar *gmem [[buffer("), "no gmem buffer argument in GPU-address mode");
                MSL_COMPILES(g->msl, "LDG");
            }
            maxwell_free(g);
            maxwell_gmem_base = 0x100000;
            maxwell_gmem_size = 0x10000; /* window mode (kept for tools): bounds-checked offsets into one buffer */
            g = xl(code, 0x400, STAGE_VERTEX, gk);
            CHECK(g->msl && strstr(g->msl, "device uchar *gmem [[buffer(") && strstr(g->msl, "ga < 0x10000ul"), "window mode keeps the bounds-checked form");
            if (g->msl) MSL_COMPILES(g->msl, "LDG window");
            maxwell_free(g);
            maxwell_gmem_base = 0;
            maxwell_gmem_size = 0;
        }

        /* LDC.64 R12, c[6][R12 + 0x18] (SMO skinning): the address register is also the low destination, so the high half must be loaded first */
        {
            uint64_t ldc = 0xef90000000000000ull | (5ull << 48) | (6ull << 36) | (0x18ull << 20) | (12ull << 8) | 12ull | PT_BITS;
            uint64_t lv[] = {ins_ald(12, 0x80, 1), ldc, ins_ast(12, 0x70, 2), ins_exit()};
            build_shader(code, ctl, 0, lv, 4, 0);
            ShaderKey lk = {0};
            ShaderInfo *l = xl(code, 0x400, STAGE_VERTEX, lk);
            CHECK(l->msl != NULL, "LDC.64 translation: %s", l->error);
            if (l->msl) {
                const char *hi = strstr(l->msl, "r13 = cbr(cb6, cbsz[6], (r12) + 28u)"), *lo = strstr(l->msl, "r12 = cbr(cb6, cbsz[6], (r12) + 24u)");
                CHECK(hi && lo && hi < lo, "LDC.64 with an aliased address loads the high half before overwriting the address register");
                MSL_COMPILES(l->msl, "LDC.64");
            }
            maxwell_free(l);
        }

        /* LDC R10, c[6][R19 - 0x10]: the 16-bit immediate is signed (Animal Crossing's skinning reads bone rows below the index register; read as +0xfff0 every
         * row was out of range and the characters collapsed to nothing) */
        {
            uint64_t ldc = 0xef90000000000000ull | (6ull << 36) | (0xfff0ull << 20) | (19ull << 8) | 10ull | PT_BITS;
            uint64_t lv[] = {ins_ald(19, 0x80, 1), ldc, ins_ast(10, 0x70, 1), ins_exit()};
            build_shader(code, ctl, 0, lv, 4, 0);
            ShaderKey lk = {0};
            ShaderInfo *l = xl(code, 0x400, STAGE_VERTEX, lk);
            CHECK(l->msl != NULL, "LDC negative offset translation: %s", l->error);
            if (l->msl) {
                CHECK(strstr(l->msl, "cbr(cb6, cbsz[6], (r19) - 16u)") != NULL, "LDC c[6][R19 - 0x10] reads 16 bytes below the register, not 65520 above it");
                MSL_COMPILES(l->msl, "LDC negative offset");
            }
            maxwell_free(l);
        }

        /* fragment: constant colour, plus a texture fetch declares slot 0 at c2+0x20 */
        uint64_t fs[] = {ins_mov32i(4, 0x3e800000), ins_mov32i(5, 0x3f400000), ins_tex2d_lz(0, 4, 4, 8), ins_exit()};
        build_shader(code, ctl, 1, fs, 4, 0xf);
        ShaderKey fk = {0};
        s = xl(code, 0x400, STAGE_FRAGMENT, fk);
        CHECK(s->msl != NULL, "fragment translation: %s", s->error);
        if (s->msl) {
            CHECK(s->ntex == 1 && s->tex[0].handle_offset == 0x20 && s->tex[0].type == TEXTYPE_2D, "texture slot: n=%d off=%#x", s->ntex,
                  s->tex[0].handle_offset);
            CHECK(strstr(s->msl, "t0.sample(s0"), "sample call");
            CHECK(!strstr(s->msl, "ltint") && !strstr(s->msl, "dbgout"), "no game-specific hooks in the output");
            MSL_COMPILES(s->msl, "fragment");
        }
        maxwell_free(s);
        fk.depth_slots = 1; /* binding slot 0 holds a depth texture: declared depth2d, result (d, 0, 0, 1) */
        s = xl(code, 0x400, STAGE_FRAGMENT, fk);
        CHECK(s->msl && strstr(s->msl, "depth2d<float> t0") && strstr(s->msl, "float4(t0.sample"), "depth texture sampled as colour");
        if (s->msl) MSL_COMPILES(s->msl, "depth variant");
        maxwell_free(s);
        fk.depth_slots = 0;
        /* garbage must fail cleanly, not crash */
        memset(code + 0x80, 0xa5, 0x200);
        s = xl(code, 0x300, STAGE_FRAGMENT, fk);
        CHECK(s->msl == NULL && s->error[0], "garbage code reports an error");
        maxwell_free(s);

        /* forms found in the LAS corpus: HSETP2 with an immediate (cond LE against 1.0 in both halves), AL2P + IPA indexed by a register,
         * bindless TEX.B whose handle is loaded from a user uniform buffer (resolved at draw time) */
        {
            uint64_t hs[] = {0x7e0683bc0f070207ull, ins_exit(), ins_exit()};
            build_shader(code, ctl, 1, hs, 3, 0xf);
            ShaderKey hk = {0};
            ShaderInfo *h = xl(code, 0x400, STAGE_FRAGMENT, hk);
            CHECK(h->msl != NULL, "HSETP2 immediate form: %s", h->error);
            if (h->msl) MSL_COMPILES(h->msl, "HSETP2 immediate");
            maxwell_free(h);
            uint64_t ip[] = {0xefa0200008470226ull, 0xe0417fc800b72626ull, ins_exit()};
            build_shader(code, ctl, 1, ip, 3, 0xf);
            ShaderInfo *p = xl(code, 0x400, STAGE_FRAGMENT, hk);
            CHECK(p->msl != NULL, "AL2P + indexed IPA: %s", p->error);
            if (p->msl) {
                CHECK(strstr(p->msl, "attr_arr["), "indexed varying array");
                MSL_COMPILES(p->msl, "indexed IPA");
            }
            maxwell_free(p);
        }

        /* ================= shader-isa round: gaps in the Maxwell ISA ================= */
        ShaderKey z = {0};
        /* -- shared memory: LDS / STS with the BAR already emitted */
        {
            XL_OK(sm, STAGE_COMPUTE, z, 0, "LDS/STS dynamic", ins_mov32i(1, 0x1234), i_sts(1, 2, 0x10, 4), i_bar(), i_lds(3, 2, 0x10, 4), ins_exit());
            CHECK(HAS(sm, "threadgroup uint smem_w[8192]"), "dynamically addressed shared memory declares 32 KB");
            CHECK(sm->shared_mem == 32768, "shared_mem %u", sm->shared_mem);
            if (sm->msl) {
                const char *st = strstr(sm->msl, "*(threadgroup uint *)(smem + sa + 0u) = r1"),
                           *br = strstr(sm->msl, "threadgroup_barrier(mem_flags::mem_threadgroup)"),
                           *ld = strstr(sm->msl, "r3 = ok ? *(threadgroup uint *)(smem + sa + 0u)");
                CHECK(st && br && ld && st < br && br < ld, "store, barrier, load keep their order");
                CHECK(HAS(sm, "constant uint SMEM_BYTES = 32768u") && HAS(sm, "bool ok = sa + 4u <= SMEM_BYTES"), "accesses are bounds checked");
            }
            maxwell_free(sm);
            XL_OK(ss, STAGE_COMPUTE, z, 0, "LDS/STS static", i_sts(1, 255, 0x20, 4), i_sts(1, 255, 0x40, 6), i_lds(2, 255, 0x20, 1), i_lds(3, 255, 0x24, 3),
                  i_lds(4, 255, 0x28, 2), i_sts(1, 255, 0x2c, 0), i_sts(1, 255, 0x2e, 2), i_lds(8, 255, 0x30, 5), ins_exit());
            CHECK(ss->shared_mem == 0x50 && HAS(ss, "threadgroup uint smem_w[20]"), "static offsets size the array (0x40 + 16 bytes -> 0x50): %u",
                  ss->shared_mem);
            CHECK(HAS(ss, "uint(int(char(smem[sa])))") && HAS(ss, "uint(int(*(threadgroup short *)(smem + sa)))") &&
                      HAS(ss, "uint(*(threadgroup ushort *)(smem + sa))"),
                  "byte and short loads");
            CHECK(HAS(ss, "smem[sa] = uchar(r1)") && HAS(ss, "*(threadgroup ushort *)(smem + sa) = ushort(r1)"),
                  "byte and short stores write only their bytes");
            CHECK(HAS(ss, "r8 = ok ? *(threadgroup uint *)(smem + sa + 0u)") && HAS(ss, "r9 = ok ? *(threadgroup uint *)(smem + sa + 4u)"), "64-bit load");
            CHECK(HAS(ss, "smem + sa + 12u) = r4"), "128-bit store writes four consecutive registers");
            maxwell_free(ss);
            ShaderKey zk = z;
            zk.shared_size = 4096;
            XL_OK(sk, STAGE_COMPUTE, zk, 0, "LDS with a declared size", i_lds(3, 2, 0, 4), ins_exit());
            CHECK(sk->shared_mem == 4096 && HAS(sk, "smem_w[1024]"), "the key's shared size is honoured: %u", sk->shared_mem);
            maxwell_free(sk);
            zk.shared_size = 49152;
            XL_REFUSE(sb, STAGE_COMPUTE, zk, 0, "32 KB", "48 KB of shared memory", i_lds(3, 2, 0, 4), ins_exit());
            XL_REFUSE(sv, STAGE_VERTEX, z, 0, "outside a compute shader", "LDS in a vertex shader", i_lds(3, 2, 0, 4), ins_exit());
        }
        /* -- atomics: ATOMS (shared), ATOM / RED (global) */
        {
            XL_OK(am, STAGE_COMPUTE, z, 0, "ATOMS", i_atoms(4, 2, 1, 0x10, 0, 0), i_atoms(5, 2, 1, 0x10, 1, 1), i_atoms(6, 2, 1, 0x10, 0, 3),
                  i_atoms(7, 2, 1, 0x10, 0, 8), ins_exit());
            CHECK(HAS(am, "atom_tg((threadgroup atomic_uint *)(smem + sa), 0u, r1, false)") &&
                      HAS(am, "atom_tg((threadgroup atomic_uint *)(smem + sa), 1u, r1, true)") && HAS(am, ", 3u, r1, false)") && HAS(am, ", 8u, r1, false)"),
                  "ATOMS add/min/inc/exch operands");
            CHECK(HAS(am, "static inline uint atom_tg(threadgroup atomic_uint *p") && !HAS(am, "atom_dev("), "only the threadgroup helper is emitted");
            CHECK(HAS(am, "r4 = old;") && HAS(am, "r7 = old;"), "old value lands in Rd");
            maxwell_free(am);
            XL_REFUSE(ac, STAGE_COMPUTE, z, 0, "CAS and float forms", "ATOMS.CAS", i_atoms(4, 2, 1, 0, 0, 9), ins_exit());
            XL_REFUSE(a64, STAGE_COMPUTE, z, 0, "atoms 64-bit", "ATOMS.64", i_atoms(4, 2, 1, 0, 2, 0), ins_exit());
            XL_REFUSE(av, STAGE_VERTEX, z, 0, "outside a compute shader", "ATOMS in a vertex shader", i_atoms(4, 2, 1, 0, 0, 0), ins_exit());
            extern uint64_t maxwell_gmem_base, maxwell_gmem_size;
            maxwell_gmem_base = 0;
            maxwell_gmem_size = ~0ull;
            XL_OK(ag, STAGE_COMPUTE, z, 0, "ATOM/RED", i_atom(4, 2, 1, 0x10, 1, 0, 0), i_atom(5, 2, 1, 0, 1, 1, 2), i_red(1, 2, 0x20, 1, 0, 6), ins_exit());
            CHECK(ag->uses_gmem && ag->uses_gmem_store, "global atomics mark the buffer as written");
            CHECK(HAS(ag, "atom_dev((device atomic_uint *)(gmem + ga), 0u, r1, false)") &&
                      HAS(ag, "atom_dev((device atomic_uint *)(gmem + ga), 2u, r1, true)") && HAS(ag, "6u, r1, false)"),
                  "ATOM add, ATOM.MAX.S32, RED.OR");
            CHECK(HAS(ag, "(ulong(r3) << 32 | ulong(r2))") && HAS(ag, "r4 = old;") && HAS(ag, "(void)old;"), "64-bit address, result only for ATOM");
            maxwell_free(ag);
            XL_REFUSE(af, STAGE_COMPUTE, z, 0, "size 3", "ATOM.F32", i_atom(4, 2, 1, 0, 1, 3, 0), ins_exit());
            /* every op through Metal: native and CAS-loop helpers, both address spaces, GPU-address and window gmem */
            XL_OK(aa, STAGE_COMPUTE, z, 0, "ATOMS every op", i_atoms(4, 2, 1, 0, 0, 0), i_atoms(4, 2, 1, 0, 1, 1), i_atoms(4, 2, 1, 0, 1, 2),
                  i_atoms(4, 2, 1, 0, 0, 3), i_atoms(4, 2, 1, 0, 0, 4), i_atoms(4, 2, 1, 0, 0, 5), i_atoms(4, 2, 1, 0, 0, 6), i_atoms(4, 2, 1, 0, 0, 7),
                  i_atoms(4, 2, 1, 0, 0, 8), ins_exit());
            maxwell_free(aa);
            XL_OK(ab, STAGE_COMPUTE, z, 0, "ATOM every op", i_atom(4, 2, 1, 0, 1, 0, 0), i_atom(4, 2, 1, 0, 1, 1, 1), i_atom(4, 2, 1, 0, 1, 0, 2),
                  i_atom(4, 2, 1, 0, 1, 0, 3), i_atom(4, 2, 1, 0, 1, 0, 4), i_atom(4, 2, 1, 0, 1, 0, 5), i_atom(4, 2, 1, 0, 1, 0, 6),
                  i_atom(4, 2, 1, 0, 1, 0, 7), i_atom(4, 2, 1, 0, 1, 0, 8), i_red(1, 2, 0, 1, 0, 1), ins_exit());
            maxwell_free(ab);
            maxwell_gmem_base = 0x100000;
            maxwell_gmem_size = 0x10000;
            XL_OK(aw, STAGE_COMPUTE, z, 0, "ATOM/RED window gmem", i_atom(4, 2, 1, 0x10, 1, 0, 0), i_red(1, 2, 0x20, 1, 0, 3), ins_exit());
            maxwell_free(aw);
            maxwell_gmem_base = 0;
            maxwell_gmem_size = 0;
        }
        /* -- VOTE, CS2R */
        {
            XL_OK(vt, STAGE_COMPUTE, z, 0, "VOTE", i_vote(1, 0, 0, 7), i_vote(2, 1, 1, 7), i_vote(3, 2, 2, 7), ins_exit());
            CHECK(HAS(vt, "r1 = uint(static_cast<ulong>(simd_ballot(vp)))") && HAS(vt, "p0 = simd_all(vp)") && HAS(vt, "p1 = simd_any(vp)") &&
                      HAS(vt, "p2 = simd_all(vp) || !simd_any(vp)"),
                  "VOTE ALL/ANY/EQ");
            maxwell_free(vt);
            XL_OK(vv, STAGE_VERTEX, z, 0, "VOTE in a vertex shader", ins_ald(0, 0x80, 1), i_vote(1, 0, 0, 7), ins_exit());
            CHECK(!HAS(vv, "simd_"), "vertex functions get no SIMD-group operations");
            maxwell_free(vv);
            XL_OK(cs, STAGE_COMPUTE, z, 0, "CS2R", i_cs2r(4, 0xff), i_cs2r(6, 0x50), ins_exit());
            CHECK(HAS(cs, "r4 = 0u;") && HAS(cs, "r5 = 0u;") && HAS(cs, "r6 = 0u;") && HAS(cs, "r7 = 0u;"), "CS2R writes the register pair");
            maxwell_free(cs);
        }
        /* -- VMNMX (video min/max): Animal Crossing v0 shader 23e43e94e45bb1d5 holds 0x3b34056060470100 = VMNMX.U32.U32.MX.MAX R0, R1, R4, R10 */
        {
            XL_OK(vm, STAGE_FRAGMENT, z, 0xf, "VMNMX from Animal Crossing", 0x3b34056060470100ull, ins_exit());
            CHECK(HAS(vm, "long vr = max(va, vb);") && HAS(vm, "res = max(res, vc);") && HAS(vm, "r0 = res;"),
                  "VMNMX MAX, U32: three-way unsigned maximum"); /* integrate: matches nx-shader's VMNMX (the one kept) */
            maxwell_free(vm);
            /* byte select + MRG_16L merge (video op 1) with the 16-bit immediate: only checks that the merge goes through insert_bits */
            uint64_t w = 0x3a00000000000000ull | 2 | (1ull << 8) | (0x0033ull << 20) | (4ull << 36) | (1ull << 51) | PT_BITS | (9ull << 36 & 0);
            XL_OK(vm2, STAGE_FRAGMENT, z, 0xf, "VMNMX with a byte select", w, ins_exit());
            CHECK(HAS(vm2, "insert_bits(") && HAS(vm2, "min("), "VMNMX byte select translates");
            maxwell_free(vm2);
        }
        /* -- double precision: emulated with the soft_double library (bit-exactness is proven by t_softdouble on the host and t_f64 on the GPU) */
        {
            /* register pair at d, a; operand B: register 6 / cbuf c3[0x20] / 20-bit immediate */
#define DF(base, d, a, b, fl) ((uint64_t)(base) | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | (fl) | PT_BITS)
            const uint64_t dadd_r = DF(0x5c70000000000000ull, 2, 4, 6, (3ull << 39) | (1ull << 45) | (1ull << 46) | (1ull << 48) | (1ull << 49));
            XL_OK(da, STAGE_FRAGMENT, z, 0xf, "DADD", dadd_r, ins_exit());
            CHECK(HAS(da, "inline ulong sd_add(ulong a, ulong b, uint rm)"), "DADD pulls in the double library");
            CHECK(HAS(da, "inline ulong sd_fma(ulong a, ulong b, ulong c, uint rm)"), "the library is emitted whole (once)");
            CHECK(HAS(da, "(ulong(r4) | (ulong(r5) << 32))") && HAS(da, "(ulong(r6) | (ulong(r7) << 32))"), "operands are even-aligned register pairs");
            CHECK(HAS(da, "(sd_abs((ulong(r4) | (ulong(r5) << 32))) ^ 0x8000000000000000ul)") &&
                      HAS(da, "(sd_abs((ulong(r6) | (ulong(r7) << 32))) ^ 0x8000000000000000ul)"),
                  "-|A| (bits 48, 46) and -|B| (bits 45, 49)");
            CHECK(HAS(da, ", 3u)") && HAS(da, "r2 = uint(dv);") && HAS(da, "r3 = uint(dv >> 32);"), "rounding field 3 (RZ) and the pair write-back");
            maxwell_free(da);
            ShaderInfo *plain = NULL;
            {
                uint64_t i1[] = {DF(0x4c70000000000000ull, 2, 4, 0x20, (0x2ull << 34)), ins_exit()};
                plain = xl_ins(STAGE_FRAGMENT, i1, 2, z, 0xf);
                CHECK(plain->msl != NULL, "DADD with a constant-buffer operand: %s", plain->error);
                CHECK(HAS(plain, "(ulong(cb2[0][0]) | (ulong(cb2[0][1]) << 32))") || HAS(plain, "ulong(cb2["),
                      "DADD reads two consecutive constant-buffer words");
                maxwell_free(plain);
                uint64_t i2[] = {DF(0x3870000000000000ull, 2, 4, 0x12345, 0), ins_exit()};
                plain = xl_ins(STAGE_FRAGMENT, i2, 2, z, 0xf);
                CHECK(plain->msl != NULL, "DADD immediate: %s", plain->error);
                CHECK(HAS(plain, "0x12345000"), "the immediate is the top of the high word (0x12345 << 12)");
                maxwell_free(plain);
            }
            struct {
                const char *name, *need;
                uint64_t w;
            } dcases[] = {
                {"DMUL rr", "sd_mul(", DF(0x5c80000000000000ull, 2, 4, 6, 0)},
                {"DMUL cbuf", "sd_mul(", DF(0x4c80000000000000ull, 2, 4, 0x20, 0)},
                {"DMUL imm", "sd_mul(", DF(0x3880000000000000ull, 2, 4, 0x300, 0)},
                {"DMUL negated", "sd_mul(sd_neg(", DF(0x5c80000000000000ull, 2, 4, 6, 1ull << 48)},
                {"DFMA rr", "sd_fma(", DF(0x5b70000000000000ull, 2, 4, 6, 8ull << 39)},
                {"DFMA cbuf (B)", "sd_fma(", DF(0x4b70000000000000ull, 2, 4, 0x20, 8ull << 39)},
                {"DFMA imm", "sd_fma(", DF(0x3670000000000000ull, 2, 4, 0x123, 8ull << 39)},
                {"DFMA cbuf (C)", "sd_fma(", DF(0x5370000000000000ull, 2, 4, 6, 8ull << 39)},
                {"DFMA neg B, neg C, RM", "sd_neg(", DF(0x5b70000000000000ull, 2, 4, 6, (8ull << 39) | (1ull << 48) | (1ull << 49) | (1ull << 50))},
                {"DMNMX rr", "sd_minmax(", DF(0x5c50000000000000ull, 2, 4, 6, (7ull << 39))},
                {"DMNMX cbuf", "sd_minmax(", DF(0x4c50000000000000ull, 2, 4, 0x20, (7ull << 39))},
                {"DMNMX imm", "sd_minmax(", DF(0x3850000000000000ull, 2, 4, 0x20, (7ull << 39))},
                {"DSETP rr", "sd_cmp(", DF(0x5b80000000000000ull, 0x3f, 4, 6, (2ull << 48) | (7ull << 39))},
                {"DSETP cbuf", "sd_cmp(", DF(0x4b80000000000000ull, 0x3f, 4, 0x20, (1ull << 48) | (7ull << 39))},
                {"DSETP imm", "sd_cmp(", DF(0x3680000000000000ull, 0x3f, 4, 0x20, (4ull << 48) | (7ull << 39))},
                {"DSET rr", "sd_cmp(", DF(0x5900000000000000ull, 2, 4, 6, (2ull << 48) | (7ull << 39))},
                {"DSET cbuf BF", "0x3f800000u", DF(0x4900000000000000ull, 2, 4, 0x20, (2ull << 48) | (7ull << 39) | (1ull << 52))},
                {"DSET imm", "sd_cmp(", DF(0x3200000000000000ull, 2, 4, 0x20, (2ull << 48) | (7ull << 39))},
                {"F2F f32->f64", "sd_from_f32(", DF(0x5ca8000000000000ull, 2, 0, 6, (3ull << 8) | (2ull << 10))},
                {"F2F f64->f32 RP", "sd_to_f32(", DF(0x5ca8000000000000ull, 2, 0, 6, (2ull << 8) | (3ull << 10) | (2ull << 39))},
                {"F2F f64->f64 floor", "sd_rint(", DF(0x5ca8000000000000ull, 2, 0, 6, (3ull << 8) | (3ull << 10) | (5ull << 39 & (3ull << 39)) | (1ull << 42))},
                {"F2F f64->f16", "half(", DF(0x5ca8000000000000ull, 2, 0, 6, (1ull << 8) | (3ull << 10))},
                {"I2F s32->f64", "sd_from_int(", DF(0x5cb8000000000000ull, 2, 0, 6, (3ull << 8) | (2ull << 10) | (1ull << 13))},
                {"I2F u64->f64", "sd_from_int(", DF(0x5cb8000000000000ull, 2, 0, 6, (3ull << 8) | (3ull << 10))},
                {"I2F s64->f32", "sd_int_to_f32(", DF(0x5cb8000000000000ull, 2, 0, 6, (2ull << 8) | (3ull << 10) | (1ull << 13))},
                {"I2F u8->f64", "sd_from_int(", DF(0x5cb8000000000000ull, 2, 0, 6, (3ull << 8) | (0ull << 10))},
                {"F2I f64->s32", "sd_to_int(", DF(0x5cb0000000000000ull, 2, 0, 6, (2ull << 8) | (3ull << 10) | (1ull << 12))},
                {"F2I f64->u64", "sd_to_int(", DF(0x5cb0000000000000ull, 2, 0, 6, (3ull << 8) | (3ull << 10))},
                {"F2I f32->s64", "sd_from_f32(", DF(0x5cb0000000000000ull, 2, 0, 6, (3ull << 8) | (2ull << 10) | (1ull << 12))},
                {"F2I f64->s16", "sd_to_int(", DF(0x5cb0000000000000ull, 2, 0, 6, (1ull << 8) | (3ull << 10) | (1ull << 12))},
            };
            for (size_t i = 0; i < sizeof dcases / sizeof *dcases; i++) {
                uint64_t ins[] = {dcases[i].w, ins_exit()};
                ShaderInfo *r = xl_ins(STAGE_FRAGMENT, ins, 2, z, 0xf);
                CHECK(r->msl != NULL, "%s translates (error \"%s\")", dcases[i].name, r->error);
                if (r->msl) {
                    CHECK(strstr(r->msl, dcases[i].need) != NULL, "%s uses %s", dcases[i].name, dcases[i].need);
                    CHECK(strstr(r->msl, "inline ulong sd_round_pack(") != NULL, "%s carries the double library", dcases[i].name);
                    MSL_COMPILES(r->msl, dcases[i].name);
                }
                maxwell_free(r);
            }
            { /* a shader without doubles pays nothing */
                XL_OK(nd, STAGE_FRAGMENT, z, 0xf, "no double", DF(0x5c58000000000000ull, 2, 4, 6, 0), ins_exit());
                CHECK(!HAS(nd, "sd_round_pack"), "the double library is emitted only when a double op is present");
                maxwell_free(nd);
            }
#undef DF
            XL_REFUSE(ou, STAGE_VERTEX, z, 0, "OUT", "OUT", 0xfbe0000000000000ull | PT_BITS, ins_exit());
#include "t_surface.inc"
        }
        /* -- the rare ISA items (audit of the 2026-10-26 f64-isa package): video ALU, IMAD32I, FCHK, PIXLD, LEPC, calls/jumps, no-ops, refusals.
         * Field layouts follow envytools envydis gm107 (MIT); the assembler below encodes them the way the translator decodes them, so a test pins the
         * translator's reading of the layout, not the hardware's. */
        {
#define EV(base, d, a, b, fl) ((uint64_t)(base) | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | (fl) | PT_BITS)
#define VREG (1ull << 50)
#define VBSEL(s) ((uint64_t)(s) << 28)
#define VASEL(s) ((uint64_t)(s) << 36)
#define VASG (1ull << 48)
#define VBSG (1ull << 49)
#define VOP2(o) ((uint64_t)(o) << 51)
#define VDSG (1ull << 54)
#define VSAT (1ull << 55)
            struct {
                const char *name, *need1, *need2;
                uint64_t w;
            } vcases[] = {
                {"VADD u32 reg ACC", "long vr = va + vb;", "res = res + vc;", EV(0x2000000000000000ull, 1, 2, 3, VREG | VBSEL(6) | VASEL(6) | VOP2(4))},
                {"VADD s8 imm MRG_16L sat", "long va = long(int(r2 << 16u) >> 24)", "vr = clamp(vr, -2147483648l, 2147483647l);",
                 EV(0x2000000000000000ull, 1, 2, 0x7ff0, VASG | VASEL(1) | VOP2(1) | VDSG | VSAT)},
                {"VADD u16 h1 MIN", "long va = long((r2 >> 16u) & 0xFFFFu)", "res = min(res, vc);", EV(0x2000000000000000ull, 1, 2, 3, VASEL(5) | VOP2(5))},
                {"VABSDIFF", "(va > vb ? va - vb : vb - va)", "r1 = res;", EV(0x5400000000000000ull, 1, 2, 3, VREG | VBSEL(6) | VASEL(6) | VOP2(7))},
                {"VMAD shr7", "((va * vb) >> 7) + long(uint(", "r1 = res;",
                 EV(0x5f00000000000000ull, 1, 2, 3, VREG | VBSEL(6) | VASEL(6) | (1ull << 51) | (4ull << 39))},
                {"VMAD signed shr15", "((va * vb) >> 15) + long(int(", "r1 = res;",
                 EV(0x5f00000000000000ull, 1, 2, 3, VREG | VBSEL(6) | VASEL(6) | VASG | (2ull << 51))},
                {"VSHL clamp", "(va << min(vb & 65535l, 32l))", "r1 = res;", EV(0x5700000000000000ull, 1, 2, 5, VASEL(6) | VOP2(7))},
                {"VSHR wrap", "(va >> (vb & 31l))", "r1 = res;", EV(0x5600000000000000ull, 1, 2, 5, VASEL(6) | VOP2(7) | VBSG)},
                {"VSET lt", "(va < vb) ? 1l : 0l", "r1 = res;", EV(0x4000000000000000ull, 1, 2, 3, VREG | VBSEL(6) | VASEL(6) | VOP2(7) | (1ull << 43))},
                {"VSET ge", "(va >= vb) ? 1l : 0l", "r1 = res;",
                 EV(0x4000000000000000ull, 1, 2, 3, VREG | VBSEL(6) | VASEL(6) | VOP2(7) | (1ull << 44) | (1ull << 47))},
                {"VSETP gt", "bool cmp = va > vb;", "p1 = cmp ||",
                 EV(0x50f0000000000000ull, (1 << 3) | 2, 2, 3, VREG | VBSEL(6) | VASEL(6) | (1ull << 47) | (1ull << 45) | (7ull << 39))},
                {"IMAD32I", "r1 = (uint(", "* long(int(0x", EV(0x1000000000000000ull, 1, 2, 0, (0x1234ull << 20) | (1ull << 54))},
                {"IMAD32I.HI", ">> 32) + r1)", "long(0x", EV(0x1000000000000000ull, 1, 2, 0, (0x1234ull << 20) | (1ull << 53))},
                {"FCHK.DIVIDE", "eq <= 24", "p2 = ea < 2u", EV(0x5c88000000000000ull, 2 << 3, 4, 6, 0)},
                {"PIXLD.COVMASK", "r1 = 1u;", "r1 = 1u;", EV(0xefe8000000000000ull, 1, 255, 0, (1ull << 31) | (7ull << 45))},
                {"PIXLD.MY_INDEX", "r1 = 0u;", "p3 = false;", EV(0xefe8000000000000ull, 1, 255, 0, (5ull << 31) | (3ull << 45))},
                {"LEPC", "r1 = 8u;", "r1 = 8u;", EV(0x50d0000000000000ull, 1, 0, 0, 0)},
                {"CCTL", "", "", EV(0xef60000000000000ull, 0, 4, 0, 0)},
                {"CCTLL", "", "", EV(0xef80000000000000ull, 0, 4, 0, 0)},
            };
            for (size_t i = 0; i < sizeof vcases / sizeof *vcases; i++) {
                uint64_t ins[] = {vcases[i].w, ins_exit()};
                ShaderInfo *r = xl_ins(STAGE_FRAGMENT, ins, 2, z, 0xf);
                CHECK(r->msl != NULL, "%s translates (error \"%s\")", vcases[i].name, r->error);
                if (r->msl) {
                    CHECK(strstr(r->msl, vcases[i].need1) != NULL, "%s emits \"%s\"", vcases[i].name, vcases[i].need1);
                    CHECK(strstr(r->msl, vcases[i].need2) != NULL, "%s emits \"%s\"", vcases[i].name, vcases[i].need2);
                    MSL_COMPILES(r->msl, vcases[i].name);
                }
                maxwell_free(r);
            }
            /* calls and jumps: CAL/RET use a call stack, JMP/JCAL take absolute targets, JMX an absolute register target; each target becomes a label */
            {
                uint64_t cal = 0xe260000000000000ull | PT_BITS | ((uint64_t)(2 * 8 & 0xffffff) << 20); /* CAL +16 from the next instruction */
                uint64_t ins[] = {cal, 0xe300000000000000ull | 0xf | PT_BITS, 0x5c98000000000000ull | 1 | (2ull << 20) | PT_BITS,
                                  0xe320000000000000ull | 0xf | PT_BITS, ins_exit()};
                ShaderInfo *r = xl_ins(STAGE_FRAGMENT, ins, 5, z, 0xf);
                CHECK(r->msl != NULL, "CAL/RET translates: %s", r->error);
                CHECK(HAS(r, "cal[cal_n++] = ") && HAS(r, "pc = cal[--cal_n]; continue;") && HAS(r, "uint cal[16]; uint cal_n = 0;"),
                      "CAL pushes the return address, RET pops it");
                CHECK(HAS(r, "if (cal_n == 0u) { exit_now = true; break; }"), "RET with no caller ends the program");
                if (r->msl) MSL_COMPILES(r->msl, "CAL/RET");
                maxwell_free(r);
                uint64_t jmp = 0xe210000000000000ull | PT_BITS | (0x18ull << 20), jcal = 0xe220000000000000ull | PT_BITS | (0x28ull << 20);
                uint64_t jmx = 0xe200000000000000ull | PT_BITS | (4ull << 8) | (0x10ull << 20);
                uint64_t ins2[] = {jmp, ins_exit(), jcal, ins_exit(), jmx, ins_exit()};
                r = xl_ins(STAGE_FRAGMENT, ins2, 6, z, 0xf);
                CHECK(r->msl != NULL, "JMP/JCAL/JMX translate: %s", r->error);
                CHECK(HAS(r, "pc = 24u; continue;") && HAS(r, "cal[cal_n++] = ") && HAS(r, "pc = r4 + 0x10u; continue;"),
                      "absolute targets, and JMX = register + imm32");
                if (r->msl) MSL_COMPILES(r->msl, "JMP/JCAL/JMX");
                maxwell_free(r);
            }
            /* the control/system ops (BPT IDE RAM SAM RTT PEXIT LONGJMP PLONGJMP GET/SETCRSPTR GET/SETLMEMBASE B2R R2B ISBERD STP) and IDP are translated since
             * the shader-rare-ops round: tests/t_xlate_rare.inc */
#undef EV
#undef VREG
#undef VBSEL
#undef VASEL
#undef VASG
#undef VBSG
#undef VOP2
#undef VDSG
#undef VSAT
        }
        /* -- TLD (texel fetch), TXD (gradients) */
        {
            XL_OK(td, STAGE_FRAGMENT, z, 0xf, "TLD 2D", i_tld(0, 8, 12, 8, 1, 0, 0xf, 0, 0, 1), ins_exit());
            CHECK(td->ntex == 1 && td->tex[0].handle_offset == 0x20 && td->tex[0].type == TEXTYPE_2D, "TLD declares the 2D slot");
            CHECK(HAS(td, "t0.read(rsc(uint2(uint(int(r8)), uint(int(r9))), rscale[1]), uint(r12))"), "TLD 2D reads integer texels at the lod in Rb");
            maxwell_free(td);
            XL_OK(tz, STAGE_FRAGMENT, z, 0xf, "TLD.LZ", i_tld(0, 8, 12, 8, 1, 0, 0xf, 0, 0, 0), ins_exit());
            CHECK(HAS(tz, "), 0u)"), "TLD without the lod flag reads level 0");
            maxwell_free(tz);
            XL_OK(t1, STAGE_FRAGMENT, z, 0xf, "TLD 1D array + offsets", i_tld(0, 8, 12, 8, 0, 1, 0x1, 1, 0, 1), ins_exit());
            CHECK(t1->tex[0].type == TEXTYPE_1D_ARRAY && HAS(t1, "get_array_size() - 1u") && HAS(t1, "uint(int(r9) + (int(r13 << 28) >> 28))"),
                  "1D array layer clamp, x offset in the first nibble");
            maxwell_free(t1);
            XL_OK(t3, STAGE_FRAGMENT, z, 0xf, "TLD 3D", i_tld(0, 8, 12, 8, 2, 0, 0xf, 1, 0, 0), ins_exit());
            CHECK(HAS(t3, "t0.read(uint3(uint(int(r8) + (int(r12 << 28) >> 28)), uint(int(r9) + (int(r12 << 24) >> 28)), uint(int(r10) + (int(r12 << 20) >> "
                          "28))), 0u)"),
                  "3D fetch with 4-bit signed x/y/z offsets");
            maxwell_free(t3);
            XL_OK(tv, STAGE_VERTEX, z, 0, "TLD in a vertex shader", ins_ald(8, 0x80, 2), i_tld(0, 8, 12, 8, 1, 0, 0xf, 0, 0, 1), ins_exit());
            CHECK(HAS(tv, "t0.read(uint2(uint(int(r8)), uint(int(r9))), uint(r12))"), "no resolution scale in vertex shaders");
            maxwell_free(tv);
            XL_OK(tm, STAGE_FRAGMENT, z, 0xf, "TLD.MS", i_tld(0, 8, 12, 8, 1, 0, 0xf, 0, 1, 0), ins_exit());
            CHECK(HAS(tm, "texture2d_ms<float> t0") && HAS(tm, "t0.get_num_samples() - 1u)"), "TLD.MS reads a texture2d_ms by sample index (t_xlate_stages.inc has the rest)");
            maxwell_free(tm);
            XL_REFUSE(tc, STAGE_FRAGMENT, z, 0xf, "cube", "TLD cube", i_tld(0, 8, 12, 8, 3, 0, 0xf, 0, 0, 0), ins_exit());
            XL_OK(xd, STAGE_FRAGMENT, z, 0xf, "TXD 2D", i_txd(0, 8, 12, 8, 1, 0, 0xf, 0), ins_exit());
            CHECK(HAS(xd, "t0.sample(s0, float2(as_type<float>(r8), as_type<float>(r9)), gradient2d(float2(as_type<float>(r12), as_type<float>(r13)), "
                          "float2(as_type<float>(r14), as_type<float>(r15))))"),
                  "TXD: dx then dy from Rb");
            maxwell_free(xd);
            XL_OK(xe, STAGE_FRAGMENT, z, 0xf, "TXD 3D", i_txd(0, 8, 12, 8, 2, 0, 0xf, 0), ins_exit());
            CHECK(HAS(xe, "gradient3d(float3(as_type<float>(r12), as_type<float>(r13), as_type<float>(r14)), float3(as_type<float>(r15), as_type<float>(r16), "
                          "as_type<float>(r17)))"),
                  "TXD 3D uses six derivative registers");
            maxwell_free(xe);
            XL_OK(xa, STAGE_FRAGMENT, z, 0xf, "TXD 2D array", i_txd(0, 8, 12, 8, 1, 1, 0xf, 0), ins_exit());
            maxwell_free(xa);
            XL_OK(xc, STAGE_FRAGMENT, z, 0xf, "TXD cube", i_txd(0, 8, 12, 8, 3, 0, 0xf, 0), ins_exit());
            CHECK(HAS(xc, "gradientcube("), "TXD cube uses gradientcube");
            maxwell_free(xc);
            XL_OK(l1, STAGE_FRAGMENT, z, 0xf, "TLD 1D", i_tld(0, 8, 12, 8, 0, 0, 0xf, 0, 0, 1), ins_exit());
            CHECK(HAS(l1, "t0.read(uint(int(r8)))"), "1D fetch passes no lod (Metal 1D textures have one level)");
            maxwell_free(l1);
            XL_OK(l2, STAGE_FRAGMENT, z, 0xf, "TLD 2D array + offsets", i_tld(0, 8, 12, 8, 1, 1, 0xf, 1, 0, 1), ins_exit());
            maxwell_free(l2);
            ShaderKey dk = z;
            dk.depth_slots = 1;
            XL_OK(ld, STAGE_FRAGMENT, dk, 0xf, "TLD of a depth texture", i_tld(0, 8, 12, 8, 1, 0, 0xf, 0, 0, 1), ins_exit());
            CHECK(HAS(ld, "depth2d<float> t0"), "TLD keeps the depth2d declaration");
            maxwell_free(ld);
            XL_OK(lk, STAGE_FRAGMENT, dk, 0xf, "TLD.LZ of a depth texture", i_tld(0, 8, 12, 8, 1, 0, 0xf, 0, 0, 0), ins_exit());
            maxwell_free(lk);
            XL_REFUSE(x1, STAGE_FRAGMENT, z, 0xf, "1D", "TXD 1D", i_txd(0, 8, 12, 8, 0, 0, 0xf, 0), ins_exit());
            XL_REFUSE(xo, STAGE_FRAGMENT, z, 0xf, "offsets", "TXD with offsets", i_txd(0, 8, 12, 8, 1, 0, 0xf, 1), ins_exit());
        }
        /* -- IMAD (reg, rc, cr, imm forms; .HI; negations) and LEA (.LO, .LO.CC/.X, .HI) */
        {
            XL_OK(im, STAGE_FRAGMENT, z, 0xf, "IMAD", i_imad(0x5a, 1, 2, 3, 4, 0), i_imad(0x5a, 6, 2, 3, 4, IMAD_HI),
                  i_imad(0x5a, 7, 2, 3, 4, IMAD_HI | IMAD_S), i_imad(0x52, 8, 2, 3, 0x10, 0), i_imad(0x4a, 9, 2, 0x10, 4, 0), ins_exit());
            CHECK(HAS(im, "r1 = (r2 * r3 + r4)"), "IMAD low word");
            CHECK(HAS(im, "r6 = (uint((ulong(r2) * ulong(r3)) >> 32) + r4)"), "IMAD.HI unsigned (bit 54, envytools layout)");
            CHECK(HAS(im, "r7 = (uint(ulong(long(int(r2)) * long(int(r3))) >> 32) + r4)"), "IMAD.HI signed (bit 48)");
            CHECK(HAS(im, "r8 = (r2 * r16 + cb0["), "IMAD rc form: B = REG_39, C = constant buffer");
            CHECK(HAS(im, "r9 = (r2 * cb0["), "IMAD cr form: B = constant buffer");
            maxwell_free(im);
            XL_OK(ii, STAGE_FRAGMENT, z, 0xf, "IMAD imm", i_imad(0x34, 1, 2, 0x10, 4, 0), ins_exit());
            CHECK(HAS(ii, "r1 = (r2 * 0x00000010u + r4)"), "IMAD with an immediate B");
            maxwell_free(ii);
            XL_REFUSE(ic, STAGE_FRAGMENT, z, 0xf, "imad carry", "IMAD.CC", i_imad(0x5a, 1, 2, 3, 4, 1ull << 47), ins_exit());
            XL_REFUSE(is, STAGE_FRAGMENT, z, 0xf, "imad carry", "IMAD.SAT", i_imad(0x5a, 1, 2, 3, 4, 1ull << 50), ins_exit());
            XL_OK(lo, STAGE_FRAGMENT, z, 0xf, "LEA.LO", i_lea_lo(0x5b, 1, 2, 3, 2, 0), i_lea_lo(0x5b, 4, 2, 3, 5, 1ull << 45),
                  i_lea_lo(0x5b, 5, 2, 3, 3, 1ull << 47), i_lea_lo(0x5b, 6, 2, 3, 0, 1ull << 48), ins_exit());
            CHECK(HAS(lo, "r1 = ((r2 << 2u) + r3)") && HAS(lo, "r4 = ((r2 << 5u) + (0u - r3))"), "LEA.LO scales A and adds (or subtracts) B");
            CHECK(HAS(lo, "cc = uint(sum >> 32) & 1u") && HAS(lo, "ulong sum = ulong(r2 << 3u) + ulong(r3);") && HAS(lo, "ulong(r3) + ulong(cc)") &&
                      HAS(lo, "uint cc = 0;"),
                  "LEA.LO .CC and .X use the carry flag");
            maxwell_free(lo);
            XL_OK(hi, STAGE_FRAGMENT, z, 0xf, "LEA.HI", i_lea_hi(1, 2, 3, 4, 3, 0), i_lea_hi(5, 2, 3, 4, 0, 0), i_lea_hi(6, 2, 3, 4, 7, 1ull << 48),
                  ins_exit());
            CHECK(HAS(hi, "r1 = (((r4 << 3u) | (r2 >> 29u)) + r3);"), "LEA.HI: high word of the shifted 64-bit pair");
            CHECK(HAS(hi, "r5 = (r4 + r3);") && HAS(hi, "r6 = (((r4 << 7u) | (r2 >> 25u)) + r3 + cc)"), "LEA.HI scale 0 and .X");
            maxwell_free(hi);
            XL_OK(li, STAGE_FRAGMENT, z, 0xf, "LEA imm", i_lea_lo(0x36, 1, 2, 0x10, 4, 0), ins_exit());
            CHECK(HAS(li, "r1 = ((r2 << 4u) + 0x00000010u)"), "LEA.LO with an immediate B");
            maxwell_free(li);
        }
        /* -- bfe.brev, prmt modes, r2p.cc, indexed attribute loads */
        {
            XL_OK(bf, STAGE_FRAGMENT, z, 0xf, "BFE.BREV", i_bfe(1, 2, 3, 1, 0), i_bfe(4, 2, 3, 1, 1), i_bfe(5, 2, 3, 0, 0), ins_exit());
            CHECK(HAS(bf, "r1 = bfe_u(reverse_bits(r2), r3)") && HAS(bf, "r4 = bfe_s(reverse_bits(r2), r3)") && HAS(bf, "r5 = bfe_u(r2, r3)"),
                  "BFE.BREV reverses the source first");
            maxwell_free(bf);
            XL_OK(pm, STAGE_FRAGMENT, z, 0xf, "PRMT modes", i_prmt(1, 2, 3, 4, 0), i_prmt(5, 2, 3, 4, 1), i_prmt(6, 2, 3, 4, 2), i_prmt(7, 2, 3, 4, 3),
                  i_prmt(8, 2, 3, 4, 4), i_prmt(9, 2, 3, 4, 5), i_prmt(10, 2, 3, 4, 6), ins_exit());
            CHECK(HAS(pm, "r1 = prmt(r2, r4, r3)") && HAS(pm, "r5 = prmt_m(r2, r4, r3, 1u)") && HAS(pm, "r10 = prmt_m(r2, r4, r3, 6u)"),
                  "each mode names its helper variant");
            CHECK(HAS(pm, "if (n & 8u) byte = (byte & 0x80u) ? 0xFFu : 0u"), "selector bit 3 replicates the byte's sign");
            maxwell_free(pm);
            XL_REFUSE(pb, STAGE_FRAGMENT, z, 0xf, "prmt mode 9", "PRMT mode 9", i_prmt(1, 2, 3, 4, 9), ins_exit());
            XL_OK(rp, STAGE_FRAGMENT, z, 0xf, "R2P.CC", i_r2p(2, 3, 1, 1), ins_exit());
            CHECK(HAS(rp, "rv = r2 >> 8u") && HAS(rp, "ccz = (rv & 1u) != 0u") && HAS(rp, "ccs = (rv & 2u) != 0u") && HAS(rp, "cc = (rv >> 2) & 1u") &&
                      HAS(rp, "uint cc = 0;"),
                  "R2P.CC sets the Z, S, C flags from the byte of Ra");
            maxwell_free(rp);
            XL_OK(r0, STAGE_FRAGMENT, z, 0xf, "R2P predicates", i_r2p(2, 3, 0, 0), ins_exit());
            CHECK(HAS(r0, "p6 = ((r2 >> 6u) & 1u) != 0u"), "R2P without .CC still writes the predicates");
            maxwell_free(r0);
            /* ALD a[Ra + imm]: fragment (varying array) and vertex (input table) */
            XL_OK(if1, STAGE_FRAGMENT, z, 0xf, "indexed ALD (fragment)", i_ald_idx(4, 1, 0x80, 2), ins_exit());
            CHECK(HAS(if1, "uint ix = r1;") && HAS(if1, "attr_arr[min(((ix + 128u) - 0x80u) >> 2, 127u)]") &&
                      HAS(if1, "attr_arr[min(((ix + 132u) - 0x80u) >> 2, 127u)]"),
                  "fragment indexed load latches the index");
            maxwell_free(if1);
            XL_OK(iv, STAGE_VERTEX, z, 0, "indexed ALD (vertex)", ins_ald(0, 0x80, 1), i_ald_idx(4, 4, 0x80, 1), ins_exit());
            CHECK(HAS(iv, "uint vattr_arr[128]") && HAS(iv, "vattr_arr[0 + k] = as_type<uint>(in.a0[k])") && HAS(iv, "vattr_arr[4 + k] = 0u"),
                  "vertex inputs are mirrored into the table; undeclared ones read 0");
            CHECK(HAS(iv, "uint ix = r4;") && HAS(iv, "r4 = ((((ix + 128u)) >= 0x80u"), "index latched before the destination, which aliases it, is written");
            maxwell_free(iv);
        }
        /* -- gather with offsets, tld4s with offsets */
        {
            XL_OK(g1, STAGE_FRAGMENT, z, 0xf, "TLD4 AOFFI", i_tld4(0, 8, 12, 8, 1, 0, 0), ins_exit());
            CHECK(HAS(g1, "t0.gather(s0, float2(as_type<float>(r8), as_type<float>(r9)) + float2(int2(int(r12 << 28) >> 28, int(r12 << 24) >> 28)) / "
                          "(float2(t0.get_width(), t0.get_height()) / rscale[1].xy), int2(0), component::x)"),
                  "gather offset folds into the coordinate (whole texels keep the footprint)");
            maxwell_free(g1);
            XL_OK(g2, STAGE_FRAGMENT, z, 0xf, "TLD4 AOFFI + depth compare", i_tld4(0, 8, 12, 8, 1, 0, 1), ins_exit());
            CHECK(HAS(g2, "gather_compare(s0, float2(") && HAS(g2, "as_type<float>(r13)"), "offset register precedes the depth reference");
            maxwell_free(g2);
            XL_REFUSE(g3, STAGE_FRAGMENT, z, 0xf, "per-texel offsets", "TLD4 PTP", i_tld4(0, 8, 12, 8, 2, 0, 0), ins_exit());
            XL_OK(s1, STAGE_FRAGMENT, z, 0xf, "TLD4S AOFFI", i_tld4s(0, 2, 8, 12, 8, 0, 1, 2), ins_exit());
            CHECK(HAS(s1, "float2(as_type<float>(r8), as_type<float>(r9)) + float2(int2(int(r12 << 28) >> 28, int(r12 << 24) >> 28))") &&
                      HAS(s1, "component::z"),
                  "TLD4S offsets from Rb, coordinates from Ra and Ra+1");
            maxwell_free(s1);
            XL_REFUSE(s2, STAGE_FRAGMENT, z, 0xf, "depth reference and offsets", "TLD4S dc+offset", i_tld4s(0, 2, 8, 12, 8, 1, 1, 0), ins_exit());
        }
        /* -- nx-shader round: varyings start at (0,0,0,1) (G7), NaN-exact FMNMX / FSETP.NE (G13), SR_Y_DIRECTION per origin (G14), VMNMX */
        {
            ShaderKey zz = {0};
            XL_OK(g7, STAGE_VERTEX, zz, 0, "partial varying write", ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_ast(0, 0x90, 2), ins_exit());
            CHECK(HAS(g7, "out.v1 = float4(0, 0, 0, 1);") && HAS(g7, "out.pos = float4(0, 0, 0, 1);"),
                  "unwritten varying components default to (0,0,0,1) (G7)");
            maxwell_free(g7);

            XL_OK(mn, STAGE_FRAGMENT, zz, 0xf, "FMNMX", i_fmnmx(0, 1, 2, 7, 0), ins_exit());
            CHECK(HAS(mn, "precise::fmin(") && HAS(mn, "precise::fmax(") && !HAS(mn, " fmin("), "FMNMX uses precise:: min/max");
            maxwell_free(mn);
#ifdef __APPLE__
            const uint32_t qnan = 0x7fc00000u, one = 0x3f800000u, two = 0x40000000u;
            for (int mm = 0; mm < 3; mm++) { /* safe, relaxed, fast: the NaN-sensitive translations hold in every math mode */
                ShaderKey uk = {0};
                uk.rt_kind[0] = 2;
                /* r0 = fmin(NaN, 1) ; r1 = fmax(1, NaN) ; r2 = (NaN NE 1) ? 2 : 1 (ordered: false) ; r3 = (2 NE 1) ? 2 : 1 */
                XL_OK(nf, STAGE_FRAGMENT, uk, 0xf, "NaN FMNMX/FSETP", ins_mov32i(4, qnan), ins_mov32i(5, one), ins_mov32i(6, two), i_fmnmx(0, 4, 5, 7, 0),
                      i_fmnmx(1, 5, 4, 7, 1), i_fsetp(0, 4, 5, 5), i_sel(2, 6, 5, 0), i_fsetp(1, 6, 5, 5), i_sel(3, 6, 5, 1), ins_exit());
                uint32_t o[4] = {0};
                if (nf->msl && run_fs_u4(nf->msl, o, mm))
                    CHECK(o[0] == one && o[1] == one && o[2] == one && o[3] == two, "math mode %d: fmin(NaN,1)=%08x fmax(1,NaN)=%08x NaN.NE=%08x 2.NE.1=%08x",
                          mm, o[0], o[1], o[2], o[3]);
                maxwell_free(nf);
            }
            /* SR_Y_DIRECTION: -1.0 for the lower-left origin (default key), +1.0 for upper-left */
            for (int ul = 0; ul < 2; ul++) {
                ShaderKey uk = {0};
                uk.rt_kind[0] = 2;
                uk.upper_left = ul;
                XL_OK(yd, STAGE_FRAGMENT, uk, 0xf, "S2R SR_Y_DIRECTION", i_s2r(0, 0x12), ins_exit());
                uint32_t o[4] = {0};
                if (yd->msl && run_fs_u4(yd->msl, o, 0)) CHECK(o[0] == (ul ? 0x3f800000u : 0xbf800000u), "SR_Y_DIRECTION upper_left=%d reads %08x", ul, o[0]);
                maxwell_free(yd);
            }
            /* VMNMX: the Animal Crossing word (R0 = max(max.u32(R1, R4), R10)), then byte/half selects, signedness, immediate, merge and accumulate ops */
            {
                ShaderKey uk = {0};
                uk.rt_kind[0] = 2;
                XL_OK(vm, STAGE_FRAGMENT, uk, 0xf, "VMNMX (AC 23e43e94e45bb1d5 @0x3e8)", ins_mov32i(1, 7), ins_mov32i(4, 0x90000000u), ins_mov32i(10, 3),
                      0x3b34056060470100ull, ins_mov32i(1, 0xfffffff0u), ins_mov32i(4, 5), ins_mov32i(10, 0x80000000u),
                      0x3b34056060470101ull, /* r1 = max(max(0xfffffff0, 5), 0x80000000) */
                      ins_mov32i(5, 0x0000ff02u), ins_mov32i(6, 0x00030004u), ins_mov32i(7, 0xaabbccddu),
                      i_vmnmx(2, 5, 6, 7, 1, 1, 4, 1, 1, 0, 1, 0,
                              0), /* min(s8 byte1 of r5 = -1, s16 half0 of r6 = 4) = -1 -> merge low 16 bits into r7: 0xaabbffff */
                      i_vmnmx(3, 5, 0x123, 7, 0, 0, 0, 0, 0, 1, 4, 0, 0), /* max(u8 byte0 of r5 = 2, imm 0x123) = 0x123, + r7 */
                      ins_exit());
                uint32_t o[4] = {0};
                if (vm->msl && run_fs_u4(vm->msl, o, 0))
                    CHECK(o[0] == 0x90000000u && o[1] == 0xfffffff0u && o[2] == 0xaabbffffu && o[3] == 0xaabbccddu + 0x123u,
                          "VMNMX results %08x %08x %08x %08x", o[0], o[1], o[2], o[3]);
                maxwell_free(vm);
                /* mixed signedness: max(s32 -1, u32 5) = 5 ; min(s32 -1, u32 0x80000000) = -1 (exact, 64-bit compare) */
                XL_OK(vx, STAGE_FRAGMENT, uk, 0xf, "VMNMX mixed sign", ins_mov32i(5, 0xffffffffu), ins_mov32i(6, 5), ins_mov32i(7, 0x80000000u),
                      ins_mov32i(9, 0), i_vmnmx(0, 5, 6, 9, 6, 1, 6, 0, 1, 1, 4, 0, 0), i_vmnmx(1, 5, 7, 9, 6, 1, 6, 0, 1, 0, 4, 0, 0), ins_exit());
                if (vx->msl && run_fs_u4(vx->msl, o, 0)) CHECK(o[0] == 5 && o[1] == 0xffffffffu, "VMNMX mixed sign %08x %08x", o[0], o[1]);
                maxwell_free(vx);
            }
#endif
        }
        const char *dir = getenv("NAPPLE_SHADER_CORPUS");
        if (dir) {
            int total = 0, ok = 0, comp = 0;
            DIR *d = opendir(dir);
            struct dirent *de;
            while (d && (de = readdir(d))) {
                size_t n = strlen(de->d_name);
                if (n < 4 || strcmp(de->d_name + n - 4, ".bin")) continue;
                char p[1024];
                snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
                FILE *f = fopen(p, "rb");
                if (!f) continue;
                static uint8_t buf[1 << 20];
                size_t len = fread(buf, 1, sizeof buf, f);
                fclose(f);
                snprintf(p + strlen(p) - 4, 8, ".ctl");
                f = fopen(p, "rb");
                if (f) {
                    uint8_t c[0x880];
                    size_t m = fread(c, 1, sizeof c, f);
                    fclose(f);
                    uint32_t off;
                    if (m > 0x704) {
                        memcpy(&off, c + 0x700, 4);
                        if (off && off <= len) len = off;
                    }
                }
                int stage = strstr(de->d_name, "_ps.") ? STAGE_FRAGMENT : strstr(de->d_name, "_cs.") ? STAGE_COMPUTE : STAGE_VERTEX;
                ShaderKey ck = {0};
                ck.flip_y = false;
                ShaderInfo *r = xl(buf, len, stage, ck);
                total++;
                if (r->msl) {
                    ok++;
#ifdef __APPLE__
                    NSString *ce = nil;
                    if (compiles(r->msl, &ce)) comp++;
                    else printf("  METAL COMPILE FAIL %s: %.200s\n", de->d_name, ce.UTF8String);
#else
                    comp++; /* no Metal here: compile status unknown, not counted as a failure */
#endif
                } else printf("  TRANSLATE FAIL %s: %s\n", de->d_name, r->error);
                maxwell_free(r);
            }
            if (d) closedir(d);
            printf("corpus: %d shaders, %d translated, %d compile with Metal\n", total, ok, comp);
            CHECK(total == 0 || ok * 100 >= total * 95, "translation rate below 95%%");
            CHECK(comp == ok, "%d translated shaders failed Metal compilation", ok - comp);
        }
    }
#ifndef __APPLE__
    printf("t_xlate: %d Metal compile checks skipped (no Metal on this host)\n", g_unverified);
#endif
    printf(g_fail ? "t_xlate: %d FAILED\n" : "t_xlate: ok\n", g_fail);
    return g_fail != 0;
}
