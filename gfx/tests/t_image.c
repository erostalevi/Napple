/* t_image.c - surface image binding (docs/architecture/gfx.md section 11): a fragment shader stores through SUST.D.32 into image 0, an R32UI texture registered in the
 * texture pool and bound with BindImage(FRAGMENT, 0). The texel it writes must hold the value, the others stay cleared; the integer kind of the image comes from the
 * pipeline key (image_uint_mask from the bound texture's format). No game data. */
#include "nvn_harness.h"
#include "t_surface_asm.inc"

int main(void) {
    H h; if (h_init(&h, 64, 64, NULL)) return 1;
    NVNtexture *rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 64, 1, 0x20000, NVN_TEXTURE_FLAGS_DISPLAY);
    NVNtexture *img = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32UI, 8, 8, 1, 0x60000, NVN_TEXTURE_FLAGS_IMAGE);
    NVNtexturePool *tp = calloc(1, sizeof(NVNtexturePool));
    CHECK(L(nvnTexturePoolInitialize)(tp, h.pool, 0x80000, 512), "texture pool");
    L(nvnTexturePoolRegisterImage)(tp, 7, img, NULL);
    uint64_t handle = L(nvnDeviceGetImageHandle)(h.dev, 7);

    /* vertex: pass the position; fragment: r4 = 0xdeadbeef, (r8, r9) = (3, 2), SUST.D.32 image 0, then a red colour */
    float tri[12] = { -0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0.0f, 0.5f, 0, 1 };
    memcpy(h.mem, tri, sizeof tri);
    uint64_t vs_i[] = { ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit() };
    uint64_t fs_i[] = { ins_mov32i(4, 0xdeadbeef), ins_mov32i(8, 3), ins_mov32i(9, 2), i_sust(4, 8, SD_2D, 0, SZ_32, CL_IGN),
                        ins_mov32i(0, 0x3f800000), ins_mov32i(1, 0), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit() };
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(h.mem + 0x1000, vctl, 0, vs_i, 3, 0);
    build_shader(h.mem + 0x3000, fctl, 1, fs_i, 9, 0xf);
    NVNprogram *prog = calloc(1, sizeof(NVNprogram));
    CHECK(L(nvnProgramInitialize)(prog, h.dev), "program init");
    NVNshaderData sd[2] = { { (NVNbufferAddress)(uintptr_t)(h.mem + 0x1000), vctl }, { (NVNbufferAddress)(uintptr_t)(h.mem + 0x3000), fctl } };
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "set shaders");

    NVNcommandBuffer *cb = h.cb;
    h_begin(&h);
    NVNcopyRegion all = { 0, 0, 0, 8, 8, 1 };
    float zero[4] = { 0, 0, 0, 0 };
    L(nvnCommandBufferClearTexture)(cb, img, NULL, &all, zero, 0xf);
    L(nvnCommandBufferSetTexturePool)(cb, tp);
    L(nvnCommandBufferBindImage)(cb, NVN_SHADER_STAGE_FRAGMENT, 0, handle);
    const NVNtexture *rts[1] = { rt };
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, 64, 64); L(nvnCommandBufferSetScissor)(cb, 0, 0, 64, 64);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnDepthStencilStateSetDepthWriteEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0); L(nvnVertexAttribStateSetStreamIndex)(&va, 0);
    NVNvertexStreamState vstream; L(nvnVertexStreamStateSetDefaults)(&vstream); L(nvnVertexStreamStateSetStride)(&vstream, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va); L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vstream);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)h.mem, sizeof tri);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    h_readback(&h, img, 0, 0, 8, 8, H_READBACK);
    h_readback(&h, rt, 0, 0, 64, 64, H_READBACK + 0x1000);
    h_submit(&h);
    const uint32_t *px = (const uint32_t *)(h.mem + H_READBACK);
    CHECK(px[2 * 8 + 3] == 0xdeadbeefu, "SUST.D.32 wrote the texel (3,2) of the R32UI image: %08x", px[2 * 8 + 3]);
    CHECK(px[0] == 0 && px[63] == 0 && px[2 * 8 + 4] == 0, "the other texels stay cleared: %08x %08x %08x", px[0], px[63], px[2 * 8 + 4]);
    uint32_t c = ((const uint32_t *)(h.mem + H_READBACK + 0x1000))[32 * 64 + 32];
    CHECK(c == 0xff0000ffu, "the triangle still draws red: %08x", c);
    return h_finish("t_image", &h);
}
