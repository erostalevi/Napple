/* H.264 (VideoToolbox) and JPEG (ImageIO) host decoders: encode a generated clip with VideoToolbox, decode it through vt_h264.c in Annex B and
   AVCC form, with and without frame reordering; JPEG encode/decode round trip. Links vt_h264.c alone. */
#include "check.h"
#include "vt_h264.h"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

enum { W = 160, H = 96, N = 12 };
typedef struct { uint8_t *data[N]; size_t len[N]; int64_t pts[N]; int n; uint8_t *sps, *pps; size_t spsn, ppsn; } Clip;

static void on_encoded(void *ref, void *src, OSStatus st, VTEncodeInfoFlags fl, CMSampleBufferRef sb) {
    Clip *c = ref; (void)src; (void)fl;
    if (st != noErr || !sb || c->n == N) return;
    if (!c->sps) {
        CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb); const uint8_t *p; size_t n;
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, &p, &n, NULL, NULL); c->sps = malloc(n); memcpy(c->sps, p, n); c->spsn = n;
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 1, &p, &n, NULL, NULL); c->pps = malloc(n); memcpy(c->pps, p, n); c->ppsn = n;
    }
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb); size_t len = CMBlockBufferGetDataLength(bb);
    c->data[c->n] = malloc(len); c->len[c->n] = len; CMBlockBufferCopyDataBytes(bb, 0, len, c->data[c->n]);
    c->pts[c->n] = CMSampleBufferGetPresentationTimeStamp(sb).value; c->n++;
}
static int encode(Clip *c, int reorder, int baseline) {
    memset(c, 0, sizeof *c);
    VTCompressionSessionRef s;
    if (VTCompressionSessionCreate(NULL, W, H, kCMVideoCodecType_H264, NULL, NULL, NULL, on_encoded, c, &s) != noErr) return 0;
    VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, reorder ? kCFBooleanTrue : kCFBooleanFalse);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel, baseline ? kVTProfileLevel_H264_Baseline_AutoLevel : kVTProfileLevel_H264_High_AutoLevel);
    int br = 2000000; CFNumberRef brn = CFNumberCreate(NULL, kCFNumberIntType, &br);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_AverageBitRate, brn); CFRelease(brn);
    for (int i = 0; i < N; i++) {
        CVPixelBufferRef pb; CVPixelBufferCreate(NULL, W, H, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, NULL, &pb);
        CVPixelBufferLockBaseAddress(pb, 0);
        uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), us = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
        for (int r = 0; r < H; r++) for (int x = 0; x < W; x++) y[r * ys + x] = (uint8_t)(40 + x + i * 4);
        for (int r = 0; r < H / 2; r++) for (int x = 0; x < W; x += 2) { uv[r * us + x] = 100; uv[r * us + x + 1] = 160; }
        CVPixelBufferUnlockBaseAddress(pb, 0);
        VTCompressionSessionEncodeFrame(s, pb, CMTimeMake(i, 1), kCMTimeInvalid, NULL, NULL, NULL);
        CVPixelBufferRelease(pb);
    }
    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
    VTCompressionSessionInvalidate(s); CFRelease(s);
    return c->n == N;
}
typedef struct { int n, bad; int64_t last; int ordered; } Seen;
static void on_decoded(void *u, const uint8_t *nv12, int w, int h, int64_t pts, int full, int bt601) {
    Seen *s = u; (void)full; (void)bt601;
    if (w != W || h != H || pts < 0 || pts >= N) { s->bad++; return; }
    if (pts <= s->last && s->n) s->ordered = 0;
    s->last = pts;
    long err = 0; for (int x = 0; x < W; x++) err += abs((int)nv12[(H / 2) * W + x] - (40 + x + (int)pts * 4));
    if (err / W > 10) s->bad++;
    if (abs(nv12[W * H] - 100) > 6 || abs(nv12[W * H + 1] - 160) > 6) s->bad++;
    s->n++;
}
static uint8_t *annexb(const Clip *c, int i, int with_ps, size_t *out) {   /* AVCC sample -> Annex B (+ SPS/PPS) */
    uint8_t *b = malloc(c->len[i] + c->spsn + c->ppsn + 16); size_t n = 0;
    if (with_ps) { memcpy(b + n, "\0\0\0\1", 4); n += 4; memcpy(b + n, c->sps, c->spsn); n += c->spsn; memcpy(b + n, "\0\0\0\1", 4); n += 4; memcpy(b + n, c->pps, c->ppsn); n += c->ppsn; }
    for (size_t o = 0; o + 4 <= c->len[i];) {
        size_t l = ((size_t)c->data[i][o] << 24) | (c->data[i][o + 1] << 16) | (c->data[i][o + 2] << 8) | c->data[i][o + 3]; o += 4;
        memcpy(b + n, "\0\0\0\1", 4); n += 4; memcpy(b + n, c->data[i] + o, l); n += l; o += l;
    }
    *out = n; return b;
}
static void run(const char *name, int reorder, int baseline, int use_annexb) {
    Clip c; if (!encode(&c, reorder, baseline)) { CHECK(0, "%s: VideoToolbox encode failed", name); return; }
    VtH264 *d = vth264_create(); Seen s = {0, 0, -1, 1};
    if (use_annexb) {
        for (int i = 0; i < N; i++) { size_t n; uint8_t *b = annexb(&c, i, i == 0, &n); CHECK(vth264_decode(d, b, n, c.pts[i], on_decoded, &s) >= 0, "%s: decode %d", name, i); free(b); }
    } else {   /* csd-0/csd-1 first, then raw AVCC samples */
        uint8_t csd[512]; size_t n = 0;
        memcpy(csd, "\0\0\0\1", 4); n = 4; memcpy(csd + n, c.sps, c.spsn); n += c.spsn; memcpy(csd + n, "\0\0\0\1", 4); n += 4; memcpy(csd + n, c.pps, c.ppsn); n += c.ppsn;
        CHECK(vth264_config(d, csd, n) == 0, "%s: config", name);
        for (int i = 0; i < N; i++) {
            if (i == 0) { /* first sample creates the session through the config's parameter sets */ }
            CHECK(vth264_decode(d, c.data[i], c.len[i], c.pts[i], on_decoded, &s) >= 0, "%s: decode %d", name, i);
        }
    }
    vth264_flush(d, on_decoded, &s);
    CHECK(vth264_width(d) == W && vth264_height(d) == H, "%s: size %dx%d", name, vth264_width(d), vth264_height(d));
    CHECK(s.n == N, "%s: %d of %d frames", name, s.n, N);
    CHECK(s.bad == 0, "%s: %d frames with wrong size or content", name, s.bad);
    CHECK(s.ordered, "%s: frames out of presentation order", name);
    printf("%s: %d frames, %d bad\n", name, s.n, s.bad);
    vth264_destroy(d);
    for (int i = 0; i < N; i++) free(c.data[i]);
    free(c.sps); free(c.pps);
}
int main(void) {
    run("baseline annexb", 0, 1, 1);
    run("high annexb reorder", 1, 0, 1);
    run("high avcc+csd reorder", 1, 0, 0);

    VtH264 *d = vth264_create(); uint8_t junk[] = {0, 0, 0, 1, 0x65, 1, 2, 3};
    CHECK(vth264_decode(d, junk, sizeof junk, 0, on_decoded, NULL) == 0, "slice before parameter sets is dropped");
    vth264_destroy(d);

    enum { JW = 64, JH = 48 }; uint8_t px[JW * JH * 4];
    for (int y = 0; y < JH; y++) for (int x = 0; x < JW; x++) { uint8_t *p = px + (y * JW + x) * 4; p[0] = 200; p[1] = (uint8_t)(x * 4); p[2] = 30; p[3] = 255; }
    size_t jn = 0; uint8_t *jpg = jpeg_encode_rgba(px, JW, JH, JW * 4, 0.95f, &jn);
    CHECK(jpg && jn > 100 && jpg[0] == 0xFF && jpg[1] == 0xD8, "jpeg encode");
    int w = 0, h = 0; uint8_t *back = jpg ? jpeg_decode_rgba(jpg, jn, &w, &h) : NULL;
    CHECK(back && w == JW && h == JH, "jpeg decode size %dx%d", w, h);
    if (back) {
        long err = 0; for (int i = 0; i < JW * JH; i++) for (int k = 0; k < 3; k++) err += abs(back[i * 4 + k] - px[i * 4 + k]);
        CHECK(err / (JW * JH * 3) < 8, "jpeg round trip error %ld", err / (JW * JH * 3));
        CHECK(back[3] == 255, "jpeg alpha");
    }
    CHECK(jpeg_decode_rgba("not a jpeg", 10, &w, &h) == NULL, "garbage rejected");
    CHECK(jpeg_decode_rgba(NULL, 0, &w, &h) == NULL, "empty rejected");
    free(jpg); free(back);
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
