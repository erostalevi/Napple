/* VideoToolbox DecoderBackend (decoder_vt.c) behind decoder_backend.h: encodes a clip with VideoToolbox, feeds access units through the backend as NVDEC would
   (configure with the Annex B parameter sets, one decode per request, no reordering by the backend) and checks size, content and a frame checksum. Earlier header:  encode a generated clip with VideoToolbox, decode it through vt_h264.c in Annex B and
   AVCC form, with and without frame reordering; JPEG encode/decode round trip. Links vt_h264.c alone. */
#include "check.h"
#include "decoder_backend.h"
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
static uint8_t *annexb(const Clip *c, int i, int with_ps, size_t *out) {   /* AVCC sample -> Annex B (+ SPS/PPS) */
    uint8_t *b = malloc(c->len[i] + c->spsn + c->ppsn + 16); size_t n = 0;
    if (with_ps) { memcpy(b + n, "\0\0\0\1", 4); n += 4; memcpy(b + n, c->sps, c->spsn); n += c->spsn; memcpy(b + n, "\0\0\0\1", 4); n += 4; memcpy(b + n, c->pps, c->ppsn); n += c->ppsn; }
    for (size_t o = 0; o + 4 <= c->len[i];) {
        size_t l = ((size_t)c->data[i][o] << 24) | (c->data[i][o + 1] << 16) | (c->data[i][o + 2] << 8) | c->data[i][o + 3]; o += 4;
        memcpy(b + n, "\0\0\0\1", 4); n += 4; memcpy(b + n, c->data[i] + o, l); n += l; o += l;
    }
    *out = n; return b;
}

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n) { while (n--) h = (h ^ *p++) * 16777619u; return h; }
static uint32_t run_clip(const Clip *c, int *ok) {
    const DecoderBackend *b = decoder_backend_current();
    CHECK(!strcmp(b->name, "videotoolbox"), "constructor installed the VideoToolbox backend (%s)", b->name);
    CHECK(b->open(DEC_CODEC_VP9) == NULL, "VP9 refused");
    void *ctx = b->open(DEC_CODEC_H264);
    CHECK(ctx != NULL, "open h264");
    uint32_t sum = 2166136261u; *ok = 1;
    uint8_t luma[W * H], chroma[W * H / 2];
    for (int i = 0; i < N; i++) {
        size_t n; uint8_t *bs = annexb(c, i, i == 0, &n);
        if (i == 0) {
            DecoderConfig cfg; memset(&cfg, 0, sizeof cfg); cfg.codec = DEC_CODEC_H264; cfg.width = W; cfg.height = H;
            size_t o = 0; memcpy(cfg.h264_annexb, "\0\0\0\1", 4); o = 4; memcpy(cfg.h264_annexb + o, c->sps, c->spsn); o += c->spsn;
            memcpy(cfg.h264_annexb + o, "\0\0\0\1", 4); o += 4; memcpy(cfg.h264_annexb + o, c->pps, c->ppsn); o += c->ppsn; cfg.h264_annexb_size = o;
            CHECK(b->configure(ctx, &cfg) == 0, "configure");
        }
        DecodeRequest r = {DEC_CODEC_H264, NULL, 0, bs, n, 0, luma, chroma, sizeof luma, sizeof chroma, 0, 0};
        DecodeResult out; memset(&out, 0, sizeof out);
        memset(luma, 0, sizeof luma);
        int rc = b->decode(ctx, &r, &out);
        CHECK(rc == 0, "decode %d returned %d", i, rc);
        if (rc == 0) {
            CHECK(out.width == W && out.height == H && out.pitch == W, "frame %d size %dx%d pitch %d", i, out.width, out.height, out.pitch);
            long err = 0; for (int x = 0; x < W; x++) err += abs((int)luma[(H / 2) * W + x] - (40 + x + i * 4));
            CHECK(err / W < 10, "frame %d luma error %ld", i, err / W);
            CHECK(abs(chroma[0] - 100) < 6 && abs(chroma[1] - 160) < 6, "frame %d chroma", i);
            sum = fnv(fnv(sum, luma, sizeof luma), chroma, sizeof chroma);
        } else *ok = 0;
        free(bs);
    }
    /* garbage and an empty request are errors, not crashes; a slice after reset() and before new sets gives "no frame" or an error */
    uint8_t junk[] = {0, 0, 0, 1, 0x65, 1, 2, 3}; uint8_t l2[W * H], c2[W * H / 2]; DecodeResult o2;
    DecodeRequest jr = {DEC_CODEC_H264, NULL, 0, junk, sizeof junk, 0, l2, c2, sizeof l2, sizeof c2};
    b->reset(ctx);
    (void)b->decode(ctx, &jr, &o2);
    DecodeRequest er = {DEC_CODEC_H264, NULL, 0, NULL, 0, 0, l2, c2, sizeof l2, sizeof c2};
    CHECK(b->decode(ctx, &er, &o2) < 0, "empty request is an error");
    b->close(ctx);
    return sum;
}
/* The guest's surfaces have a row pitch above the width (NVDEC picture-info pitches): rows land at the pitch, the padding is not touched, the same pixels
 * as the packed decode come out, and a surface one row short is refused. */
static void run_pitched(const Clip *c, uint32_t packed_sum) {
    enum { P = W + 64, PC = W + 32 };
    const DecoderBackend *b = decoder_backend_current();
    void *ctx = b->open(DEC_CODEC_H264);
    DecoderConfig cfg; memset(&cfg, 0, sizeof cfg); cfg.codec = DEC_CODEC_H264; cfg.width = W; cfg.height = H;
    size_t o = 0; memcpy(cfg.h264_annexb, "\0\0\0\1", 4); o = 4; memcpy(cfg.h264_annexb + o, c->sps, c->spsn); o += c->spsn;
    memcpy(cfg.h264_annexb + o, "\0\0\0\1", 4); o += 4; memcpy(cfg.h264_annexb + o, c->pps, c->ppsn); o += c->ppsn; cfg.h264_annexb_size = o;
    CHECK(ctx && b->configure(ctx, &cfg) == 0, "pitched: open + configure");
    static uint8_t luma[P * H], chroma[PC * H / 2], pl[W * H], pc[W * H / 2];
    uint32_t sum = 2166136261u; int pad_ok = 1;
    for (int i = 0; i < N; i++) {
        size_t n; uint8_t *bs = annexb(c, i, i == 0, &n);
        memset(luma, 0xEE, sizeof luma); memset(chroma, 0xEE, sizeof chroma);
        DecodeRequest r = {DEC_CODEC_H264, NULL, 0, bs, n, 0, luma, chroma, sizeof luma, sizeof chroma, P, PC};
        DecodeResult out; memset(&out, 0, sizeof out);
        int rc = b->decode(ctx, &r, &out);
        CHECK(rc == 0 && out.width == W && out.height == H && out.pitch == P, "pitched frame %d: rc %d %dx%d pitch %d", i, rc, out.width, out.height, out.pitch);
        for (int y = 0; y < H; y++) { memcpy(pl + y * W, luma + y * P, W); for (int x = W; x < P; x++) pad_ok &= luma[y * P + x] == 0xEE; }
        for (int y = 0; y < H / 2; y++) { memcpy(pc + y * W, chroma + y * PC, W); for (int x = W; x < PC; x++) pad_ok &= chroma[y * PC + x] == 0xEE; }
        sum = fnv(fnv(sum, pl, sizeof pl), pc, sizeof pc);
        free(bs);
    }
    CHECK(pad_ok, "pitched: bytes between width and pitch untouched");
    CHECK(sum == packed_sum, "pitched decode gives the packed frames (%08x vs %08x)", sum, packed_sum);
    size_t n; uint8_t *bs = annexb(c, 0, 1, &n); DecodeResult out;
    DecodeRequest small = {DEC_CODEC_H264, NULL, 0, bs, n, 0, luma, chroma, (size_t)P * (H - 1), sizeof chroma, P, PC};
    b->reset(ctx); b->configure(ctx, &cfg);
    CHECK(b->decode(ctx, &small, &out) < 0, "pitched: luma surface one row short is refused");
    DecodeRequest bad = {DEC_CODEC_H264, NULL, 0, bs, n, 0, luma, chroma, sizeof luma, sizeof chroma, W / 2, 0}; /* pitch below the width: packed */
    b->reset(ctx); b->configure(ctx, &cfg);
    CHECK(b->decode(ctx, &bad, &out) == 0 && out.pitch == W, "pitch below the width falls back to packed (pitch %d)", out.pitch);
    free(bs);
    b->close(ctx);
}
/* MSENC's VideoToolbox H.264 encoder (b8, written on Linux, first run on a Mac): pitched NV12 in, Annex B out, SPS/PPS before the key frame. */
static void run_encoder(void) {
    const EncoderBackend *e = &encoder_backend_videotoolbox;
    CHECK(e->open(DEC_CODEC_VP9) == NULL, "encoder refuses VP9");
    void *ctx = e->open(DEC_CODEC_H264);
    CHECK(ctx != NULL, "encoder opens H.264");
    if (!ctx) return;
    enum { EW = 64, EH = 48, EP = 96 };
    static uint8_t y[EP * EH], uv[EP * EH / 2], out[1 << 16];
    memset(y, 0x55, sizeof y);
    memset(uv, 0x80, sizeof uv);
    EncodeRequest r = {DEC_CODEC_H264, EW, EH, y, uv, sizeof y, sizeof uv, EP, EP, 500000, 30, 0, out, sizeof out};
    EncodeResult res = {0};
    int rc = e->encode(ctx, &r, &res);
    CHECK(rc == 0 && res.idr && res.size > 16 && !memcmp(out, "\0\0\0\1", 4) && (out[4] & 31) == 7, "first frame: key frame led by an SPS (rc %d, %zu bytes)", rc, res.size);
    memset(y, 0x60, sizeof y);
    rc = e->encode(ctx, &r, &res);
    CHECK(rc == 0 && res.size > 4 && !memcmp(out, "\0\0\0\1", 4), "second frame encodes (rc %d, %zu bytes, idr %d)", rc, res.size, res.idr);
    r.force_idr = 1;
    rc = e->encode(ctx, &r, &res);
    CHECK(rc == 0 && res.idr && (out[4] & 31) == 7, "force-IDR gives a key frame with parameter sets");
    r.out_cap = 8;
    CHECK(e->encode(ctx, &r, &res) < 0, "an output buffer that is too small is an error");
    e->close(ctx);
}
int main(void) {
    run_encoder();
    Clip c;
    if (!encode(&c, 0, 1)) { CHECK(0, "VideoToolbox encode failed"); return 1; }
    int ok1, ok2;
    uint32_t s1 = run_clip(&c, &ok1), s2 = run_clip(&c, &ok2);
    CHECK(s1 == s2, "decoding is deterministic (%08x vs %08x)", s1, s2);
    printf("frame checksum over %d frames: %08x\n", N, s1);
    run_pitched(&c, s1);
    /* too-small surface */
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
