/* Graphics back end plug-in interface (NVN on Metal lives behind this).
 *
 * The runtime never implements NVN itself. The guest reaches its graphics driver through the imports `nvnBootstrapLoader`
 * (and the `nv::` graphics-memory calls); the runtime overrides those imports with whatever the back end lists in
 * gfx_hle_table(), so the SDK's own NVN driver and nvdrv are never executed. The default back end (gfx_stub.c) hands out
 * traps that name each NVN function the guest asks for; a real back end replaces gfx_stub.c at link time (see
 * docs/architecture/gfx.md for the contract the graphics work stream implements, and gfx_napple.c for the adapter).
 *
 * Contract summary:
 *  - gfx_init runs once before the guest starts. `metal_layer` is a CAMetalLayer* (NULL = headless).
 *  - gfx_hle_table lists host replacements of guest imports; the runtime applies them while relocating modules, wraps them so
 *    they run on the calling guest thread's host stack, and so every function pointer the back end later returns to the
 *    guest must go through hooks.wrap_guest_callable.
 *  - Guest memory is identity mapped: a guest address is a host address. The runtime forwards access faults inside guest
 *    memory to gfx_handle_fault first and calls gfx_guest_write_prepare before host-side writes into guest buffers.
 *  - Presentation: the back end calls hooks.on_present for each presented frame; the runtime owns the guest-visible vsync event. */
#pragma once
#include "napple.h"

typedef struct {
    void *metal_layer; /* CAMetalLayer*, NULL when headless */
    const char *title_id, *build_id;
    const char *cache_dir; /* per-game cache directory (shader cache) */
    int headless;
    unsigned output_width, output_height;
    unsigned nvn_major, nvn_minor; /* NVN API version the game expects (data dir game.json); 0,0 = back end default */
    uint64_t gmem_base, gmem_size; /* guest heap window shaders may address with global memory (0 = none) */
} GfxConfig;

typedef struct {
    void *user;
    void (*on_present)(void *user, uint64_t frame, unsigned swap_interval);
    void *(*wrap_guest_callable)(void *user, void *host_fn, const char *name);
    void (*core_away)(void *user, int *token); /* about to block: give the virtual core up */
    void (*core_back)(void *user, int *token);
} GfxHooks;

int gfx_init(const GfxConfig *cfg, const GfxHooks *hooks); /* 0 = ok */
const HostSymbol *gfx_hle_table(size_t *count);
int gfx_handle_fault(void *addr, int is_write, void *ucontext); /* 1 = handled, retry the instruction */
void gfx_guest_write_prepare(void *addr, size_t n);
uint64_t gfx_vsync_period_ns(void);
void gfx_set_nvn_version(unsigned major, unsigned minor); /* API version nvnDeviceGetInteger reports; may follow gfx_init */
void gfx_shutdown(void);
int gfx_screenshot(const char *png_path); /* PNG of the next presented frame (async); 0 = queued */
int gfx_screenshot_pending(void);
void gfx_set_hidden(int hidden); /* frames not shown while a library applet draws on the layer */
/* Shader compile progress for the loading screen (app.m): the launch-time cache compile, compiles the game waits on right now, pipelines built. */
typedef struct {
    unsigned prewarm_done, prewarm_total, blocking;
    unsigned long long pipelines;
} GfxCompileStatus;
void gfx_compile_status(GfxCompileStatus *out);
/* Host video decoders (movie.c): the host fills a texture the game created. Return 0 on success, <0 when the back end has no such texture. */
int gfx_texture_info(void *nvn_texture, int *format, unsigned *width, unsigned *height);
int gfx_texture_write_rgba(void *nvn_texture, const void *rgba, size_t pitch, unsigned width, unsigned height);

/* Player-facing graphics settings (frontend settings.json -> back end), applied at any time. pacing: 0 follow the game (real time, vsync-locked),
 * 1 fixed `target_fps` (also the guest's vsync rate: the game's speed scales with it), 2 uncapped (fast forward). internal_scale: render target
 * multiplier (1 = the game's native size); docked: the console mode the game was told about. */
typedef struct {
    float internal_scale;
    int pacing;
    float target_fps;
    int docked;
} GfxSettings;
void gfx_set_settings(const GfxSettings *s);

/* Runtime side (gfx_glue.c): the hooks every back end receives and the vsync clock. */
const GfxHooks *runtime_gfx_hooks(void);
