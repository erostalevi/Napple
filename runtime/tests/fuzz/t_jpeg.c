/* Fuzz target: the portable JPEG codec (services/jpeg_core.c), reached through nvjpg and the caps screenshot paths with guest-supplied bytes.
 * Input starting with 0xFF is a JPEG stream (info + decode, and the decoded picture is re-encoded and scaled); anything else is an encoder request:
 * [mode][width-1][height-1][quality][flags] then pixel bytes. */
#include "../../src/services/jpeg_core.h"
#include "../jpeg_vectors.h"
#include "fuzz.h"
#include <stdlib.h>
#include <string.h>

int fuzz_jpeg(const uint8_t *d, size_t n) {
    JpegInfo info;
    if (n && d[0] == 0xFF) {
        int rc = napjpeg_info(d, n, &info);
        int w = 0, h = 0, err = 0;
        uint8_t *px = napjpeg_decode_rgba(d, n, &w, &h, &err);
        if (px) {
            if (rc == JPEGCORE_OK && (info.width != w || info.height != h)) __builtin_trap(); /* info and decode must agree */
            if (w <= 256 && h <= 256) { /* the encoder/scaler on a plausible picture only: they are not what reads the guest's bytes */
                size_t m;
                uint8_t *j = napjpeg_encode_rgba(px, w, h, w * 4, 50, 1, &m);
                if (j) {
                    int w2, h2;
                    free(napjpeg_decode_rgba(j, m, &w2, &h2, &err));
                    free(j);
                }
                free(napjpeg_scale_rgba(px, w, h, (w + 1) / 2, (h + 1) / 2));
            }
            free(px);
        }
        return 0;
    }
    FzIn r = {d, n, 1};
    int w = 1 + fz_u8(&r) % 96, h = 1 + fz_u8(&r) % 96, q = fz_u8(&r) % 104, sub = fz_u8(&r) & 1;
    size_t need = (size_t)w * h * 4;
    uint8_t *rgba = calloc(need, 1);
    for (size_t i = 0; i < need; i++) rgba[i] = fz_u8(&r);
    size_t m;
    uint8_t *j = napjpeg_encode_rgba(rgba, w, h, w * 4, q, sub, &m);
    if (j) {
        int w2, h2, err;
        uint8_t *back = napjpeg_decode_rgba(j, m, &w2, &h2, &err);
        if (!back || w2 != w || h2 != h) __builtin_trap(); /* an encoded picture decodes to the same size */
        free(back);
        free(j);
    }
    free(napjpeg_scale_rgba(rgba, w, h, 1 + q % 40, 1 + sub * 7 + q % 9));
    free(rgba);
    return 0;
}

void fuzz_jpeg_seeds(FuzzSeedFn add, void *ctx) {
    for (unsigned i = 0; i < sizeof vec / sizeof *vec; i++) add(ctx, vec[i].data, vec[i].size);
    uint8_t enc[5 + 24 * 16 * 4] = {0, 23, 15, 80, 1};
    for (size_t i = 5; i < sizeof enc; i++) enc[i] = (uint8_t)(i * 7 ^ (i >> 4));
    add(ctx, enc, sizeof enc);
}
FUZZ_TARGET(jpeg);
