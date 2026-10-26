/* mtl_core.m - Metal device, memory pool registry, scratch ring, the replay thread, presentation and screenshots. */
#include <TargetConditionals.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#import "mtl_priv.h"
#import "mtl_display.h"

gfx_mtl g_mtl;
_Atomic uint64_t g_frame_gpu_us;
_Atomic uint64_t g_prof[8];
int g_prof_on;

/* ---------------------------------------------------------------------------------------------------------------
 * Memory pool registry. Pool memory is wrapped without copying; copy-on-write snapshots keep lookups lock-free.
 * --------------------------------------------------------------------------------------------------------------- */
@interface PoolRec : NSObject { @public uintptr_t base, end; id<MTLBuffer> buf; NSArray<PoolRec *> *kids; } @end
@implementation PoolRec @end

static NSArray<PoolRec *> *g_pools;                 /* sorted by base; replaced wholesale */
static _Atomic uint64_t g_pool_epoch;               /* bumped whenever the set of pools changes */
uint64_t mtl_pool_epoch(void) { return atomic_load(&g_pool_epoch); }
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
/* The snapshot is copied under the mutex: a bare read of the strong global races with the writer's assignment (ARC releases the old array between the load and
 * the retain: Animal Crossing crashed in mtl_addr with a freed array while it created pools on the game thread). The array itself is never mutated after publication. */
static NSArray<PoolRec *> *pools_snapshot(void) { pthread_mutex_lock(&g_pool_mu); NSArray<PoolRec *> *p = g_pools; pthread_mutex_unlock(&g_pool_mu); return p; }
void mtl_pool_each(void (^fn)(id<MTLBuffer> buf)) { NSArray<PoolRec *> *p = pools_snapshot(); for (PoolRec *r in p) fn(r->buf); }

static void pool_insert(PoolRec *r) {      /* g_pool_mu held */
    NSMutableArray *n = [NSMutableArray arrayWithArray:g_pools ?: @[]];
    NSUInteger i = 0;
    while (i < n.count && ((PoolRec *)n[i])->base < r->base) i++;
    [n insertObject:r atIndex:i];
    g_pools = n;
}

/* A pool larger than the device's maxBufferLength (256 MiB on the iOS Simulator and older iPhones) is wrapped by overlapping chunks: every GPU access of up to
 * POOL_OVERLAP bytes lies inside one chunk, and mtl_addr() already scans neighbouring records. The returned head record only owns the chunk list. */
#define POOL_OVERLAP (32u << 20)

void *gfx_mtl_pool_add(uint8_t *mem, uint64_t size, uint32_t flags) {
    (void)flags;
    @autoreleasepool {
        uintptr_t pg = (uintptr_t)getpagesize();
        uintptr_t b = (uintptr_t)mem & ~(pg - 1), e = ((uintptr_t)mem + size + pg - 1) & ~(pg - 1);
        uintptr_t maxl = (uintptr_t)g_mtl.dev.maxBufferLength & ~(pg - 1);
        NSMutableArray<PoolRec *> *kids = [NSMutableArray array];
        uintptr_t step = maxl > POOL_OVERLAP * 2 ? maxl - POOL_OVERLAP : 0;
        if (e - b > maxl && !step) { LOGE("memory pool of %llu bytes exceeds the Metal buffer limit", (unsigned long long)size); return NULL; }
        for (uintptr_t c = b; c < e; c += step ? step : e - b) {
            uintptr_t ce = e - c > maxl ? c + maxl : e;
            id<MTLBuffer> buf = [g_mtl.dev newBufferWithBytesNoCopy:(void *)c length:ce - c options:MTLResourceStorageModeShared deallocator:nil];
            if (!buf) { LOGE("newBufferWithBytesNoCopy failed for pool %p+%#llx", mem, (unsigned long long)size); return NULL; }
            PoolRec *k = [PoolRec new];
            k->base = c; k->end = ce; k->buf = buf;
            [kids addObject:k];
            if (ce == e) break;
        }
        PoolRec *r = [PoolRec new];
        r->base = b; r->end = e; r->kids = kids;
        if (kids.count == 1) { r->buf = kids[0]->buf; }
        pthread_mutex_lock(&g_pool_mu);
        for (PoolRec *k in kids) pool_insert(k);
        atomic_fetch_add(&g_pool_epoch, 1);
        pthread_mutex_unlock(&g_pool_mu);
        if (kids.count > 1) LOGI("memory pool of %llu bytes wrapped as %lu overlapping Metal buffers (maxBufferLength %llu)", (unsigned long long)size, (unsigned long)kids.count, (unsigned long long)g_mtl.dev.maxBufferLength);
        return (void *)CFBridgingRetain(r);
    }
}

void gfx_mtl_pool_remove(void *rec) {
    if (!rec) return;
    PoolRec *r = (__bridge PoolRec *)rec;
    mtl_tex_forget_range((const void *)r->base, r->end - r->base);
    pthread_mutex_lock(&g_pool_mu);
    NSMutableArray *n = [NSMutableArray arrayWithArray:g_pools ?: @[]];
    [n removeObjectsInArray:r->kids];
    g_pools = n;
    atomic_fetch_add(&g_pool_epoch, 1);
    pthread_mutex_unlock(&g_pool_mu);
    /* the GPU may still read the buffers: drop our reference when everything submitted so far completed */
    gfx_retire((void (*)(void *))CFRelease, rec);
}

int mtl_addr(uint64_t addr, uint64_t len, id<MTLBuffer> __autoreleasing *buf, uint64_t *off) {
    NSArray<PoolRec *> *p = pools_snapshot();
    NSUInteger lo = 0, hi = p.count;
    while (lo < hi) { NSUInteger m = (lo + hi) / 2; if (((PoolRec *)p[m])->base <= addr) lo = m + 1; else hi = m; }
    for (NSUInteger i = lo, n = 0; i > 0 && n < 64; i--, n++) {
        PoolRec *r = p[i - 1];
        if (addr >= r->base && addr + len <= r->end) { *buf = r->buf; *off = addr - r->base; return 1; }
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Scratch ring
 * --------------------------------------------------------------------------------------------------------------- */
@implementation GfxRingChunk @end
static NSMutableArray<GfxRingChunk *> *g_free_chunks;
static pthread_mutex_t g_ring_mu = PTHREAD_MUTEX_INITIALIZER;
#define RING_CHUNK (4u << 20)

void ring_begin(ring *r) { r->chunks = (__bridge NSMutableArray *)CFBridgingRetain([NSMutableArray array]); r->cur = 0; }

void *ring_alloc(ring *r, uint32_t size, uint32_t align, id<MTLBuffer> __autoreleasing *buf, uint32_t *off) {
    NSMutableArray<GfxRingChunk *> *cs = r->chunks;
    for (;;) {
        if (r->cur < cs.count) {
            GfxRingChunk *c = cs[r->cur];
            uint32_t o = (c->used + align - 1) & ~(align - 1);
            if ((uint64_t)o + size <= c->size) { c->used = o + size; *buf = c->buf; *off = o; return c->cpu + o; }
            r->cur++;
            continue;
        }
        uint32_t want = size + align > RING_CHUNK ? (size + align + 0xffff) & ~0xffffu : RING_CHUNK;
        GfxRingChunk *c = nil;
        pthread_mutex_lock(&g_ring_mu);
        for (NSUInteger i = 0; i < g_free_chunks.count; i++) if (((GfxRingChunk *)g_free_chunks[i])->size >= want) { c = g_free_chunks[i]; [g_free_chunks removeObjectAtIndex:i]; break; }
        pthread_mutex_unlock(&g_ring_mu);
        if (!c) {
            c = [GfxRingChunk new];
            c->buf = [g_mtl.dev newBufferWithLength:want options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
            if (!c->buf) gfx_die("scratch buffer allocation of %u bytes failed", want);
            c->cpu = c->buf.contents; c->size = want;
        }
        c->used = 0;
        [cs addObject:c];
        r->cur = (uint32_t)cs.count - 1;
    }
}

void ring_end(ring *r) {
    pthread_mutex_lock(&g_ring_mu);
    if (!g_free_chunks) g_free_chunks = [NSMutableArray array];
    for (GfxRingChunk *c in r->chunks) if (g_free_chunks.count < 16) [g_free_chunks addObject:c];
    pthread_mutex_unlock(&g_ring_mu);
    CFRelease((__bridge CFTypeRef)r->chunks);
    r->chunks = nil;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Replay thread: executes accepted work in order.
 * --------------------------------------------------------------------------------------------------------------- */
enum { JOB_SUBMIT = 1, JOB_PRESENT, JOB_WAIT, JOB_STOP };
typedef struct job {
    int kind; uint64_t seq;
    cmdrec **recs; uint32_t n;
    win_t *win; int idx; texsnap snap; uint32_t interval;
    uint32_t wkind; uint64_t wseq; _Atomic uint64_t *wcell; uint64_t tpush;
    struct job *next;
} job;
static job *g_jq_head, *g_jq_tail;
static pthread_mutex_t g_jq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_jq_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static int g_thread_started;

/* The sequence number is taken under the queue lock so jobs run in sequence order: completion is "everything up to seq", so a job
 * pushed ahead of a lower sequence number (two threads submitting at once) would mark the other one complete before it ran. */
static uint64_t jq_push(job *j) {
    j->tpush = g_prof_on ? gfx_now_ns() : 0;
    pthread_mutex_lock(&g_jq_mu);
    if (j->kind != JOB_STOP) j->seq = gfx_timeline_alloc();
    if (g_jq_tail) g_jq_tail->next = j; else g_jq_head = j;
    g_jq_tail = j;
    pthread_cond_signal(&g_jq_cv);
    pthread_mutex_unlock(&g_jq_mu);
    return j->seq;
}

static void prof_report(void) {
    static uint64_t last[8], last_t; uint64_t n = atomic_load(&g_prof[6]);
    if (n % 120) return;
    uint64_t now = gfx_now_ns(), cur[8]; for (int i = 0; i < 8; i++) cur[i] = atomic_load(&g_prof[i]);
    double subs = (double)(cur[5] - last[5]); if (subs < 1) subs = 1;
    LOGI("prof: %.1f presents/s | per submit: replay cpu %.2f ms, gpu %.2f ms, queue latency %.2f ms | per present: guest blocked in SyncWait %.2f ms, pacing sleep %.2f ms",
         120.0 * 1e9 / (double)(now - last_t), (double)(cur[1] - last[1]) / subs / 1e6, (double)(cur[2] - last[2]) / subs / 1e6, (double)(cur[4] - last[4]) / (subs + 120) / 1e6,
         (double)(cur[0] - last[0]) / 120 / 1e6, (double)(cur[3] - last[3]) / 120 / 1e6);
    memcpy(last, cur, sizeof last); last_t = now;
}

static void *replay_thread(void *arg) {
    (void)arg;
    pthread_setname_np("napple-gfx-replay");
    for (;;) {
        pthread_mutex_lock(&g_jq_mu);
        while (!g_jq_head) pthread_cond_wait(&g_jq_cv, &g_jq_mu);
        job *j = g_jq_head;
        g_jq_head = j->next;
        if (!g_jq_head) g_jq_tail = NULL;
        pthread_mutex_unlock(&g_jq_mu);
        if (j->kind == JOB_STOP) { free(j); return NULL; }
        uint64_t tj0 = g_prof_on ? gfx_now_ns() : 0;
        if (tj0) atomic_fetch_add(&g_prof[4], tj0 - j->tpush);
        @autoreleasepool {
            switch (j->kind) {
            case JOB_SUBMIT:
                replay_run_submission(j->recs, j->n, j->seq);
                if (tj0) { atomic_fetch_add(&g_prof[1], gfx_now_ns() - tj0); atomic_fetch_add(&g_prof[5], 1); }
                for (uint32_t i = 0; i < j->n; i++) cmdrec_release(j->recs[i]);
                free(j->recs);
                break;
            case JOB_PRESENT:
                replay_run_present(j->win, j->idx, &j->snap, j->interval, j->seq);
                if (tj0) { atomic_fetch_add(&g_prof[6], 1); prof_report(); }
                break;
            case JOB_WAIT:
                if (j->wkind) gfx_wait_cell(j->wcell, j->wseq, UINT64_MAX); else gfx_wait_seq(j->wseq, UINT64_MAX);
                gfx_timeline_complete(j->seq);
                break;
            }
        }
        free(j);
    }
}

uint64_t gfx_mtl_submit(cmdrec *const *recs, uint32_t n) {
    job *j = calloc(1, sizeof *j);
    j->kind = JOB_SUBMIT;
    j->recs = malloc(n * sizeof *j->recs);
    memcpy(j->recs, recs, n * sizeof *j->recs);
    j->n = n;
    return jq_push(j);
}
uint64_t gfx_mtl_present(win_t *w, int idx, const texsnap *t, uint32_t interval) {
    job *j = calloc(1, sizeof *j);
    j->kind = JOB_PRESENT; j->win = w; j->idx = idx; j->snap = *t; j->interval = interval;
    return jq_push(j);
}
uint64_t gfx_mtl_queue_wait(uint32_t kind, uint64_t seq, _Atomic uint64_t *cell) {
    job *j = calloc(1, sizeof *j);
    j->kind = JOB_WAIT; j->wkind = kind; j->wseq = seq; j->wcell = cell;
    return jq_push(j);
}
void gfx_mtl_flush(void) {}
void gfx_mtl_finish(void) { gfx_wait_seq(gfx_timeline_last(), UINT64_MAX); }
void gfx_mtl_window_attach(win_t *w) {
    @autoreleasepool {
        { const texsnap *s0 = &((const tex_t *)w->tex[0])->s; mtl_scale_note_window(s0->d.width, s0->d.height); mtl_scale_tick(); }   /* the swapchain size is the game's output size */
        for (int i = 0; i < w->ntex; i++) mtl_tex_get(&((const tex_t *)w->tex[i])->s, 1);
    }
}
void gfx_mtl_tex_invalidate(const texsnap *t) { (void)t; }

/* ---------------------------------------------------------------------------------------------------------------
 * Present: moved to mtl_present.m (replay_run_present, screenshots, output filters, frame generation hook).
 * --------------------------------------------------------------------------------------------------------------- */

uint64_t napple_gfx_vsync_period_ns(void) {
    napple_gfx_settings st;
    gfx_settings_get(&st);
    if (st.pacing == NAPPLE_PACING_FIXED && st.target_fps > 0) return (uint64_t)(1e9 / st.target_fps);
    return 1000000000ull / 60;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------------------------------------------- */
int gfx_mtl_init(void *layer, uint32_t hw, uint32_t hh) {
    @autoreleasepool {
        g_mtl.dev = MTLCreateSystemDefaultDevice();
        if (!g_mtl.dev) return -1;
        g_mtl.queue = [g_mtl.dev newCommandQueue];
        g_mtl.bc_ok = g_mtl.dev.supportsBCTextureCompression;
        if (layer) {
            g_mtl.layer = (__bridge CAMetalLayer *)layer;
            g_mtl.layer.device = g_mtl.dev;
            g_mtl.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            g_mtl.layer.framebufferOnly = YES;
        } else {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:hw height:hh mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModeShared;
            g_mtl.headless = [g_mtl.dev newTextureWithDescriptor:d];
            g_mtl.out_w = hw; g_mtl.out_h = hh;
        }
        if (mtl_present_init()) return -1;
        mtl_shader_init();
        if (!g_thread_started) {
            pthread_attr_t a; pthread_attr_init(&a);
            pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
            pthread_create(&g_thread, &a, replay_thread, NULL);
            g_thread_started = 1;
        }
        LOGI("Metal device: %s (BC textures %s)", g_mtl.dev.name.UTF8String, g_mtl.bc_ok ? "native" : "decoded on the CPU");
        return 0;
    }
}

void gfx_mtl_shutdown(void) {
    if (!g_thread_started) return;
    gfx_mtl_finish();
    mtl_shader_shutdown();
    job *j = calloc(1, sizeof *j);
    j->kind = JOB_STOP;
    jq_push(j);
    pthread_join(g_thread, NULL);
    g_thread_started = 0;
}

void gfx_mtl_settings_changed(void) {
    napple_gfx_settings st;
    gfx_settings_get(&st);
#if TARGET_OS_OSX
    if (g_mtl.layer) g_mtl.layer.displaySyncEnabled = st.pacing != NAPPLE_PACING_UNCAPPED;   /* iOS presents on the display link only (no such property) */
#else
    (void)st;
#endif
}
