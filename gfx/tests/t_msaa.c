/* t_msaa.c - multisampled textures beyond the fixed-function resolve (gfx-stages-real, 2026-10-27-b7):
 *  1. TLD.MS in a fragment program reads one sample of a 4x RGBA8 target bound as a texture (declared texture2d_ms, binding path of a multisampled view);
 *  2. nvnProgramSetSampleShading on a program drawn into the 4x target makes the pipeline's fragment function per-sample (the key carries it; drawn result
 *     unchanged for a constant colour, so the check is the recorded bind + a draw that still renders);
 *  3. Downsample between different formats (RGBA8 4x -> BGRA8) goes through the generic resolve pass;
 *  4. a sparse texture builder reports a tile layout (128x128 tiles for 4-byte texels, levels at least one tile large) and the texture initialises.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"
int gfx_program_sample_shading(const NVNprogram *p, float *rate); /* api_program.c test hook */

#define W 8
enum { OFF_SH = 0x1000, OFF_VB = 0x8000, OFF_MS = 0x20000, OFF_RT = 0x60000, OFF_RT2 = 0x70000, OFF_TP = 0x80000, OFF_SP = 0x81000, OFF_SPARSE = 0x100000, OFF_RB = H_READBACK };
/* TLD.MS: Ra = integer coordinates (r_a, r_a + 1), Rb = sample index, 2D target, full mask */
#define TLD_MS(d, a, b, index) (0xdc38000000000000ull | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | (1ull << 29) | (0xfull << 31) | ((uint64_t)(index) << 36) | (1ull << 50) | PT_BITS)

static H h;
static int g_nsh;
static uint8_t g_ctl[8][0x880];
static NVNshaderData stage(int st, const uint64_t *ins, int n, uint32_t omap) {
    uint8_t *m = h.mem + OFF_SH + 0x1000 * (size_t)g_nsh, *ctl = g_ctl[g_nsh++];
    build_shader(m, ctl, st, ins, n, omap);
    return (NVNshaderData){(NVNbufferAddress)(uintptr_t)m, ctl};
}
static NVNprogram *program(int n, const NVNshaderData *sd) {
    NVNprogram *p = calloc(1, sizeof(NVNprogram));
    CHECK(L(nvnProgramInitialize)(p, h.dev), "program init");
    CHECK(L(nvnProgramSetShaders)(p, n, sd), "set shaders");
    return p;
}
static void state(const NVNtexture *rt) {
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
    NVNmultisampleState ms; L(nvnMultisampleStateSetDefaults)(&ms); L(nvnCommandBufferBindMultisampleState)(cb, &ms);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0);
    NVNvertexStreamState vs; L(nvnVertexStreamStateSetDefaults)(&vs); L(nvnVertexStreamStateSetStride)(&vs, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vs);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 48);
}
static uint32_t rb_px(int x, int y) { return ((const uint32_t *)(h.mem + OFF_RB))[y * W + x]; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    float tri[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    memcpy(h.mem + OFF_VB, tri, sizeof tri);
    /* the 4x target */
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h.dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D_MULTISAMPLE); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    L(nvnTextureBuilderSetSize2D)(&tb, W, W); L(nvnTextureBuilderSetSamples)(&tb, 4); L(nvnTextureBuilderSetStorage)(&tb, h.pool, OFF_MS);
    NVNtexture *ms = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(ms, &tb), "4x texture init");
    NVNtexture *rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, W, W, 1, OFF_RT, 0);
    NVNtexture *rt_bgra = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_BGRA8, W, W, 1, OFF_RT2, 0);
    /* programs: position pass-through + (a) constant green with sample shading, (b) TLD.MS sample 2 of texel (3, 3) of binding 0 */
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    uint64_t fa_i[] = {ins_mov32i(0, 0), ins_mov32i(1, 0x3f800000), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit()};
    uint64_t fb_i[] = {ins_mov32i(4, 3), ins_mov32i(5, 3), ins_mov32i(6, 2), TLD_MS(0, 4, 6, 8), ins_exit()};
    NVNshaderData sa[2] = {stage(0, vs_i, 3, 0), stage(1, fa_i, 5, 0xf)};
    NVNprogram *green = program(2, sa);
    L(nvnProgramSetSampleShading)(green, 1, 1.0f);
    NVNshaderData sb[2] = {stage(0, vs_i, 3, 0), stage(1, fb_i, 5, 0xf)};
    NVNprogram *reader = program(2, sb);
    float rate = 0;
    CHECK(gfx_program_sample_shading(green, &rate) == 1 && rate == 1.0f, "sample shading stored (%d, %g)", gfx_program_sample_shading(green, &rate), rate);

    /* 1 + 2: clear the 4x target blue, draw the green full-screen triangle with sample shading, resolve to RGBA8 (fixed function): green everywhere */
    float blue[4] = {0, 0, 1, 1};
    h_begin(&h);
    state(ms);
    L(nvnCommandBufferClearColor)(h.cb, 0, blue, NVN_CLEAR_COLOR_MASK_RGBA);
    L(nvnCommandBufferBindProgram)(h.cb, green, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    L(nvnCommandBufferDownsample)(h.cb, ms, rt);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    CHECK(rb_px(0, 0) == 0xff00ff00u && rb_px(7, 7) == 0xff00ff00u, "sample-shaded draw into the 4x target resolves green: %08x %08x", rb_px(0, 0), rb_px(7, 7));

    /* 1: read sample 2 of texel (3, 3) through TLD.MS into every pixel of the single-sampled target */
    NVNtexturePool *tp = calloc(1, sizeof(NVNtexturePool));
    NVNsamplerPool *sp = calloc(1, sizeof(NVNsamplerPool));
    CHECK(L(nvnTexturePoolInitialize)(tp, h.pool, OFF_TP, 512), "texture pool");
    CHECK(L(nvnSamplerPoolInitialize)(sp, h.pool, OFF_SP, 64), "sampler pool");
    NVNtextureView tv; L(nvnTextureViewSetDefaults)(&tv);
    L(nvnTexturePoolRegisterTexture)(tp, 300, ms, &tv);
    NVNsamplerBuilder smb; L(nvnSamplerBuilderSetDevice)(&smb, h.dev); L(nvnSamplerBuilderSetDefaults)(&smb);
    NVNsampler *smp = calloc(1, sizeof(NVNsampler)); L(nvnSamplerInitialize)(smp, &smb);
    L(nvnSamplerPoolRegisterSampler)(sp, 5, smp);
    h_begin(&h);
    state(rt);
    L(nvnCommandBufferBindProgram)(h.cb, reader, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferSetTexturePool)(h.cb, tp);
    L(nvnCommandBufferSetSamplerPool)(h.cb, sp);
    L(nvnCommandBufferBindTexture)(h.cb, NVN_SHADER_STAGE_FRAGMENT, 0, L(nvnDeviceGetTextureHandle)(h.dev, 300, 5));
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    CHECK(rb_px(1, 1) == 0xff00ff00u && rb_px(6, 2) == 0xff00ff00u, "TLD.MS reads sample 2 of the 4x texture (green): %08x %08x", rb_px(1, 1), rb_px(6, 2));

    /* 3: resolve to another format through the generic pass */
    h_begin(&h);
    L(nvnCommandBufferDownsample)(h.cb, ms, rt_bgra);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt_bgra, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    CHECK(rb_px(0, 0) == 0xff00ff00u && rb_px(7, 0) == 0xff00ff00u, "Downsample RGBA8 4x -> BGRA8 (generic resolve): green: %08x %08x", rb_px(0, 0), rb_px(7, 0));

    /* 4: sparse tile layout */
    NVNtextureBuilder sb2; L(nvnTextureBuilderSetDevice)(&sb2, h.dev); L(nvnTextureBuilderSetDefaults)(&sb2);
    L(nvnTextureBuilderSetTarget)(&sb2, NVN_TEXTURE_TARGET_2D); L(nvnTextureBuilderSetFormat)(&sb2, NVN_FORMAT_RGBA8);
    L(nvnTextureBuilderSetSize2D)(&sb2, 256, 256); L(nvnTextureBuilderSetLevels)(&sb2, 4); L(nvnTextureBuilderSetFlags)(&sb2, NVN_TEXTURE_FLAGS_SPARSE);
    L(nvnTextureBuilderSetStorage)(&sb2, h.pool, OFF_SPARSE);
    struct { int32_t levels, w, hgt, d, reserved[4]; } lay;
    memset(&lay, 0x7f, sizeof lay);
    L(nvnTextureBuilderGetSparseTileLayout)(&sb2, (NVNtextureSparseTileLayout *)&lay);
    CHECK(lay.w == 128 && lay.hgt == 128 && lay.d == 1 && lay.levels == 2, "sparse tile layout 128x128x1, 2 tiled levels of 256x256 (got %dx%dx%d, %d)", lay.w, lay.hgt, lay.d, lay.levels);
    NVNtexture *sparse = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(sparse, &sb2), "sparse texture initialises (fully resident)");
    memset(&lay, 0, sizeof lay);
    L(nvnTextureGetSparseTileLayout)(sparse, (NVNtextureSparseTileLayout *)&lay);
    CHECK(lay.w == 128 && lay.levels == 2, "texture reports the same layout");
    return h_finish("t_msaa", &h);
}
