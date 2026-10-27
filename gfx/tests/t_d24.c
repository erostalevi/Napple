/* t_d24.c - 24-bit depth uploads: DEPTH24_STENCIL8 (0x35, depth in the high 24 bits of each word, stencil in the low 8) written with CopyBufferToTexture must reach
 * the depth buffer that stands in for it (Metal has no 24-bit depth on Apple GPUs). Checked through the depth test: a triangle at depth 0 passes LESS against an
 * uploaded depth of 0.5 and fails against an uploaded depth of 0. Same for DEPTH24 (0x33, depth in the low 24 bits). No game data. */
#include "nvn_harness.h"

static uint32_t draw_with_depth(H *h, NVNtexture *col, NVNtexture *dep, uint32_t word) {
    uint32_t *up = (uint32_t *)(h->mem + 0x200000);
    for (int i = 0; i < 64 * 64; i++) up[i] = word;
    float black[4] = { 0, 0, 0, 1 };
    h_begin(h);
    L(nvnCommandBufferCopyBufferToTexture)(h->cb, (NVNbufferAddress)(uintptr_t)up, dep, NULL, &(NVNcopyRegion){ 0, 0, 0, 64, 64, 1 }, 0);
    const NVNtexture *rts[1] = { col };
    L(nvnCommandBufferSetRenderTargets)(h->cb, 1, rts, NULL, dep, NULL);
    L(nvnCommandBufferClearColor)(h->cb, 0, black, 0xf);
    g_h_depth_tex = dep;
    h_triangle(h, col, 0);                                  /* binds the colour and depth targets and draws with depth test LESS */
    h_readback(h, col, 0, 0, 64, 64, H_READBACK);
    h_submit(h);
    return ((const uint32_t *)(h->mem + H_READBACK))[32 * 64 + 32];
}

int main(void) {
    H h; if (h_init(&h, 64, 64, NULL)) return 1;
    g_h_depth_test = 1;
    NVNtexture *col = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 64, 1, 0x20000, NVN_TEXTURE_FLAGS_DISPLAY);
    NVNtexture *d24s8 = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24_STENCIL8, 64, 64, 1, 0x80000, 0);
    NVNtexture *d24 = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24, 64, 64, 1, 0x100000, 0);
    uint32_t red = 0xff0000ffu, black = 0xff000000u;
    uint32_t p = draw_with_depth(&h, col, d24s8, (0x800000u << 8) | 0x55);
    CHECK(p == red, "D24S8: depth 0.5 uploaded, triangle at depth 0 passes LESS: center %08x", p);
    p = draw_with_depth(&h, col, d24s8, 0x00000055u);
    CHECK(p == black, "D24S8: depth 0 uploaded, triangle at depth 0 fails LESS: center %08x", p);
    p = draw_with_depth(&h, col, d24, 0x00800000u);
    CHECK(p == red, "D24: depth 0.5 uploaded: center %08x", p);
    p = draw_with_depth(&h, col, d24, 0xff000000u);
    CHECK(p == black, "D24: depth 0 uploaded (the high byte is unused): center %08x", p);
    return h_finish("t_d24", &h);
}
