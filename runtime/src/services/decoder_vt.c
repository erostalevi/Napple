/* VideoToolbox DecoderBackend for the nvdrv NVDEC engine (H.264; VP9 opt-in) and EncoderBackend for MSENC (H.264). VP9 policy (b8): open(VP9) succeeds only with
 * NAPPLE_VT_VP9=1 AND when VideoToolbox reports a VP9 decoder (VTRegisterSupplementalVideoDecoderIfAvailable + VTIsHardwareDecodeSupported); in every other case
 * it returns NULL, the engine reports "codec not supported" (status error 1) in the guest's status structure and the channel's summary line, and VP9 movies keep
 * going through movie.c's libvpx path at the media library level. VP8/H.265 are always refused. The VP9 path (VTDecompressionSession with a vpcC record from
 * configure()) was written on Linux and has never run: UNBUILT, hence the opt-in.
 * Wraps vt_h264.c: decode() is synchronous, so each request is decoded and flushed, and the single frame produced is copied to the guest surface.
 * Installed at startup by a constructor; NAPPLE_DECODER=null keeps the null backend. Host (Linux) builds do not compile this file. */
#include "decoder_backend.h"
#include "vt_h264.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int codec;
    /* VP9 (VTDecompressionSession) */
    CMVideoFormatDescriptionRef vfd;
    VTDecompressionSessionRef vs;
    int vp9_w, vp9_h, vp9_full, vp9_709;
    /* H.264 (vt_h264.c) */
    VtH264 *d;
    int64_t pts;
    uint8_t *nv12; /* last frame produced by the current request (packed, stride = w) */
    int w, h, full, bt601, got;
} VtCtx;

static void on_frame(void *user, const uint8_t *nv12, int w, int h, int64_t pts, int full, int bt601) {
    VtCtx *c = user;
    (void)pts;
    free(c->nv12);
    c->nv12 = calloc(1, (size_t)w * (h + (h + 1) / 2)); /* room for the last chroma row of an odd height */
    if (!c->nv12) return;
    memcpy(c->nv12, nv12, (size_t)w * h * 3 / 2);
    c->w = w, c->h = h, c->full = full, c->bt601 = bt601, c->got = 1;
}

static int vp9_available(void) {
    const char *e = getenv("NAPPLE_VT_VP9");
    if (!e || !*e || *e == '0') return 0;
    static int known = -1;
    if (known < 0) {
        if (__builtin_available(macOS 11.0, *)) {
            VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
            known = VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9) ? 1 : 0;
        } else known = 0;
    }
    return known;
}
static void vp9_drop(VtCtx *c) {
    if (c->vs) {
        VTDecompressionSessionInvalidate(c->vs);
        CFRelease(c->vs);
        c->vs = NULL;
    }
    if (c->vfd) {
        CFRelease(c->vfd);
        c->vfd = NULL;
    }
}
/* Decoded VP9 picture -> packed NV12 in the context (same copy as on_frame, from a locked CVPixelBuffer). */
static void vp9_on_frame(void *refcon, void *src, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef img, CMTime pts, CMTime dur) {
    VtCtx *c = refcon;
    (void)src, (void)flags, (void)pts, (void)dur;
    if (status != noErr || !img) return;
    if (CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    int w = (int)CVPixelBufferGetWidth(img), h = (int)CVPixelBufferGetHeight(img);
    if (CVPixelBufferGetPlaneCount(img) == 2 && w > 0 && h > 0) {
        free(c->nv12);
        c->nv12 = calloc(1, (size_t)w * (h + (h + 1) / 2));
        if (c->nv12) {
            const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(img, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(img, 1);
            size_t yp = CVPixelBufferGetBytesPerRowOfPlane(img, 0), uvp = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
            for (int r = 0; r < h; r++) memcpy(c->nv12 + (size_t)r * w, y + r * yp, (size_t)w);
            for (int r = 0; r < (h + 1) / 2; r++) memcpy(c->nv12 + (size_t)w * h + (size_t)r * w, uv + r * uvp, (size_t)w);
            c->w = w, c->h = h, c->full = c->vp9_full, c->bt601 = !c->vp9_709, c->got = 1;
        }
    }
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
}
/* A key frame's size/profile/colour config -> a decode session. Profile 0 (8-bit 4:2:0) only; anything else is refused. */
static int vp9_configure(VtCtx *c, const DecoderConfig *cfg) {
    vp9_drop(c);
    if (cfg->codec != DEC_CODEC_VP9 || !cfg->vpcc_size || cfg->width <= 0 || cfg->height <= 0 || cfg->vp9.profile != 0 || cfg->vp9.bit_depth != 8) return -1;
    CFDataRef vpcc = CFDataCreate(NULL, cfg->vpcc, (CFIndex)cfg->vpcc_size);
    CFMutableDictionaryRef atoms = CFDictionaryCreateMutable(NULL, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef ext = CFDictionaryCreateMutable(NULL, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(atoms, CFSTR("vpcC"), vpcc);
    CFDictionarySetValue(ext, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms);
    OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_VP9, cfg->width, cfg->height, ext, &c->vfd);
    CFRelease(ext), CFRelease(atoms), CFRelease(vpcc);
    if (st != noErr) return -1;
    int fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef f = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    const void *keys[] = {kCVPixelBufferPixelFormatTypeKey}, *vals[] = {f};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(f);
    VTDecompressionOutputCallbackRecord rec = {vp9_on_frame, c};
    st = VTDecompressionSessionCreate(NULL, c->vfd, NULL, attrs, &rec, &c->vs);
    CFRelease(attrs);
    if (st != noErr) {
        c->vs = NULL;
        vp9_drop(c);
        return -1;
    }
    c->vp9_w = cfg->width, c->vp9_h = cfg->height;
    c->vp9_full = cfg->vp9.color_range != 0;
    c->vp9_709 = cfg->vp9.color_space == 2; /* VP9 color_space 2 = BT.709 */
    return 0;
}
/* One VP9 frame (a superframe is split first) as a sample; the shown frame of the request is the last picture the callback delivers. */
static int vp9_decode(VtCtx *c, const DecodeRequest *r, DecodeResult *out) {
    size_t offs[8], sizes[8];
    int n = vp9_superframe_split(r->bitstream, r->bitstream_size, offs, sizes);
    if (n < 0) return -1;
    if (!c->vs) return 1; /* no key frame seen yet */
    c->got = 0;
    for (int i = 0; i < n; i++) {
        if (!sizes[i]) continue;
        CMBlockBufferRef bb = NULL;
        if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, sizes[i], NULL, NULL, 0, sizes[i], 0, &bb) != noErr) return -1;
        if (CMBlockBufferReplaceDataBytes(r->bitstream + offs[i], bb, 0, sizes[i]) != noErr) {
            CFRelease(bb);
            return -1;
        }
        CMSampleTimingInfo ti = {kCMTimeInvalid, CMTimeMake(c->pts++, 60), kCMTimeInvalid};
        CMSampleBufferRef sb = NULL;
        OSStatus st = CMSampleBufferCreate(NULL, bb, true, NULL, NULL, c->vfd, 1, 1, &ti, 1, &sizes[i], &sb);
        CFRelease(bb);
        if (st != noErr) return -1;
        st = VTDecompressionSessionDecodeFrame(c->vs, sb, 0, NULL, NULL);
        CFRelease(sb);
        if (st != noErr) return -1;
    }
    VTDecompressionSessionWaitForAsynchronousFrames(c->vs);
    if (!c->got || !c->nv12) return 1;
    if (decoder_write_nv12(r, c->nv12, c->w, c->h, out)) return -1;
    out->bt709 = !c->bt601;
    out->full_range = c->full;
    return 0;
}

static void *vt_open(int codec) {
    if (codec != DEC_CODEC_H264 && !(codec == DEC_CODEC_VP9 && vp9_available())) return NULL;
    VtCtx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->codec = codec;
    if (codec == DEC_CODEC_VP9) return c;
    c->d = vth264_create();
    if (!c->d) {
        free(c);
        return NULL;
    }
    return c;
}
static void vt_close(void *ctx) {
    VtCtx *c = ctx;
    if (c->d) vth264_destroy(c->d);
    vp9_drop(c);
    free(c->nv12);
    free(c);
}
static void vt_reset(void *ctx) {
    VtCtx *c = ctx;
    if (c->codec == DEC_CODEC_VP9) vp9_drop(c);
    else vth264_reset(c->d);
}
/* A new sequence: start from a clean decoder so an old SPS cannot win over the new one (vt_h264 keeps the first SPS it saw). */
static int vt_configure(void *ctx, const DecoderConfig *cfg) {
    VtCtx *c = ctx;
    if (c->codec == DEC_CODEC_VP9) return vp9_configure(c, cfg);
    if (cfg->codec != DEC_CODEC_H264 || !cfg->h264_annexb_size) return -1;
    VtH264 *nd = vth264_create();
    if (!nd) return -1;
    if (vth264_config(nd, cfg->h264_annexb, cfg->h264_annexb_size) != 0) {
        vth264_destroy(nd);
        return -1;
    }
    vth264_destroy(c->d);
    c->d = nd;
    return 0;
}
static int vt_decode(void *ctx, const DecodeRequest *r, DecodeResult *out) {
    VtCtx *c = ctx;
    if (!r->bitstream || !r->bitstream_size) return -1;
    if (c->codec == DEC_CODEC_VP9) return vp9_decode(c, r, out);
    c->got = 0;
    int n = vth264_decode(c->d, r->bitstream, r->bitstream_size, c->pts++, on_frame, c);
    if (n < 0) return -1;
    vth264_flush(c->d, on_frame, c); /* the guest reorders itself: this request's frame must come out now */
    if (!c->got || !c->nv12) return 1;
    if (decoder_write_nv12(r, c->nv12, c->w, c->h, out)) return -1;
    out->bt709 = !c->bt601;
    out->full_range = c->full;
    return 0;
}

const DecoderBackend decoder_backend_videotoolbox = {"videotoolbox", vt_open, vt_close, vt_decode, vt_reset, vt_configure, DECODER_BACKEND_ABI};

__attribute__((constructor)) static void decoder_vt_install(void) {
    const char *e = getenv("NAPPLE_DECODER");
    if (e && !strcmp(e, "null")) return;
    decoder_backend_install(&decoder_backend_videotoolbox);
}

/* ---- MSENC: H.264 encode with VTCompressionSession. UNBUILT/UNTESTED (written on Linux). One frame per encode(): the frame is submitted and
 * VTCompressionSessionCompleteFrames waits for it, so the Annex B access unit (SPS+PPS in front of a key frame) is in the request's buffer on return. ---- */
typedef struct {
    VTCompressionSessionRef s;
    int w, h;
    uint32_t br, fps;
    int64_t pts;
    uint8_t *out; /* the current request's destination, valid only inside encode() */
    size_t cap, n;
    int idr, err;
} VtEnc;
static void enc_put(VtEnc *e, const uint8_t *p, size_t n) {
    if (e->err || e->n + n > e->cap) {
        e->err = 1;
        return;
    }
    memcpy(e->out + e->n, p, n);
    e->n += n;
}
static void enc_on_sample(void *refcon, void *src, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
    VtEnc *e = refcon;
    (void)src, (void)flags;
    if (status != noErr || !sb || !CMSampleBufferDataIsReady(sb)) {
        e->err = 1;
        return;
    }
    CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    int key = 1;
    if (att && CFArrayGetCount(att) > 0) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(att, 0);
        key = !CFDictionaryContainsKey(d, kCMSampleAttachmentKey_NotSync);
    }
    CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
    static const uint8_t sc[4] = {0, 0, 0, 1};
    int nal_len = 4;
    if (fd) {
        size_t cnt = 0;
        const uint8_t *ps;
        size_t psn;
        int nl = 0;
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, &ps, &psn, &cnt, &nl) == noErr) {
            if (nl >= 1 && nl <= 4) nal_len = nl;
            if (key)
                for (size_t i = 0; i < cnt; i++)
                    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, i, &ps, &psn, NULL, NULL) == noErr) enc_put(e, sc, 4), enc_put(e, ps, psn);
        }
    }
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    char *data = NULL;
    size_t total = 0;
    if (!bb || CMBlockBufferGetDataPointer(bb, 0, NULL, &total, &data) != kCMBlockBufferNoErr) {
        e->err = 1;
        return;
    }
    for (size_t off = 0; off + (size_t)nal_len <= total;) {
        size_t len = 0;
        for (int i = 0; i < nal_len; i++) len = (len << 8) | (uint8_t)data[off + i];
        off += (size_t)nal_len;
        if (len > total - off) {
            e->err = 1;
            return;
        }
        enc_put(e, sc, 4);
        enc_put(e, (const uint8_t *)data + off, len);
        off += len;
    }
    e->idr = key;
}
static void enc_drop(VtEnc *e) {
    if (e->s) {
        VTCompressionSessionInvalidate(e->s);
        CFRelease(e->s);
        e->s = NULL;
    }
}
static int enc_session(VtEnc *e, const EncodeRequest *r) {
    if (e->s && e->w == r->width && e->h == r->height && e->br == r->bitrate && e->fps == r->fps) return 0;
    enc_drop(e);
    if (VTCompressionSessionCreate(NULL, r->width, r->height, kCMVideoCodecType_H264, NULL, NULL, NULL, enc_on_sample, e, &e->s) != noErr) {
        e->s = NULL;
        return -1;
    }
    uint32_t br = r->bitrate ? r->bitrate : 4000000, fps = r->fps ? r->fps : 30;
    int kf = (int)fps * 2;
    CFNumberRef nbr = CFNumberCreate(NULL, kCFNumberSInt32Type, &br), nfps = CFNumberCreate(NULL, kCFNumberSInt32Type, &fps), nkf = CFNumberCreate(NULL, kCFNumberIntType, &kf);
    VTSessionSetProperty(e->s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(e->s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse); /* output order == input order, one sample per request */
    VTSessionSetProperty(e->s, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Main_AutoLevel);
    VTSessionSetProperty(e->s, kVTCompressionPropertyKey_AverageBitRate, nbr);
    VTSessionSetProperty(e->s, kVTCompressionPropertyKey_ExpectedFrameRate, nfps);
    VTSessionSetProperty(e->s, kVTCompressionPropertyKey_MaxKeyFrameInterval, nkf);
    CFRelease(nbr), CFRelease(nfps), CFRelease(nkf);
    if (VTCompressionSessionPrepareToEncodeFrames(e->s) != noErr) {
        enc_drop(e);
        return -1;
    }
    e->w = r->width, e->h = r->height, e->br = r->bitrate, e->fps = r->fps;
    return 0;
}
static void *vtenc_open(int codec) { return codec == DEC_CODEC_H264 ? calloc(1, sizeof(VtEnc)) : NULL; }
static void vtenc_close(void *ctx) {
    enc_drop(ctx);
    free(ctx);
}
static int vtenc_encode(void *ctx, const EncodeRequest *r, EncodeResult *out) {
    VtEnc *e = ctx;
    if (!r->out || !r->luma || !r->chroma || r->width <= 0 || r->height <= 0 || ((r->width | r->height) & 1)) return -1;
    if (enc_session(e, r)) return -1;
    CVPixelBufferRef pb = NULL;
    if (CVPixelBufferCreate(NULL, (size_t)r->width, (size_t)r->height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, NULL, &pb) != kCVReturnSuccess) return -1;
    size_t lp = r->luma_pitch ? r->luma_pitch : (size_t)r->width, cp = r->chroma_pitch ? r->chroma_pitch : (size_t)r->width;
    if (lp < (size_t)r->width || cp < (size_t)r->width || r->luma_cap < lp * (r->height - 1) + r->width || r->chroma_cap < cp * (r->height / 2 - 1) + r->width ||
        CVPixelBufferLockBaseAddress(pb, 0) != kCVReturnSuccess) {
        CVPixelBufferRelease(pb);
        return -1;
    }
    uint8_t *dy = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *duv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    size_t dyp = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), duvp = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    for (int y = 0; y < r->height; y++) memcpy(dy + y * dyp, r->luma + y * lp, (size_t)r->width);
    for (int y = 0; y < r->height / 2; y++) memcpy(duv + y * duvp, r->chroma + y * cp, (size_t)r->width);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    e->out = r->out, e->cap = r->out_cap, e->n = 0, e->idr = 0, e->err = 0;
    CFDictionaryRef fprops = NULL;
    if (r->force_idr) {
        const void *k[] = {kVTEncodeFrameOptionKey_ForceKeyFrame}, *v[] = {kCFBooleanTrue};
        fprops = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    OSStatus st = VTCompressionSessionEncodeFrame(e->s, pb, CMTimeMake(e->pts++, (int32_t)(e->fps ? e->fps : 30)), kCMTimeInvalid, fprops, NULL, NULL);
    if (fprops) CFRelease(fprops);
    CVPixelBufferRelease(pb);
    if (st == noErr) st = VTCompressionSessionCompleteFrames(e->s, kCMTimeInvalid);
    e->out = NULL;
    if (st != noErr || e->err || !e->n) return -1;
    out->size = e->n;
    out->idr = e->idr;
    return 0;
}
const EncoderBackend encoder_backend_videotoolbox = {"videotoolbox", vtenc_open, vtenc_close, vtenc_encode, ENCODER_BACKEND_ABI};

__attribute__((constructor)) static void encoder_vt_install(void) {
    const char *e = getenv("NAPPLE_ENCODER");
    if (e && !strcmp(e, "null")) return;
    encoder_backend_install(&encoder_backend_videotoolbox);
}
