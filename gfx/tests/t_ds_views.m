/* t_ds_views.m - sampled views of depth-stencil textures (mtl_tex_view): a D32F_S8 texture sampled in stencil mode (texture's own mode or a view's) is a stencil-only
 * format view (X32_Stencil8), depth mode is the texture itself, a view's mode overrides the texture's, D24S8 (stored as D32F_S8) behaves the same, Depth32F has no
 * stencil plane. Headless Metal, no game data. */
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/napple_gfx.h"
#import "mtl_display.h"
#import "internal.h"
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static texsnap mk(uint32_t fmt, void *mem) {
    texsnap s;
    memset(&s, 0, sizeof s);
    s.d.width = 64; s.d.height = 64; s.d.depth = 1; s.d.levels = 1; s.d.target = 1; s.d.format = fmt; s.d.samples = 1; s.mem = mem; s.size = 64 * 64 * 8; s.valid = 1;
    return s;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    napple_gfx_config cfg; napple_gfx_config_default(&cfg); cfg.headless_width = 256; cfg.headless_height = 256;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    uint8_t *mem = calloc(1, 64 * 64 * 8);
    const uint8_t swz[4] = { 2, 3, 4, 5 };
    view_t none; memset(&none, 0, sizeof none);
    view_t vs = none; vs.set_ds = 1; vs.ds_mode = 1;
    view_t vd = none; vd.set_ds = 1; vd.ds_mode = 0;
    uint32_t fmts[2] = { 0x36 /* D32F_S8 */, 0x35 /* D24_S8 */ };
    for (int i = 0; i < 2; i++) {
        texsnap s = mk(fmts[i], mem);
        GfxTex *t = mtl_tex_get(&s, 1);
        CHECK(t && t->has_stencil, "format %#x has a stencil plane", fmts[i]);
        if (!t) continue;
        id<MTLTexture> dv = mtl_tex_view(t, swz, &none, 1, TEXTYPE_2D, 0, 0);
        CHECK(dv.pixelFormat == t->pf, "depth mode samples the texture itself (format %lu)", (unsigned long)dv.pixelFormat);
        id<MTLTexture> sv = mtl_tex_view(t, swz, &none, 1, TEXTYPE_2D, 0, 1);
        CHECK(sv && sv.pixelFormat == MTLPixelFormatX32_Stencil8, "stencil mode of the texture: stencil-only view (got %lu)", (unsigned long)sv.pixelFormat);
        sv = mtl_tex_view(t, swz, &vs, 1, TEXTYPE_2D, 0, 0);
        CHECK(sv && sv.pixelFormat == MTLPixelFormatX32_Stencil8, "stencil mode of the view: stencil-only view (got %lu)", (unsigned long)sv.pixelFormat);
        dv = mtl_tex_view(t, swz, &vd, 1, TEXTYPE_2D, 0, 1);
        CHECK(dv.pixelFormat == t->pf, "the view's depth mode overrides the texture's stencil mode (got %lu)", (unsigned long)dv.pixelFormat);
        CHECK(sv.width == 64 && sv.height == 64, "stencil view size %lux%lu", (unsigned long)sv.width, (unsigned long)sv.height);
    }
    texsnap d32 = mk(0x34 /* Depth32F */, mem);
    GfxTex *t32 = mtl_tex_get(&d32, 1);
    CHECK(t32 && !t32->has_stencil, "Depth32F has no stencil plane");
    if (t32) {
        id<MTLTexture> v = mtl_tex_view(t32, swz, &vs, 1, TEXTYPE_2D, 0, 1);
        CHECK(v.pixelFormat == t32->pf, "stencil mode on a depth-only texture stays the depth texture (got %lu)", (unsigned long)v.pixelFormat);
    }
    napple_gfx_shutdown();
    printf(g_fail ? "t_ds_views: %d FAILED\n" : "t_ds_views: ok\n", g_fail);
    return g_fail != 0;
}
