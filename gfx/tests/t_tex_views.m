/* t_tex_views.m - sampled texture views (mtl_tex_view) that used to fall back to the whole texture: a level/layer range of a 2D array, a cube viewed as a 2D array
 * slice, an sRGB format view, a view with more layers than the texture (clamped by the fallback) - each must be a real Metal view with the chosen range. */
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/napple_gfx.h"
#import "mtl_display.h"
#import "internal.h"
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    napple_gfx_config cfg; napple_gfx_config_default(&cfg); cfg.headless_width = 256; cfg.headless_height = 256;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    uint8_t *mem = calloc(1, 8 << 20);
    const uint8_t swz[4] = { 2, 3, 4, 5 };
    texsnap a; memset(&a, 0, sizeof a);
    a.d.width = 64; a.d.height = 64; a.d.depth = 4; a.d.levels = 3; a.d.target = 4 /* 2D array */; a.d.format = 0x25 /* RGBA8 */; a.d.samples = 1; a.mem = mem; a.size = 8 << 20; a.valid = 1;
    GfxTex *t = mtl_tex_get(&a, 0);
    CHECK(t && t->tex.textureType == MTLTextureType2DArray, "2D array texture");
    if (!t) return 1;
    view_t v; memset(&v, 0, sizeof v);
    v.set_layers = 1; v.min_layer = 1; v.nlayers = 2; v.set_levels = 1; v.base_level = 1; v.nlevels = 2;
    id<MTLTexture> r = mtl_tex_view(t, swz, &v, 1, TEXTYPE_2D_ARRAY, 0, 0);
    CHECK(r != t->tex && r.arrayLength == 2 && r.mipmapLevelCount == 2 && r.width == 32, "layer 1..2, level 1..2 range view: array %lu levels %lu width %lu", (unsigned long)r.arrayLength, (unsigned long)r.mipmapLevelCount, (unsigned long)r.width);
    v.nlayers = 1;
    r = mtl_tex_view(t, swz, &v, 1, TEXTYPE_2D, 0, 0);
    CHECK(r != t->tex && r.textureType == MTLTextureType2D, "single layer as 2D: type %lu", (unsigned long)r.textureType);
    /* the same range asked as a cube (not expressible: 1 layer): keeps the range, not the whole texture */
    r = mtl_tex_view(t, swz, &v, 1, TEXTYPE_CUBE, 0, 0);
    CHECK(r != t->tex && r.width == 32, "cube request over one layer keeps the level/layer range (width %lu)", (unsigned long)r.width);
    v = (view_t){0}; v.set_format = 1; v.format = 0x38 /* RGBA8_SRGB */;
    r = mtl_tex_view(t, swz, &v, 1, TEXTYPE_2D_ARRAY, 0, 0);
    CHECK(r.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB, "sRGB format view: %lu", (unsigned long)r.pixelFormat);
    napple_gfx_shutdown();
    printf(g_fail ? "t_tex_views: %d FAILED\n" : "t_tex_views: ok\n", g_fail);
    return g_fail != 0;
}
