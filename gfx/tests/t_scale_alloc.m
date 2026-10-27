/* t_scale_alloc.m - what mtl_scale_pick_size really allocates (headless Metal init): sizes for factors 0.5 / 1 / 1.5 / 2 / 4 / custom 1.37, which textures count as
 * screen sized (shadow maps, small targets, CPU-filled and mipped textures stay as they are), NVN_TEXTURE_FLAGS_IMAGE on a GPU-written target, and the per-title
 * table (skip / force / disable / max_scale) layered on top. */
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../include/napple_gfx.h"
#import "mtl_display.h"
#import "internal.h"
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static texsnap mk(uint32_t w, uint32_t h, uint32_t flags) {
    texsnap s;
    memset(&s, 0, sizeof s);
    s.d.width = w; s.d.height = h; s.d.depth = 1; s.d.levels = 1; s.d.target = 1; s.d.flags = flags; s.valid = 1;
    return s;
}
static void set_scale(float f) {
    napple_display_settings ds; napple_display_default(&ds);
    ds.render_mode = NAPPLE_RENDER_SCALE; ds.scale = f; napple_display_set(&ds);
    usleep(1000);
    mtl_scale_tick();      /* the first change after note_window is applied at once (g_force); later ones after the 300 ms debounce */
    usleep(350000);
    mtl_scale_tick();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    napple_gfx_config cfg; napple_gfx_config_default(&cfg); cfg.headless_width = 1280; cfg.headless_height = 720;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    napple_display_set_window(1280, 720, 60);
    mtl_scale_note_window(1920, 1080);
    const float f[] = {0.5f, 1.0f, 1.5f, 2.0f, 4.0f, 1.37f};
    for (unsigned i = 0; i < sizeof f / sizeof *f; i++) {
        set_scale(f[i]);
        mtl_scale_pick g;
        texsnap rt = mk(1920, 1080, 0x1);
        mtl_scale_pick_size(&rt, 0, 1, &g);
        uint32_t ew = (uint32_t)lroundf(1920 * f[i]), eh = (uint32_t)lroundf(1080 * f[i]);
        CHECK(g.w == ew && g.h == eh, "x%g: 1920x1080 target -> %ux%u (want %ux%u)", f[i], g.w, g.h, ew, eh);
        CHECK(fabsf(g.rx - (float)ew / 1920) < 1e-4f && fabsf(g.ry - (float)eh / 1080) < 1e-4f, "x%g: rscale entry %g %g", f[i], g.rx, g.ry);
        texsnap half = mk(960, 540, 0); mtl_scale_pick_size(&half, 1, 1, &g);
        CHECK(g.w == (uint32_t)lroundf(960 * f[i]) && g.h == (uint32_t)lroundf(540 * f[i]), "x%g: a half size depth target scales too (%ux%u)", f[i], g.w, g.h);
        if (f[i] == 1.0f) CHECK(g.rx == 1 && g.ry == 1, "1x leaves targets alone");
        /* things that must not scale */
        texsnap sh = mk(2048, 2048, 0x1); mtl_scale_pick_size(&sh, 1, 1, &g); CHECK(g.w == 2048 && g.h == 2048, "x%g: square shadow map stays", f[i]);
        texsnap sm = mk(64, 36, 0x1); mtl_scale_pick_size(&sm, 0, 1, &g); CHECK(g.w == 64 && g.h == 36, "x%g: small utility target stays", f[i]);
        texsnap big = mk(3840, 2160, 0x1); mtl_scale_pick_size(&big, 0, 1, &g); CHECK(g.w == 3840 && g.h == 2160, "x%g: the game's own 4K target stays", f[i]);
        texsnap cpu = mk(1920, 1080, 0); mtl_scale_pick_size(&cpu, 0, 0, &g); CHECK(g.w == 1920 && g.h == 1080, "x%g: a CPU-filled texture stays", f[i]);
        texsnap mip = mk(1920, 1080, 0x1); mip.d.levels = 4; mtl_scale_pick_size(&mip, 0, 1, &g); CHECK(g.w == 1920, "x%g: mipped stays", f[i]);
        texsnap vid = mk(1920, 1080, 0x2); mtl_scale_pick_size(&vid, 0, 1, &g); CHECK(g.w == 1920, "x%g: video decode target stays", f[i]);
        texsnap img = mk(1920, 1080, 0x4 | 0x8); mtl_scale_pick_size(&img, 0, 1, &g);
        CHECK(f[i] == 1.0f ? g.w == 1920 : g.w == ew, "x%g: IMAGE-flagged GPU-written target scales (%u)", f[i], g.w);
        mtl_scale_pick_size(&img, 0, 0, &g); CHECK(g.w == 1920, "x%g: IMAGE-flagged CPU-filled texture stays", f[i]);
    }
    /* per-title table: the headless title id is 0000000000000000 */
    set_scale(2.0f);
    mtl_scale_pick g;
    const char *t1 = "{\"titles\":{\"0000000000000000\":{\"skip\":[[1920,1080]],\"force\":[[640,360]]}}}";
    mtl_scale_table_merge(t1, strlen(t1));
    texsnap rt = mk(1920, 1080, 0x1); mtl_scale_pick_size(&rt, 0, 1, &g); CHECK(g.w == 1920 && g.h == 1080, "table skip");
    texsnap sm = mk(640, 360, 0); mtl_scale_pick_size(&sm, 0, 0, &g); CHECK(g.w == 1280 && g.h == 720, "table force scales a CPU-side 640x360 plain target (%ux%u)", g.w, g.h);
    const char *t2 = "{\"titles\":{\"0000000000000000\":{\"max_scale\":1.5}}}";
    mtl_scale_table_merge(t2, strlen(t2));
    mtl_scale_pick_size(&rt, 0, 1, &g); CHECK(g.w == 2880 || g.w == 2880u, "table max_scale 1.5 caps a x2 render (%u)", g.w);
    const char *t3 = "{\"titles\":{\"0000000000000000\":{\"disable\":true}}}";
    mtl_scale_table_merge(t3, strlen(t3));
    mtl_scale_pick_size(&rt, 0, 1, &g); CHECK(g.w == 1920 && g.rx == 1, "table disable");
    mtl_scale_table_reset();
    mtl_scale_pick_size(&rt, 0, 1, &g); CHECK(g.w == 3840, "no rule: x2 again (%u)", g.w);
    printf(g_fail ? "t_scale_alloc FAILED (%d)\n" : "t_scale_alloc ok\n", g_fail);
    return g_fail != 0;
}
