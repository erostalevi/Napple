/* t_bcrt.c - rendering into a block-compressed texture through an RGBA16UI view (one texel per 4x4 block; Mario Kart 8 compresses on the GPU this way): the blocks
 * drawn must end up in the compressed texture (src/mtl_tex.m mtl_tex_attach_view shadow + mtl_bcrt_flush). BC4 (RGTC1) 16x16 = 4x4 blocks of 8 bytes. No game data. */
#include "nvn_harness.h"

int main(void) {
    H h; if (h_init(&h, 16, 16, NULL)) return 1;
    NVNtexture *t = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGTC1_UNORM, 16, 16, 1, 0x20000, 0);
    NVNtextureView view; L(nvnTextureViewSetDefaults)(&view); L(nvnTextureViewSetFormat)(&view, NVN_FORMAT_RGBA16UI);
    const NVNtexture *rts[1] = { t }; const NVNtextureView *views[1] = { &view };
    uint32_t c1[4] = { 0x1111, 0x2222, 0x3333, 0x4444 }, c2[4] = { 0xaaaa, 0xbbbb, 0xcccc, 0xdddd };
    NVNcopyRegion all = { 0, 0, 0, 16, 16, 1 };
    h_begin(&h);
    L(nvnCommandBufferSetRenderTargets)(h.cb, 1, rts, views, NULL, NULL);
    L(nvnCommandBufferClearColorui)(h.cb, 0, c1, 0xf);
    h_readback(&h, t, 0, 0, 16, 16, H_READBACK);
    h_submit(&h);
    const uint8_t *b = h.mem + H_READBACK;
    static const uint8_t e1[8] = { 0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0x44, 0x44 };
    int ok1 = 1; for (int i = 0; i < 16; i++) if (memcmp(b + i * 8, e1, 8)) ok1 = 0;
    CHECK(ok1, "every block of the compressed texture holds the rendered block (first block %02x%02x %02x%02x %02x%02x %02x%02x)", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);

    /* a second pass overwrites them */
    h_begin(&h);
    L(nvnCommandBufferSetRenderTargets)(h.cb, 1, rts, views, NULL, NULL);
    L(nvnCommandBufferClearColorui)(h.cb, 0, c2, 0xf);
    h_readback(&h, t, 0, 0, 16, 16, H_READBACK + 0x1000);
    h_submit(&h);
    b = h.mem + H_READBACK + 0x1000;
    static const uint8_t e2[8] = { 0xaa, 0xaa, 0xbb, 0xbb, 0xcc, 0xcc, 0xdd, 0xdd };
    int ok2 = 1; for (int i = 0; i < 16; i++) if (memcmp(b + i * 8, e2, 8)) ok2 = 0;
    CHECK(ok2, "the second pass replaces the blocks (first block %02x%02x %02x%02x)", b[0], b[1], b[2], b[3]);
    (void)all;
    return h_finish("t_bcrt", &h);
}
