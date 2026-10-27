/* display.c - settings store, scale policy and statistics of the display pipeline (pure C, no Metal). See include/napple_display.h. */
#include "../include/napple_display.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static napple_display_settings g_s;
static int g_init;
static int g_report_ok = 1;
static uint32_t g_win_w, g_win_h; static double g_refresh = 60;
napple_display_stats g_display_stats;           /* written by the Metal side (mtl_present.m, mtl_scale.m, mtl_interp.m) */

void napple_display_default(napple_display_settings *s) {
    memset(s, 0, sizeof *s);
    s->size = sizeof *s; s->scale = 1.0f; s->supersample = 1.0f; s->filter = NAPPLE_FILTER_BICUBIC; s->sharpness = 0.3f; s->upscale_factor = 1.5f;
    s->speed = 1.0f; s->rscale = 1; s->rscale_min_div = 8;
}
static void ensure(void) { if (!g_init) { napple_display_default(&g_s); g_init = 1; } }

static float clampf(float v, float lo, float hi) { return !(v >= lo) ? lo : v > hi ? hi : v; }
void napple_display_set(const napple_display_settings *in) {
    napple_display_settings s = *in;
    s.size = sizeof s;
    if (s.render_mode > NAPPLE_RENDER_FIXED) s.render_mode = NAPPLE_RENDER_NATIVE;
    s.scale = clampf(s.scale, 0.1f, 8.0f); s.supersample = clampf(s.supersample, 1.0f, 4.0f);
    s.sharpness = clampf(s.sharpness, 0, 1); s.upscale_factor = clampf(s.upscale_factor, 1.0f, 3.0f);
    if (s.fit > NAPPLE_FIT_INTEGER) s.fit = NAPPLE_FIT_LETTERBOX;
    if (s.filter > NAPPLE_FILTER_SHARP) s.filter = NAPPLE_FILTER_BICUBIC;
    if (s.upscaler > NAPPLE_UP_CAS) s.upscaler = NAPPLE_UP_NONE;
    if (s.anisotropy > 16) s.anisotropy = 16;
    if (s.frame_gen_mult == 1 || s.frame_gen_mult > 8) s.frame_gen_mult = s.frame_gen_mult ? 8 : 0;
    if (s.frame_gen_method > NAPPLE_FG_BLEND) s.frame_gen_method = NAPPLE_FG_AUTO;
    s.speed = clampf(s.speed, 0.1f, 10.0f);
    if (!s.rscale_min_div) s.rscale_min_div = 8;
    if (s.fixed_w < 16 || s.fixed_h < 16) s.fixed_w = s.fixed_h = 0;
    pthread_mutex_lock(&g_mu); g_s = s; g_init = 1; pthread_mutex_unlock(&g_mu);
}
void napple_display_get(napple_display_settings *s) { pthread_mutex_lock(&g_mu); ensure(); *s = g_s; pthread_mutex_unlock(&g_mu); }
void napple_display_set_window(uint32_t w, uint32_t h, double hz) {
    pthread_mutex_lock(&g_mu); g_win_w = w; g_win_h = h; if (hz >= 24 && hz <= 1000) g_refresh = hz; pthread_mutex_unlock(&g_mu);
}
void display_window_get(uint32_t *w, uint32_t *h, double *hz) { pthread_mutex_lock(&g_mu); *w = g_win_w; *h = g_win_h; *hz = g_refresh; pthread_mutex_unlock(&g_mu); }

void napple_display_set_report_aspect_allowed(int allowed) { pthread_mutex_lock(&g_mu); g_report_ok = allowed != 0; pthread_mutex_unlock(&g_mu); }
void napple_display_game_size(uint32_t bw, uint32_t bh, uint32_t *w, uint32_t *h) {
    napple_display_settings s; napple_display_get(&s);
    uint32_t ww, wh; double hz; display_window_get(&ww, &wh, &hz);
    *w = bw; *h = bh;
    if (s.report_window_aspect && g_report_ok && ww >= 64 && wh >= 64) {
        double asp = (double)ww / (double)wh;
        uint32_t nw = (uint32_t)(bh * asp + 0.5) & ~1u;
        if (nw < 16) nw = 16; if (nw > 16384) nw = 16384;
        *w = nw;
    }
}

void napple_display_compute_scale(const napple_display_settings *s, uint32_t bw, uint32_t bh, uint32_t ww, uint32_t wh, uint32_t max_dim, uint64_t max_pixels, float *sx, float *sy) {
    float fx = 1, fy = 1;
    if (!max_dim || max_dim > 16384) max_dim = 16384;
    if (!max_pixels) max_pixels = 48ull << 20;
    if (!bw || !bh) { *sx = *sy = 1; return; }
    uint32_t tw = 0, th = 0;                      /* the target the image is fitted into */
    switch (s->render_mode) {
    case NAPPLE_RENDER_SCALE: fx = fy = s->scale; break;
    case NAPPLE_RENDER_WINDOW: tw = ww; th = wh; break;
    case NAPPLE_RENDER_FIXED: tw = s->fixed_w; th = s->fixed_h; break;
    default: break;
    }
    if (tw && th) {
        float ax = (float)tw / (float)bw, ay = (float)th / (float)bh;
        if (s->fit == NAPPLE_FIT_STRETCH) { fx = ax; fy = ay; }
        else if (s->fit == NAPPLE_FIT_FILL) fx = fy = ax > ay ? ax : ay;
        else fx = fy = ax < ay ? ax : ay;
        if (s->upscaler != NAPPLE_UP_NONE) { fx /= s->upscale_factor; fy /= s->upscale_factor; }   /* render below the target, the upscaler brings it up */
    }
    /* device budget. Degrade gracefully: supersampling is the luxury, so it is given up first (down to 1x), then the render size itself is reduced */
    float ss = s->supersample < 1 ? 1 : s->supersample;
    float lim = (float)max_dim;
    for (;;) {
        float ex = fx * ss, ey = fy * ss;
        double px = (double)ex * bw * (double)ey * bh;
        if (ex * bw <= lim && ey * bh <= lim && px <= (double)max_pixels) break;
        if (ss > 1.0f) { ss = ss > 1.25f ? ss * 0.8f : 1.0f; continue; }
        break;
    }
    fx *= ss; fy *= ss;
    if (fx * bw > lim) fx = lim / (float)bw;
    if (fy * bh > lim) fy = lim / (float)bh;
    double px = (double)fx * bw * (double)fy * bh;
    if (px > (double)max_pixels) { double k = sqrt((double)max_pixels / px); fx = (float)(fx * k); fy = (float)(fy * k); }
    if (fx < 0.1f) fx = 0.1f; if (fy < 0.1f) fy = 0.1f;
    /* 1x stays exactly 1x (a render target of the game's own size is not touched) */
    if (fabsf(fx - 1) < 0.002f) fx = 1; if (fabsf(fy - 1) < 0.002f) fy = 1;
    *sx = fx; *sy = fy;
}

void napple_display_fit_rect(uint32_t fit, float img_asp, uint32_t tw, uint32_t th, uint32_t src_w, uint32_t src_h, napple_fit_rect *r) {
    r->dx = r->dy = 0; r->dw = tw; r->dh = th; r->keep_x = r->keep_y = 1;
    if (!tw || !th || !(img_asp > 0) || fit == NAPPLE_FIT_STRETCH) return;
    float tgt = (float)tw / (float)th;
    if (fabsf(img_asp - tgt) <= 0.001f && fit != NAPPLE_FIT_INTEGER) return;
    if (fit == NAPPLE_FIT_FILL) {
        if (img_asp > tgt) r->keep_x = tgt / img_asp; else r->keep_y = img_asp / tgt;
        return;
    }
    uint32_t w, h;
    if (img_asp > tgt) { w = tw; h = (uint32_t)lroundf((float)tw / img_asp); } else { h = th; w = (uint32_t)lroundf((float)th * img_asp); }
    if (fit == NAPPLE_FIT_INTEGER && src_w && src_h) {
        /* largest whole multiple of the source that fits; when even 1x does not fit, behave like letterbox */
        uint32_t k = tw / src_w < th / src_h ? tw / src_w : th / src_h;
        if (k >= 1) { w = src_w * k; h = src_h * k; }
    }
    if (w < 1) w = 1; if (h < 1) h = 1; if (w > tw) w = tw; if (h > th) h = th;
    r->dw = w; r->dh = h; r->dx = (tw - w) / 2; r->dy = (th - h) / 2;
}

uint32_t napple_display_effective_upscaler(uint32_t req, int metalfx_ok, int temporal_inputs, const char **why) {
    (void)temporal_inputs; /* no title of ours provides depth and motion vectors: a temporal scaler is never selected */
    if (why) *why = NULL;
    if (req == NAPPLE_UP_METALFX && !metalfx_ok) {
        if (why) *why = "MetalFX is not available here: using the built-in upscaler";
        return NAPPLE_UP_FSR;
    }
    if (req > NAPPLE_UP_CAS) return NAPPLE_UP_NONE;
    return req;
}

uint64_t napple_display_budget_pixels(uint64_t ws) {
    uint64_t b = 48ull << 20;                     /* ~48 Mpixel: 8 K is 33 Mpixel */
    if (ws && ws / 220 < b) b = ws / 220;         /* ~220 bytes per pixel over the targets of a typical frame */
    if (b < (4ull << 20)) b = 4ull << 20;
    return b;
}

void napple_display_get_stats(napple_display_stats *out) {
    uint32_t ww, wh; double hz; display_window_get(&ww, &wh, &hz);
    *out = g_display_stats; out->size = sizeof *out; out->window_w = ww; out->window_h = wh; out->refresh_hz = hz;
}
void napple_display_stats_text(char *out, unsigned cap) {
    napple_display_stats st; napple_display_get_stats(&st);
    int n = snprintf(out, cap, "guest %.0f fps  |  present %.0f fps  |  GPU %.1f ms (output %.1f)  |  render %ux%u -> %ux%u", st.game_fps, st.out_fps, st.gpu_ms, st.output_ms,
                     st.render_w, st.render_h, st.window_w, st.window_h);
    if (st.fg_active && n > 0 && (unsigned)n < cap) snprintf(out + n, cap - (unsigned)n, "  |  frame gen %.1f ms, +%.1f ms latency, %llu cuts", st.fg_ms, st.latency_ms, (unsigned long long)st.scene_cuts);
}
