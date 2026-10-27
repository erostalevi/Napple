/* H.264 via VideoToolbox, JPEG via ImageIO. See vt_h264.h. */
#include "vt_h264.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <ImageIO/ImageIO.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_HELD = 6, MAX_PS = 4 };
typedef struct {
    uint8_t *nv12;
    int64_t pts;
    int full, bt601;
} Held;
struct VtH264 {
    uint8_t *sps[MAX_PS], *pps[MAX_PS];
    size_t sps_n[MAX_PS], pps_n[MAX_PS];
    int nsps, npps;
    int avcc; /* input is length-prefixed */
    CMVideoFormatDescriptionRef fd;
    VTDecompressionSessionRef vt;
    int w, h, depth; /* reorder depth: 0 for baseline, else 2 */
    Held held[MAX_HELD];
    int nheld;
    int fail; /* set by the decode callback */
};

VtH264 *vth264_create(void) { return calloc(1, sizeof(VtH264)); }
static void drop_session(VtH264 *d) {
    if (d->vt) {
        VTDecompressionSessionInvalidate(d->vt);
        CFRelease(d->vt);
        d->vt = NULL;
    }
    if (d->fd) {
        CFRelease(d->fd);
        d->fd = NULL;
    }
    for (int i = 0; i < d->nheld; i++) free(d->held[i].nv12);
    d->nheld = 0;
}
void vth264_reset(VtH264 *d) { drop_session(d); }
void vth264_destroy(VtH264 *d) {
    if (!d) return;
    drop_session(d);
    for (int i = 0; i < d->nsps; i++) free(d->sps[i]);
    for (int i = 0; i < d->npps; i++) free(d->pps[i]);
    free(d);
}
int vth264_held(const VtH264 *d) { return d->nheld; }
int vth264_width(const VtH264 *d) { return d->w; }
int vth264_height(const VtH264 *d) { return d->h; }

/* Walk Annex B NAL units: calls f(nal, len) for each. */
static size_t start_code(const uint8_t *p, size_t n, size_t i) { /* length of the start code at i, or 0 */
    if (i + 3 <= n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return 3;
    if (i + 4 <= n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) return 4;
    return 0;
}
static int is_annexb(const uint8_t *p, size_t n) { return start_code(p, n, 0) != 0; }
typedef int (*NalFn)(void *u, const uint8_t *nal, size_t len);
static int each_nal(const uint8_t *p, size_t n, int annexb, NalFn f, void *u) {
    if (annexb) {
        size_t i = 0;
        while (i < n && !start_code(p, n, i)) i++;
        while (i < n) {
            size_t s = start_code(p, n, i), b = i + s, e = b;
            while (e < n && !start_code(p, n, e)) e++;
            size_t len = e;
            while (len > b && p[len - 1] == 0 && e < n) len--; /* trailing zero of the next 4-byte start code */
            if (len > b && !f(u, p + b, len - b)) return 0;
            i = e;
        }
    } else {
        size_t i = 0;
        while (i + 4 <= n) {
            size_t len = ((size_t)p[i] << 24) | (p[i + 1] << 16) | (p[i + 2] << 8) | p[i + 3];
            i += 4;
            if (len == 0 || len > n - i) return 0;
            if (!f(u, p + i, len)) return 0;
            i += len;
        }
    }
    return 1;
}
static void add_ps(uint8_t **arr, size_t *lens, int *n, const uint8_t *nal, size_t len) {
    for (int i = 0; i < *n; i++)
        if (lens[i] == len && !memcmp(arr[i], nal, len)) return;
    if (*n == MAX_PS) return;
    arr[*n] = malloc(len);
    memcpy(arr[*n], nal, len);
    lens[*n] = len;
    (*n)++;
}
typedef struct {
    VtH264 *d;
    int changed, idr, slice;
} Scan;
static int scan_nal(void *u, const uint8_t *nal, size_t len) {
    Scan *s = u;
    int t = nal[0] & 31;
    if (t == 7) {
        int before = s->d->nsps;
        add_ps(s->d->sps, s->d->sps_n, &s->d->nsps, nal, len);
        s->changed |= s->d->nsps != before;
        if (len > 1 && nal[1] == 66) s->d->depth = 0;
        else s->d->depth = 2;
    } else if (t == 8) {
        int before = s->d->npps;
        add_ps(s->d->pps, s->d->pps_n, &s->d->npps, nal, len);
        s->changed |= s->d->npps != before;
    } else if (t == 5) s->idr = 1;
    if (t >= 1 && t <= 5) s->slice = 1;
    return 1;
}

/* Re-order and deliver. */
static void deliver_one(VtH264 *d, int i, VtH264Frame cb, void *user, int *count) {
    Held *h = &d->held[i];
    if (cb) cb(user, h->nv12, d->w, d->h, h->pts, h->full, h->bt601);
    (*count)++;
    free(h->nv12);
    memmove(d->held + i, d->held + i + 1, (size_t)(--d->nheld - i) * sizeof *h);
}
static int lowest(const VtH264 *d) {
    int b = 0;
    for (int i = 1; i < d->nheld; i++)
        if (d->held[i].pts < d->held[b].pts) b = i;
    return b;
}

static void on_frame(void *refcon, void *src, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef img, CMTime pts, CMTime dur) {
    VtH264 *d = refcon;
    (void)src;
    (void)flags;
    (void)dur;
    if (status != noErr || !img) {
        d->fail = 1;
        return;
    }
    if (CVPixelBufferGetPixelFormatType(img) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
        CVPixelBufferGetPixelFormatType(img) != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        d->fail = 1;
        return;
    }
    int w = (int)CVPixelBufferGetWidth(img), h = (int)CVPixelBufferGetHeight(img);
    if (w < d->w || h < d->h || d->nheld == MAX_HELD) {
        d->fail = 1;
        return;
    }
    uint8_t *out = malloc((size_t)d->w * d->h * 3 / 2);
    if (!out) {
        d->fail = 1;
        return;
    }
    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(img, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(img, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(img, 0), us = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
    for (int r = 0; r < d->h; r++) memcpy(out + (size_t)r * d->w, y + r * ys, (size_t)d->w);
    for (int r = 0; r < d->h / 2; r++) memcpy(out + (size_t)d->w * d->h + (size_t)r * d->w, uv + r * us, (size_t)d->w);
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    CFTypeRef mat = CVBufferCopyAttachment(img, kCVImageBufferYCbCrMatrixKey, NULL);
    int bt601 = mat && CFEqual(mat, kCVImageBufferYCbCrMatrix_ITU_R_601_4);
    if (mat) CFRelease(mat);
    d->held[d->nheld++] =
        (Held){out, CMTIME_IS_VALID(pts) ? pts.value : 0, CVPixelBufferGetPixelFormatType(img) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, bt601};
}

static int make_session(VtH264 *d) {
    drop_session(d);
    if (!d->nsps || !d->npps) return 0;
    const uint8_t *ps[2 * MAX_PS];
    size_t sz[2 * MAX_PS];
    int n = 0;
    ps[n] = d->sps[0];
    sz[n++] = d->sps_n[0];
    for (int i = 0; i < d->npps; i++) {
        ps[n] = d->pps[i];
        sz[n++] = d->pps_n[i];
    }
    if (CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, (size_t)n, ps, sz, 4, &d->fd) != noErr) return 0;
    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(d->fd);
    d->w = dim.width;
    d->h = dim.height;
    if (d->w <= 0 || d->h <= 0 || ((d->w | d->h) & 1)) {
        CFRelease(d->fd);
        d->fd = NULL;
        return 0;
    }
    int fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef f = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    const void *keys[] = {kCVPixelBufferPixelFormatTypeKey}, *vals[] = {f};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(f);
    VTDecompressionOutputCallbackRecord rec = {on_frame, d};
    OSStatus st = VTDecompressionSessionCreate(NULL, d->fd, NULL, attrs, &rec, &d->vt);
    CFRelease(attrs);
    if (st != noErr) {
        CFRelease(d->fd);
        d->fd = NULL;
        d->vt = NULL;
        return 0;
    }
    return 1;
}

typedef struct {
    uint8_t *buf;
    size_t n;
} Avcc;
static int to_avcc(void *u, const uint8_t *nal, size_t len) {
    Avcc *a = u;
    int t = nal[0] & 31;
    if (t == 7 || t == 8 || t == 9 || t == 12) return 1; /* parameter sets live in the format description; drop AUD and filler */
    a->buf[a->n++] = (uint8_t)(len >> 24);
    a->buf[a->n++] = (uint8_t)(len >> 16);
    a->buf[a->n++] = (uint8_t)(len >> 8);
    a->buf[a->n++] = (uint8_t)len;
    memcpy(a->buf + a->n, nal, len);
    a->n += len;
    return 1;
}

int vth264_config(VtH264 *d, const uint8_t *annexb, size_t size) {
    Scan s = {d, 0, 0, 0};
    if (!is_annexb(annexb, size) || !each_nal(annexb, size, 1, scan_nal, &s)) return -1;
    return d->nsps && d->npps ? 0 : -1;
}
int vth264_flush(VtH264 *d, VtH264Frame cb, void *user) {
    int count = 0;
    if (d->vt) VTDecompressionSessionWaitForAsynchronousFrames(d->vt);
    while (d->nheld) deliver_one(d, lowest(d), cb, user, &count);
    return count;
}
int vth264_decode(VtH264 *d, const uint8_t *data, size_t size, int64_t pts, VtH264Frame cb, void *user) {
    if (!size) return 0;
    int annexb = is_annexb(data, size);
    Scan s = {d, 0, 0, 0};
    if (!each_nal(data, size, annexb, scan_nal, &s)) return -1;
    if (getenv("NAPPLE_MOVIE_TC")) {
        static int said;
        if (said < 3) { fprintf(stderr, "[vth264] head:"); for (size_t i = 0; i < 48 && i < size; i++) fprintf(stderr, " %02x", data[i]); fprintf(stderr, "\n"); }
        if (said++ < 10) fprintf(stderr, "[vth264] size %zu annexb %d changed %d slice %d idr %d vt %p nsps %d npps %d w %d h %d depth %d nheld %d\n", size, annexb, s.changed, s.slice, s.idr, (void *)d->vt, d->nsps, d->npps, d->w, d->h, d->depth, d->nheld);
    }
    if (s.changed || (!d->vt && s.slice)) {
        if (s.changed && d->vt) vth264_flush(d, cb, user); /* parameter sets changed: finish the old stream first */
        if (!make_session(d)) return s.slice ? 0 : (d->nsps && d->npps ? -1 : 0);
    }
    if (!d->vt || !s.slice) return 0;
    Avcc a = {malloc(size + size / 2 + 64), 0};
    if (!a.buf) return -1;
    each_nal(data, size, annexb, to_avcc, &a);
    if (!a.n) {
        free(a.buf);
        return 0;
    }
    CMBlockBufferRef bb = NULL;
    CMSampleBufferRef sb = NULL;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, a.buf, a.n, kCFAllocatorMalloc, NULL, 0, a.n, 0, &bb); /* takes ownership of a.buf */
    if (st != noErr) {
        free(a.buf);
        return -1;
    }
    CMSampleTimingInfo ti = {kCMTimeInvalid, CMTimeMake(pts, 1), kCMTimeInvalid};
    size_t one = a.n;
    st = CMSampleBufferCreateReady(NULL, bb, d->fd, 1, 1, &ti, 1, &one, &sb);
    CFRelease(bb);
    if (st != noErr) return -1;
    d->fail = 0;
    st = VTDecompressionSessionDecodeFrame(d->vt, sb, 0, NULL, NULL);
    CFRelease(sb);
    if (st == noErr) st = VTDecompressionSessionWaitForAsynchronousFrames(d->vt);
    if (st != noErr || d->fail) return -1;
    int count = 0;
    while (d->nheld > d->depth) deliver_one(d, lowest(d), cb, user, &count);
    return count;
}

/* ---- JPEG ---- */
uint8_t *jpeg_decode_rgba(const void *data, size_t size, int *width, int *height) {
    if (!data || !size) return NULL;
    CFDataRef cd = CFDataCreate(NULL, data, (CFIndex)size);
    CGImageSourceRef src = CGImageSourceCreateWithData(cd, NULL);
    CFRelease(cd);
    if (!src) return NULL;
    CGImageRef img = CGImageSourceGetCount(src) ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    CFRelease(src);
    if (!img) return NULL;
    int w = (int)CGImageGetWidth(img), h = (int)CGImageGetHeight(img);
    uint8_t *px = (w > 0 && h > 0) ? calloc((size_t)w * h, 4) : NULL;
    if (px) {
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx =
            CGBitmapContextCreate(px, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cs);
        if (ctx) {
            CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
            CGContextRelease(ctx);
            *width = w;
            *height = h;
        } else {
            free(px);
            px = NULL;
        }
    }
    CGImageRelease(img);
    return px;
}
uint8_t *jpeg_encode_rgba(const uint8_t *rgba, int width, int height, int stride, float quality, size_t *out_size) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate((void *)rgba, (size_t)width, (size_t)height, 8, (size_t)stride, cs,
                                             (CGBitmapInfo)kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) return NULL;
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CFMutableDataRef out = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(out, CFSTR("public.jpeg"), 1, NULL);
    uint8_t *res = NULL;
    if (dst && img) {
        CFNumberRef q = CFNumberCreate(NULL, kCFNumberFloatType, &quality);
        const void *k[] = {kCGImageDestinationLossyCompressionQuality}, *v[] = {q};
        CFDictionaryRef opt = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CGImageDestinationAddImage(dst, img, opt);
        CFRelease(opt);
        CFRelease(q);
        if (CGImageDestinationFinalize(dst)) {
            *out_size = (size_t)CFDataGetLength(out);
            res = malloc(*out_size);
            if (res) memcpy(res, CFDataGetBytePtr(out), *out_size);
        }
    }
    if (dst) CFRelease(dst);
    if (img) CGImageRelease(img);
    CFRelease(out);
    return res;
}
