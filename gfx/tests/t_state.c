/* t_state.c - render state: depth test across draws, scissored clear (draw fallback), clear folded into the load action, blending. */
#include "nvn_test.h"

typedef struct { NVNdevice *dev; NVNqueue *q; uint8_t *mem; NVNprogram *prog[3]; } ctx;

static NVNprogram *make_prog(ctx *c, int slot, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    static uint8_t vctl[0x880], fctl[0x880];
    uint64_t vs[] = { ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit() };
    uint64_t fs[] = { ins_mov32i(0, r), ins_mov32i(1, g), ins_mov32i(2, b), ins_mov32i(3, a), ins_exit() };
    uint8_t *vm = c->mem + 0x1000 + slot * 0x2000, *fm = vm + 0x1000;
    build_shader(vm, vctl, 0, vs, 3, 0); build_shader(fm, fctl, 1, fs, 5, 0xf);
    NVNprogram *p = calloc(1, sizeof(NVNprogram)); L(nvnProgramInitialize)(p, c->dev);
    NVNshaderData sd[2] = { { (NVNbufferAddress)(uintptr_t)vm, vctl }, { (NVNbufferAddress)(uintptr_t)fm, fctl } };
    CHECK(L(nvnProgramSetShaders)(p, 2, sd), "program %d", slot);
    return p;
}

int main(void) {
    napple_gfx_config cfg; napple_gfx_config_default(&cfg);
    cfg.headless_width = 32; cfg.headless_height = 32; cfg.settings.async_shader_compile = 0;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    ctx c; c.dev = calloc(1, sizeof(NVNdevice));
    NVNdeviceBuilder db; L(nvnDeviceBuilderSetDefaults)(&db); L(nvnDeviceInitialize)(c.dev, &db);
    L(nvnDeviceSetWindowOriginMode)(c.dev, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT); L(nvnDeviceSetDepthMode)(c.dev, NVN_DEPTH_MODE_NEAR_IS_ZERO);
    size_t PS = 4 << 20;
    c.mem = mmap(NULL, PS, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    NVNmemoryPoolBuilder pb; L(nvnMemoryPoolBuilderSetDevice)(&pb, c.dev); L(nvnMemoryPoolBuilderSetDefaults)(&pb);
    L(nvnMemoryPoolBuilderSetStorage)(&pb, c.mem, PS); L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    NVNmemoryPool *pool = calloc(1, sizeof(NVNmemoryPool)); L(nvnMemoryPoolInitialize)(pool, &pb);
    NVNqueueBuilder qb; L(nvnQueueBuilderSetDevice)(&qb, c.dev); L(nvnQueueBuilderSetDefaults)(&qb);
    c.q = calloc(1, sizeof(NVNqueue)); L(nvnQueueInitialize)(c.q, &qb);

    /* three fullscreen triangles at depths 0.5, 0.2, 0.8 */
    float z[3] = { 0.5f, 0.2f, 0.8f };
    for (int i = 0; i < 3; i++) { float t[12] = { -1, -1, z[i], 1, 3, -1, z[i], 1, -1, 3, z[i], 1 }; memcpy(c.mem + 0x100 * (i + 1) * 0 + 0x400 * i, t, sizeof t); }
    c.prog[0] = make_prog(&c, 0, 0x3f800000, 0, 0, 0x3f800000);          /* red   */
    c.prog[1] = make_prog(&c, 1, 0, 0x3f800000, 0, 0x3f800000);          /* green */
    c.prog[2] = make_prog(&c, 2, 0, 0, 0x3f800000, 0x3f800000);          /* blue  */

    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, c.dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetSize2D)(&tb, 32, 32); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8); L(nvnTextureBuilderSetStorage)(&tb, pool, 0x20000);
    NVNtexture *rt = calloc(1, sizeof(NVNtexture)); CHECK(L(nvnTextureInitialize)(rt, &tb), "rt");
    L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_DEPTH32F); L(nvnTextureBuilderSetStorage)(&tb, pool, 0x40000);
    NVNtexture *ds = calloc(1, sizeof(NVNtexture)); CHECK(L(nvnTextureInitialize)(ds, &tb), "depth texture");

    NVNcommandBuffer *cb = calloc(1, sizeof(NVNcommandBuffer)); L(nvnCommandBufferInitialize)(cb, c.dev);
    L(nvnCommandBufferBeginRecording)(cb);
    const NVNtexture *rts[1] = { rt };
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, ds, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, 32, 32); L(nvnCommandBufferSetScissor)(cb, 0, 0, 32, 32);
    float black[4] = { 0, 0, 0, 1 };
    L(nvnCommandBufferClearColor)(cb, 0, black, 0xf);
    L(nvnCommandBufferClearDepthStencil)(cb, 1.0f, 1, 0, 0);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss);
    L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 1); L(nvnDepthStencilStateSetDepthWriteEnable)(&dss, 1); L(nvnDepthStencilStateSetDepthFunc)(&dss, NVN_DEPTH_FUNC_LESS);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0);
    NVNvertexStreamState vs; L(nvnVertexStreamStateSetDefaults)(&vs); L(nvnVertexStreamStateSetStride)(&vs, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va); L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vs);
    for (int i = 0; i < 3; i++) {       /* red z=.5, green z=.2 (wins), blue z=.8 (rejected) */
        L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)(c.mem + 0x400 * i), 48);
        L(nvnCommandBufferBindProgram)(cb, c.prog[i], NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
        L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    }
    /* partial clear with a scissor: only the top-left 8x8 becomes magenta (needs the draw fallback: a pass is already open) */
    L(nvnCommandBufferSetScissor)(cb, 0, 0, 8, 8);
    float mag[4] = { 1, 0, 1, 1 };
    L(nvnCommandBufferClearColor)(cb, 0, mag, 0xf);
    NVNcopyRegion region = { 0, 0, 0, 32, 32, 1 };
    L(nvnCommandBufferCopyTextureToBuffer)(cb, rt, NULL, &region, (NVNbufferAddress)(uintptr_t)(c.mem + 0x10000), 0);
    NVNcommandHandle h = L(nvnCommandBufferEndRecording)(cb);
    L(nvnQueueSubmitCommands)(c.q, 1, &h); L(nvnQueueFinish)(c.q);

    const uint32_t *px = (const uint32_t *)(c.mem + 0x10000);
    printf("pixels: (16,16) %08x (2,2) %08x\n", px[16 * 32 + 16], px[2 * 32 + 2]);
    CHECK(px[16 * 32 + 16] == 0xff00ff00u, "depth test: green (nearest) expected, got %08x", px[16 * 32 + 16]);
    CHECK(px[2 * 32 + 2] == 0xffff00ffu, "scissored clear: magenta expected in the corner, got %08x", px[2 * 32 + 2]);
    CHECK(px[2 * 32 + 20] == 0xff00ff00u, "scissored clear must not touch outside the scissor, got %08x", px[2 * 32 + 20]);

    napple_gfx_stats st; napple_gfx_get_stats(&st);
    CHECK(st.draws_total == 3, "draws %llu", (unsigned long long)st.draws_total);
    CHECK(st.unimplemented_calls_total == 0, "unimplemented calls %llu", (unsigned long long)st.unimplemented_calls_total);
    printf(g_fail ? "t_state: %d FAILED\n" : "t_state: ok\n", g_fail);
    return g_fail != 0;
}
