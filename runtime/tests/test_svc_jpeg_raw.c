/* Unit tests: header-less JPEG decode (napjpeg_decode_raw_rgba), the core of the NVJPG engine's raw path. Every image is encoded by the core's own encoder or taken
 * from the libjpeg vectors, stripped of its markers (jpeg_strip.h) and decoded again from the programmed tables + entropy data: the pixels must equal the
 * headered decode exactly (the same MCU decoder runs in both). */
#include "../src/services/jpeg_core.h"
#include "check.h"
#include "jpeg_strip.h"
#include "jpeg_vectors.h"
#include <stdlib.h>

static void fill(uint8_t *p, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *o = p + ((size_t)y * w + x) * 4;
            o[0] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1) ^ (y & 8 ? 0x20 : 0)); o[1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1)); o[2] = (uint8_t)((x * 7 + y * 3) & 255); o[3] = 255;
        }
}
/* decode both ways and compare; returns 1 on an exact match */
static int same(const char *what, const uint8_t *jpg, size_t n, int w, int h, int ncomp) {
    int hw, hh, he = -1, rw, rh, re = -1;
    uint8_t *a = napjpeg_decode_rgba(jpg, n, &hw, &hh, &he);
    JpegRawParams rp; const uint8_t *scan; size_t sn;
    int ok = strip_jpeg(jpg, n, &rp, &scan, &sn) == 0;
    CHECK(ok && a && he == 0, "%s: stripped and headered decode available", what);
    if (!ok || !a) { free(a); return 0; }
    uint8_t *b = napjpeg_decode_raw_rgba(&rp, scan, sn, &rw, &rh, &re);
    int eq = b && re == 0 && rw == w && rh == h && hw == w && hh == h && !memcmp(a, b, (size_t)w * h * 4) && rp.ncomp == ncomp;
    CHECK(eq, "%s: header-less decode is pixel-exact (%dx%d, err %d)", what, rw, rh, re);
    free(a); free(b);
    return eq;
}
int main(void) {
    static const struct { int w, h, sub; } sizes[] = {{8, 8, 0}, {16, 16, 1}, {37, 29, 0}, {37, 29, 1}, {1, 1, 1}, {17, 3, 1}, {3, 41, 0}, {64, 48, 1}, {121, 67, 1}};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        int w = sizes[i].w, h = sizes[i].h; uint8_t *src = malloc((size_t)w * h * 4); fill(src, w, h);
        for (int quality = 40; quality <= 95; quality += 55) {
            size_t n; uint8_t *j = napjpeg_encode_rgba(src, w, h, w * 4, quality, sizes[i].sub, &n); char what[64];
            snprintf(what, sizeof what, "encoder %dx%d %s q%d", w, h, sizes[i].sub ? "4:2:0" : "4:4:4", quality);
            same(what, j, n, w, h, 3); free(j);
        }
        free(src);
    }
    for (unsigned i = 0; i < sizeof vec / sizeof *vec; i++) {
        if (vec[i].kind == -2) continue; /* progressive: not an NVJPG stream */
        same(vec[i].name, vec[i].data, vec[i].size, vec[i].w, vec[i].h, vec[i].kind == -1 ? 1 : 3);
    }
    { /* the restart vector really has a restart interval and RSTn markers in its entropy data */
        JpegRawParams rp; const uint8_t *scan; size_t sn; int rst = 0;
        CHECK(strip_jpeg(pilrst_data, sizeof pilrst_data, &rp, &scan, &sn) == 0 && rp.restart_interval > 0, "pilrst: restart interval %d", rp.restart_interval);
        for (size_t i = 0; i + 1 < sn; i++) rst += scan[i] == 0xff && scan[i + 1] >= 0xd0 && scan[i + 1] <= 0xd7;
        CHECK(rst > 0, "pilrst: %d RSTn markers stay in the entropy segment", rst);
    }
    { /* refusals: parameters no JPEG could carry */
        size_t n; uint8_t px[16 * 16 * 4]; fill(px, 16, 16); uint8_t *j = napjpeg_encode_rgba(px, 16, 16, 64, 80, 1, &n);
        JpegRawParams rp, bad; const uint8_t *scan; size_t sn; int w, h, err;
        CHECK(strip_jpeg(j, n, &rp, &scan, &sn) == 0, "strip");
        bad = rp; bad.width = 0; CHECK(!napjpeg_decode_raw_rgba(&bad, scan, sn, &w, &h, &err) && err == JPEGCORE_BAD, "zero width refused");
        bad = rp; bad.height = JPEGCORE_MAX_DIM + 1; CHECK(!napjpeg_decode_raw_rgba(&bad, scan, sn, &w, &h, &err) && err == JPEGCORE_BAD, "oversize refused");
        bad = rp; bad.ncomp = 2; CHECK(!napjpeg_decode_raw_rgba(&bad, scan, sn, &w, &h, &err) && err == JPEGCORE_BAD, "two components refused");
        bad = rp; bad.h[0] = 3; CHECK(!napjpeg_decode_raw_rgba(&bad, scan, sn, &w, &h, &err) && err == JPEGCORE_BAD, "sampling factor 3 refused");
        bad = rp; bad.tq[1] = 4; CHECK(!napjpeg_decode_raw_rgba(&bad, scan, sn, &w, &h, &err) && err == JPEGCORE_BAD, "quant selector 4 refused");
        bad = rp; bad.dc_t[0] = 7; CHECK(!napjpeg_decode_raw_rgba(&bad, scan, sn, &w, &h, &err) && err == JPEGCORE_BAD, "huffman selector 7 refused");
        bad = rp; bad.restart_interval = 70000; CHECK(!napjpeg_decode_raw_rgba(&bad, scan, sn, &w, &h, &err) && err == JPEGCORE_BAD, "restart interval beyond 16 bits refused");
        bad = rp; memset(&bad.huff[1][0], 0, sizeof bad.huff[1][0]); CHECK(!napjpeg_decode_raw_rgba(&bad, scan, sn, &w, &h, &err) && err == JPEGCORE_BAD, "absent AC table refused");
        bad = rp; memset(bad.huff[0][0].bits + 1, 0, 16); bad.huff[0][0].bits[1] = 3; CHECK(!napjpeg_decode_raw_rgba(&bad, scan, sn, &w, &h, &err) && err == JPEGCORE_BAD, "over-subscribed code refused");
        CHECK(!napjpeg_decode_raw_rgba(NULL, scan, sn, &w, &h, &err) && err != 0, "NULL parameters refused");
        CHECK(!napjpeg_decode_raw_rgba(&rp, NULL, 0, &w, &h, &err) && err == JPEGCORE_TRUNCATED, "no entropy data: truncated");
        uint8_t *d = napjpeg_decode_raw_rgba(&rp, scan, sn / 2, &w, &h, &err);
        CHECK((d && w == 16 && h == 16) || (!d && err != 0), "half the entropy data: an image (rest grey, like the headered path) or a clean error (err %d)", err); free(d);
        uint8_t *cp = malloc(sn); memcpy(cp, scan, sn); for (size_t i = 0; i < sn; i += 3) cp[i] ^= 0xa5;
        d = napjpeg_decode_raw_rgba(&rp, cp, sn, &w, &h, &err); free(d); free(cp); CHECK(1, "corrupt entropy data returns without crashing");
        free(j);
    }
    printf("{\"test\":\"svc_jpeg_raw\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
