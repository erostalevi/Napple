/* Null/test decoder backend: no decoding. The frame size comes from the codec headers (SPS, VP8/VP9 key frame), and the picture is a flat
 * grey whose level depends on the bitstream bytes, so tests can tell frames apart and a title that shows the output sees a plausible surface. */
#include "codec_headers_core.h"
#include "decoder_backend.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    int codec, width, height; /* last known size (inter frames reuse it) */
    unsigned frames, configures;
} NullCtx;

static void *null_open(int codec) {
    if (codec != DEC_CODEC_H264 && codec != DEC_CODEC_VP8 && codec != DEC_CODEC_H265 && codec != DEC_CODEC_VP9) return NULL;
    NullCtx *c = calloc(1, sizeof *c);
    if (c) c->codec = codec;
    return c;
}
static void null_close(void *ctx) { free(ctx); }
static void null_reset(void *ctx) {
    NullCtx *c = ctx;
    c->width = c->height = 0;
}
static int null_decode(void *ctx, const DecodeRequest *r, DecodeResult *out) {
    NullCtx *c = ctx;
    int w = 0, h = 0;
    if (!r->bitstream || !r->bitstream_size) return -1;
    switch (c->codec) {
        case DEC_CODEC_H264: {
            H264Sps s;
            if (!h264_find_sps(r->bitstream, r->bitstream_size, &s)) w = s.width, h = s.height;
            break;
        }
        case DEC_CODEC_H265: {
            H265Sps s;
            if (!h265_find_sps(r->bitstream, r->bitstream_size, &s)) w = s.width, h = s.height;
            break;
        }
        case DEC_CODEC_VP9: {
            Vp9Header v;
            if (!vp9_parse_header(r->bitstream, r->bitstream_size, &v)) w = v.width, h = v.height;
            break;
        }
        case DEC_CODEC_VP8: {
            Vp8Header v;
            if (!vp8_parse_header(r->bitstream, r->bitstream_size, &v)) w = v.width, h = v.height;
            break;
        }
    }
    if (w && h) c->width = w, c->height = h;
    if (!c->width) return 1; /* no size known yet (stream starts mid-GOP): no frame */
    w = c->width;
    h = c->height;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < r->bitstream_size; i++) hash = (hash ^ r->bitstream[i]) * 16777619u;
    size_t ysz = (size_t)w * h;
    uint8_t *f = malloc(ysz + (size_t)w * ((h + 1) / 2));
    if (!f) return -1;
    memset(f, (int)(32 + (hash & 0x7f)), ysz);
    memset(f + ysz, 128, (size_t)w * ((h + 1) / 2));
    int rc = decoder_write_nv12(r, f, w, h, out);
    free(f);
    if (rc) return -1;
    c->frames++;
    out->bt709 = h > 576;
    out->full_range = 0;
    return 0;
}
/* Takes the size from the parameter sets the engine found, so a stream whose slices come without an SPS (the sets arrived earlier) still sizes the picture. */
static int null_configure(void *ctx, const DecoderConfig *cfg) {
    NullCtx *c = ctx;
    if (cfg->codec != c->codec) return -1;
    if (cfg->width > 0 && cfg->height > 0) c->width = cfg->width, c->height = cfg->height;
    c->configures++;
    return 0;
}
const DecoderBackend decoder_backend_null = {"null", null_open, null_close, null_decode, null_reset, null_configure, DECODER_BACKEND_ABI};

static size_t row_pitch(uint32_t p, int w) { return p >= (uint32_t)w ? p : (size_t)w; }
int decoder_write_nv12(const DecodeRequest *r, const uint8_t *nv12, int w, int h, DecodeResult *out) {
    if (!r->luma || !r->chroma || w <= 0 || h <= 0) return -1;
    size_t lp = row_pitch(r->luma_pitch, w), cp = row_pitch(r->chroma_pitch, w), crows = ((size_t)h + 1) / 2;
    if (r->luma_cap < lp * (h - 1) + w || r->chroma_cap < cp * (crows - 1) + w) return -1;
    const uint8_t *uv = nv12 + (size_t)w * h;
    if (lp == (size_t)w) memcpy(r->luma, nv12, (size_t)w * h);
    else
        for (int y = 0; y < h; y++) memcpy(r->luma + y * lp, nv12 + (size_t)y * w, (size_t)w);
    if (cp == (size_t)w) memcpy(r->chroma, uv, (size_t)w * crows);
    else
        for (size_t y = 0; y < crows; y++) memcpy(r->chroma + y * cp, uv + y * w, (size_t)w);
    out->width = w;
    out->height = h;
    out->pitch = (int)lp;
    return 0;
}

static const DecoderBackend *installed;
const DecoderBackend *decoder_backend_current(void) { return installed ? installed : &decoder_backend_null; }
int decoder_backend_install(const DecoderBackend *b) {
    if (b && (b->abi != DECODER_BACKEND_ABI || !b->open || !b->close || !b->decode)) return -1;
    installed = b;
    return 0;
}
