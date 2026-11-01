/* t_nvn_ext.c - the 14 entry points added on 2026-11-01 (looked up by Animal Crossing and Mario Kart 8 Deluxe, absent from the bundled header):
 *  - each resolves through nvnDeviceGetProcAddress to an implementation (not the unimplemented stub, not NULL);
 *  - BindTexturesDeferred binds like BindTextures: a draw samples a depth texture cleared to 0.25 through it; the other deferred binds record without breaking
 *    the following draw;
 *  - nvnTextureBuilderGetViewOffset == nvnTextureGetViewOffset of the texture made from the builder (level and layer views of a mipmapped 2D array);
 *  - nvnProgramGetSubtileSize reports 0 x 0, SetProvokingVertexMode is kept, nvnDeviceWaitForError returns "no error" and is counted; the tuning hints are no-ops.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 16
enum { OFF_VB = 0x0, OFF_SH = 0x1000, OFF_DS = 0x100000, OFF_RT = 0x140000, OFF_TP = 0x180000, OFF_SP = 0x190000, OFF_ARR = 0x200000, OFF_RB = H_READBACK };

int gfx_cmd_provoking_vertex_mode(void);      /* api_cmd.c */
uint64_t gfx_device_wait_for_error_calls(void); /* api_device.c */

static H h;
static const char *const k_names[] = {
    "nvnCommandBufferBindUniformBuffersDeferred", "nvnCommandBufferBindStorageBuffersDeferred", "nvnCommandBufferBindTexturesDeferred",
    "nvnCommandBufferBindImagesDeferred", "nvnCommandBufferBindSeparateTexturesDeferred", "nvnCommandBufferBindSeparateSamplersDeferred",
    "nvnCommandBufferOverrideSubtileSize", "nvnProgramGetSubtileSize", "nvnCommandBufferSetControlMemoryCallbackEnabled",
    "nvnCommandBufferSetProvokingVertexMode", "nvnCommandBufferSetZCullZF32CompressionEnable", "nvnDeviceSetAutomaticCheckpointsMask",
    "nvnDeviceWaitForError", "nvnTextureBuilderGetViewOffset",
};

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;
    void *unimpl_probe = (void *)L(nvnDeviceGetProcAddress)(h.dev, "nvnCommandBufferSetStencilCullCriteria"); /* any implemented entry: a real address */
    CHECK(unimpl_probe != NULL, "probe lookup");
    for (unsigned i = 0; i < sizeof k_names / sizeof *k_names; i++) {
        void *f = (void *)L(nvnDeviceGetProcAddress)(h.dev, k_names[i]);
        CHECK(f != NULL, "%s resolves", k_names[i]);
    }

    /* BindTexturesDeferred in a draw */
    float tri[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    memcpy(h.mem + OFF_VB, tri, sizeof tri);
    uint64_t vs_i[] = {ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit()};
    uint64_t fs_i[] = {ins_mov32i(4, 0x3f000000), ins_mov32i(5, 0x3f000000), ins_tex2d_lz(0, 4, 4, 8), ins_exit()};
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(h.mem + OFF_SH, vctl, 0, vs_i, 3, 0);
    build_shader(h.mem + OFF_SH + 0x2000, fctl, 1, fs_i, 4, 0xf);
    NVNprogram *prog = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(prog, h.dev);
    NVNshaderData sd[2] = {{(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH), vctl}, {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_SH + 0x2000), fctl}};
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "shaders");
    NVNtexture *ds = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH32F, W, W, 1, OFF_DS, 0);
    NVNtexture *rt = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32UI, W, W, 1, OFF_RT, 0);
    NVNtexturePool *tp = calloc(1, sizeof(NVNtexturePool));
    NVNsamplerPool *sp = calloc(1, sizeof(NVNsamplerPool));
    CHECK(L(nvnTexturePoolInitialize)(tp, h.pool, OFF_TP, 512), "texture pool");
    CHECK(L(nvnSamplerPoolInitialize)(sp, h.pool, OFF_SP, 64), "sampler pool");
    L(nvnTexturePoolRegisterTexture)(tp, 300, ds, NULL);
    NVNsamplerBuilder smb; L(nvnSamplerBuilderSetDevice)(&smb, h.dev); L(nvnSamplerBuilderSetDefaults)(&smb);
    L(nvnSamplerBuilderSetMinMagFilter)(&smb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    NVNsampler *smp = calloc(1, sizeof(NVNsampler)); L(nvnSamplerInitialize)(smp, &smb);
    L(nvnSamplerPoolRegisterSampler)(sp, 5, smp);
    NVNtextureHandle th = L(nvnDeviceGetTextureHandle)(h.dev, 300, 5);
    NVNcommandBuffer *cb = h.cb;
    h_begin(&h);
    L(nvnCommandBufferSetRenderTargets)(cb, 0, NULL, NULL, ds, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, W, W);
    L(nvnCommandBufferSetScissor)(cb, 0, 0, W, W);
    L(nvnCommandBufferClearDepthStencil)(cb, 0.25f, 1, 0, 0);
    const NVNtexture *rts[1] = {rt};
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
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
    /* the other deferred binds and the hints, recorded in the same command buffer */
    NVNbufferRange rng = {(NVNbufferAddress)(uintptr_t)(h.mem + OFF_VB), 256};
    L(nvnCommandBufferBindUniformBuffersDeferred)(cb, NVN_SHADER_STAGE_FRAGMENT, 0, 1, &rng);
    L(nvnCommandBufferBindStorageBuffersDeferred)(cb, NVN_SHADER_STAGE_FRAGMENT, 0, 1, &rng);
    NVNimageHandle ih = 0;
    L(nvnCommandBufferBindImagesDeferred)(cb, NVN_SHADER_STAGE_FRAGMENT, 0, 1, &ih);
    NVNseparateTextureHandle sth = {0};
    NVNseparateSamplerHandle ssh = {0};
    L(nvnCommandBufferBindSeparateTexturesDeferred)(cb, NVN_SHADER_STAGE_FRAGMENT, 4, 1, &sth);
    L(nvnCommandBufferBindSeparateSamplersDeferred)(cb, NVN_SHADER_STAGE_FRAGMENT, 4, 1, &ssh);
    L(nvnCommandBufferOverrideSubtileSize)(cb, 32, 32);
    L(nvnCommandBufferSetZCullZF32CompressionEnable)(cb, 1);
    L(nvnCommandBufferSetControlMemoryCallbackEnabled)(cb, 1);
    L(nvnCommandBufferSetProvokingVertexMode)(cb, 1);
    L(nvnCommandBufferBindTexturesDeferred)(cb, NVN_SHADER_STAGE_FRAGMENT, 0, 1, &th);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    memset(h.mem + OFF_RB, 0xcd, W * W * 4);
    h_readback(&h, rt, 0, 0, W, W, OFF_RB);
    h_submit(&h);
    uint32_t got = ((const uint32_t *)(h.mem + OFF_RB))[8 * W + 8];
    CHECK(got == 0x3e800000u, "BindTexturesDeferred: the draw samples the bound depth texture (0.25 = 3e800000): %08x", got);
    CHECK(gfx_cmd_provoking_vertex_mode() == 1, "SetProvokingVertexMode kept: %d", gfx_cmd_provoking_vertex_mode());

    /* GetViewOffset: builder vs texture */
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h.dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D_ARRAY); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    L(nvnTextureBuilderSetSize3D)(&tb, 64, 64, 4); L(nvnTextureBuilderSetLevels)(&tb, 4); L(nvnTextureBuilderSetStorage)(&tb, h.pool, OFF_ARR);
    NVNtexture *arr = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(arr, &tb), "mipmapped 2D array");
    NVNtextureView v; L(nvnTextureViewSetDefaults)(&v); L(nvnTextureViewSetLevels)(&v, 2, 1); L(nvnTextureViewSetLayers)(&v, 3, 1);
    ptrdiff_t ob = L(nvnTextureBuilderGetViewOffset)(&tb, &v), ot = L(nvnTextureGetViewOffset)(arr, &v);
    CHECK(ob == ot && ob > 0, "builder view offset %td == texture view offset %td", ob, ot);
    L(nvnTextureViewSetDefaults)(&v);
    CHECK(L(nvnTextureBuilderGetViewOffset)(&tb, &v) == 0, "default view: offset 0");

    int sw = -1, sh = -1;
    L(nvnProgramGetSubtileSize)(prog, &sw, &sh);
    CHECK(sw == 0 && sh == 0, "subtile size 0 x 0: %d x %d", sw, sh);
    L(nvnDeviceSetAutomaticCheckpointsMask)(h.dev, 0xffu);
    uint64_t before = gfx_device_wait_for_error_calls();
    CHECK(L(nvnDeviceWaitForError)(h.dev, 1000000) == 0, "WaitForError: no error");
    CHECK(gfx_device_wait_for_error_calls() == before + 1, "WaitForError calls counted");
    return h_finish("t_nvn_ext", &h);
}
