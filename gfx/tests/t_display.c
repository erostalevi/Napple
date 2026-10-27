/* t_display.c - display settings store and the scale policy (pure maths, no Metal): render size modes, aspect handling, supersampling, device budget,
 * the size reported to the game. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../include/napple_display.h"
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)
static int near(float a, float b) { return fabsf(a - b) < 0.002f; }

int main(void) {
    napple_display_settings s; napple_display_default(&s);
    float sx, sy;
    napple_display_compute_scale(&s, 1920, 1080, 2560, 1440, 0, 0, &sx, &sy);
    CHECK(sx == 1 && sy == 1, "native mode ignores the window (%g %g)", sx, sy);
    s.render_mode = NAPPLE_RENDER_SCALE; s.scale = 1.5f;
    napple_display_compute_scale(&s, 1920, 1080, 800, 600, 0, 0, &sx, &sy);
    CHECK(near(sx, 1.5f) && near(sy, 1.5f), "scale factor mode");
    s.scale = 1; s.supersample = 2;
    napple_display_compute_scale(&s, 1920, 1080, 800, 600, 0, 0, &sx, &sy);
    CHECK(near(sx, 2) && near(sy, 2), "2x supersampling on a native size renders 3840x2160");
    /* follow window */
    s.render_mode = NAPPLE_RENDER_WINDOW; s.supersample = 1; s.fit = NAPPLE_FIT_LETTERBOX;
    napple_display_compute_scale(&s, 1920, 1080, 2560, 1440, 0, 0, &sx, &sy);
    CHECK(near(sx, 1.3333f) && near(sy, 1.3333f), "1440p window, 16:9: %g", sx);
    napple_display_compute_scale(&s, 1920, 1080, 3440, 1440, 0, 0, &sx, &sy);
    CHECK(near(sx, 1.3333f) && near(sy, 1.3333f), "ultrawide window, letterbox: limited by the height (%g)", sx);
    s.fit = NAPPLE_FIT_FILL;
    napple_display_compute_scale(&s, 1920, 1080, 3440, 1440, 0, 0, &sx, &sy);
    CHECK(near(sx, 1.7917f) && near(sy, 1.7917f), "ultrawide, fill: limited by the width (%g)", sx);
    s.fit = NAPPLE_FIT_STRETCH;
    napple_display_compute_scale(&s, 1920, 1080, 3440, 1440, 0, 0, &sx, &sy);
    CHECK(near(sx, 1.7917f) && near(sy, 1.3333f), "ultrawide, stretch: per axis (%g %g)", sx, sy);
    s.fit = NAPPLE_FIT_LETTERBOX; s.supersample = 2;
    napple_display_compute_scale(&s, 1920, 1080, 2560, 1440, 0, 0, &sx, &sy);
    CHECK(near(sx, 2.6667f), "window x supersample (%g)", sx);
    /* fixed size */
    s.render_mode = NAPPLE_RENDER_FIXED; s.supersample = 1; s.fixed_w = 1280; s.fixed_h = 720;
    napple_display_compute_scale(&s, 1920, 1080, 3440, 1440, 0, 0, &sx, &sy);
    CHECK(near(sx, 0.6667f) && near(sy, 0.6667f), "fixed 1280x720 on a 1080p base (%g)", sx);
    /* MetalFX: render below the target */
    s.render_mode = NAPPLE_RENDER_WINDOW; s.upscaler = 1; s.upscale_factor = 1.5f; s.fit = NAPPLE_FIT_LETTERBOX;
    napple_display_compute_scale(&s, 1920, 1080, 3840, 2160, 0, 0, &sx, &sy);
    CHECK(near(sx, 1.3333f), "upscaler: 4K window renders at 1440p (%g)", sx);
    s.upscaler = 0;
    /* budget: 4x supersampling of a 4K window would be 15360x8640 x ... : capped */
    s.supersample = 4;
    napple_display_compute_scale(&s, 1920, 1080, 3840, 2160, 0, 0, &sx, &sy);
    CHECK(sx * 1920 <= 16384.01f && sy * 1080 <= 16384.01f && (double)sx * 1920 * sy * 1080 <= (48ull << 20) * 1.001, "budget cap (%g x %g)", sx * 1920, sy * 1080);
    CHECK(near(sx, sy), "cap keeps the aspect");
    s.supersample = 1; s.render_mode = NAPPLE_RENDER_SCALE; s.scale = 0.05f; napple_display_set(&s);
    napple_display_get(&s); CHECK(near(s.scale, 0.1f), "set clamps the scale to 10%%");
    napple_display_compute_scale(&s, 1280, 720, 0, 0, 0, 0, &sx, &sy); CHECK(near(sx, 0.1f), "min scale");
    napple_display_compute_scale(&s, 0, 0, 0, 0, 0, 0, &sx, &sy); CHECK(sx == 1 && sy == 1, "no base size -> identity");
    /* 1x exactly */
    s.scale = 1.001f; napple_display_compute_scale(&s, 1920, 1080, 0, 0, 0, 0, &sx, &sy); CHECK(sx == 1 && sy == 1, "1.001 snaps to 1 (untouched targets)");
    /* reported size */
    uint32_t w, h;
    napple_display_default(&s); napple_display_set(&s); napple_display_set_window(3440, 1440, 120);
    napple_display_game_size(1920, 1080, &w, &h); CHECK(w == 1920 && h == 1080, "window aspect not reported by default");
    s.report_window_aspect = 1; napple_display_set(&s);
    napple_display_game_size(1920, 1080, &w, &h); CHECK(h == 1080 && w == 2580, "ultrawide reported as 2580x1080 (got %ux%u)", w, h);
    napple_display_set_window(1000, 1000, 60); napple_display_game_size(1920, 1080, &w, &h); CHECK(h == 1080 && w == 1080, "square window");
    napple_display_set_window(0, 0, 0); napple_display_game_size(1920, 1080, &w, &h); CHECK(w == 1920, "unknown window: native");
    napple_display_stats st; napple_display_get_stats(&st); CHECK(st.size == sizeof st, "stats");
    char t[300]; napple_display_stats_text(t, sizeof t); CHECK(strlen(t) > 10, "stats text");
    printf(g_fail ? "t_display FAILED (%d)\n" : "t_display ok\n", g_fail);
    return g_fail != 0;
}
