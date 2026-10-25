/* gfx_napple.c - adapter between the runtime's graphics plug-in interface (runtime/src/gfx.h) and libnapple_gfx (include/napple_gfx.h).
 * Link this object and libnapple_gfx.a instead of runtime/src/gfx_stub.c (make -C runtime GFX=napple). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "napple_gfx.h"

static GfxHooks g_h;
static void nv_noop(void) {}
static HostSymbol *g_table;
static size_t g_table_n;

static int a_core_away(void *u) { int t = 0; if (g_h.core_away) g_h.core_away(g_h.user, &t); (void)u; return t; }
static void a_core_back(void *u, int *t) { (void)u; if (g_h.core_back) g_h.core_back(g_h.user, t); }
static void *a_wrap(void *u, void *fn, const char *name) { (void)u; return g_h.wrap_guest_callable ? g_h.wrap_guest_callable(g_h.user, fn, name) : fn; }
static void a_present(void *u, uint64_t frame, uint32_t interval) { (void)u; if (g_h.on_present) g_h.on_present(g_h.user, frame, interval); }

int gfx_init(const GfxConfig *cfg, const GfxHooks *hooks) {
    g_h = *hooks;
    napple_gfx_config c;
    napple_gfx_config_default(&c);
    c.metal_layer = cfg->headless ? NULL : cfg->metal_layer;
    if (cfg->output_width && cfg->output_height) { c.headless_width = cfg->output_width; c.headless_height = cfg->output_height; }
    c.title_id = cfg->title_id;
    c.cache_dir = cfg->cache_dir;
    c.nvn_major = cfg->nvn_major; c.nvn_minor = cfg->nvn_minor;
    c.gmem_base = cfg->gmem_base; c.gmem_size = cfg->gmem_size;
    c.hooks.user = NULL;
    c.hooks.core_away = a_core_away;
    c.hooks.core_back = a_core_back;
    c.hooks.wrap_guest_callable = a_wrap;
    c.hooks.on_present = a_present;
    /* NAPPLE_TURBO=<fps>: debug aid that runs the guest faster than real time (virtual vsync and presents at <fps>, default 300), to reach late game states quickly. */
    if (getenv("NAPPLE_TURBO")) { float f = (float)atof(getenv("NAPPLE_TURBO")); c.settings.pacing = NAPPLE_PACING_FIXED; c.settings.target_fps = f > 1 ? f : 300; }
    if (!getenv("NAPPLE_GFX_SKIPINIT") && napple_gfx_init(&c)) return -1;
    return 0;
}
/* The runtime registers the overrides before gfx_init (while it maps the modules), so the table is built on first use. */
const HostSymbol *gfx_hle_table(size_t *count) {
    if (!g_table) {
        size_t n = 0;
        const napple_gfx_hle_entry *e = napple_gfx_hle_table(&n);
        g_table = calloc(n + 1, sizeof *g_table);
        for (size_t i = 0; i < n; i++) g_table[i] = (HostSymbol){ e[i].symbol, e[i].fn };
        g_table_n = n;
        if (getenv("NAPPLE_NV_INIT_NOOP")) g_table[g_table_n++] = (HostSymbol){ "_ZN2nv18InitializeGraphicsEPvm", (void *)nv_noop };
    }
    *count = g_table_n; return g_table;
}
int gfx_handle_fault(void *addr, int is_write, void *uc) { return napple_gfx_handle_fault(addr, is_write, uc); }
void gfx_guest_write_prepare(void *addr, size_t n) { napple_gfx_guest_write_prepare(addr, n); }
uint64_t gfx_vsync_period_ns(void) { return napple_gfx_vsync_period_ns(); }
void gfx_set_nvn_version(unsigned major, unsigned minor) { napple_gfx_set_nvn_version(major, minor); }
void gfx_compile_status(GfxCompileStatus *out) {
    napple_gfx_stats st; st.size = sizeof st; napple_gfx_get_stats(&st);
    out->prewarm_done = st.prewarm_done; out->prewarm_total = st.prewarm_total; out->blocking = st.compiles_blocking; out->pipelines = st.pipelines_compiled;
}
void gfx_set_settings(const GfxSettings *in) {   /* frontend settings -> libnapple_gfx (runtime/src/app.m); only the fields the player controls */
    napple_gfx_settings s; napple_gfx_get_settings(&s);
    s.size = sizeof s;
    s.internal_res_scale = in->internal_scale > 0 ? in->internal_scale : 1.0f;
    s.pacing = in->pacing == 1 ? NAPPLE_PACING_FIXED : in->pacing == 2 ? NAPPLE_PACING_UNCAPPED : NAPPLE_PACING_GAME;
    s.target_fps = in->pacing == 1 ? in->target_fps : 0;
    s.docked = in->docked != 0;
    napple_gfx_set_settings(&s);
}
void gfx_shutdown(void) {
    napple_gfx_stats st; st.size = sizeof st; napple_gfx_get_stats(&st);
    fprintf(stderr, "[napple-gfx] stats: frames %llu draws %llu dispatches %llu submits %llu pipelines %llu (cached %llu) shaders %llu pending %llu skipped draws %llu unimplemented calls %llu\n",
            (unsigned long long)st.frames, (unsigned long long)st.draws_total, (unsigned long long)st.dispatches_total, (unsigned long long)st.submits_total,
            (unsigned long long)st.pipelines_compiled, (unsigned long long)st.pipelines_cached_hit, (unsigned long long)st.shaders_translated,
            (unsigned long long)st.shaders_pending_async, (unsigned long long)st.draws_skipped_pending_shader, (unsigned long long)st.unimplemented_calls_total);
    napple_gfx_diag_dump(); napple_gfx_shutdown();
}
int gfx_screenshot(const char *png_path) { return napple_gfx_screenshot(png_path); }
int gfx_screenshot_pending(void) { return napple_gfx_screenshot_pending(); }
int gfx_texture_info(void *tex, int *format, unsigned *w, unsigned *h) { uint32_t a = 0, b = 0; int r = napple_gfx_texture_info(tex, format, &a, &b); if (w) *w = a; if (h) *h = b; return r; }
int gfx_texture_write_rgba(void *tex, const void *rgba, size_t pitch, unsigned w, unsigned h) { return napple_gfx_texture_write_rgba(tex, rgba, pitch, w, h); }
