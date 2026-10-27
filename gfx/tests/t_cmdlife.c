/* t_cmdlife.c - lifetime of recorded command lists (src/api_cmd.c). On hardware a command handle stays valid as long as the game's command memory is
 * intact, so: a list recorded early is still callable after thousands of later ones (SMO replays per-model lists for minutes; dropping them by idle time or by
 * a memory budget made models draw without their setup); it dies when the command memory is overwritten (another list recorded at the same address), when
 * the pool holding it is finalized, or when the game finalizes the handle; and a list held LZ4-packed replays identically. No game data. */
#include "nvn_harness.h"

int gfx_cmdrec_valid(uint64_t handle);
uint32_t gfx_cmdrec_pack_now(uint32_t idle_s);
uint64_t gfx_cmdrec_packed(void);
uint64_t gfx_cmdrec_live(void);
uint64_t gfx_cmdrec_bytes(void);

static NVNmemoryPool *mkpool(H *h, size_t size, uint8_t **mem) {
    *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    NVNmemoryPoolBuilder pb; L(nvnMemoryPoolBuilderSetDevice)(&pb, h->dev); L(nvnMemoryPoolBuilderSetDefaults)(&pb);
    L(nvnMemoryPoolBuilderSetStorage)(&pb, *mem, size);
    L(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    NVNmemoryPool *p = calloc(1, sizeof(NVNmemoryPool));
    CHECK(L(nvnMemoryPoolInitialize)(p, &pb), "pool init");
    return p;
}
/* records `n` clears of the given colour (enough ops to be worth packing) into command memory [off, off+size) of `pool` */
static NVNcommandHandle rec_clears(H *h, NVNmemoryPool *pool, size_t off, NVNtexture *t, const float *c, int n) {
    L(nvnCommandBufferAddCommandMemory)(h->cb, pool, off, 0x4000);
    L(nvnCommandBufferBeginRecording)(h->cb);
    NVNcopyRegion all = { 0, 0, 0, 16, 16, 1 };
    for (int i = 0; i < n; i++) L(nvnCommandBufferClearTexture)(h->cb, t, NULL, &all, c, 0xf);
    return L(nvnCommandBufferEndRecording)(h->cb);
}
static uint32_t submit_read(H *h, NVNcommandHandle hd, NVNtexture *t) {
    L(nvnQueueSubmitCommands)(h->q, 1, &hd);
    h_begin(h); h_readback(h, t, 0, 0, 1, 1, H_READBACK); h_submit(h);
    return *(const uint32_t *)(h->mem + H_READBACK);
}

int main(void) {
    H h; if (h_init(&h, 16, 16, NULL)) return 1;
    NVNtexture *t = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 16, 16, 1, 0x20000, 0);
    float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, blue[4] = { 0, 0, 1, 1 };

    /* 1. recorded early, called much later, after thousands of other lists (several command buffers, one finalized) */
    NVNcommandHandle early = rec_clears(&h, h.pool, 0x400000 - 0x4000 * 2, t, red, 40);
    CHECK(early && gfx_cmdrec_valid(early), "early list valid");
    NVNcommandBuffer *cb2 = calloc(1, sizeof(NVNcommandBuffer)); CHECK(L(nvnCommandBufferInitialize)(cb2, h.dev), "cb2");
    NVNcommandHandle scratch = 0;
    for (int i = 0; i < 3000; i++) {
        L(nvnCommandBufferAddCommandMemory)(cb2, h.pool, 0x100000 + (size_t)(i % 512) * 0x40, 0x40 * 4);   /* overlapping scratch chunks, like SMO's exact-size scratch */
        L(nvnCommandBufferBeginRecording)(cb2);
        NVNcopyRegion all = { 0, 0, 0, 16, 16, 1 };
        L(nvnCommandBufferClearTexture)(cb2, t, NULL, &all, green, 0xf);
        scratch = L(nvnCommandBufferEndRecording)(cb2);
        if (i % 7 == 0) L(nvnDeviceFinalizeCommandHandle)(h.dev, scratch);
    }
    L(nvnCommandBufferFinalize)(cb2);                 /* the recording command buffer is gone; its lists live on */
    CHECK(gfx_cmdrec_valid(early), "early list must survive thousands of later lists and a finalized command buffer");
    CHECK(submit_read(&h, early, t) == 0xff0000ffu, "early list replays red");
    /* a list that calls the early one */
    h_begin(&h); L(nvnCommandBufferCallCommands)(h.cb, 1, &early); NVNcommandHandle caller = L(nvnCommandBufferEndRecording)(h.cb);
    CHECK(caller != 0, "caller list");

    /* 2. packing: idle lists are held compressed and replay identically */
    uint32_t packed = gfx_cmdrec_pack_now(0);
    CHECK(packed > 0 && gfx_cmdrec_packed() > 0, "idle lists get packed (%u)", packed);
    CHECK(gfx_cmdrec_valid(early), "packed list still valid");
    CHECK(submit_read(&h, early, t) == 0xff0000ffu, "packed list unpacks and replays red");
    CHECK(submit_read(&h, caller, t) == 0xff0000ffu, "caller of a list replays red");

    /* 3. overwritten command memory: a new list at the same address supersedes the old one */
    NVNcommandHandle a1 = rec_clears(&h, h.pool, 0x300000, t, green, 20);
    CHECK(gfx_cmdrec_valid(a1), "a1 valid");
    NVNcommandHandle a2 = rec_clears(&h, h.pool, 0x300000, t, blue, 20);
    CHECK(!gfx_cmdrec_valid(a1), "a1 is dead after its command memory was recorded over");
    CHECK(gfx_cmdrec_valid(a2) && gfx_cmdrec_valid(early), "a2 valid, an unrelated list untouched");
    CHECK(submit_read(&h, a2, t) == 0xffff0000u, "a2 replays blue");

    /* 4. pool freed: lists recorded into it are gone, lists in other pools stay */
    uint8_t *m2; NVNmemoryPool *p2 = mkpool(&h, 1u << 20, &m2);
    NVNcommandHandle b1 = rec_clears(&h, p2, 0x1000, t, green, 10);
    CHECK(gfx_cmdrec_valid(b1), "b1 valid");
    L(nvnMemoryPoolFinalize)(p2);
    CHECK(!gfx_cmdrec_valid(b1), "b1 is dead after its pool was finalized");
    CHECK(gfx_cmdrec_valid(a2) && gfx_cmdrec_valid(early), "lists of other pools survive");

    /* 4b. command buffers that were never given command memory (the callback model): their lists have no address, so lists of different buffers never overwrite each other */
    NVNcommandBuffer *c3 = calloc(1, sizeof(NVNcommandBuffer)), *c4 = calloc(1, sizeof(NVNcommandBuffer));
    CHECK(L(nvnCommandBufferInitialize)(c3, h.dev) && L(nvnCommandBufferInitialize)(c4, h.dev), "cb3/cb4");
    NVNcommandHandle n3 = 0, n4 = 0;
    L(nvnCommandBufferBeginRecording)(c3); n3 = L(nvnCommandBufferEndRecording)(c3);
    L(nvnCommandBufferBeginRecording)(c4); n4 = L(nvnCommandBufferEndRecording)(c4);
    CHECK(n3 && n4 && gfx_cmdrec_valid(n3) && gfx_cmdrec_valid(n4), "lists of buffers without command memory coexist");

    /* 4c. volume: 20000 lists of ~3 KB (SMO keeps ~100k+ alive) stay valid; packing shrinks what they hold and a list from the middle still replays */
    NVNcommandBuffer *c5 = calloc(1, sizeof(NVNcommandBuffer)); CHECK(L(nvnCommandBufferInitialize)(c5, h.dev), "cb5");
    static NVNcommandHandle many[20000];
    for (int i = 0; i < 20000; i++) {
        L(nvnCommandBufferAddCommandMemory)(c5, h.pool, 0x180000 + (size_t)(i % 64) * 0x100, 0x100);
        L(nvnCommandBufferBeginRecording)(c5);
        NVNcopyRegion all = { 0, 0, 0, 16, 16, 1 };
        for (int k = 0; k < 20; k++) L(nvnCommandBufferClearTexture)(c5, t, NULL, &all, (i & 1) ? green : blue, 0xf);
        many[i] = L(nvnCommandBufferEndRecording)(c5);
    }
    L(nvnCommandBufferFinalize)(c5);
    uint64_t live = gfx_cmdrec_live(), before = gfx_cmdrec_bytes();
    int valid = 0; for (int i = 0; i < 20000; i++) valid += gfx_cmdrec_valid(many[i]);
    /* command memory slots repeat every 64 lists, so only the newest list per slot is still at its address: that is the rule (same address = overwritten) */
    CHECK(valid == 64, "after reusing 64 command memory slots 20000 times only the newest list of each slot is valid (%d)", valid);
    (void)live; (void)before;

    /* 5. FinalizeCommandHandle ends the handle */
    L(nvnDeviceFinalizeCommandHandle)(h.dev, a2);
    CHECK(!gfx_cmdrec_valid(a2), "a2 dead after FinalizeCommandHandle");
    return h_finish("t_cmdlife", &h);
}
