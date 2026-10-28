/* VideoToolbox DecoderBackend for the nvdrv NVDEC engine (H.264 only). VP9 policy: open() refuses it (and VP8/H.265) cleanly, the engine then reports
 * "codec not supported" in the guest's status structure and the channel's summary line; VP9 movies keep going through movie.c's libvpx path at the media
 * library level, and this backend does not grow a VideoToolbox VP9 decoder (VTDecompressionSession VP9 needs a supplemental-decoder opt-in and a
 * vpcC record per stream; not worth it while no title sends VP9 to nvhost-nvdec).
 * Wraps vt_h264.c: decode() is synchronous, so each request is decoded and flushed, and the single frame produced is copied to the guest surface.
 * Installed at startup by a constructor; NAPPLE_DECODER=null keeps the null backend. Host (Linux) builds do not compile this file. */
#include "decoder_backend.h"
#include "vt_h264.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
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

static void *vt_open(int codec) {
    if (codec != DEC_CODEC_H264) return NULL;
    VtCtx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->d = vth264_create();
    if (!c->d) {
        free(c);
        return NULL;
    }
    return c;
}
static void vt_close(void *ctx) {
    VtCtx *c = ctx;
    vth264_destroy(c->d);
    free(c->nv12);
    free(c);
}
static void vt_reset(void *ctx) { vth264_reset(((VtCtx *)ctx)->d); }
/* A new sequence: start from a clean decoder so an old SPS cannot win over the new one (vt_h264 keeps the first SPS it saw). */
static int vt_configure(void *ctx, const DecoderConfig *cfg) {
    VtCtx *c = ctx;
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
