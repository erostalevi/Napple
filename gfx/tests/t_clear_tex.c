/* t_clear_tex.c - nvnCommandBufferClearTexture on an RGBA8 texture: full clear, a sub-rectangle, and a channel mask that must leave the other channels. No game data.
 * Measured 2026-10-26: the full clear works; the region and the channel mask are ignored (the whole texture takes the colour, all four channels). Those two checks are KNOWN
 * (skip, fail under T_STRICT=1) until gfx/src/api_cmd.c / mtl_replay.m honour them; owned by the ac-render session. */
#include "nvn_harness.h"

int main(void) {
    H h; if (h_init(&h, 64, 64, NULL)) return 1;
    NVNtexture *t = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 64, 1, 0x20000, NVN_TEXTURE_FLAGS_DISPLAY);
    uint32_t *px = (uint32_t *)(h.mem + H_READBACK);

    h_begin(&h);
    float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 };
    NVNcopyRegion all = { 0, 0, 0, 64, 64, 1 }, sub = { 16, 8, 0, 16, 16, 1 };
    L(nvnCommandBufferClearTexture)(h.cb, t, NULL, &all, red, 0xf);
    h_readback(&h, t, 0, 0, 64, 64, H_READBACK);
    L(nvnCommandBufferClearTexture)(h.cb, t, NULL, &sub, green, 0xf);
    h_readback(&h, t, 0, 0, 64, 64, H_READBACK + 0x10000);
    h_submit(&h);
    CHECK(px[0] == 0xff0000ffu && px[63 * 64 + 63] == 0xff0000ffu, "full clear red: %08x %08x", px[0], px[63 * 64 + 63]);
    const uint32_t *p2 = px + 0x10000 / 4;
    CHECK(p2[8 * 64 + 16] == 0xff00ff00u && p2[23 * 64 + 31] == 0xff00ff00u, "sub-rect green inside: %08x %08x", p2[8 * 64 + 16], p2[23 * 64 + 31]);
    KNOWN(p2[7 * 64 + 16] == 0xff0000ffu && p2[8 * 64 + 15] == 0xff0000ffu && p2[24 * 64 + 31] == 0xff0000ffu && p2[23 * 64 + 32] == 0xff0000ffu,
          "sub-rect must not touch the outside: %08x %08x %08x %08x", p2[7 * 64 + 16], p2[8 * 64 + 15], p2[24 * 64 + 31], p2[23 * 64 + 32]);

    /* channel mask 0x3 (R and G only): B and A keep the previous value */
    h_begin(&h);
    float blue[4] = { 0, 0, 1, 1 }, rg[4] = { 1, 1, 0, 0 };
    L(nvnCommandBufferClearTexture)(h.cb, t, NULL, &all, blue, 0xf);
    L(nvnCommandBufferClearTexture)(h.cb, t, NULL, &all, rg, 0x3);
    h_readback(&h, t, 0, 0, 4, 4, H_READBACK + 0x20000);
    h_submit(&h);
    uint32_t m = *(const uint32_t *)(h.mem + H_READBACK + 0x20000);
    KNOWN(m == 0xffffffffu, "masked clear (R,G only) must keep B and A from the blue clear: expected ffffffff, got %08x", m);
    return h_finish("t_clear_tex", &h);
}
