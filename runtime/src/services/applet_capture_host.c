/* The capture frame source on the Mac: the last frame the graphics back end completed (napple_gfx_screenshot_last, a PNG of the screen image) decoded
 * and scaled into the w x h RGBA8 the capture slots hold. Registered by service_register_applet, so IDisplayController's capture images,
 * TakeScreenShotOfOwnLayer(Ex) and ISelfController::SaveCurrentScreenshot see what the game drew instead of black. Elsewhere there is no source. */
#include "applet_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include "napple_gfx.h"

static int last_frame_rgba(uint8_t *rgba, uint32_t w, uint32_t h) {
    char path[1024];
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/napple-capture-%d.png", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    if (napple_gfx_screenshot_last(path)) return 0; /* no frame completed yet */
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageSourceRef src = url ? CGImageSourceCreateWithURL(url, NULL) : NULL;
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    int ok = 0;
    if (img) {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(rgba, w, h, 8, (size_t)w * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        if (ctx) {
            CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
            CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
            CGContextRelease(ctx);
            for (size_t i = 0; i < (size_t)w * h; i++) rgba[i * 4 + 3] = 255;
            ok = 1;
        }
        CGColorSpaceRelease(cs);
        CGImageRelease(img);
    }
    if (src) CFRelease(src);
    if (url) CFRelease(url);
    unlink(path);
    return ok;
}
void applet_capture_register_host_source(void) { applet_capture_set_source(last_frame_rgba); }
#else
void applet_capture_register_host_source(void) {}
#endif
