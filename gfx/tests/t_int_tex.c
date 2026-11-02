/* t_int_tex.c - integer sampled textures (ShaderKey.tex_uint_slots / tex_sint_slots, the translator's IntTex wrapper, mtl_tex_int_kind): a fragment program
 * samples binding 0 at (0.5, 0.5) and writes the four result registers to an RGBA32UI target, so the target holds the texel's register bits.
 *  1. RGBA8UI view of an RGBA8 texture (a Metal format view): the registers hold the bytes as integers (1, 2, 3, 4);
 *  2. RGBA8I view of another RGBA8 texture with bytes (ff, 80, 7f, 01): sign-extended (-1, -128, 127, 1);
 *  3. RG32UI view of a BC1 texture (one 4x4 block; a copy): the block's two words, then (0, 1);
 *  4. a 3D RGBA8UI texture (TEX.3D): texture3d<uint> through IntTex3D;
 *  5. a CUBEMAP view of a 2D RGBA8UI texture (TEX.CUBE; a copy-backed cube): texturecube<uint> through IntTexCube;
 *  6. a 4x RGBA8UI render target a draw filled with (1, 2, 3, 4), read by TLD.MS sample 2: texture2d_ms<uint> through IntTex2DMS.
 * Before integer slots, these bindings were declared texture2d<float> (the format view read as converted floats, the BC1 integer view fell back to the
 * texture's own format). The replay fills the slot masks (int_slot_masks in mtl_replay.m). Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 8
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_T8 = 0x100000, OFF_T8I = 0x108000, OFF_TBC = 0x110000, OFF_T3 = 0x118000, OFF_TC = 0x120000, OFF_TMS = 0x128000,
       OFF_RT = 0x160000, OFF_TP = 0x180000, OFF_SP = 0x190000, OFF_RB = H_READBACK };
#define TEX_LZ_T(d, a, b, index, target) ((ins_tex2d_lz(d, a, b, index) & ~(3ull << 29)) | ((uint64_t)(target) << 29)) /* target 2 = 3D, 3 = cube */
#define TLD_MS(d, a, b, index) (0xdc38000000000000ull | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | (1ull << 29) | (0xfull << 31) | ((uint64_t)(index) << 36) | (1ull << 50) | PT_BITS)

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
    /* 4-6: other targets */
    static uint8_t c2[6][0x880];
    NVNprogram *pp[3];
    uint64_t f3[] = {ins_mov32i(4, 0x3f000000), ins_mov32i(5, 0x3f000000), ins_mov32i(6, 0x3f000000), TEX_LZ_T(0, 4, 4, 8, 2), ins_exit()};
    uint64_t fc[] = {ins_mov32i(4, 0x3f800000), ins_mov32i(5, 0x3dcccccd), ins_mov32i(6, 0x3e4ccccd), TEX_LZ_T(0, 4, 4, 8, 3), ins_exit()};
    uint64_t fm[] = {ins_mov32i(4, 3), ins_mov32i(5, 3), ins_mov32i(6, 2), TLD_MS(0, 4, 6, 8), ins_exit()};
    const uint64_t *fs[3] = {f3, fc, fm};
    for (int k = 0; k < 3; k++) {
        build_shader(h.mem + OFF_SH + 0x2000 * (k + 1), c2[2 * k], 0, vs_i, 3, 0);
        build_shader(h.mem + OFF_SH + 0x2000 * (k + 1) + 0x1000, c2[2 * k + 1], 1, fs[k], 5, 0xf);
        pp[k] = calloc(1, sizeof(NVNprogram));
        L(nvnProgramInitialize)(pp[k], h.dev);
        NVNshaderData d2[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x2000 * (k + 1)), c2[2 * k]},
                               {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x2000 * (k + 1) + 0x1000), c2[2 * k + 1]}};
        CHECK(L(nvnProgramSetShaders)(pp[k], 2, d2), "shaders %d", k);
    }
    for (int i = 0; i < 0x2000; i += 4) memcpy(h.mem + OFF_T3 + i, "\x05\x06\x07\x08", 4);
    for (int i = 0; i < 0x2000; i += 4) memcpy(h.mem + OFF_TC + i, "\x09\x0a\x0b\x0c", 4);
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h.dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_3D); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8UI);
    L(nvnTextureBuilderSetSize3D)(&tb, 4, 4, 4); L(nvnTextureBuilderSetStorage)(&tb, h.pool, OFF_T3);
    NVNtexture *t3 = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(t3, &tb), "3D RGBA8UI texture");
    NVNtexture *tc = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8UI, 4, 4, 1, OFF_TC, 0);
    L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D_MULTISAMPLE); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8UI);
    L(nvnTextureBuilderSetSize2D)(&tb, W, W); L(nvnTextureBuilderSetSamples)(&tb, 4); L(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    L(nvnTextureBuilderSetStorage)(&tb, h.pool, OFF_TMS);
    NVNtexture *tms = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(tms, &tb), "4x RGBA8UI render target");
    NVNtextureView vcube; L(nvnTextureViewSetDefaults)(&vcube); L(nvnTextureViewSetTarget)(&vcube, NVN_TEXTURE_TARGET_CUBEMAP);
    L(nvnTexturePoolRegisterTexture)(tp, 303, t3, NULL);
    L(nvnTexturePoolRegisterTexture)(tp, 304, tc, &vcube);
    L(nvnTexturePoolRegisterTexture)(tp, 305, tms, NULL);
    /* 4 */
    run(pp[0], rt, L(nvnDeviceGetTextureHandle)(h.dev, 303, 5));
    t = texel(3, 3);
    printf("3D RGBA8UI: %x %x %x %x\n", t[0], t[1], t[2], t[3]);
    CHECK(t[0] == 5 && t[1] == 6 && t[2] == 7 && t[3] == 8, "3D RGBA8UI texture read as integers: %x %x %x %x", t[0], t[1], t[2], t[3]);
    /* 5 */
    run(pp[1], rt, L(nvnDeviceGetTextureHandle)(h.dev, 304, 5));
    t = texel(3, 3);
    printf("cube view of 2D RGBA8UI: %x %x %x %x\n", t[0], t[1], t[2], t[3]);
    CHECK(t[0] == 9 && t[1] == 10 && t[2] == 11 && t[3] == 12, "cube view of a 2D RGBA8UI texture read as integers: %x %x %x %x", t[0], t[1], t[2], t[3]);
    /* 6: fill the 4x integer target with (1, 2, 3, 4) by a draw, then read sample 2 */
    uint64_t fw[] = {ins_mov32i(0, 1), ins_mov32i(1, 2), ins_mov32i(2, 3), ins_mov32i(3, 4), ins_exit()};
    static uint8_t cw[2][0x880];
    build_shader(h.mem + OFF_SH + 0x8000, cw[0], 0, vs_i, 3, 0);
    build_shader(h.mem + OFF_SH + 0x9000, cw[1], 1, fw, 5, 0xf);
    NVNprogram *pw = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(pw, h.dev);
    NVNshaderData dw[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x8000), cw[0]}, {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x9000), cw[1]}};
    CHECK(L(nvnProgramSetShaders)(pw, 2, dw), "writer shaders");
    NVNmultisampleState ms; L(nvnMultisampleStateSetDefaults)(&ms); L(nvnMultisampleStateSetSamples)(&ms, 4);
    h_begin(&h);
    L(nvnCommandBufferBindMultisampleState)(h.cb, &ms);
    draw(pw, tms, 0);
    L(nvnMultisampleStateSetSamples)(&ms, 0);
    L(nvnCommandBufferBindMultisampleState)(h.cb, &ms);
    h_submit(&h);
    run(pp[2], rt, L(nvnDeviceGetTextureHandle)(h.dev, 305, 5));
    t = texel(3, 3);
    printf("4x RGBA8UI sample 2: %x %x %x %x\n", t[0], t[1], t[2], t[3]);
    CHECK(t[0] == 1 && t[1] == 2 && t[2] == 3 && t[3] == 4, "multisampled RGBA8UI read by sample (TLD.MS): %x %x %x %x", t[0], t[1], t[2], t[3]);
    return h_finish("t_int_tex", &h);
}
