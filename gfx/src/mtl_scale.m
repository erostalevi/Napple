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
        uint64_t budget = 48ull << 20;
        if (g_mtl.dev) {
            uint64_t ws = g_mtl.dev.recommendedMaxWorkingSetSize;
            if (ws / 220 < budget) budget = ws / 220;
        }
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
    if (d->target != 1 && d->target != 7 && d->target != 5) return; /* plain 2D (and multisampled 2D) */
    if (d->levels > 1 || d->depth > 1) return;
    if (d->flags & (0x2 | 0x4 | 0x10 | 0x20)) return; /* video decode, image bound, linear, sparse */
    uint32_t w = out->w, h = out->h, div = ds.rscale_min_div ? ds.rscale_min_div : 8;
    if (w > bw + 16 || h > bh + 16) return; /* larger than the screen: the game's own supersampling */
    if (w * div < bw || h * div < bh) return; /* small utility targets */
    float asp = (float)w / (float)h, basp = (float)bw / (float)bh;
    if (fabsf(asp - basp) > 0.04f * basp) return; /* shadow maps and other differently shaped targets */
    if (!for_gpu_write && !is_depth && !(d->flags & 0x1)) return; /* a texture nothing renders to (yet) stays as it is */
    uint32_t nw = (uint32_t)lroundf((float)w * sx), nh = (uint32_t)lroundf((float)h * sy);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > 16384) nw = 16384;
    if (nh > 16384) nh = 16384;
    out->w = nw;
    out->h = nh;
    out->rx = (float)nw / (float)w;
    out->ry = (float)nh / (float)h;
}
