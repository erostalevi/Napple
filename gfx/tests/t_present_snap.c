/* t_present_snap.c - a screenshot of a presented frame shows the frame as it was at present time, not what the guest drew into the same texture afterwards.
 * Clears the display texture red, requests a screenshot, presents, then clears it green before the presentation is shown. The PNG's centre pixel must be red.
 * Needs the PNG decoder of ImageIO; no game data. The snapshot guarantee is KNOWN-open until mtl_present.m copies at present (ac-render owns mtl_present.m). */
#include "nvn_harness.h"
#include <ImageIO/ImageIO.h>
#include <CoreFoundation/CoreFoundation.h>

static int png_center(const char *path, uint8_t rgba[4]) {
    CFStringRef p = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef u = CFURLCreateWithFileSystemPath(NULL, p, kCFURLPOSIXPathStyle, false);
    CGImageSourceRef src = CGImageSourceCreateWithURL(u, NULL);
    CFRelease(p); CFRelease(u);
    if (!src) return 1;
    CGImageRef im = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    if (!im) return 1;
    size_t w = CGImageGetWidth(im), hh = CGImageGetHeight(im);
    uint8_t *buf = calloc(w * hh, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef cx = CGBitmapContextCreate(buf, w, hh, 8, w * 4, cs, kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(cx, CGRectMake(0, 0, w, hh), im);
    memcpy(rgba, buf + ((hh / 2) * w + w / 2) * 4, 4);
    CGContextRelease(cx); CGColorSpaceRelease(cs); CGImageRelease(im); free(buf);
    return 0;
}

int main(void) {
    H h; if (h_init(&h, 64, 64, NULL)) return 1;
    NVNtexture *t = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 64, 1, 0x20000, NVN_TEXTURE_FLAGS_DISPLAY);
    NVNwindowBuilder wb; L(nvnWindowBuilderSetDevice)(&wb, h.dev); L(nvnWindowBuilderSetDefaults)(&wb);
    NVNtexture *wt[1] = { t };
    L(nvnWindowBuilderSetTextures)(&wb, 1, wt); L(nvnWindowBuilderSetPresentInterval)(&wb, 1);
    NVNwindow *win = calloc(1, sizeof(NVNwindow)); CHECK(L(nvnWindowInitialize)(win, &wb), "window init");
    NVNsync *acq = calloc(1, sizeof(NVNsync)); L(nvnSyncInitialize)(acq, h.dev);
    int idx = -1;
    CHECK(L(nvnWindowAcquireTexture)(win, acq, &idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS, "acquire");
    L(nvnSyncWait)(acq, 1000000000ull);

    float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 };
    const char *png = "build/t_present_snap.png"; remove(png);
    h_begin(&h);
    const NVNtexture *rts[1] = { t };
    L(nvnCommandBufferSetRenderTargets)(h.cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferClearColor)(h.cb, 0, red, 0xf);
    h_submit(&h);
    napple_gfx_screenshot(png);
    L(nvnQueuePresentTexture)(h.q, win, idx);
    h_begin(&h);
    L(nvnCommandBufferSetRenderTargets)(h.cb, 1, rts, NULL, NULL, NULL);
    L(nvnCommandBufferClearColor)(h.cb, 0, green, 0xf);
    h_submit(&h);
    for (int i = 0; i < 500 && napple_gfx_screenshot_pending(); i++) usleep(1000);
    usleep(50000);
    uint8_t px[4] = { 0 };
    if (access(png, R_OK)) { printf("SKIP no screenshot file written (headless screenshot not available)\n"); g_skip++; }
    else if (png_center(png, px)) { printf("FAIL cannot decode %s\n", png); g_fail++; }
    else {
        printf("screenshot centre rgba %02x%02x%02x%02x\n", px[0], px[1], px[2], px[3]);
        CHECK(px[0] > 0xc0 || px[1] > 0xc0, "screenshot centre should be the red or green frame, got %02x%02x%02x", px[0], px[1], px[2]);
        KNOWN(px[0] > 0xc0 && px[1] < 0x40, "screenshot must show the frame as presented (red), got %02x%02x%02x (clear after present leaked into it)", px[0], px[1], px[2]);
    }
    return h_finish("t_present_snap", &h);
}
