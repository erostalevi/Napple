/* t_sep_sampler.c - separate texture / sampler handles (nvnCommandBufferBindSeparateTexture / Sampler, nvnDeviceGetSeparate*Handle): the fragment program
 * builds the handle as GLSLC does for sampler2D(texture, sampler): two 32-bit words loaded from constant buffers and OR-ed (or added), then a bindless
 * TEX. The translator records the pair (ShaderTexture.separate) and the replay ORs the two words from c2's separate tables (SEP_TEX_C2_BASE /
 * SEP_SAMP_C2_BASE, a hypothesis: see maxwell.h) or from a user uniform buffer.
 * The texture's first row is red, green, blue, white; the program samples u = -0.125 (texel -1 of row 0): a REPEAT sampler reads texel 3 (white), a
 * CLAMP_TO_EDGE one texel 0 (red), so the colour tells which sampler the handle selected.
 *  1. c2 words, LOP.OR: separate texture 0 + separate sampler 0 (clamp) -> red; rebinding sampler 0 to the repeat sampler -> white;
 *  2. user uniform buffer words (c3[0] | c3[4]), IADD: texture 1 + clamp sampler -> red;
 *  3. an RGBA8UI texture at separate texture binding 1 (ShaderKey.sep_uint_slots): declared texture2d<uint>, the texel's integers (1, 2, 3, 4);
 *  4. a stencil-mode view of a DEPTH24_STENCIL8 texture at separate texture binding 2 (sep_stencil_slots): texture2d<uint>, the stencil value 0x5a.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"
#include "shader/maxwell.h"

#define W 8
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_UBO = 0x8000, OFF_T = 0x100000, OFF_TU = 0x110000, OFF_DS = 0x120000, OFF_RT = 0x160000, OFF_RT4 = 0x170000,
       OFF_TP = 0x180000, OFF_SP = 0x190000, OFF_RB = H_READBACK };
#define LDC(d, idx, off) (0xef90000000000000ull | (uint64_t)(d) | (255ull << 8) | ((uint64_t)(off) << 20) | ((uint64_t)(idx) << 36) | (4ull << 48) | PT_BITS)
#define LOP_OR(d, a, b) (0x5c40000000000000ull | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | (1ull << 41) | (7ull << 48) | PT_BITS)
#define IADD(d, a, b) (0x5c10000000000000ull | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | PT_BITS)
/* TEX.B 2D LZ: coordinates in Ra, Ra+1, handle in Rb, four components to Rd.. */
#define TEX_B(d, a, b) (0xdeb8000000000000ull | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | (1ull << 29) | (0xfull << 31) | (1ull << 37) | PT_BITS)

static H h;
static NVNtexturePool *tp;
static NVNsamplerPool *sp;
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static NVNprogram *prog_of(const uint64_t *fs, int n, uint8_t *vctl, uint8_t *fctl, size_t off) {
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    build_shader(h.mem + OFF_SH + off, vctl, 0, vs_i, 3, 0);
    build_shader(h.mem + OFF_SH + off + 0x1000, fctl, 1, fs, n, 0xf);
    NVNprogram *p = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(p, h.dev);
    NVNshaderData sd[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + off), vctl}, {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + off + 0x1000), fctl}};
    CHECK(L(nvnProgramSetShaders)(p, 2, sd), "shaders");
    return p;
}
static uint32_t draw(NVNprogram *prog, NVNtexture *rt, void (*bind)(void)) {
    NVNcommandBuffer *cb = h.cb;
    h_begin(&h);
    const NVNtexture *rts[1] = {rt};
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, W, W);
    L(nvnCommandBufferSetScissor)(cb, 0, 0, W, W);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnDepthStencilStateSetDepthWriteEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0);
    NVNvertexStreamState vst; L(nvnVertexStreamStateSetDefaults)(&vst); L(nvnVertexStreamStateSetStride)(&vst, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vst);
    L(nvnCommandBufferSetTexturePool)(cb, tp);
    L(nvnCommandBufferSetSamplerPool)(cb, sp);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 48);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    bind();
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    memset(h.mem + OFF_RB, 0xcd, W * W * 16);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    return ((const uint32_t *)(h.mem + OFF_RB))[3 * W + 3];
}
static uint32_t g_tex0, g_tex1, g_clamp, g_repeat;
static void bind_c2_clamp(void) {
    NVNseparateTextureHandle t = L(nvnDeviceGetSeparateTextureHandle)(h.dev, (int)g_tex0);
    NVNseparateSamplerHandle s = L(nvnDeviceGetSeparateSamplerHandle)(h.dev, (int)g_clamp);
    L(nvnCommandBufferBindSeparateTexture)(h.cb, NVN_SHADER_STAGE_FRAGMENT, 0, t);
    L(nvnCommandBufferBindSeparateSampler)(h.cb, NVN_SHADER_STAGE_FRAGMENT, 0, s);
}
static void bind_c2_repeat(void) {
    NVNseparateTextureHandle t = L(nvnDeviceGetSeparateTextureHandle)(h.dev, (int)g_tex0);
    NVNseparateSamplerHandle s = L(nvnDeviceGetSeparateSamplerHandle)(h.dev, (int)g_repeat);
    L(nvnCommandBufferBindSeparateTextures)(h.cb, NVN_SHADER_STAGE_FRAGMENT, 0, 1, &t);
    L(nvnCommandBufferBindSeparateSamplers)(h.cb, NVN_SHADER_STAGE_FRAGMENT, 0, 1, &s);
}
static uint32_t g_tex_u, g_tex_s;
static void bind_c2_int(void) { /* separate texture bindings 1 (integer) and 2 (stencil view), sampler 0 */
    NVNseparateTextureHandle t[2] = {L(nvnDeviceGetSeparateTextureHandle)(h.dev, (int)g_tex_u), L(nvnDeviceGetSeparateTextureHandle)(h.dev, (int)g_tex_s)};
    NVNseparateSamplerHandle s = L(nvnDeviceGetSeparateSamplerHandle)(h.dev, (int)g_clamp);
    L(nvnCommandBufferBindSeparateTextures)(h.cb, NVN_SHADER_STAGE_FRAGMENT, 1, 2, t);
    L(nvnCommandBufferBindSeparateSampler)(h.cb, NVN_SHADER_STAGE_FRAGMENT, 0, s);
}
static void bind_ubo(void) { L(nvnCommandBufferBindUniformBuffer)(h.cb, NVN_SHADER_STAGE_FRAGMENT, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_UBO), 256); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    float tri[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    memcpy(h.mem + OFF_VB, tri, sizeof tri);
    /* row 0 of a block-linear RGBA8 texture: its first 16 bytes are the first four texels, in order */
    const uint32_t row0[4] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu};
    for (int k = 0; k < 2; k++) {
        memset(h.mem + OFF_T + 0x4000 * k, 0, 0x4000);
        memcpy(h.mem + OFF_T + 0x4000 * k, row0, sizeof row0);
    }
    for (int i = 0; i < 0x1000; i += 4) memcpy(h.mem + OFF_TU + i, "\x01\x02\x03\x04", 4); /* written before first use: the replay write-protects used storage */
    NVNtexture *t0 = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 4, 1, OFF_T, 0);
    NVNtexture *t1 = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 4, 1, OFF_T + 0x4000, 0);
    NVNtexture *rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, W, W, 1, OFF_RT, 0);
    tp = calloc(1, sizeof(NVNtexturePool));
    sp = calloc(1, sizeof(NVNsamplerPool));
    CHECK(L(nvnTexturePoolInitialize)(tp, h.pool, OFF_TP, 512), "texture pool");
    CHECK(L(nvnSamplerPoolInitialize)(sp, h.pool, OFF_SP, 64), "sampler pool");
    g_tex0 = 300;
    g_tex1 = 301;
    L(nvnTexturePoolRegisterTexture)(tp, (int)g_tex0, t0, NULL);
    L(nvnTexturePoolRegisterTexture)(tp, (int)g_tex1, t1, NULL);
    NVNsamplerBuilder smb; L(nvnSamplerBuilderSetDevice)(&smb, h.dev); L(nvnSamplerBuilderSetDefaults)(&smb);
    L(nvnSamplerBuilderSetMinMagFilter)(&smb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    L(nvnSamplerBuilderSetWrapMode)(&smb, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE);
    NVNsampler *clamp = calloc(1, sizeof(NVNsampler)); L(nvnSamplerInitialize)(clamp, &smb);
    L(nvnSamplerBuilderSetWrapMode)(&smb, NVN_WRAP_MODE_REPEAT, NVN_WRAP_MODE_REPEAT, NVN_WRAP_MODE_REPEAT);
    NVNsampler *repeat = calloc(1, sizeof(NVNsampler)); L(nvnSamplerInitialize)(repeat, &smb);
    g_clamp = 7;
    g_repeat = 9;
    L(nvnSamplerPoolRegisterSampler)(sp, (int)g_clamp, clamp);
    L(nvnSamplerPoolRegisterSampler)(sp, (int)g_repeat, repeat);

    /* 1. words from c2: separate texture slot 0, separate sampler slot 0, OR-ed */
    uint64_t f1[] = {ins_mov32i(4, fbits(-0.125f)), ins_mov32i(5, fbits(0.125f)), LDC(10, 2, SEP_TEX_C2_BASE), LDC(11, 2, SEP_SAMP_C2_BASE), LOP_OR(12, 10, 11),
                     TEX_B(0, 4, 12), ins_exit()};
    static uint8_t ctl[4][0x880];
    NVNprogram *p1 = prog_of(f1, 7, ctl[0], ctl[1], 0);
    uint32_t a = draw(p1, rt, bind_c2_clamp), b = draw(p1, rt, bind_c2_repeat);
    printf("c2 pair: clamp sampler %08x, repeat sampler %08x\n", a, b);
    CHECK(a == row0[0], "separate texture + clamp sampler (c2 words): texel 0 (red) at u = -0.125: %08x", a);
    CHECK(b == row0[3], "separate texture + repeat sampler (c2 words): texel 3 (white) at u = -0.125: %08x", b);
    /* 2. words from a user uniform buffer (c3[0] texture, c3[4] sampler), added */
    uint32_t *ubo = (uint32_t *)(h.mem + OFF_UBO);
    ubo[0] = (uint32_t)L(nvnDeviceGetSeparateTextureHandle)(h.dev, (int)g_tex1).value;
    ubo[1] = (uint32_t)L(nvnDeviceGetSeparateSamplerHandle)(h.dev, (int)g_clamp).value;
    uint64_t f2[] = {ins_mov32i(4, fbits(-0.125f)), ins_mov32i(5, fbits(0.125f)), LDC(10, 3, 0), LDC(11, 3, 4), IADD(12, 10, 11), TEX_B(0, 4, 12), ins_exit()};
    NVNprogram *p2 = prog_of(f2, 7, ctl[2], ctl[3], 0x2000);
    uint32_t c = draw(p2, rt, bind_ubo);
    printf("uniform buffer pair: %08x\n", c);
    CHECK(c == row0[0], "separate handles from a uniform buffer (IADD): texture 1 + clamp sampler: %08x", c);
    /* 3, 4: integer and stencil textures at separate bindings 1 and 2 (the masks the replay fills from the separate bindings) */
    NVNtexture *tu = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8UI, 4, 4, 1, OFF_TU, 0);
    NVNtexture *ds = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24_STENCIL8, W, W, 1, OFF_DS, 0);
    NVNtexture *rt4 = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA32UI, W, W, 1, OFF_RT4, 0);
    h_begin(&h);
    L(nvnCommandBufferSetRenderTargets)(h.cb, 0, NULL, NULL, ds, NULL);
    L(nvnCommandBufferSetViewport)(h.cb, 0, 0, W, W);
    L(nvnCommandBufferSetScissor)(h.cb, 0, 0, W, W);
    L(nvnCommandBufferClearDepthStencil)(h.cb, 0.25f, 1, 0x5A, 0xff);
    h_submit(&h);
    NVNtextureView sv; L(nvnTextureViewSetDefaults)(&sv); L(nvnTextureViewSetDepthStencilMode)(&sv, NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);
    g_tex_u = 302;
    g_tex_s = 303;
    L(nvnTexturePoolRegisterTexture)(tp, (int)g_tex_u, tu, NULL);
    L(nvnTexturePoolRegisterTexture)(tp, (int)g_tex_s, ds, &sv);
    static uint8_t ctl2[4][0x880];
    uint64_t f3[] = {ins_mov32i(4, fbits(0.5f)), ins_mov32i(5, fbits(0.5f)), LDC(10, 2, SEP_TEX_C2_BASE + 8), LDC(11, 2, SEP_SAMP_C2_BASE), LOP_OR(12, 10, 11),
                     TEX_B(0, 4, 12), ins_exit()};
    uint64_t f4[] = {ins_mov32i(4, fbits(0.5f)), ins_mov32i(5, fbits(0.5f)), LDC(10, 2, SEP_TEX_C2_BASE + 16), LDC(11, 2, SEP_SAMP_C2_BASE), LOP_OR(12, 11, 10),
                     TEX_B(0, 4, 12), ins_exit()};
    NVNprogram *p3 = prog_of(f3, 7, ctl2[0], ctl2[1], 0x4000), *p4 = prog_of(f4, 7, ctl2[2], ctl2[3], 0x6000);
    draw(p3, rt4, bind_c2_int);
    const uint32_t *q = (const uint32_t *)(h.mem + OFF_RB) + (3 * W + 3) * 4;
    printf("separate RGBA8UI texture: %x %x %x %x\n", q[0], q[1], q[2], q[3]);
    CHECK(q[0] == 1 && q[1] == 2 && q[2] == 3 && q[3] == 4, "integer texture at a separate binding reads integers: %x %x %x %x", q[0], q[1], q[2], q[3]);
    draw(p4, rt4, bind_c2_int);
    printf("separate stencil view: %#x\n", q[0]);
    CHECK(q[0] == 0x5A, "stencil view at a separate binding (sampler word first in the OR) reads the stencil value: %#x", q[0]);
    return h_finish("t_sep_sampler", &h);
}
