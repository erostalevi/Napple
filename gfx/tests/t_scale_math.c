/* t_scale_math.c - render scale maths (pure C): factors 0.5 / 1 / 1.5 / 2 / 4 / custom 1.37, allocated sizes, viewport and scissor conversion, the
 * rscale table, aspect fit rectangles (21:9, 16:10, 4:3, 9:16), upscaler fallback and the memory budget. */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../include/napple_display.h"
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)
static int near(float a, float b) { return fabsf(a - b) < 0.002f; }

/* the conversions mtl_replay.m applies per axis (viewport / scissor origin and size, fragment position table entry) */
static void conv(float s, int v0, int vsz, int *o0, int *osz) { *o0 = (int)lroundf((float)v0 * s); *osz = (int)lroundf((float)vsz * s); }
static uint32_t alloc_dim(uint32_t guest, float s) { uint32_t n = (uint32_t)lroundf((float)guest * s); return n < 1 ? 1 : n > 16384 ? 16384 : n; }

/* the fit code of mtl_present_compose before it was moved into napple_display_fit_rect (verbatim arithmetic): the refactor must not move a pixel at 1x */
static void old_fit(uint32_t fit, float img_asp, uint32_t tw, uint32_t th, uint32_t *dx, uint32_t *dy, uint32_t *dw, uint32_t *dh, float *keep_x, float *keep_y) {
    *dx = 0; *dy = 0; *dw = tw; *dh = th; *keep_x = *keep_y = 1;
    float tgt_asp = (float)tw / (float)th;
    if (fit == NAPPLE_FIT_LETTERBOX) {
        if (fabsf(img_asp - tgt_asp) > 0.001f) {
            if (img_asp > tgt_asp) { *dw = tw; *dh = (uint32_t)lroundf((float)tw / img_asp); }
            else { *dh = th; *dw = (uint32_t)lroundf((float)th * img_asp); }
            if (*dw < 1) *dw = 1; if (*dh < 1) *dh = 1; if (*dw > tw) *dw = tw; if (*dh > th) *dh = th;
            *dx = (tw - *dw) / 2; *dy = (th - *dh) / 2;
        }
    } else if (fit == NAPPLE_FIT_FILL && fabsf(img_asp - tgt_asp) > 0.001f) {
        if (img_asp > tgt_asp) *keep_x = tgt_asp / img_asp; else *keep_y = img_asp / tgt_asp;
    }
}

int main(void) {
    napple_display_settings s; napple_display_default(&s);
    const float factors[] = {0.5f, 1.0f, 1.5f, 2.0f, 4.0f, 1.37f};
    s.render_mode = NAPPLE_RENDER_SCALE;
    for (unsigned i = 0; i < sizeof factors / sizeof *factors; i++) {
        float sx, sy, f = factors[i];
        s.scale = f; napple_display_set(&s);
        napple_display_settings g; napple_display_get(&g);
        CHECK(near(g.scale, f), "scale %g survives set/get (%g)", f, g.scale);
        napple_display_compute_scale(&g, 1920, 1080, 3840, 2160, 16384, 0, &sx, &sy);
        CHECK(near(sx, f) && near(sy, f), "factor %g -> %g %g", f, sx, sy);
        uint32_t w = alloc_dim(1920, sx), h = alloc_dim(1080, sy);
        CHECK(w == (uint32_t)lroundf(1920 * f) && h == (uint32_t)lroundf(1080 * f), "allocated %ux%u for x%g", w, h, f);
        /* viewport (0,0,1920,1080) covers the whole scaled target; a quarter viewport stays a quarter */
        int o0, osz; conv(sx, 0, 1920, &o0, &osz); CHECK(o0 == 0 && (uint32_t)osz == w, "full viewport x%g", f);
        conv(sx, 960, 960, &o0, &osz); CHECK(o0 + osz == (int)w || o0 + osz == (int)w + 0, "right half viewport ends at the target edge x%g (%d+%d vs %u)", f, o0, osz, w);
        conv(sy, 270, 270, &o0, &osz); CHECK(abs(o0 - (int)lroundf(270 * f)) <= 1 && abs(osz - (int)lroundf(270 * f)) <= 1, "scissor y x%g", f);
        /* rscale table entry: scaled size / guest size is what the shader multiplies the fragment position by, so pos / table = guest pixels */
        float rx = (float)w / 1920.0f; CHECK(fabsf(rx - f) < 0.002f, "rscale table x%g = %g", f, rx);
    }
    /* percent grid 10..800: the settings range */
    for (int pct = 10; pct <= 800; pct += 10) {
        float sx, sy; s.scale = pct / 100.0f; napple_display_set(&s); napple_display_get(&s);
        napple_display_compute_scale(&s, 1280, 720, 0, 0, 16384, 1ull << 40, &sx, &sy);
        CHECK(near(sx, pct / 100.0f), "custom %d%% -> %g", pct, sx);
    }
    s.scale = 0.01f; napple_display_set(&s); napple_display_get(&s); CHECK(near(s.scale, 0.1f), "below 10%% clamps to 10%%");
    s.scale = 50; napple_display_set(&s); napple_display_get(&s); CHECK(near(s.scale, 8.0f), "above 800%% clamps to 800%%");

    /* "target output 4K": render height follows the target, any base */
    napple_display_default(&s); s.render_mode = NAPPLE_RENDER_FIXED; s.fixed_w = 3840; s.fixed_h = 2160;
    float sx, sy;
    napple_display_compute_scale(&s, 1920, 1080, 1280, 720, 16384, 0, &sx, &sy); CHECK(near(sx, 2) && near(sy, 2), "4K target on 1080p base = 2x");
    napple_display_compute_scale(&s, 1280, 720, 1280, 720, 16384, 0, &sx, &sy); CHECK(near(sx, 3) && near(sy, 3), "4K target on 720p base = 3x");
    s.fixed_w = 2560; s.fixed_h = 1440; s.fit = NAPPLE_FIT_STRETCH; napple_display_compute_scale(&s, 1920, 1080, 0, 0, 16384, 0, &sx, &sy);
    CHECK(near(sx, 1.3333f) && near(sy, 1.3333f), "custom WxH 2560x1440");

    for (uint32_t up = NAPPLE_UP_METALFX; up <= NAPPLE_UP_CAS; up++) {   /* every upscaler renders below the window and brings it up */
        napple_display_default(&s); s.render_mode = NAPPLE_RENDER_WINDOW; s.upscaler = up; s.upscale_factor = 1.5f;
        napple_display_compute_scale(&s, 1920, 1080, 3840, 2160, 16384, 0, &sx, &sy); CHECK(near(sx, 1.3333f) && near(sy, 1.3333f), "upscaler %u: 4K window renders at 1440p (%g)", up, sx);
    }
    /* budget: supersampling is given up first, then the scale; never above the budget and never below the minimum */
    napple_display_default(&s); s.render_mode = NAPPLE_RENDER_SCALE; s.scale = 4; s.supersample = 4;
    uint64_t budget = napple_display_budget_pixels(8ull << 30);
    napple_display_compute_scale(&s, 1920, 1080, 3840, 2160, 16384, budget, &sx, &sy);
    CHECK((double)sx * 1920 * sy * 1080 <= (double)budget * 1.001, "4x + 4x SSAA within the budget (%g)", sx);
    CHECK(sx >= 2.0f, "SSAA is dropped before the scale falls below 2x (%g)", sx);
    s.scale = 1; s.supersample = 2; napple_display_compute_scale(&s, 1920, 1080, 0, 0, 16384, budget, &sx, &sy); CHECK(near(sx, 2), "2x SSAA fits and is kept");
    uint64_t tiny = napple_display_budget_pixels(1ull << 28);
    CHECK(tiny >= (4ull << 20) && tiny <= (48ull << 20), "budget bounds (%llu)", (unsigned long long)tiny);
    CHECK(napple_display_budget_pixels(0) == (48ull << 20) && napple_display_budget_pixels(64ull << 30) == (48ull << 20), "budget caps");
    s.scale = 8; s.supersample = 4; napple_display_compute_scale(&s, 1920, 1080, 0, 0, 16384, 0, &sx, &sy);
    CHECK(sx * 1920 <= 16384.01f && (double)sx * 1920 * sy * 1080 <= (48ull << 20) * 1.001, "never crashes the allocator: 8x + 4x SSAA caps (%g)", sx);

    /* aspect fit rectangles */
    struct { const char *n; float asp; uint32_t tw, th; } t[] = {
        {"16:9 into 21:9", 16.0f / 9, 3440, 1440}, {"16:9 into 16:10", 16.0f / 9, 1920, 1200}, {"16:9 into 4:3", 16.0f / 9, 1024, 768},
        {"16:9 into 9:16", 16.0f / 9, 1080, 1920}, {"21:9 into 16:9", 21.0f / 9, 1920, 1080}, {"9:16 into 16:9", 9.0f / 16, 1920, 1080}};
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        napple_fit_rect r;
        napple_display_fit_rect(NAPPLE_FIT_LETTERBOX, t[i].asp, t[i].tw, t[i].th, 1920, 1080, &r);
        CHECK(r.dw <= t[i].tw && r.dh <= t[i].th, "%s: inside", t[i].n);
        CHECK(fabsf((float)r.dw / (float)r.dh - t[i].asp) < 0.01f * t[i].asp + 0.01f, "%s: aspect kept (%ux%u)", t[i].n, r.dw, r.dh);
        CHECK(r.dx * 2 + r.dw == t[i].tw || r.dx * 2 + r.dw + 1 == t[i].tw, "%s: centred in x", t[i].n);
        CHECK(r.dy * 2 + r.dh == t[i].th || r.dy * 2 + r.dh + 1 == t[i].th, "%s: centred in y", t[i].n);
        CHECK(r.dw == t[i].tw || r.dh == t[i].th, "%s: touches two sides", t[i].n);
        napple_display_fit_rect(NAPPLE_FIT_STRETCH, t[i].asp, t[i].tw, t[i].th, 1920, 1080, &r);
        CHECK(r.dx == 0 && r.dy == 0 && r.dw == t[i].tw && r.dh == t[i].th && r.keep_x == 1 && r.keep_y == 1, "%s: stretch", t[i].n);
        napple_display_fit_rect(NAPPLE_FIT_FILL, t[i].asp, t[i].tw, t[i].th, 1920, 1080, &r);
        CHECK(r.dw == t[i].tw && r.dh == t[i].th && (r.keep_x < 1) != (r.keep_y < 1 ) , "%s: fill crops one axis (%g %g)", t[i].n, r.keep_x, r.keep_y);
        float kasp = t[i].asp * r.keep_x / r.keep_y; CHECK(fabsf(kasp - (float)t[i].tw / t[i].th) < 0.01f, "%s: fill crop matches the target aspect", t[i].n);
    }
    napple_fit_rect r;
    napple_display_fit_rect(NAPPLE_FIT_INTEGER, 16.0f / 9, 3840, 2160, 1280, 720, &r); CHECK(r.dw == 3840 && r.dh == 2160, "integer 3x");
    napple_display_fit_rect(NAPPLE_FIT_INTEGER, 16.0f / 9, 3000, 2000, 1280, 720, &r); CHECK(r.dw == 2560 && r.dh == 1440 && r.dx == 220 && r.dy == 280, "integer 2x with bars");
    napple_display_fit_rect(NAPPLE_FIT_INTEGER, 16.0f / 9, 1000, 600, 1280, 720, &r); CHECK(r.dw == 1000 && r.dh == 563, "integer falls back to letterbox when 1x does not fit (%ux%u)", r.dw, r.dh);
    napple_display_fit_rect(NAPPLE_FIT_LETTERBOX, 1.0f, 0, 0, 0, 0, &r); CHECK(r.dw == 0, "empty target");

    /* parity with the pre-refactor present code over many sizes and image aspects */
    {
        static const uint32_t sizes[] = {1280, 720, 1920, 1080, 2560, 1440, 3840, 2160, 3440, 1440, 2560, 1080, 1920, 1200, 1024, 768, 720, 1280, 1080, 1920, 1000, 1000, 641, 479, 64, 64, 16384, 8192};
        static const float asps[] = {16.0f / 9, 4.0f / 3, 21.0f / 9, 1.0f, 9.0f / 16, 1.6f, 2.39f, 1.78f};
        int n = 0, bad = 0;
        for (unsigned i = 0; i + 1 < sizeof sizes / sizeof *sizes; i += 2)
            for (unsigned a = 0; a < sizeof asps / sizeof *asps; a++)
                for (uint32_t fit = 0; fit <= NAPPLE_FIT_FILL; fit++) {
                    uint32_t dx, dy, dw, dh; float kx, ky; napple_fit_rect fr;
                    old_fit(fit, asps[a], sizes[i], sizes[i + 1], &dx, &dy, &dw, &dh, &kx, &ky);
                    if (fit == NAPPLE_FIT_STRETCH) { dx = dy = 0; dw = sizes[i]; dh = sizes[i + 1]; kx = ky = 1; }
                    napple_display_fit_rect(fit, asps[a], sizes[i], sizes[i + 1], 1920, 1080, &fr);
                    n++; bad += !(fr.dx == dx && fr.dy == dy && fr.dw == dw && fr.dh == dh && fr.keep_x == kx && fr.keep_y == ky);
                }
        CHECK(!bad, "fit rectangle parity with the previous present code: %d of %d differ", bad, n);
    }
    /* upscaler selection and fallback */
    const char *why = NULL;
    CHECK(napple_display_effective_upscaler(NAPPLE_UP_METALFX, 1, 0, &why) == NAPPLE_UP_METALFX && !why, "MetalFX granted");
    CHECK(napple_display_effective_upscaler(NAPPLE_UP_METALFX, 0, 0, &why) == NAPPLE_UP_FSR && why && strstr(why, "built-in"), "MetalFX absent -> built-in, says so");
    CHECK(napple_display_effective_upscaler(NAPPLE_UP_METALFX, 1, 1, &why) == NAPPLE_UP_METALFX, "temporal inputs do not change the spatial choice");
    CHECK(napple_display_effective_upscaler(NAPPLE_UP_CAS, 0, 0, NULL) == NAPPLE_UP_CAS && napple_display_effective_upscaler(NAPPLE_UP_NONE, 0, 0, NULL) == 0, "others unchanged");
    CHECK(napple_display_effective_upscaler(99, 1, 0, NULL) == NAPPLE_UP_NONE, "junk -> none");
    napple_display_default(&s); s.upscaler = 3; napple_display_set(&s); napple_display_get(&s); CHECK(s.upscaler == 3, "CAS kept");
    s.upscaler = 9; napple_display_set(&s); napple_display_get(&s); CHECK(s.upscaler == 0, "junk upscaler rejected");
    /* HUD-safe fallback: a title marked "report_aspect": false is always told its native size */
    {
        uint32_t w, h; napple_display_default(&s); s.report_window_aspect = 1; napple_display_set(&s); napple_display_set_window(2560, 1080, 60);
        napple_display_game_size(1920, 1080, &w, &h); CHECK(w == 2560 && h == 1080, "21:9 reported (%ux%u)", w, h);
        napple_display_set_report_aspect_allowed(0); napple_display_game_size(1920, 1080, &w, &h); CHECK(w == 1920 && h == 1080, "policy off: native");
        napple_display_set_report_aspect_allowed(1); napple_display_game_size(1920, 1080, &w, &h); CHECK(w == 2560, "policy on again");
        napple_display_set_window(720, 1280, 60); napple_display_game_size(1920, 1080, &w, &h); CHECK(h == 1080 && w == 608, "portrait window reports a narrow screen (%ux%u)", w, h);
    }
    printf(g_fail ? "t_scale_math FAILED (%d)\n" : "t_scale_math ok\n", g_fail);
    return g_fail != 0;
}
