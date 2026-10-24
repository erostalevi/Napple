/* core.c - library lifecycle, settings, host hooks, logging, handle table, NVN name table and diagnostics. */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "obj.h"
#include "gfx_mtl.h"

gfx_global g_gfx;
#define HOOK(field) (g_gfx.cfg.hooks.size > offsetof(napple_gfx_host_hooks, field) ? g_gfx.cfg.hooks.field : NULL)
static pthread_mutex_t g_settings_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------------------------------------------------------------------
 * Logging
 * --------------------------------------------------------------------------------------------------------------- */
void gfx_log(napple_log_level lvl, const char *fmt, ...) {
    if ((int)lvl > g_gfx.log_level) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    void (*hook)(void *, napple_log_level, const char *) = HOOK(log);
    if (hook) { hook(g_gfx.cfg.hooks.user, lvl, buf); return; }
    static const char *const tag[] = { "error", "warn", "info", "debug" };
    fprintf(stderr, "[napple-gfx] %s: %s\n", tag[lvl & 3], buf);
}

void gfx_die(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    gfx_log(NAPPLE_LOG_ERROR, "fatal: %s", buf);
    fflush(stderr);
    abort();
}

/* ---------------------------------------------------------------------------------------------------------------
 * Host hooks
 * --------------------------------------------------------------------------------------------------------------- */
uint64_t gfx_now_ns(void) {
    uint64_t (*f)(void *) = HOOK(now_ns);
    if (f) return f(g_gfx.cfg.hooks.user);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
int gfx_core_away(void) {
    int (*f)(void *) = HOOK(core_away);
    return f ? f(g_gfx.cfg.hooks.user) : 0;
}
void gfx_core_back(int *token) {
    void (*f)(void *, int *) = HOOK(core_back);
    if (f) f(g_gfx.cfg.hooks.user, token);
}
void *gfx_wrap_callable(void *fn, const char *name) {
    void *(*f)(void *, void *, const char *) = HOOK(wrap_guest_callable);
    return f && fn ? f(g_gfx.cfg.hooks.user, fn, name) : fn;
}
void gfx_notify_present(uint64_t frame, uint32_t swap_interval) {
    void (*f)(void *, uint64_t, uint32_t) = HOOK(on_present);
    if (f) f(g_gfx.cfg.hooks.user, frame, swap_interval);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Handle table
 * --------------------------------------------------------------------------------------------------------------- */
#define HT_SEG_BITS 12
#define HT_SEG_SIZE (1u << HT_SEG_BITS)
#define HT_MAX_SEGS 4096
typedef struct { _Atomic(void *) p; _Atomic uint32_t gen; } hent;
static _Atomic(hent *) g_ht_seg[HT_MAX_SEGS];
static pthread_mutex_t g_ht_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *g_ht_free;
static size_t g_ht_nfree, g_ht_capfree;
static uint32_t g_ht_next = 1;

static hent *ht_slot(uint32_t id) {
    uint32_t s = id >> HT_SEG_BITS;
    if (s >= HT_MAX_SEGS) return NULL;
    hent *seg = atomic_load_explicit(&g_ht_seg[s], memory_order_acquire);
    return seg ? &seg[id & (HT_SEG_SIZE - 1)] : NULL;
}

uint64_t htab_alloc(void *p) {
    pthread_mutex_lock(&g_ht_mu);
    uint32_t id;
    if (g_ht_nfree) id = g_ht_free[--g_ht_nfree];
    else {
        id = g_ht_next++;
        uint32_t s = id >> HT_SEG_BITS;
        if (s >= HT_MAX_SEGS) gfx_die("handle table exhausted");
        if (!atomic_load(&g_ht_seg[s])) {
            hent *seg = calloc(HT_SEG_SIZE, sizeof(hent));
            if (!seg) gfx_die("out of memory (handle table)");
            for (uint32_t i = 0; i < HT_SEG_SIZE; i++) atomic_store(&seg[i].gen, 1);
            atomic_store_explicit(&g_ht_seg[s], seg, memory_order_release);
        }
    }
    hent *e = ht_slot(id);
    atomic_store_explicit(&e->p, p, memory_order_release);
    uint64_t h = (uint64_t)atomic_load(&e->gen) << 32 | id;
    pthread_mutex_unlock(&g_ht_mu);
    return h;
}

void *htab_get(uint64_t h) {
    hent *e = ht_slot((uint32_t)h);
    if (!e || atomic_load_explicit(&e->gen, memory_order_acquire) != (uint32_t)(h >> 32)) return NULL;
    return atomic_load_explicit(&e->p, memory_order_acquire);
}

void htab_free(uint64_t h) {
    hent *e = ht_slot((uint32_t)h);
    if (!e) return;
    pthread_mutex_lock(&g_ht_mu);
    if (atomic_load(&e->gen) == (uint32_t)(h >> 32)) {
        atomic_store(&e->p, NULL);
        uint32_t g = atomic_fetch_add(&e->gen, 1) + 1;
        if (g == 0) atomic_store(&e->gen, 1);
        if (g_ht_nfree == g_ht_capfree) {
            g_ht_capfree = g_ht_capfree ? g_ht_capfree * 2 : 1024;
            g_ht_free = realloc(g_ht_free, g_ht_capfree * sizeof *g_ht_free);
        }
        g_ht_free[g_ht_nfree++] = (uint32_t)h;
    }
    pthread_mutex_unlock(&g_ht_mu);
}

/* ---------------------------------------------------------------------------------------------------------------
 * NVN names, implementations and unimplemented stubs
 * --------------------------------------------------------------------------------------------------------------- */
#define NAME_HASH_SIZE 2048
static int16_t g_name_hash[NAME_HASH_SIZE];            /* index + 1, 0 = empty */
static void *g_impl[1024];                              /* implementation per name index */
static _Atomic uint64_t g_unimpl_calls[1024];
static _Atomic uint64_t g_unknown_lookup_count[64];
static char g_unknown_lookup_name[64][96];
static _Atomic uint32_t g_unknown_lookup_n;
static pthread_once_t g_names_once = PTHREAD_ONCE_INIT;

extern void gfx_api_link_core(void), gfx_api_link_device(void), gfx_api_link_memory(void), gfx_api_link_texture(void),
    gfx_api_link_program(void), gfx_api_link_cmd(void), gfx_api_link_state(void), gfx_api_link_window(void);

static const void *impl_section(unsigned long *size) {
    Dl_info di;
    if (!dladdr((const void *)&g_names_once, &di)) return NULL;
    return getsectiondata((const struct mach_header_64 *)di.dli_fbase, "__DATA", "napple_nvn", size);
}

static void names_init(void) {
    /* The API files only contribute section entries; reference them so a static link keeps their objects. */
    gfx_api_link_core(); gfx_api_link_device(); gfx_api_link_memory(); gfx_api_link_texture();
    gfx_api_link_program(); gfx_api_link_cmd(); gfx_api_link_state(); gfx_api_link_window();
    for (unsigned i = 0; i < nvn_name_count; i++) {
        uint32_t h = (uint32_t)fnv1a(nvn_names[i], strlen(nvn_names[i]), FNV_INIT) & (NAME_HASH_SIZE - 1);
        while (g_name_hash[h]) h = (h + 1) & (NAME_HASH_SIZE - 1);
        g_name_hash[h] = (int16_t)(i + 1);
    }
    unsigned long sz = 0;
    const nvn_impl *im = (const nvn_impl *)impl_section(&sz);
    unsigned n = im ? (unsigned)(sz / sizeof *im) : 0, hit = 0;
    for (unsigned k = 0; k < n; k++) {
        int i = nvn_name_index(im[k].name);
        if (i < 0) { LOGE("NVN implementation %s is not in the name table", im[k].name); continue; }
        g_impl[i] = im[k].fn;
        hit++;
    }
    LOGI("NVN: %u of %u entry points implemented", hit, nvn_name_count);
}

int nvn_name_index(const char *name) {
    if (!name) return -1;
    uint32_t h = (uint32_t)fnv1a(name, strlen(name), FNV_INIT) & (NAME_HASH_SIZE - 1);
    while (g_name_hash[h]) {
        int i = g_name_hash[h] - 1;
        if (!strcmp(nvn_names[i], name)) return i;
        h = (h + 1) & (NAME_HASH_SIZE - 1);
    }
    return -1;
}

void *nvn_lookup(const char *name) {
    pthread_once(&g_names_once, names_init);
    int i = nvn_name_index(name);
    if (i < 0) {
        uint32_t n = atomic_fetch_add(&g_unknown_lookup_n, 1);
        if (n < 64) { snprintf(g_unknown_lookup_name[n], sizeof g_unknown_lookup_name[n], "%s", name ? name : "(null)"); atomic_store(&g_unknown_lookup_count[n], 1); }
        LOGW("NVN lookup of unknown name '%s'", name ? name : "(null)");
        return NULL;
    }
    return g_impl[i] ? g_impl[i] : nvn_unimpl_thunk[i];
}

void nvn_unimpl_report(unsigned idx) {
    if (idx >= nvn_name_count) return;
    uint64_t n = atomic_fetch_add(&g_unimpl_calls[idx], 1);
    atomic_fetch_add(&g_gfx.unimpl_total, 1);
    if (n == 0) LOGW("unimplemented NVN entry point called: %s", nvn_names[idx]);
    if (g_gfx.settings.unimplemented_abort) gfx_die("unimplemented NVN entry point %s", nvn_names[idx]);
}

size_t napple_gfx_diag_unimplemented(napple_gfx_diag_entry *out, size_t max) {
    size_t total = 0;
    for (unsigned i = 0; i < nvn_name_count; i++) {
        uint64_t c = atomic_load(&g_unimpl_calls[i]);
        if (!c) continue;
        if (out && total < max) out[total] = (napple_gfx_diag_entry){ nvn_names[i], c };
        total++;
    }
    return total;
}
size_t napple_gfx_diag_unknown_lookups(napple_gfx_diag_entry *out, size_t max) {
    size_t n = atomic_load(&g_unknown_lookup_n);
    if (n > 64) n = 64;
    for (size_t i = 0; out && i < n && i < max; i++) out[i] = (napple_gfx_diag_entry){ g_unknown_lookup_name[i], atomic_load(&g_unknown_lookup_count[i]) };
    return n;
}
void napple_gfx_diag_dump(void) {
    napple_gfx_diag_entry e[256];
    size_t n = napple_gfx_diag_unimplemented(e, 256);
    LOGI("diag: %zu distinct unimplemented NVN entry points hit", n);
    for (size_t i = 0; i < n && i < 256; i++) LOGI("diag:   %s x%llu", e[i].name, (unsigned long long)e[i].calls);
    napple_gfx_diag_entry u[64];
    size_t m = napple_gfx_diag_unknown_lookups(u, 64);
    for (size_t i = 0; i < m; i++) LOGI("diag: unknown lookup %s", u[i].name);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Settings, stats, config
 * --------------------------------------------------------------------------------------------------------------- */
static void settings_defaults(napple_gfx_settings *s) {
    memset(s, 0, sizeof *s);
    s->size = sizeof *s;
    s->internal_res_scale = 1.0f;
    s->pacing = NAPPLE_PACING_GAME;
    s->async_shader_compile = 1;
    s->prewarm_shader_cache = 1;
    s->docked = 1;
}

void gfx_settings_get(napple_gfx_settings *out) {
    pthread_mutex_lock(&g_settings_mu);
    *out = g_gfx.settings;
    pthread_mutex_unlock(&g_settings_mu);
}
void napple_gfx_get_settings(napple_gfx_settings *out) { gfx_settings_get(out); }
void napple_gfx_set_settings(const napple_gfx_settings *s) {
    if (!s || s->size < offsetof(napple_gfx_settings, reserved)) return;
    pthread_mutex_lock(&g_settings_mu);
    napple_gfx_settings n;
    settings_defaults(&n);
    memcpy(&n, s, s->size < sizeof n ? s->size : sizeof n);
    n.size = sizeof n;
    if (n.internal_res_scale <= 0.0f) n.internal_res_scale = 1.0f;
    if (n.internal_res_scale > 8.0f) n.internal_res_scale = 8.0f;
    g_gfx.settings = n;
    atomic_fetch_add(&g_gfx.settings_gen, 1);
    pthread_mutex_unlock(&g_settings_mu);
    if (g_gfx.initialized) gfx_mtl_settings_changed();
}

void napple_gfx_config_default(napple_gfx_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->size = sizeof *cfg;
    cfg->abi_version = NAPPLE_GFX_ABI_VERSION;
    cfg->headless_width = 1280;
    cfg->headless_height = 720;
    settings_defaults(&cfg->settings);
    cfg->hooks.size = sizeof cfg->hooks;
}

void napple_gfx_get_stats(napple_gfx_stats *out) {
    memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->frames = atomic_load(&g_gfx.frames);
    out->draws_total = atomic_load(&g_gfx.draws);
    out->dispatches_total = atomic_load(&g_gfx.dispatches);
    out->submits_total = atomic_load(&g_gfx.submits);
    out->pipelines_compiled = atomic_load(&g_gfx.pipelines_compiled);
    out->pipelines_cached_hit = atomic_load(&g_gfx.pipelines_hit);
    out->shaders_translated = atomic_load(&g_gfx.shaders_translated);
    out->shader_cache_disk_hits = atomic_load(&g_gfx.shader_disk_hits);
    out->shaders_pending_async = atomic_load(&g_gfx.shaders_pending);
    out->draws_skipped_pending_shader = atomic_load(&g_gfx.draws_skipped);
    out->unimplemented_calls_total = atomic_load(&g_gfx.unimpl_total);
    out->unimplemented_distinct = (uint32_t)napple_gfx_diag_unimplemented(NULL, 0);
    out->gpu_ms_last_frame_x1000 = atomic_load(&g_gfx.gpu_us_last);
    out->prewarm_done = atomic_load(&g_gfx.prewarm_done);
    out->prewarm_total = atomic_load(&g_gfx.prewarm_total);
    out->compiles_blocking = atomic_load(&g_gfx.compiles_blocking);
    out->pipelines_archive_hits = atomic_load(&g_gfx.archive_hits);
}
uint64_t napple_gfx_frame_count(void) { return atomic_load(&g_gfx.frames); }

int napple_gfx_is_initialized(void) { return g_gfx.initialized; }

int napple_gfx_init(const napple_gfx_config *cfg) {
    if (g_gfx.initialized) return 0;
    if (!cfg || cfg->size < offsetof(napple_gfx_config, settings) || cfg->abi_version != NAPPLE_GFX_ABI_VERSION) return -1;
    memset(&g_gfx.cfg, 0, sizeof g_gfx.cfg);
    memcpy(&g_gfx.cfg, cfg, cfg->size < sizeof g_gfx.cfg ? cfg->size : sizeof g_gfx.cfg);
    g_gfx.log_level = getenv("NAPPLE_GFX_LOG") ? atoi(getenv("NAPPLE_GFX_LOG")) : NAPPLE_LOG_INFO;
    g_prof_on = getenv("NAPPLE_GFX_PROF") != NULL;
    g_gfx.origin_flip = getenv("NAPPLE_ORIGIN_FLIP") ? atoi(getenv("NAPPLE_ORIGIN_FLIP")) != 0 : 1;   /* NAPPLE_ORIGIN_FLIP=0: old convention (debug) */
    if (cfg->size >= sizeof(napple_gfx_config)) g_gfx.settings = cfg->settings; else settings_defaults(&g_gfx.settings);
    if (g_gfx.settings.size == 0) settings_defaults(&g_gfx.settings);
    g_gfx.settings.size = sizeof g_gfx.settings;
    snprintf(g_gfx.title_id, sizeof g_gfx.title_id, "%s", cfg->title_id ? cfg->title_id : "0000000000000000");
    snprintf(g_gfx.cache_dir, sizeof g_gfx.cache_dir, "%s", cfg->cache_dir ? cfg->cache_dir : "");
    g_gfx.cfg.title_id = g_gfx.title_id;
    g_gfx.cfg.cache_dir = g_gfx.cache_dir[0] ? g_gfx.cache_dir : NULL;
    g_gfx.cfg.shader_translator = NULL;
    g_gfx.device_origin = 0;          /* NVN defaults: lower-left origin, near = -w */
    g_gfx.device_depth_mode = 0;
    pthread_once(&g_names_once, names_init);
    if (gfx_mtl_init(cfg->metal_layer, cfg->headless_width ? cfg->headless_width : 1280, cfg->headless_height ? cfg->headless_height : 720) != 0) {
        LOGE("Metal initialization failed");
        return -2;
    }
    gfx_mtl_set_gmem(cfg->gmem_base, cfg->gmem_size);       /* global memory is addressed by GPU virtual address; the window is unused */
    g_gfx.initialized = 1;
    LOGI("initialized (title %s, %s)", g_gfx.title_id, cfg->metal_layer ? "layer" : "headless");
    return 0;
}

void napple_gfx_shutdown(void) {
    if (!g_gfx.initialized) return;
    gfx_mtl_shutdown();
    g_gfx.initialized = 0;
}

/* nv:: entry points the guest imports; see napple_gfx_hle_table(). */
void *napple_gfx_nvn_bootstrap_loader(const char *name) {
    void *fn = nvn_lookup(name);
    return gfx_wrap_callable(fn, name);
}
static void *hle_bootstrap(const char *name) { return napple_gfx_nvn_bootstrap_loader(name); }

const napple_gfx_hle_entry *napple_gfx_hle_table(size_t *count) {
    static const napple_gfx_hle_entry t[] = {
        { "nvnBootstrapLoader", (void *)hle_bootstrap, "NVN entry point loader; the SDK's own NVN driver is never executed" },
        /* nv::InitializeGraphics / SetGraphicsAllocator / SetGraphicsDevtoolsAllocator are deliberately NOT overridden: they run natively
         * against the runtime's nvdrv service, which the SDK's android:: / NvRm code (nn::vi layers, gralloc, media) needs. Only NVN is ours.
         * (Older note kept for the allocators:) nv::SetGraphicsAllocator / SetGraphicsDevtoolsAllocator are deliberately NOT overridden: the SDK's android:: code inside nn::vi
         * allocates through the callbacks they store (overriding them crashes Super Mario Odyssey). */
    };
    if (count) *count = sizeof t / sizeof *t;
    return t;
}

/* The API files register themselves through the section; these anchors keep their objects in a static link. */
void gfx_api_link_core(void) {}
