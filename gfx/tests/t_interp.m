/* t_interp.m - frame generation quality and cost on synthetic motion (headless Metal): interpolated frames against the exactly translated ground truth,
 * plain blend as the baseline, static overlays, scene cut detection, GPU time. */
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../include/napple_gfx.h"
#import "mtl_interp.h"
#import "internal.h"
static int g_fail;
#define CHECK(c, ...)                                                                                                                                          \
    do {                                                                                                                                                       \
        if (!(c)) {                                                                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                                                        \
            printf(__VA_ARGS__);                                                                                                                               \
            printf("\n");                                                                                                                                      \
            g_fail++;                                                                                                                                          \
        }                                                                                                                                                      \
    } while (0)
int mtl_interp_test_pair(id<MTLTexture> a, id<MTLTexture> b, float t, int mode, id<MTLTexture> out, double *cut_score, double *gpu_ms);
double mtl_interp_cut_score(const uint8_t *a, const uint8_t *b, uint32_t w, uint32_t h);

static id<MTLTexture> mk(uint32_t w, uint32_t h, MTLPixelFormat pf, MTLTextureUsage u) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.usage = u;
    d.storageMode = MTLStorageModeShared;
    return [g_mtl.dev newTextureWithDescriptor:d];
}
/* natural-looking texture: value noise over 5 octaves (3..48 px) plus a few hard-edged bars, shifted by (-dx, -dy) */
static float hash2(int x, int y, int seed) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}
static float vnoise(float x, float y, float cell, int seed) {
    float fx = x / cell, fy = y / cell;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float ux = fx - ix, uy = fy - iy;
    ux = ux * ux * (3 - 2 * ux);
    uy = uy * uy * (3 - 2 * uy);
    float a = hash2(ix, iy, seed), b = hash2(ix + 1, iy, seed), c = hash2(ix, iy + 1, seed), d = hash2(ix + 1, iy + 1, seed);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}
static float field(float x, float y, int seed) {
    float v = 0, amp = 0.5f, sum = 0;
    for (int o = 0; o < 5; o++) {
        float cell = 3.0f * (float)(1 << o);
        v += amp * vnoise(x, y, cell, seed * 17 + o);
        sum += amp;
        amp *= 0.6f;
    }
    v /= sum;
    if (fmodf(fabsf(x * 0.5f + y * 0.2f), 90.0f) < 6.0f) v = 1.0f - v; /* hard-edged diagonal bars */
    return v;
}
static void shade(float v, uint8_t *p, int seed) {
    float u = fminf(1, fmaxf(0, v));
    p[0] = (uint8_t)(255 * u * 0.6f);
    p[1] = (uint8_t)(255 * u);
    p[2] = (uint8_t)(255 * (1 - u * 0.5f));
    p[3] = 255;
    (void)seed;
}
static id<MTLTexture> scene(uint32_t w, uint32_t h, float dx, float dy, int seed, int hud) { /* hud 2: a different kind of picture (large flat areas) */
    uint8_t *px = malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = px + ((size_t)y * w + x) * 4;
            shade(hud == 2 ? 1.0f - vnoise((float)x - dx, (float)y - dy, 140.0f, seed) : field((float)x - dx, (float)y - dy, seed), p, seed);
            if (hud == 1 && x > 40 && x < 40 + 160 && y > 30 && y < 30 + 40) {
                p[0] = 30;
                p[1] = 240;
                p[2] = 255;
            } /* static HUD box (yellow in BGRA) */
        }
    id<MTLTexture> t = mk(w, h, MTLPixelFormatBGRA8Unorm, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:w * 4];
    free(px);
    return t;
}
static double mae(id<MTLTexture> a, id<MTLTexture> b, uint32_t margin) {
    uint32_t w = (uint32_t)a.width, h = (uint32_t)a.height;
    uint8_t *pa = malloc((size_t)w * h * 4), *pb = malloc((size_t)w * h * 4);
    [a getBytes:pa bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    [b getBytes:pb bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    double s = 0;
    long n = 0;
    for (uint32_t y = margin; y < h - margin; y++)
        for (uint32_t x = margin; x < w - margin; x++)
            for (int c = 0; c < 3; c++) {
                s += abs((int)pa[((size_t)y * w + x) * 4 + c] - (int)pb[((size_t)y * w + x) * 4 + c]);
                n++;
            }
    free(pa);
    free(pb);
    return s / (double)n;
}

int main(void) {
    napple_gfx_config cfg;
    napple_gfx_config_default(&cfg);
    cfg.headless_width = 64;
    cfg.headless_height = 64;
    if (napple_gfx_init(&cfg)) {
        printf("FAIL init\n");
        return 1;
    }
    const uint32_t W = 960, H = 540;
    id<MTLTexture> out = mk(W, H, MTLPixelFormatBGRA8Unorm, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    struct {
        float dx, dy;
    } motions[] = {{4, 0}, {12, 6}, {24, -10}, {60, 0}, {-48, 20}};
    for (unsigned m = 0; m < sizeof motions / sizeof *motions; m++) {
        float dx = motions[m].dx, dy = motions[m].dy;
        id<MTLTexture> A = scene(W, H, 0, 0, 1, 0), B = scene(W, H, dx, dy, 1, 0), truth = scene(W, H, dx * 0.5f, dy * 0.5f, 1, 0);
        double cut = 0, msa[3] = {0}, ms;
        CHECK(mtl_interp_test_pair(A, B, 0.5f, 0, out, &cut, msa) == 0, "flow interpolation ran");
        ms = msa[0];
        double e_flow = mae(out, truth, 100);
        mtl_interp_test_pair(A, B, 0.5f, 1, out, NULL, NULL);
        double e_blend = mae(out, truth, 100);
        printf("motion (%g,%g) px: mean abs error vs ground truth: motion-compensated %.2f, plain blend %.2f  (cut score %.3f, GPU %.2f ms for flow + warp)\n",
               dx, dy, e_flow, e_blend, cut, ms);
        CHECK(e_flow < e_blend * 0.6 || e_flow < 1.5, "motion compensation beats the blend for (%g,%g): %.2f vs %.2f", dx, dy, e_flow, e_blend);
        CHECK(cut < 0.075, "a pan is not a scene cut (%.3f)", cut);
    }
    /* phases: t = 0.25 / 0.75 */
    {
        id<MTLTexture> A = scene(W, H, 0, 0, 2, 0), B = scene(W, H, 16, 8, 2, 0);
        for (int k = 1; k <= 3; k++) {
            float t = 0.25f * k;
            id<MTLTexture> truth = scene(W, H, 16 * t, 8 * t, 2, 0);
            mtl_interp_test_pair(A, B, t, 0, out, NULL, NULL);
            double e = mae(out, truth, 60);
            CHECK(e < 2.5, "phase %.2f error %.2f", t, e);
        }
    }
    /* static HUD over a moving background stays crisp */
    {
        id<MTLTexture> A = scene(W, H, 0, 0, 3, 1), B = scene(W, H, 20, 0, 3, 1);
        mtl_interp_test_pair(A, B, 0.5f, 0, out, NULL, NULL);
        uint8_t px[4];
        [out getBytes:px bytesPerRow:4 fromRegion:MTLRegionMake2D(100, 50, 1, 1) mipmapLevel:0];
        CHECK(abs(px[0] - 30) <= 2 && abs(px[1] - 240) <= 2 && abs(px[2] - 255) <= 2, "HUD pixel inside the box stays (%d %d %d)", px[0], px[1], px[2]);
    }
    /* scene cut */
    {
        id<MTLTexture> A = scene(W, H, 0, 0, 1, 0), B = scene(W, H, 0, 0, 7, 2);
        double cut = 0;
        mtl_interp_test_pair(A, B, 0.5f, 0, out, &cut, NULL);
        printf("different scene: cut score %.3f\n", cut);
        CHECK(cut > 0.075, "unrelated frames are a scene cut (%.3f)", cut);
        id<MTLTexture> C = scene(W, H, 0, 0, 1, 0);
        mtl_interp_test_pair(A, C, 0.5f, 0, out, &cut, NULL);
        CHECK(cut < 0.01, "identical frames: score %.3f", cut);
    }
    /* cost at the window sizes of the owner's tests */
    {
        uint32_t sizes[][2] = {{1920, 1080}, {2560, 1440}, {3440, 1440}};
        for (int i = 0; i < 3; i++) {
            uint32_t w = sizes[i][0], h = sizes[i][1];
            id<MTLTexture> A = scene(w, h, 0, 0, 1, 0), B = scene(w, h, 14, 5, 1, 0),
                           o = mk(w, h, MTLPixelFormatBGRA8Unorm, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
            double m3[3], tot[3] = {0, 0, 0};
            for (int k = 0; k < 6; k++) {
                mtl_interp_test_pair(A, B, 0.5f, 0, o, NULL, m3);
                if (k >= 2)
                    for (int q = 0; q < 3; q++) tot[q] += m3[q];
            }
            printf("%ux%u: GPU %.2f ms = flow %.2f + warp %.2f (avg of 4)\n", w, h, tot[0] / 4, tot[1] / 4, tot[2] / 4);
        }
    }
    printf(g_fail ? "t_interp FAILED (%d)\n" : "t_interp ok\n", g_fail);
    napple_gfx_shutdown();
    return g_fail != 0;
}
