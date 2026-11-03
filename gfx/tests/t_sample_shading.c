/* t_sample_shading.c - nvnProgramSetSampleShading runs the fragment program once per sample: a program that writes its fragment x position (ALD 0x70)
 * into a 4x R32F target is drawn with and without sample shading, then TLD.MS reads each of the 4 samples of texel (3, 3) back into an R32UI target.
 * Per-sample execution stores each sample's own position (four different values inside the pixel, averaging to its centre); per-pixel execution stores the
 * centre (3.5) in every sample. Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 8
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_MS = 0x100000, OFF_RT = 0x140000, OFF_TP = 0x180000, OFF_SP = 0x190000, OFF_RB = H_READBACK };
#define TLD_MS(d, a, b, index) (0xdc38000000000000ull | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | (1ull << 29) | (0xfull << 31) | ((uint64_t)(index) << 36) | (1ull << 50) | PT_BITS)

static H h;
static NVNtexturePool *tp;
static NVNsamplerPool *sp;
static int g_nsh;
static uint8_t g_ctl[16][0x880];

static NVNprogram *program(const uint64_t *fs, int n) {
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    uint8_t *m = h.mem + OFF_SH + 0x2000 * (size_t)g_nsh;
    build_shader(m, g_ctl[2 * g_nsh], 0, vs_i, 3, 0);
    build_shader(m + 0x1000, g_ctl[2 * g_nsh + 1], 1, fs, n, 0xf);
    NVNshaderData sd[2] = {{(NVNbufferAddress)(uintptr_t)m, g_ctl[2 * g_nsh]}, {(NVNbufferAddress)(uintptr_t)(m + 0x1000), g_ctl[2 * g_nsh + 1]}};
    g_nsh++;
    NVNprogram *p = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(p, h.dev);
    CHECK(L(nvnProgramSetShaders)(p, 2, sd), "shaders");
    return p;
}
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
    NVNmultisampleState ms; L(nvnMultisampleStateSetDefaults)(&ms); L(nvnMultisampleStateSetSamples)(&ms, rt == NULL ? 1 : 4); L(nvnCommandBufferBindMultisampleState)(cb, &ms);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0);
    NVNvertexStreamState vs; L(nvnVertexStreamStateSetDefaults)(&vs); L(nvnVertexStreamStateSetStride)(&vs, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vs);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 48);
    L(nvnCommandBufferSetTexturePool)(cb, tp);
    L(nvnCommandBufferSetSamplerPool)(cb, sp);
}
static void draw(NVNprogram *p) {
    L(nvnCommandBufferBindProgram)(h.cb, p, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    float tri[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    memcpy(h.mem + OFF_VB, tri, sizeof tri);
    uint64_t pos_i[] = {ins_ald(0, 0x70, 1), ins_mov32i(1, 0), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit()};
    NVNprogram *shaded = program(pos_i, 5), *plain = program(pos_i, 5);
    L(nvnProgramSetSampleShading)(shaded, 1, 1.0f);
    NVNprogram *rd[4];
    for (int k = 0; k < 4; k++) {
        uint64_t r_i[] = {ins_mov32i(4, 3), ins_mov32i(5, 3), ins_mov32i(6, (uint32_t)k), TLD_MS(0, 4, 6, 8), ins_exit()};
        rd[k] = program(r_i, 5);
    }
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h.dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D_MULTISAMPLE); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_R32F);
    L(nvnTextureBuilderSetSize2D)(&tb, W, W); L(nvnTextureBuilderSetSamples)(&tb, 4); L(nvnTextureBuilderSetStorage)(&tb, h.pool, OFF_MS);
    NVNtexture *ms = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(ms, &tb), "4x R32F target");
    NVNtexture *rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32UI, W, W, 1, OFF_RT, 0);
    tp = calloc(1, sizeof(NVNtexturePool));
    sp = calloc(1, sizeof(NVNsamplerPool));
    CHECK(L(nvnTexturePoolInitialize)(tp, h.pool, OFF_TP, 512), "texture pool");
    CHECK(L(nvnSamplerPoolInitialize)(sp, h.pool, OFF_SP, 64), "sampler pool");
    NVNtextureView tv; L(nvnTextureViewSetDefaults)(&tv);
    L(nvnTexturePoolRegisterTexture)(tp, 300, ms, &tv);
    NVNsamplerBuilder smb; L(nvnSamplerBuilderSetDevice)(&smb, h.dev); L(nvnSamplerBuilderSetDefaults)(&smb);
    NVNsampler *smp = calloc(1, sizeof(NVNsampler)); L(nvnSamplerInitialize)(smp, &smb);
    L(nvnSamplerPoolRegisterSampler)(sp, 5, smp);
    uint64_t hms = L(nvnDeviceGetTextureHandle)(h.dev, 300, 5);

    float got[2][4];
    for (int pass = 0; pass < 2; pass++) {
        h_begin(&h);
        setup(ms);
        L(nvnCommandBufferClearColor)(h.cb, 0, (const float[4]){0, 0, 0, 0}, NVN_CLEAR_COLOR_MASK_RGBA);
        draw(pass ? plain : shaded);
        h_submit(&h);
        for (int k = 0; k < 4; k++) {
            h_begin(&h);
            setup(rt);
            L(nvnCommandBufferBindTexture)(h.cb, NVN_SHADER_STAGE_FRAGMENT, 0, hms);
            draw(rd[k]);
            memset(h.mem + OFF_RB, 0xcd, W * W * 4);
            h_readback(&h, rt, 0, 0, W, W, OFF_RB);
            h_submit(&h);
            memcpy(&got[pass][k], h.mem + OFF_RB + (2 * W + 2) * 4, 4);
        }
        printf("%s: sample x positions of texel 3: %g %g %g %g\n", pass ? "per pixel " : "per sample", got[pass][0], got[pass][1], got[pass][2], got[pass][3]);
    }
    int distinct = 0;
    float mean = 0;
    for (int k = 0; k < 4; k++) {
        mean += got[0][k] / 4;
        for (int j = 0; j < k; j++) distinct |= got[0][k] != got[0][j];
        CHECK(got[0][k] > 3.0f && got[0][k] < 4.0f, "per-sample x of sample %d inside texel 3: %g", k, got[0][k]);
        CHECK(got[1][k] == 3.5f, "per-pixel x of sample %d is the centre: %g", k, got[1][k]);
    }
    /* Metal runs the function per sample (the key and [[sample_id]] work: the draw above rendered with the per-sample pipeline), but its [[position]] and default
     * interpolants stay at the pixel centre: the sample's own position needs get_sample_position(sample_id) and [[sample_perspective]] varyings in the translated
     * fragment function (gfx/src/shader/maxwell_translate.c; see docs/archive/changes/2026-11-01-gfx-msaa-views-2026-11-01-r1.md). Known gap until then. */
    KNOWN(distinct, "sample shading: the samples hold different positions (inputs evaluated at each sample)");
    KNOWN(mean > 3.45f && mean < 3.55f, "sample positions average to the pixel centre: %g", mean);
    return h_finish("t_sample_shading", &h);
}
