/* H.264 decode on the host with VideoToolbox, and JPEG decode/encode with ImageIO. Used by movie.c (movie::Decoder, video/avc) and by the
   nvjpg / caps paths. No guest or runtime dependencies: tests/test_media.c links these two files alone. */
#ifndef NAPPLE_VT_H264_H
#define NAPPLE_VT_H264_H
#include <stddef.h>
#include <stdint.h>

typedef struct VtH264 VtH264;
/* One decoded frame, packed NV12 (Y rows, then rows of interleaved CbCr; stride = width), in presentation order. Valid during the call only. */
typedef void (*VtH264Frame)(void *user, const uint8_t *nv12, int width, int height, int64_t pts, int full_range, int bt601);

VtH264 *vth264_create(void);
void vth264_destroy(VtH264 *d);
/* Feed one access unit: Annex B (start codes) or AVCC (4-byte lengths, after vth264_set_avcc or when SPS/PPS came in-band). SPS/PPS NAL units
 * (in the data, or from csd-0/csd-1 via vth264_config) create the session; frames before that are dropped (returns 0 with no output).
 * Returns the number of frames delivered through `cb` or -1 on a decode error. */
int vth264_decode(VtH264 *d, const uint8_t *data, size_t size, int64_t pts, VtH264Frame cb, void *user);
/* Codec config: an Annex B blob holding SPS and PPS (csd-0 and csd-1 concatenated). */
int vth264_config(VtH264 *d, const uint8_t *annexb, size_t size);
/* Deliver every frame still held for reordering (end of stream, flush). */
int vth264_flush(VtH264 *d, VtH264Frame cb, void *user);
/* Drop held frames and the session (seek / reset); SPS/PPS are kept so decoding resumes at the next IDR. */
void vth264_reset(VtH264 *d);
int vth264_held(const VtH264 *d);
int vth264_width(const VtH264 *d);
int vth264_height(const VtH264 *d);

/* JPEG (any ImageIO still image) -> tightly packed RGBA8. Returns malloc'd pixels (free()) or NULL. */
uint8_t *jpeg_decode_rgba(const void *data, size_t size, int *width, int *height);
/* RGBA8 -> JPEG at `quality` (0..1). Returns malloc'd bytes (free()) or NULL. */
uint8_t *jpeg_encode_rgba(const uint8_t *rgba, int width, int height, int stride, float quality, size_t *out_size);
#endif
