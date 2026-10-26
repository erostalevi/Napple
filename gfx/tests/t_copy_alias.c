/* t_copy_alias.c - two RGBA8 textures bound to the SAME pool storage (the guest aliases memory that way): data written through one must be visible through the other
 * (CopyBufferToTexture into A, CopyTextureToBuffer from B; and ClearTexture on A, read from B). The aliasing guarantee is KNOWN-open until the gfx texture cache
 * resolves aliased pool ranges (ac-render owns gfx/src/api_*.c, mtl_replay.m and tex/). No game data. */
#include "nvn_harness.h"

int main(void) {
    H h; if (h_init(&h, 16, 16, NULL)) return 1;
    NVNtexture *a = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 16, 16, 1, 0x20000, 0);
    NVNtexture *b = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 16, 16, 1, 0x20000, 0);     /* same storage */
    uint32_t *src = (uint32_t *)(h.mem + 0x100000);
    for (int i = 0; i < 256; i++) src[i] = 0xff000000u | (uint32_t)i * 0x010203u;
    NVNcopyRegion all = { 0, 0, 0, 16, 16, 1 };
    h_begin(&h);
    L(nvnCommandBufferCopyBufferToTexture)(h.cb, (NVNbufferAddress)(uintptr_t)src, a, NULL, &all, 0);
    h_readback(&h, a, 0, 0, 16, 16, H_READBACK);
    h_readback(&h, b, 0, 0, 16, 16, H_READBACK + 0x1000);
    h_submit(&h);
    const uint32_t *ra = (const uint32_t *)(h.mem + H_READBACK), *rb = (const uint32_t *)(h.mem + H_READBACK + 0x1000);
    CHECK(!memcmp(ra, src, 1024), "upload then readback through the SAME texture must round-trip (first words %08x %08x)", ra[0], ra[1]);
    KNOWN(!memcmp(rb, src, 1024), "readback through the aliasing texture B must see A's upload (first words %08x %08x, expected %08x %08x)", rb[0], rb[1], src[0], src[1]);

    float g[4] = { 0, 1, 0, 1 };
    h_begin(&h);
    L(nvnCommandBufferClearTexture)(h.cb, a, NULL, &all, g, 0xf);
    h_readback(&h, b, 0, 0, 16, 16, H_READBACK + 0x2000);
    h_submit(&h);
    KNOWN(*(const uint32_t *)(h.mem + H_READBACK + 0x2000) == 0xff00ff00u, "clear of A must be visible through B: %08x", *(const uint32_t *)(h.mem + H_READBACK + 0x2000));
    return h_finish("t_copy_alias", &h);
}
