/* Fuzz target: movie audio core (services/movie_audio_core.c: Vorbis through stb_vorbis with the raw-packet Ogg wrapper, OpusHead). The movie library hands the
 * codec headers and packets of a guest-supplied media file to these functions.
 * Input: [mode][bytes]. Modes 0..5 demux `bytes` as an Ogg file and feed it the way movie.c does (0 headers in-band, 1 one Xiph-laced csd-0, 2 csd-0 + csd-1,
 * 3 csd-0..2, 4 identification only; 5 Opus). Mode 6 gives `bytes` straight to the Xiph csd-0 parser, mode 7 splits `bytes` into three csd blobs. */
#include "../../src/services/movie_audio_core.h"
#include "../audio_vectors.h"
#include "fuzz.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *p;
    size_t n;
} Pkt;
/* Lenient Ogg demuxer: the packets of every page in order (a packet continued over a page boundary is delivered in pieces), at most `max`. */
static int demux(const uint8_t *d, size_t n, Pkt *out, int max) {
    int np = 0;
    size_t pos = 0;
    while (pos + 27 <= n && !memcmp(d + pos, "OggS", 4) && np < max) {
        unsigned nseg = d[pos + 26];
        size_t body = pos + 27 + nseg, at = body, len = 0;
        if (body > n) break;
        for (unsigned i = 0; i < nseg && np < max; i++) {
            len += d[pos + 27 + i];
            if (d[pos + 27 + i] < 255 || i + 1 == nseg) {
                if (at + len > n) len = n - at;
                out[np].p = d + at;
                out[np++].n = len;
                at += len;
                len = 0;
            }
        }
        pos = at;
    }
    return np;
}

static int16_t pcm[VORBISPCM_MAX_FRAMES * 2];
static void feed_vorbis(VorbisPcm *v, const Pkt *pk, int from, int np) {
    for (int i = from; i < np; i++) {
        int frames = 0, ch = vorbispcm_channels(v);
        /* max_frames is what fits the caller's buffer: 2 channels here, so a wider stream gets proportionally fewer frames */
        int cap = ch > 2 ? VORBISPCM_MAX_FRAMES * 2 / ch : VORBISPCM_MAX_FRAMES;
        if (ch > 255) return;
        vorbispcm_packet(v, pk[i].p, pk[i].n, pcm, cap, &frames);
        if (i == from + 7) vorbispcm_flush(v);
    }
}

int fuzz_movie_audio(const uint8_t *d, size_t n) {
    if (!n) return 0;
    unsigned mode = d[0] & 7;
    d++, n--;
    Pkt pk[64];
    static uint8_t blob[1 << 16];
    if (mode == 6) {
        VorbisPcm *v = vorbispcm_create();
        if (vorbispcm_set_headers_xiph(v, d, n) == AUDIO_OK) vorbispcm_flush(v);
        vorbispcm_destroy(v);
        return 0;
    }
    if (mode == 7) { /* three csd blobs cut out of the input at positions it gives */
        FzIn r = {d, n, 0};
        size_t a = fz_u16(&r), b = fz_u16(&r), c = fz_u16(&r), at = 6;
        const uint8_t *blobs[3];
        size_t sizes[3] = {a, b, c};
        for (int i = 0; i < 3; i++) {
            if (at > n) at = n;
            if (sizes[i] > n - at) sizes[i] = n - at;
            blobs[i] = sizes[i] ? d + at : NULL;
            at += sizes[i];
        }
        VorbisPcm *v = vorbispcm_create();
        vorbispcm_set_headers_csd(v, blobs, sizes);
        vorbispcm_destroy(v);
        return 0;
    }
    int np = demux(d, n, pk, 64);
    if (mode == 5) {
        int ch = 0, skip = 0;
        if (np && opuspcm_parse_head(pk[0].p, pk[0].n, &ch, &skip) == 0) {
            OpusPcm *o = opuspcm_create(ch, skip);
            if (o) {
                for (int i = 1; i < np; i++) {
                    int frames = 0;
                    opuspcm_packet(o, pk[i].p, pk[i].n, pcm, OPUSPCM_MAX_FRAMES / 2, &frames);
                }
                opuspcm_flush(o);
                opuspcm_destroy(o);
            }
        }
        opuspcm_available();
        return 0;
    }
    if (np < 3 && mode) return 0;
    VorbisPcm *v = vorbispcm_create();
    int first = 0;
    const uint8_t *b[3] = {0, 0, 0};
    size_t sz[3] = {0, 0, 0};
    switch (mode) {
        case 1: { /* csd-0: count-1, Xiph-laced lengths of the first two packets, the three packets */
            size_t at = 0;
            blob[at++] = 2;
            for (int i = 0; i < 2; i++) {
                size_t l = pk[i].n;
                while (l >= 255 && at < 4000) blob[at++] = 255, l -= 255;
                blob[at++] = (uint8_t)l;
            }
            for (int i = 0; i < 3 && at + pk[i].n <= sizeof blob; i++) memcpy(blob + at, pk[i].p, pk[i].n), at += pk[i].n;
            b[0] = blob, sz[0] = at, first = 3;
            vorbispcm_set_headers_csd(v, b, sz);
            break;
        }
        case 2: b[0] = pk[0].p, sz[0] = pk[0].n, b[1] = pk[2].p, sz[1] = pk[2].n, first = 3, vorbispcm_set_headers_csd(v, b, sz); break;
        case 3:
            for (int i = 0; i < 3; i++) b[i] = pk[i].p, sz[i] = pk[i].n;
            first = 3;
            vorbispcm_set_headers_csd(v, b, sz);
            break;
        case 4: b[0] = pk[0].p, sz[0] = pk[0].n, first = 1, vorbispcm_set_headers_csd(v, b, sz); break;
        default: break;
    }
    feed_vorbis(v, pk, first, np);
    vorbispcm_ready(v), vorbispcm_rate(v);
    vorbispcm_destroy(v);
    return 0;
}

static void with_mode(FuzzSeedFn add, void *ctx, unsigned mode, const uint8_t *ogg, size_t n) {
    uint8_t *b = malloc(n + 1);
    b[0] = (uint8_t)mode;
    memcpy(b + 1, ogg, n);
    add(ctx, b, n + 1);
    free(b);
}
void fuzz_movie_audio_seeds(FuzzSeedFn add, void *ctx) {
    for (unsigned m = 0; m <= 4; m++) {
        with_mode(add, ctx, m, mono_ogg, sizeof mono_ogg);
        with_mode(add, ctx, m, st_ogg, sizeof st_ogg);
    }
    with_mode(add, ctx, 5, opus_ogg, sizeof opus_ogg);
    /* mode 6/7: the three headers of the mono file as a Xiph bundle / split csd blobs */
    Pkt pk[64];
    int np = demux(mono_ogg, sizeof mono_ogg, pk, 64);
    if (np >= 3) {
        static uint8_t buf[1 << 15];
        size_t at = 0;
        buf[at++] = 6;
        buf[at++] = 2;
        for (int i = 0; i < 2; i++) {
            size_t l = pk[i].n;
            while (l >= 255) buf[at++] = 255, l -= 255;
            buf[at++] = (uint8_t)l;
        }
        for (int i = 0; i < 3; i++) memcpy(buf + at, pk[i].p, pk[i].n), at += pk[i].n;
        add(ctx, buf, at);
        at = 0;
        buf[at++] = 7;
        for (int i = 0; i < 3; i++) fz_put16(buf + at, (uint16_t)pk[i].n), at += 2;
        for (int i = 0; i < 3; i++) memcpy(buf + at, pk[i].p, pk[i].n), at += pk[i].n;
        add(ctx, buf, at);
    }
}
FUZZ_TARGET(movie_audio);
