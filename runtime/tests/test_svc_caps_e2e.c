/* End-to-end: the album JPEG round trip across caps:su, caps:u, the accessor session and caps:dc, with an independent JPEG codec on each side.
 * A 1280x720 frame (a real game frame when NAPPLE_CAPS_E2E_PNG names a PNG, e.g. a NAPPLE_SCREENSHOT of a headless run; else a synthetic scene)
 * is saved through caps:su SaveScreenShot (jpeg_core encodes it), found again in the caps:u album listing, read back as a file through
 * OpenAccessorSessionForApplication -> LoadAlbumFile, and that file is decoded twice: by ImageIO (Apple's decoder: the album file is a standard JPEG
 * any viewer opens) and by caps:dc DecodeJpeg. The other direction: ImageIO encodes the frame (a JPEG from a foreign encoder, as an album file copied
 * from a console would be) and caps:dc decodes it. NAPPLE_CAPS_E2E_OUT=<dir> keeps album.jpg and the decoded frames as PNG for inspection.
 * The ImageIO halves are macOS-only; on Linux the test runs the caps-only round trip. */
#include "../src/services/caps.c"
#include "../src/services/caps_dc.c"
#include "svc_test.h"
#include <math.h>
#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif
#ifdef __linux__
int applet_screenshot_permitted(void) { return 1; }
#endif

enum { W = CAPS_IMAGE_W, H = CAPS_IMAGE_H };

/* PSNR over RGB in dB (99 = identical). JPEG at quality 90 with 4:2:0 chroma gives 32-40 dB on MK8D's menus (the sparkle backgrounds are the worst
 * case; ImageIO's own encoder lands within 1 dB of jpeg_core there) and ~51 dB on the synthetic scene; a wrong layout, channel order or scale falls
 * under 15 dB. The thresholds leave room for noisier frames. */
static double psnr(const uint8_t *a, const uint8_t *b) {
    double s = 0;
    for (size_t i = 0; i < (size_t)W * H; i++)
        for (int k = 0; k < 3; k++) {
            double d = (double)a[i * 4 + k] - b[i * 4 + k];
            s += d * d;
        }
    s /= (double)W * H * 3;
    return s == 0 ? 99 : 10 * log10(255.0 * 255.0 / s);
}
enum { MIN_DB = 26, AGREE_DB = 33 }; /* against the source frame; the two decoders of one file against each other */
/* A scene with edges, flat areas and gradients (JPEG's hard and easy cases) when no real frame is given. */
static void synth(uint8_t *p) {
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) {
            uint8_t *o = p + ((size_t)y * W + x) * 4;
            int sq = ((x / 80) + (y / 80)) & 1;
            o[0] = (uint8_t)(sq ? 230 : x * 255 / (W - 1));
            o[1] = (uint8_t)(sq ? 40 : y * 255 / (H - 1));
            o[2] = (uint8_t)((x * y) >> 12);
            o[3] = 255;
        }
}

#ifdef __APPLE__
/* Draws an image into a W x H RGBA8 (premultiplied-last, opaque) buffer. */
static int draw_rgba(CGImageRef img, uint8_t *dst) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(dst, W, H, 8, W * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) return -1;
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, W, H), img);
    CGContextRelease(ctx);
    for (size_t i = 0; i < (size_t)W * H; i++) dst[i * 4 + 3] = 255;
    return 0;
}
static int load_png(const char *path, uint8_t *dst) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageSourceRef src = url ? CGImageSourceCreateWithURL(url, NULL) : NULL;
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    int rc = img ? draw_rgba(img, dst) : -1;
    if (img) CGImageRelease(img);
    if (src) CFRelease(src);
    if (url) CFRelease(url);
    return rc;
}
static int imageio_decode(const uint8_t *jpg, size_t n, uint8_t *dst, int *w, int *h) {
    CFDataRef d = CFDataCreate(NULL, jpg, (CFIndex)n);
    CGImageSourceRef src = CGImageSourceCreateWithData(d, NULL);
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    int rc = -1;
    if (img) {
        *w = (int)CGImageGetWidth(img);
        *h = (int)CGImageGetHeight(img);
        rc = draw_rgba(img, dst);
        CGImageRelease(img);
    }
    if (src) CFRelease(src);
    CFRelease(d);
    return rc;
}
static CGImageRef rgba_image(const uint8_t *px) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CFDataRef d = CFDataCreate(NULL, px, (CFIndex)W * H * 4);
    CGDataProviderRef dp = CGDataProviderCreateWithCFData(d);
    CGImageRef img = CGImageCreate(W, H, 8, 32, W * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big, dp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CFRelease(d);
    CGColorSpaceRelease(cs);
    return img;
}
/* JPEG (type "public.jpeg") or PNG of an RGBA frame, through ImageIO. Returns a malloc'd buffer. */
static uint8_t *imageio_encode(const uint8_t *px, CFStringRef type, size_t *n) {
    CGImageRef img = rgba_image(px);
    CFMutableDataRef out = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(out, type, 1, NULL);
    uint8_t *r = NULL;
    if (dst && img) {
        float q = 0.9f;
        CFNumberRef qn = CFNumberCreate(NULL, kCFNumberFloatType, &q);
        const void *k[] = {kCGImageDestinationLossyCompressionQuality}, *v[] = {qn};
        CFDictionaryRef props = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CGImageDestinationAddImage(dst, img, props);
        if (CGImageDestinationFinalize(dst)) {
            *n = (size_t)CFDataGetLength(out);
            r = malloc(*n);
            if (r) memcpy(r, CFDataGetBytePtr(out), *n);
        }
        CFRelease(props);
        CFRelease(qn);
    }
    if (dst) CFRelease(dst);
    if (img) CGImageRelease(img);
    CFRelease(out);
    return r;
}
static void keep(const char *dir, const char *name, const uint8_t *p, size_t n) {
    if (!dir || !*dir || !p) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}
static void keep_png(const char *dir, const char *name, const uint8_t *px) {
    size_t n = 0;
    uint8_t *png = dir && *dir ? imageio_encode(px, CFSTR("public.png"), &n) : NULL;
    keep(dir, name, png, n);
    free(png);
}
#endif

/* caps:dc DecodeJpeg of `jpg` to W x H into out; returns the call result. */
static uint32_t dc_decode(IpcObject *dc, const uint8_t *jpg, size_t n, uint8_t *out) {
    uint32_t args[10] = {W, H};
    IpcCall c = {0};
    svc_set_in_buf(&c, jpg, n);
    svc_set_out_buf(&c, out, CAPS_IMAGE_BYTES);
    c.in = (const uint8_t *)args;
    c.in_size = sizeof args;
    svc_call_c(dc, 3001, &c);
    return c.result;
}

int main(void) {
    uint8_t *frame = malloc(CAPS_IMAGE_BYTES), *a = malloc(CAPS_IMAGE_BYTES), *b = malloc(CAPS_IMAGE_BYTES);
    const char *png = getenv("NAPPLE_CAPS_E2E_PNG"), *outdir = getenv("NAPPLE_CAPS_E2E_OUT");
    int real = 0;
#ifdef __APPLE__
    if (png && *png) {
        real = load_png(png, frame) == 0;
        CHECK(real, "real frame %s loaded", png);
    }
#endif
    if (!real) synth(frame);
    printf("source: %s\n", real ? png : "synthetic scene");
    IpcObject *su = su_open(), *u = u_open(), *dc = dc_open();
    IpcCall c;

    /* 1. save: caps:su SaveScreenShot with the frame as the capture buffer */
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, frame, CAPS_IMAGE_BYTES);
    svc_call_c(su, 32, &c);
    CHECK(c.result == 0 && c.out_size == CAPS_ENTRY_SIZE, "caps:su SaveScreenShot");
    uint8_t e[CAPS_ENTRY_SIZE];
    memcpy(e, c.out, sizeof e);
    uint64_t fsize;
    memcpy(&fsize, e, 8);
    CHECK(fsize > 1000 && fsize < CAPS_IMAGE_BYTES, "entry size %llu", (unsigned long long)fsize);

    /* 2. list: caps:u GetAlbumFileList finds the same entry */
    uint8_t list[CAPS_ALBUM_MAX * CAPS_ENTRY_SIZE];
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, list, sizeof list);
    svc_call_c(u, 102, &c);
    int64_t cnt = c.result == 0 ? SVC_OUT(&c, int64_t, 0) : -1;
    int found = 0;
    for (int64_t i = 0; i < cnt; i++) found |= !memcmp(list + i * CAPS_ENTRY_SIZE, e, CAPS_ENTRY_SIZE);
    CHECK(cnt == 1 && found, "caps:u album listing holds the saved entry (%lld entries)", (long long)cnt);

    /* 3. load the file through the application accessor session */
    svc_call(u, 60002, NULL, 0, &c);
    IpcObject *sess = c.out_objects[0];
    CHECK(sess != NULL, "OpenAccessorSessionForApplication");
    uint8_t *file = malloc(fsize);
    memset(&c, 0, sizeof c);
    c.in = e;
    c.in_size = sizeof e;
    svc_set_out_buf(&c, file, fsize);
    if (sess) svc_call_c(sess, 2, &c);
    CHECK(c.result == 0 && (uint64_t)SVC_OUT(&c, int64_t, 0) == fsize && file[0] == 0xff && file[1] == 0xd8, "LoadAlbumFile returns the %llu-byte JPEG",
          (unsigned long long)fsize);

    /* 4. caps:dc decodes the album file back to the frame */
    uint32_t r = dc_decode(dc, file, fsize, a);
    double d_dc = psnr(frame, a);
    CHECK(r == 0 && d_dc > MIN_DB, "caps:dc decodes the album file (%.1f dB)", d_dc);

    /* 5. the album image path agrees with caps:dc */
    memset(&c, 0, sizeof c);
    c.in = e;
    c.in_size = sizeof e;
    svc_set_out_buf(&c, b, CAPS_IMAGE_BYTES);
    svc_call_c(u, 110, &c);
    CHECK(c.result == 0 && !memcmp(a, b, CAPS_IMAGE_BYTES), "LoadAlbumScreenShotImage gives the same pixels as caps:dc");

#ifdef __APPLE__
    /* 6. the album file is a standard JPEG: ImageIO opens it and sees the same picture */
    int iw = 0, ih = 0;
    int ok = imageio_decode(file, fsize, b, &iw, &ih) == 0;
    double d_io = ok ? psnr(frame, b) : 0;
    CHECK(ok && iw == W && ih == H && d_io > MIN_DB, "ImageIO decodes the album file at %dx%d (%.1f dB)", iw, ih, d_io);
    double d_both = ok ? psnr(a, b) : 0;
    CHECK(d_both > AGREE_DB, "caps:dc and ImageIO agree on the album file (%.1f dB)", d_both);

    /* 7. a foreign JPEG (ImageIO's encoder) decodes through caps:dc */
    size_t fn = 0;
    uint8_t *foreign = imageio_encode(frame, CFSTR("public.jpeg"), &fn);
    CHECK(foreign && fn > 1000, "ImageIO encoded the frame (%zu bytes)", fn);
    r = foreign ? dc_decode(dc, foreign, fn, b) : 1;
    double d_foreign = r == 0 ? psnr(frame, b) : 0;
    CHECK(r == 0 && d_foreign > MIN_DB, "caps:dc decodes an ImageIO JPEG (%.1f dB)", d_foreign);

    keep(outdir, "album.jpg", file, fsize);
    keep_png(outdir, "album-capsdc.png", a);
    keep_png(outdir, "foreign-capsdc.png", b);
    free(foreign);
    printf("PSNR: caps:dc %.1f dB, ImageIO %.1f dB, caps:dc vs ImageIO %.1f dB, foreign JPEG %.1f dB\n", d_dc, d_io, d_both, d_foreign);
#endif

    /* 8. delete: the entry is gone from the listing and the session */
    svc_call(u, 103, e, sizeof e, &c);
    CHECK(c.result == 0, "DeleteAlbumFile");
    memset(&c, 0, sizeof c);
    c.in = e;
    c.in_size = sizeof e;
    svc_set_out_buf(&c, file, fsize);
    if (sess) svc_call_c(sess, 2, &c);
    CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND, "deleted file is not found through the session");

    caps_album_clear(&caps);
    free(file);
    free(frame);
    free(a);
    free(b);
    printf("{\"test\":\"svc_caps_e2e\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
