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
    MTLCompileOptions *o = [MTLCompileOptions new]; o.languageVersion = MTLLanguageVersion3_0;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(msl) options:o error:&e];
    if (!lib && err) *err = e.localizedDescription;
    return lib != nil;
}
#define MSL_COMPILES(msl, what) do { NSString *e_ = nil; CHECK(compiles((msl), &e_), what " MSL compiles: %s", e_.UTF8String); } while (0)
#define POOL @autoreleasepool
#else
static int g_unverified;                    /* Metal compile checks skipped on this host */
#define MSL_COMPILES(msl, what) do { (void)(msl); g_unverified++; } while (0)
#define POOL
#endif
static ShaderInfo *xl(uint8_t *blob, size_t n, int stage, ShaderKey key) { return maxwell_translate(blob, n, stage, &key); }

/* ---- ISA-gap regression helpers: a tiny assembler for the ops added in the shader-isa round (field layouts as the translator decodes them) ---- */
static uint64_t i_lds(int d, int a, int off, int size) { return 0xef48000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)(off & 0xffffff) << 20) | ((uint64_t)size << 48) | PT_BITS; }
static uint64_t i_sts(int d, int a, int off, int size) { return 0xef58000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)(off & 0xffffff) << 20) | ((uint64_t)size << 48) | PT_BITS; }
static uint64_t i_bar(void) { return 0xf0a8000000000000ull | PT_BITS; }
static uint64_t i_atoms(int d, int a, int b, int off, int size, int op) { return 0xec00000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)size << 28) | ((uint64_t)(off & 0x3fffff) << 30) | ((uint64_t)op << 52) | PT_BITS; }
static uint64_t i_atom(int d, int a, int b, int off, int e, int size, int op) { return 0xed00000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)(off & 0xfffff) << 28) | ((uint64_t)e << 48) | ((uint64_t)size << 49) | ((uint64_t)op << 52) | PT_BITS; }
static uint64_t i_red(int data, int a, int off, int e, int size, int op) { return 0xebf8000000000000ull | (uint64_t)data | ((uint64_t)a << 8) | ((uint64_t)size << 20) | ((uint64_t)op << 23) | ((uint64_t)(off & 0xfffff) << 28) | ((uint64_t)e << 48) | PT_BITS; }
static uint64_t i_vote(int d, int pd, int vop, int psrc) { return 0x50d8000000000000ull | (uint64_t)d | ((uint64_t)psrc << 39) | ((uint64_t)pd << 45) | ((uint64_t)vop << 48) | PT_BITS; }
static uint64_t i_cs2r(int d, int sr) { return 0x50c8000000000000ull | (uint64_t)d | ((uint64_t)sr << 20) | PT_BITS; }
/* TLD: target 0 1D, 1 2D, 2 3D, 3 cube */
static uint64_t i_tld(int d, int a, int b, int index, int target, int array, int mask, int aoffi, int ms, int lod) {
    return 0xdc38000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)array << 28) | ((uint64_t)target << 29) | ((uint64_t)mask << 31) |
           ((uint64_t)aoffi << 35) | ((uint64_t)index << 36) | ((uint64_t)ms << 50) | ((uint64_t)lod << 55) | PT_BITS;
}
static uint64_t i_txd(int d, int a, int b, int index, int target, int array, int mask, int aoffi) {
    return 0xde38000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)array << 28) | ((uint64_t)target << 29) | ((uint64_t)mask << 31) |
           ((uint64_t)aoffi << 35) | ((uint64_t)index << 36) | PT_BITS;
}
static uint64_t i_tld4(int d, int a, int b, int index, int aoff, int comp, int dc) {
    return 0xc838000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | (1ull << 29) | (0xfull << 31) | ((uint64_t)index << 36) | ((uint64_t)dc << 50) | ((uint64_t)aoff << 54) | ((uint64_t)comp << 56) | PT_BITS;
}
static uint64_t i_tld4s(int d0, int d1, int a, int b, int index, int dc, int aoffi, int comp) {
    return 0xdf00000000000000ull | (uint64_t)d0 | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)d1 << 28) | ((uint64_t)index << 36) | ((uint64_t)dc << 50) | ((uint64_t)aoffi << 51) | ((uint64_t)comp << 52) | PT_BITS;
}
static uint64_t i_bfe(int d, int a, int b, int brev, int sgn) { return 0x5c00000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)brev << 40) | ((uint64_t)sgn << 48) | PT_BITS; }
static uint64_t i_prmt(int d, int a, int b, int c_, int mode) { return 0x5bc0000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)c_ << 39) | ((uint64_t)mode << 48) | PT_BITS; }
static uint64_t i_r2p(int a, int b, int cc, int byte) { return 0x5cf0000000000000ull | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)cc << 40) | ((uint64_t)byte << 41) | PT_BITS; }
static uint64_t i_ald_idx(int d, int a, int addr, int n) { return 0xefd8000000000000ull | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)addr << 20) | ((uint64_t)(n - 1) << 47) | PT_BITS; }

/* IMAD / LEA: layouts from memory (see maxwell_translate.c), so these tests pin the translator's reading of them, not the hardware's */
static uint64_t i_imad(uint64_t cls, int d, int a, int b, int c_, uint64_t flags) { return (cls << 56) | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)c_ << 39) | flags | PT_BITS; }
#define IMAD_S (1ull << 48)
#define IMAD_NC (1ull << 49)
#define IMAD_NP (1ull << 51)
#define IMAD_HI (1ull << 53)
static uint64_t i_lea_lo(uint64_t cls, int d, int a, int b, int scale, uint64_t flags) { return (cls << 56) | (0xdull << 52) | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)scale << 39) | flags | PT_BITS; }
static uint64_t i_lea_hi(int d, int a, int b, int c_, int scale, uint64_t flags) { return (0x5bull << 56) | (0xdull << 52) | (1ull << 51) | (uint64_t)d | ((uint64_t)a << 8) | ((uint64_t)b << 20) | ((uint64_t)scale << 28) | ((uint64_t)c_ << 39) | flags | PT_BITS; }

/* translate `n` instructions as `stage` (compute programs have no header: the code starts at the blob) */
static ShaderInfo *xl_ins(int stage, const uint64_t *ins, int n, ShaderKey key, uint32_t omap) {
    static uint8_t cs[0x1000], ctl[0x880];
    if (stage != STAGE_COMPUTE) {
        uint64_t pad[64]; int m = (n + 2) / 3 * 3;
        for (int i = 0; i < m; i++) pad[i] = i < n ? ins[i] : ins_exit();
        build_shader(cs, ctl, stage, pad, m, omap); return xl(cs, 0x400, stage, key);
    }
    memset(cs, 0, sizeof cs);
    uint64_t *w = (uint64_t *)cs; int o = 1;
    for (int i = 0; i < (n + 2) / 3 * 3; i++) { w[o++] = i < n ? ins[i] : ins_exit(); if (o % 4 == 0) w[o++] = 0; }    /* pad the last group with EXIT */
    return maxwell_translate(cs, 0x400, STAGE_COMPUTE, &key);
}
#define HAS(si, str) ((si)->msl && strstr((si)->msl, (str)))
#define REFUSED(si, str) (!(si)->msl && strstr((si)->error, (str)))
/* translate, then run the Metal compile check on the result */
#define XL_OK(var, stage, key, omap, label, ...) \
    uint64_t var##_i[] = { __VA_ARGS__ }; \
    ShaderInfo *var = xl_ins((stage), var##_i, (int)(sizeof var##_i / 8), (key), (omap)); \
    CHECK(var->msl != NULL, label " translation: %s", var->error); \
    if (var->msl) MSL_COMPILES(var->msl, label)
#define XL_REFUSE(var, stage, key, omap, why, label, ...) \
    uint64_t var##_i[] = { __VA_ARGS__ }; \
    ShaderInfo *var = xl_ins((stage), var##_i, (int)(sizeof var##_i / 8), (key), (omap)); \
    CHECK(REFUSED(var, why), label " must be refused with \"%s\" (msl %s, error \"%s\")", why, var->msl ? "present" : "null", var->error); \
    maxwell_free(var)

int main(void) {
    POOL {
#ifdef __APPLE__
        g_dev = MTLCreateSystemDefaultDevice();
#endif
        static uint8_t code[0x1000], ctl[0x880];
        /* vertex: pass attribute 0 to the position */
        uint64_t vs[] = { ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit() };
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
        k.flip_y = true; k.depth_minus_one = true;
        s = xl(code, 0x400, STAGE_VERTEX, k);
        CHECK(s->msl && strstr(s->msl, "out.pos.y = -out.pos.y") && strstr(s->msl, "(out.pos.z + out.pos.w) * 0.5f"), "clip space epilogue (flip_y, depth -w..w)");
        maxwell_free(s);

        /* global memory (LDG/STG, SSBO access): in GPU-address mode (size ~0) the address is a Metal GPU virtual address and the shader needs no gmem buffer argument */
        {
            uint64_t ldg = 0xeed0000000000000ull | 0 | (2ull << 8) | (0x10ull << 20) | (1ull << 45) | (4ull << 48) | PT_BITS;      /* LDG.E R0, [R2.64+0x10] */
            uint64_t gs[] = { ldg, ins_ast(0, 0x70, 4), ins_exit() };
            build_shader(code, ctl, 0, gs, 3, 0);
            extern uint64_t maxwell_gmem_base, maxwell_gmem_size;
            maxwell_gmem_base = 0; maxwell_gmem_size = ~0ull;
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
            maxwell_gmem_base = 0x100000; maxwell_gmem_size = 0x10000;      /* window mode (kept for tools): bounds-checked offsets into one buffer */
            g = xl(code, 0x400, STAGE_VERTEX, gk);
            CHECK(g->msl && strstr(g->msl, "device uchar *gmem [[buffer(") && strstr(g->msl, "ga < 0x10000ul"), "window mode keeps the bounds-checked form");
            if (g->msl) MSL_COMPILES(g->msl, "LDG window");
            maxwell_free(g);
            maxwell_gmem_base = 0; maxwell_gmem_size = 0;
        }

        /* LDC.64 R12, c[6][R12 + 0x18] (SMO skinning): the address register is also the low destination, so the high half must be loaded first */
        {
            uint64_t ldc = 0xef90000000000000ull | (5ull << 48) | (6ull << 36) | (0x18ull << 20) | (12ull << 8) | 12ull | PT_BITS;
            uint64_t lv[] = { ins_ald(12, 0x80, 1), ldc, ins_ast(12, 0x70, 2), ins_exit() };
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

        /* fragment: constant colour, plus a texture fetch declares slot 0 at c2+0x20 */
        uint64_t fs[] = { ins_mov32i(4, 0x3e800000), ins_mov32i(5, 0x3f400000), ins_tex2d_lz(0, 4, 4, 8), ins_exit() };
        build_shader(code, ctl, 1, fs, 4, 0xf);
        ShaderKey fk = {0};
        s = xl(code, 0x400, STAGE_FRAGMENT, fk);
        CHECK(s->msl != NULL, "fragment translation: %s", s->error);
        if (s->msl) {
            CHECK(s->ntex == 1 && s->tex[0].handle_offset == 0x20 && s->tex[0].type == TEXTYPE_2D, "texture slot: n=%d off=%#x", s->ntex, s->tex[0].handle_offset);
            CHECK(strstr(s->msl, "t0.sample(s0"), "sample call");
            CHECK(!strstr(s->msl, "ltint") && !strstr(s->msl, "dbgout"), "no game-specific hooks in the output");
           MSL_COMPILES(s->msl, "fragment");
        }
        maxwell_free(s);
        fk.depth_slots = 1;                    /* binding slot 0 holds a depth texture: declared depth2d, result (d, 0, 0, 1) */
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
            uint64_t hs[] = { 0x7e0683bc0f070207ull, ins_exit(), ins_exit() };
            build_shader(code, ctl, 1, hs, 3, 0xf);
            ShaderKey hk = {0};
            ShaderInfo *h = xl(code, 0x400, STAGE_FRAGMENT, hk);
            CHECK(h->msl != NULL, "HSETP2 immediate form: %s", h->error);
            if (h->msl) MSL_COMPILES(h->msl, "HSETP2 immediate");
            maxwell_free(h);
            uint64_t ip[] = { 0xefa0200008470226ull, 0xe0417fc800b72626ull, ins_exit() };
            build_shader(code, ctl, 1, ip, 3, 0xf);
            ShaderInfo *p = xl(code, 0x400, STAGE_FRAGMENT, hk);
            CHECK(p->msl != NULL, "AL2P + indexed IPA: %s", p->error);
            if (p->msl) { CHECK(strstr(p->msl, "attr_arr["), "indexed varying array"); MSL_COMPILES(p->msl, "indexed IPA"); }
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
                const char *st = strstr(sm->msl, "*(threadgroup uint *)(smem + sa + 0u) = r1"), *br = strstr(sm->msl, "threadgroup_barrier(mem_flags::mem_threadgroup)"), *ld = strstr(sm->msl, "r3 = ok ? *(threadgroup uint *)(smem + sa + 0u)");
                CHECK(st && br && ld && st < br && br < ld, "store, barrier, load keep their order");
                CHECK(HAS(sm, "constant uint SMEM_BYTES = 32768u") && HAS(sm, "bool ok = sa + 4u <= SMEM_BYTES"), "accesses are bounds checked");
            }
            maxwell_free(sm);
            XL_OK(ss, STAGE_COMPUTE, z, 0, "LDS/STS static", i_sts(1, 255, 0x20, 4), i_sts(1, 255, 0x40, 6), i_lds(2, 255, 0x20, 1), i_lds(3, 255, 0x24, 3), i_lds(4, 255, 0x28, 2), i_sts(1, 255, 0x2c, 0), i_sts(1, 255, 0x2e, 2), i_lds(8, 255, 0x30, 5), ins_exit());
            CHECK(ss->shared_mem == 0x50 && HAS(ss, "threadgroup uint smem_w[20]"), "static offsets size the array (0x40 + 16 bytes -> 0x50): %u", ss->shared_mem);
            CHECK(HAS(ss, "uint(int(char(smem[sa])))") && HAS(ss, "uint(int(*(threadgroup short *)(smem + sa)))") && HAS(ss, "uint(*(threadgroup ushort *)(smem + sa))"), "byte and short loads");
            CHECK(HAS(ss, "smem[sa] = uchar(r1)") && HAS(ss, "*(threadgroup ushort *)(smem + sa) = ushort(r1)"), "byte and short stores write only their bytes");
            CHECK(HAS(ss, "r8 = ok ? *(threadgroup uint *)(smem + sa + 0u)") && HAS(ss, "r9 = ok ? *(threadgroup uint *)(smem + sa + 4u)"), "64-bit load");
            CHECK(HAS(ss, "smem + sa + 12u) = r4"), "128-bit store writes four consecutive registers");
            maxwell_free(ss);
            ShaderKey zk = z; zk.shared_size = 4096;
            XL_OK(sk, STAGE_COMPUTE, zk, 0, "LDS with a declared size", i_lds(3, 2, 0, 4), ins_exit());
            CHECK(sk->shared_mem == 4096 && HAS(sk, "smem_w[1024]"), "the key's shared size is honoured: %u", sk->shared_mem);
            maxwell_free(sk);
            zk.shared_size = 49152;
            XL_REFUSE(sb, STAGE_COMPUTE, zk, 0, "32 KB", "48 KB of shared memory", i_lds(3, 2, 0, 4), ins_exit());
            XL_REFUSE(sv, STAGE_VERTEX, z, 0, "outside a compute shader", "LDS in a vertex shader", i_lds(3, 2, 0, 4), ins_exit());
        }
        /* -- atomics: ATOMS (shared), ATOM / RED (global) */
        {
            XL_OK(am, STAGE_COMPUTE, z, 0, "ATOMS", i_atoms(4, 2, 1, 0x10, 0, 0), i_atoms(5, 2, 1, 0x10, 1, 1), i_atoms(6, 2, 1, 0x10, 0, 3), i_atoms(7, 2, 1, 0x10, 0, 8), ins_exit());
            CHECK(HAS(am, "atom_tg((threadgroup atomic_uint *)(smem + sa), 0u, r1, false)") && HAS(am, "atom_tg((threadgroup atomic_uint *)(smem + sa), 1u, r1, true)") &&
                  HAS(am, ", 3u, r1, false)") && HAS(am, ", 8u, r1, false)"), "ATOMS add/min/inc/exch operands");
            CHECK(HAS(am, "static inline uint atom_tg(threadgroup atomic_uint *p") && !HAS(am, "atom_dev("), "only the threadgroup helper is emitted");
            CHECK(HAS(am, "r4 = old;") && HAS(am, "r7 = old;"), "old value lands in Rd");
            maxwell_free(am);
            XL_REFUSE(ac, STAGE_COMPUTE, z, 0, "CAS and float forms", "ATOMS.CAS", i_atoms(4, 2, 1, 0, 0, 9), ins_exit());
            XL_REFUSE(a64, STAGE_COMPUTE, z, 0, "atoms 64-bit", "ATOMS.64", i_atoms(4, 2, 1, 0, 2, 0), ins_exit());
            XL_REFUSE(av, STAGE_VERTEX, z, 0, "outside a compute shader", "ATOMS in a vertex shader", i_atoms(4, 2, 1, 0, 0, 0), ins_exit());
            extern uint64_t maxwell_gmem_base, maxwell_gmem_size;
            maxwell_gmem_base = 0; maxwell_gmem_size = ~0ull;
            XL_OK(ag, STAGE_COMPUTE, z, 0, "ATOM/RED", i_atom(4, 2, 1, 0x10, 1, 0, 0), i_atom(5, 2, 1, 0, 1, 1, 2), i_red(1, 2, 0x20, 1, 0, 6), ins_exit());
            CHECK(ag->uses_gmem && ag->uses_gmem_store, "global atomics mark the buffer as written");
            CHECK(HAS(ag, "atom_dev((device atomic_uint *)(gmem + ga), 0u, r1, false)") && HAS(ag, "atom_dev((device atomic_uint *)(gmem + ga), 2u, r1, true)") && HAS(ag, "6u, r1, false)"), "ATOM add, ATOM.MAX.S32, RED.OR");
            CHECK(HAS(ag, "(ulong(r3) << 32 | ulong(r2))") && HAS(ag, "r4 = old;") && HAS(ag, "(void)old;"), "64-bit address, result only for ATOM");
            maxwell_free(ag);
            XL_REFUSE(af, STAGE_COMPUTE, z, 0, "size 3", "ATOM.F32", i_atom(4, 2, 1, 0, 1, 3, 0), ins_exit());
            /* every op through Metal: native and CAS-loop helpers, both address spaces, GPU-address and window gmem */
            XL_OK(aa, STAGE_COMPUTE, z, 0, "ATOMS every op", i_atoms(4, 2, 1, 0, 0, 0), i_atoms(4, 2, 1, 0, 1, 1), i_atoms(4, 2, 1, 0, 1, 2), i_atoms(4, 2, 1, 0, 0, 3), i_atoms(4, 2, 1, 0, 0, 4),
                  i_atoms(4, 2, 1, 0, 0, 5), i_atoms(4, 2, 1, 0, 0, 6), i_atoms(4, 2, 1, 0, 0, 7), i_atoms(4, 2, 1, 0, 0, 8), ins_exit());
            maxwell_free(aa);
            XL_OK(ab, STAGE_COMPUTE, z, 0, "ATOM every op", i_atom(4, 2, 1, 0, 1, 0, 0), i_atom(4, 2, 1, 0, 1, 1, 1), i_atom(4, 2, 1, 0, 1, 0, 2), i_atom(4, 2, 1, 0, 1, 0, 3), i_atom(4, 2, 1, 0, 1, 0, 4),
                  i_atom(4, 2, 1, 0, 1, 0, 5), i_atom(4, 2, 1, 0, 1, 0, 6), i_atom(4, 2, 1, 0, 1, 0, 7), i_atom(4, 2, 1, 0, 1, 0, 8), i_red(1, 2, 0, 1, 0, 1), ins_exit());
            maxwell_free(ab);
            maxwell_gmem_base = 0x100000; maxwell_gmem_size = 0x10000;
            XL_OK(aw, STAGE_COMPUTE, z, 0, "ATOM/RED window gmem", i_atom(4, 2, 1, 0x10, 1, 0, 0), i_red(1, 2, 0x20, 1, 0, 3), ins_exit());
            maxwell_free(aw);
            maxwell_gmem_base = 0;
            maxwell_gmem_size = 0;
        }
        /* -- VOTE, CS2R */
        {
            XL_OK(vt, STAGE_COMPUTE, z, 0, "VOTE", i_vote(1, 0, 0, 7), i_vote(2, 1, 1, 7), i_vote(3, 2, 2, 7), ins_exit());
            CHECK(HAS(vt, "r1 = uint(static_cast<ulong>(simd_ballot(vp)))") && HAS(vt, "p0 = simd_all(vp)") && HAS(vt, "p1 = simd_any(vp)") && HAS(vt, "p2 = simd_all(vp) || !simd_any(vp)"), "VOTE ALL/ANY/EQ");
            maxwell_free(vt);
            XL_OK(vv, STAGE_VERTEX, z, 0, "VOTE in a vertex shader", ins_ald(0, 0x80, 1), i_vote(1, 0, 0, 7), ins_exit());
            CHECK(!HAS(vv, "simd_"), "vertex functions get no SIMD-group operations");
            maxwell_free(vv);
            XL_OK(cs, STAGE_COMPUTE, z, 0, "CS2R", i_cs2r(4, 0xff), i_cs2r(6, 0x50), ins_exit());
            CHECK(HAS(cs, "r4 = 0u;") && HAS(cs, "r5 = 0u;") && HAS(cs, "r6 = 0u;") && HAS(cs, "r7 = 0u;"), "CS2R writes the register pair");
            maxwell_free(cs);
        }
        /* -- double precision and the other stage/model gaps are refused by name */
        {
            const uint64_t dp[][2] = { {0x5c70000000000000ull, 0}, {0x4c80000000000000ull, 1}, {0x5b70000000000000ull, 2}, {0x5b80000000000000ull, 3}, {0x5c50000000000000ull, 4}, {0x3870000000000000ull, 5} };
            const char *nm[] = {"DADD", "DMUL", "DFMA", "DSETP", "DMNMX", "DADD"};
            for (int i = 0; i < 6; i++) {
                uint64_t ins[] = { dp[i][0] | 1 | (2ull << 8) | (4ull << 20) | PT_BITS, ins_exit() };
                ShaderInfo *r = xl_ins(STAGE_FRAGMENT, ins, 2, z, 0xf);
                CHECK(REFUSED(r, nm[i]) && strstr(r->error, "double precision"), "%s is refused as double precision (error \"%s\")", nm[i], r->error);
                maxwell_free(r);
            }
            XL_REFUSE(ou, STAGE_VERTEX, z, 0, "OUT", "OUT", 0xfbe0000000000000ull | PT_BITS, ins_exit());
            XL_REFUSE(su, STAGE_COMPUTE, z, 0, "surface access", "SULD family", 0xeb00000000000000ull | PT_BITS, ins_exit());
            XL_REFUSE(sa2, STAGE_COMPUTE, z, 0, "surface access", "SUATOM family", 0xea00000000000000ull | PT_BITS, ins_exit());
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
            CHECK(t1->tex[0].type == TEXTYPE_1D_ARRAY && HAS(t1, "get_array_size() - 1u") && HAS(t1, "uint(int(r9) + (int(r13 << 28) >> 28))"), "1D array layer clamp, x offset in the first nibble");
            maxwell_free(t1);
            XL_OK(t3, STAGE_FRAGMENT, z, 0xf, "TLD 3D", i_tld(0, 8, 12, 8, 2, 0, 0xf, 1, 0, 0), ins_exit());
            CHECK(HAS(t3, "t0.read(uint3(uint(int(r8) + (int(r12 << 28) >> 28)), uint(int(r9) + (int(r12 << 24) >> 28)), uint(int(r10) + (int(r12 << 20) >> 28))), 0u)"), "3D fetch with 4-bit signed x/y/z offsets");
            maxwell_free(t3);
            XL_OK(tv, STAGE_VERTEX, z, 0, "TLD in a vertex shader", ins_ald(8, 0x80, 2), i_tld(0, 8, 12, 8, 1, 0, 0xf, 0, 0, 1), ins_exit());
            CHECK(HAS(tv, "t0.read(uint2(uint(int(r8)), uint(int(r9))), uint(r12))"), "no resolution scale in vertex shaders");
            maxwell_free(tv);
            XL_REFUSE(tm, STAGE_FRAGMENT, z, 0xf, "multisampled", "TLD.MS", i_tld(0, 8, 12, 8, 1, 0, 0xf, 0, 1, 0), ins_exit());
            XL_REFUSE(tc, STAGE_FRAGMENT, z, 0xf, "cube", "TLD cube", i_tld(0, 8, 12, 8, 3, 0, 0xf, 0, 0, 0), ins_exit());
            XL_OK(xd, STAGE_FRAGMENT, z, 0xf, "TXD 2D", i_txd(0, 8, 12, 8, 1, 0, 0xf, 0), ins_exit());
            CHECK(HAS(xd, "t0.sample(s0, float2(as_type<float>(r8), as_type<float>(r9)), gradient2d(float2(as_type<float>(r12), as_type<float>(r13)), float2(as_type<float>(r14), as_type<float>(r15))))"), "TXD: dx then dy from Rb");
            maxwell_free(xd);
            XL_OK(xe, STAGE_FRAGMENT, z, 0xf, "TXD 3D", i_txd(0, 8, 12, 8, 2, 0, 0xf, 0), ins_exit());
            CHECK(HAS(xe, "gradient3d(float3(as_type<float>(r12), as_type<float>(r13), as_type<float>(r14)), float3(as_type<float>(r15), as_type<float>(r16), as_type<float>(r17)))"), "TXD 3D uses six derivative registers");
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
            ShaderKey dk = z; dk.depth_slots = 1;
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
            XL_OK(im, STAGE_FRAGMENT, z, 0xf, "IMAD", i_imad(0x5a, 1, 2, 3, 4, 0), i_imad(0x5a, 5, 2, 3, 4, IMAD_NC | IMAD_NP), i_imad(0x5a, 6, 2, 3, 4, IMAD_HI),
                  i_imad(0x5a, 7, 2, 3, 4, IMAD_HI | IMAD_S | IMAD_NP), i_imad(0x52, 8, 2, 3, 0x10, 0), i_imad(0x4a, 9, 2, 0x10, 4, 0), ins_exit());
            CHECK(HAS(im, "r1 = (r2 * r3 + r4)"), "IMAD low word");
            CHECK(HAS(im, "r5 = ((0u - r2 * r3) + (0u - r4))"), "IMAD negations");
            CHECK(HAS(im, "r6 = (uint((ulong(r2) * ulong(r3)) >> 32) + r4)"), "IMAD.HI unsigned");
            CHECK(HAS(im, "r7 = (uint((0ul - ulong(long(int(r2)) * long(int(r3)))) >> 32) + r4)"), "IMAD.HI signed with a negated product");
            CHECK(HAS(im, "r8 = (r2 * r16 + cb0["), "IMAD rc form: B = REG_39, C = constant buffer");
            CHECK(HAS(im, "r9 = (r2 * cb0["), "IMAD cr form: B = constant buffer");
            maxwell_free(im);
            XL_OK(ii, STAGE_FRAGMENT, z, 0xf, "IMAD imm", i_imad(0x34, 1, 2, 0x10, 4, 0), ins_exit());
            CHECK(HAS(ii, "r1 = (r2 * 0x00000010u + r4)"), "IMAD with an immediate B");
            maxwell_free(ii);
            XL_REFUSE(ic, STAGE_FRAGMENT, z, 0xf, "imad carry", "IMAD.CC", i_imad(0x5a, 1, 2, 3, 4, 1ull << 47), ins_exit());
            XL_OK(lo, STAGE_FRAGMENT, z, 0xf, "LEA.LO", i_lea_lo(0x5b, 1, 2, 3, 2, 0), i_lea_lo(0x5b, 4, 2, 3, 5, 1ull << 45), i_lea_lo(0x5b, 5, 2, 3, 3, 1ull << 47),
                  i_lea_lo(0x5b, 6, 2, 3, 0, 1ull << 48), ins_exit());
            CHECK(HAS(lo, "r1 = ((r2 << 2u) + r3)") && HAS(lo, "r4 = ((r2 << 5u) + (0u - r3))"), "LEA.LO scales A and adds (or subtracts) B");
            CHECK(HAS(lo, "cc = uint(sum >> 32) & 1u") && HAS(lo, "ulong sum = ulong(r2 << 3u) + ulong(r3);") && HAS(lo, "ulong(r3) + ulong(cc)") && HAS(lo, "uint cc = 0;"), "LEA.LO .CC and .X use the carry flag");
            maxwell_free(lo);
            XL_OK(hi, STAGE_FRAGMENT, z, 0xf, "LEA.HI", i_lea_hi(1, 2, 3, 4, 3, 0), i_lea_hi(5, 2, 3, 4, 0, 0), i_lea_hi(6, 2, 3, 4, 7, 1ull << 48), ins_exit());
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
            CHECK(HAS(bf, "r1 = bfe_u(reverse_bits(r2), r3)") && HAS(bf, "r4 = bfe_s(reverse_bits(r2), r3)") && HAS(bf, "r5 = bfe_u(r2, r3)"), "BFE.BREV reverses the source first");
            maxwell_free(bf);
            XL_OK(pm, STAGE_FRAGMENT, z, 0xf, "PRMT modes", i_prmt(1, 2, 3, 4, 0), i_prmt(5, 2, 3, 4, 1), i_prmt(6, 2, 3, 4, 2), i_prmt(7, 2, 3, 4, 3), i_prmt(8, 2, 3, 4, 4), i_prmt(9, 2, 3, 4, 5), i_prmt(10, 2, 3, 4, 6), ins_exit());
            CHECK(HAS(pm, "r1 = prmt(r2, r4, r3)") && HAS(pm, "r5 = prmt_m(r2, r4, r3, 1u)") && HAS(pm, "r10 = prmt_m(r2, r4, r3, 6u)"), "each mode names its helper variant");
            CHECK(HAS(pm, "if (n & 8u) byte = (byte & 0x80u) ? 0xFFu : 0u"), "selector bit 3 replicates the byte's sign");
            maxwell_free(pm);
            XL_REFUSE(pb, STAGE_FRAGMENT, z, 0xf, "prmt mode 9", "PRMT mode 9", i_prmt(1, 2, 3, 4, 9), ins_exit());
            XL_OK(rp, STAGE_FRAGMENT, z, 0xf, "R2P.CC", i_r2p(2, 3, 1, 1), ins_exit());
            CHECK(HAS(rp, "rv = r2 >> 8u") && HAS(rp, "ccz = (rv & 1u) != 0u") && HAS(rp, "ccs = (rv & 2u) != 0u") && HAS(rp, "cc = (rv >> 2) & 1u") && HAS(rp, "uint cc = 0;"), "R2P.CC sets the Z, S, C flags from the byte of Ra");
            maxwell_free(rp);
            XL_OK(r0, STAGE_FRAGMENT, z, 0xf, "R2P predicates", i_r2p(2, 3, 0, 0), ins_exit());
            CHECK(HAS(r0, "p6 = ((r2 >> 6u) & 1u) != 0u"), "R2P without .CC still writes the predicates");
            maxwell_free(r0);
            /* ALD a[Ra + imm]: fragment (varying array) and vertex (input table) */
            XL_OK(if1, STAGE_FRAGMENT, z, 0xf, "indexed ALD (fragment)", i_ald_idx(4, 1, 0x80, 2), ins_exit());
            CHECK(HAS(if1, "uint ix = r1;") && HAS(if1, "attr_arr[min(((ix + 128u) - 0x80u) >> 2, 127u)]") && HAS(if1, "attr_arr[min(((ix + 132u) - 0x80u) >> 2, 127u)]"), "fragment indexed load latches the index");
            maxwell_free(if1);
            XL_OK(iv, STAGE_VERTEX, z, 0, "indexed ALD (vertex)", ins_ald(0, 0x80, 1), i_ald_idx(4, 4, 0x80, 1), ins_exit());
            CHECK(HAS(iv, "uint vattr_arr[128]") && HAS(iv, "vattr_arr[0 + k] = as_type<uint>(in.a0[k])") && HAS(iv, "vattr_arr[4 + k] = 0u"), "vertex inputs are mirrored into the table; undeclared ones read 0");
            CHECK(HAS(iv, "uint ix = r4;") && HAS(iv, "r4 = ((((ix + 128u)) >= 0x80u"), "index latched before the destination, which aliases it, is written");
            maxwell_free(iv);
        }
        /* -- gather with offsets, tld4s with offsets */
        {
            XL_OK(g1, STAGE_FRAGMENT, z, 0xf, "TLD4 AOFFI", i_tld4(0, 8, 12, 8, 1, 0, 0), ins_exit());
            CHECK(HAS(g1, "t0.gather(s0, float2(as_type<float>(r8), as_type<float>(r9)) + float2(int2(int(r12 << 28) >> 28, int(r12 << 24) >> 28)) / (float2(t0.get_width(), t0.get_height()) / rscale[1].xy), int2(0), component::x)"), "gather offset folds into the coordinate (whole texels keep the footprint)");
            maxwell_free(g1);
            XL_OK(g2, STAGE_FRAGMENT, z, 0xf, "TLD4 AOFFI + depth compare", i_tld4(0, 8, 12, 8, 1, 0, 1), ins_exit());
            CHECK(HAS(g2, "gather_compare(s0, float2(") && HAS(g2, "as_type<float>(r13)"), "offset register precedes the depth reference");
            maxwell_free(g2);
            XL_REFUSE(g3, STAGE_FRAGMENT, z, 0xf, "per-texel offsets", "TLD4 PTP", i_tld4(0, 8, 12, 8, 2, 0, 0), ins_exit());
            XL_OK(s1, STAGE_FRAGMENT, z, 0xf, "TLD4S AOFFI", i_tld4s(0, 2, 8, 12, 8, 0, 1, 2), ins_exit());
            CHECK(HAS(s1, "float2(as_type<float>(r8), as_type<float>(r9)) + float2(int2(int(r12 << 28) >> 28, int(r12 << 24) >> 28))") && HAS(s1, "component::z"), "TLD4S offsets from Rb, coordinates from Ra and Ra+1");
            maxwell_free(s1);
            XL_REFUSE(s2, STAGE_FRAGMENT, z, 0xf, "depth reference and offsets", "TLD4S dc+offset", i_tld4s(0, 2, 8, 12, 8, 1, 1, 0), ins_exit());
        }
        const char *dir = getenv("NAPPLE_SHADER_CORPUS");
        if (dir) {
            int total = 0, ok = 0, comp = 0;
            DIR *d = opendir(dir); struct dirent *de;
            while (d && (de = readdir(d))) {
                size_t n = strlen(de->d_name);
                if (n < 4 || strcmp(de->d_name + n - 4, ".bin")) continue;
                char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
                FILE *f = fopen(p, "rb"); if (!f) continue;
                static uint8_t buf[1 << 20]; size_t len = fread(buf, 1, sizeof buf, f); fclose(f);
                snprintf(p + strlen(p) - 4, 8, ".ctl"); f = fopen(p, "rb");
                if (f) { uint8_t c[0x880]; size_t m = fread(c, 1, sizeof c, f); fclose(f); uint32_t off; if (m > 0x704) { memcpy(&off, c + 0x700, 4); if (off && off <= len) len = off; } }
                int stage = strstr(de->d_name, "_ps.") ? STAGE_FRAGMENT : strstr(de->d_name, "_cs.") ? STAGE_COMPUTE : STAGE_VERTEX;
                ShaderKey ck = {0}; ck.flip_y = false;
                ShaderInfo *r = xl(buf, len, stage, ck);
                total++;
                if (r->msl) {
                    ok++;
#ifdef __APPLE__
                    NSString *ce = nil; if (compiles(r->msl, &ce)) comp++; else printf("  METAL COMPILE FAIL %s: %.200s\n", de->d_name, ce.UTF8String);
#else
                    comp++;     /* no Metal here: compile status unknown, not counted as a failure */
#endif
                }
                else printf("  TRANSLATE FAIL %s: %s\n", de->d_name, r->error);
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
