/* t_msaa_copy.c - copies and resolves the replay used to refuse, through the generic copy pass (mtl_replay.m xcopy_draw), checked by readback:
 *  1. Downsample 4x RGBA8UI -> RGBA32F (integer -> float, sample 0 by value);
 *  2. Downsample 4x Depth32F -> R32F (depth -> float) and -> Depth16 (depth of another format);
 *  3. Downsample 4x R32F -> Depth32F (float -> depth);
 *  4. CopyTextureToTexture 4x RGBA8 -> 4x BGRA8 (multisampled, another format: per sample), then resolved to check;
 *  5. CopyTextureToTexture Depth32F -> R32F and R32F -> Depth32F (single-sampled depth <-> colour);
 *  6. CopyTextureToTexture of two depth slices of a 3D RGBA8 texture into two layers of a BGRA8 2D array (3D with a format change, one draw per slice);
 *  7. CopyTextureToTexture 4x RGBA8 -> 1x RGBA8 of the same size (sample 0; Metal's blit refuses differing sample counts).
 * Headless Metal (MTL_DEBUG_LAYER=1 in make test), no game data. */
#include "nvn_harness.h"

#define W 8
enum { OFF_A = 0x10000, OFF_B = 0x40000, OFF_C = 0x80000, OFF_D = 0xC0000, OFF_E = 0x100000, OFF_SRC = 0x200000, OFF_RB = H_READBACK };

static H h;
static NVNtexture *tex(int target, int fmt, int w, int hgt, int depth, int samples, size_t off) {
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h.dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, target); L(nvnTextureBuilderSetFormat)(&tb, fmt);
    if (target == NVN_TEXTURE_TARGET_3D || target == NVN_TEXTURE_TARGET_2D_ARRAY) L(nvnTextureBuilderSetSize3D)(&tb, w, hgt, depth);
    else L(nvnTextureBuilderSetSize2D)(&tb, w, hgt);
    if (samples > 1) L(nvnTextureBuilderSetSamples)(&tb, samples);
    L(nvnTextureBuilderSetStorage)(&tb, h.pool, off);
    NVNtexture *t = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(t, &tb), "texture fmt %#x target %d", fmt, target);
    return t;
}
static void target(NVNtexture *c, NVNtexture *d) {
    const NVNtexture *rts[1] = {c};
    L(nvnCommandBufferSetRenderTargets)(h.cb, c ? 1 : 0, c ? rts : NULL, NULL, d, NULL);
    L(nvnCommandBufferSetViewport)(h.cb, 0, 0, W, W);
    L(nvnCommandBufferSetScissor)(h.cb, 0, 0, W, W);
}
static const uint32_t *rb(void) { return (const uint32_t *)(h.mem + OFF_RB); }
static float rbf(int i) { float f; memcpy(&f, h.mem + OFF_RB + 4 * i, 4); return f; }
static void readback(NVNtexture *t, int w) { memset(h.mem + OFF_RB, 0xcd, 0x10000); h_readback(&h, t, 0, 0, w, w, OFF_RB); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (h_init(&h, W, W, NULL)) return 1;

    /* 1 */
    NVNtexture *msu = tex(NVN_TEXTURE_TARGET_2D_MULTISAMPLE, NVN_FORMAT_RGBA8UI, W, W, 1, 4, OFF_A), *f4 = tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA32F, W, W, 1, 1, OFF_B);
    h_begin(&h);
    target(msu, NULL);
    const uint32_t cu[4] = {10, 20, 30, 40};
    L(nvnCommandBufferClearColorui)(h.cb, 0, cu, NVN_CLEAR_COLOR_MASK_RGBA);
    L(nvnCommandBufferDownsample)(h.cb, msu, f4);
    memset(h.mem + OFF_RB, 0xcd, 0x10000);
    NVNcopyRegion r8 = {0, 0, 0, W, W, 1};
    L(nvnCommandBufferCopyTextureToBuffer)(h.cb, f4, NULL, &r8, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_RB), 0);
    h_submit(&h);
    printf("4x RGBA8UI -> RGBA32F: %g %g %g %g\n", rbf(0), rbf(1), rbf(2), rbf(3));
    CHECK(rbf(0) == 10 && rbf(1) == 20 && rbf(2) == 30 && rbf(3) == 40 && rbf(4 * 27 + 2) == 30, "integer -> float Downsample by value: %g %g %g %g", rbf(0), rbf(1), rbf(2), rbf(3));

    /* 2 + 3 */
    NVNtexture *msd = tex(NVN_TEXTURE_TARGET_2D_MULTISAMPLE, NVN_FORMAT_DEPTH32F, W, W, 1, 4, OFF_A), *r32 = tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32F, W, W, 1, 1, OFF_B);
    NVNtexture *d16 = tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH16, W, W, 1, 1, OFF_C);
    h_begin(&h);
    target(NULL, msd);
    L(nvnCommandBufferClearDepthStencil)(h.cb, 0.25f, 1, 0, 0);
    L(nvnCommandBufferDownsample)(h.cb, msd, r32);
    L(nvnCommandBufferDownsample)(h.cb, msd, d16);
    readback(r32, W);
    h_submit(&h);
    CHECK(rbf(0) == 0.25f && rbf(63) == 0.25f, "4x depth -> R32F: %g %g", rbf(0), rbf(63));
    h_begin(&h);
    readback(d16, W);
    h_submit(&h);
    uint16_t c16 = ((const uint16_t *)rb())[9];
    CHECK(c16 == 0x4000 || c16 == 0x3fff, "4x Depth32F -> Depth16: %04x (0.25)", c16);
    NVNtexture *msf = tex(NVN_TEXTURE_TARGET_2D_MULTISAMPLE, NVN_FORMAT_R32F, W, W, 1, 4, OFF_D), *d32 = tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH32F, W, W, 1, 1, OFF_E);
    h_begin(&h);
    target(msf, NULL);
    L(nvnCommandBufferClearColor)(h.cb, 0, (const float[4]){0.5f, 0, 0, 1}, NVN_CLEAR_COLOR_MASK_RGBA);
    L(nvnCommandBufferDownsample)(h.cb, msf, d32);
    readback(d32, W);
    h_submit(&h);
    CHECK(rbf(5) == 0.5f, "4x R32F -> Depth32F: %g", rbf(5));

    /* 4 */
    NVNtexture *ms8 = tex(NVN_TEXTURE_TARGET_2D_MULTISAMPLE, NVN_FORMAT_RGBA8, W, W, 1, 4, OFF_A), *msb = tex(NVN_TEXTURE_TARGET_2D_MULTISAMPLE, NVN_FORMAT_BGRA8, W, W, 1, 4, OFF_C);
    NVNtexture *one = tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, W, W, 1, 1, OFF_B);
    h_begin(&h);
    target(ms8, NULL);
    L(nvnCommandBufferClearColor)(h.cb, 0, (const float[4]){1, 0, 0, 1}, NVN_CLEAR_COLOR_MASK_RGBA);
    L(nvnCommandBufferCopyTextureToTexture)(h.cb, ms8, NULL, &r8, msb, NULL, &r8, 0);
    L(nvnCommandBufferDownsample)(h.cb, msb, one);
    readback(one, W);
    h_submit(&h);
    CHECK(rb()[0] == 0xff0000ffu && rb()[63] == 0xff0000ffu, "4x RGBA8 -> 4x BGRA8 copy keeps red: %08x %08x", rb()[0], rb()[63]);
    /* 7 */
    h_begin(&h);
    target(ms8, NULL);
    L(nvnCommandBufferClearColor)(h.cb, 0, (const float[4]){0, 1, 0, 1}, NVN_CLEAR_COLOR_MASK_RGBA);
    L(nvnCommandBufferCopyTextureToTexture)(h.cb, ms8, NULL, &r8, one, NULL, &r8, 0);
    readback(one, W);
    h_submit(&h);
    CHECK(rb()[10] == 0xff00ff00u, "4x -> 1x copy of the same format takes sample 0 (green): %08x", rb()[10]);

    /* 5 */
    h_begin(&h);
    target(NULL, d32);
    L(nvnCommandBufferClearDepthStencil)(h.cb, 0.75f, 1, 0, 0);
    L(nvnCommandBufferCopyTextureToTexture)(h.cb, d32, NULL, &r8, r32, NULL, &r8, 0);
    readback(r32, W);
    h_submit(&h);
    CHECK(rbf(20) == 0.75f, "Depth32F -> R32F copy: %g", rbf(20));
    h_begin(&h);
    target(r32, NULL);
    L(nvnCommandBufferClearColor)(h.cb, 0, (const float[4]){0.125f, 0, 0, 1}, NVN_CLEAR_COLOR_MASK_RGBA);
    L(nvnCommandBufferCopyTextureToTexture)(h.cb, r32, NULL, &r8, d32, NULL, &r8, 0);
    readback(d32, W);
    h_submit(&h);
    CHECK(rbf(33) == 0.125f, "R32F -> Depth32F copy: %g", rbf(33));

    /* 6: slices z = 1, 2 of a 4x4x4 RGBA8 volume -> layers 0, 1 of a BGRA8 array */
    NVNtexture *vol = tex(NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA8, 4, 4, 4, 1, OFF_A), *arr = tex(NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_BGRA8, 4, 4, 2, 1, OFF_C);
    uint32_t *src = (uint32_t *)(h.mem + OFF_SRC);
    for (int z = 0; z < 4; z++) for (int i = 0; i < 16; i++) src[z * 16 + i] = 0xff000000u | (uint32_t)(z * 0x40); /* red = 0x40 * z */
    NVNcopyRegion vr = {0, 0, 0, 4, 4, 4}, sr = {0, 0, 1, 4, 4, 2}, dr = {0, 0, 0, 4, 4, 2};
    h_begin(&h);
    L(nvnCommandBufferCopyBufferToTexture)(h.cb, (NVNbufferAddress)(uintptr_t)src, vol, NULL, &vr, 0);
    L(nvnCommandBufferCopyTextureToTexture)(h.cb, vol, NULL, &sr, arr, NULL, &dr, 0);
    memset(h.mem + OFF_RB, 0xcd, 0x10000);
    NVNcopyRegion ar = {0, 0, 0, 4, 4, 2};
    L(nvnCommandBufferCopyTextureToBuffer)(h.cb, arr, NULL, &ar, (NVNbufferAddress)(uintptr_t)(h.mem + OFF_RB), 0);
    h_submit(&h);
    printf("3D slices -> BGRA8 array layers: %08x %08x\n", rb()[0], rb()[16]);
    /* BGRA8 memory order is b g r a: red 0x40 -> bytes 00 00 40 ff */
    CHECK(rb()[0] == 0xff400000u && rb()[16] == 0xff800000u, "3D z=1,2 -> array layers 0,1 with a format change: %08x %08x", rb()[0], rb()[16]);
    return h_finish("t_msaa_copy", &h);
}
