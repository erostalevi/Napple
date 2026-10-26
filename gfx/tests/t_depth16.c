/* t_depth16.c - Depth16 clears: the two production paths (ClearDepthStencil on the render target, ClearTexture on the texture) must store the same 16-bit value, including
 * the half-bin boundaries (k + 0.5) / 65535 and their neighbouring floats, where a unorm conversion that rounds differently shows up. The expected value is
 * round(d * 65535) +-1 (the GPU's own conversion); the paths must agree with each other exactly. No game data. */
#include "nvn_harness.h"
#include <math.h>

int main(void) {
    H h; if (h_init(&h, 8, 8, NULL)) return 1;
    NVNtexture *col = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 0x20000, NVN_TEXTURE_FLAGS_DISPLAY);
    NVNtexture *da = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH16, 8, 8, 1, 0x80000, 0);
    NVNtexture *db = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH16, 8, 8, 1, 0xa0000, 0);
    float vals[10]; int n = 0;
    vals[n++] = 0; vals[n++] = 1; vals[n++] = 0.5f;
    float half = (100 + 0.5f) / 65535.0f;
    vals[n++] = nextafterf(half, 0); vals[n++] = half; vals[n++] = nextafterf(half, 1);
    half = (40000 + 0.5f) / 65535.0f;
    vals[n++] = nextafterf(half, 0); vals[n++] = half; vals[n++] = nextafterf(half, 1);
    vals[n++] = 1.0f / 65535.0f;
    for (int i = 0; i < n; i++) {
        h_begin(&h);
        const NVNtexture *rts[1] = { col };
        L(nvnCommandBufferSetRenderTargets)(h.cb, 1, rts, NULL, da, NULL);
        L(nvnCommandBufferClearDepthStencil)(h.cb, vals[i], 1, 0, 0);
        NVNcopyRegion all = { 0, 0, 0, 8, 8, 1 };
        float c[4] = { vals[i], 0, 0, 0 };
        L(nvnCommandBufferClearTexture)(h.cb, db, NULL, &all, c, 0xf);
        h_readback(&h, da, 0, 0, 8, 8, H_READBACK);
        h_readback(&h, db, 0, 0, 8, 8, H_READBACK + 0x1000);
        h_submit(&h);
        uint16_t a = *(const uint16_t *)(h.mem + H_READBACK), b = *(const uint16_t *)(h.mem + H_READBACK + 0x1000);
        long want = lroundf(vals[i] * 65535.0f);
        CHECK(labs((long)a - want) <= 1, "ClearDepthStencil %.9g: stored %u, expected %ld +-1", vals[i], a, want);
        KNOWN(a == b, "value %.9g: render-target clear stores %u, ClearTexture stores %u (paths disagree)", vals[i], a, b);
        CHECK(labs((long)b - want) <= 1, "ClearTexture %.9g: stored %u, expected %ld +-1", vals[i], b, want);
    }
    return h_finish("t_depth16", &h);
}
