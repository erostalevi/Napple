/* mtl_scale.m - render target scaling ("internal resolution", supersampling, follow-window rendering).
 *
 * The game allocates its screen-sized render targets at the size it believes the display has (1920x1080 docked). With scaling on, those textures are
 * really created at (w * sx, h * sy) and everything that talks about their pixels is converted: viewports and scissors (mtl_replay.m), texel fetch
 * coordinates, texture size queries and the fragment position inside shaders (through the `rscale` table the translator already reads), copies
 * between textures, and the final present. The guest never sees a size other than its own, so UBO texel-size constants stay valid.
 *
 * Which textures are "screen sized" is decided once per texture (see mtl_scale_pick_size) from its guest size against the game's output size (the
 * swapchain textures), its aspect ratio, its flags and whether the GPU renders to it; shadow maps (square), cube / array / mipped / image-bound textures and
 * everything the CPU fills (video frames, UI atlases) keep their size. The factors follow the display settings and the window size; a change is applied
 * after the window size was stable for 300 ms (a live resize drag does not rebuild every target per pixel) by bumping a generation: scaled textures are
 * rebuilt the next time they are used. Per game escape hatches: settings.rscale = 0 (or NAPPLE_RSCALE=0), settings.rscale_min_div. */
#include <math.h>
#include <pthread.h>
#import "mtl_display.h"

static _Atomic uint32_t g_gen = 1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static float g_sx = 1, g_sy = 1; /* applied */
static float g_pend_x = 1, g_pend_y = 1;
static uint64_t g_pend_since;
static uint32_t g_base_w, g_base_h; /* the game's output size (swapchain textures) */
static int g_force;
static _Atomic uint64_t g_scaled_bytes;
static _Atomic uint32_t g_scaled_n, g_total_n;

uint32_t mtl_scale_generation(void) { return atomic_load(&g_gen); }

void mtl_scale_note_window(uint32_t w, uint32_t h) {
    if (w < 64 || h < 64) return;
    pthread_mutex_lock(&g_mu);
    if (w != g_base_w || h != g_base_h) {
        g_base_w = w;
        g_base_h = h;
        g_force = 1;
        LOGI("display: the game's output size is %ux%u", w, h);
    }
    pthread_mutex_unlock(&g_mu);
}

static int env_off(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("NAPPLE_RSCALE");
        v = e && !strcmp(e, "0");
    }
    return v;
}

void mtl_scale_tick(void) {
    napple_display_settings ds;
    napple_display_get(&ds);
    uint32_t ww, wh;
    double hz;
    display_window_get(&ww, &wh, &hz);
    pthread_mutex_lock(&g_mu);
    float sx = 1, sy = 1;
    if (ds.rscale && !env_off() && g_base_w) {
        uint64_t budget = napple_display_budget_pixels(g_mtl.dev ? g_mtl.dev.recommendedMaxWorkingSetSize : 0);
        napple_display_compute_scale(&ds, g_base_w, g_base_h, ww, wh, 16384, budget, &sx, &sy);
    }
    uint64_t now = gfx_now_ns();
    if (sx != g_pend_x || sy != g_pend_y) {
        g_pend_x = sx;
        g_pend_y = sy;
        g_pend_since = now;
    }
    int apply = (g_pend_x != g_sx || g_pend_y != g_sy) && (g_force || now - g_pend_since > 300000000ull || atomic_load(&g_scaled_n) == 0);
    if (apply) {
        g_sx = g_pend_x;
        g_sy = g_pend_y;
        atomic_fetch_add(&g_gen, 1);
        LOGI("display: render targets scaled x%.3f / x%.3f (output %ux%u, window %ux%u)", g_sx, g_sy, g_base_w, g_base_h, ww, wh);
    }
    g_force = 0;
    pthread_mutex_unlock(&g_mu);
}

void mtl_scale_count(int scaled, uint64_t bytes) {
    atomic_fetch_add(&g_total_n, 1);
    if (scaled) {
        atomic_fetch_add(&g_scaled_n, 1);
        atomic_fetch_add(&g_scaled_bytes, bytes);
    }
    g_display_stats.rt_scaled = atomic_load(&g_scaled_n);
    g_display_stats.rt_total = atomic_load(&g_total_n);
    g_display_stats.rt_scaled_bytes = atomic_load(&g_scaled_bytes);
}

/* NAPPLE_SCALE_DIAG=1: every distinct (size, kind, decision) once, for the per-title audit (docs/architecture/gfx.md, "Render scale") */
static void diag_note(const napple_texdesc *d, int is_depth, int gpu, const char *why, uint32_t nw, uint32_t nh) {
    static int on = -1;
    if (on < 0) on = getenv("NAPPLE_SCALE_DIAG") != NULL;
    if (!on) return;
    static uint64_t seen[256];
    static int n;
    uint64_t k = (uint64_t)d->width | (uint64_t)d->height << 16 | (uint64_t)is_depth << 32 | (uint64_t)gpu << 33 | (uint64_t)(d->target & 15) << 34 |
                 (uint64_t)(d->levels > 1) << 38 | ((uint64_t)(uintptr_t)why >> 3 & 0xFFFFF) << 40;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < n; i++)
        if (seen[i] == k) {
            pthread_mutex_unlock(&g_mu);
            return;
        }
    if (n < 256) seen[n++] = k;
    pthread_mutex_unlock(&g_mu);
    LOGI("scale-diag: target %ux%u%s%s target=%u levels=%u samples=%u fmt=%u flags=0x%x -> %ux%u (%s)", d->width, d->height, is_depth ? " depth" : "", gpu ? " gpu-written" : "",
         d->target, d->levels, d->samples, d->format, d->flags, nw, nh, why);
}

void mtl_scale_pick_size(const texsnap *s, int is_depth, int for_gpu_write, mtl_scale_pick *out) {
    const napple_texdesc *d = &s->d;
    out->w = d->width ? d->width : 1;
    out->h = d->height ? d->height : 1;
    out->rx = out->ry = 1;
    pthread_mutex_lock(&g_mu);
    float sx = g_sx, sy = g_sy;
    uint32_t bw = g_base_w, bh = g_base_h;
    pthread_mutex_unlock(&g_mu);
    if ((sx == 1 && sy == 1) || !bw) return;
    napple_display_settings ds;
    napple_display_get(&ds);
    if (!ds.rscale) return;
    const mtl_scale_rule *rule = mtl_scale_table_active();
    uint32_t w = out->w, h = out->h, div = ds.rscale_min_div ? ds.rscale_min_div : 8;
    int forced = 0;
    if (rule) {
        if (rule->disable) return;
        if (rule->min_div) div = rule->min_div;
        if (rule->max_scale > 0) {
            if (sx > rule->max_scale) sx = rule->max_scale;
            if (sy > rule->max_scale) sy = rule->max_scale;
        }
        if (mtl_scale_rule_has(rule->force, rule->nforce, w, h)) forced = 1;
    }
#define KEEP(why) do { diag_note(d, is_depth, for_gpu_write, why, w, h); return; } while (0)
    if (d->target != 1 && d->target != 7 && d->target != 5) KEEP("not a plain 2D target"); /* plain 2D (and multisampled 2D) */
    if (d->levels > 1 || d->depth > 1) KEEP("mipped or layered");
    if (d->flags & (0x2 | 0x10 | 0x20)) KEEP("video decode, linear or sparse");
    /* NVN_TEXTURE_FLAGS_IMAGE: engines set it on most of their render targets (LAS, SMO and ACNH do, for every screen sized target), so it cannot veto the scaling
     * by itself; a texture that is only filled by the CPU and image bound keeps its size (its image coordinates are not converted) */
    if ((d->flags & 0x4) && !for_gpu_write && !is_depth) KEEP("image bound and not rendered to");
    if (rule && mtl_scale_rule_has(rule->skip, rule->nskip, w, h)) KEEP("per-title table: skip");
    float asp = (float)w / (float)h, basp = (float)bw / (float)bh;
    if (!forced) {
        if (w > bw + 16 || h > bh + 16) KEEP("larger than the screen (the game's own supersampling)"); /* larger than the screen */
        if (w * div < bw || h * div < bh) KEEP("small utility target");
        if (fabsf(asp - basp) > 0.04f * basp) KEEP("different shape (shadow map or similar)"); /* shadow maps and other differently shaped targets */
        if (!for_gpu_write && !is_depth && !(d->flags & 0x1)) KEEP("not rendered to (yet)"); /* a texture nothing renders to (yet) stays as it is */
    }
    if (sx == 1 && sy == 1) return;
    uint32_t nw = (uint32_t)lroundf((float)w * sx), nh = (uint32_t)lroundf((float)h * sy);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > 16384) nw = 16384;
    if (nh > 16384) nh = 16384;
    out->w = nw;
    out->h = nh;
    out->rx = (float)nw / (float)w;
    out->ry = (float)nh / (float)h;
    diag_note(d, is_depth, for_gpu_write, forced ? "scaled (per-title table: force)" : "scaled", nw, nh);
#undef KEEP
}
