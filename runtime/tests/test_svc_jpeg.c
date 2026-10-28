/* Unit tests: jpeg core (baseline decoder/encoder). Synthetic images only: encoded by the core's own encoder and by ffmpeg (embedded vectors). */
#include "../src/services/jpeg_core.h"
#include "check.h"
#include <math.h>

static void fill(uint8_t *p, int w, int h, int kind) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *o = p + ((size_t)y * w + x) * 4;
            if (kind == 0) { o[0] = 200; o[1] = 60; o[2] = 30; }                       /* flat */
            else { o[0] = (uint8_t)(x * 255 / (w - 1)); o[1] = (uint8_t)(y * 255 / (h - 1)); o[2] = (uint8_t)((x + y) * 255 / (w + h - 2)); } /* smooth gradient */
            o[3] = 255;
        }
}
static double psnr(const uint8_t *a, const uint8_t *b, size_t n4) {
    double se = 0; size_t k = 0;
    for (size_t i = 0; i < n4; i++) if (i % 4 != 3) { double d = (double)a[i] - b[i]; se += d * d; k++; }
    return se == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / (se / (double)k));
}
#include "jpeg_vectors.h"

int main(void) {
    for (int kind = 0; kind < 2; kind++)
        for (int sub = 0; sub < 2; sub++) {
            int w = 37, h = 29; /* not a multiple of the MCU */
            uint8_t *src = malloc((size_t)w * h * 4); fill(src, w, h, kind);
            size_t n = 0; uint8_t *j = napjpeg_encode_rgba(src, w, h, w * 4, 90, sub, &n);
            CHECK(j && n > 100 && j[0] == 0xff && j[1] == 0xd8 && j[n - 2] == 0xff && j[n - 1] == 0xd9, "encode kind %d sub %d: SOI/EOI framed", kind, sub);
            JpegInfo info; CHECK(napjpeg_info(j, n, &info) == JPEGCORE_OK && info.width == w && info.height == h && info.ncomp == 3 && info.h[0] == (sub ? 2 : 1), "info");
            int dw = 0, dh = 0, err = -1; uint8_t *d = napjpeg_decode_rgba(j, n, &dw, &dh, &err);
            CHECK(d && err == 0 && dw == w && dh == h, "decode kind %d sub %d (err %d)", kind, sub, err);
            if (d) { double p = psnr(src, d, (size_t)w * h * 4); CHECK(p > (kind ? 30.0 : 28.0), "round trip kind %d sub %d: PSNR %.1f dB", kind, sub, p); }
            free(d); free(j); free(src);
        }
    { /* large and exactly-MCU-aligned: 1280x720 4:2:0 gradient, as the caps album stores it */
        int w = 1280, h = 720; uint8_t *src = malloc((size_t)w * h * 4); fill(src, w, h, 1);
        size_t n; uint8_t *j = napjpeg_encode_rgba(src, w, h, w * 4, 85, 1, &n); int dw, dh, err; uint8_t *d = napjpeg_decode_rgba(j, n, &dw, &dh, &err);
        CHECK(d && dw == w && dh == h && psnr(src, d, (size_t)w * h * 4) > 30.0, "1280x720 round trip");
        free(d); free(j); free(src);
    }
    { /* libjpeg vectors: the decoder reads another encoder's output (all samplings, restart intervals, grey); progressive is refused */
        for (unsigned i = 0; i < sizeof vec / sizeof *vec; i++) {
            int dw, dh, err; uint8_t *d = napjpeg_decode_rgba(vec[i].data, vec[i].size, &dw, &dh, &err);
            if (vec[i].kind == -2) { CHECK(!d && err == JPEGCORE_UNSUPPORTED, "vector %s: progressive refused", vec[i].name); continue; }
            CHECK(d && err == 0 && dw == vec[i].w && dh == vec[i].h, "vector %s decodes (err %d)", vec[i].name, err);
            if (!d) continue;
            if (vec[i].kind == -1) { CHECK(d[0] == d[1] && d[1] == d[2] && d[(size_t)(dw * dh - 1) * 4] > d[0], "vector %s: grey ramp", vec[i].name); }
            else { uint8_t *ref = malloc((size_t)dw * dh * 4); fill(ref, dw, dh, vec[i].kind); double p = psnr(ref, d, (size_t)dw * dh * 4); CHECK(p > 27.0, "vector %s: PSNR %.1f dB", vec[i].name, p); free(ref); }
            free(d);
        }
    }
    { /* grey image */
        uint8_t px[8 * 8 * 4]; for (int i = 0; i < 64; i++) { px[4*i] = px[4*i+1] = px[4*i+2] = (uint8_t)(i * 4); px[4*i+3] = 255; }
        size_t n; uint8_t *j = napjpeg_encode_rgba(px, 8, 8, 32, 95, 0, &n); int w, h, err; uint8_t *d = napjpeg_decode_rgba(j, n, &w, &h, &err);
        CHECK(d && w == 8 && h == 8, "tiny image"); free(d); free(j);
    }
    { /* refusals */
        int w, h, err; uint8_t junk[16] = {1, 2, 3};
        CHECK(napjpeg_decode_rgba(junk, sizeof junk, &w, &h, &err) == NULL && err == JPEGCORE_BAD, "not a JPEG");
        CHECK(napjpeg_decode_rgba(NULL, 0, &w, &h, &err) == NULL && err != 0, "empty input");
        size_t n; uint8_t px[4 * 4 * 4] = {0}; uint8_t *j = napjpeg_encode_rgba(px, 4, 4, 16, 50, 0, &n);
        CHECK(napjpeg_decode_rgba(j, 20, &w, &h, &err) == NULL && err == JPEGCORE_TRUNCATED, "truncated in the headers");
        uint8_t *p = malloc(n); memcpy(p, j, n);
        for (size_t i = 0; i + 3 < n; i++) if (p[i] == 0xff && p[i + 1] == 0xc0) p[i + 1] = 0xc2; /* progressive */
        CHECK(napjpeg_decode_rgba(p, n, &w, &h, &err) == NULL && err == JPEGCORE_UNSUPPORTED, "progressive refused");
        JpegInfo info; CHECK(napjpeg_info(p, n, &info) == JPEGCORE_UNSUPPORTED && info.progressive, "progressive reported by info");
        free(p); free(j);
        uint8_t *k = napjpeg_encode_rgba(px, 4, 4, 16, 50, 0, &n); k[n / 2] ^= 0x55; k[n / 2 + 1] ^= 0xaa;  /* corrupt entropy data: must not crash */
        uint8_t *d = napjpeg_decode_rgba(k, n, &w, &h, &err); free(d); free(k);
        CHECK(1, "corrupt scan data returns without crashing");
    }
    { uint8_t px[4 * 4 * 4]; for (int i = 0; i < 16; i++) { px[4*i] = (uint8_t)(i < 8 ? 0 : 200); px[4*i+1] = 0; px[4*i+2] = 0; px[4*i+3] = 255; }
      uint8_t *s = napjpeg_scale_rgba(px, 4, 4, 2, 2); CHECK(s && s[0] == 0 && s[8] == 200 && s[3] == 255, "box downscale averages areas"); free(s); }
    { /* napjpeg_decode_into (caps:dc DecodeJpeg): own size, scaled size, short buffer, garbage */
        uint8_t px[32 * 16 * 4]; for (int i = 0; i < 32 * 16; i++) { px[4*i] = (uint8_t)(i % 32 * 8); px[4*i+1] = 60; px[4*i+2] = 30; px[4*i+3] = 255; }
        size_t n; uint8_t *j = napjpeg_encode_rgba(px, 32, 16, 128, 92, 0, &n); static uint8_t out[32 * 16 * 4];
        int w = 32, h = 16; CHECK(napjpeg_decode_into(j, n, &w, &h, out, sizeof out) == JPEGCORE_OK && w == 32 && h == 16 && psnr(px, out, sizeof px) > 30, "decode_into at the file size");
        w = 0; h = 0; CHECK(napjpeg_decode_into(j, n, &w, &h, out, sizeof out) == JPEGCORE_OK && w == 32 && h == 16, "decode_into reports the file size for 0x0");
        w = 16; h = 8; memset(out, 0, sizeof out);
        CHECK(napjpeg_decode_into(j, n, &w, &h, out, 16 * 8 * 4) == JPEGCORE_OK && out[15 * 4] > out[0] && out[3] == 255 && out[16 * 8 * 4] == 0, "decode_into box-scales to 16x8, writes no more");
        w = 32; h = 16; memset(out, 0xab, sizeof out);
        CHECK(napjpeg_decode_into(j, n, &w, &h, out, sizeof out - 1) == JPEGCORE_TOO_SMALL && out[0] == 0xab, "decode_into: buffer one byte short, nothing written");
        uint8_t junk[8] = {0xff, 0xd8, 1}; w = 4; h = 4; CHECK(napjpeg_decode_into(junk, sizeof junk, &w, &h, out, sizeof out) != JPEGCORE_OK, "decode_into: garbage refused");
        free(j);
    }
    printf("{\"test\":\"svc_jpeg\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
