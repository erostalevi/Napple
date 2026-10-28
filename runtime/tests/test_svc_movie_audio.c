/* Unit tests: movie audio core (Vorbis through stb_vorbis with the raw-packet Ogg wrapper, Opus when built with libopus). The vectors are sine
 * waves: the tests demux the Ogg files into raw packets (what movie::Decoder inputs are), decode them and measure the tone. */
#include "../src/services/movie_audio_core.h"
#include "check.h"
#include <math.h>
#include "audio_vectors.h"

typedef struct { const uint8_t *p; size_t n; } Pkt;
/* Minimal Ogg demuxer: packets of the first logical stream (lacing, continuation across pages). */
static int demux(const uint8_t *d, size_t n, Pkt *out, int max) {
    int np = 0; size_t pos = 0, start = 0; static uint8_t *cont; static size_t clen; free(cont); cont = NULL; clen = 0;
    while (pos + 27 <= n && !memcmp(d + pos, "OggS", 4)) {
        int nseg = d[pos + 26]; size_t body = pos + 27 + nseg, len = 0;
        for (int i = 0; i < nseg; i++) {
            len += d[pos + 27 + i];
            if (d[pos + 27 + i] < 255) {
                if (clen) { cont = realloc(cont, clen + len); memcpy(cont + clen, d + body + start, len); clen += len; out[np].p = cont; out[np].n = clen; /* only header-sized spill: keep the buffer alive */ np++; cont = NULL; clen = 0; }
                else { out[np].p = d + body + start; out[np].n = len; np++; }
                start += len; len = 0;
                if (np >= max) return np;
            }
        }
        if (len) { cont = realloc(cont, clen + len); memcpy(cont + clen, d + body + start, len); clen += len; }
        pos = body + start + len; start = 0;
    }
    return np;
}
/* Granule position of the stream's last page: the sample count (Vorbis: after the end trim; Opus: plus the pre-skip, at 48 kHz). */
static int64_t last_granule(const uint8_t *d, size_t n) {
    int64_t g = -1;
    for (size_t pos = 0; pos + 27 <= n && !memcmp(d + pos, "OggS", 4);) {
        int nseg = d[pos + 26]; size_t body = 0;
        for (int i = 0; i < nseg; i++) body += d[pos + 27 + i];
        memcpy(&g, d + pos + 6, 8);
        pos += 27 + nseg + body;
    }
    return g;
}
static double tone_hz(const int16_t *x, int n, int ch, int rate, double *amp) { /* zero-crossing frequency of channel 0 over the steady part */
    int a = n / 4, b = n - n / 4, cross = 0; double peak = 0;
    for (int i = a + 1; i < b; i++) { int16_t p = x[(i - 1) * ch], q = x[i * ch]; if ((p < 0) != (q < 0)) cross++; if (fabs((double)q) > peak) peak = fabs((double)q); }
    *amp = peak / 32768.0;
    return cross / 2.0 / ((b - a) / (double)rate);
}
static int decode_all(const uint8_t *ogg, size_t n, int use_xiph, int16_t **pcm, int *frames, int *ch, int *rate) {
    static Pkt pk[400]; int np = demux(ogg, n, pk, 400);
    VorbisPcm *v = vorbispcm_create(); int total = 0; *pcm = malloc(sizeof(int16_t) * 2 * 60000); int rc = 0; static int16_t buf[VORBISPCM_MAX_FRAMES * 2];
    int first = 0;
    if (use_xiph) { /* csd-0: count-1, laced lengths of packets 0 and 1, the three packets */
        static uint8_t blob[16384]; size_t at = 0; blob[at++] = 2;
        for (int i = 0; i < 2; i++) { size_t l = pk[i].n; while (l >= 255) { blob[at++] = 255; l -= 255; } blob[at++] = (uint8_t)l; }
        for (int i = 0; i < 3; i++) { memcpy(blob + at, pk[i].p, pk[i].n); at += pk[i].n; }
        rc = vorbispcm_set_headers_xiph(v, blob, at); CHECK(rc == AUDIO_OK && vorbispcm_ready(v), "headers via csd-0 (Xiph lacing): ready");
        first = 3;
    }
    for (int i = first; i < np; i++) {
        int fr = 0; rc = vorbispcm_packet(v, pk[i].p, pk[i].n, buf, VORBISPCM_MAX_FRAMES, &fr);
        if (rc < 0) { CHECK(0, "packet %d failed (%d)", i, rc); break; }
        if (i < 3 && !use_xiph) CHECK(fr == 0 && (i < 2 ? !vorbispcm_ready(v) : vorbispcm_ready(v)), "in-band header %d accepted without output", i);
        int c = vorbispcm_channels(v);
        if (fr) { memcpy(*pcm + (size_t)total * c, buf, sizeof(int16_t) * (size_t)fr * c); total += fr; }
    }
    *frames = total; *ch = vorbispcm_channels(v); *rate = vorbispcm_rate(v);
    vorbispcm_destroy(v);
    return np;
}
/* Out-of-band headers the way the movie library's format metadata carries them (csd-0..2); mode 1 = one Xiph bundle, 2 = identification + setup (no comment
 * header), 3 = the three packets, 4 = identification only, the other two arrive as the first input buffers. Returns the decoded frames. */
static int decode_csd(const uint8_t *ogg, size_t n, int mode, int expect_rc, int *ch, int *rate, double *hz) {
    static Pkt pk[400]; int np = demux(ogg, n, pk, 400);
    static uint8_t blob[16384]; size_t at = 0;
    const uint8_t *b[3] = {0, 0, 0}; size_t sz[3] = {0, 0, 0}; int first = 3;
    if (mode == 1) {
        blob[at++] = 2;
        for (int i = 0; i < 2; i++) { size_t l = pk[i].n; while (l >= 255) { blob[at++] = 255; l -= 255; } blob[at++] = (uint8_t)l; }
        for (int i = 0; i < 3; i++) { memcpy(blob + at, pk[i].p, pk[i].n); at += pk[i].n; }
        b[0] = blob; sz[0] = at;
    } else if (mode == 2) { b[0] = pk[0].p; sz[0] = pk[0].n; b[1] = pk[2].p; sz[1] = pk[2].n; }
    else if (mode == 3) { for (int i = 0; i < 3; i++) { b[i] = pk[i].p; sz[i] = pk[i].n; } }
    else { b[0] = pk[0].p; sz[0] = pk[0].n; first = 1; }
    VorbisPcm *v = vorbispcm_create(); int total = 0; int16_t *pcm = malloc(sizeof(int16_t) * 2 * 60000); static int16_t buf[VORBISPCM_MAX_FRAMES * 2];
    int rc = vorbispcm_set_headers_csd(v, b, sz);
    CHECK(rc == expect_rc, "csd mode %d: set_headers_csd -> %d (want %d)", mode, rc, expect_rc);
    for (int i = mode == 4 ? 1 : 3; i < np; i++) {
        int fr = 0; rc = vorbispcm_packet(v, pk[i].p, pk[i].n, buf, VORBISPCM_MAX_FRAMES, &fr);
        if (rc < 0) { CHECK(0, "csd mode %d packet %d failed (%d)", mode, i, rc); break; }
        if (fr) { memcpy(pcm + (size_t)total * vorbispcm_channels(v), buf, sizeof(int16_t) * (size_t)fr * vorbispcm_channels(v)); total += fr; }
    }
    (void)first;
    *ch = vorbispcm_channels(v); *rate = vorbispcm_rate(v);
    double amp; *hz = total ? tone_hz(pcm, total, *ch > 0 ? *ch : 1, *rate, &amp) : 0;
    free(pcm); vorbispcm_destroy(v);
    return total;
}
int main(void) {
    int16_t *pcm; int frames, ch, rate; double amp;
    int np = decode_all(mono_ogg, sizeof mono_ogg, 0, &pcm, &frames, &ch, &rate);
    CHECK(np > 6 && ch == 1 && rate == 22050, "mono vector: %d packets, %d ch, %d Hz", np, ch, rate);
    CHECK(frames > 22050 * 0.5 && frames <= 22050 * 0.7, "mono: %d sample frames for 0.6 s", frames);
    { /* exact count: what the stream's last granule says, plus at most the end trim of the last block (raw packets carry no granule, so it is not applied) */
        int64_t g = last_granule(mono_ogg, sizeof mono_ogg);
        CHECK(g > 0 && frames >= g && frames - g < 2048, "mono: %d sample frames decoded, stream granule %lld (end trim %lld)", frames, (long long)g, (long long)(frames - g));
    }
    double hz = tone_hz(pcm, frames, 1, rate, &amp); CHECK(fabs(hz - 440.0) < 6.0 && amp > 0.05, "mono: tone %.1f Hz (want 440), peak %.2f", hz, amp);
    free(pcm);

    decode_all(mono_ogg, sizeof mono_ogg, 1, &pcm, &frames, &ch, &rate);
    hz = tone_hz(pcm, frames, ch, rate, &amp); CHECK(ch == 1 && fabs(hz - 440.0) < 6.0, "mono via csd-0 blob: %.1f Hz", hz);
    free(pcm);

    decode_all(st_ogg, sizeof st_ogg, 0, &pcm, &frames, &ch, &rate);
    CHECK(ch == 2 && rate == 44100 && frames > 44100 * 0.4, "stereo vector: %d ch, %d Hz, %d frames", ch, rate, frames);
    { int64_t g = last_granule(st_ogg, sizeof st_ogg); CHECK(g > 0 && frames >= g && frames - g < 2048, "stereo: %d sample frames decoded, stream granule %lld", frames, (long long)g); }
    hz = tone_hz(pcm, frames, 2, rate, &amp); double hr = 0, amp_r = 0; /* right channel: same tone at half level */
    { int16_t *r = pcm + 1; int a = frames / 4, b = frames - frames / 4; for (int i = a; i < b; i++) if (fabs((double)r[i * 2]) > amp_r) amp_r = fabs((double)r[i * 2]); amp_r /= 32768.0; (void)hr; }
    CHECK(fabs(hz - 1000.0) < 12.0 && amp > 0.05 && amp_r > amp * 0.35 && amp_r < amp * 0.65, "stereo: left %.0f Hz peak %.2f, right peak %.2f (about half)", hz, amp, amp_r);
    free(pcm);

    { /* out-of-band headers (SDK-style container: the format metadata holds the headers, the inputs are bare audio packets) */
        int ch2, rate2; double hz2; int ref = frames;
        decode_all(mono_ogg, sizeof mono_ogg, 0, &pcm, &frames, &ch, &rate); ref = frames; free(pcm);
        for (int mode = 1; mode <= 4; mode++) {
            int f = decode_csd(mono_ogg, sizeof mono_ogg, mode, mode == 4 ? AUDIO_NEED_MORE : AUDIO_OK, &ch2, &rate2, &hz2);
            CHECK(ch2 == 1 && rate2 == 22050 && f == ref && fabs(hz2 - 440.0) < 6.0, "csd mode %d: %d frames (in-band %d), %.1f Hz", mode, f, ref, hz2);
        }
        int f = decode_csd(st_ogg, sizeof st_ogg, 2, AUDIO_OK, &ch2, &rate2, &hz2);
        CHECK(ch2 == 2 && rate2 == 44100 && f > 44100 * 0.4 && fabs(hz2 - 1000.0) < 12.0, "csd identification + setup, stereo: %d frames %.0f Hz", f, hz2);
        VorbisPcm *v = vorbispcm_create(); const uint8_t junk[8] = {2, 5, 5, 1, 2, 3, 4, 5}; const uint8_t *b[3] = {junk, 0, 0}; size_t sz[3] = {8, 0, 0};
        CHECK(vorbispcm_set_headers_csd(v, b, sz) == AUDIO_BAD && !vorbispcm_ready(v), "csd: malformed bundle refused");
        const uint8_t setup_first[8] = {5, 'v', 'o', 'r', 'b', 'i', 's', 0}; b[0] = setup_first; sz[0] = 8;
        CHECK(vorbispcm_set_headers_csd(v, b, sz) == AUDIO_BAD, "csd: setup header without an identification header refused");
        const uint8_t *none[3] = {0, 0, 0}; size_t zero[3] = {0, 0, 0};
        CHECK(vorbispcm_set_headers_csd(v, none, zero) == AUDIO_NEED_MORE, "csd: no blobs: still waiting for headers");
        vorbispcm_destroy(v);
    }
    { /* error handling */
        VorbisPcm *v = vorbispcm_create(); int fr = 7; int16_t out[64]; uint8_t audio[8] = {0x00, 1, 2, 3};
        CHECK(vorbispcm_packet(v, audio, 4, out, 64, &fr) == AUDIO_NEED_MORE && fr == 0, "audio before the headers: need more");
        uint8_t bad[8] = {1, 'v', 'o', 'r', 'b', 'i', 's'};
        CHECK(vorbispcm_packet(v, bad, 7, out, 64, &fr) == AUDIO_OK, "identification header prefix accepted");
        uint8_t wrong[8] = {5, 'v', 'o', 'r', 'b', 'i', 's'};
        CHECK(vorbispcm_packet(v, wrong, 7, out, 64, &fr) == AUDIO_BAD, "setup header out of order refused");
        CHECK(vorbispcm_packet(v, NULL, 0, out, 64, &fr) == AUDIO_BAD, "empty packet refused");
        uint8_t blob[4] = {2, 200, 200, 1};
        CHECK(vorbispcm_set_headers_xiph(v, blob, 4) == AUDIO_BAD, "short csd-0 refused");
        CHECK(vorbispcm_set_headers_xiph(v, (const uint8_t *)"", 0) == AUDIO_BAD, "empty csd-0 refused");
        vorbispcm_destroy(v);
        v = vorbispcm_create(); static Pkt pk[400]; int n2 = demux(mono_ogg, sizeof mono_ogg, pk, 400); int16_t buf[VORBISPCM_MAX_FRAMES * 2];
        for (int i = 0; i < 3; i++) vorbispcm_packet(v, pk[i].p, pk[i].n, buf, VORBISPCM_MAX_FRAMES, &fr);
        uint8_t junk[40]; memset(junk, 0x5a, sizeof junk); junk[0] = 0x00;
        int rc = vorbispcm_packet(v, junk, sizeof junk, buf, VORBISPCM_MAX_FRAMES, &fr); CHECK(rc == AUDIO_OK || rc == AUDIO_BAD, "garbage audio packet does not crash (rc %d)", rc);
        int total = 0; vorbispcm_flush(v);
        for (int i = 3; i < n2; i++) { rc = vorbispcm_packet(v, pk[i].p, pk[i].n, buf, VORBISPCM_MAX_FRAMES, &fr); total += fr; }
        CHECK(total > 22050 * 0.4, "decoding resumes after a flush (%d frames)", total);
        vorbispcm_destroy(v);
    }
    if (opuspcm_available()) {
        static Pkt pk[400]; int n3 = demux(opus_ogg, sizeof opus_ogg, pk, 400); int c = 0, skip = 0;
        CHECK(n3 > 4 && opuspcm_parse_head(pk[0].p, pk[0].n, &c, &skip) == 0 && c == 1 && skip > 0, "OpusHead parsed (%d ch, pre-skip %d)", c, skip);
        OpusPcm *o = opuspcm_create(c, skip); int total = 0; static int16_t all[48000], buf[OPUSPCM_MAX_FRAMES * 2];
        for (int i = 2; i < n3; i++) { int fr; if (opuspcm_packet(o, pk[i].p, pk[i].n, buf, OPUSPCM_MAX_FRAMES, &fr) != AUDIO_OK) { CHECK(0, "opus packet %d", i); break; } if (total + fr < 48000) memcpy(all + total, buf, sizeof(int16_t) * fr); total += fr; }
        double a2; double h2 = tone_hz(all, total < 48000 ? total : 48000, 1, 48000, &a2);
        CHECK(total > 48000 * 0.45 && fabs(h2 - 440.0) < 6.0 && a2 > 0.05, "opus: %d frames, tone %.1f Hz, peak %.2f", total, h2, a2);
        { int64_t g = last_granule(opus_ogg, sizeof opus_ogg); CHECK(g > 0 && total + skip >= g && total + skip - g < 960 * 6, "opus: %d frames + pre-skip %d against granule %lld", total, skip, (long long)g); }
        opuspcm_flush(o); opuspcm_destroy(o);
        CHECK(opuspcm_create(3, 0) == NULL, "three channels not supported by this decoder object");
    } else printf("note: Opus not built here (no NAPPLE_HAVE_OPUS); the Opus checks run on the Mac build\n");
    printf("{\"test\":\"svc_movie_audio\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
