/* t_xlate.m - Maxwell -> MSL translator: golden checks on hand-assembled shaders and, when NAPPLE_SHADER_CORPUS points to a directory of
 * extracted shaders (<name>.bin code + <name>.ctl control, stage from the _vs/_ps/_cs suffix; kept outside the repo), translation rate and
 * Metal compilation of every result. */
#import <Metal/Metal.h>
#include <dirent.h>
#include "shader/maxwell.h"
#include "nvn_test.h"

static id<MTLDevice> g_dev;
static int compiles(const char *msl, NSString **err) {
    NSError *e = nil;
    MTLCompileOptions *o = [MTLCompileOptions new]; o.languageVersion = MTLLanguageVersion3_0;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(msl) options:o error:&e];
    if (!lib && err) *err = e.localizedDescription;
    return lib != nil;
}
static ShaderInfo *xl(uint8_t *blob, size_t n, int stage, ShaderKey key) { return maxwell_translate(blob, n, stage, &key); }

int main(void) {
    @autoreleasepool {
        g_dev = MTLCreateSystemDefaultDevice();
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
            NSString *e = nil; CHECK(compiles(s->msl, &e), "vertex MSL compiles: %s", e.UTF8String);
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
                NSString *e = nil; CHECK(compiles(g->msl, &e), "LDG MSL compiles: %s", e.UTF8String);
            }
            maxwell_free(g);
            maxwell_gmem_base = 0x100000; maxwell_gmem_size = 0x10000;      /* window mode (kept for tools): bounds-checked offsets into one buffer */
            g = xl(code, 0x400, STAGE_VERTEX, gk);
            CHECK(g->msl && strstr(g->msl, "device uchar *gmem [[buffer(") && strstr(g->msl, "ga < 0x10000ul"), "window mode keeps the bounds-checked form");
            if (g->msl) { NSString *e = nil; CHECK(compiles(g->msl, &e), "LDG window MSL compiles: %s", e.UTF8String); }
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
                const char *hi = strstr(l->msl, "r13 = cbr(cb6, (r12) + 28u)"), *lo = strstr(l->msl, "r12 = cbr(cb6, (r12) + 24u)");
                CHECK(hi && lo && hi < lo, "LDC.64 with an aliased address loads the high half before overwriting the address register");
                NSString *e = nil; CHECK(compiles(l->msl, &e), "LDC.64 MSL compiles: %s", e.UTF8String);
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
            NSString *e = nil; CHECK(compiles(s->msl, &e), "fragment MSL compiles: %s", e.UTF8String);
        }
        maxwell_free(s);
        fk.depth_slots = 1;                    /* binding slot 0 holds a depth texture: declared depth2d, result (d, 0, 0, 1) */
        s = xl(code, 0x400, STAGE_FRAGMENT, fk);
        CHECK(s->msl && strstr(s->msl, "depth2d<float> t0") && strstr(s->msl, "float4(t0.sample"), "depth texture sampled as colour");
        if (s->msl) { NSString *e = nil; CHECK(compiles(s->msl, &e), "depth variant compiles: %s", e.UTF8String); }
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
            if (h->msl) { NSString *e = nil; CHECK(compiles(h->msl, &e), "HSETP2 immediate MSL compiles: %s", e.UTF8String); }
            maxwell_free(h);
            uint64_t ip[] = { 0xefa0200008470226ull, 0xe0417fc800b72626ull, ins_exit() };
            build_shader(code, ctl, 1, ip, 3, 0xf);
            ShaderInfo *p = xl(code, 0x400, STAGE_FRAGMENT, hk);
            CHECK(p->msl != NULL, "AL2P + indexed IPA: %s", p->error);
            if (p->msl) { CHECK(strstr(p->msl, "attr_arr["), "indexed varying array"); NSString *e = nil; CHECK(compiles(p->msl, &e), "indexed IPA MSL compiles: %s", e.UTF8String); }
            maxwell_free(p);
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
                if (r->msl) { ok++; NSString *ce = nil; if (compiles(r->msl, &ce)) comp++; else printf("  METAL COMPILE FAIL %s: %.200s\n", de->d_name, ce.UTF8String); }
                else printf("  TRANSLATE FAIL %s: %s\n", de->d_name, r->error);
                maxwell_free(r);
            }
            if (d) closedir(d);
            printf("corpus: %d shaders, %d translated, %d compile with Metal\n", total, ok, comp);
            CHECK(total == 0 || ok * 100 >= total * 95, "translation rate below 95%%");
            CHECK(comp == ok, "%d translated shaders failed Metal compilation", ok - comp);
        }
    }
    printf(g_fail ? "t_xlate: %d FAILED\n" : "t_xlate: ok\n", g_fail);
    return g_fail != 0;
}
