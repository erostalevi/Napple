/* t_present.m - the output stage (mtl_present.m): aspect fit, filters and supersample resolve, checked on real pixels (headless Metal). */
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../include/napple_gfx.h"
#import "mtl_display.h"
#import "internal.h"
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static id<MTLTexture> mk(uint32_t w, uint32_t h, MTLPixelFormat pf, MTLTextureUsage u) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO]; d.usage = u; d.storageMode = MTLStorageModeShared;
    return [g_mtl.dev newTextureWithDescriptor:d];
}
typedef void (*fill_fn)(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *bgra);
static id<MTLTexture> make_src(uint32_t w, uint32_t h, fill_fn f) {
    id<MTLTexture> t = mk(w, h, MTLPixelFormatBGRA8Unorm, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    uint8_t *px = malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) f(x, y, w, h, px + ((size_t)y * w + x) * 4);
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:w * 4]; free(px);
    return t;
}
static void checker(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *p) { uint8_t v = ((x ^ y) & 1) ? 255 : 0; p[0] = p[1] = p[2] = v; p[3] = 255; }
static void grad(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *p) { p[0] = (uint8_t)(x * 255 / (w - 1)); p[1] = (uint8_t)(y * 255 / (h - 1)); p[2] = 100; p[3] = 255; }
static void edge(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *p) { uint8_t v = x < w / 2 ? 80 : 180; p[0] = p[1] = p[2] = v; p[3] = 255; }
static void quad(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *p) { /* red top-left, green top-right, blue bottom-left, white bottom-right */
    int r = x < w / 2, t = y < h / 2; p[3] = 255; p[0] = p[1] = p[2] = 0;
    if (t && r) p[2] = 255; else if (t) p[1] = 255; else if (r) p[0] = 255; else p[0] = p[1] = p[2] = 255; }

static uint8_t *render(id<MTLTexture> src, const float crop[4], float gw, float gh, uint32_t tw, uint32_t th, napple_display_settings *ds) {
    id<MTLTexture> dst = mk(tw, th, MTLPixelFormatBGRA8Unorm, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    id<MTLCommandBuffer> cb = [g_mtl.queue commandBuffer];
    mtl_present_compose(cb, src, crop, gw, gh, dst, ds, 0);
    [cb commit]; [cb waitUntilCompleted];
    uint8_t *out = malloc((size_t)tw * th * 4);
    [dst getBytes:out bytesPerRow:tw * 4 fromRegion:MTLRegionMake2D(0, 0, tw, th) mipmapLevel:0];
    return out;
}
#define PX(img, w, x, y, c) ((img)[((size_t)(y) * (w) + (x)) * 4 + (c)])

int main(void) {
    napple_gfx_config cfg; napple_gfx_config_default(&cfg); cfg.headless_width = 64; cfg.headless_height = 64;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    napple_display_settings ds; napple_display_default(&ds);
    const float full[4] = {0, 0, 1, 1};

    /* 2x supersampled 1-pixel checker: box / Catmull-Rom / Lanczos resolve to the mean, nearest does not */
    id<MTLTexture> ck = make_src(256, 144, checker);
    for (int f = 0; f < 6; f++) {
        ds.filter = (uint32_t)f; ds.sharpness = 0;
        uint8_t *o = render(ck, full, 256, 144, 128, 72, &ds);
        double dev = 0; int n = 0;
        for (int y = 8; y < 64; y++) for (int x = 8; x < 120; x++) { dev += fabs((double)PX(o, 128, x, y, 1) - 127.5); n++; }
        dev /= n;
        const char *nm[] = {"bilinear", "nearest", "box", "bicubic", "lanczos", "sharp"};
        if (f == 2 || f == 3 || f == 4 || f == 5) CHECK(dev < 6.0, "%s on a 1 px checker, mean deviation from gray %.1f", nm[f], dev);
        if (f == 1) CHECK(dev > 100, "nearest keeps the aliasing (%.1f)", dev);
        free(o);
    }
    /* letterbox: 16:9 image into a 4:3 target */
    id<MTLTexture> qd = make_src(160, 90, quad);
    ds.filter = NAPPLE_FILTER_BICUBIC; ds.fit = NAPPLE_FIT_LETTERBOX;
    uint8_t *o = render(qd, full, 160, 90, 160, 120, &ds);        /* image fits 160x90, bars of 15 rows */
    CHECK(PX(o, 160, 80, 5, 0) == 0 && PX(o, 160, 80, 5, 1) == 0 && PX(o, 160, 80, 5, 2) == 0, "top bar black");
    CHECK(PX(o, 160, 80, 114, 0) == 0 && PX(o, 160, 80, 114, 2) == 0, "bottom bar black");
    CHECK(PX(o, 160, 20, 25, 2) > 240 && PX(o, 160, 20, 25, 1) < 15, "top-left quadrant is red (BGRA: R channel 2) at the image's corner");
    CHECK(PX(o, 160, 140, 100, 0) > 240 && PX(o, 160, 140, 100, 1) > 240, "bottom-right quadrant white");
    free(o);
    /* stretch fills the target */
    ds.fit = NAPPLE_FIT_STRETCH;
    o = render(qd, full, 160, 90, 160, 120, &ds);
    CHECK(PX(o, 160, 80, 2, 0) != 0 || PX(o, 160, 80, 2, 1) != 0 || PX(o, 160, 80, 2, 2) != 0, "stretch has no bars");
    free(o);
    /* fill crops: a 16:9 image filling a square keeps the centre 90x90 */
    ds.fit = NAPPLE_FIT_FILL;
    o = render(qd, full, 160, 90, 90, 90, &ds);
    CHECK(PX(o, 90, 2, 2, 2) > 240, "fill: top-left stays red");
    CHECK(PX(o, 90, 87, 2, 1) > 240, "fill: top-right green");
    free(o);
    /* vertical flip (negative crop height = bottom-up storage) */
    ds.fit = NAPPLE_FIT_LETTERBOX; ds.filter = NAPPLE_FILTER_NEAREST;
    float flip[4] = {0, 1, 1, -1};
    o = render(qd, flip, 160, 90, 160, 90, &ds);
    CHECK(PX(o, 160, 20, 20, 0) > 240 && PX(o, 160, 20, 20, 2) < 15, "flipped: top-left shows the source's bottom-left (blue = B channel 0)");
    free(o);
    ds.filter = NAPPLE_FILTER_LANCZOS;
    o = render(qd, flip, 160, 90, 320, 180, &ds);                 /* flip through the separable path as well, upscaling */
    CHECK(PX(o, 320, 40, 40, 0) > 240 && PX(o, 320, 40, 40, 2) < 20, "flipped through the resampler");
    free(o);
    /* 1:1 is an exact copy */
    id<MTLTexture> gr = make_src(200, 100, grad);
    ds.filter = NAPPLE_FILTER_LANCZOS;
    o = render(gr, full, 200, 100, 200, 100, &ds);
    int exact = 1; uint8_t *ref = malloc(200 * 100 * 4); [gr getBytes:ref bytesPerRow:800 fromRegion:MTLRegionMake2D(0, 0, 200, 100) mipmapLevel:0];
    for (int i = 0; i < 200 * 100 * 4; i++) if (o[i] != ref[i]) { exact = 0; break; }
    CHECK(exact, "1:1 copy is exact");
    free(o); free(ref);
    /* a smooth gradient survives a 2x down-scale with every kernel */
    id<MTLTexture> gr2 = make_src(400, 200, grad);
    for (int f = 2; f <= 4; f++) {
        ds.filter = (uint32_t)f;
        o = render(gr2, full, 400, 200, 200, 100, &ds);
        double err = 0; int n = 0;
        for (int y = 6; y < 94; y++) for (int x = 6; x < 194; x++) { double want = (x + 0.5) * 255.0 / 200.0; err += fabs(PX(o, 200, x, y, 0) - want); n++; }   /* BGRA: channel 0 = B = x gradient */
        CHECK(err / n < 3.0, "filter %d keeps the gradient (mean error %.2f)", f, err / n);
        free(o);
    }
    /* sharpening raises the contrast of an edge, never overshoots */
    id<MTLTexture> ed = make_src(100, 50, edge);
    ds.filter = NAPPLE_FILTER_SHARP; ds.sharpness = 0; o = render(ed, full, 100, 50, 200, 100, &ds); int soft = PX(o, 200, 99, 50, 1), softr = PX(o, 200, 100, 50, 1); free(o);
    ds.sharpness = 1; o = render(ed, full, 100, 50, 200, 100, &ds); int sh1 = PX(o, 200, 99, 50, 1), sh2 = PX(o, 200, 100, 50, 1);
    CHECK(sh1 <= soft && sh2 >= softr && (sh2 - sh1) > (softr - soft), "sharpening steepens the edge (%d,%d -> %d,%d)", soft, softr, sh1, sh2);
    free(o);
    printf(g_fail ? "t_present FAILED (%d)\n" : "t_present ok\n", g_fail);
    napple_gfx_shutdown();
    return g_fail != 0;
}
