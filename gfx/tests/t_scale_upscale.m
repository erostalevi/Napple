/* t_scale_upscale.m - the upscaler matrix (headless Metal): nearest, bilinear, bicubic, Lanczos, the built-in edge adaptive upscaler (FSR-like), CAS and
 * MetalFX spatial (falls back to the built-in one when the OS has none) on a synthetic scene rendered 2x larger and box-filtered down as the source.
 * Asserts on PSNR against the full resolution reference and on a sharpness metric (mean absolute Laplacian of luma); writes the contact sheet to
 * $NAPPLE_T_OUT (default build/tests/upscaler-matrix.png). */
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../include/napple_gfx.h"
#import "mtl_display.h"
#import "internal.h"
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)
enum { RW = 640, RH = 360, SW = RW / 2, SH = RH / 2 };

static float lum(const uint8_t *p) { return 0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]; } /* BGRA */
/* full resolution scene: smooth gradient, slanted edge, circle, thin lines, small-feature stripes */
static void scene(uint8_t *px) {
    for (int y = 0; y < RH; y++)
        for (int x = 0; x < RW; x++) {
            float r = 60 + 80.0f * x / RW, g = 90 + 60.0f * y / RH, b = 140;
            if (y > (x * 0.55f) + 40 && x < 300) r = 235, g = 220, b = 60;                       /* slanted edge */
            float dx = x - 470, dy = y - 110; if (dx * dx + dy * dy < 70 * 70) r = 20, g = 30, b = 40;  /* disc */
            if (y > 250 && ((x / 3) & 1) && x > 40 && x < 300) r = g = b = 250;                 /* fine vertical stripes */
            if (y > 250 && y < 254 && x > 320) r = g = b = 255;                                 /* thin horizontal line */
            if (x > 330 && x < 600 && y > 160 && y < 230 && ((x + y) / 4 & 1)) r = 220, g = 40, b = 60;   /* diagonal hatch */
            uint8_t *o = px + ((size_t)y * RW + x) * 4;
            o[0] = (uint8_t)b; o[1] = (uint8_t)g; o[2] = (uint8_t)r; o[3] = 255;
        }
}
static id<MTLTexture> mk(uint32_t w, uint32_t h, MTLPixelFormat pf, MTLTextureUsage u) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.usage = u; d.storageMode = MTLStorageModeShared; return [g_mtl.dev newTextureWithDescriptor:d];
}
static uint8_t *render(id<MTLTexture> src, uint32_t tw, uint32_t th, napple_display_settings *ds) {
    id<MTLTexture> dst = mk(tw, th, MTLPixelFormatBGRA8Unorm, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    float full[4] = {0, 0, 1, 1};
    id<MTLCommandBuffer> cb = [g_mtl.queue commandBuffer];
    mtl_present_compose(cb, src, full, (float)src.width, (float)src.height, dst, ds, 1);
    [cb commit]; [cb waitUntilCompleted];
    uint8_t *out = malloc((size_t)tw * th * 4);
    [dst getBytes:out bytesPerRow:tw * 4 fromRegion:MTLRegionMake2D(0, 0, tw, th) mipmapLevel:0];
    return out;
}
/* PSNR over the edge regions only (slanted edge and disc): the fine stripes and the hatch sit at the Nyquist limit of the half size source, where any
 * reconstruction is a guess and sharpening only moves the error around */
static int in_roi(int x, int y) { return (x > 4 && x < 320 && y > 4 && y < 240) || (x > 380 && x < 560 && y > 30 && y < 155); }
static double psnr(const uint8_t *a, const uint8_t *b, int w, int h) {
    double se = 0; long n = 0;
    for (int y = 4; y < h - 4; y++) for (int x = 4; x < w - 4; x++) if (in_roi(x, y)) for (int c = 0; c < 3; c++) { double d = (double)a[((size_t)y * w + x) * 4 + c] - b[((size_t)y * w + x) * 4 + c]; se += d * d; n++; }
    return 10 * log10(255.0 * 255.0 / (se / n + 1e-9));
}
static double sharpness(const uint8_t *a, int w, int h) {
    double s = 0; long n = 0;
    for (int y = 2; y < h - 2; y++) for (int x = 2; x < w - 2; x++) {
        const uint8_t *p = a + ((size_t)y * w + x) * 4;
        float l = 4 * lum(p) - lum(p - 4) - lum(p + 4) - lum(p - (size_t)w * 4) - lum(p + (size_t)w * 4);
        s += fabsf(l); n++;
    }
    return s / n;
}
/* mean luma step edge rise: columns across the slanted edge are not axis aligned, so use the disc: count mid-grey pixels (a sharp edge has few) */
static long mid_pixels(const uint8_t *a, int w, int h) {
    long n = 0;
    for (int y = 40; y < 180; y++) for (int x = 380; x < 560 && x < w; x++) { float l = lum(a + ((size_t)y * w + x) * 4); if (l > 60 && l < 120) n++; }
    return n;
}
static void save_png(const char *path, const uint8_t *bgra, int w, int h) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate((void *)bgra, w, h, 8, (size_t)w * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
    CGImageRef im = CGBitmapContextCreateImage(cg);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(path)], (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dst) { CGImageDestinationAddImage(dst, im, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); } else printf("cannot write %s\n", path);
    CGImageRelease(im); CGContextRelease(cg); CGColorSpaceRelease(cs);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    napple_gfx_config cfg; napple_gfx_config_default(&cfg); cfg.headless_width = 64; cfg.headless_height = 64;
    if (napple_gfx_init(&cfg)) { printf("FAIL init\n"); return 1; }
    uint8_t *ref = malloc((size_t)RW * RH * 4); scene(ref);
    uint8_t *lo = malloc((size_t)SW * SH * 4);
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) for (int c = 0; c < 4; c++) {
        int s = 0; for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) s += ref[((size_t)(2 * y + j) * RW + 2 * x + i) * 4 + c];
        lo[((size_t)y * SW + x) * 4 + c] = (uint8_t)((s + 2) / 4); }
    id<MTLTexture> src = mk(SW, SH, MTLPixelFormatBGRA8Unorm, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    [src replaceRegion:MTLRegionMake2D(0, 0, SW, SH) mipmapLevel:0 withBytes:lo bytesPerRow:SW * 4];

    struct { const char *name; uint32_t filter, up; } m[] = {
        {"nearest", NAPPLE_FILTER_NEAREST, 0}, {"bilinear", NAPPLE_FILTER_BILINEAR, 0}, {"bicubic", NAPPLE_FILTER_BICUBIC, 0}, {"lanczos", NAPPLE_FILTER_LANCZOS, 0},
        {"fsr-like", NAPPLE_FILTER_BILINEAR, NAPPLE_UP_FSR}, {"cas", NAPPLE_FILTER_BILINEAR, NAPPLE_UP_CAS}, {"metalfx", NAPPLE_FILTER_BILINEAR, NAPPLE_UP_METALFX}};
    enum { N = 7 };
    uint8_t *img[N]; double ps[N], sh[N]; long mid[N];
    napple_display_settings ds; napple_display_default(&ds); ds.sharpness = 0.8f; ds.fit = NAPPLE_FIT_STRETCH;
    printf("metalfx available: %d\n", mtl_present_metalfx_available());
    for (int i = 0; i < N; i++) {
        ds.filter = m[i].filter; ds.upscaler = m[i].up;
        img[i] = render(src, RW, RH, &ds);
        ps[i] = psnr(img[i], ref, RW, RH); sh[i] = sharpness(img[i], RW, RH); mid[i] = mid_pixels(img[i], RW, RH);
        printf("%-9s PSNR %.2f dB  sharpness %.2f  mid-grey edge pixels %ld\n", m[i].name, ps[i], sh[i], mid[i]);
    }
    double rs = sharpness(ref, RW, RH); printf("reference sharpness %.2f\n", rs);
    enum { NEAREST, BILIN, BICUBIC, LANCZOS, FSR, CAS, MFX };
    CHECK(sh[FSR] > sh[BILIN] * 1.05, "built-in upscaler is sharper than bilinear (%.2f vs %.2f)", sh[FSR], sh[BILIN]);
    CHECK(sh[CAS] > sh[BILIN] * 1.05 && sh[CAS] > sh[BICUBIC], "CAS is sharper than bilinear and bicubic (%.2f)", sh[CAS]);
    CHECK(sh[LANCZOS] > sh[BILIN], "Lanczos sharper than bilinear");
    CHECK(sh[FSR] < rs * 1.6 && sh[CAS] < rs * 1.6, "sharpening stays sane (no runaway halos)");
    CHECK(ps[FSR] > ps[BILIN] - 0.3, "built-in upscaler is not worse than bilinear in PSNR (%.2f vs %.2f)", ps[FSR], ps[BILIN]);
    CHECK(ps[FSR] > ps[NEAREST], "built-in upscaler beats nearest in PSNR");
    CHECK(mid[FSR] <= mid[BILIN], "built-in upscaler has fewer blurred edge pixels than bilinear (%ld vs %ld)", mid[FSR], mid[BILIN]);
    CHECK(ps[MFX] > 20 && sh[MFX] > 0, "MetalFX (or its fallback) produced an image");
    for (int i = 0; i < N; i++) CHECK(ps[i] > 18, "%s output is sane (PSNR %.1f)", m[i].name, ps[i]);
    /* contact sheet: 4 x 2 grid of 640x360 (reference first) */
    const int cols = 4, rows = 2, W = RW * cols, H = RH * rows;
    uint8_t *sheet = calloc((size_t)W * H, 4);
    for (int k = 0; k < N + 1; k++) {
        const uint8_t *s = k == 0 ? ref : img[k - 1];
        int ox = (k % cols) * RW, oy = (k / cols) * RH;
        for (int y = 0; y < RH; y++) memcpy(sheet + ((size_t)(oy + y) * W + ox) * 4, s + (size_t)y * RW * 4, RW * 4);
    }
    const char *out = getenv("NAPPLE_T_OUT"); save_png(out ? out : "build/tests/upscaler-matrix.png", sheet, W, H);
    printf(g_fail ? "t_scale_upscale FAILED (%d)\n" : "t_scale_upscale ok (sheet order: reference, nearest, bilinear, bicubic, lanczos, fsr-like, cas, metalfx)\n", g_fail);
    return g_fail != 0;
}
