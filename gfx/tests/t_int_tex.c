/* t_int_tex.c - integer sampled textures (ShaderKey.tex_uint_slots / tex_sint_slots, the translator's IntTex wrapper, mtl_tex_int_kind): a fragment program
 * samples binding 0 at (0.5, 0.5) and writes the four result registers to an RGBA32UI target, so the target holds the texel's register bits.
 *  1. RGBA8UI view of an RGBA8 texture (a Metal format view): the registers hold the bytes as integers (1, 2, 3, 4);
 *  2. RGBA8I view of another RGBA8 texture with bytes (ff, 80, 7f, 01): sign-extended (-1, -128, 127, 1);
 *  3. RG32UI view of a BC1 texture (one 4x4 block; a copy): the block's two words, then (0, 1).
 * Before integer slots, these bindings were declared texture2d<float> (the format view read as converted floats, the BC1 integer view fell back to the
 * texture's own format). Needs the replay to fill the slot masks (int_slot_masks in mtl_replay.m): built with -DNAPPLE_TEST_INT_TEX_SLOTS until that
 * hook lands, else the test reports itself skipped. Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 8
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_T8 = 0x100000, OFF_T8I = 0x108000, OFF_TBC = 0x110000, OFF_RT = 0x160000, OFF_TP = 0x180000, OFF_SP = 0x190000, OFF_RB = H_READBACK };

#ifdef NAPPLE_TEST_INT_TEX_SLOTS
static H h;
static NVNtexturePool *tp;
static NVNsamplerPool *sp;

static void draw(NVNprogram *prog, NVNtexture *rt, uint64_t handle) {
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
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 48);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferBindTexture)(cb, NVN_SHADER_STAGE_FRAGMENT, 0, handle);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
}
static const uint32_t *texel(int x, int y) { return (const uint32_t *)(h.mem + OFF_RB) + (y * W + x) * 4; }
static void run(NVNprogram *prog, NVNtexture *rt, uint64_t handle) {
    h_begin(&h);
    draw(prog, rt, handle);
    memset(h.mem + OFF_RB, 0xcd, W * W * 16);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    float tri[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    memcpy(h.mem + OFF_VB, tri, sizeof tri);
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    uint64_t fs_i[] = {ins_mov32i(4, 0x3f000000), ins_mov32i(5, 0x3f000000), ins_tex2d_lz(0, 4, 4, 8), ins_exit()};
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(h.mem + OFF_SH, vctl, 0, vs_i, 3, 0);
    build_shader(h.mem + OFF_SH + 0x1000, fctl, 1, fs_i, 4, 0xf);
    NVNprogram *prog = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(prog, h.dev);
    NVNshaderData sd[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH), vctl}, {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x1000), fctl}};
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "shaders");

    /* every texel of the RGBA8 texture is the same 4 bytes, whatever the block-linear layout; the BC1 texture is one block */
    for (int i = 0; i < 0x1000; i += 4) memcpy(h.mem + OFF_T8 + i, "\x01\x02\x03\x04", 4);
    for (int i = 0; i < 0x1000; i += 4) memcpy(h.mem + OFF_T8I + i, "\xff\x80\x7f\x01", 4); /* written before first use: the replay write-protects used storage */
    const uint32_t blk[2] = {0x8410f800u, 0xe4e4e4e4u};
    memset(h.mem + OFF_TBC, 0, 0x1000);
    memcpy(h.mem + OFF_TBC, blk, 8);
    NVNtexture *t8 = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 4, 1, OFF_T8, 0);
    NVNtexture *t8i = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 4, 1, OFF_T8I, 0);
    NVNtexture *tbc = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA_DXT1, 4, 4, 1, OFF_TBC, 0);
    NVNtexture *rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA32UI, W, W, 1, OFF_RT, 0);
    tp = calloc(1, sizeof(NVNtexturePool));
    sp = calloc(1, sizeof(NVNsamplerPool));
    CHECK(L(nvnTexturePoolInitialize)(tp, h.pool, OFF_TP, 512), "texture pool");
    CHECK(L(nvnSamplerPoolInitialize)(sp, h.pool, OFF_SP, 64), "sampler pool");
    NVNtextureView vu; L(nvnTextureViewSetDefaults)(&vu); L(nvnTextureViewSetFormat)(&vu, NVN_FORMAT_RGBA8UI);
    NVNtextureView vi; L(nvnTextureViewSetDefaults)(&vi); L(nvnTextureViewSetFormat)(&vi, NVN_FORMAT_RGBA8I);
    NVNtextureView vb; L(nvnTextureViewSetDefaults)(&vb); L(nvnTextureViewSetFormat)(&vb, NVN_FORMAT_RG32UI);
    L(nvnTexturePoolRegisterTexture)(tp, 300, t8, &vu);
    L(nvnTexturePoolRegisterTexture)(tp, 301, t8i, &vi);
    L(nvnTexturePoolRegisterTexture)(tp, 302, tbc, &vb);
    NVNsamplerBuilder smb; L(nvnSamplerBuilderSetDevice)(&smb, h.dev); L(nvnSamplerBuilderSetDefaults)(&smb);
    L(nvnSamplerBuilderSetMinMagFilter)(&smb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    NVNsampler *smp = calloc(1, sizeof(NVNsampler)); L(nvnSamplerInitialize)(smp, &smb);
    L(nvnSamplerPoolRegisterSampler)(sp, 5, smp);

    /* 1 */
    run(prog, rt, L(nvnDeviceGetTextureHandle)(h.dev, 300, 5));
    const uint32_t *t = texel(3, 3);
    printf("RGBA8UI view: %08x %08x %08x %08x\n", t[0], t[1], t[2], t[3]);
    CHECK(t[0] == 1 && t[1] == 2 && t[2] == 3 && t[3] == 4, "RGBA8UI view of RGBA8 reads the bytes as integers: %x %x %x %x", t[0], t[1], t[2], t[3]);
    /* 2 */
    run(prog, rt, L(nvnDeviceGetTextureHandle)(h.dev, 301, 5));
    t = texel(3, 3);
    printf("RGBA8I view: %08x %08x %08x %08x\n", t[0], t[1], t[2], t[3]);
    CHECK(t[0] == 0xffffffffu && t[1] == 0xffffff80u && t[2] == 0x7f && t[3] == 1, "RGBA8I view reads sign-extended integers: %x %x %x %x", t[0], t[1], t[2], t[3]);
    /* 3 */
    run(prog, rt, L(nvnDeviceGetTextureHandle)(h.dev, 302, 5));
    t = texel(3, 3);
    printf("RG32UI view of BC1: %08x %08x %08x %08x\n", t[0], t[1], t[2], t[3]);
    CHECK(t[0] == blk[0] && t[1] == blk[1] && t[2] == 0 && t[3] == 1, "RG32UI copy of a BC1 block reads its words: %08x %08x %x %x", t[0], t[1], t[2], t[3]);
    return h_finish("t_int_tex", &h);
}
#else
int main(void) {
    printf("t_int_tex: ok (1 skipped: built without -DNAPPLE_TEST_INT_TEX_SLOTS; needs the int_slot_masks hook in mtl_replay.m)\n");
    return 0;
}
#endif
