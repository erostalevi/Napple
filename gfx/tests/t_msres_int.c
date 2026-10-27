/* t_msres_int.c - nvnCommandBufferDownsample of a multisampled INTEGER render target (Metal's fixed-function resolve cannot do integer formats): the single-sampled
 * destination gets sample 0 of every pixel. R32UI 8x8, 4 samples, cleared with ClearColorui then resolved and read back. No game data. */
#include "nvn_harness.h"

int main(void) {
    H h; if (h_init(&h, 8, 8, NULL)) return 1;
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h.dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D_MULTISAMPLE); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_R32UI);
    L(nvnTextureBuilderSetSize2D)(&tb, 8, 8); L(nvnTextureBuilderSetSamples)(&tb, 4); L(nvnTextureBuilderSetFlags)(&tb, 0);
    L(nvnTextureBuilderSetStorage)(&tb, h.pool, 0x20000);
    NVNtexture *ms = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(ms, &tb), "ms texture init");
    NVNtexture *dst = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32UI, 8, 8, 1, 0x80000, 0);
    uint32_t c[4] = { 0xdeadbeef, 0, 0, 0 };
    const NVNtexture *rts[1] = { ms };
    h_begin(&h);
    L(nvnCommandBufferSetRenderTargets)(h.cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferClearColorui)(h.cb, 0, c, 0xf);
    L(nvnCommandBufferDownsample)(h.cb, ms, dst);
    h_readback(&h, dst, 0, 0, 8, 8, H_READBACK);
    h_submit(&h);
    const uint32_t *px = (const uint32_t *)(h.mem + H_READBACK);
    int ok = 1; for (int i = 0; i < 64; i++) if (px[i] != 0xdeadbeefu) ok = 0;
    CHECK(ok, "every pixel of the resolved integer target holds the cleared value (first %08x, last %08x)", px[0], px[63]);
    return h_finish("t_msres_int", &h);
}
