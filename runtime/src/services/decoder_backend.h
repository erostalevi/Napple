/* DecoderBackend: where the nvdrv multimedia engines (nvdrv_media_core) send decode work. FROZEN CONTRACT, version DECODER_BACKEND_ABI = 1.
 *
 * Roles. The engine parses the host1x command stream and the engine registers, resolves the guest surfaces and hands the backend one frame at a time:
 * bitstream in, NV12 out. A backend is installed once (decoder_backend_install) before the first channel opens; the default is the null/test backend
 * (decoder_null_core.c), which reads the frame size from the codec headers and writes a deterministic picture. The Mac backend (VideoToolbox for H.264
 * through vt_h264.[ch], libvpx or VideoToolbox VP9 for VP9) is written later on the Mac: it fills in a DecoderBackend, sets .abi, and calls
 * decoder_backend_install at startup. Nothing in this header or in codec_headers_core.h needs changing for it.
 *
 * Call sequence for one NVDEC channel (all calls on the thread that submits, under nvdrv's lock; a backend needs no locking of its own for one ctx):
 *   open(codec)                      first EXECUTE, or when SET_APPLICATION_ID names another codec (the old ctx is closed first)
 *   configure(ctx, cfg)              before a decode whose stream carries new parameter sets: H.264 SPS+PPS NAL units found in the bitstream (a
 *                                    changed set re-triggers it), VP9 key frames whose size/profile/colour config changed. reset(ctx) comes first.
 *   decode(ctx, req, out)            once per EXECUTE
 *   close(ctx)                       channel closed or codec changed
 * A backend with configure == NULL gets the raw stream and works the parameter sets out itself (the null backend does).
 *
 * decode() rules:
 *   - Synchronous and in decode order: the frame produced by THIS request is written to req->luma / req->chroma before it returns (NVDEC hands the guest
 *     one output slot per request and the guest does the display reordering itself). A VideoToolbox backend therefore decodes with temporal processing
 *     off, or waits for the frame of this very access unit. Never write after returning, never keep the pointers (they point into guest memory).
 *   - Output is pitch-linear NV12 (luma rows of out->pitch bytes, chroma = interleaved CbCr, height/2 rows of out->pitch bytes); the writer checks
 *     luma_cap >= pitch*height and chroma_cap >= pitch*((height+1)/2) itself and returns -1 when the guest surface is too small. pitch >= width.
 *   - Return 0: a frame was produced and out is valid. >0: no frame yet (stream starts mid-GOP, parameter sets still missing): the engine reports
 *     "no picture" in the status structure and counts nothing. <0: error; the engine counts it and reports it in the status structure.
 *   - H.264 bitstream: Annex B slice NAL units (and possibly SPS/PPS/SEI in-band). VP9/VP8: one frame (superframe index, if any, left in).
 *     req->pic is the codec's picture-info structure as the guest wrote it (NVDEC layout, not needed by the VideoToolbox backend); may be NULL.
 *   - open() returns NULL for a codec the backend does not take; the engine then reports "codec not supported".
 * reset() drops reference frames and held output (new sequence); configure() and reset() are optional (NULL allowed), open/close/decode are required.
 * Errors are never fatal to the runtime: a backend may not abort(), and must tolerate being fed garbage.
 */
#pragma once
#include "codec_headers_core.h"
#include <stddef.h>
#include <stdint.h>

#define DECODER_BACKEND_ABI 1

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

/* Parameter sets handed to configure(). Pointers inside are valid for the duration of the call only; copy what you keep (CMVideoFormatDescription and
 * vpcC consumers copy anyway). width/height are the coded picture size after cropping (0 when unknown). */
typedef struct {
    int codec;
    int width, height;
    H264ParamSets h264;        /* DEC_CODEC_H264: SPS/PPS NAL units, header byte first, as CMVideoFormatDescriptionCreateFromH264ParameterSets wants them */
    uint8_t h264_annexb[1024]; /* the same sets in Annex B form, ready for vth264_config(); h264_annexb_size is 0 if they did not fit */
    size_t h264_annexb_size;
    Vp9Header vp9;               /* DEC_CODEC_VP9: the key frame's header */
    uint8_t vpcc[VP9_VPCC_SIZE]; /* and its vpcC record, vpcc_size is 0 when it could not be built */
    size_t vpcc_size;
} DecoderConfig;

typedef struct DecoderBackend {
    const char *name;
    void *(*open)(int codec); /* per-engine decoder state, NULL when the codec is not supported */
    void (*close)(void *ctx);
    int (*decode)(void *ctx, const DecodeRequest *req, DecodeResult *out); /* 0 = a frame was produced, <0 = error, >0 = no frame yet */
    void (*reset)(void *ctx);                                              /* optional */
    int (*configure)(void *ctx, const DecoderConfig *cfg); /* optional; 0 = accepted, <0 = refused (the engine reports an error and skips the decode) */
    uint32_t abi;                                          /* must be DECODER_BACKEND_ABI */
} DecoderBackend;

extern const DecoderBackend decoder_backend_null;
const DecoderBackend *decoder_backend_current(void);
/* Installs a backend for engines created afterwards (existing ones keep theirs). NULL restores the null backend. Returns 0, or -1 and installs nothing when
 * the abi differs or open/close/decode is missing. */
int decoder_backend_install(const DecoderBackend *b);
