/* mtl_shader.m - shader translation (Maxwell -> MSL), Metal compilation, pipeline creation, disk cache. Compilation runs on background queues.
 * A draw whose pipeline is still compiling WAITS for it (a skipped draw is a visible pop or a missing object; a few milliseconds of hitch on first use is
 * not). To make that wait rare, every pipeline key the game ever used is persisted (<cache>/pipelines.bin) and rebuilt in the background as soon as the
 * programs it names are registered ("prewarm"). NAPPLE_GFX_PIPE_SKIP=1 restores the old behaviour (skip while compiling) for comparison.
 * Compiled pipelines go into a Metal binary archive (<cache>/pipelines-v<N>.metalar) so later launches load GPU code instead of compiling it; the
 * translated MSL stays on disk as readable text (<cache>/shaders/<hash>.msl, NAPPLE_SAVE_MSL=1 adds `// pc` annotations). NAPPLE_GFX_NO_ARCHIVE=1 disables
 * the archive for comparison. */
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#import "mtl_priv.h"

#define TRANSLATOR_VERSION 6u          /* bump when generated MSL changes: invalidates the disk cache */

@implementation GfxShader @end
@implementation GfxPipe @end

/* Whoever claims a shader variant or pipeline first builds it. A draw that needs one now builds it on its own (user-interactive) thread
 * instead of waiting for a queue, so no wait sits behind background work and nothing can deadlock on a full worker pool: a waiter
 * either builds or waits for a build that is running. Background work (pipeline prewarm, async variants) runs at the performance cores'
 * QoS but on fewer queues than there are performance cores, so the game's own threads always have cores left. The launch-time cache
 * compile runs before the game starts and may use every core. */
#define MAX_COMPILE_Q 8
static dispatch_queue_t g_compile_q[MAX_COMPILE_Q], g_pipe_q, g_prewarm_q, g_archive_q;
static unsigned g_ncompile_q;
static _Atomic unsigned g_compile_rr;
static void compile_async(dispatch_block_t b) { dispatch_async(g_compile_q[atomic_fetch_add(&g_compile_rr, 1) % g_ncompile_q], b); }
static unsigned perf_cores(void) {
    int n = 0; size_t sz = sizeof n;
    if (sysctlbyname("hw.perflevel0.physicalcpu", &n, &sz, NULL, 0) || n <= 0) { sz = sizeof n; if (sysctlbyname("hw.physicalcpu", &n, &sz, NULL, 0) || n <= 0) n = 4; n = (n + 1) / 2; }
    return (unsigned)n;
}

/* Metal binary archives. g_archive is the file loaded at init: every build looks it up first, and it never changes during the run (so builds on
 * any thread can read it). Every pipeline the run builds, from the archive or not, is also added on g_archive_q to g_out, a fresh archive
 * that replaces the file (at most every ARCHIVE_SAVE_S seconds and at shutdown), so the file holds what the game uses.
 * Metal sometimes drops a fragment stage it finds writes nothing, and the archiver then refuses to save the whole archive ("expecting
 * 'fragment' stage in pipeline no. N"; each such pipeline saves fine alone). On that error pipeline N (g_out's add order) is left out and
 * g_out is rebuilt from the rest. A stale file (other OS / GPU) fails to load and is recreated. */
#define ARCHIVE_SAVE_S 10
#define ARCHIVE_SAVE_TRIES 10
static id<MTLBinaryArchive> g_archive;
static NSURL *g_archive_url;
static id<MTLBinaryArchive> g_out;                         /* g_archive_q only, like everything below */
static NSMutableArray *g_out_desc;                         /* descriptors in g_out, in add order */
static int g_archive_dirty, g_archive_save_queued, g_archive_broken;
static unsigned g_archive_dropped;
static void archive_open(void) {
#if TARGET_OS_SIMULATOR
    LOGI("binary archive: disabled on the iOS Simulator (its GPU has no target architecture; saving asserts)");
    return;
#endif
    if (!g_gfx.cfg.cache_dir || getenv("NAPPLE_GFX_NO_ARCHIVE")) return;
    g_archive_url = [NSURL fileURLWithPath:[NSString stringWithFormat:@"%s/pipelines-v%u.metalar", g_gfx.cfg.cache_dir, TRANSLATOR_VERSION]];
    MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new];
    NSError *err = nil;
    if ([NSFileManager.defaultManager fileExistsAtPath:g_archive_url.path]) {
        d.url = g_archive_url;
        g_archive = [g_mtl.dev newBinaryArchiveWithDescriptor:d error:&err];
        if (!g_archive) { LOGW("binary archive %s not usable (%s): rebuilding it", g_archive_url.path.UTF8String, err.localizedDescription.UTF8String); [NSFileManager.defaultManager removeItemAtURL:g_archive_url error:nil]; }
    }
    d.url = nil; err = nil;
    if (!g_archive) g_archive = [g_mtl.dev newBinaryArchiveWithDescriptor:d error:&err];
    g_out = [g_mtl.dev newBinaryArchiveWithDescriptor:d error:&err];
    if (!g_archive || !g_out) { LOGW("binary archive unavailable: %s", err.localizedDescription.UTF8String); g_archive = nil; return; }
    g_out_desc = [NSMutableArray array];
    g_archive_q = dispatch_queue_create("napple.gfx.archive", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
}
static BOOL out_add(id<MTLBinaryArchive> a, id desc, NSError **err) {
    return [desc isKindOfClass:MTLRenderPipelineDescriptor.class] ? [a addRenderPipelineFunctionsWithDescriptor:desc error:err] : [a addComputePipelineFunctionsWithDescriptor:desc error:err];
}
static void archive_save(void) {
    if (!g_archive_dirty || g_archive_broken) return;
    for (int tries = 0; tries < ARCHIVE_SAVE_TRIES; tries++) {
        NSError *err = nil;
        if ([g_out serializeToURL:g_archive_url error:&err]) { g_archive_dirty = 0; return; }
        NSString *m = err.localizedDescription;
        NSRange r = [m rangeOfString:@"pipeline no. '"];
        NSUInteger n = r.location == NSNotFound ? NSNotFound : (NSUInteger)[m substringFromIndex:NSMaxRange(r)].integerValue;
        if (n == NSNotFound || n >= g_out_desc.count) { LOGW("binary archive save failed, archive disabled for this run: %s", m.UTF8String); g_archive_broken = 1; return; }
        [g_out_desc removeObjectAtIndex:n]; g_archive_dropped++;
        MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new];
        g_out = [g_mtl.dev newBinaryArchiveWithDescriptor:d error:&err];
        for (id desc in g_out_desc) if (!g_out || !out_add(g_out, desc, &err)) { LOGW("binary archive rebuild failed: %s", err.localizedDescription.UTF8String); g_archive_broken = 1; return; }
    }
    LOGW("binary archive save failed %d times, archive disabled for this run", ARCHIVE_SAVE_TRIES); g_archive_broken = 1;
}
static void archive_add(id desc) {        /* every pipeline the run builds (render or compute descriptor) */
    if (!g_archive) return;
    dispatch_async(g_archive_q, ^{
        if (g_archive_broken) return;
        NSError *err = nil;
        if (!out_add(g_out, desc, &err)) { LOGW("binary archive add failed: %s", err.localizedDescription.UTF8String); return; }
        [g_out_desc addObject:desc];
        g_archive_dirty = 1;
        if (!g_archive_save_queued) {
            g_archive_save_queued = 1;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ARCHIVE_SAVE_S * NSEC_PER_SEC), g_archive_q, ^{ g_archive_save_queued = 0; archive_save(); });
        }
    });
}
static void shader_progress_summary(void);
void mtl_shader_shutdown(void) {
    shader_progress_summary();
    if (!g_archive) return;
    dispatch_sync(g_archive_q, ^{
        archive_save();
        LOGI("binary archive: %llu of %lu pipelines loaded precompiled this run, %u left out (fragment stage dropped by Metal)%s",
             (unsigned long long)atomic_load(&g_gfx.archive_hits), (unsigned long)g_out_desc.count + g_archive_dropped, g_archive_dropped, g_archive_broken ? ", not saved" : "");
    });
}

/* Functions compiled by the launch-time prewarm, by cache file path: build_shader takes them instead of compiling the MSL again. */
static NSMutableDictionary<NSString *, id<MTLFunction>> *g_prewarmed;
static pthread_mutex_t g_prewarm_mu = PTHREAD_MUTEX_INITIALIZER;
static MTLCompileOptions *compile_options(void) {
    MTLCompileOptions *o = [MTLCompileOptions new];
    o.languageVersion = MTLLanguageVersion3_0;
    o.preserveInvariance = YES;                 /* depth pre-pass + EQUAL test needs identical positions */
    o.fastMathEnabled = NO;                     /* Maxwell NaN / inf semantics */
    return o;
}
static NSMutableDictionary<NSNumber *, GfxShader *> *g_shaders;
static NSMutableDictionary<NSNumber *, GfxPipe *> *g_pipes;
static pthread_mutex_t g_sh_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSNumber *, NSMutableArray<NSData *> *> *g_stored;      /* persisted pipeline keys by program hash */
static NSMutableSet<NSNumber *> *g_stored_hashes;                                  /* hashes of every persisted key (dedupe) */
static NSString *g_pipe_file;
static void build_pipe(GfxPipe *p, pipe_key k);
typedef struct { uint32_t magic, version, key_size, pad; } pipe_hdr;
#define PIPE_MAGIC 0x50495045u

/* Build timing and progress (log only; NAPPLE_GFX_SHADER_LOG=0 silences the periodic line). Every shader variant requested is "queued"; each is "built" when
 * translate (or the disk cache) and the Metal compile are done. A single step slower than SLOW_STEP_MS is logged on its own. */
#define SLOW_STEP_MS 1500
static _Atomic uint64_t g_st_queued, g_st_built, g_st_failed, g_st_disk, g_st_tr_n, g_st_tr_ns, g_st_tr_max, g_st_cmp_n, g_st_cmp_ns, g_st_cmp_max;
static _Atomic uint64_t g_st_pipe_n, g_st_pipe_ns, g_st_pipe_max, g_st_first_ns, g_st_last_ns;
static void st_max(_Atomic uint64_t *m, uint64_t v) { uint64_t o = atomic_load(m); while (v > o && !atomic_compare_exchange_weak(m, &o, v)) {} }
static void st_note_first(void) { uint64_t z = 0; atomic_compare_exchange_strong(&g_st_first_ns, &z, gfx_now_ns()); }
static void shader_progress_line(const char *tag, uint64_t last_built) {
    uint64_t q = atomic_load(&g_st_queued), b = atomic_load(&g_st_built), f = atomic_load(&g_st_failed), first = atomic_load(&g_st_first_ns);
    uint64_t tn = atomic_load(&g_st_tr_n), cn = atomic_load(&g_st_cmp_n), pn = atomic_load(&g_st_pipe_n);
    uint64_t endt = atomic_load(&g_st_last_ns); if (!endt || endt < first || q > b) endt = gfx_now_ns();
    double el = first ? (double)(endt - first) / 1e9 : 0, rate = el > 0.5 ? (double)b / el : 0;
    char eta[48] = "";
    if (q > b && rate > 0) snprintf(eta, sizeof eta, ", ETA ~%.0f s", (double)(q - b) / rate);
    uint32_t pw = atomic_load(&g_gfx.prewarm_total), pd = atomic_load(&g_gfx.prewarm_done);
    char pre[64] = "";
    if (pw && pd < pw) snprintf(pre, sizeof pre, "; launch prewarm %u of %u", pd, pw);
    LOGI("%s: shaders built %llu of %llu queued (%llu failed, %llu from disk cache)%s%s | translate %llu avg %.1f ms max %.0f ms | metal compile %llu avg %.1f ms max %.0f ms | pipelines %llu avg %.1f ms max %.0f ms | %.1f s%s",
         tag, (unsigned long long)b, (unsigned long long)q, (unsigned long long)f, (unsigned long long)atomic_load(&g_st_disk), pre, "",
         (unsigned long long)tn, tn ? atomic_load(&g_st_tr_ns) / 1e6 / (double)tn : 0, atomic_load(&g_st_tr_max) / 1e6,
         (unsigned long long)cn, cn ? atomic_load(&g_st_cmp_ns) / 1e6 / (double)cn : 0, atomic_load(&g_st_cmp_max) / 1e6,
         (unsigned long long)pn, pn ? atomic_load(&g_st_pipe_ns) / 1e6 / (double)pn : 0, atomic_load(&g_st_pipe_max) / 1e6, el, eta);
    (void)last_built;
}
static void shader_progress_summary(void) { if (atomic_load(&g_st_queued) || atomic_load(&g_st_cmp_n)) shader_progress_line("shader summary", 0); }
static void shader_progress_start(void) {
    const char *e = getenv("NAPPLE_GFX_SHADER_LOG");
    if (e && !strcmp(e, "0")) return;
    dispatch_queue_t q = dispatch_queue_create("napple.gfx.progress", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    static uint64_t last_b, last_q; static uint32_t last_pd;
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, NSEC_PER_SEC / 4);
    dispatch_source_set_event_handler(t, ^{
        uint64_t b = atomic_load(&g_st_built), qd = atomic_load(&g_st_queued); uint32_t pd = atomic_load(&g_gfx.prewarm_done);
        if (b == last_b && qd == last_q && pd == last_pd) return;          /* nothing moved: stay quiet */
        last_b = b; last_q = qd; last_pd = pd;
        shader_progress_line("shader progress", b);
    });
    dispatch_resume(t);
}

static int cache_load(NSString *path, ShaderInfo *info);
void mtl_shader_init(void) {
    unsigned pc = perf_cores();
    g_ncompile_q = pc > 3 ? pc - 2 : 1;
    if (g_ncompile_q > MAX_COMPILE_Q) g_ncompile_q = MAX_COMPILE_Q;
    for (unsigned i = 0; i < g_ncompile_q; i++)
        g_compile_q[i] = dispatch_queue_create("napple.gfx.compile", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    g_pipe_q = dispatch_queue_create("napple.gfx.pipe", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    g_prewarm_q = dispatch_queue_create("napple.gfx.prewarm", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0));
    g_prewarmed = [NSMutableDictionary dictionary];
    archive_open();
    shader_progress_start();
    LOGI("shader compile: %u background queue(s) (%u performance cores); binary archive %s", g_ncompile_q, pc, g_archive ? "on" : "off");
    g_shaders = [NSMutableDictionary dictionary];
    g_pipes = [NSMutableDictionary dictionary];
    g_stored = [NSMutableDictionary dictionary]; g_stored_hashes = [NSMutableSet set];
    if (g_gfx.cfg.cache_dir) {
        g_pipe_file = [NSString stringWithFormat:@"%s/pipelines.bin", g_gfx.cfg.cache_dir];
        NSData *d = [NSData dataWithContentsOfFile:g_pipe_file];
        pipe_hdr h; size_t ks = sizeof(pipe_key);
        if (d.length >= sizeof h) {
            memcpy(&h, d.bytes, sizeof h);
            if (h.magic == PIPE_MAGIC && h.version == TRANSLATOR_VERSION && h.key_size == ks) {
                for (size_t o = sizeof h; o + ks <= d.length; o += ks) {
                    NSData *kd = [d subdataWithRange:NSMakeRange(o, ks)];
                    const pipe_key *k = kd.bytes;
                    [g_stored_hashes addObject:@(fnv1a(k, ks, FNV_INIT))];
                    for (int w = 0; w < 2; w++) { uint64_t hs = w ? k->fs : k->vs; if (!hs) continue; NSMutableArray *a = g_stored[@(hs)]; if (!a) g_stored[@(hs)] = a = [NSMutableArray array]; if (w == 0 || k->fs != k->vs) [a addObject:kd]; }
                }
                LOGI("pipeline prewarm list: %lu pipelines from %s", (unsigned long)g_stored_hashes.count, g_pipe_file.UTF8String);
            } else { [NSFileManager.defaultManager removeItemAtPath:g_pipe_file error:nil]; }
        }
    }
    napple_gfx_settings st; gfx_settings_get(&st);
    if (st.prewarm_shader_cache && g_gfx.cfg.cache_dir) {
        /* Compile every cached shader before the game needs it (the runtime holds the game until prewarm_done == prewarm_total, behind a
         * progress bar); build_shader then takes the compiled function instead of compiling again. */
        NSString *dir = [NSString stringWithFormat:@"%s/shaders", g_gfx.cfg.cache_dir];
        NSArray<NSString *> *files = [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil];
        NSMutableArray<NSString *> *msl = [NSMutableArray array];
        for (NSString *f in files) if ([f hasSuffix:@".msl"]) [msl addObject:[dir stringByAppendingPathComponent:f]];
        atomic_store(&g_gfx.prewarm_total, (uint32_t)msl.count);
        for (NSString *path in msl) dispatch_async(g_prewarm_q, ^{
            @autoreleasepool {
                ShaderInfo info;
                if (cache_load(path, &info)) {
                    st_note_first();
                    uint64_t c0 = gfx_now_ns();
                    id<MTLLibrary> lib = [g_mtl.dev newLibraryWithSource:@(info.msl) options:compile_options() error:nil];
                    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
                    free(info.msl);
                    { uint64_t dt = gfx_now_ns() - c0; atomic_fetch_add(&g_st_cmp_n, 1); atomic_fetch_add(&g_st_cmp_ns, dt); st_max(&g_st_cmp_max, dt);
                      if (dt > SLOW_STEP_MS * 1000000ull) LOGW("slow shader: prewarm compile of %s took %.0f ms", path.lastPathComponent.UTF8String, dt / 1e6); }
                    if (fn) { pthread_mutex_lock(&g_prewarm_mu); g_prewarmed[path] = fn; pthread_mutex_unlock(&g_prewarm_mu); }
                }
            }
            atomic_fetch_add(&g_gfx.prewarm_done, 1);
        });
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * Disk cache of translated MSL + reflection
 * --------------------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t magic, version; uint32_t info_size; uint32_t msl_size; } cache_hdr;
#define CACHE_MAGIC 0x4c534d4eu

static NSString *cache_path(uint64_t blob, uint64_t keyhash) {
    if (!g_gfx.cfg.cache_dir) return nil;
    return [NSString stringWithFormat:@"%s/shaders/%016llx_%016llx.msl", g_gfx.cfg.cache_dir, (unsigned long long)blob, (unsigned long long)keyhash];
}
static void cache_store(NSString *path, const ShaderInfo *info) {
    if (!path || !info->msl) return;
    NSString *dir = path.stringByDeletingLastPathComponent;
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    ShaderInfo copy = *info; copy.msl = NULL;
    cache_hdr h = { CACHE_MAGIC, TRANSLATOR_VERSION, (uint32_t)sizeof copy, (uint32_t)strlen(info->msl) };
    NSMutableData *d = [NSMutableData dataWithBytes:&h length:sizeof h];
    [d appendBytes:&copy length:sizeof copy];
    [d appendBytes:info->msl length:h.msl_size];
    [d writeToFile:[path stringByAppendingString:@".tmp"] atomically:NO];
    rename([path stringByAppendingString:@".tmp"].UTF8String, path.UTF8String);
}
static int cache_load(NSString *path, ShaderInfo *info) {
    if (!path) return 0;
    NSData *d = [NSData dataWithContentsOfFile:path];
    if (d.length < sizeof(cache_hdr)) return 0;
    cache_hdr h; memcpy(&h, d.bytes, sizeof h);
    if (h.magic != CACHE_MAGIC || h.version != TRANSLATOR_VERSION || h.info_size != sizeof *info || d.length != sizeof h + h.info_size + h.msl_size) return 0;
    memcpy(info, (const uint8_t *)d.bytes + sizeof h, sizeof *info);
    info->msl = malloc(h.msl_size + 1);
    memcpy(info->msl, (const uint8_t *)d.bytes + sizeof h + h.info_size, h.msl_size);
    info->msl[h.msl_size] = 0;
    atomic_fetch_add(&g_gfx.shader_disk_hits, 1);
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Translation + compilation of one shader variant
 * --------------------------------------------------------------------------------------------------------------- */
static uint64_t key_hash(const ShaderKey *k) { return fnv1a(k, sizeof *k, FNV_INIT); }

/* Returns the new state (1 ready, 2 failed); the caller publishes it. */
static int build_shader(GfxShader *s, const shader_blob *b, ShaderKey key) {
    /* the generated code bakes the global memory window (base, size): it is part of the cache identity */
    uint64_t gm[2] = { maxwell_gmem_base, maxwell_gmem_size };
    NSString *cp = cache_path(b->hash, fnv1a(gm, sizeof gm, key_hash(&key)));
    st_note_first();
    if (!cache_load(cp, &s->info)) {
        uint64_t t0 = gfx_now_ns();
        ShaderInfo *t = maxwell_translate(b->code, b->code_size, b->stage, &key);
        { uint64_t dt = gfx_now_ns() - t0; atomic_fetch_add(&g_st_tr_n, 1); atomic_fetch_add(&g_st_tr_ns, dt); st_max(&g_st_tr_max, dt);
          if (dt > SLOW_STEP_MS * 1000000ull) LOGW("slow shader: translate of %016llx (stage %d, %zu bytes of Maxwell code) took %.0f ms", (unsigned long long)b->hash, b->stage, (size_t)b->code_size, dt / 1e6); }
        if (!t || !t->msl) {
            { const char *dd = getenv("NAPPLE_SHADER_DUMP"); if (dd) { char fp[1024]; snprintf(fp, sizeof fp, "%s/%016llx_s%d.bin", dd, (unsigned long long)b->hash, b->stage); FILE *f = fopen(fp, "wb"); if (f) { fwrite(b->code, 1, b->code_size, f); fclose(f); } } }
            LOGW("shader %016llx (stage %d) could not be translated: %s", (unsigned long long)b->hash, b->stage, t ? t->error : "out of memory");
            if (t) maxwell_free(t);
            return 2;
        }
        s->info = *t;
        s->info.msl = t->msl;
        t->msl = NULL;
        maxwell_free(t);
        atomic_fetch_add(&g_gfx.shaders_translated, 1);
        cache_store(cp, &s->info);
    } else atomic_fetch_add(&g_st_disk, 1);
    memcpy(s->local, b->local, sizeof s->local);
    s->blob = b;
    if (cp) {                                       /* compiled at launch by the prewarm */
        pthread_mutex_lock(&g_prewarm_mu);
        s->fn = g_prewarmed[cp]; if (s->fn) [g_prewarmed removeObjectForKey:cp];
        pthread_mutex_unlock(&g_prewarm_mu);
        if (s->fn) { free(s->info.msl); s->info.msl = NULL; return 1; }
    }
    @autoreleasepool {
        NSError *err = nil;
        NSString *src = [NSString stringWithUTF8String:s->info.msl];
        uint64_t c0 = gfx_now_ns();
        id<MTLLibrary> lib = [g_mtl.dev newLibraryWithSource:src options:compile_options() error:&err];
        { uint64_t dt = gfx_now_ns() - c0; atomic_fetch_add(&g_st_cmp_n, 1); atomic_fetch_add(&g_st_cmp_ns, dt); st_max(&g_st_cmp_max, dt);
          if (dt > SLOW_STEP_MS * 1000000ull) LOGW("slow shader: Metal compile of %016llx (stage %d, %zu bytes of MSL) took %.0f ms", (unsigned long long)b->hash, b->stage, strlen(s->info.msl), dt / 1e6); }
        free(s->info.msl); s->info.msl = NULL;
        if (!lib) { LOGW("shader %016llx: Metal compile failed: %s", (unsigned long long)b->hash, err.localizedDescription.UTF8String); return 2; }
        s->fn = [lib newFunctionWithName:@"main0"];
    }
    return s->fn ? 1 : 2;
}

/* Builds the variant if nobody has claimed it yet (see the comment at the top); otherwise returns at once. */
static void run_shader(GfxShader *s, const shader_blob *b, ShaderKey key) {
    int unclaimed = 0;
    if (!__atomic_compare_exchange_n(&s->claimed, &unclaimed, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    int st = build_shader(s, b, key);
    if (st == 2) atomic_fetch_add(&g_st_failed, 1);
    atomic_fetch_add(&g_st_built, 1);
    atomic_store(&g_st_last_ns, gfx_now_ns());
    __atomic_store_n(&s->state, st, __ATOMIC_RELEASE);
    atomic_fetch_sub(&g_gfx.shaders_pending, 1);
}

/* Variant lookup; `wait` blocks until built. The blob must be registered. */
static GfxShader *shader_variant(uint64_t hash, const ShaderKey *key, int wait) {
    uint64_t kh = key_hash(key);
    NSNumber *k = @(hash ^ (kh * 0x9e3779b97f4a7c15ull));
    pthread_mutex_lock(&g_sh_mu);
    GfxShader *s = g_shaders[k];
    int create = !s;
    if (create) { s = [GfxShader new]; g_shaders[k] = s; }
    pthread_mutex_unlock(&g_sh_mu);
    const shader_blob *b = NULL;
    if (create) {
        b = shader_blob_find(hash);
        if (!b) { s->claimed = 1; __atomic_store_n(&s->state, 2, __ATOMIC_RELEASE); return s; }
        ShaderKey kc = *key;
        atomic_fetch_add(&g_gfx.shaders_pending, 1);
        atomic_fetch_add(&g_st_queued, 1);
        if (!wait) compile_async(^{ run_shader(s, b, kc); });
    }
    if (wait && __atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == 0) {
        if (!b) b = shader_blob_find(hash);
        if (b) run_shader(s, b, *key);              /* needed now: build it on this thread unless a worker already is */
        while (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == 0) usleep(100);
    }
    return s;
}

GfxShader *mtl_shader_for(uint64_t hash, const ShaderKey *key) { return shader_variant(hash, key, 0); }

/* ---------------------------------------------------------------------------------------------------------------
 * Render pipelines
 * --------------------------------------------------------------------------------------------------------------- */
MTLBlendFactor mtl_blend_factor(unsigned f) {
    switch (f) {
    case 1: return MTLBlendFactorZero; case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor; case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha; case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha; case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor; case 0xA: return MTLBlendFactorOneMinusDestinationColor;
    case 0xB: return MTLBlendFactorSourceAlphaSaturated;
    case 0x10: return MTLBlendFactorSource1Color; case 0x11: return MTLBlendFactorOneMinusSource1Color;
    case 0x12: return MTLBlendFactorSource1Alpha; case 0x13: return MTLBlendFactorOneMinusSource1Alpha;
    case 0x61: return MTLBlendFactorBlendColor; case 0x62: return MTLBlendFactorOneMinusBlendColor;
    case 0x63: return MTLBlendFactorBlendAlpha; case 0x64: return MTLBlendFactorOneMinusBlendAlpha;
    default: return MTLBlendFactorOne;
    }
}
MTLBlendOperation mtl_blend_op(unsigned e) {
    switch (e) { case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract; case 4: return MTLBlendOperationMin; case 5: return MTLBlendOperationMax; default: return MTLBlendOperationAdd; }
}

/* Advanced blend modes (NV_blend_equation_advanced) as fixed-function blending. With a premultiplied source and the uncorrelated overlap (the
 * defaults) the Porter-Duff modes, PLUS, SCREEN and MINUS are exact: result = Cs * Fs + Cd * Fd for constant factors. The others (MULTIPLY,
 * OVERLAY, DARKEN, the HSL modes, ...) need the destination colour inside the fragment shader (framebuffer fetch, translator work) and fall
 * back to SRC_OVER, as do the combinations a constant factor cannot express (see below). */
blend_st mtl_blend_resolve(blend_st b) {
    if (!b.adv_mode) return b;
    enum { Z = 1, O = 2, SC = 3, OMSC = 4, SA = 5, OMSA = 6, DA = 7, OMDA = 8 };
    static const struct { uint8_t mode, s, d, sa, da, eq, eqa; } map[] = {
        { 0x05, Z, Z, Z, Z, 1, 1 },          /* ZERO */
        { 0x06, O, Z, O, Z, 1, 1 },          /* SRC */
        { 0x07, Z, O, Z, O, 1, 1 },          /* DST */
        { 0x08, O, OMSA, O, OMSA, 1, 1 },    /* SRC_OVER */
        { 0x09, OMDA, O, OMDA, O, 1, 1 },    /* DST_OVER */
        { 0x0A, DA, Z, DA, Z, 1, 1 },        /* SRC_IN */
        { 0x0B, Z, SA, Z, SA, 1, 1 },        /* DST_IN */
        { 0x0C, OMDA, Z, OMDA, Z, 1, 1 },    /* SRC_OUT */
        { 0x0D, Z, OMSA, Z, OMSA, 1, 1 },    /* DST_OUT */
        { 0x0E, DA, OMSA, Z, O, 1, 1 },      /* SRC_ATOP: alpha = Ad */
        { 0x0F, OMDA, SA, O, Z, 1, 1 },      /* DST_ATOP: alpha = As */
        { 0x10, OMDA, OMSA, OMDA, OMSA, 1, 1 },  /* XOR */
        { 0x11, O, O, O, O, 1, 1 },          /* PLUS */
        { 0x12, O, O, O, O, 1, 1 },          /* PLUS_CLAMPED (normalized targets clamp) */
        { 0x13, O, O, O, O, 1, 1 },          /* PLUS_CLAMPED_ALPHA */
        { 0x16, O, OMSC, O, OMSA, 1, 1 },    /* SCREEN: Cs + Cd - Cs Cd */
        { 0x1F, O, O, O, OMSA, 3, 1 },       /* MINUS: Cd - Cs, alpha As + Ad - As Ad */
        { 0x20, O, O, O, OMSA, 3, 1 },       /* MINUS_CLAMPED */
    };
    blend_st r = b;
    r.adv_mode = r.adv_overlap = r.adv_nonpremul = r.adv_nonnorm = 0;
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++) {
        if (map[i].mode != b.adv_mode) continue;
        /* the overlap only matters for the modes defined through it (not ZERO / SRC / DST / PLUS* / MINUS*); a non-premultiplied source only
         * works when its own factor is 0 or 1 and the destination factor does not depend on its colour */
        int m = (int)b.adv_mode, overlap_free = m <= 0x07 || (m >= 0x11 && m <= 0x13) || m >= 0x1F;
        int premul_ok = !b.adv_nonpremul || ((map[i].s == Z || map[i].s == O) && map[i].d != OMSC);
        if ((overlap_free || !b.adv_overlap) && premul_ok) {
            r.src_rgb = map[i].s; r.dst_rgb = map[i].d; r.src_a = map[i].sa; r.dst_a = map[i].da; r.eq_rgb = map[i].eq; r.eq_a = map[i].eqa;
            if (b.adv_nonpremul && map[i].s == O) r.src_rgb = SA;           /* a non-premultiplied source colour is premultiplied by the factor */
            return r;
        }
        break;
    }
    static _Atomic uint32_t warned[2];
    uint32_t bit = 1u << (b.adv_mode & 31);
    if (!(atomic_fetch_or(&warned[b.adv_mode >> 5], bit) & bit))
        LOGW("advanced blend mode %#x (overlap %u, %spremultiplied source) has no fixed-function equivalent: drawn as SRC_OVER", (unsigned)b.adv_mode, (unsigned)b.adv_overlap, b.adv_nonpremul ? "non-" : "");
    r.src_rgb = b.adv_nonpremul ? SA : O; r.dst_rgb = OMSA; r.src_a = O; r.dst_a = OMSA; r.eq_rgb = r.eq_a = 1;
    return r;
}

/* Channels (RGBA bits) a colour format stores, for the archive filter in build_pipe. */
static uint32_t fmt_channels(uint32_t f) {
    switch (f) {
    case MTLPixelFormatA8Unorm: return 8;
    case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB: case MTLPixelFormatR8Snorm: case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint:
    case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint: case MTLPixelFormatR16Float:
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatR32Float: return 1;
    case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Unorm_sRGB: case MTLPixelFormatRG8Snorm: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
    case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRG16Float:
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float: return 3;
    case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatRG11B10Float: case MTLPixelFormatRGB9E5Float: return 7;
    default: return 15;
    }
}
static int writes_colour(const pipe_key *k) {
    for (int i = 0; i < 8; i++) if (k->color_fmt[i] && (k->wmask[i] & fmt_channels(k->color_fmt[i]))) return 1;
    return 0;
}

/* Builds once: whoever claims the pipe first (prewarm, the async path or a waiting draw) builds it, everyone else returns. */
static void build_pipe(GfxPipe *p, pipe_key k) {
    int unclaimed = 0;
    if (!__atomic_compare_exchange_n(&p->claimed, &unclaimed, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    ShaderKey vk = {0};
    memcpy(vk.attr_kind, k.attr_kind, sizeof vk.attr_kind);
    vk.depth_slots = k.depth_slots[0];
    vk.point_prim = k.topo == MTLPrimitiveTopologyClassPoint;
    vk.flip_y = k.flip_y; vk.depth_minus_one = k.depth_mode == 0;
    GfxShader *vs = shader_variant(k.vs, &vk, 1);
    if (vs->state != 1) { p->state = 2; return; }
    ShaderKey fk = {0};
    memcpy(fk.rt_kind, k.rt_kind, sizeof fk.rt_kind);
    fk.vs_out_mask = vs->info.out_mask;
    fk.depth_slots = k.depth_slots[1];
    { /* NAPPLE_SHADER_PROBE=<fragment program hash hex>:<pc hex>:<rA>:<rB>:<rC>[:<scale>][;<next probe>...]: the fragment program draws those registers as its colour
         * (debugging aid; several probes are cycled every 30 frames so one run can capture them all) */
        const char *pr = getenv("NAPPLE_SHADER_PROBE");
        unsigned long long h = 0; unsigned pc = 0, ra = 0, rb = 0, rc = 0; float sc = 1;
        for (unsigned idx = 0; pr && k.fs && k.probe; idx++) {
            const char *e = pr; for (unsigned j = 0; j + 1 < k.probe && e; j++) { e = strchr(e, ';'); if (e) e++; }
            if (e && sscanf(e, "%llx:%x:%u:%u:%u:%f", &h, &pc, &ra, &rb, &rc, &sc) >= 5 && h == k.fs) {
                fk.probe_pc = pc; fk.probe_reg[0] = (uint8_t)ra; fk.probe_reg[1] = (uint8_t)rb; fk.probe_reg[2] = (uint8_t)rc; fk.probe_scale = sc;
            }
            break;
        }
    }
    GfxShader *fs = k.fs ? shader_variant(k.fs, &fk, 1) : nil;
    if (k.fs && fs->state != 1) { p->state = 2; return; }
    @autoreleasepool {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = vs->fn;
        d.fragmentFunction = fs ? fs->fn : nil;
        { /* debugging aid: NAPPLE_DEBUG_SOLID_FS=<fragment program hash in hex> draws that program's geometry in solid magenta */
            const char *dbg = getenv("NAPPLE_DEBUG_SOLID_FS");
            if (dbg && fs && strtoull(dbg, NULL, 16) == k.fs) {
                static id<MTLFunction> solid;
                if (!solid) { NSError *e = nil; id<MTLLibrary> l = [g_mtl.dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 dbg() { return float4(1, 0, 1, 1); }" options:nil error:&e]; solid = [l newFunctionWithName:@"dbg"]; }
                d.fragmentFunction = solid; k.blend_en[0] = 0;
            }
        }
        d.rasterSampleCount = k.samples ? k.samples : 1;
        d.alphaToCoverageEnabled = k.a2c;
        d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)k.topo;
        for (int i = 0; i < 8; i++) {
            if (!k.color_fmt[i]) continue;
            MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
            c.pixelFormat = (MTLPixelFormat)k.color_fmt[i];
            uint32_t m = k.wmask[i];
            c.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
            if (k.blend_en[i]) {
                const blend_st *b = &k.blend[i];
                c.blendingEnabled = YES;
                c.sourceRGBBlendFactor = mtl_blend_factor((unsigned)b->src_rgb); c.destinationRGBBlendFactor = mtl_blend_factor((unsigned)b->dst_rgb);
                c.sourceAlphaBlendFactor = mtl_blend_factor((unsigned)b->src_a); c.destinationAlphaBlendFactor = mtl_blend_factor((unsigned)b->dst_a);
                c.rgbBlendOperation = mtl_blend_op((unsigned)b->eq_rgb); c.alphaBlendOperation = mtl_blend_op((unsigned)b->eq_a);
            }
        }
        d.depthAttachmentPixelFormat = (MTLPixelFormat)k.depth_fmt;
        d.stencilAttachmentPixelFormat = (MTLPixelFormat)k.stencil_fmt;
        /* vertex layout */
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        uint32_t used = vs->info.attr_mask, have = 0;
        for (int i = 0; i < k.nattr && i < 32; i++) {
            int kind;
            uint32_t vf = mtl_vertex_format((int)k.attr[i].fmt, &kind);
            if (!vf || !(used & (1u << i))) continue;
            vd.attributes[i].format = (MTLVertexFormat)vf;
            vd.attributes[i].offset = k.attr[i].offset;
            vd.attributes[i].bufferIndex = VB_BASE + (k.attr[i].stream % MAX_STREAMS);
            have |= 1u << i;
        }
        int need_dummy = 0;
        for (int i = 0; i < 32; i++) {            /* inputs without a bound attribute read zeros from a constant stream */
            if ((used & (1u << i)) && !(have & (1u << i))) {
                vd.attributes[i].format = MTLVertexFormatFloat4;
                vd.attributes[i].offset = 0;
                vd.attributes[i].bufferIndex = VB_BASE + MAX_STREAMS;
                need_dummy = 1;
            }
        }
        uint32_t used_streams = 0;
        for (int i = 0; i < 32; i++) if (have & (1u << i)) used_streams |= 1u << (k.attr[i].stream % MAX_STREAMS);
        for (int s = 0; s < k.nstream && s < MAX_STREAMS; s++) {
            if (!(used_streams & (1u << s))) continue;
            MTLVertexBufferLayoutDescriptor *l = vd.layouts[VB_BASE + s];
            if (k.stream[s].stride == 0) { l.stepFunction = MTLVertexStepFunctionConstant; l.stepRate = 0; l.stride = 16; }
            else if (k.stream[s].divisor) { l.stepFunction = MTLVertexStepFunctionPerInstance; l.stepRate = k.stream[s].divisor; l.stride = k.stream[s].stride; }
            else { l.stepFunction = MTLVertexStepFunctionPerVertex; l.stride = k.stream[s].stride; }
        }
        if (need_dummy) {
            MTLVertexBufferLayoutDescriptor *z = vd.layouts[VB_BASE + MAX_STREAMS];
            z.stepFunction = MTLVertexStepFunctionConstant; z.stepRate = 0; z.stride = 16;
        }
        d.vertexDescriptor = vd;
        NSError *err = nil;
        uint64_t pt0 = gfx_now_ns();
        if (g_archive) {
            d.binaryArchives = @[g_archive];
            p->ps = [g_mtl.dev newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
            if (p->ps) { atomic_fetch_add(&g_gfx.archive_hits, 1); archive_add(d); }
        }
        if (!p->ps) {
            p->ps = [g_mtl.dev newRenderPipelineStateWithDescriptor:d error:&err];
            /* Not archived: pipelines whose fragment shader writes nothing (no fragment shader, no colour target, or write masks that miss
             * every channel the targets have, e.g. alpha into R16Float): Metal drops their fragment stage and the whole archive then fails
             * to save ("expecting 'fragment' stage in pipeline no. N"). They compile on every run. */
            if (p->ps && d.fragmentFunction && writes_colour(&k)) archive_add(d);
        }
        { uint64_t dt = gfx_now_ns() - pt0; atomic_fetch_add(&g_st_pipe_n, 1); atomic_fetch_add(&g_st_pipe_ns, dt); st_max(&g_st_pipe_max, dt);
          if (dt > SLOW_STEP_MS * 1000000ull) LOGW("slow pipeline: vs %016llx fs %016llx took %.0f ms", (unsigned long long)k.vs, (unsigned long long)k.fs, dt / 1e6); }
        if (!p->ps) { LOGW("render pipeline creation failed: %s", err.localizedDescription.UTF8String); p->state = 2; return; }
        p->vs = vs; p->fs = fs;
        atomic_fetch_add(&g_gfx.pipelines_compiled, 1);
    }
    p->state = 1;
}

static void pipe_persist(const pipe_key *k, uint64_t h) {
    if (!g_pipe_file || k->probe) return;
    @synchronized (g_stored_hashes) {
        if ([g_stored_hashes containsObject:@(h)]) return;
        [g_stored_hashes addObject:@(h)];
        FILE *f = fopen(g_pipe_file.UTF8String, "ab");
        if (!f) { [NSFileManager.defaultManager createDirectoryAtPath:g_pipe_file.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil]; f = fopen(g_pipe_file.UTF8String, "ab"); }
        if (!f) return;
        if (ftell(f) == 0) { pipe_hdr hd = { PIPE_MAGIC, TRANSLATOR_VERSION, (uint32_t)sizeof *k, 0 }; fwrite(&hd, sizeof hd, 1, f); }
        fwrite(k, sizeof *k, 1, f);
        fclose(f);
    }
}

/* A program was registered: build, in the background, every persisted pipeline that names it and whose programs are all registered by now. */
void mtl_pipe_prewarm(uint64_t blob_hash) {
    if (!g_stored || getenv("NAPPLE_GFX_NO_PREWARM")) return;
    NSArray<NSData *> *keys;
    pthread_mutex_lock(&g_sh_mu);
    keys = [g_stored[@(blob_hash)] copy];
    pthread_mutex_unlock(&g_sh_mu);
    for (NSData *kd in keys) {
        const pipe_key *k = kd.bytes;
        if ((k->vs && !shader_blob_find(k->vs)) || (k->fs && !shader_blob_find(k->fs))) continue;        /* the other program is not registered yet: it triggers this */
        uint64_t h = fnv1a(k, sizeof *k, FNV_INIT);
        pthread_mutex_lock(&g_sh_mu);
        GfxPipe *p = g_pipes[@(h)];
        int create = !p;
        if (create) { p = [GfxPipe new]; g_pipes[@(h)] = p; }
        pthread_mutex_unlock(&g_sh_mu);
        if (!create) continue;
        pipe_key kc = *k;
        atomic_fetch_add(&g_gfx.pipelines_prewarmed, 1);
        dispatch_async(g_pipe_q, ^{ build_pipe(p, kc); });
    }
}

GfxPipe *mtl_pipe_get(const pipe_key *k) {
    uint64_t h = fnv1a(k, sizeof *k, FNV_INIT);
    NSNumber *key = @(h);
    static int skip_mode = -1;
    if (skip_mode < 0) skip_mode = getenv("NAPPLE_GFX_PIPE_SKIP") != NULL;
    pthread_mutex_lock(&g_sh_mu);
    GfxPipe *p = g_pipes[key];
    int create = !p;
    if (create) { p = [GfxPipe new]; g_pipes[key] = p; }
    pthread_mutex_unlock(&g_sh_mu);
    if (create) {
        pipe_key kc = *k;
        pipe_persist(k, h);
        if (skip_mode && !kc.probe) dispatch_async(g_pipe_q, ^{ build_pipe(p, kc); });
        else {                        /* synchronous on first use: nothing is skipped (built on this thread, see the comment at the top) */
            atomic_fetch_add(&g_gfx.compiles_blocking, 1);
            build_pipe(p, kc);
            atomic_fetch_sub(&g_gfx.compiles_blocking, 1);
        }
    } else {
        atomic_fetch_add(&g_gfx.pipelines_hit, 1);
    }
    int s = __atomic_load_n(&p->state, __ATOMIC_ACQUIRE);
    if (s == 0 && !skip_mode) {          /* queued for prewarm but not started: build it here; being built elsewhere: wait */
        uint64_t t0 = (gfx_now_ns() / 1000);
        atomic_fetch_add(&g_gfx.compiles_blocking, 1);
        build_pipe(p, *k);
        while (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == 0) usleep(100);
        atomic_fetch_sub(&g_gfx.compiles_blocking, 1);
        atomic_fetch_add(&g_gfx.pipeline_wait_us, (gfx_now_ns() / 1000) - t0);
        s = p->state;
    }
    if (s == 1) return p;
    return nil;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Compute pipelines
 * --------------------------------------------------------------------------------------------------------------- */
static NSMutableDictionary<NSNumber *, id> *g_cpipes;
id<MTLComputePipelineState> mtl_compute_pipe(uint64_t hash, GfxShader *__autoreleasing *sh) {
    ShaderKey key = {0};
    GfxShader *s = shader_variant(hash, &key, 0);
    if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == 0) {
        atomic_fetch_add(&g_gfx.compiles_blocking, 1);
        shader_variant(hash, &key, 1);
        atomic_fetch_sub(&g_gfx.compiles_blocking, 1);
    }
    *sh = s;
    if (s->state != 1) return nil;
    NSNumber *k = @(hash);
    pthread_mutex_lock(&g_sh_mu);
    if (!g_cpipes) g_cpipes = [NSMutableDictionary dictionary];
    id cp = g_cpipes[k];
    pthread_mutex_unlock(&g_sh_mu);
    if (cp) return cp == (id)NSNull.null ? nil : cp;
    NSError *err = nil;
    MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
    cd.computeFunction = s->fn;
    id<MTLComputePipelineState> ps = nil;
    atomic_fetch_add(&g_gfx.compiles_blocking, 1);
    if (g_archive) {
        cd.binaryArchives = @[g_archive];
        ps = [g_mtl.dev newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        if (ps) { atomic_fetch_add(&g_gfx.archive_hits, 1); archive_add(cd); }
    }
    if (!ps) { ps = [g_mtl.dev newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionNone reflection:nil error:&err]; if (ps) archive_add(cd); }
    atomic_fetch_sub(&g_gfx.compiles_blocking, 1);
    if (!ps) LOGW("compute pipeline creation failed: %s", err.localizedDescription.UTF8String);
    pthread_mutex_lock(&g_sh_mu);
    g_cpipes[k] = ps ?: (id)NSNull.null;
    pthread_mutex_unlock(&g_sh_mu);
    return ps;
}
