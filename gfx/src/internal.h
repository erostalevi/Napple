/* internal.h - shared internals of libnapple_gfx (not installed; the public header is include/napple_gfx.h). */
#ifndef NAPPLE_GFX_INTERNAL_H
#define NAPPLE_GFX_INTERNAL_H

#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../include/napple_gfx.h"
#include "nvnapi/nvn.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_PRINTF(a, b) __attribute__((format(printf, a, b)))

/* ---- logging ------------------------------------------------------------------------------------------------ */
void gfx_log(napple_log_level lvl, const char *fmt, ...) GFX_PRINTF(2, 3);
#define LOGE(...) gfx_log(NAPPLE_LOG_ERROR, __VA_ARGS__)
#define LOGW(...) gfx_log(NAPPLE_LOG_WARN, __VA_ARGS__)
#define LOGI(...) gfx_log(NAPPLE_LOG_INFO, __VA_ARGS__)
#define LOGD(...) gfx_log(NAPPLE_LOG_DEBUG, __VA_ARGS__)
/* Fatal: the guest passed something that cannot be handled at all. Logs and aborts. */
void gfx_die(const char *fmt, ...) GFX_PRINTF(1, 2) __attribute__((noreturn));
/* One-shot log keyed by call site. */
#define LOG_ONCE(lvl, ...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) gfx_log(lvl, __VA_ARGS__); } while (0)

/* ---- global state ------------------------------------------------------------------------------------------------ */
typedef struct gfx_global {
    napple_gfx_config cfg;                /* copy of the init config (strings duplicated) */
    char title_id[20];
    char cache_dir[1024];
    int initialized;
    int log_level;
    uint32_t device_origin;               /* NVNwindowOriginMode: 0 lower-left, 1 upper-left */
    uint32_t device_depth_mode;           /* NVNdepthMode: 0 near=-w (GL), 1 near=0 */
    int origin_flip;                      /* lower-left origin emulated exactly: render targets stored bottom-up (see mtl_replay.m), flipped at present */
    napple_gfx_settings settings;         /* protected by settings_lock (readers copy) */
    _Atomic uint32_t settings_gen;
    _Atomic uint64_t frames;
    _Atomic uint64_t draws, dispatches, submits;
    _Atomic uint64_t pipelines_compiled, pipelines_hit, shaders_translated, shader_disk_hits, shaders_pending, draws_skipped, pipelines_prewarmed, pipeline_wait_us, pipelines_failed;
    _Atomic uint64_t unimpl_total;
    _Atomic uint64_t gpu_us_last;
    _Atomic uint32_t prewarm_done, prewarm_total;
    _Atomic uint32_t compiles_blocking;   /* game-facing calls waiting on a shader/pipeline compile right now */
    _Atomic uint64_t archive_hits;        /* pipelines loaded from the Metal binary archive */
} gfx_global;
extern gfx_global g_gfx;

/* NAPPLE_GFX_PROF=1: coarse pipeline timing counters (ns): 0 guest blocked in SyncWait, 1 replay CPU in submissions, 2 GPU commit->complete,
   3 headless pacing sleep, 4 job queue latency, 5 submissions, 6 presents */
extern _Atomic uint64_t g_prof[8];
extern int g_prof_on;

/* settings snapshot (cheap, lock-free enough: copies under a spin flag) */
void gfx_settings_get(napple_gfx_settings *out);

/* ---- host hooks ------------------------------------------------------------------------------------------------ */
uint64_t gfx_now_ns(void);
int  gfx_core_away(void);
void gfx_core_back(int *token);
#define GFX_BLOCKING() __attribute__((cleanup(gfx_core_back))) int gfx_away_token_ = gfx_core_away()
void *gfx_wrap_callable(void *fn, const char *name);
void gfx_notify_present(uint64_t frame, uint32_t swap_interval);

/* ---- NVN name table / diag ------------------------------------------------------------------------------------ */
extern const char *const nvn_names[];     /* generated from nvnapi/names.txt */
extern const unsigned nvn_name_count;
extern void *nvn_unimpl_thunk[];          /* generated per-name stubs */
int  nvn_name_index(const char *name);    /* -1 if unknown */
void nvn_unimpl_report(unsigned idx);     /* called by the common unimplemented stub */
void *nvn_lookup(const char *name);       /* implemented function, unimplemented thunk, or NULL for unknown names */

typedef struct nvn_impl { const char *name; void *fn; } nvn_impl;
/* Register an NVN entry implementation. The prototype is checked against nvnapi/nvn_protos.h at compile time. */
#include "nvnapi/nvn_protos.h"
#define NVN_IMPL_SECTION "__DATA,napple_nvn"
#define NVN(name, ret, ...)                                                                              \
    static ret h_##name(__VA_ARGS__);                                                                    \
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                              \
    __attribute__((used, section(NVN_IMPL_SECTION))) static const nvn_impl e_##name = {#name, (void *)h_##name}; \
    static ret h_##name(__VA_ARGS__)
/* Entries with no prototype in nvn_protos.h (newer NVN versions): no signature check. */
#define NVN_NOPROTO(name, ret, ...)                                                                      \
    static ret h_##name(__VA_ARGS__);                                                                    \
    __attribute__((used, section(NVN_IMPL_SECTION))) static const nvn_impl e_##name = {#name, (void *)h_##name}; \
    static ret h_##name(__VA_ARGS__)

/* NVNboolean parameters arrive in a 64-bit register and Switch callers do not zero-extend: only the low 8 bits are valid. */
#define B(x) ((x) & 0xff)

/* ---- small helpers --------------------------------------------------------------------------------------------- */
static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
static inline uint64_t fnv1a(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
#define FNV_INIT 0xcbf29ce484222325ull

#ifdef __cplusplus
}
#endif
#endif
