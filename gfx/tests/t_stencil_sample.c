/* t_stencil_sample.c - a fragment program samples a depth-stencil texture through a stencil-mode view (ShaderKey.stencil_slots: the replay binds the
 * X32_Stencil8 view, the translator declares texture2d<uint> and returns (stencil, 0, 0, 1) as integer bits) and writes the value to an R32UI target.
 * The stencil plane is filled by ClearDepthStencil; a second draw samples the same texture through a view in depth mode (the cleared depth, as a float).
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 16
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_DS = 0x100000, OFF_RT = 0x140000, OFF_TP = 0x180000, OFF_SP = 0x190000, OFF_RB = H_READBACK };

static H h;

static uint32_t sample_once(NVNprogram *prog, NVNtexture *rt, NVNtexturePool *tp, NVNsamplerPool *sp, uint64_t handle) {
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
    NVNvertexStreamState vs; L(nvnVertexStreamStateSetDefaults)(&vs); L(nvnVertexStreamStateSetStride)(&vs, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vs);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 48);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferSetTexturePool)(cb, tp);
    L(nvnCommandBufferSetSamplerPool)(cb, sp);
    L(nvnCommandBufferBindTexture)(cb, NVN_SHADER_STAGE_FRAGMENT, 0, handle);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    return ((const uint32_t *)(h.mem + OFF_RB))[(W / 2) * W + W / 2];
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    float tri[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    memcpy(h.mem + OFF_VB, tri, sizeof tri);
    /* fragment: TEX at (0.5, 0.5) of binding 0 -> r0..r3, written to the uint target as raw bits */
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    uint64_t fs_i[] = {ins_mov32i(4, 0x3f000000), ins_mov32i(5, 0x3f000000), ins_tex2d_lz(0, 4, 4, 8), ins_exit()};
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(h.mem + OFF_SH, vctl, 0, vs_i, 3, 0);
    build_shader(h.mem + OFF_SH + 0x2000, fctl, 1, fs_i, 4, 0xf);
    NVNprogram *prog = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(prog, h.dev);
    NVNshaderData sd[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH), vctl}, {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x2000), fctl}};
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "shaders");

    NVNtexture *ds = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24_STENCIL8, W, W, 1, OFF_DS, 0);
    NVNtexture *rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32UI, W, W, 1, OFF_RT, 0);
    /* fill the stencil plane: 0x5A, depth 0.25 */
    h_begin(&h);
    L(nvnCommandBufferSetRenderTargets)(h.cb, 0, NULL, NULL, ds, NULL);
    L(nvnCommandBufferSetViewport)(h.cb, 0, 0, W, W);
    L(nvnCommandBufferSetScissor)(h.cb, 0, 0, W, W);
    L(nvnCommandBufferClearDepthStencil)(h.cb, 0.25f, 1, 0x5A, 0xff);
    h_submit(&h);

    NVNtexturePool *tp = calloc(1, sizeof(NVNtexturePool));
    NVNsamplerPool *sp = calloc(1, sizeof(NVNsamplerPool));
    CHECK(L(nvnTexturePoolInitialize)(tp, h.pool, OFF_TP, 512), "texture pool");
    CHECK(L(nvnSamplerPoolInitialize)(sp, h.pool, OFF_SP, 64), "sampler pool");
    NVNtextureView sv; L(nvnTextureViewSetDefaults)(&sv); L(nvnTextureViewSetDepthStencilMode)(&sv, NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);
    NVNtextureView dv; L(nvnTextureViewSetDefaults)(&dv); L(nvnTextureViewSetDepthStencilMode)(&dv, NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH);
    L(nvnTexturePoolRegisterTexture)(tp, 300, ds, &sv);
    L(nvnTexturePoolRegisterTexture)(tp, 301, ds, &dv);
    NVNsamplerBuilder smb; L(nvnSamplerBuilderSetDevice)(&smb, h.dev); L(nvnSamplerBuilderSetDefaults)(&smb);
    L(nvnSamplerBuilderSetMinMagFilter)(&smb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    NVNsampler *smp = calloc(1, sizeof(NVNsampler)); L(nvnSamplerInitialize)(smp, &smb);
    L(nvnSamplerPoolRegisterSampler)(sp, 5, smp);

    uint32_t s = sample_once(prog, rt, tp, sp, L(nvnDeviceGetTextureHandle)(h.dev, 300, 5));
    printf("stencil view sampled: %#x (0x5a expected)\n", s);
    CHECK(s == 0x5A, "stencil-mode view in a fragment program reads the stencil value as an integer: %#x", s);
    uint32_t d = sample_once(prog, rt, tp, sp, L(nvnDeviceGetTextureHandle)(h.dev, 301, 5));
    float df;
    memcpy(&df, &d, 4);
    printf("depth view sampled: %08x (%g, 0.25 expected)\n", d, df);
    CHECK(df > 0.2499f && df < 0.2501f, "depth-mode view of the same texture reads the depth: %g", df);
    return h_finish("t_stencil_sample", &h);
}
