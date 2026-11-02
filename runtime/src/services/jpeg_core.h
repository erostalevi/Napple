/* jpeg core: portable baseline JPEG decoder and encoder (no libraries, no macOS headers). nvjpg (nvdrv_media) and the caps screenshot and thumbnail
 * paths use it so the same code runs on the Mac and under the Linux checks; the VideoToolbox/ImageIO helpers in vt_h264.c stay for movie.c.
 * Decoder: sequential Huffman, 8-bit, 1 (grey) or 3 (YCbCr) components, sampling factors up to 2x2 (4:4:4, 4:2:2, 4:4:0, 4:2:0), restart
 * intervals, any marker order a JFIF/Exif file has. Progressive, arithmetic, lossless and 12-bit streams are refused (JPEGCORE_UNSUPPORTED).
 * Encoder: baseline, Annex K tables scaled by quality, 4:2:0 or 4:4:4, JFIF header; and napjpeg_encode_scan, the entropy-coded segment alone from planar or
 * semi-planar YCbCr with caller-given tables (what the NVJPG encoder produces: the driver writes the headers). */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { JPEGCORE_OK = 0, JPEGCORE_BAD = 1, JPEGCORE_UNSUPPORTED = 2, JPEGCORE_NOMEM = 3, JPEGCORE_TRUNCATED = 4, JPEGCORE_TOO_SMALL = 5 };
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
/* What caps:dc DecodeJpeg does: decodes `data` and writes width x height RGBA8 (tight rows) to `out`, box-scaled when the file has another size
 * (width/height 0 = the file's own size, which then is reported back). JPEGCORE_TOO_SMALL when cap < width*height*4; nothing is written on an error. */
int napjpeg_decode_into(const uint8_t *data, size_t size, int *width, int *height, uint8_t *out, size_t cap);

/* ---- scan-only encode (NVJPG encoder contract) ----
 * One Huffman table as the encoder uses it: code and length per symbol (length 0 = the symbol has no code; meeting one fails the encode). */
typedef struct {
    uint16_t code[256];
    uint8_t len[256];
} JpegHuffCode;
enum { JPEGSCAN_444 = 0, JPEGSCAN_422H = 1, JPEGSCAN_422V = 2, JPEGSCAN_420 = 3 }; /* chroma formats, nvjpg ENC_TOP_CTL numbering */
typedef struct {
    int width, height, chroma;      /* JPEGSCAN_*; MCU 8x8 (444), 16x8 (422H), 8x16 (422V), 16x16 (420) */
    const uint8_t *y, *u, *v;       /* planes; with uv_interleaved u points at CbCr byte pairs and v is unused */
    int y_stride, uv_stride, uv_interleaved;
    uint16_t qt[2][64];             /* luma / chroma quantisers, raster order, 1..255 */
    JpegHuffCode dc[2], ac[2];      /* luma / chroma */
    int restart_interval;           /* MCUs between RSTn markers, 0 = none */
} JpegScanParams;
/* Encodes the planes as one interleaved baseline scan (Y blocks, then Cb, then Cr per MCU), byte-stuffed, padded with 1 bits, RSTn markers every restart
 * interval; no SOI/headers/EOI. malloc'd result (free()) or NULL (bad parameters, a symbol without a code, out of memory). */
uint8_t *napjpeg_encode_scan(const JpegScanParams *p, size_t *out_size);
/* The Annex K (typical) Huffman tables as JpegHuffCode: index 0 luma, 1 chroma. Tests and callers that want the standard tables. */
void napjpeg_std_huff(JpegHuffCode dc[2], JpegHuffCode ac[2]);
/* DHT payload of a JpegHuffCode built from canonical code lengths (what napjpeg_std_huff makes): bits[1..16] counts and the symbols in code order. Returns the
 * number of symbols, or -1 when the codes are not canonical. */
int napjpeg_huff_to_dht(const JpegHuffCode *h, uint8_t bits[17], uint8_t vals[256]);
