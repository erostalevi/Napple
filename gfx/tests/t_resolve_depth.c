/* t_resolve_depth.c - nvnCommandBufferResolveDepthBuffer closes the pass so the multisampled depth reaches its texture, and Downsample resolves the 4x depth buffer
 * into a single-sampled one; nvnProgramSetSampleShading / nvnProgramSetDebugLabel are defined and stored. MSAA Depth32F 64x64 cleared to 0.25. No game data. */
#include "nvn_harness.h"

int gfx_program_sample_shading(const NVNprogram *p, float *rate);

static NVNtexture *ms_tex(H *h, int fmt, size_t off) {
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h->dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D_MULTISAMPLE); L(nvnTextureBuilderSetFormat)(&tb, fmt);
    L(nvnTextureBuilderSetSize2D)(&tb, 64, 64); L(nvnTextureBuilderSetSamples)(&tb, 4); L(nvnTextureBuilderSetFlags)(&tb, 0);
    L(nvnTextureBuilderSetStorage)(&tb, h->pool, off);
    NVNtexture *t = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(t, &tb), "ms texture init fmt %#x", fmt);
    return t;
}

int main(void) {
    H h; if (h_init(&h, 64, 64, NULL)) return 1;
    NVNprogram prog; CHECK(L(nvnProgramInitialize)(&prog, h.dev), "program init");
    L(nvnProgramSetSampleShading)(&prog, 1, 0.5f);
    L(nvnProgramSetDebugLabel)(&prog, "t_resolve_depth");
    float rate = 0; int en = gfx_program_sample_shading(&prog, &rate);
    CHECK(en == 1 && rate == 0.5f, "sample shading stored: %d %g", en, rate);
    L(nvnProgramSetSampleShading)(&prog, 0, 7.0f);
    en = gfx_program_sample_shading(&prog, &rate);
    CHECK(en == 0 && rate == 1.0f, "sample shading disabled, rate clamped: %d %g", en, rate);

    NVNtexture *col = ms_tex(&h, NVN_FORMAT_RGBA8, 0x20000);
    NVNtexture *dep = ms_tex(&h, NVN_FORMAT_DEPTH32F, 0x100000);
    NVNtexture *dst = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH32F, 64, 64, 1, 0x200000, 0);
    const float *d = (const float *)(h.mem + H_READBACK);
    float black[4] = { 0, 0, 0, 1 };
    h_begin(&h);
    const NVNtexture *rts[1] = { col };
    L(nvnCommandBufferSetRenderTargets)(h.cb, 1, rts, NULL, dep, NULL);
    L(nvnCommandBufferClearColor)(h.cb, 0, black, 0xf);
    L(nvnCommandBufferClearDepthStencil)(h.cb, 0.25f, 1, 0, 0);
    L(nvnCommandBufferResolveDepthBuffer)(h.cb);
    L(nvnCommandBufferDownsample)(h.cb, dep, dst);
    h_readback(&h, dst, 0, 0, 64, 64, H_READBACK);
    h_submit(&h);
    CHECK(d[0] == 0.25f && d[32 * 64 + 32] == 0.25f && d[63 * 64 + 63] == 0.25f, "resolved depth 0.25: %g %g %g", d[0], d[32 * 64 + 32], d[63 * 64 + 63]);
    return h_finish("t_resolve_depth", &h);
}
