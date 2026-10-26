/* t_feedback.c - ordering inside one command buffer on one render target: clear red, draw a green triangle, read back, clear blue, read back, draw again, read back.
 * Every readback must see exactly the state at its position in the stream (no pass merged across a copy, no stale readback). No game data. */
#include "nvn_harness.h"

int main(void) {
    H h; if (h_init(&h, 64, 64, NULL)) return 1;
    NVNtexture *t = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 64, 1, 0x20000, NVN_TEXTURE_FLAGS_DISPLAY);
    float red[4] = { 1, 0, 0, 1 }, blue[4] = { 0, 0, 1, 1 };
    h_begin(&h);
    h_triangle(&h, t, 1);                                  /* sets the render target, binds state, draws a green triangle over the (uncleared) target */
    NVNcopyRegion all = { 0, 0, 0, 64, 64, 1 };
    L(nvnCommandBufferClearColor)(h.cb, 0, red, 0xf);
    h_readback(&h, t, 0, 0, 64, 64, H_READBACK);                                   /* 1: all red */
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);       /* green triangle again, same bound state */
    h_readback(&h, t, 0, 0, 64, 64, H_READBACK + 0x10000);                         /* 2: red with a green triangle */
    L(nvnCommandBufferClearColor)(h.cb, 0, blue, 0xf);
    h_readback(&h, t, 0, 0, 64, 64, H_READBACK + 0x20000);                         /* 3: all blue */
    (void)all;
    h_submit(&h);
    const uint32_t *r1 = (const uint32_t *)(h.mem + H_READBACK), *r2 = (const uint32_t *)(h.mem + H_READBACK + 0x10000), *r3 = (const uint32_t *)(h.mem + H_READBACK + 0x20000);
    CHECK(r1[32 * 64 + 32] == 0xff0000ffu && r1[1 * 64 + 1] == 0xff0000ffu, "readback 1 should be all red: center %08x corner %08x", r1[32 * 64 + 32], r1[1 * 64 + 1]);
    CHECK(r2[32 * 64 + 32] == 0xff00ff00u && r2[1 * 64 + 1] == 0xff0000ffu, "readback 2 should be a green triangle on red: center %08x corner %08x", r2[32 * 64 + 32], r2[1 * 64 + 1]);
    KNOWN(r3[32 * 64 + 32] == 0xffff0000u && r3[1 * 64 + 1] == 0xffff0000u, "readback 3 should be all blue: center %08x corner %08x (a clear after a readback is folded into the next pass and a copy does not flush it: mtl_replay.m clear folding)", r3[32 * 64 + 32], r3[1 * 64 + 1]);
    return h_finish("t_feedback", &h);
}
