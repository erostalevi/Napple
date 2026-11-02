/* t_stencil_sample.c - a fragment program samples a depth-stencil texture through a stencil-mode view (ShaderKey.stencil_slots: the replay binds the
 * X32_Stencil8 view, the translator declares texture2d<uint> and returns (stencil, 0, 0, 1) as integer bits) and writes the value to an R32UI target.
 * The stencil plane is filled by ClearDepthStencil; a second draw samples the same texture through a view in depth mode (the cleared depth, as a float).
 * Then cube and cube array depth-stencil textures, each face (layer-face) cleared to its own stencil value, sampled through stencil views with TEX.CUBE
 * (texturecube<uint> / texturecube_array<uint> + StencilTexCube / StencilTexCubeArray): the direction must select the face's value.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 16
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_DS = 0x100000, OFF_RT = 0x140000, OFF_TP = 0x180000, OFF_SP = 0x190000, OFF_DC = 0x1a0000, OFF_DCA = 0x200000,
       OFF_RB = H_READBACK };
#define TEX_CUBE_LZ(d, a, index, array) ((ins_tex2d_lz(d, a, a, index) | (3ull << 29)) | ((uint64_t)(array) << 28))

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

    /* cube (6 faces) and cube array (2 cubes = 12 layer-faces) depth-stencil textures, layer-face f cleared to stencil 0x10 + f */
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h.dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_CUBEMAP); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_DEPTH24_STENCIL8);
    L(nvnTextureBuilderSetSize2D)(&tb, W, W); L(nvnTextureBuilderSetStorage)(&tb, h.pool, OFF_DC);
    NVNtexture *dc = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(dc, &tb), "cube DEPTH24_STENCIL8 texture");
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY); L(nvnTextureBuilderSetSize3D)(&tb, W, W, 12); L(nvnTextureBuilderSetStorage)(&tb, h.pool, OFF_DCA);
    NVNtexture *dca = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(dca, &tb), "cube array DEPTH24_STENCIL8 texture");
    for (int f = 0; f < 18; f++) {
        NVNtextureView lv; L(nvnTextureViewSetDefaults)(&lv); L(nvnTextureViewSetTarget)(&lv, NVN_TEXTURE_TARGET_2D); L(nvnTextureViewSetLayers)(&lv, f < 6 ? f : f - 6, 1);
        h_begin(&h);
        L(nvnCommandBufferSetRenderTargets)(h.cb, 0, NULL, NULL, f < 6 ? dc : dca, &lv);
        L(nvnCommandBufferSetViewport)(h.cb, 0, 0, W, W);
        L(nvnCommandBufferSetScissor)(h.cb, 0, 0, W, W);
        L(nvnCommandBufferClearDepthStencil)(h.cb, 0.5f, 1, 0x10 + (f < 6 ? f : f - 6), 0xff);
        h_submit(&h);
    }
    L(nvnTexturePoolRegisterTexture)(tp, 302, dc, &sv);
    L(nvnTexturePoolRegisterTexture)(tp, 303, dca, &sv);
    /* direction (x, y, z) [, cube index] -> expected layer-face */
    static const struct { float d[3]; int cube, layer, face; } probe[] = {
        {{1.0f, 0.1f, 0.2f}, 0, 0, 0}, {{0.1f, -1.0f, 0.2f}, 0, 0, 3}, {{0.1f, 0.2f, 1.0f}, 0, 0, 4}, {{-0.2f, 0.1f, -1.0f}, 0, 0, 5},
        {{-1.0f, 0.1f, 0.2f}, 1, 1, 1}, {{0.1f, 1.0f, 0.2f}, 1, 0, 2}, {{0.2f, 0.1f, 1.0f}, 1, 1, 4},
    };
    for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        uint32_t dw[3];
        memcpy(dw, probe[i].d, 12);
        int arr = probe[i].cube;
        uint64_t fi[] = {ins_mov32i(4, (uint32_t)probe[i].layer), ins_mov32i(4 + arr, dw[0]), ins_mov32i(5 + arr, dw[1]), ins_mov32i(6 + arr, dw[2]),
                         TEX_CUBE_LZ(0, 4, 8, arr), ins_exit()};
        static uint8_t cv[16][0x880], cf[16][0x880];
        build_shader(h.mem + OFF_SH + 0x4000 + 0x2000 * i, cv[i], 0, vs_i, 3, 0);
        build_shader(h.mem + OFF_SH + 0x5000 + 0x2000 * i, cf[i], 1, fi, 6, 0xf);
        NVNprogram *pc = calloc(1, sizeof(NVNprogram));
        L(nvnProgramInitialize)(pc, h.dev);
        NVNshaderData sc[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x4000 + 0x2000 * i), cv[i]},
                               {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x5000 + 0x2000 * i), cf[i]}};
        CHECK(L(nvnProgramSetShaders)(pc, 2, sc), "cube shaders %u", i);
        uint32_t v = sample_once(pc, rt, tp, sp, L(nvnDeviceGetTextureHandle)(h.dev, arr ? 303 : 302, 5));
        uint32_t want = 0x10 + (arr ? probe[i].layer * 6 : 0) + probe[i].face;
        printf("%s stencil view, direction (%g, %g, %g)%s: %#x (%#x expected)\n", arr ? "cube array" : "cube", probe[i].d[0], probe[i].d[1], probe[i].d[2],
               arr ? (probe[i].layer ? ", cube 1" : ", cube 0") : "", v, want);
        CHECK(v == want, "stencil view of a %s depth-stencil texture reads the face's stencil: %#x, %#x expected", arr ? "cube array" : "cube", v, want);
    }
    return h_finish("t_stencil_sample", &h);
}
