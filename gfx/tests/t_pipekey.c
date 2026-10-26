/* t_pipekey.c - first-use stutter work (G11, docs/changes/2026-10-25-nx-shader.md):
 *  1. pipeline key normalisation: draws that differ only in state that cannot change the Metal pipeline (blend factors with blending off, the write
 *     mask of an absent render target, the format of an attribute the vertex program never reads, the stride of a stream only that attribute uses)
 *     build ONE pipeline; enabling blending (a real difference) builds a second.
 *  2. speculation: programs registered with no persisted key get their likely variants compiled in the background, and the first draw uses them.
 *  3. persisted keys survive a translator version bump: the run re-executes itself (pass 2) after rewriting the pipelines.bin header to an older
 *     version, registers the programs and expects the persisted pipelines to be queued for prewarm. */
#include <time.h>
#include "nvn_test.h"
#include "internal.h"

#define CACHE "build/t_pipekey-cache"
void mtl_shader_spec_stats(uint64_t *compiled, uint64_t *used);

static NVNdevice *dev;
static uint8_t *mem;
static NVNprogram *prog;
static NVNtexture *rt;

static void make_program(void) {
    static uint8_t vctl[0x880], fctl[0x880];
    uint64_t vs_i[] = { ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit() };                  /* reads attribute 0 only */
    build_shader(mem + 0x1000, vctl, 0, vs_i, 3, 0);
    uint64_t fs_i[] = { ins_mov32i(0, 0x3f800000), ins_mov32i(1, 0x3e800000), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit() };
    build_shader(mem + 0x4000, fctl, 1, fs_i, 5, 0xf);
    prog = calloc(1, sizeof(NVNprogram));
    CHECK(L(nvnProgramInitialize)(prog, dev), "program init");
    NVNshaderData sd[2] = { { (NVNbufferAddress)(uintptr_t)(mem + 0x1000), vctl }, { (NVNbufferAddress)(uintptr_t)(mem + 0x4000), fctl } };
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "set shaders");
}

/* variant v: 0 base; 1 other blend factors (blending off); 2 write mask of absent target 1; 3 unused attribute 1 as RGBA8; 4 unused stream 1 stride 8;
 * 5 blending on (a different pipeline) */
static void draw_variant(NVNcommandBuffer *cb, int v) {
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnColorStateSetBlendEnable)(&cs, 0, v == 5); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs);
    if (v == 1 || v == 5) L(nvnBlendStateSetBlendFunc)(&bs, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO);
    L(nvnCommandBufferBindBlendState)(cb, &bs);
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm);
    if (v == 2) L(nvnChannelMaskStateSetChannelMask)(&cm, 1, 1, 0, 0, 0);
    L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNvertexAttribState va[2];
    L(nvnVertexAttribStateSetDefaults)(&va[0]); L(nvnVertexAttribStateSetFormat)(&va[0], NVN_FORMAT_RGBA32F, 0); L(nvnVertexAttribStateSetStreamIndex)(&va[0], 0);
    L(nvnVertexAttribStateSetDefaults)(&va[1]); L(nvnVertexAttribStateSetFormat)(&va[1], v == 3 ? NVN_FORMAT_RGBA8 : NVN_FORMAT_RGBA32F, 0); L(nvnVertexAttribStateSetStreamIndex)(&va[1], 1);
    NVNvertexStreamState vst[2];
    L(nvnVertexStreamStateSetDefaults)(&vst[0]); L(nvnVertexStreamStateSetStride)(&vst[0], 16);
    L(nvnVertexStreamStateSetDefaults)(&vst[1]); L(nvnVertexStreamStateSetStride)(&vst[1], v == 4 ? 8 : 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 2, va); L(nvnCommandBufferBindVertexStreamState)(cb, 2, vst);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)mem, 48);
    L(nvnCommandBufferBindVertexBuffer)(cb, 1, (NVNbufferAddress)(uintptr_t)mem, 48);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
}

static void draw_all(int nvar) {
    NVNqueueBuilder qb; L(nvnQueueBuilderSetDevice)(&qb, dev); L(nvnQueueBuilderSetDefaults)(&qb);
    NVNqueue *q = calloc(1, sizeof(NVNqueue));
    CHECK(L(nvnQueueInitialize)(q, &qb), "queue init");
    NVNcommandBuffer *cb = calloc(1, sizeof(NVNcommandBuffer));
    CHECK(L(nvnCommandBufferInitialize)(cb, dev), "cmdbuf init");
    L(nvnCommandBufferBeginRecording)(cb);
    const NVNtexture *rts[1] = { rt };
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, 32, 32); L(nvnCommandBufferSetScissor)(cb, 0, 0, 32, 32);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNpolygonState ps; L(nvnPolygonStateSetDefaults)(&ps); L(nvnCommandBufferBindPolygonState)(cb, &ps);
    for (int v = 0; v < nvar; v++) draw_variant(cb, v);
    NVNcommandHandle h = L(nvnCommandBufferEndRecording)(cb);
    L(nvnQueueSubmitCommands)(q, 1, &h);
    L(nvnQueueFinish)(q);
}

int main(int argc, char **argv) {
    (void)argc;
    const char *ps_ = getenv("T_PK_PASS");
    int pass = ps_ ? atoi(ps_) : 1;
    if (pass == 1) system("rm -rf " CACHE);
    setenv("NAPPLE_GFX_SPECULATE", getenv("T_PK_NOSPEC") ? "0" : "1", 1);      /* speculation is opt-in */
    napple_gfx_config cfg; napple_gfx_config_default(&cfg);
    cfg.headless_width = 32; cfg.headless_height = 32; cfg.cache_dir = CACHE;
    cfg.settings.prewarm_shader_cache = 0;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    dev = calloc(1, sizeof(NVNdevice));
    NVNdeviceBuilder db; L(nvnDeviceBuilderSetDefaults)(&db);
    CHECK(L(nvnDeviceInitialize)(dev, &db), "device init");
    size_t PS = 1 << 20;
    mem = mmap(NULL, PS, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    float tri[12] = { -1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1 };
    memcpy(mem, tri, sizeof tri);
    NVNmemoryPoolBuilder pb; L(nvnMemoryPoolBuilderSetDevice)(&pb, dev); L(nvnMemoryPoolBuilderSetDefaults)(&pb);
    L(nvnMemoryPoolBuilderSetStorage)(&pb, mem, PS); L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    NVNmemoryPool *pool = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(pool, &pb), "pool init");
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D); L(nvnTextureBuilderSetSize2D)(&tb, 32, 32); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    L(nvnTextureBuilderSetStorage)(&tb, pool, 0x80000);
    rt = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(rt, &tb), "texture init");

    make_program();
    if (pass == 1) {
        for (int i = 0; i < 200; i++) { uint64_t n, u; mtl_shader_spec_stats(&n, &u); if (n >= 2 && atomic_load(&g_gfx.shaders_pending) == 0) break; usleep(10000); }
        draw_all(5);
        napple_gfx_stats st; napple_gfx_get_stats(&st);
        CHECK(st.draws_total == 5, "draws %llu", (unsigned long long)st.draws_total);
        CHECK(st.pipelines_compiled == 1, "5 draws differing only in pipeline-irrelevant state built %llu pipelines, expected 1", (unsigned long long)st.pipelines_compiled);
        uint64_t n, u; mtl_shader_spec_stats(&n, &u);
        CHECK(n == 2 && u == 2, "speculation: %llu variants compiled at registration, %llu used by the draws (expected 2 and 2)", (unsigned long long)n, (unsigned long long)u);
        draw_all(6);
        napple_gfx_get_stats(&st);
        CHECK(st.pipelines_compiled == 2, "blending on must build its own pipeline: %llu pipelines", (unsigned long long)st.pipelines_compiled);
        napple_gfx_shutdown();
        /* pretend the keys were written by an older translator */
        FILE *f = fopen(CACHE "/pipelines.bin", "r+b");
        CHECK(f != NULL, "pipelines.bin written");
        if (f) { uint32_t hdr[4]; if (fread(hdr, 4, 4, f) == 4) { hdr[1] = 1; fseek(f, 0, SEEK_SET); fwrite(hdr, 4, 4, f); } fclose(f); }
        if (!g_fail) {
            char cmd[1200]; snprintf(cmd, sizeof cmd, "T_PK_PASS=2 %s", argv[0]);
            int rc = system(cmd);
            CHECK(rc == 0, "pass 2 failed");
        }
    } else {
        uint64_t pre = atomic_load(&g_gfx.pipelines_prewarmed);
        CHECK(pre == 2, "pass 2: %llu persisted pipelines queued for prewarm after a translator version change, expected 2", (unsigned long long)pre);
        napple_gfx_shutdown();
    }
    printf(g_fail ? "t_pipekey pass %d: %d FAILED\n" : "t_pipekey pass %d: ok\n", pass, g_fail);
    return g_fail != 0;
}
