/* t_d24_readback.c - DEPTH24 (0x33) and DEPTH24_STENCIL8 (0x35) textures are 32-bit float depth (+ stencil) in Metal: a buffer -> texture copy converts the
 * packed words (copy_d24_to_tex) and a texture -> buffer copy packs them again (copy_d24_from_tex, mtl_replay.m). Round trip of an 8x8 region with depth
 * values over the whole 24-bit range (0, 1, 2^23, 0xffffff, a ramp) and every stencil byte pattern: the words read back must be the words written.
 * Before: "reading back a 24-bit depth texture ... is not implemented" and the destination kept its old bytes.
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define N 8
enum { OFF_D24 = 0x100000, OFF_D24S8 = 0x110000, OFF_SRC = 0x200000, OFF_DST = 0x210000 };

static H h;

static int round_trip(NVNtexture *t, int stencil, const char *name) {
    uint32_t *src = (uint32_t *)(h.mem + OFF_SRC), *dst = (uint32_t *)(h.mem + OFF_DST);
    static const uint32_t fixed[6] = {0, 1, 0x800000, 0xffffff, 0x123456, 0xabcdef};
    for (int i = 0; i < N * N; i++) {
        uint32_t dv = i < 6 ? fixed[i] : (uint32_t)(((uint64_t)i * 0xffffff) / (N * N - 1)) ^ (uint32_t)(i * 0x10101);
        dv &= 0xffffff;
        src[i] = stencil ? (dv << 8) | (uint32_t)((i * 37 + 5) & 0xff) : dv;
    }
    memset(dst, 0xee, N * N * 4);
    NVNcopyRegion r = {0, 0, 0, N, N, 1};
    h_begin(&h);
    L(nvnCommandBufferCopyBufferToTexture)(h.cb, (NVNbufferAddress)(uintptr_t)src, t, NULL, &r, 0);
    h_submit(&h);
    h_begin(&h);
    L(nvnCommandBufferCopyTextureToBuffer)(h.cb, t, NULL, &r, (NVNbufferAddress)(uintptr_t)dst, 0);
    h_submit(&h);
    int bad = 0;
    for (int i = 0; i < N * N; i++)
        if (dst[i] != src[i]) {
            if (bad < 4) printf("  %s texel %d: wrote %08x read %08x\n", name, i, src[i], dst[i]);
            bad++;
        }
    printf("%s: %d of %d words differ (first %08x %08x %08x %08x)\n", name, bad, N * N, dst[0], dst[1], dst[2], dst[3]);
    return bad;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, N, N, NULL)) return 1;
    NVNtexture *d24 = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24, N, N, 1, OFF_D24, 0);
    NVNtexture *d24s8 = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24_STENCIL8, N, N, 1, OFF_D24S8, 0);
    CHECK(round_trip(d24, 0, "DEPTH24") == 0, "DEPTH24 words round trip through the texture");
    CHECK(round_trip(d24s8, 1, "DEPTH24_STENCIL8") == 0, "DEPTH24_STENCIL8 words (depth << 8 | stencil) round trip through the texture");
    return h_finish("t_d24_readback", &h);
}
