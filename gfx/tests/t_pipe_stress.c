/* t_pipe_stress.c - pipeline-build hang regression (docs/archive/changes/2026-10-24-nvn-pipeline.md): many persisted pipelines, prewarm running while the
 * game draws, GCD's shared worker pool flooded with blocked work. Pass 1 (T_STRESS_PASS=1) draws NFS fragment programs x NBLEND blend modes into a
 * fresh cache so pipelines.bin lists them all. Pass 2 (T_STRESS_PASS=2) starts with that cache, floods the global queues with sleeping blocks,
 * registers the programs (which queues every persisted pipeline for prewarm) and draws the combinations in reverse queue order, from T_STRESS_THREADS
 * threads (own queue and command buffer each) without waiting for the launch-time shader prewarm. A watchdog fails the run if it does not finish. */
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include "nvn_test.h"
#include "internal.h"

#define NFS 48
#define NBLEND 4
#define NCOMBO (NFS * NBLEND)
#define CACHE "build/t_pipe_stress-cache"
#define WATCHDOG_S 120

static NVNdevice *dev;
static NVNmemoryPool *pool;
static uint8_t *mem;
static NVNprogram *progs[NFS];
static NVNtexture *rt;

static void on_alarm(int sig) { (void)sig; static const char m[] = "FAIL: t_pipe_stress hung (watchdog)\n"; write(1, m, sizeof m - 1); _exit(2); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void sleeper(void *ctx) { (void)ctx; sleep(3); }

/* Program i: shared VS, FS writing a colour unique to i (so every FS is a distinct blob, MSL file and pipeline). */
static void make_programs(void) {
    static uint8_t vctl[0x880], fctl[NFS][0x880];
    uint64_t vs_i[] = { ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit() };
    build_shader(mem + 0x1000, vctl, 0, vs_i, 3, 0);
    for (int i = 0; i < NFS; i++) {
        uint32_t g = 0x3c000000u + (uint32_t)i * 0x00080000u;           /* small distinct floats */
        uint64_t fs_i[] = { ins_mov32i(0, 0x3f800000), ins_mov32i(1, g), ins_mov32i(2, 0), ins_mov32i(3, 0x3f800000), ins_exit() };
        build_shader(mem + 0x4000 + (size_t)i * 0x1000, fctl[i], 1, fs_i, 5, 0xf);
        progs[i] = calloc(1, sizeof(NVNprogram));
        CHECK(L(nvnProgramInitialize)(progs[i], dev), "program init");
        NVNshaderData sd[2] = { { (NVNbufferAddress)(uintptr_t)(mem + 0x1000), vctl }, { (NVNbufferAddress)(uintptr_t)(mem + 0x4000 + (size_t)i * 0x1000), fctl[i] } };
        CHECK(L(nvnProgramSetShaders)(progs[i], 2, sd), "set shaders %d", i);
    }
}

static void draw_combo(NVNcommandBuffer *cb, int c) {
    int f = c / NBLEND, b = c % NBLEND;
    NVNcolorState cs; L(nvnColorStateSetDefaults)(&cs); L(nvnColorStateSetBlendEnable)(&cs, 0, b != 0); L(nvnCommandBufferBindColorState)(cb, &cs);
    NVNblendState bs; L(nvnBlendStateSetDefaults)(&bs);
    static const NVNblendFunc src[NBLEND] = { NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_DST_COLOR };
    static const NVNblendFunc dst[NBLEND] = { NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO };
    L(nvnBlendStateSetBlendFunc)(&bs, src[b], dst[b], src[b], dst[b]);
    L(nvnCommandBufferBindBlendState)(cb, &bs);
    L(nvnCommandBufferBindProgram)(cb, progs[f], NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
}

typedef struct { int first, step; } job;
static void *drawer(void *arg) {
    const job *j = arg;
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
    NVNchannelMaskState cm; L(nvnChannelMaskStateSetDefaults)(&cm); L(nvnCommandBufferBindChannelMaskState)(cb, &cm);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0); L(nvnVertexAttribStateSetStreamIndex)(&va, 0);
    NVNvertexStreamState vstream; L(nvnVertexStreamStateSetDefaults)(&vstream); L(nvnVertexStreamStateSetStride)(&vstream, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va); L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vstream);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)mem, 48);
    for (int c = j->first; c >= 0 && c < NCOMBO; c += j->step) draw_combo(cb, c);
    NVNcommandHandle h = L(nvnCommandBufferEndRecording)(cb);
    L(nvnQueueSubmitCommands)(q, 1, &h);
    L(nvnQueueFinish)(q);
    return NULL;
}

int main(void) {
    const char *ps_ = getenv("T_STRESS_PASS");
    int pass = ps_ ? atoi(ps_) : 0;
    if (pass != 1 && pass != 2) { printf("t_pipe_stress: set T_STRESS_PASS=1 then 2 (make test does)\n"); return 0; }
    int nthreads = getenv("T_STRESS_THREADS") ? atoi(getenv("T_STRESS_THREADS")) : 4;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 16) nthreads = 16;
    signal(SIGALRM, on_alarm); alarm(WATCHDOG_S);
    if (pass == 1) system("rm -rf " CACHE);

    napple_gfx_config cfg; napple_gfx_config_default(&cfg);
    cfg.headless_width = 32; cfg.headless_height = 32; cfg.cache_dir = CACHE;
    cfg.settings.async_shader_compile = 0;
    cfg.settings.prewarm_shader_cache = getenv("T_STRESS_NO_SHADER_PREWARM") == NULL;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    double t0 = now_s();
    if (pass == 2 && !getenv("T_STRESS_NOFLOOD")) {   /* a full pool: 4x as many blocked blocks as GCD will run on the user-initiated pool */
        for (int i = 0; i < 256; i++) dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), NULL, sleeper);
        for (int i = 0; i < 256; i++) dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), NULL, sleeper);
    }

    dev = calloc(1, sizeof(NVNdevice));
    NVNdeviceBuilder db; L(nvnDeviceBuilderSetDefaults)(&db);
    CHECK(L(nvnDeviceInitialize)(dev, &db), "device init");
    size_t PS = 1 << 20;
    mem = mmap(NULL, PS, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    float tri[12] = { -1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1 };
    memcpy(mem, tri, sizeof tri);
    NVNmemoryPoolBuilder pb; L(nvnMemoryPoolBuilderSetDevice)(&pb, dev); L(nvnMemoryPoolBuilderSetDefaults)(&pb);
    L(nvnMemoryPoolBuilderSetStorage)(&pb, mem, PS); L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    pool = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(pool, &pb), "pool init");
    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D); L(nvnTextureBuilderSetSize2D)(&tb, 32, 32); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    L(nvnTextureBuilderSetStorage)(&tb, pool, 0x80000);
    rt = calloc(1, sizeof(NVNtexture));
    CHECK(L(nvnTextureInitialize)(rt, &tb), "texture init");

    make_programs();                                  /* pass 2: every persisted pipeline is now queued for prewarm */
    napple_gfx_stats st; napple_gfx_get_stats(&st);
    printf("programs registered after %.2fs; shader prewarm %u / %u, pipelines queued for prewarm %llu\n", now_s() - t0, st.prewarm_done, st.prewarm_total,
           (unsigned long long)atomic_load(&g_gfx.pipelines_prewarmed));

    pthread_t th[16]; job jobs[16];
    for (int i = 0; i < nthreads; i++) { jobs[i].first = NCOMBO - 1 - i; jobs[i].step = -nthreads; pthread_create(&th[i], NULL, drawer, &jobs[i]); }
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    double dt = now_s() - t0;

    napple_gfx_get_stats(&st);
    uint64_t wait_us = atomic_load(&g_gfx.pipeline_wait_us), pre = atomic_load(&g_gfx.pipelines_prewarmed);
    printf("pass %d: %d threads, %llu draws in %.2fs; pipelines compiled %llu, prewarmed %llu, archive hits %llu, pipeline wait %.1f ms, shader prewarm %u / %u\n",
           pass, nthreads, (unsigned long long)st.draws_total, dt, (unsigned long long)st.pipelines_compiled, (unsigned long long)pre,
           (unsigned long long)st.pipelines_archive_hits, wait_us / 1000.0, st.prewarm_done, st.prewarm_total);
    CHECK(st.draws_total == NCOMBO, "draws %llu, expected %d", (unsigned long long)st.draws_total, NCOMBO);
    CHECK(st.draws_skipped_pending_shader == 0, "%llu draws skipped", (unsigned long long)st.draws_skipped_pending_shader);
    if (pass == 2) {
        CHECK(pre == NCOMBO, "pass 2: %llu pipelines queued for prewarm, expected %d", (unsigned long long)pre, NCOMBO);
        CHECK(getenv("T_STRESS_NO_SHADER_PREWARM") || st.prewarm_total >= NFS, "pass 2: shader prewarm total %u", st.prewarm_total);
    }
    CHECK(st.unimplemented_calls_total == 0, "unimplemented NVN calls hit: %llu", (unsigned long long)st.unimplemented_calls_total);
    napple_gfx_shutdown();
    printf(g_fail ? "t_pipe_stress: %d FAILED\n" : "t_pipe_stress: ok\n", g_fail);
    return g_fail != 0;
}
