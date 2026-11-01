/* movie.c's host movie::Decoder driven the way the SDK movie library's callers drive it (construct, Configure with a MediaData format, RegisterClientEvents,
 * Start, AcquireInputBufferIndex / GetInputBuffer / SendInputBufferForDecode, AcquireOutputBufferIndex / GetOutputBuffer / ReleaseOutputBufferIndex), with
 * fake guest objects (movie::Buffer, movie::MediaData, nn::os events) and streams made here by encoders: AAC by AudioToolbox (raw access units with an
 * AudioSpecificConfig csd-0, the same units ADTS-framed, 5.1 with one tone per channel for the channel order), VP8 and VP9 by libvpx. Checks: frame counts,
 * tones per channel, picture PSNR in output formats 4 (NV12) and 2 (ARGB, against the library's ConvertToArgb formula), the format / output events, and
 * the refusals (video in PCM format, a type left to the library). macOS only (AudioToolbox, libvpx encoder). */
#define module_find_symbol test_find_symbol
#define movie_register_overrides test_movie_register_overrides
#define movie_host_tick test_movie_host_tick
#include "../src/movie.c"
#include "svc_test.h"
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

/* ---- fake guest objects ---- */
typedef struct {
    uint8_t *data;
    size_t cap;
    int off, size;
} FBuf;
enum { K_INT, K_STR, K_BUF };
typedef struct {
    const char *key;
    int kind, i;
    const char *s;
    FBuf b;
} FEntry;
typedef struct {
    FEntry e[24];
    int n;
} FMd;
static FEntry *md_get(const FMd *m, const char *k, int kind) {
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->e[i].key, k) && m->e[i].kind == kind) return (FEntry *)&m->e[i];
    return NULL;
}
static FEntry *md_put(FMd *m, const char *k, int kind) {
    FEntry *e = md_get(m, k, kind);
    if (!e) e = &m->e[m->n++], e->key = k, e->kind = kind;
    return e;
}
static void md_int(FMd *m, const char *k, int v) { md_put(m, k, K_INT)->i = v; }
static void md_str(FMd *m, const char *k, const char *v) { md_put(m, k, K_STR)->s = v; }
static void md_buf(FMd *m, const char *k, const uint8_t *p, size_t n) { md_put(m, k, K_BUF)->b = (FBuf){(uint8_t *)p, n, 0, (int)n}; }
static bool f_find_int32(const void *md, const char *k, int *v) {
    FEntry *e = md_get(md, k, K_INT);
    if (e) *v = e->i;
    return e != NULL;
}
static bool f_find_string(const void *md, const char *k, const char **v) {
    FEntry *e = md_get(md, k, K_STR);
    if (e) *v = e->s;
    return e != NULL;
}
static void f_set_int32(void *md, const char *k, int v) { md_int(md, k, v); }
static void f_set_string(void *md, const char *k, const char *v) { md_str(md, k, v); }
static void f_set_rect(void *md, const char *k, int l, int t, int r, int b) { (void)md, (void)k, (void)l, (void)t, (void)r, (void)b; }
static int f_find_buffer(const void *md, const char *k, void **out) {
    FEntry *e = md_get(md, k, K_BUF);
    if (e) *out = &e->b;
    return e != NULL;
}
static void *f_buf_data(const void *b) { return ((const FBuf *)b)->data; }
static size_t f_buf_size(const void *b) { return (size_t)((const FBuf *)b)->size; }
static void f_set_range(void *b, int off, int size) { ((FBuf *)b)->off = off, ((FBuf *)b)->size = size; }
static void f_set_data(void *b, void *p, size_t cap) { ((FBuf *)b)->data = p, ((FBuf *)b)->cap = cap; }
static void *f_base(const void *b) { return ((const FBuf *)b)->data; }
static size_t f_capacity(const void *b) { return ((const FBuf *)b)->cap; }
static int signalled[4];
static int ev_store[4];
static void f_signal(void *ev) { signalled[(int *)ev - ev_store]++; }
static int lib_ctor_calls, lib_ctor_type;
static void f_orig_ctor(void *obj, int type, int format, int mode, int prio) { (void)obj, (void)format, (void)mode, (void)prio, lib_ctor_calls++, lib_ctor_type = type; }
void *test_find_symbol(const char *name) {
    if (!strcmp(name, "_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE")) return (void *)f_find_buffer;
    if (!strcmp(name, "_ZNK5movie6Buffer4DataEv")) return (void *)f_buf_data;
    if (!strcmp(name, "_ZNK5movie6Buffer4SizeEv")) return (void *)f_buf_size;
    return NULL;
}
static void guest_fake(void) {
    G.signal_event = f_signal, G.buf_set_range = f_set_range, G.buf_set_data = f_set_data, G.buf_base = f_base, G.buf_capacity = f_capacity;
    G.md_find_int32 = f_find_int32, G.md_find_string = f_find_string, G.md_set_int32 = f_set_int32, G.md_set_rect = f_set_rect, G.md_set_string = f_set_string;
    static char vtable[0x100];
    G.vptr = vtable + 0x10;
    G.orig[MOVIE_CTOR] = (void *)f_orig_ctor;
    g_ready = 1;
}

/* ---- driving one decoder ---- */
typedef struct {
    uint8_t *pcm_or_pic; /* everything the decoder handed out, concatenated */
    size_t bytes, cap;
    int outputs, eos;
} Sink;
static void sink_add(Sink *k, const uint8_t *p, size_t n) {
    if (k->bytes + n > k->cap) k->pcm_or_pic = realloc(k->pcm_or_pic, k->cap = (k->bytes + n) * 2);
    memcpy(k->pcm_or_pic + k->bytes, p, n);
    k->bytes += n;
}
static void drain(void *obj, Sink *k) {
    int idx, rem;
    int64_t t;
    unsigned fl;
    static uint8_t out[1 << 22];
    while (h_acquire_output(obj, &idx, &t, &fl, &rem) == ST_OK) {
        FBuf b = {out, sizeof out, 0, 0};
        CHECK(h_get_output(obj, idx, &b) == ST_OK, "GetOutputBuffer %d", idx);
        if (fl & FLAG_EOS) k->eos++;
        else {
            sink_add(k, out + b.off, (size_t)b.size);
            k->outputs++;
        }
        CHECK(h_release_output(obj, idx) == ST_OK, "ReleaseOutputBufferIndex %d", idx);
    }
}
static int feed(void *obj, const uint8_t *p, size_t n, int64_t t, unsigned flags, Sink *k) {
    int idx, rem;
    drain(obj, k);
    if (h_acquire_input(obj, &idx, &rem) != ST_OK) return -1;
    FBuf b = {0};
    if (h_get_input(obj, idx, &b) != ST_OK || b.cap < n) return -1;
    if (n) memcpy(b.data, p, n);
    int st = h_send_input(obj, idx, 0, (int)n, t, flags);
    drain(obj, k);
    return st;
}
static void *new_decoder(void *obj, int type, int format, int mode, void **events) {
    h_ctor(obj, type, format, mode, 0);
    if (find(obj)) h_register_events(obj, events);
    return obj;
}

/* ---- AAC streams (AudioToolbox encoder) ---- */
typedef struct {
    uint8_t *data[1024];
    size_t size[1024];
    int n;
    uint8_t asc[16];
    size_t asc_size;
} Units;
typedef struct {
    const int16_t *pcm;
    size_t frames, pos;
    int ch;
} EncIn;
static OSStatus enc_input(AudioConverterRef c, UInt32 *n, AudioBufferList *io, AudioStreamPacketDescription **d, void *u) {
    (void)c, (void)d;
    EncIn *e = u;
    UInt32 left = (UInt32)(e->frames - e->pos), k = *n < left ? *n : left;
    io->mBuffers[0].mData = (void *)(e->pcm + e->pos * e->ch);
    io->mBuffers[0].mDataByteSize = k * 2 * (UInt32)e->ch;
    io->mBuffers[0].mNumberChannels = (UInt32)e->ch;
    e->pos += k;
    *n = k;
    return noErr;
}
/* The ASC inside an esds cookie: descriptor tag 5's payload. */
static size_t asc_of_esds(const uint8_t *p, size_t n, uint8_t *out) {
    for (size_t i = 0; i + 2 < n; i++)
        if (p[i] == 5) {
            size_t k = i + 1, len = 0;
            for (int j = 0; j < 4 && k < n; j++) {
                len = (len << 7) | (p[k] & 0x7f);
                if (!(p[k++] & 0x80)) break;
            }
            if (len >= 2 && len <= 16 && k + len <= n && (p[k] >> 3) >= 1 && (p[k] >> 3) <= 5) {
                memcpy(out, p + k, len);
                return len;
            }
        }
    return 0;
}
static int encode_aac(const int16_t *pcm, size_t frames, int rate, int ch, AudioChannelLayoutTag layout, Units *u) {
    AudioStreamBasicDescription in = {rate, kAudioFormatLinearPCM, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, 2 * (UInt32)ch, 1, 2 * (UInt32)ch,
                                      (UInt32)ch, 16, 0};
    AudioStreamBasicDescription out = {.mSampleRate = rate, .mFormatID = kAudioFormatMPEG4AAC, .mChannelsPerFrame = (UInt32)ch, .mFramesPerPacket = 1024};
    AudioConverterRef c;
    if (AudioConverterNew(&in, &out, &c)) return -1;
    if (layout) {
        AudioChannelLayout l = {.mChannelLayoutTag = layout};
        AudioConverterSetProperty(c, kAudioConverterInputChannelLayout, sizeof l, &l);
        AudioConverterSetProperty(c, kAudioConverterOutputChannelLayout, sizeof l, &l);
    }
    UInt32 br = 64000 * (UInt32)ch;
    AudioConverterSetProperty(c, kAudioConverterEncodeBitRate, sizeof br, &br);
    UInt32 maxp = 0, sz = sizeof maxp;
    AudioConverterGetProperty(c, kAudioConverterPropertyMaximumOutputPacketSize, &sz, &maxp);
    EncIn e = {pcm, frames, 0, ch};
    uint8_t *buf = malloc(maxp ? maxp : 8192);
    u->n = 0;
    for (;;) {
        UInt32 np = 1;
        AudioStreamPacketDescription d;
        AudioBufferList abl = {1, {{(UInt32)ch, maxp ? maxp : 8192, buf}}};
        if (AudioConverterFillComplexBuffer(c, enc_input, &e, &np, &abl, &d) || !np) break;
        u->data[u->n] = malloc(d.mDataByteSize);
        memcpy(u->data[u->n], buf + d.mStartOffset, d.mDataByteSize);
        u->size[u->n++] = d.mDataByteSize;
        if (u->n == 1024) break;
    }
    UInt32 cs = 0;
    uint8_t cookie[256];
    if (!AudioConverterGetPropertyInfo(c, kAudioConverterCompressionMagicCookie, &cs, NULL) && cs <= sizeof cookie &&
        !AudioConverterGetProperty(c, kAudioConverterCompressionMagicCookie, &cs, cookie))
        u->asc_size = asc_of_esds(cookie, cs, u->asc);
    free(buf);
    AudioConverterDispose(c);
    return u->n;
}
static void units_free(Units *u) {
    for (int i = 0; i < u->n; i++) free(u->data[i]);
    u->n = 0;
}
/* Sine tones, one per channel. */
static int16_t *tones(size_t frames, int rate, int ch, const double *hz, double amp) {
    int16_t *p = malloc(frames * ch * 2);
    for (size_t f = 0; f < frames; f++)
        for (int c = 0; c < ch; c++) p[f * ch + c] = (int16_t)(amp * 32767 * sin(2 * M_PI * hz[c] * (double)f / rate));
    return p;
}
/* Frequency of channel c over frames [a, b) by counting upward zero crossings. */
static double freq_of(const int16_t *p, int ch, int c, size_t a, size_t b, int rate) {
    int n = 0;
    for (size_t f = a + 1; f < b; f++) n += p[(f - 1) * ch + c] < 0 && p[f * ch + c] >= 0;
    return (double)n * rate / (double)(b - a);
}
static double rms_of(const int16_t *p, int ch, int c, size_t a, size_t b) {
    double s = 0;
    for (size_t f = a; f < b; f++) s += (double)p[f * ch + c] * p[f * ch + c];
    return sqrt(s / (double)(b - a));
}
static void adts_wrap(const Units *u, int rate_index, int chcfg, Units *o) {
    o->n = u->n;
    for (int i = 0; i < u->n; i++) {
        size_t len = u->size[i] + 7;
        uint8_t *p = malloc(len);
        p[0] = 0xff, p[1] = 0xf1, p[2] = (uint8_t)((1 << 6) | (rate_index << 2) | (chcfg >> 2)), p[3] = (uint8_t)(((chcfg & 3) << 6) | (len >> 11));
        p[4] = (uint8_t)(len >> 3), p[5] = (uint8_t)(((len & 7) << 5) | 0x1f), p[6] = 0xfc;
        memcpy(p + 7, u->data[i], u->size[i]);
        o->data[i] = p, o->size[i] = len;
    }
}
/* Runs a whole AAC stream through a host decoder; returns the decoded PCM (frames in *frames). */
static int16_t *run_aac(const char *what, const Units *u, FMd *fmt, int ch, size_t *frames) {
    static char obj[2][64];
    static int which;
    void *o = obj[which++ & 1];
    void *events[4] = {&ev_store[0], &ev_store[1], &ev_store[2], &ev_store[3]};
    memset(signalled, 0, sizeof signalled);
    new_decoder(o, TYPE_AAC, FORMAT_PCM, MODE_BUFFER, events);
    CHECK(find(o) != NULL, "%s: type 4 / PCM / buffer runs on the host", what);
    CHECK(h_configure(o, fmt) == ST_OK, "%s: Configure", what);
    CHECK(h_start(o) == ST_OK, "%s: Start", what);
    Sink k = {0};
    int bad = 0;
    for (int i = 0; i < u->n; i++) bad += feed(o, u->data[i], u->size[i], (int64_t)i * 21333, 0, &k) != ST_OK;
    feed(o, NULL, 0, (int64_t)u->n * 21333, FLAG_EOS, &k);
    FMd outfmt = {0};
    int rate = 0, och = 0;
    CHECK(h_get_output_format(o, &outfmt) == ST_OK && f_find_int32(&outfmt, "sample-rate", &rate) && f_find_int32(&outfmt, "channel-count", &och) && och == ch,
          "%s: output format %d Hz, %d channels", what, rate, och);
    CHECK(!bad && k.eos == 1 && signalled[EV_FORMAT] == 1 && signalled[EV_OUTPUT] >= k.outputs && !signalled[EV_ERROR],
          "%s: %d units in, %d outputs, end of stream, format event once, no error event (in %d out %d fmt %d err %d)", what, u->n, k.outputs, signalled[EV_INPUT],
          signalled[EV_OUTPUT], signalled[EV_FORMAT], signalled[EV_ERROR]);
    h_d2(o);
    *frames = k.bytes / 2 / (size_t)ch;
    return (int16_t *)k.pcm_or_pic;
}

/* ---- VP8 / VP9 streams (libvpx encoder) ---- */
enum { VW = 160, VH = 96, VFRAMES = 30 };
static void picture(vpx_image_t *img, int f) { /* moving gradient with a bright square: chroma varies too */
    for (int y = 0; y < VH; y++)
        for (int x = 0; x < VW; x++) {
            int sq = (x - f * 3 + 200) % 160 < 40 && y > 30 && y < 70;
            img->planes[0][y * img->stride[0] + x] = (uint8_t)(sq ? 230 : 40 + (x + y + f * 2) % 150);
        }
    for (int y = 0; y < VH / 2; y++)
        for (int x = 0; x < VW / 2; x++) {
            img->planes[1][y * img->stride[1] + x] = (uint8_t)(90 + (x * 2 + f) % 80);
            img->planes[2][y * img->stride[2] + x] = (uint8_t)(170 - (y * 2 + f) % 70);
        }
}
static int encode_vpx(int vp8, Units *u, vpx_image_t *src) {
    vpx_codec_ctx_t c;
    vpx_codec_enc_cfg_t cfg;
    vpx_codec_iface_t *ifc = vp8 ? vpx_codec_vp8_cx() : vpx_codec_vp9_cx();
    if (vpx_codec_enc_config_default(ifc, &cfg, 0)) return -1;
    cfg.g_w = VW, cfg.g_h = VH, cfg.g_timebase = (vpx_rational_t){1, 30}, cfg.rc_target_bitrate = 1500, cfg.g_lag_in_frames = 0, cfg.kf_max_dist = 10;
    if (vpx_codec_enc_init(&c, ifc, &cfg, 0)) return -1;
    vpx_codec_control(&c, VP8E_SET_CPUUSED, 8);
    u->n = 0;
    for (int f = 0; f <= VFRAMES; f++) {
        vpx_image_t *img = f < VFRAMES ? &src[f] : NULL;
        if (img) picture(img, f);
        if (vpx_codec_encode(&c, img, f, 1, 0, VPX_DL_REALTIME)) break;
        const vpx_codec_cx_pkt_t *p;
        vpx_codec_iter_t it = NULL;
        while ((p = vpx_codec_get_cx_data(&c, &it)))
            if (p->kind == VPX_CODEC_CX_FRAME_PKT) {
                u->data[u->n] = malloc(p->data.frame.sz);
                memcpy(u->data[u->n], p->data.frame.buf, p->data.frame.sz);
                u->size[u->n++] = p->data.frame.sz;
            }
    }
    vpx_codec_destroy(&c);
    return u->n;
}
static double psnr(const uint8_t *a, const uint8_t *b, size_t n, size_t step, size_t skip) {
    double s = 0;
    size_t k = 0;
    for (size_t i = 0; i < n; i += step)
        for (size_t j = skip; j < step; j++, k++) s += ((double)a[i + j] - b[i + j]) * ((double)a[i + j] - b[i + j]);
    s /= (double)k;
    return s == 0 ? 99 : 10 * log10(255.0 * 255.0 / s);
}
/* The library's ARGB from a source I420 picture (what a console would hand the game). */
static void ref_argb(const vpx_image_t *img, uint8_t *o) {
    for (int y = 0; y < VH; y++)
        for (int x = 0; x < VW; x++, o += 4) {
            float Y = img->planes[0][y * img->stride[0] + x], U = img->planes[1][(y / 2) * img->stride[1] + x / 2] - 128.f,
                  V = img->planes[2][(y / 2) * img->stride[2] + x / 2] - 128.f;
            int r = (int)(Y + 1.28033f * V), g = (int)(Y - 0.21482f * U - 0.38059f * V), b = (int)(Y + 2.12798f * U);
            o[0] = 255, o[1] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r), o[2] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g), o[3] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
}
static void ref_nv12(const vpx_image_t *img, uint8_t *o) {
    for (int y = 0; y < VH; y++) memcpy(o + y * VW, img->planes[0] + y * img->stride[0], VW);
    for (int y = 0; y < VH / 2; y++)
        for (int x = 0; x < VW / 2; x++)
            o[VW * VH + y * VW + 2 * x] = img->planes[1][y * img->stride[1] + x], o[VW * VH + y * VW + 2 * x + 1] = img->planes[2][y * img->stride[2] + x];
}
static void run_video(int vp8, int format) {
    const char *name = vp8 ? "VP8" : "VP9";
    static vpx_image_t src[VFRAMES];
    for (int f = 0; f < VFRAMES; f++) vpx_img_alloc(&src[f], VPX_IMG_FMT_I420, VW, VH, 16);
    Units u = {0};
    CHECK(encode_vpx(vp8, &u, src) == VFRAMES, "%s: libvpx encoded %d frames of %dx%d", name, u.n, VW, VH);
    static char obj[64];
    void *events[4] = {&ev_store[0], &ev_store[1], &ev_store[2], &ev_store[3]};
    memset(signalled, 0, sizeof signalled);
    new_decoder(obj, vp8 ? TYPE_VP8 : TYPE_VP9, format, MODE_BUFFER, events);
    FMd fmt = {0};
    md_str(&fmt, "mime", vp8 ? "video/x-vnd.on2.vp8" : "video/x-vnd.on2.vp9");
    md_int(&fmt, "width", VW);
    md_int(&fmt, "height", VH);
    CHECK(find(obj) && h_configure(obj, &fmt) == ST_OK && h_start(obj) == ST_OK, "%s format %d: host decoder configured and started", name, format);
    int size = 0, count = 0;
    h_get_output_size(obj, &size, &count);
    int fb = format == FORMAT_ARGB ? VW * VH * 4 : VW * VH * 3 / 2;
    CHECK(size == fb, "%s format %d: output size %d (expected %d)", name, format, size, fb);
    Sink k = {0};
    for (int i = 0; i < u.n; i++) feed(obj, u.data[i], u.size[i], (int64_t)i * 33333, 0, &k);
    feed(obj, NULL, 0, (int64_t)u.n * 33333, FLAG_EOS, &k);
    CHECK(k.outputs == VFRAMES && k.eos == 1 && k.bytes == (size_t)fb * VFRAMES && !signalled[EV_ERROR], "%s format %d: %d pictures out, end of stream", name, format,
          k.outputs);
    FMd of = {0};
    int cf = 0, stride = 0;
    h_get_output_format(obj, &of);
    f_find_int32(&of, "color-format", &cf);
    f_find_int32(&of, "stride", &stride);
    CHECK(format == FORMAT_ARGB ? cf == 16 && stride == VW * 4 : cf == 21 && stride == VW, "%s format %d: output format color-format %d stride %d", name, format, cf,
          stride);
    double worst = 99;
    int alpha = 1;
    uint8_t *ref = malloc((size_t)fb);
    for (int f = 0; f < VFRAMES && k.outputs == VFRAMES; f++) {
        const uint8_t *got = k.pcm_or_pic + (size_t)f * fb;
        if (format == FORMAT_ARGB) {
            ref_argb(&src[f], ref);
            for (int i = 0; i < VW * VH; i++) alpha &= got[i * 4] == 255;
        } else ref_nv12(&src[f], ref);
        double d = format == FORMAT_ARGB ? psnr(got, ref, (size_t)fb, 4, 1) : psnr(got, ref, (size_t)fb, 1, 0);
        if (d < worst) worst = d;
    }
    CHECK(worst > 30 && alpha, "%s format %d: every picture matches the source (worst PSNR %.1f dB%s)", name, format, worst,
          format == FORMAT_ARGB ? ", A = 255 first in each pixel" : "");
    printf("%s format %d: worst picture PSNR %.1f dB over %d frames\n", name, format, worst, VFRAMES);
    h_d2(obj);
    free(ref);
    free(k.pcm_or_pic);
    units_free(&u);
    for (int f = 0; f < VFRAMES; f++) vpx_img_free(&src[f]);
}

int main(void) {
    guest_fake();
    enum { RATE = 48000, SECS = 2 };
    size_t nf = RATE * SECS;

    /* 1. AAC-LC stereo, raw access units + AudioSpecificConfig in csd-0 */
    double hz2[2] = {440, 1000};
    int16_t *src = tones(nf, RATE, 2, hz2, 0.5);
    Units u = {0};
    CHECK(encode_aac(src, nf, RATE, 2, 0, &u) > 80 && u.asc_size == 2, "AudioToolbox encoded %d AAC units, ASC %zu bytes", u.n, u.asc_size);
    AacConfig ac;
    CHECK(!aac_parse_asc(u.asc, u.asc_size, &ac) && ac.object_type == 2 && ac.rate == RATE && ac.channels == 2, "ASC parses: object %d, %d Hz, %d channels",
          ac.object_type, ac.rate, ac.channels);
    FMd f1 = {0};
    md_str(&f1, "mime", "audio/mp4a-latm"), md_int(&f1, "sample-rate", RATE), md_int(&f1, "channel-count", 2), md_buf(&f1, "csd-0", u.asc, u.asc_size);
    size_t got = 0;
    int16_t *pcm = run_aac("AAC stereo csd-0", &u, &f1, 2, &got);
    CHECK(got + 4096 >= nf && got <= nf + 4096, "AAC stereo: %zu frames out for %zu in", got, nf);
    size_t a = RATE / 2, b = RATE * 3 / 2;
    double fl = freq_of(pcm, 2, 0, a, b, RATE), fr = freq_of(pcm, 2, 1, a, b, RATE), rl = rms_of(pcm, 2, 0, a, b), rr = rms_of(pcm, 2, 1, a, b);
    CHECK(fabs(fl - 440) < 5 && fabs(fr - 1000) < 5, "AAC stereo: left %.1f Hz, right %.1f Hz (440 / 1000 in)", fl, fr);
    CHECK(fabs(rl - 11585) < 1200 && fabs(rr - 11585) < 1200, "AAC stereo: RMS %.0f / %.0f (11585 in)", rl, rr);
    printf("AAC stereo (csd-0): %zu frames for %zu in, left %.1f Hz right %.1f Hz, RMS %.0f / %.0f (440 / 1000 Hz, RMS 11585 in)\n", got, nf, fl, fr, rl, rr);
    free(pcm);

    /* 2. the same units ADTS-framed, no csd-0 ("is-adts" 1): the first header configures */
    Units ua = {0};
    adts_wrap(&u, 3 /* 48000 */, 2, &ua);
    size_t hl, flen;
    CHECK((hl = aac_parse_adts(ua.data[0], ua.size[0], &ac, &flen)) == 7 && flen == ua.size[0] && ac.rate == RATE && ac.channels == 2, "ADTS header parses");
    FMd f2 = {0};
    md_str(&f2, "mime", "audio/mp4a-latm"), md_int(&f2, "sample-rate", RATE), md_int(&f2, "channel-count", 2), md_int(&f2, "is-adts", 1);
    pcm = run_aac("AAC ADTS", &ua, &f2, 2, &got);
    CHECK(got + 4096 >= nf && fabs(freq_of(pcm, 2, 1, a, b, RATE) - 1000) < 5, "AAC ADTS: %zu frames, right channel %.1f Hz", got, freq_of(pcm, 2, 1, a, b, RATE));
    free(pcm);
    units_free(&ua);

    /* 3. raw units with neither csd-0 nor ADTS: AAC-LC at the announced rate / channels */
    FMd f3 = {0};
    md_str(&f3, "mime", "audio/mp4a-latm"), md_int(&f3, "sample-rate", RATE), md_int(&f3, "channel-count", 2);
    pcm = run_aac("AAC raw, no csd-0", &u, &f3, 2, &got);
    CHECK(got + 4096 >= nf && fabs(freq_of(pcm, 2, 0, a, b, RATE) - 440) < 5, "AAC without csd-0: %zu frames, left %.1f Hz", got, freq_of(pcm, 2, 0, a, b, RATE));
    free(pcm);
    units_free(&u);
    free(src);

    /* 4. 5.1: one tone per channel, encoded in WAVE order (L R C LFE Ls Rs) -> the decoder must hand the same order back */
    double hz6[6] = {300, 500, 700, 60, 1100, 1300};
    src = tones(nf, RATE, 6, hz6, 0.3);
    CHECK(encode_aac(src, nf, RATE, 6, kAudioChannelLayoutTag_MPEG_5_1_A, &u) > 80 && u.asc_size == 2, "AudioToolbox encoded 5.1 AAC (%d units)", u.n);
    FMd f4 = {0};
    md_str(&f4, "mime", "audio/mp4a-latm"), md_int(&f4, "sample-rate", RATE), md_int(&f4, "channel-count", 6), md_buf(&f4, "csd-0", u.asc, u.asc_size);
    pcm = run_aac("AAC 5.1", &u, &f4, 6, &got);
    int order = got > b;
    char seen[128] = "";
    for (int c = 0; c < 6 && got > b; c++) {
        double f = freq_of(pcm, 6, c, a, b, RATE);
        snprintf(seen + strlen(seen), sizeof seen - strlen(seen), " %.0f", f);
        order &= fabs(f - hz6[c]) < (c == 3 ? 6 : 10);
    }
    CHECK(order, "AAC 5.1: channels in WAVE order L R C LFE Ls Rs (Hz:%s; expected 300 500 700 60 1100 1300)", seen);
    printf("AAC 5.1: per-channel Hz%s (L R C LFE Ls Rs = 300 500 700 60 1100 1300 in)\n", seen);
    free(pcm);
    units_free(&u);
    free(src);

    /* 5. a corrupt unit: error event, the decoder keeps going */
    {
        static char o[64];
        void *events[4] = {&ev_store[0], &ev_store[1], &ev_store[2], &ev_store[3]};
        memset(signalled, 0, sizeof signalled);
        new_decoder(o, TYPE_AAC, FORMAT_PCM, MODE_BUFFER, events);
        h_configure(o, &f3);
        h_start(o);
        Sink k = {0};
        uint8_t junk[300];
        for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 37 + 11);
        feed(o, junk, sizeof junk, 0, 0, &k);
        CHECK(h_get_last_error(o) == ST_ERROR || signalled[EV_ERROR] == 0, "AAC: a corrupt unit sets the last error or is concealed (error events %d)", signalled[EV_ERROR]);
        CHECK(signalled[EV_INPUT] >= 2, "AAC: the corrupt unit's input buffer comes back");
        h_d2(o);
        free(k.pcm_or_pic);
    }

    /* 6. video: VP8 and VP9 in output formats 4 (NV12) and 2 (ARGB) */
    run_video(1, FORMAT_NV12);
    run_video(1, FORMAT_ARGB);
    run_video(0, FORMAT_ARGB);

    /* 7. refusals */
    {
        static char o[64];
        void *events[4] = {&ev_store[0], &ev_store[1], &ev_store[2], &ev_store[3]};
        new_decoder(o, TYPE_VP8, FORMAT_PCM, MODE_BUFFER, events);
        FMd f = {0};
        md_str(&f, "mime", "video/x-vnd.on2.vp8"), md_int(&f, "width", VW), md_int(&f, "height", VH);
        CHECK(find(o) && h_configure(o, &f) == ST_UNSUPPORTED, "VP8 with output format 1 (PCM) is refused, as IsOutputFormatSupported does");
        h_d2(o);
        lib_ctor_calls = 0;
        static char o2[64];
        new_decoder(o2, TYPE_AAC, FORMAT_NV12, MODE_BUFFER, events);
        CHECK(!find(o2) && lib_ctor_calls == 1 && lib_ctor_type == TYPE_AAC, "AAC with a video output format is left to the library");
        static char o3[64];
        md_str(&f, "mime", "video/x-vnd.on2.vp9");
        new_decoder(o3, TYPE_VP8, FORMAT_NV12, MODE_BUFFER, events);
        CHECK(h_configure(o3, &f) == ST_UNSUPPORTED, "a VP8 decoder refuses a VP9 format");
        h_d2(o3);
    }
    printf("{\"test\":\"svc_movie_host\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
