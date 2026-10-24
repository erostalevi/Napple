/* api_device.c - NVN device, queue, sync objects and events. */
#include <stdlib.h>
#include <unistd.h>

#include "gfx_mtl.h"
#include "timeline.h"
#include "gfx_mtl.h"

void gfx_api_link_device(void) {}

/* ---------------------------------------------------------------------------------------------------------------
 * Device
 * --------------------------------------------------------------------------------------------------------------- */
#define DEV_MAGIC MAGIC('D', 'E', 'V', '0')

NVN(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder *b) { memset(b, 0, sizeof(dev_builder_t)); }
NVN(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder *b, int f) { ((dev_builder_t *)b)->flags = (uint32_t)f; }
NVN_NOPROTO(nvnDeviceBuilderGetFlags, int, const NVNdeviceBuilder *b) { return (int)((const dev_builder_t *)b)->flags; }

NVN(nvnDeviceInitialize, NVNboolean, NVNdevice *d, const NVNdeviceBuilder *b) {
    dev_t_ *o = (dev_t_ *)d;
    memset(o, 0, sizeof *o);
    o->magic = DEV_MAGIC;
    o->flags = ((const dev_builder_t *)b)->flags;
    o->origin = 0;
    o->depth_mode = 0;
    LOGD("DeviceInitialize flags=%#x", o->flags);
    return 1;
}
NVN(nvnDeviceFinalize, void, NVNdevice *d) { ((dev_t_ *)d)->magic = 0; }
NVN(nvnDeviceSetDebugLabel, void, NVNdevice *d, const char *s) { (void)d; (void)s; }
NVN(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice *d, const char *name) {
    (void)d;
    return (PFNNVNGENERICFUNCPTRPROC)gfx_wrap_callable(nvn_lookup(name), name);
}

/* Device properties of a Tegra X1 (values from nnheaders nvn_DeviceConstantsNX.h, as returned by the real driver). */
static int dev_info(unsigned what, int *out) {
    static const int t[] = {
        [0x00] = 55, [0x01] = 15,       /* API version: overridden by the config */
        [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
        [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
        [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
        [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
        [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
        [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
        [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
        [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
        [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
        [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
        [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
        [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
    };
    if (what >= sizeof t / sizeof *t) return 0;
    *out = t[what];
    if (what == 0 && g_gfx.cfg.nvn_major) *out = (int)g_gfx.cfg.nvn_major;
    if (what == 1 && g_gfx.cfg.nvn_major) *out = (int)g_gfx.cfg.nvn_minor;
    return 1;
}
NVN(nvnDeviceGetInteger, void, const NVNdevice *d, NVNdeviceInfo what, int *out) {
    (void)d;
    if (!dev_info((unsigned)what, out)) {
        *out = 0;
        LOG_ONCE(NAPPLE_LOG_WARN, "nvnDeviceGetInteger: unknown property %#x returns 0", (unsigned)what);
    }
}
NVN(nvnDeviceGetCurrentTimestampInNanoseconds, uint64_t, const NVNdevice *d) { (void)d; return gfx_now_ns(); }
NVN(nvnDeviceGetTimestampInNanoseconds, uint64_t, const NVNdevice *d, const NVNcounterData *c) {
    (void)d;
    return c->timestamp / 12 * 625 + c->timestamp % 12 * 625 / 12;     /* counters are written in 19.2 MHz ticks */
}
NVN(nvnDeviceSetIntermediateShaderCache, void, NVNdevice *d, int n) { (void)d; (void)n; }

/* Combined texture handle: texture id (20 bits) | sampler id << 20. */
NVN(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *d, int tid, int sid) {
    (void)d;
    return (uint64_t)((uint32_t)tid & 0xfffff) | (uint64_t)((uint32_t)sid & 0xfff) << 20;
}
NVN(nvnDeviceGetTexelFetchHandle, NVNtextureHandle, const NVNdevice *d, int tid) { (void)d; return (uint64_t)((uint32_t)tid & 0xfffff); }
NVN(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice *d, int id) { (void)d; return (uint64_t)(uint32_t)id; }
NVN(nvnDeviceGetSeparateTextureHandle, NVNseparateTextureHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateTextureHandle){ (uint64_t)(uint32_t)id }; }
NVN(nvnDeviceGetSeparateSamplerHandle, NVNseparateSamplerHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateSamplerHandle){ (uint64_t)(uint32_t)id << 20 }; }

NVN(nvnDeviceInstallDebugCallback, void, NVNdevice *d, const PFNNVNDEBUGCALLBACKPROC cb, void *data, uint32_t enable) {
    dev_t_ *o = (dev_t_ *)d;
    o->dbg_cb = B(enable) ? (void *)cb : NULL;
    o->dbg_data = data;
}
NVN(nvnDeviceGenerateDebugDomainId, NVNdebugDomainId, const NVNdevice *d, const char *s) { (void)d; return (NVNdebugDomainId)(fnv1a(s ? s : "", s ? strlen(s) : 0, FNV_INIT) & 0x7fffffff); }
NVN(nvnDeviceSetWindowOriginMode, void, NVNdevice *d, NVNwindowOriginMode m) {
    ((dev_t_ *)d)->origin = (uint32_t)m;
    g_gfx.device_origin = (uint32_t)m;
    LOGI("window origin mode: %s", m ? "upper-left" : "lower-left");
}
NVN(nvnDeviceSetDepthMode, void, NVNdevice *d, NVNdepthMode m) {
    ((dev_t_ *)d)->depth_mode = (uint32_t)m;
    g_gfx.device_depth_mode = (uint32_t)m;
    LOGI("depth mode: %s", m ? "near=0" : "near=-w");
}
NVN(nvnDeviceGetWindowOriginMode, NVNwindowOriginMode, const NVNdevice *d) { return (NVNwindowOriginMode)((const dev_t_ *)d)->origin; }
NVN(nvnDeviceGetDepthMode, NVNdepthMode, const NVNdevice *d) { return (NVNdepthMode)((const dev_t_ *)d)->depth_mode; }

/* Fast clear registration is an optimization hint: "not registered" keeps the normal clear path. */
NVN(nvnDeviceRegisterFastClearColor, NVNboolean, NVNdevice *d, const float *c, NVNformat f) { (void)d; (void)c; (void)f; return 0; }
NVN(nvnDeviceRegisterFastClearColori, NVNboolean, NVNdevice *d, const int *c, NVNformat f) { (void)d; (void)c; (void)f; return 0; }
NVN(nvnDeviceRegisterFastClearColorui, NVNboolean, NVNdevice *d, const uint32_t *c, NVNformat f) { (void)d; (void)c; (void)f; return 0; }
NVN(nvnDeviceRegisterFastClearDepth, NVNboolean, NVNdevice *d, float v) { (void)d; (void)v; return 0; }
NVN(nvnDeviceApplyDeferredFinalizes, void, NVNdevice *d, int n) { (void)d; (void)n; }
void gfx_cmdrec_finalize_handle(uint64_t handle);
NVN(nvnDeviceFinalizeCommandHandle, void, NVNdevice *d, NVNcommandHandle h) { (void)d; gfx_cmdrec_finalize_handle(h); }
NVN(nvnDeviceWalkDebugDatabase, void, const NVNdevice *d, NVNdebugObjectType t, PFNNVNWALKDEBUGDATABASECALLBACKPROC cb, void *u) { (void)d; (void)t; (void)cb; (void)u; }
NVN(nvnDeviceIsExternalDebuggerAttached, NVNboolean, const NVNdevice *d) { (void)d; return 0; }
NVN_NOPROTO(nvnDeviceSetAutomaticCheckpointsEnable, void, NVNdevice *d, uint32_t e) { (void)d; (void)e; }
NVN_NOPROTO(nvnDeviceSetAutomaticCheckpointsSamplingInterval, void, NVNdevice *d, int v) { (void)d; (void)v; }
NVN_NOPROTO(nvnDeviceSetAutomaticCheckpointsByDebugGroup, void, NVNdevice *d, uint32_t e) { (void)d; (void)e; }
NVN_NOPROTO(nvnDeviceSetCheckpointCallStacksEnable, void, NVNdevice *d, uint32_t e) { (void)d; (void)e; }
NVN_NOPROTO(nvnDeviceSetCheckpointMaxCallStacksDepth, void, NVNdevice *d, int v) { (void)d; (void)v; }

/* ---------------------------------------------------------------------------------------------------------------
 * Queue
 * --------------------------------------------------------------------------------------------------------------- */
#define QUEUE_MAGIC MAGIC('Q', 'U', 'E', '0')
#define QB(b) ((queue_builder_t *)(b))
#define CQB(b) ((const queue_builder_t *)(b))
NVN(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { QB(b)->dev = d; }
NVN(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    NVNdevice *d = QB(b)->dev;
    memset(b, 0, sizeof(queue_builder_t));
    QB(b)->dev = d;
    QB(b)->flush = 0x8000;
    QB(b)->priority = 1;
    QB(b)->cmd_size = 0x40000; QB(b)->comp_size = 0x40000; QB(b)->ctl_size = 0x1000;
}
NVN(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { QB(b)->flags = (uint32_t)f; }
NVN(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->cmd_size = n; }
NVN(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->comp_size = n; }
NVN(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->ctl_size = n; }
NVN(nvnQueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *b) { return (size_t)(CQB(b)->cmd_size + CQB(b)->comp_size + CQB(b)->ctl_size); }
NVN(nvnQueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *m, size_t n) { QB(b)->mem = m; QB(b)->mem_size = n; }
NVN(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { QB(b)->flush = (uint32_t)n; }
NVN_NOPROTO(nvnQueueBuilderSetQueuePriority, void, NVNqueueBuilder *b, int p) { QB(b)->priority = (uint32_t)p; }
NVN_NOPROTO(nvnQueueBuilderGetQueuePriority, int, const NVNqueueBuilder *b) { return (int)CQB(b)->priority; }
NVN_NOPROTO(nvnQueueBuilderGetDevice, const NVNdevice *, const NVNqueueBuilder *b) { return CQB(b)->dev; }
NVN_NOPROTO(nvnQueueBuilderGetFlags, int, const NVNqueueBuilder *b) { return (int)CQB(b)->flags; }
NVN_NOPROTO(nvnQueueBuilderGetCommandMemorySize, size_t, const NVNqueueBuilder *b) { return (size_t)CQB(b)->cmd_size; }
NVN_NOPROTO(nvnQueueBuilderGetComputeMemorySize, size_t, const NVNqueueBuilder *b) { return (size_t)CQB(b)->comp_size; }
NVN_NOPROTO(nvnQueueBuilderGetControlMemorySize, size_t, const NVNqueueBuilder *b) { return (size_t)CQB(b)->ctl_size; }
NVN_NOPROTO(nvnQueueBuilderGetCommandFlushThreshold, size_t, const NVNqueueBuilder *b) { return (size_t)CQB(b)->flush; }
NVN_NOPROTO(nvnQueueBuilderGetMemorySize, size_t, const NVNqueueBuilder *b) { return (size_t)CQB(b)->mem_size; }
NVN_NOPROTO(nvnQueueBuilderGetMemory, void *, const NVNqueueBuilder *b) { return CQB(b)->mem; }

NVN(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    queue_t *o = (queue_t *)q;
    memset(o, 0, sizeof *o);
    o->magic = QUEUE_MAGIC;
    o->dev = CQB(b)->dev;
    return 1;
}
NVN(nvnQueueFinalize, void, NVNqueue *q) {
    queue_t *o = (queue_t *)q;
    if (o->magic == QUEUE_MAGIC) { GFX_BLOCKING(); gfx_mtl_finish(); }
    o->magic = 0;
}
NVN(nvnQueueSetDebugLabel, void, NVNqueue *q, const char *s) { (void)q; (void)s; }
NVN(nvnQueueGetError, NVNqueueGetErrorResult, NVNqueue *q, NVNqueueErrorInfo *i) { (void)q; (void)i; return NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR; }
NVN(nvnQueueGetTotalCommandMemoryUsed, size_t, NVNqueue *q) { (void)q; return 0; }
NVN(nvnQueueGetTotalControlMemoryUsed, size_t, NVNqueue *q) { (void)q; return 0; }
NVN(nvnQueueGetTotalComputeMemoryUsed, size_t, NVNqueue *q) { (void)q; return 0; }
NVN(nvnQueueResetMemoryUsageCounts, void, NVNqueue *q) { (void)q; }

NVN(nvnQueueSubmitCommands, void, NVNqueue *q, int n, const NVNcommandHandle *h) {
    queue_t *o = (queue_t *)q;
    if (n <= 0 || !h) return;
    cmdrec *recs[64];
    cmdrec **list = n <= 64 ? recs : malloc((size_t)n * sizeof *list);
    if (!list) return;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        cmdrec *r = cmdrec_from_handle(h[i]);
        if (!r) {
            static _Atomic uint64_t n_stale;
            uint64_t k = atomic_fetch_add(&n_stale, 1);
            if (k < 20 || (k & (k - 1)) == 0) LOGW("QueueSubmitCommands: stale or invalid command handle %#llx ignored (%llu so far)", (unsigned long long)h[i], (unsigned long long)k + 1);
            continue;
        }
        cmdrec_retain(r);
        atomic_store_explicit(&r->last_use, cmdrec_now(), memory_order_relaxed);
        list[cnt++] = r;
    }
    if (cnt) {
        uint64_t seq = gfx_mtl_submit(list, (uint32_t)cnt);
        atomic_store(&o->last_seq, seq);
        o->submits++;
        atomic_fetch_add(&g_gfx.submits, 1);
    }
    if (list != recs) free(list);
}
NVN(nvnQueueFlush, void, NVNqueue *q) { (void)q; gfx_mtl_flush(); }
NVN(nvnQueueFinish, void, NVNqueue *q) { (void)q; GFX_BLOCKING(); gfx_mtl_finish(); }

/* ---------------------------------------------------------------------------------------------------------------
 * Sync
 * --------------------------------------------------------------------------------------------------------------- */
#define SYNC_MAGIC MAGIC('S', 'Y', 'N', 'C')
NVN(nvnSyncInitialize, NVNboolean, NVNsync *s, NVNdevice *d) {
    (void)d;
    sync_t *o = (sync_t *)s;
    memset(o, 0, sizeof *o);
    o->magic = SYNC_MAGIC;
    return 1;
}
NVN(nvnSyncFinalize, void, NVNsync *s) { ((sync_t *)s)->magic = 0; }
NVN(nvnSyncSetDebugLabel, void, NVNsync *s, const char *l) { (void)s; (void)l; }
NVN(nvnQueueFenceSync, void, NVNqueue *q, NVNsync *s, NVNsyncCondition c, int flags) {
    (void)q; (void)c;
    sync_t *o = (sync_t *)s;
    o->kind = 0;
    o->cell = NULL;
    o->seq = gfx_timeline_last();
    if (flags & 1) gfx_mtl_flush();     /* NVN_SYNC_FLAGS_FLUSH_FOR_CPU_BIT */
    gfx_mtl_flush();
}
static int sync_ready(const sync_t *o) { return o->kind ? atomic_load(o->cell) >= o->seq : gfx_completed() >= o->seq; }
NVN(nvnSyncWait, NVNsyncWaitResult, const NVNsync *s, uint64_t timeout_ns) {
    const sync_t *o = (const sync_t *)s;
    if (sync_ready(o)) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    GFX_BLOCKING();
    gfx_mtl_flush();
    uint64_t start = gfx_now_ns();
    for (;;) {                                /* a fence recorded in a command list gets its sequence when the replay reaches it */
        if (sync_ready(o)) { if (g_prof_on) atomic_fetch_add(&g_prof[0], gfx_now_ns() - start); return NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED; }
        uint64_t seq = o->seq;
        int ok = o->kind ? gfx_wait_cell(o->cell, seq, 2000000) : (seq == UINT64_MAX ? 0 : gfx_wait_seq(seq, 2000000));
        if (ok) { if (g_prof_on) atomic_fetch_add(&g_prof[0], gfx_now_ns() - start); return NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED; }
        if (timeout_ns != UINT64_MAX && gfx_now_ns() - start >= timeout_ns) return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
        if (seq == UINT64_MAX) usleep(200);
    }
}
NVN(nvnQueueWaitSync, NVNboolean, NVNqueue *q, const NVNsync *s) {
    (void)q;
    const sync_t *o = (const sync_t *)s;
    if (sync_ready(o)) return 1;
    gfx_mtl_queue_wait(o->kind, o->seq, o->cell);
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Events: a 32-bit value in pool memory that the GPU writes after previous work completed.
 * --------------------------------------------------------------------------------------------------------------- */
#define EVENT_MAGIC MAGIC('E', 'V', 'N', 'T')
NVN(nvnEventBuilderSetDefaults, void, NVNeventBuilder *b) { memset(b, 0, sizeof(ev_builder_t)); }
NVN(nvnEventBuilderSetStorage, void, NVNeventBuilder *b, const NVNmemoryPool *p, int64_t off) { ((ev_builder_t *)b)->pool = p; ((ev_builder_t *)b)->off = off; }
NVN_NOPROTO(nvnEventBuilderGetStorage, void, const NVNeventBuilder *b, const NVNmemoryPool **p, int64_t *off) { *p = ((const ev_builder_t *)b)->pool; *off = ((const ev_builder_t *)b)->off; }
NVN_NOPROTO(nvnEventBuilderGetMemoryPool, const NVNmemoryPool *, const NVNeventBuilder *b) { return ((const ev_builder_t *)b)->pool; }
NVN_NOPROTO(nvnEventBuilderGetMemoryOffset, int64_t, const NVNeventBuilder *b) { return ((const ev_builder_t *)b)->off; }
NVN(nvnEventInitialize, NVNboolean, NVNevent *e, const NVNeventBuilder *b) {
    const ev_builder_t *eb = (const ev_builder_t *)b;
    const pool_t *p = (const pool_t *)eb->pool;
    if (!p || p->magic != MAGIC('P', 'O', 'O', 'L') || eb->off < 0 || (uint64_t)eb->off + 4 > p->size) return 0;
    ev_t *o = (ev_t *)e;
    o->magic = EVENT_MAGIC;
    o->pool = p;
    o->off = eb->off;
    o->val = (volatile uint32_t *)(p->mem + eb->off);
    return 1;
}
NVN(nvnEventFinalize, void, NVNevent *e) { ((ev_t *)e)->magic = 0; }
NVN(nvnEventGetValue, uint32_t, const NVNevent *e) { return *((const ev_t *)e)->val; }
NVN(nvnEventSignal, void, NVNevent *e, NVNeventSignalMode m, uint32_t v) {
    volatile uint32_t *p = ((ev_t *)e)->val;
    if (m == NVN_EVENT_SIGNAL_MODE_ADD) __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); else __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
    gfx_wake();
}
NVN_NOPROTO(nvnEventGetMemoryPool, const NVNmemoryPool *, const NVNevent *e) { return (const NVNmemoryPool *)((const ev_t *)e)->pool; }
NVN_NOPROTO(nvnEventGetMemoryOffset, int64_t, const NVNevent *e) { return ((const ev_t *)e)->off; }
