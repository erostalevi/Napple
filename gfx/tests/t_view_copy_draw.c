/* t_view_copy_draw.c - texture views Metal cannot make, through the replay: a fragment program samples depth textures through format views and writes the
 * texel's register bits to an R32UI target.
 *  1. R32UI view of Depth32F: sampled as depth, the register holds the depth float's bits (what the console's integer view returns);
 *  2. R16UI view of Depth16 (a copy bound as a uint slot): cleared to 0 in one submission and sampled; then cleared to 1.0 and sampled in the SAME command
 *     buffer after the target pass is open: the bind finds the copy stale, the replay closes the pass, copies and reopens it (mtl_replay.m prepare_draw), and
 *     draws on both sides of the split land in the same target;
 *  3. TLD.MS of a 4x Depth32F through an R32UI view (sampled as multisampled depth);
 *  4. the R32F view of depth reads the depth value; an RGBA8 view of Depth32F is a copy of the bytes, sampled as unorm colour.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 16
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_DS = 0x100000, OFF_D16 = 0x110000, OFF_MSD = 0x120000, OFF_RT = 0x160000, OFF_TP = 0x180000, OFF_SP = 0x190000, OFF_RB = H_READBACK };
#define TLD_MS(d, a, b, index) (0xdc38000000000000ull | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | (1ull << 29) | (0xfull << 31) | ((uint64_t)(index) << 36) | (1ull << 50) | PT_BITS)

static H h;
static NVNtexturePool *tp;
static NVNsamplerPool *sp;

static void setup(NVNtexture *rt) {
    NVNcommandBuffer *cb = h.cb;
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
    NVNvertexStreamState vs; L(nvnVertexStreamStateSetDefaults)(&vs); L(nvnVertexStreamStateSetStride)(&vs, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vs);
    L(nvnCommandBufferSetTexturePool)(cb, tp);
    L(nvnCommandBufferSetSamplerPool)(cb, sp);
}
static void clear_depth(NVNtexture *ds, float d) {
    L(nvnCommandBufferSetRenderTargets)(h.cb, 0, NULL, NULL, ds, NULL);
    L(nvnCommandBufferSetViewport)(h.cb, 0, 0, W, W);
    L(nvnCommandBufferSetScissor)(h.cb, 0, 0, W, W);
    L(nvnCommandBufferClearDepthStencil)(h.cb, d, 1, 0, 0);
}
/* vertex buffer: 0 = full-screen triangle, 48 = triangle over the left half only */
static void draw(NVNprogram *prog, uint64_t handle, int left_half) {
    L(nvnCommandBufferBindVertexBuffer)(h.cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB + (left_half ? 48 : 0)), 48);
    L(nvnCommandBufferBindProgram)(h.cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferBindTexture)(h.cb, NVN_SHADER_STAGE_FRAGMENT, 0, handle);
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
}
static uint32_t px(int x, int y) { return ((const uint32_t *)(h.mem + OFF_RB))[y * W + x]; }
static NVNprogram *prog_of(const uint64_t *fs, int n, uint8_t *vctl, uint8_t *fctl, uint32_t off) {
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    build_shader(h.mem + OFF_SH + off, vctl, 0, vs_i, 3, 0);
    build_shader(h.mem + OFF_SH + off + 0x1000, fctl, 1, fs, n, 0xf);
    NVNprogram *p = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(p, h.dev);
    NVNshaderData sd[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + off), vctl}, {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + off + 0x1000), fctl}};
    CHECK(L(nvnProgramSetShaders)(p, 2, sd), "shaders");
    return p;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    float tri[24] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1, /* left half: x in [-1, 0] */ -1, -1, 0, 1, 0, -1, 0, 1, -1, 3, 0, 1};
    memcpy(h.mem + OFF_VB, tri, sizeof tri);
    /* TEX at (0.5, 0.5) of binding 0; TLD.MS sample 2 of texel (3, 3); constant 0x1234 */
    uint64_t tex_i[] = {ins_mov32i(4, 0x3f000000), ins_mov32i(5, 0x3f000000), ins_tex2d_lz(0, 4, 4, 8), ins_exit()};
    uint64_t tld_i[] = {ins_mov32i(4, 3), ins_mov32i(5, 3), ins_mov32i(6, 2), TLD_MS(0, 4, 6, 8), ins_exit()};
    uint64_t k_i[] = {ins_mov32i(0, 0x1234), ins_mov32i(1, 0), ins_mov32i(2, 0), ins_mov32i(3, 0), ins_exit()};
    static uint8_t c[6][0x880];
    NVNprogram *ptex = prog_of(tex_i, 4, c[0], c[1], 0), *ptld = prog_of(tld_i, 5, c[2], c[3], 0x2000), *pk = prog_of(k_i, 5, c[4], c[5], 0x4000);

    NVNtexture *ds = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH32F, W, W, 1, OFF_DS, 0);
    NVNtexture *d16 = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH16, W, W, 1, OFF_D16, 0);
    NVNtexture *rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32UI, W, W, 1, OFF_RT, 0);
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h.dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D_MULTISAMPLE); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_DEPTH32F);
    L(nvnTextureBuilderSetSize2D)(&tb, 8, 8); L(nvnTextureBuilderSetSamples)(&tb, 4); L(nvnTextureBuilderSetStorage)(&tb, h.pool, OFF_MSD);
    NVNtexture *msd = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(msd, &tb), "4x depth texture");

    tp = calloc(1, sizeof(NVNtexturePool));
    sp = calloc(1, sizeof(NVNsamplerPool));
    CHECK(L(nvnTexturePoolInitialize)(tp, h.pool, OFF_TP, 512), "texture pool");
    CHECK(L(nvnSamplerPoolInitialize)(sp, h.pool, OFF_SP, 64), "sampler pool");
    NVNtextureView vu; L(nvnTextureViewSetDefaults)(&vu); L(nvnTextureViewSetFormat)(&vu, NVN_FORMAT_R32UI);
    NVNtextureView vf; L(nvnTextureViewSetDefaults)(&vf); L(nvnTextureViewSetFormat)(&vf, NVN_FORMAT_R32F);
    L(nvnTexturePoolRegisterTexture)(tp, 300, ds, &vu);
    L(nvnTexturePoolRegisterTexture)(tp, 301, ds, &vf);
    L(nvnTexturePoolRegisterTexture)(tp, 302, msd, &vu);
    NVNtextureView v16; L(nvnTextureViewSetDefaults)(&v16); L(nvnTextureViewSetFormat)(&v16, NVN_FORMAT_R16UI);
    NVNtextureView v8; L(nvnTextureViewSetDefaults)(&v8); L(nvnTextureViewSetFormat)(&v8, NVN_FORMAT_RGBA8);
    L(nvnTexturePoolRegisterTexture)(tp, 303, d16, &v16);
    L(nvnTexturePoolRegisterTexture)(tp, 304, ds, &v8);
    NVNsamplerBuilder smb; L(nvnSamplerBuilderSetDevice)(&smb, h.dev); L(nvnSamplerBuilderSetDefaults)(&smb);
    L(nvnSamplerBuilderSetMinMagFilter)(&smb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    NVNsampler *smp = calloc(1, sizeof(NVNsampler)); L(nvnSamplerInitialize)(smp, &smb);
    L(nvnSamplerPoolRegisterSampler)(sp, 5, smp);
    uint64_t hu = L(nvnDeviceGetTextureHandle)(h.dev, 300, 5), hf = L(nvnDeviceGetTextureHandle)(h.dev, 301, 5), hms = L(nvnDeviceGetTextureHandle)(h.dev, 302, 5);
    uint64_t h16 = L(nvnDeviceGetTextureHandle)(h.dev, 303, 5), h8 = L(nvnDeviceGetTextureHandle)(h.dev, 304, 5);

    /* 1 */
    h_begin(&h);
    clear_depth(ds, 0.25f);
    h_submit(&h);
    h_begin(&h);
    setup(rt);
    draw(ptex, hu, 0);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    printf("R32UI view of Depth32F (0.25): %08x\n", px(8, 8));
    CHECK(px(8, 8) == 0x3e800000u && px(0, 15) == 0x3e800000u, "R32UI view of depth reads the depth bits: %08x %08x", px(8, 8), px(0, 15));

    /* 2 */
    h_begin(&h);
    clear_depth(d16, 0.0f);
    setup(rt);
    draw(ptex, h16, 0);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    CHECK(px(8, 8) == 0, "R16UI copy of Depth16 cleared to 0: %08x", px(8, 8));
    h_begin(&h);
    clear_depth(d16, 1.0f);
    setup(rt);
    L(nvnCommandBufferClearColor)(h.cb, 0, (const float[4]){0, 0, 0, 0}, NVN_CLEAR_COLOR_MASK_RGBA);
    draw(pk, h16, 1);  /* opens the target pass (left half 0x1234) */
    draw(ptex, h16, 0); /* the copy is stale (the depth clear pass closed after it was made): the pass is split for the copy */
    draw(pk, h16, 1);  /* left half again over the sampled values */
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    printf("same command buffer: left %08x right %08x\n", px(2, 8), px(13, 8));
    CHECK(px(13, 8) == 0xffffu, "sampled after a clear in the same command buffer: the copy is refreshed (Depth16 1.0 = ffff): %08x", px(13, 8));
    CHECK(px(2, 8) == 0x1234u, "draws around the split land in the same target: %08x", px(2, 8));

    /* 3 */
    h_begin(&h);
    clear_depth(msd, 0.5f);
    setup(rt);
    draw(ptld, hms, 0);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    printf("TLD.MS of 4x depth through R32UI: %08x\n", px(5, 5));
    CHECK(px(5, 5) == 0x3f000000u, "TLD.MS sample 2 of the 4x depth through an R32UI view: %08x (3f000000)", px(5, 5));

    /* 4 */
    h_begin(&h);
    setup(rt);
    draw(ptex, hf, 0);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    CHECK(px(8, 8) == 0x3e800000u, "R32F view of depth samples the depth value (0.25): %08x", px(8, 8));
    h_begin(&h);
    clear_depth(ds, 0.3f);
    setup(rt);
    draw(ptex, h8, 0);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    float r8, want8 = (float)0x9a / 255.0f; /* 0.3f = 3e99999a: byte 0 is 0x9a */
    uint32_t b8 = px(8, 8);
    memcpy(&r8, &b8, 4);
    printf("RGBA8 copy of Depth32F (0.3): r = %g (%g)\n", r8, want8);
    CHECK(r8 > want8 - 0.002f && r8 < want8 + 0.002f, "RGBA8 view of depth: the copy's first byte as unorm: %g (%g)", r8, want8);
    return h_finish("t_view_copy_draw", &h);
}
