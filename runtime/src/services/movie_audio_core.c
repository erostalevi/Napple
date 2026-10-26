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
static int add_header(VorbisPcm *v, const uint8_t *pkt, size_t n) {
    static const uint8_t want[3] = {1, 3, 5};
    if (n < 7 || pkt[0] != want[v->nhdr] || memcmp(pkt + 1, "vorbis", 6) || n > MAX_PACKET) return AUDIO_BAD;
    v->hdr[v->nhdr] = malloc(n);
    if (!v->hdr[v->nhdr]) return AUDIO_NOMEM;
    memcpy(v->hdr[v->nhdr], pkt, n);
    v->hdr_size[v->nhdr++] = n;
    return v->nhdr == 3 ? open_decoder(v) : AUDIO_NEED_MORE;
}
int vorbispcm_set_headers_xiph(VorbisPcm *v, const uint8_t *blob, size_t size) {
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
    for (int i = 0; i < v->nhdr; i++) free(v->hdr[i]);
    v->nhdr = 0;
    close_decoder(v);
    int r = AUDIO_NEED_MORE;
    for (int i = 0; i < 3; i++) {
        r = add_header(v, blob + p, len[i]);
        if (r < 0) return r;
        p += len[i];
    }
    return r;
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
                out[i * ch + c] = (int16_t)(x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (x < 0 ? x - 0.5f : x + 0.5f));
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
