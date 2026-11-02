/* t_cube_view.c - a cube view of a 2D texture (Animal Crossing binds a 15x15 R8 texture through a CUBEMAP view): Metal cannot view a 2D texture as a cube,
 * so mtl_tex_view makes a copy-backed cube (AK_CUBE) with the 2D image on every face. A fragment program samples the view with TEX.CUBE in six directions
 * (one per face) and writes the result to an RGBA8 target: every face reads the 2D texture's colour, and the binding is a real cube (no "using a dummy
 * texture" fallback, which reads zero). A second texture is written after the first draw: the copy is refreshed.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 8
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_T = 0x100000, OFF_T2 = 0x108000, OFF_RT = 0x160000, OFF_TP = 0x180000, OFF_SP = 0x190000, OFF_RB = H_READBACK };
#define TEX_CUBE_LZ(d, a, b, index) (ins_tex2d_lz(d, a, b, index) | (1ull << 30)) /* target bits [29,31) = 3: cube, coordinates in Ra..Ra+2 */

static H h;
static NVNtexturePool *tp;
static NVNsamplerPool *sp;
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static uint32_t sample_dir(NVNtexture *rt, uint64_t handle, const float dir[3], uint8_t *vctl, uint8_t *fctl, int k) {
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    uint64_t fs_i[] = {ins_mov32i(4, fbits(dir[0])), ins_mov32i(5, fbits(dir[1])), ins_mov32i(6, fbits(dir[2])), TEX_CUBE_LZ(0, 4, 4, 8), ins_exit()};
    uint8_t *code = h.mem + OFF_SH + 0x2000 * (size_t)k;
    build_shader(code, vctl, 0, vs_i, 3, 0);
    build_shader(code + 0x1000, fctl, 1, fs_i, 5, 0xf);
    NVNprogram *prog = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(prog, h.dev);
    NVNshaderData sd[2] = {{(NVNbufferAddress)(uintptr_t)code, vctl}, {(NVNbufferAddress)(uintptr_t)(code + 0x1000), fctl}};
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "shaders");
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
    L(nvnCommandBufferBindTexture)(cb, NVN_SHADER_STAGE_FRAGMENT, 0, handle);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    return ((const uint32_t *)(h.mem + OFF_RB))[3 * W + 3];
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    float tri[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    memcpy(h.mem + OFF_VB, tri, sizeof tri);
    /* every texel the same colour, whatever the block-linear layout */
    for (int i = 0; i < 0x2000; i += 4) memcpy(h.mem + OFF_T + i, "\x11\x22\x33\xff", 4);
    for (int i = 0; i < 0x2000; i += 4) memcpy(h.mem + OFF_T2 + i, "\x80\x40\x20\xff", 4);
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h.dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    L(nvnTextureBuilderSetSize2D)(&tb, 16, 16); L(nvnTextureBuilderSetLevels)(&tb, 2);
    L(nvnTextureBuilderSetStorage)(&tb, h.pool, OFF_T);
    NVNtexture *t2d = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(t2d, &tb), "16x16 2D texture, 2 levels");
    L(nvnTextureBuilderSetStorage)(&tb, h.pool, OFF_T2);
    NVNtexture *t2d2 = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(t2d2, &tb), "second 2D texture");
    NVNtexture *rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, W, W, 1, OFF_RT, 0);
    tp = calloc(1, sizeof(NVNtexturePool));
    sp = calloc(1, sizeof(NVNsamplerPool));
    CHECK(L(nvnTexturePoolInitialize)(tp, h.pool, OFF_TP, 512), "texture pool");
    CHECK(L(nvnSamplerPoolInitialize)(sp, h.pool, OFF_SP, 64), "sampler pool");
    NVNtextureView vc; L(nvnTextureViewSetDefaults)(&vc); L(nvnTextureViewSetTarget)(&vc, NVN_TEXTURE_TARGET_CUBEMAP);
    L(nvnTexturePoolRegisterTexture)(tp, 300, t2d, &vc);
    L(nvnTexturePoolRegisterTexture)(tp, 301, t2d2, &vc);
    NVNsamplerBuilder smb; L(nvnSamplerBuilderSetDevice)(&smb, h.dev); L(nvnSamplerBuilderSetDefaults)(&smb);
    L(nvnSamplerBuilderSetMinMagFilter)(&smb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    NVNsampler *smp = calloc(1, sizeof(NVNsampler)); L(nvnSamplerInitialize)(smp, &smb);
    L(nvnSamplerPoolRegisterSampler)(sp, 5, smp);
    uint64_t hc = L(nvnDeviceGetTextureHandle)(h.dev, 300, 5), hc2 = L(nvnDeviceGetTextureHandle)(h.dev, 301, 5);

    static const float dirs[6][3] = {{1, 0.2f, 0.1f}, {-1, 0.1f, -0.2f}, {0.1f, 1, 0.2f}, {0.2f, -1, 0.1f}, {0.1f, 0.2f, 1}, {-0.1f, 0.2f, -1}};
    static uint8_t ctl[14][2][0x880];
    int bad = 0;
    for (int f = 0; f < 6; f++) {
        uint32_t p = sample_dir(rt, hc, dirs[f], ctl[f][0], ctl[f][1], f);
        if (p != 0xff332211u) {
            bad++;
            printf("  face %d: %08x\n", f, p);
        }
    }
    CHECK(bad == 0, "every face of the cube view of a 2D texture reads the 2D image (%d of 6 faces differ)", bad);
    uint32_t p2 = sample_dir(rt, hc2, dirs[4], ctl[6][0], ctl[6][1], 6);
    CHECK(p2 == 0xff204080u, "a second 2D texture through a cube view: %08x", p2);
    return h_finish("t_cube_view", &h);
}
