/* t_clear_ds.c - nvnCommandBufferClearDepthStencil on a Depth32F render target, read back with CopyTextureToBuffer: the depth value arrives intact, a clear with the
 * depth mask off leaves the old value, and a second clear replaces the first. Depth24Stencil8 stencil readback layout is KNOWN-open here. No game data. */
#include "nvn_harness.h"

int main(void) {
    H h; if (h_init(&h, 64, 64, NULL)) return 1;
    NVNtexture *col = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 64, 1, 0x20000, NVN_TEXTURE_FLAGS_DISPLAY);
    NVNtexture *dep = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH32F, 64, 64, 1, 0x80000, 0);
    const float *d0 = (const float *)(h.mem + H_READBACK), *d1 = (const float *)(h.mem + H_READBACK + 0x10000), *d2 = (const float *)(h.mem + H_READBACK + 0x20000);
    h_begin(&h);
    const NVNtexture *rts[1] = { col };
    L(nvnCommandBufferSetRenderTargets)(h.cb, 1, rts, NULL, dep, NULL);
    L(nvnCommandBufferClearDepthStencil)(h.cb, 0.25f, 1, 0, 0);
    h_readback(&h, dep, 0, 0, 64, 64, H_READBACK);
    L(nvnCommandBufferClearDepthStencil)(h.cb, 0.75f, 0, 0, 0);           /* depth mask 0: must keep 0.25 */
    h_readback(&h, dep, 0, 0, 64, 64, H_READBACK + 0x10000);
    L(nvnCommandBufferClearDepthStencil)(h.cb, 1.0f, 1, 0, 0);
    h_readback(&h, dep, 0, 0, 64, 64, H_READBACK + 0x20000);
    h_submit(&h);
    CHECK(d0[0] == 0.25f && d0[63 * 64 + 63] == 0.25f, "depth clear 0.25: got %g %g", d0[0], d0[63 * 64 + 63]);
    KNOWN(d1[0] == 0.25f && d1[40 * 64 + 40] == 0.25f, "depth clear with depth mask 0 must keep 0.25: got %g %g (the replay clears regardless of the mask)", d1[0], d1[40 * 64 + 40]);
    KNOWN(d2[0] == 1.0f && d2[63 * 64 + 63] == 1.0f, "depth clear 1.0: got %g %g (a clear that follows a readback is folded into the next pass and a copy does not flush it: mtl_replay.m do_clear_ds)", d2[0], d2[63 * 64 + 63]);
    return h_finish("t_clear_ds", &h);
}
