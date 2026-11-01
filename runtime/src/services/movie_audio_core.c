#include "movie_audio_core.h"
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "../third_party/stb_vorbis.c"

#define MAX_PACKET 65000 /* one Ogg page carries at most 255 lacing values of 255 bytes */
struct VorbisPcm {
    stb_vorbis *f;
    uint8_t *hdr[3];
    size_t hdr_size[3];
    int nhdr;
    uint32_t seq;
};

static uint32_t crc_tab[256];
static void crc_init(void) {
    if (crc_tab[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int k = 0; k < 8; k++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : r << 1;
        crc_tab[i] = r;
    }
}
/* Wraps one packet in one Ogg page (serial 1). Returns the page length (header + body) in out, or 0 when the packet does not fit. */
static size_t ogg_page(uint8_t *out, size_t cap, const uint8_t *pkt, size_t n, uint8_t flags, uint64_t granule, uint32_t seq) {
    if (n > MAX_PACKET) return 0;
    size_t nseg = n / 255 + 1, hdr = 27 + nseg;
    if (hdr + n > cap) return 0;
    crc_init();
    memcpy(out, "OggS", 4);
    out[4] = 0;
    out[5] = flags;
    for (int i = 0; i < 8; i++) out[6 + i] = (uint8_t)(granule >> (8 * i));
    out[14] = 1;
    out[15] = out[16] = out[17] = 0; /* serial 1 */
    for (int i = 0; i < 4; i++) out[18 + i] = (uint8_t)(seq >> (8 * i));
    memset(out + 22, 0, 4);
    out[26] = (uint8_t)nseg;
    for (size_t i = 0; i + 1 < nseg; i++) out[27 + i] = 255;
    out[27 + nseg - 1] = (uint8_t)(n % 255);
    memcpy(out + hdr, pkt, n);
    uint32_t crc = 0;
    for (size_t i = 0; i < hdr + n; i++) crc = (crc << 8) ^ crc_tab[(crc >> 24) ^ out[i]];
    for (int i = 0; i < 4; i++) out[22 + i] = (uint8_t)(crc >> (8 * i));
    return hdr + n;
}
VorbisPcm *vorbispcm_create(void) { return calloc(1, sizeof(VorbisPcm)); }
static void close_decoder(VorbisPcm *v) {
    if (v->f) stb_vorbis_close(v->f);
    v->f = NULL;
}
void vorbispcm_destroy(VorbisPcm *v) {
    if (!v) return;
    close_decoder(v);
    for (int i = 0; i < 3; i++) free(v->hdr[i]);
    free(v);
}
/* Feeds the three stored header packets, as Ogg pages, to a fresh push-data decoder. */
static int open_decoder(VorbisPcm *v) {
    size_t total = 0;
    for (int i = 0; i < 3; i++) total += v->hdr_size[i] + 27 + v->hdr_size[i] / 255 + 1;
    uint8_t *buf = malloc(total);
    if (!buf) return AUDIO_NOMEM;
    size_t at = 0;
    for (int i = 0; i < 3; i++) {
        size_t n = ogg_page(buf + at, total - at, v->hdr[i], v->hdr_size[i], i == 0 ? 2 : 0, 0, (uint32_t)i);
        if (!n) {
            free(buf);
            return AUDIO_BAD;
        }
        at += n;
    }
    int used = 0, err = 0;
    close_decoder(v);
    v->f = stb_vorbis_open_pushdata(buf, (int)at, &used, &err, NULL);
    free(buf);
    v->seq = 3;
    return v->f ? AUDIO_OK : AUDIO_BAD;
}
static int store_header(VorbisPcm *v, const uint8_t *pkt, size_t n) {
    v->hdr[v->nhdr] = malloc(n);
    if (!v->hdr[v->nhdr]) return AUDIO_NOMEM;
    memcpy(v->hdr[v->nhdr], pkt, n);
    v->hdr_size[v->nhdr++] = n;
    return AUDIO_OK;
}
static int add_header_(VorbisPcm *v, const uint8_t *pkt, size_t n) {
    static const uint8_t want[3] = {1, 3, 5};
    static const uint8_t empty_comment[16] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    /* Android's MediaCodec Vorbis convention (csd-0 = identification, csd-1 = setup) leaves the comment header out: it carries nothing the decoder
     * needs, so a setup header that follows the identification header gets an empty comment header (no vendor, no comments, framing bit) in front. */
    if (v->nhdr == 1 && n >= 7 && pkt[0] == 5 && !memcmp(pkt + 1, "vorbis", 6)) {
        int r = store_header(v, empty_comment, sizeof empty_comment);
        if (r < 0) return r;
    }
    if (v->nhdr > 2 || n < 7 || pkt[0] != want[v->nhdr] || memcmp(pkt + 1, "vorbis", 6) || n > MAX_PACKET) return AUDIO_BAD;
    /* The comment header is never handed to stb_vorbis: its vendor/comment lengths are 32-bit values from the stream that it allocates and loops over (a few
     * hundred bytes could ask for 4 GiB), and nothing in them is used. The decoder gets the empty one for every comment header. */
    int r = v->nhdr == 1 ? store_header(v, empty_comment, sizeof empty_comment) : store_header(v, pkt, n);
    if (r < 0) return r;
    return v->nhdr == 3 ? open_decoder(v) : AUDIO_NEED_MORE;
}
/* A rejected header leaves the collection as it was (a synthesised comment header included). */
static int add_header(VorbisPcm *v, const uint8_t *pkt, size_t n) {
    int had = v->nhdr, r = add_header_(v, pkt, n);
    if (r < 0 && !v->f)
        while (v->nhdr > had) free(v->hdr[--v->nhdr]), v->hdr[v->nhdr] = NULL;
    return r;
}
static void vorbispcm_forget_headers(VorbisPcm *v) {
    for (int i = 0; i < 3; i++) free(v->hdr[i]), v->hdr[i] = NULL;
    v->nhdr = 0;
    close_decoder(v);
}
/* Parses the Xiph-laced blob into the pieces and feeds the packets in; `reset` starts from no headers. */
static int xiph_headers(VorbisPcm *v, const uint8_t *blob, size_t size, int reset) {
    if (!v || size < 3 || blob[0] != 2) return AUDIO_BAD;
    size_t p = 1, len[3] = {0, 0, 0};
    for (int i = 0; i < 2; i++) {
        for (;;) {
            if (p >= size) return AUDIO_BAD;
            uint8_t b = blob[p++];
            len[i] += b;
            if (b != 255) break;
        }
    }
    if (p > size || len[0] + len[1] > size - p) return AUDIO_BAD;
    len[2] = size - p - len[0] - len[1];
    if (reset) vorbispcm_forget_headers(v);
    int r = AUDIO_NEED_MORE;
    for (int i = 0; i < 3; i++) {
        r = add_header(v, blob + p, len[i]);
        if (r < 0) return r;
        p += len[i];
    }
    return r;
}
int vorbispcm_set_headers_xiph(VorbisPcm *v, const uint8_t *blob, size_t size) { return xiph_headers(v, blob, size, 1); }
/* One out-of-band blob: a Xiph-laced bundle (first byte 2) or a single header packet (first byte 1, 3 or 5). */
static int csd_blob(VorbisPcm *v, const uint8_t *blob, size_t size) {
    if (!blob || !size) return AUDIO_OK;
    if (blob[0] == 2) return xiph_headers(v, blob, size, 0);
    int r = add_header(v, blob, size);
    return r == AUDIO_NEED_MORE ? AUDIO_OK : r;
}
int vorbispcm_set_headers_csd(VorbisPcm *v, const uint8_t *const blobs[3], const size_t sizes[3]) {
    if (!v) return AUDIO_BAD;
    vorbispcm_forget_headers(v);
    for (int i = 0; i < 3; i++) {
        int r = csd_blob(v, blobs[i], sizes[i]);
        if (r < 0) {
            vorbispcm_forget_headers(v);
            return r;
        }
    }
    return v->f ? AUDIO_OK : AUDIO_NEED_MORE;
}
int vorbispcm_packet(VorbisPcm *v, const uint8_t *pkt, size_t n, int16_t *out, int max_frames, int *frames) {
    *frames = 0;
    if (!v || !pkt || !n) return AUDIO_BAD;
    if (pkt[0] & 1) {              /* a header packet */
        if (v->f) return AUDIO_OK; /* a repeat of the headers after they were all taken (csd given twice): ignore */
        int r = add_header(v, pkt, n);
        return r == AUDIO_NEED_MORE ? AUDIO_OK : r;
    }
    if (!v->f) return AUDIO_NEED_MORE;
    if (n > MAX_PACKET) return AUDIO_BAD;
    size_t cap = n + 27 + n / 255 + 2 + 4; /* 4 bytes of slack after the page: the resync scan stops 3 bytes short of the end of its data */
    uint8_t *page = calloc(1, cap);
    if (!page) return AUDIO_NOMEM;
    size_t plen = ogg_page(page, cap - 4, pkt, n, 0, 0, v->seq++);
    int ch = 0, samples = 0, rc = AUDIO_OK, used;
    float **pcm = NULL;
    /* After a flush stb_vorbis first searches for a page and verifies its CRC, which swallows that page; the same page is then fed again. */
    int searching = v->f->page_crc_tests >= 0;
    used = stb_vorbis_decode_frame_pushdata(v->f, page, (int)plen + 4, &ch, &pcm, &samples);
    if (searching && v->f->page_crc_tests < 0) used = stb_vorbis_decode_frame_pushdata(v->f, page, (int)plen + 4, &ch, &pcm, &samples);
    if (used == 0 && stb_vorbis_get_error(v->f) != VORBIS__no_error) rc = AUDIO_BAD;
    else if (samples > 0 && pcm) {
        if (samples > max_frames) samples = max_frames;
        for (int i = 0; i < samples; i++)
            for (int c = 0; c < ch; c++) {
                float x = pcm[c][i] * 32767.0f;
                out[i * ch + c] = (int16_t)(x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : x != x ? 0 : (x < 0 ? x - 0.5f : x + 0.5f)); /* NaN (a corrupt stream) -> 0 */
            }
        *frames = samples;
    }
    free(page);
    return rc;
}
int vorbispcm_ready(const VorbisPcm *v) { return v && v->f; }
int vorbispcm_channels(const VorbisPcm *v) { return v && v->f ? v->f->channels : 0; }
int vorbispcm_rate(const VorbisPcm *v) { return v && v->f ? (int)v->f->sample_rate : 0; }
void vorbispcm_flush(VorbisPcm *v) {
    if (v && v->f) stb_vorbis_flush_pushdata(v->f);
}

/* ---- Opus ---- */
#ifdef NAPPLE_HAVE_OPUS /* the runtime Makefile defines it (libopus is linked there already); the Linux checks leave it off unless built with -lopus */
#include <opus/opus.h>
struct OpusPcm {
    OpusDecoder *dec;
    int channels, skip, pre_skip;
};
int opuspcm_available(void) { return 1; }
OpusPcm *opuspcm_create(int channels, int pre_skip) {
    if (channels < 1 || channels > 2 || pre_skip < 0) return NULL;
    OpusPcm *o = calloc(1, sizeof *o);
    int err = 0;
    if (!o) return NULL;
    o->dec = opus_decoder_create(48000, channels, &err);
    if (!o->dec) {
        free(o);
        return NULL;
    }
    o->channels = channels;
    o->skip = o->pre_skip = pre_skip;
    return o;
}
void opuspcm_destroy(OpusPcm *o) {
    if (!o) return;
    opus_decoder_destroy(o->dec);
    free(o);
}
int opuspcm_packet(OpusPcm *o, const uint8_t *pkt, size_t size, int16_t *out, int max_frames, int *frames) {
    *frames = 0;
    if (!o || !pkt || !size) return AUDIO_BAD;
    int n = opus_decode(o->dec, pkt, (opus_int32)size, out, max_frames, 0);
    if (n < 0) return AUDIO_BAD;
    if (o->skip > 0) { /* OpusHead pre-skip: drop the first samples */
        int d = o->skip < n ? o->skip : n;
        memmove(out, out + (size_t)d * o->channels, (size_t)(n - d) * o->channels * sizeof *out);
        o->skip -= d;
        n -= d;
    }
    *frames = n;
    return AUDIO_OK;
}
void opuspcm_flush(OpusPcm *o) {
    if (!o) return;
    opus_decoder_ctl(o->dec, OPUS_RESET_STATE);
    o->skip = o->pre_skip;
}
#else
int opuspcm_available(void) { return 0; }
OpusPcm *opuspcm_create(int channels, int pre_skip) {
    (void)channels;
    (void)pre_skip;
    return NULL;
}
void opuspcm_destroy(OpusPcm *o) { (void)o; }
int opuspcm_packet(OpusPcm *o, const uint8_t *pkt, size_t size, int16_t *out, int max_frames, int *frames) {
    (void)o;
    (void)pkt;
    (void)size;
    (void)out;
    (void)max_frames;
    *frames = 0;
    return AUDIO_UNSUPPORTED;
}
void opuspcm_flush(OpusPcm *o) { (void)o; }
#endif
int opuspcm_parse_head(const uint8_t *h, size_t size, int *channels, int *pre_skip) {
    if (size < 19 || memcmp(h, "OpusHead", 8) || h[8] != 1) return -1;
    *channels = h[9];
    *pre_skip = h[10] | (h[11] << 8);
    return 0;
}

/* ---- AAC ---- */
static const int AAC_RATES[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
int aac_channel_config(int channels) { return channels >= 1 && channels <= 6 ? channels : channels == 8 ? 7 : 0; }
static int aac_channels_of(int config) { return config >= 1 && config <= 6 ? config : config == 7 ? 8 : 0; }
typedef struct {
    const uint8_t *p;
    size_t n, bit;
} Bits;
static int bits(Bits *b, int k) {
    int v = 0;
    while (k--) {
        if (b->bit >= b->n * 8) return -1;
        v = (v << 1) | ((b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1);
        b->bit++;
    }
    return v;
}
static int asc_object(Bits *b) {
    int t = bits(b, 5);
    return t == 31 ? (t = bits(b, 6)) < 0 ? -1 : 32 + t : t;
}
static int asc_rate(Bits *b) {
    int i = bits(b, 4);
    return i < 0 ? -1 : i == 15 ? bits(b, 24) : i < 13 ? AAC_RATES[i] : -1;
}
int aac_parse_asc(const uint8_t *asc, size_t size, AacConfig *out) {
    Bits b = {asc, size, 0};
    AacConfig c = {0};
    if (!asc || size < 2 || (c.object_type = asc_object(&b)) <= 0 || (c.rate = asc_rate(&b)) <= 0) return -1;
    int cfg = bits(&b, 4);
    if (cfg < 0) return -1;
    c.channels = aac_channels_of(cfg);
    if (c.object_type == 5 || c.object_type == 29) { /* explicit SBR / PS: extension rate, then the core object type */
        c.sbr = 1;
        if (asc_rate(&b) <= 0 || (c.object_type = asc_object(&b)) <= 0) return -1;
    }
    *out = c;
    return 0;
}
size_t aac_parse_adts(const uint8_t *p, size_t size, AacConfig *out, size_t *frame_len) {
    if (!p || size < 7 || p[0] != 0xff || (p[1] & 0xf6) != 0xf0) return 0; /* sync 0xFFF, layer 0 */
    int fi = (p[2] >> 2) & 15, cfg = ((p[2] & 1) << 2) | (p[3] >> 6);
    size_t len = ((size_t)(p[3] & 3) << 11) | ((size_t)p[4] << 3) | (p[5] >> 5), hdr = (p[1] & 1) ? 7 : 9;
    if (fi >= 13 || len < hdr) return 0;
    if (out) *out = (AacConfig){((p[2] >> 6) & 3) + 1, AAC_RATES[fi], aac_channels_of(cfg), 0};
    if (frame_len) *frame_len = len;
    return hdr;
}
size_t aac_make_asc(const AacConfig *c, uint8_t out[2]) {
    int fi = -1, cfg = aac_channel_config(c->channels);
    for (int i = 0; i < 13; i++)
        if (AAC_RATES[i] == c->rate) fi = i;
    if (fi < 0 || !cfg || c->object_type <= 0 || c->object_type >= 31) return 0;
    out[0] = (uint8_t)((c->object_type << 3) | (fi >> 1));
    out[1] = (uint8_t)(((fi & 1) << 7) | (cfg << 3));
    return 2;
}

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
struct AacPcm {
    AudioConverterRef conv;
    AacConfig cfg;
    int rate, channels, ready, reorder; /* reorder: the converter kept the AAC bitstream order, remap to WAVE order here */
    uint8_t asc[64];
    size_t asc_size;
    const uint8_t *in; /* the access unit the input callback hands over (once) */
    UInt32 in_size;
    AudioStreamPacketDescription desc;
};
enum { NO_MORE_INPUT = 0x6e6d6f72 }; /* 'nmor': the callback has nothing more for this call (not end of stream) */
static OSStatus aac_input(AudioConverterRef c, UInt32 *n, AudioBufferList *io, AudioStreamPacketDescription **desc, void *user) {
    (void)c;
    AacPcm *a = user;
    if (!a->in) {
        *n = 0;
        return NO_MORE_INPUT;
    }
    io->mBuffers[0].mData = (void *)a->in;
    io->mBuffers[0].mDataByteSize = a->in_size;
    io->mBuffers[0].mNumberChannels = (UInt32)a->channels;
    a->desc = (AudioStreamPacketDescription){0, 0, a->in_size};
    if (desc) *desc = &a->desc;
    *n = 1;
    a->in = NULL;
    return noErr;
}
/* An esds payload (ES_Descriptor > DecoderConfigDescriptor > DecoderSpecificInfo = the ASC, SLConfig) as AudioToolbox takes for its AAC magic cookie. */
static size_t esds(const uint8_t *asc, size_t n, uint8_t *o) {
    size_t k = 0, l4 = 13 + 2 + n, l3 = 3 + 2 + l4 + 3;
    o[k++] = 3, o[k++] = (uint8_t)l3, o[k++] = 0, o[k++] = 0, o[k++] = 0;
    o[k++] = 4, o[k++] = (uint8_t)l4, o[k++] = 0x40, o[k++] = 0x15;
    memset(o + k, 0, 11), k += 11; /* buffer size (3), max bitrate (4), average bitrate (4) */
    o[k++] = 5, o[k++] = (uint8_t)n;
    memcpy(o + k, asc, n), k += n;
    o[k++] = 6, o[k++] = 1, o[k++] = 2;
    return k;
}
/* AAC bitstream order of each channel configuration -> WAVE order (index into the decoded frame for each output channel). */
static const int8_t AAC_TO_WAVE[9][8] = {{0},
                                         {0},
                                         {0, 1},
                                         {1, 2, 0},                /* C L R -> L R C */
                                         {1, 2, 0, 3},             /* C L R Cs -> L R C Cs */
                                         {1, 2, 0, 3, 4},          /* C L R Ls Rs -> L R C Ls Rs */
                                         {1, 2, 0, 5, 3, 4},       /* C L R Ls Rs LFE -> L R C LFE Ls Rs */
                                         {0},
                                         {3, 4, 0, 7, 5, 6, 1, 2}}; /* C Lc Rc L R Ls Rs LFE -> L R C LFE Ls Rs Lc Rc */
static const AudioChannelLayoutTag WAVE_TAG[9] = {0, 0, 0, kAudioChannelLayoutTag_MPEG_3_0_A, kAudioChannelLayoutTag_MPEG_4_0_A, kAudioChannelLayoutTag_MPEG_5_0_A,
                                                   kAudioChannelLayoutTag_MPEG_5_1_A, 0, kAudioChannelLayoutTag_MPEG_7_1_C};
static int aac_open(AacPcm *a) {
    uint8_t cookie[96];
    size_t cn = esds(a->asc, a->asc_size, cookie);
    AudioStreamBasicDescription in = {.mFormatID = a->cfg.sbr ? kAudioFormatMPEG4AAC_HE : kAudioFormatMPEG4AAC};
    UInt32 sz = sizeof in;
    if (AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, (UInt32)cn, cookie, &sz, &in) != noErr || !in.mSampleRate || !in.mChannelsPerFrame) {
        in = (AudioStreamBasicDescription){.mFormatID = kAudioFormatMPEG4AAC,
                                           .mSampleRate = a->cfg.rate,
                                           .mChannelsPerFrame = (UInt32)a->cfg.channels,
                                           .mFramesPerPacket = 1024,
                                           .mFormatFlags = kMPEG4Object_AAC_LC};
    }
    if (!in.mChannelsPerFrame || in.mChannelsPerFrame > 8 || in.mChannelsPerFrame == 7) return AUDIO_UNSUPPORTED;
    AudioStreamBasicDescription out = {.mSampleRate = in.mSampleRate,
                                       .mFormatID = kAudioFormatLinearPCM,
                                       .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
                                       .mBytesPerPacket = 2 * in.mChannelsPerFrame,
                                       .mFramesPerPacket = 1,
                                       .mBytesPerFrame = 2 * in.mChannelsPerFrame,
                                       .mChannelsPerFrame = in.mChannelsPerFrame,
                                       .mBitsPerChannel = 16};
    if (AudioConverterNew(&in, &out, &a->conv) != noErr) {
        a->conv = NULL;
        return AUDIO_UNSUPPORTED;
    }
    AudioConverterSetProperty(a->conv, kAudioConverterDecompressionMagicCookie, (UInt32)cn, cookie);
    a->rate = (int)in.mSampleRate;
    a->channels = (int)in.mChannelsPerFrame;
    a->reorder = 0;
    if (a->channels >= 3 && WAVE_TAG[a->channels]) { /* ask the converter for WAVE order; if it refuses, remap from the bitstream order ourselves */
        AudioChannelLayout l = {.mChannelLayoutTag = WAVE_TAG[a->channels]};
        a->reorder = AudioConverterSetProperty(a->conv, kAudioConverterOutputChannelLayout, sizeof l, &l) != noErr;
    }
    a->ready = 1;
    return AUDIO_OK;
}
int aacpcm_available(void) { return 1; }
AacPcm *aacpcm_create(const uint8_t *asc, size_t asc_size, int rate, int channels) {
    AacPcm *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    if (asc && asc_size && asc_size <= sizeof a->asc && !aac_parse_asc(asc, asc_size, &a->cfg)) {
        memcpy(a->asc, asc, asc_size);
        a->asc_size = asc_size;
    } else if (rate > 0 && channels > 0) {
        a->cfg = (AacConfig){2, rate, channels, 0};
        a->asc_size = aac_make_asc(&a->cfg, a->asc);
    }
    if (a->asc_size && aac_open(a) != AUDIO_OK) {
        free(a);
        return NULL;
    }
    return a;
}
void aacpcm_destroy(AacPcm *a) {
    if (!a) return;
    if (a->conv) AudioConverterDispose(a->conv);
    free(a);
}
int aacpcm_ready(const AacPcm *a) { return a && a->ready; }
int aacpcm_rate(const AacPcm *a) { return a ? a->rate : 0; }
int aacpcm_channels(const AacPcm *a) { return a ? a->channels : 0; }
void aacpcm_flush(AacPcm *a) {
    if (a && a->conv) AudioConverterReset(a->conv);
}
int aacpcm_packet(AacPcm *a, const uint8_t *pkt, size_t size, int16_t *out, int max_frames, int *frames) {
    *frames = 0;
    if (!a || !pkt || !size) return AUDIO_BAD;
    AacConfig hc;
    size_t flen = 0, h = aac_parse_adts(pkt, size, &hc, &flen);
    if (h) { /* ADTS framing: strip the header; the first one configures a decoder that had no csd-0 */
        if (flen > size) return AUDIO_BAD;
        if (!a->ready) {
            a->cfg = hc;
            if (!(a->asc_size = aac_make_asc(&a->cfg, a->asc))) return AUDIO_UNSUPPORTED;
            int rc = aac_open(a);
            if (rc != AUDIO_OK) return rc;
        }
        pkt += h;
        size = flen - h;
    }
    if (!a->ready) return AUDIO_NEED_MORE;
    if (max_frames > AACPCM_MAX_FRAMES) max_frames = AACPCM_MAX_FRAMES;
    a->in = pkt;
    a->in_size = (UInt32)size;
    UInt32 n = (UInt32)max_frames;
    AudioBufferList abl = {1, {{(UInt32)a->channels, (UInt32)(max_frames * a->channels * 2), out}}};
    OSStatus st = AudioConverterFillComplexBuffer(a->conv, aac_input, a, &n, &abl, NULL);
    a->in = NULL;
    if (st != noErr && st != NO_MORE_INPUT) return AUDIO_BAD;
    if (a->reorder && n) {
        const int8_t *map = AAC_TO_WAVE[a->channels];
        int16_t t[8];
        for (UInt32 f = 0; f < n; f++) {
            int16_t *s = out + (size_t)f * a->channels;
            for (int c = 0; c < a->channels; c++) t[c] = s[map[c]];
            memcpy(s, t, (size_t)a->channels * 2);
        }
    }
    *frames = (int)n;
    return AUDIO_OK;
}
#else
int aacpcm_available(void) { return 0; }
AacPcm *aacpcm_create(const uint8_t *asc, size_t asc_size, int rate, int channels) {
    (void)asc;
    (void)asc_size;
    (void)rate;
    (void)channels;
    return NULL;
}
void aacpcm_destroy(AacPcm *a) { (void)a; }
int aacpcm_ready(const AacPcm *a) { return (void)a, 0; }
int aacpcm_rate(const AacPcm *a) { return (void)a, 0; }
int aacpcm_channels(const AacPcm *a) { return (void)a, 0; }
void aacpcm_flush(AacPcm *a) { (void)a; }
int aacpcm_packet(AacPcm *a, const uint8_t *pkt, size_t size, int16_t *out, int max_frames, int *frames) {
    (void)a;
    (void)pkt;
    (void)size;
    (void)out;
    (void)max_frames;
    *frames = 0;
    return AUDIO_UNSUPPORTED;
}
#endif
