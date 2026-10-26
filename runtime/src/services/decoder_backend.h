/* DecoderBackend: where the nvdrv multimedia engines (nvdrv_media_core) send decode work. The engine parses the host1x command stream and the
 * engine registers, resolves the guest surfaces and hands the backend one frame at a time: bitstream in, NV12 out. A backend is installed once
 * (decoder_backend_install) before the first channel opens; the default is the null/test backend (decoder_null_core.c), which reads the frame size
 * from the codec headers and writes a deterministic picture. The Mac VideoToolbox backend (vth264_* in vt_h264.c for H.264, libvpx for VP9) is wired
 * on the Mac side: it implements this struct and calls decoder_backend_install at startup (see integrators/ file). Output is pitch-linear NV12. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* NVDEC codec ids as written to the application-id register (unverified, from the public driver sources). */
enum { DEC_CODEC_NONE = 0, DEC_CODEC_H264 = 3, DEC_CODEC_VP8 = 5, DEC_CODEC_H265 = 7, DEC_CODEC_VP9 = 9 };

typedef struct {
    int codec;
    const uint8_t *pic; /* the codec's picture-info structure (what the guest wrote for the engine), or NULL */
    size_t pic_size;
    const uint8_t *bitstream; /* the coded frame: slice NAL units (H.264/H.265), a VP8/VP9 frame */
    size_t bitstream_size;
    uint32_t picture_index; /* output slot the guest selected */
    uint8_t *luma, *chroma; /* output planes (host pointers into the guest surfaces) */
    size_t luma_cap, chroma_cap;
} DecodeRequest;
typedef struct {
    int width, height, pitch; /* what was written: luma rows of `pitch` bytes, chroma (interleaved CbCr) half as many rows */
    int bt709, full_range;
} DecodeResult;

typedef struct DecoderBackend {
    const char *name;
    void *(*open)(int codec); /* per-engine decoder state, NULL when the codec is not supported */
    void (*close)(void *ctx);
    int (*decode)(void *ctx, const DecodeRequest *req, DecodeResult *out); /* 0 = a frame was produced, <0 = error, >0 = no frame yet */
    void (*reset)(void *ctx);
} DecoderBackend;

extern const DecoderBackend decoder_backend_null;
const DecoderBackend *decoder_backend_current(void);
void decoder_backend_install(const DecoderBackend *b); /* NULL restores the null backend */
