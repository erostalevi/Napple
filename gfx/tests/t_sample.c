/* t_sample.c - texture path end to end: CPU writes a 2x2 texture through nvnTextureWriteTexels (block-linear storage), the replay layer
 * deswizzles and uploads it (dirty-page tracked), a fragment shader samples it through a texture/sampler pool handle. Then the CPU rewrites
 * a texel and the next draw must show the change (write-protection dirty tracking). */
#include "nvn_test.h"
#include <signal.h>

static void on_fault(int sig, siginfo_t *si, void *uc) {
    if (napple_gfx_handle_fault(si->si_addr, 1, uc)) return;
    signal(sig, SIG_DFL);
}

static uint32_t render_and_read(NVNdevice *dev, NVNqueue *q, uint8_t *mem, NVNtexture *rt, NVNprogram *prog, NVNtexturePool *tp, NVNsamplerPool *sp, uint64_t handle) {
    NVNcommandBuffer *cb = calloc(1, sizeof(NVNcommandBuffer));
    L(nvnCommandBufferInitialize)(cb, dev);
    L(nvnCommandBufferBeginRecording)(cb);
    const NVNtexture *rts[1] = { rt };
    L(nvnCommandBufferSetRenderTargets)(cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferSetViewport)(cb, 0, 0, 16, 16);
    L(nvnCommandBufferSetScissor)(cb, 0, 0, 16, 16);
    NVNdepthStencilState dss; L(nvnDepthStencilStateSetDefaults)(&dss); L(nvnDepthStencilStateSetDepthTestEnable)(&dss, 0); L(nvnDepthStencilStateSetDepthWriteEnable)(&dss, 0);
    L(nvnCommandBufferBindDepthStencilState)(cb, &dss);
    NVNvertexAttribState va; L(nvnVertexAttribStateSetDefaults)(&va); L(nvnVertexAttribStateSetFormat)(&va, NVN_FORMAT_RGBA32F, 0);
    NVNvertexStreamState vs; L(nvnVertexStreamStateSetDefaults)(&vs); L(nvnVertexStreamStateSetStride)(&vs, 16);
    L(nvnCommandBufferBindVertexAttribState)(cb, 1, &va);
    L(nvnCommandBufferBindVertexStreamState)(cb, 1, &vs);
    L(nvnCommandBufferBindVertexBuffer)(cb, 0, (NVNbufferAddress)(uintptr_t)mem, 48);
    L(nvnCommandBufferBindProgram)(cb, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    L(nvnCommandBufferSetTexturePool)(cb, tp);
    L(nvnCommandBufferSetSamplerPool)(cb, sp);
    L(nvnCommandBufferBindTexture)(cb, NVN_SHADER_STAGE_FRAGMENT, 0, handle);
    L(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    NVNcopyRegion region = { 0, 0, 0, 16, 16, 1 };
    L(nvnCommandBufferCopyTextureToBuffer)(cb, rt, NULL, &region, (NVNbufferAddress)(uintptr_t)(mem + 0x10000), 0);
    NVNcommandHandle h = L(nvnCommandBufferEndRecording)(cb);
    L(nvnQueueSubmitCommands)(q, 1, &h);
    L(nvnQueueFinish)(q);
    L(nvnCommandBufferFinalize)(cb);
    return ((const uint32_t *)(mem + 0x10000))[8 * 16 + 8];
}

int main(void) {
    struct sigaction sa = { .sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_NODEFER };
    sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL);

    napple_gfx_config cfg; napple_gfx_config_default(&cfg);
    cfg.headless_width = 16; cfg.headless_height = 16; cfg.settings.async_shader_compile = 0;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    NVNdevice *dev = calloc(1, sizeof(NVNdevice));
    NVNdeviceBuilder db; L(nvnDeviceBuilderSetDefaults)(&db); L(nvnDeviceInitialize)(dev, &db);
    L(nvnDeviceSetWindowOriginMode)(dev, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT); L(nvnDeviceSetDepthMode)(dev, NVN_DEPTH_MODE_NEAR_IS_ZERO);

    size_t PS = 4 << 20;                       /* 16 KiB aligned anonymous memory, as the runtime provides for guest heaps */
    uint8_t *mem = mmap(NULL, PS, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    NVNmemoryPoolBuilder pb; L(nvnMemoryPoolBuilderSetDevice)(&pb, dev); L(nvnMemoryPoolBuilderSetDefaults)(&pb);
    L(nvnMemoryPoolBuilderSetStorage)(&pb, mem, PS); L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    NVNmemoryPool *pool = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(pool, &pb), "pool");

    /* fullscreen triangle; fragment shader samples (0.25, 0.75): texel x=0, y=1 of the 2x2 texture */
    float tri[12] = { -1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1 };
    memcpy(mem, tri, sizeof tri);
    uint64_t vs_i[] = { ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit() };
    uint64_t fs_i[] = { ins_mov32i(4, 0x3e800000), ins_mov32i(5, 0x3f400000), ins_tex2d_lz(0, 4, 4, 8), ins_exit() };
    static uint8_t vctl[0x880], fctl[0x880];
    build_shader(mem + 0x1000, vctl, 0, vs_i, 3, 0);
    build_shader(mem + 0x3000, fctl, 1, fs_i, 4, 0xf);
    NVNprogram *prog = calloc(1, sizeof(NVNprogram));
    L(nvnProgramInitialize)(prog, dev);
    NVNshaderData sd[2] = { { (NVNbufferAddress)(uintptr_t)(mem + 0x1000), vctl }, { (NVNbufferAddress)(uintptr_t)(mem + 0x3000), fctl } };
    CHECK(L(nvnProgramSetShaders)(prog, 2, sd), "shaders");

    NVNtextureBuilder tb; L(nvnTextureBuilderSetDevice)(&tb, dev); L(nvnTextureBuilderSetDefaults)(&tb);
    L(nvnTextureBuilderSetSize2D)(&tb, 16, 16); L(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8); L(nvnTextureBuilderSetStorage)(&tb, pool, 0x20000);
    NVNtexture *rt = calloc(1, sizeof(NVNtexture)); CHECK(L(nvnTextureInitialize)(rt, &tb), "rt");
    L(nvnTextureBuilderSetSize2D)(&tb, 2, 2); L(nvnTextureBuilderSetStorage)(&tb, pool, 0x40000);
    NVNtexture *src = calloc(1, sizeof(NVNtexture)); CHECK(L(nvnTextureInitialize)(src, &tb), "src");

    uint32_t texels[4] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu };   /* (0,0) red (1,0) green (0,1) blue (1,1) white, RGBA8 */
    NVNcopyRegion all = { 0, 0, 0, 2, 2, 1 };
    L(nvnTextureWriteTexels)(src, NULL, &all, texels);
    uint32_t back[4] = { 0 };
    L(nvnTextureReadTexels)(src, NULL, &all, back);
    CHECK(!memcmp(back, texels, sizeof back), "write/read texels round trip");

    NVNtexturePool *tp = calloc(1, sizeof(NVNtexturePool)); NVNsamplerPool *sp = calloc(1, sizeof(NVNsamplerPool));
    CHECK(L(nvnTexturePoolInitialize)(tp, pool, 0x80000, 512), "texture pool");
    CHECK(L(nvnSamplerPoolInitialize)(sp, pool, 0x90000, 64), "sampler pool");
    L(nvnTexturePoolRegisterTexture)(tp, 300, src, NULL);
    NVNsamplerBuilder smb; L(nvnSamplerBuilderSetDevice)(&smb, dev); L(nvnSamplerBuilderSetDefaults)(&smb);
    L(nvnSamplerBuilderSetMinMagFilter)(&smb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    L(nvnSamplerBuilderSetWrapMode)(&smb, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE);
    NVNsampler *smp = calloc(1, sizeof(NVNsampler)); L(nvnSamplerInitialize)(smp, &smb);
    L(nvnSamplerPoolRegisterSampler)(sp, 5, smp);
    uint64_t handle = L(nvnDeviceGetTextureHandle)(dev, 300, 5);

    NVNqueueBuilder qb; L(nvnQueueBuilderSetDevice)(&qb, dev); L(nvnQueueBuilderSetDefaults)(&qb);
    NVNqueue *q = calloc(1, sizeof(NVNqueue)); L(nvnQueueInitialize)(q, &qb);

    uint32_t p1 = render_and_read(dev, q, mem, rt, prog, tp, sp, handle);
    printf("sampled texel: %08x (blue = ffff0000 expected)\n", p1);
    CHECK(p1 == 0xffff0000u, "expected the blue texel (x=0,y=1), got %08x", p1);

    /* the CPU rewrites the texel: the page was write-protected after upload, the fault marks it dirty and the next use uploads again */
    uint32_t red = 0xff0000ffu;
    NVNcopyRegion one = { 0, 1, 0, 1, 1, 1 };
    L(nvnTextureWriteTexels)(src, NULL, &one, &red);
    uint32_t p2 = render_and_read(dev, q, mem, rt, prog, tp, sp, handle);
    CHECK(p2 == red, "texture change not picked up, got %08x", p2);
    /* a raw guest store to a protected page (not through the API) is caught by the fault handler */
    uint8_t *raw = mem + 0x40000;
    uint32_t before = ((volatile uint32_t *)raw)[0];
    (void)before;
    L(nvnTextureReadTexels)(src, NULL, &all, back);       /* the texture memory is read again; no re-protection yet */
    render_and_read(dev, q, mem, rt, prog, tp, sp, handle);   /* upload re-protects the page */
    ((volatile uint8_t *)raw)[0] ^= 0;                        /* a store: faults if protected, handled by napple_gfx_handle_fault */
    CHECK(1, "guest write to a protected page survived");

    napple_gfx_stats st; napple_gfx_get_stats(&st);
    CHECK(st.unimplemented_calls_total == 0, "unimplemented NVN calls: %llu", (unsigned long long)st.unimplemented_calls_total);
    printf(g_fail ? "t_sample: %d FAILED\n" : "t_sample: ok\n", g_fail);
    return g_fail != 0;
}
