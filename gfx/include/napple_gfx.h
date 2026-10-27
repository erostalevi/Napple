/*
 * napple_gfx.h - public interface of libnapple_gfx (NVN -> Metal) for the Napple runtime.
 *
 * Contract version: NAPPLE_GFX_ABI_VERSION. Pure C, no Objective-C types: the runtime (Rust/C) can include or
 * bind this directly. See docs/architecture/gfx.md for the prose version (what the runtime must provide) and
 * docs/architecture/gfx.md for how the library works inside.
 *
 * Model
 *   - The guest (Switch game) runs natively. It calls NVN through function pointers obtained from
 *     nvnBootstrapLoader("nvnDeviceGetProcAddress"). The runtime must make the guest's imports of
 *     nvnBootstrapLoader / nv::InitializeGraphics / nv::SetGraphics*Allocator land in this library
 *     (napple_gfx_hle_table() lists them with their Itanium-mangled or plain names).
 *   - Guest memory is host memory: a GPU virtual address handed to NVN IS the host virtual address
 *     (identity mapping). The runtime must guarantee that for every guest range that can back an NVN memory pool.
 *   - All functions may be called from any guest/host thread unless noted. Nothing here is async-signal-safe
 *     except napple_gfx_handle_fault().
 */
#ifndef NAPPLE_GFX_H
#define NAPPLE_GFX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAPPLE_GFX_ABI_VERSION 1u

/* ---- logging ---------------------------------------------------------------------------------------------- */
typedef enum { NAPPLE_LOG_ERROR = 0, NAPPLE_LOG_WARN = 1, NAPPLE_LOG_INFO = 2, NAPPLE_LOG_DEBUG = 3 } napple_log_level;

/* ---- runtime -> gfx hooks (all optional; NULL = built-in default) ------------------------------------------- */
typedef struct napple_gfx_host_hooks {
    uint32_t size;                      /* sizeof(napple_gfx_host_hooks) */
    void *user;                         /* passed back to every hook */

    /* Log sink. Default: stderr with prefix "[napple-gfx]". msg is one line without trailing newline. */
    void (*log)(void *user, napple_log_level level, const char *msg);

    /* Monotonic nanoseconds as seen by the guest (so frame pacing and fences follow the runtime's clock). */
    uint64_t (*now_ns)(void *user);

    /* Thread model: NVN calls that block on the GPU / display (nvnQueueFinish, nvnSyncWait, nvnWindowAcquireTexture,
     * nvnEventWait, ...) call core_away before blocking and core_back(&token) after, so the runtime can release the
     * emulated Horizon core the calling guest thread occupies. token is whatever core_away returned. */
    int  (*core_away)(void *user);
    void (*core_back)(void *user, int *token);

    /* Every function pointer returned to the guest (NVN entry points) passes through this, so the runtime can wrap
     * it to run on the guest thread's host stack / switch x18 / etc. Return the wrapper (or fn itself). */
    void *(*wrap_guest_callable)(void *user, void *fn, const char *name);

    /* Called once per guest present (nvnQueuePresentTexture), on the guest's thread, after the frame was queued.
     * swap_interval: value the game requested (1 = 60 Hz, 2 = 30 Hz ...). The runtime uses it to fire the vsync/
     * display event; see napple_gfx_vsync_period_ns(). */
    void (*on_present)(void *user, uint64_t frame_index, uint32_t swap_interval);

    /* Optional notification that the drawable / output size changed (points = Metal layer size in pixels). */
    void (*on_output_resize)(void *user, uint32_t width, uint32_t height);
} napple_gfx_host_hooks;

/* ---- settings (can be changed at any time with napple_gfx_set_settings) ---------------------------------------- */
typedef enum {
    NAPPLE_PACING_GAME = 0,   /* follow the game's swap interval at the display refresh (vsync-locked) */
    NAPPLE_PACING_FIXED = 1,  /* present at target_fps regardless of the game's swap interval (see design doc) */
    NAPPLE_PACING_UNCAPPED = 2
} napple_pacing_mode;

typedef enum { NAPPLE_UPSCALE_NONE = 0, NAPPLE_UPSCALE_BILINEAR = 1, NAPPLE_UPSCALE_METALFX = 2 } napple_upscaler;

typedef struct napple_gfx_settings {
    uint32_t size;                      /* sizeof(napple_gfx_settings) */
    float    internal_res_scale;        /* multiplier on the resolution the game renders at (1.0 = native). Applies to render targets
                                           created with a window/scene-size heuristic; 0 = 1.0 */
    uint32_t output_width;              /* custom output (swapchain) size in pixels; 0 = layer size */
    uint32_t output_height;
    napple_pacing_mode pacing;
    float    target_fps;                /* for NAPPLE_PACING_FIXED; also the vsync period reported to the runtime. 0 = display refresh */
    uint32_t game_fps_divisor_fix;      /* 1: when the game runs a 60 Hz sim at swap_interval 2, keep the sim step (present-interval
                                           fix against speed-up when the display refreshes faster than the game's native rate) */
    napple_upscaler upscaler;
    uint32_t anisotropy;                /* 0 = as requested by the game */
    uint32_t async_shader_compile;      /* 1 = compile pipelines off-thread, draw is skipped (or uses a fallback) until ready */
    uint32_t prewarm_shader_cache;      /* 1 = compile every cached shader at init (progress via napple_gfx_get_stats) */
    uint32_t docked;                    /* 1 = report docked operation mode properties to the game (affects game-chosen resolution) */
    uint32_t unimplemented_abort;       /* 1 = abort() on the first unimplemented NVN call; 0 = log once, return 0 */
    uint32_t present_interval_override; /* 0 = honour the game; N>0 = treat every present as interval N (see docs/architecture/gfx.md, frame pacing) */
    uint32_t host_vsync_off;            /* 1 = the layer does not sync to the display refresh */
    uint32_t reserved[6];
} napple_gfx_settings;

/* ---- init ---------------------------------------------------------------------------------------------------- */
typedef struct napple_gfx_config {
    uint32_t size;                      /* sizeof(napple_gfx_config) */
    uint32_t abi_version;               /* NAPPLE_GFX_ABI_VERSION the caller was compiled against */
    void    *metal_layer;               /* CAMetalLayer* (bridge-retained by caller, must outlive gfx). NULL = headless: frames are
                                           rendered into an offscreen texture of headless_width x headless_height */
    uint32_t headless_width, headless_height;
    const char *title_id;               /* 16 hex digits, used in cache keys and logs */
    const char *cache_dir;              /* per-game writable dir (shader cache, pipeline cache). NULL = no disk cache */
    const char *shader_translator;      /* reserved, must be NULL: shader translation is the in-process native Maxwell translator */
    uint64_t gmem_base, gmem_size;      /* guest heap window shaders may address with global memory instructions (page aligned; 0 = none) */
    uint32_t nvn_major, nvn_minor;      /* API version to report from nvnDeviceGetInteger (0,0 = default 55.15 which is accepted by SMO and LA) */
    napple_gfx_settings settings;
    napple_gfx_host_hooks hooks;
} napple_gfx_config;

/* Fill cfg with defaults (size/abi set). Always call before editing a config. */
void napple_gfx_config_default(napple_gfx_config *cfg);

/* Creates the Metal device/queue and gfx threads. Returns 0 on success. May be called by the runtime on startup or lazily from the
 * hle override of nv::InitializeGraphics (then the runtime must have called napple_gfx_set_config earlier). */
int  napple_gfx_init(const napple_gfx_config *cfg);
void napple_gfx_shutdown(void);
int  napple_gfx_is_initialized(void);

void napple_gfx_get_settings(napple_gfx_settings *out);
void napple_gfx_set_settings(const napple_gfx_settings *s);   /* thread-safe, takes effect at the next present */

/* ---- guest-facing entry points -------------------------------------------------------------------------------- */
/* Symbols the runtime must redirect (guest import name -> host function). name is the exact symbol as imported by the guest
 * (plain C name "nvnBootstrapLoader", or Itanium mangled name for nv:: functions). fn follows the AAPCS64 signature of the original.
 * The table is static for the life of the process. */
typedef struct napple_gfx_hle_entry { const char *symbol; void *fn; const char *note; } napple_gfx_hle_entry;
const napple_gfx_hle_entry *napple_gfx_hle_table(size_t *count);

/* void *nvnBootstrapLoader(const char *name): returns NVN function pointers ("nvnDeviceGetProcAddress", "nvnDeviceInitialize"...).
 * Same function that napple_gfx_hle_table() exposes as "nvnBootstrapLoader". */
void *napple_gfx_nvn_bootstrap_loader(const char *name);

/* ---- guest memory tracking (dirty pages) ----------------------------------------------------------------------- */
/* Must be called from the runtime's fault handler (Mach exception / SIGSEGV+SIGBUS) for faults on guest memory. Returns 1 if the fault
 * was a gfx write-protection fault and has been resolved (the faulting instruction should be retried), 0 otherwise.
 * Uses only atomics + mprotect: async-signal-safe. ucontext is the platform ucontext_t* (may be NULL). */
int  napple_gfx_handle_fault(void *addr, int is_write, void *ucontext);
/* Must be called before the HOST writes into guest memory (file reads, IPC buffers, audio, memcpy by services) if the range may back an
 * NVN memory pool; it removes gfx write protection on the covered pages. Cheap when nothing is tracked. */
void napple_gfx_guest_write_prepare(void *addr, size_t len);

/* ---- frame / presentation / pacing ----------------------------------------------------------------------------- */
/* Period in ns at which the runtime should signal the guest's vsync / display event: 1e9/refresh, or 1e9/target_fps (FIXED). */
uint64_t napple_gfx_vsync_period_ns(void);
/* Override the API version nvnDeviceGetInteger reports (the runtime learns it from the guest after napple_gfx_init). */
void napple_gfx_set_nvn_version(uint32_t major, uint32_t minor);
/* Blocks until the in-flight frame budget allows another acquire (internal; the runtime normally never calls it). */
uint64_t napple_gfx_frame_count(void);

/* Write the next presented frame to a PNG file (the screen image after scaling, BGRA8 -> sRGB PNG). Asynchronous: the file exists when
 * on_done fires (or poll napple_gfx_screenshot_pending() == 0). Used for headless verification. Returns 0 if queued. */
int  napple_gfx_screenshot(const char *png_path);
int  napple_gfx_screenshot_pending(void);
/* Hidden: frames complete and pace the guest as usual but are not shown (another image draws on the layer: a library applet run in-process). */
void napple_gfx_set_hidden(int hidden);
/* Same but for the last completed frame, synchronous. Returns 0 on success. */
int  napple_gfx_screenshot_last(const char *png_path);

/* ---- host-side texture writes (video decoders: the host plays the role of the video engine) ---- */
/* Format (NVNformat value), width and height of an NVNtexture the guest initialised; 0 on success. */
int napple_gfx_texture_info(void *nvn_texture, int *format, uint32_t *width, uint32_t *height);
/* Writes RGBA8 pixels (R,G,B,A byte order) into level 0 of the texture, converting to its 8-bit RGBA/BGRA layout and block-linear
 * swizzling it into the guest's storage (dirty tracking notices). Clipped to the texture size. 0 on success. */
int napple_gfx_texture_write_rgba(void *nvn_texture, const void *rgba, size_t src_pitch, uint32_t width, uint32_t height);

/* ---- diagnostics ------------------------------------------------------------------------------------------------ */
typedef struct napple_gfx_stats {
    uint32_t size;
    uint64_t frames, draws_total, dispatches_total, submits_total;
    uint64_t pipelines_compiled, pipelines_cached_hit, shaders_translated, shader_cache_disk_hits;
    uint64_t shaders_pending_async, draws_skipped_pending_shader;
    uint64_t unimplemented_calls_total;    /* total calls to unimplemented NVN entry points */
    uint32_t unimplemented_distinct;       /* number of distinct unimplemented entry points hit */
    uint64_t gpu_ms_last_frame_x1000;
    uint32_t prewarm_done, prewarm_total;
    uint32_t compiles_blocking;            /* the game is waiting on this many shader/pipeline compiles right now */
    uint32_t pad0;
    uint64_t pipelines_archive_hits;       /* pipelines loaded precompiled from the Metal binary archive */
} napple_gfx_stats;
void napple_gfx_get_stats(napple_gfx_stats *out);

typedef struct napple_gfx_diag_entry { const char *name; uint64_t calls; } napple_gfx_diag_entry;
/* Distinct unimplemented NVN entry points that the guest actually called (name as in nvn_names, calls = hit count).
 * Copies up to max entries, returns the total distinct count. The name pointers are static. */
size_t napple_gfx_diag_unimplemented(napple_gfx_diag_entry *out, size_t max);
/* Looked-up-but-unknown names (nvnDeviceGetProcAddress with a name this build does not know). */
size_t napple_gfx_diag_unknown_lookups(napple_gfx_diag_entry *out, size_t max);
/* Print the report above at log level INFO (call at exit or from a debug key). */
void napple_gfx_diag_dump(void);

#ifdef __cplusplus
}
#endif
#endif
