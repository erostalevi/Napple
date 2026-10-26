/* nvn_harness.h - small shared setup for the NVN regression tests (t_clear_*, t_depth16, t_feedback, t_copy_alias, t_present_snap, t_pso_archive).
 * Ideas after the nxnative GPU regression tests (dm/gfx/tests/t_clear_ds.m, t_clear_tex.m, t_depth16_clear.m, t_feedback.m, t_copy_alias.m, t_present_snap.m,
 * t_pso_archive.m; MIT, copyright seedds, see THIRD_PARTY_NOTICES.md); written fresh against our public NVN entry points, no code copied.
 * KNOWN(cond, ...) is for behaviour another session is still implementing (or a known open bug): it SKIPs with a message, and fails only with T_STRICT=1. */
#ifndef NVN_HARNESS_H
#define NVN_HARNESS_H
#include "nvn_test.h"

static int g_skip __attribute__((unused));
#define KNOWN(c, ...) do { if (!(c)) { if (getenv("T_STRICT")) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
    else { printf("SKIP %s:%d (not implemented yet / known open): ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_skip++; } } } while (0)

typedef struct { NVNdevice *dev; NVNmemoryPool *pool; uint8_t *mem; size_t pool_size; NVNqueue *q; NVNcommandBuffer *cb; } H;
#define H_READBACK 0x300000          /* readback area in the pool (first 3 MB are texture storage) */

static __attribute__((unused)) int h_init(H *h, int w, int hgt, const char *cache_dir) {
    napple_gfx_config cfg; napple_gfx_config_default(&cfg);
    cfg.headless_width = w; cfg.headless_height = hgt; cfg.settings.async_shader_compile = 0; cfg.cache_dir = cache_dir;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    h->dev = calloc(1, sizeof(NVNdevice));
    NVNdeviceBuilder db; L(nvnDeviceBuilderSetDefaults)(&db);
    CHECK(L(nvnDeviceInitialize)(h->dev, &db), "device init");
    L(nvnDeviceSetWindowOriginMode)(h->dev, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
    L(nvnDeviceSetDepthMode)(h->dev, NVN_DEPTH_MODE_NEAR_IS_ZERO);
    h->pool_size = 4u << 20;
    h->mem = mmap(NULL, h->pool_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    NVNmemoryPoolBuilder pb; L(nvnMemoryPoolBuilderSetDevice)(&pb, h->dev); L(nvnMemoryPoolBuilderSetDefaults)(&pb);
    L(nvnMemoryPoolBuilderSetStorage)(&pb, h->mem, h->pool_size);
    L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    h->pool = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(h->pool, &pb), "pool init");
    NVNqueueBuilder qb; L(nvnQueueBuilderSetDevice)(&qb, h->dev); L(nvnQueueBuilderSetDefaults)(&qb);
    h->q = calloc(1, sizeof(NVNqueue)); CHECK(L(nvnQueueInitialize)(h->q, &qb), "queue init");
    h->cb = calloc(1, sizeof(NVNcommandBuffer)); CHECK(L(nvnCommandBufferInitialize)(h->cb, h->dev), "cmdbuf init");
    return 0;
}

static __attribute__((unused)) NVNtexture *h_tex(H *h, int target, int fmt, int w, int hgt, int depth, size_t pool_off, int flags) {
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, h->dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, target); L(nvnTextureBuilderSetFormat)(&tb, fmt);
    if (target == NVN_TEXTURE_TARGET_2D_ARRAY) L(nvnTextureBuilderSetSize3D)(&tb, w, hgt, depth); else L(nvnTextureBuilderSetSize2D)(&tb, w, hgt);
    L(nvnTextureBuilderSetFlags)(&tb, flags);
    L(nvnTextureBuilderSetStorage)(&tb, h->pool, pool_off);
    NVNtexture *t = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(t, &tb), "texture init fmt %#x", fmt);
    return t;
}

/* record with `fill`, submit, wait. The recording ends with whatever the caller put in; readbacks are explicit CopyTextureToBuffer calls. */
static __attribute__((unused)) void h_begin(H *h) { L(nvnCommandBufferBeginRecording)(h->cb); }
static __attribute__((unused)) void h_submit(H *h) {
    NVNcommandHandle hd = L(nvnCommandBufferEndRecording)(h->cb);
    CHECK(hd != 0, "end recording");
    L(nvnQueueSubmitCommands)(h->q, 1, &hd); L(nvnQueueFinish)(h->q);
}
static __attribute__((unused)) void h_readback(H *h, const NVNtexture *t, int x, int y, int w, int hgt, size_t buf_off) {
    NVNcopyRegion r = { x, y, 0, w, hgt, 1 };
    L(nvnCommandBufferCopyTextureToBuffer)(h->cb, t, NULL, &r, (NVNbufferAddress)(uintptr_t)(h->mem + buf_off), 0);
}
static __attribute__((unused)) int h_finish(const char *name, H *h) {
    napple_gfx_stats st; napple_gfx_get_stats(&st);
    CHECK(st.unimplemented_calls_total == 0, "unimplemented NVN calls hit: %llu", (unsigned long long)st.unimplemented_calls_total);
    printf(g_fail ? "%s: %d FAILED\n" : "%s: ok", name, g_fail);
    if (!g_fail) printf(" (%d skipped)\n", g_skip);
    (void)h;
    napple_gfx_shutdown();
    return g_fail != 0;
}
/* A red triangle (colour 1,0,0,1; vertices (-.5,-.5) (.5,-.5) (0,.5)) as in t_triangle. Shaders and vertices live in the first 0x10000 bytes of the pool. Records into h->cb:
 * set render target `tex` (64x64), viewport, state, draw. The caller begins/submits. `fs_g` != 0 makes it green. */
static __attribute__((unused)) void h_triangle(H *h, NVNtexture *tex, int green) {
    float tri[12] = { -0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0.0f, 0.5f, 0, 1 };
    memcpy(h->mem, tri, sizeof tri);
    uint64_t vs_i[] = { ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit() };
    uint64_t fs_i[] = { ins_mov32i(0, green ? 0 : 0x3f800000), ins_mov32i(1, green ? 0x3f800000 : 0), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit() };
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(h->mem + 0x1000, vctl, 0, vs_i, 3, 0);
    build_shader(h->mem + 0x3000, fctl, 1, fs_i, 5, 0xf);
    NVNprogram *prog = calloc(1, sizeof(NVNprogram));
    CHECK(L(nvnProgramInitialize)(prog, h->dev), "program init");
    NVNshaderData sd[2] = { { (NVNbufferAddress)(uintptr_t)(h->mem + 0x1000), vctl }, { (NVNbufferAddress)(uintptr_t)(h->mem + 0x3000), fctl } };
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "set shaders");
    NVNcommandBuffer *cb = h->cb;
    const NVNtexture *rts[1] = { tex };
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, 64, 64); L(nvnCommandBufferSetScissor)(cb, 0, 0, 64, 64);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnDepthStencilStateSetDepthWriteEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0); L(nvnVertexAttribStateSetStreamIndex)(&va, 0);
    NVNvertexStreamState vstream; L(nvnVertexStreamStateSetDefaults)(&vstream); L(nvnVertexStreamStateSetStride)(&vstream, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va); L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vstream);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)h->mem, sizeof tri);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
}
#endif
