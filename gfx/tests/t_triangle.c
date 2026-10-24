/* t_triangle.c - headless end-to-end test: drives the public NVN entry points exactly as a guest would, with hand-assembled Maxwell shaders
 * (no game data), renders a triangle into an offscreen texture, copies it to memory and checks pixels; then presents and writes a screenshot. */
#include "nvn_test.h"

int main(void) {
    napple_gfx_config cfg;
    napple_gfx_config_default(&cfg);
    cfg.headless_width = 64; cfg.headless_height = 64;
    cfg.settings.async_shader_compile = 0;
    /* T_CACHE_PASS=1|2 (make test runs both): disk cache in build/t_triangle-cache; pass 1 fills the binary archive, pass 2 must load from it */
    const char *pass = getenv("T_CACHE_PASS");
    if (pass) cfg.cache_dir = "build/t_triangle-cache";
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }

    NVNdevice *dev = calloc(1, sizeof(NVNdevice));
    NVNdeviceBuilder db; L(nvnDeviceBuilderSetDefaults)(&db);
    CHECK(L(nvnDeviceInitialize)(dev, &db), "device init");
    L(nvnDeviceSetWindowOriginMode)(dev, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
    L(nvnDeviceSetDepthMode)(dev, NVN_DEPTH_MODE_NEAR_IS_ZERO);

    size_t PS = 1 << 20;
    uint8_t *mem = mmap(NULL, PS, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    NVNmemoryPoolBuilder pb; L(nvnMemoryPoolBuilderSetDevice)(&pb, dev); L(nvnMemoryPoolBuilderSetDefaults)(&pb);
    L(nvnMemoryPoolBuilderSetStorage)(&pb, mem, PS); L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    NVNmemoryPool *pool = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(pool, &pb), "pool init");

    /* layout: 0x0000 vertex data, 0x1000 VS, 0x3000 FS, 0x10000 readback, 0x20000 texture storage */
    float *verts = (float *)(mem + 0);
    float tri[12] = { -0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0.0f, 0.5f, 0, 1 };
    memcpy(verts, tri, sizeof tri);
    uint64_t vs_i[] = { ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit() };
    uint64_t fs_i[] = { ins_mov32i(0, 0x3f800000), ins_mov32i(1, 0), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit() };
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(mem + 0x1000, vctl, 0, vs_i, 3, 0);
    build_shader(mem + 0x3000, fctl, 1, fs_i, 5, 0xf);

    NVNprogram *prog = calloc(1, sizeof(NVNprogram));
    CHECK(L(nvnProgramInitialize)(prog, dev), "program init");
    NVNshaderData sd[2] = { { (NVNbufferAddress)(uintptr_t)(mem + 0x1000), vctl }, { (NVNbufferAddress)(uintptr_t)(mem + 0x3000), fctl } };
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "set shaders");

    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D); L(nvnTextureBuilderSetSize2D)(&tb, 64, 64); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    L(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_DISPLAY);
    L(nvnTextureBuilderSetStorage)(&tb, pool, 0x20000);
    NVNtexture *tex = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(tex, &tb), "texture init");

    NVNqueueBuilder qb; L(nvnQueueBuilderSetDevice)(&qb, dev); L(nvnQueueBuilderSetDefaults)(&qb);
    NVNqueue *q = calloc(1, sizeof(NVNqueue));
    CHECK(L(nvnQueueInitialize)(q, &qb), "queue init");

    NVNcommandBuffer *cb = calloc(1, sizeof(NVNcommandBuffer));
    CHECK(L(nvnCommandBufferInitialize)(cb, dev), "cmdbuf init");
    L(nvnCommandBufferBeginRecording)(cb);
    const NVNtexture *rts[1] = { tex };
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, 64, 64);
    L(nvnCommandBufferSetScissor)(cb, 0, 0, 64, 64);
    float blue[4] = { 0, 0, 1, 1 };
    L(nvnCommandBufferClearColor)(cb, 0, blue, 0xf);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnDepthStencilStateSetDepthWriteEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs); L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0); L(nvnVertexAttribStateSetStreamIndex)(&va, 0);
    NVNvertexStreamState vstream; L(nvnVertexStreamStateSetDefaults)(&vstream); L(nvnVertexStreamStateSetStride)(&vstream, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vstream);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)verts, sizeof tri);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    NVNcopyRegion region = { 0, 0, 0, 64, 64, 1 };
    L(nvnCommandBufferCopyTextureToBuffer)(cb, tex, NULL, &region, (NVNbufferAddress)(uintptr_t)(mem + 0x10000), 0);
    NVNcommandHandle h = L(nvnCommandBufferEndRecording)(cb);
    CHECK(h != 0, "end recording");

    L(nvnQueueSubmitCommands)(q, 1, &h);
    L(nvnQueueFinish)(q);

    const uint32_t *px = (const uint32_t *)(mem + 0x10000);
    uint32_t center = px[32 * 64 + 32], corner = px[1 * 64 + 1], top = px[10 * 64 + 32], bottom = px[54 * 64 + 32];
    printf("pixels: center %08x corner %08x (row10 %08x, row54 %08x)\n", center, corner, top, bottom);
    CHECK((center & 0xff) == 0xff && ((center >> 8) & 0xff) == 0 && ((center >> 16) & 0xff) == 0, "center should be red (RGBA8 0xff0000ff), got %08x", center);
    CHECK(corner == 0xffff0000u, "corner should be blue 0xffff0000, got %08x", corner);
    /* the apex (0, +0.5) is at the top in upper-left origin: wide at the bottom rows, narrow at the top */
    CHECK((px[44 * 64 + 20] & 0xff) == 0xff && (px[20 * 64 + 20] & 0xffffff) != 0xff, "triangle orientation (wide side at the bottom) wrong");

    /* present through a window and take a screenshot of the presented frame */
    NVNwindowBuilder wb; L(nvnWindowBuilderSetDevice)(&wb, dev); L(nvnWindowBuilderSetDefaults)(&wb);
    NVNtexture *wt[1] = { tex };
    L(nvnWindowBuilderSetTextures)(&wb, 1, wt); L(nvnWindowBuilderSetPresentInterval)(&wb, 1);
    NVNwindow *win = calloc(1, sizeof(NVNwindow));
    CHECK(L(nvnWindowInitialize)(win, &wb), "window init");
    NVNsync *acq = calloc(1, sizeof(NVNsync)); L(nvnSyncInitialize)(acq, dev);
    int idx = -1;
    CHECK(L(nvnWindowAcquireTexture)(win, acq, &idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS && idx == 0, "acquire");
    CHECK(L(nvnSyncWait)(acq, 1000000000ull) != NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED, "acquire sync");
    remove("build/t_triangle.png");
    napple_gfx_screenshot("build/t_triangle.png");
    L(nvnQueuePresentTexture)(q, win, idx);
    L(nvnQueueFinish)(q);
    for (int i = 0; i < 200 && napple_gfx_screenshot_pending(); i++) usleep(1000);
    usleep(50000);
    CHECK(access("build/t_triangle.png", R_OK) == 0, "screenshot file missing");
    /* second acquire must wait for the display of the first present */
    CHECK(L(nvnWindowAcquireTexture)(win, acq, &idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS, "acquire 2");
    CHECK(L(nvnSyncWait)(acq, 1000000000ull) != NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED, "acquire sync 2 (present never shown)");

    napple_gfx_stats st; napple_gfx_get_stats(&st);
    CHECK(st.draws_total == 1 && st.pipelines_compiled == 1, "stats: draws %llu pipelines %llu", (unsigned long long)st.draws_total, (unsigned long long)st.pipelines_compiled);
    CHECK(st.unimplemented_calls_total == 0, "unimplemented NVN calls hit: %llu", (unsigned long long)st.unimplemented_calls_total);
    if (pass && atoi(pass) == 1) CHECK(st.pipelines_archive_hits == 0, "pass 1: archive hits %llu (cache not fresh?)", (unsigned long long)st.pipelines_archive_hits);
    if (pass && atoi(pass) == 2) {
        CHECK(st.prewarm_total >= 2 && st.prewarm_done == st.prewarm_total, "pass 2: prewarm %u / %u", st.prewarm_done, st.prewarm_total);
        CHECK(st.pipelines_archive_hits == 1, "pass 2: archive hits %llu", (unsigned long long)st.pipelines_archive_hits);
    }
    if (pass) napple_gfx_shutdown();

    printf(g_fail ? "t_triangle: %d FAILED\n" : "t_triangle: ok\n", g_fail);
    napple_gfx_diag_dump();
    return g_fail != 0;
}
