/* jpeg core: portable baseline JPEG decoder and encoder (no libraries, no macOS headers). nvjpg (nvdrv_media) and the caps screenshot and thumbnail
 * paths use it so the same code runs on the Mac and under the Linux checks; the VideoToolbox/ImageIO helpers in vt_h264.c stay for movie.c.
 * Decoder: sequential Huffman, 8-bit, 1 (grey) or 3 (YCbCr) components, sampling factors up to 2x2 (4:4:4, 4:2:2, 4:4:0, 4:2:0), restart
 * intervals, any marker order a JFIF/Exif file has. Progressive, arithmetic, lossless and 12-bit streams are refused (JPEGCORE_UNSUPPORTED).
 * Encoder: baseline, Annex K tables scaled by quality, 4:2:0 or 4:4:4, JFIF header. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { JPEGCORE_OK = 0, JPEGCORE_BAD = 1, JPEGCORE_UNSUPPORTED = 2, JPEGCORE_NOMEM = 3, JPEGCORE_TRUNCATED = 4 };
#define JPEGCORE_MAX_DIM 16384

typedef struct {
    int width, height, ncomp, progressive;
    int h[3], v[3]; /* sampling factors of each component */
} JpegInfo;

/* Reads the frame header. Returns JPEGCORE_OK or why the stream is not decodable. */
int napjpeg_info(const uint8_t *data, size_t size, JpegInfo *info);
/* Decodes to tightly packed RGBA8 (alpha 255). Returns malloc'd pixels (free()) or NULL with *err set. */
uint8_t *napjpeg_decode_rgba(const uint8_t *data, size_t size, int *width, int *height, int *err);
/* Encodes RGBA8 (row stride in bytes) as a baseline JPEG. quality 1..100; subsample420 != 0 for 4:2:0, else 4:4:4. malloc'd result or NULL. */
uint8_t *napjpeg_encode_rgba(const uint8_t *rgba, int width, int height, int stride, int quality, int subsample420, size_t *out_size);
/* Box-filter downscale of RGBA8 into a malloc'd dw x dh image (nearest area average). */
uint8_t *napjpeg_scale_rgba(const uint8_t *rgba, int w, int h, int dw, int dh);
