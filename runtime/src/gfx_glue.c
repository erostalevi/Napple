/* Runtime side of the graphics plug-in: hooks handed to the back end. */
#include "gfx.h"
#include "frontend.h"
#include "movie.h"
#include "timescale.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "napple.h"

static void *wrap(void *user, void *fn, const char *name) {
    (void)user;
    return runtime_hle_wrap_named(fn, name);
}
static void away(void *user, int *token) {
    (void)user;
    thread_core_away(token);
}
static void back(void *user, int *token) {
    (void)user;
    thread_core_back(token);
}

/* NAPPLE_SCREENSHOT=<prefix>: write <prefix>-<frame>.png for the frames listed in NAPPLE_SCREENSHOT_FRAMES (comma separated,
 * default every 120th frame, max 20 files). Headless verification of what is really rendered. */
static void maybe_screenshot(uint64_t frame) {
    static const char *prefix;
    static int init, shots;
    static uint64_t list[64];
    static int nlist;
    if (!init) {
        init = 1;
        prefix = getenv("NAPPLE_SCREENSHOT");
        const char *l = getenv("NAPPLE_SCREENSHOT_FRAMES");
        if (l)
            for (char *p = (char *)l; *p && nlist < 64;) {
                list[nlist++] = strtoull(p, &p, 10);
                if (*p == ',') p++;
                else break;
            }
    }
    if (!prefix || shots >= 20) return;
    int hit = 0;
    if (nlist) {
        for (int i = 0; i < nlist; i++)
            if (list[i] == frame) hit = 1;
    } else hit = frame % 120 == 0;
    if (!hit || gfx_screenshot_pending()) return;
    char path[1200];
    snprintf(path, sizeof path, "%s-%llu.png", prefix, (unsigned long long)frame);
    if (gfx_screenshot(path) == 0) {
        shots++;
        LOG("gfx", "screenshot of the frame after %llu -> %s", (unsigned long long)frame, path);
    }
}
static void present(void *user, uint64_t frame, unsigned swap_interval) {
    (void)user;
    LOGV("gfx", "present frame %llu (swap interval %u)", (unsigned long long)frame, swap_interval);
    timescale_count_present();
    frontend_frame_presented();
    movie_host_tick();
    maybe_screenshot(frame);
    /* The frames of a game that an applet has hidden are dropped by the presenter, which also removes its pacing: without this the game spun at
     * 17-20k frames per 60 s behind the Mii Maker. Keep the vsync cadence (60 Hz per swap interval). */
    if (applet_game_hidden()) usleep(16667u * (swap_interval ? swap_interval : 1));
}

const GfxHooks *runtime_gfx_hooks(void) {
    static const GfxHooks hooks = {NULL, present, wrap, away, back};
    return &hooks;
}
