/* Register map of the host1x multimedia engines (word offsets inside the engine class). UNVERIFIED: written from memory of the public NVIDIA
 * class headers (nvdec/vic/nvjpg/nvenc *_drv.h: method byte offset / 4) and of the yuzu/nouveau sources, never checked against a real title's
 * command stream. Everything the engines read or write goes through the names here, so a trace from a real game (NAPPLE_NVDRV verbose data
 * dumps and MediaStats unknown_regs, see docs/architecture/runtime-services.md) fixes it in one place. Offsets of surfaces are iova >> 8. */
#pragma once

/* all engines */
#define NVM_SET_APPLICATION_ID 0x080 /* NVDEC: codec id (decoder_backend.h); VIC/NVJPG/MSENC: application id, ignored */
#define NVM_EXECUTE 0x0c0
/* THI (host1x client interface) method FIFO, word offsets 0x10 / 0x11 of the class: METHOD0 takes the method (the register's word offset in the table
 * below), METHOD1 the data. A real SDK stream (MK8D, 2026-10-27) writes every engine register this way, as INCR 0x10 count 2 pairs. */
#define NVM_THI_METHOD0 0x010
#define NVM_THI_METHOD1 0x011

/* NVDEC (class 0xF0) */
#define NVDEC_SET_CONTROL_PARAMS 0x100
#define NVDEC_SET_DRV_PIC_SETUP_OFFSET 0x101 /* picture-info structure */
#define NVDEC_SET_IN_BUF_BASE_OFFSET 0x102   /* coded frame */
#define NVDEC_SET_PICTURE_INDEX 0x103 /* a running frame number in MK8D's stream (0, 1, 2, ... past the 17 slots), not the output slot: H.264 takes curr_pic_idx below */
#define NVDEC_SET_SLICE_OFFSETS_BUF_OFFSET 0x104
#define NVDEC_SET_COLOC_DATA_OFFSET 0x105
#define NVDEC_SET_HISTORY_OFFSET 0x106
#define NVDEC_SET_DISPLAY_BUF_SIZE 0x107
#define NVDEC_SET_STATUS_OFFSET 0x109
#define NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET 0x130 /* not a real register: a fallback for streams that only set one output surface (tests); the SDK writes the slots below */
#define NVDEC_SET_DISPLAY_BUF_CHROMA_OFFSET 0x131
#define NVDEC_SET_PICTURE_LUMA_OFFSET0 0x10c   /* + picture index, 17 slots (MK8D's stream, 2026-10-27: all 17 written, luma 0x10c.., chroma 0x11d..) */
#define NVDEC_SET_PICTURE_CHROMA_OFFSET0 0x11d /* + picture index */
#define NVDEC_PIC_SLOTS 17
/* word index of the coded-frame size inside the picture-info structure, per codec (0 = unknown: the whole input surface is the frame) */
/* Words of the H.264 picture-info structure (nvdec_h264_pic_s, matched against MK8D's 512x608 CABAC stream: 0xa8fe = the 43262-byte slice at word 18, 32 x 38
 * macroblocks at words 25/26, pitches 0x200 at 35/36, chroma_format_idc 1, poc type 0, bit-field word 44 as below). */
#define NVDEC_H264_W_LOG2_MAX_POC_LSB_M4 22
#define NVDEC_H264_W_DELTA_POC_ZERO 23
#define NVDEC_H264_W_FRAME_MBS_ONLY 24
#define NVDEC_H264_W_WIDTH_MBS 25
#define NVDEC_H264_W_HEIGHT_MAP_UNITS 26
#define NVDEC_H264_W_TILE_FORMAT 27 /* bits 0-1: 0 = pitch linear */
#define NVDEC_H264_W_CABAC 28
#define NVDEC_H264_W_POC_PRESENT 29
#define NVDEC_H264_W_NUM_REF_L0 30
#define NVDEC_H264_W_NUM_REF_L1 31
#define NVDEC_H264_W_DEBLOCK_CTRL 32
#define NVDEC_H264_W_REDUNDANT_PIC_CNT 33
#define NVDEC_H264_W_TRANSFORM_8X8 34
#define NVDEC_H264_W_PITCH_LUMA 35
#define NVDEC_H264_W_PITCH_CHROMA 36
#define NVDEC_H264_W_FLAGS 44 /* b1 direct_8x8_inference, b2 weighted_pred, b3 constrained_intra, b8-11 log2_max_frame_num-4, b12-13 chroma_format_idc, b14-15 poc type, b16-21 pic_init_qp-26 (signed), b22-26 chroma_qp_index_offset (signed), b27-31 second */
#define NVDEC_H264_W_FLAGS2 45 /* b0-1 weighted_bipred_idc, b2-8 curr_pic_idx (the DPB slot this picture is decoded into), b9-13 curr_col_idx, b14-29 frame_number:
                                * the public emulator's layout of this 64-bit word shifted up by 8 (the shift b6 matched for the low fields); curr_pic_idx confirmed on
                                * MK8D's stream 2026-10-27 (it follows the slot registers the SDK rewrites) */
#define NVDEC_H264_CURR_PIC_IDX(w45) (((w45) >> 2) & 0x7f)
#define NVDEC_H264_PIC_WORDS 46
/* VP9 picture-info surface pitches (b8, PROPOSED: not matched against a real VP9 trace; the plausibility guard in nvdrv_media_core.c ignores a value that is below the
 * picture width, above 0x10000 or not a multiple of 16, so a wrong word index degrades to packed rows rather than corrupting the output). */
#define NVDEC_VP9_W_PITCH_LUMA 20
#define NVDEC_VP9_W_PITCH_CHROMA 21
#define NVDEC_PIC_STREAM_LEN_WORD_H264 18
#define NVDEC_PIC_STREAM_LEN_WORD_VP9 12
#define NVDEC_PIC_STREAM_LEN_WORD_H265 12
#define NVDEC_PIC_STREAM_LEN_WORD_VP8 12
/* status structure written after an execute (0x20 bytes): u32 mbs_correct, u32 mbs_in_error, u32 reserved, u32 error (0 = ok), u32 width, u32 height, ... */

/* VIC (class 0x5D) */
/* Methods as the public emulator sources name them (yuzu's vic.h): 0x1c1 SetControlParams, 0x1c2 SetConfigStructOffset, 0x1c8 / 0x1c9 output surface luma /
 * chroma; slot 0's input surface luma / chroma are the words MK8D writes at 0x100 / 0x101 (2026-10-27 trace). Addresses are device addresses >> 8. The b6 map
 * read 0x1c1 as the config address: MK8D's 0x610110 there is the control word (it resolves nowhere), the config address is the 0x1c2 write. */
#define VIC_SET_INPUT0_LUMA_OFFSET 0x100
#define VIC_SET_INPUT0_CHROMA_OFFSET 0x101
#define VIC_SET_CONTROL_PARAMS 0x1c1
#define VIC_SET_CONFIG_STRUCT_OFFSET 0x1c2
#define VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET 0x1c8
#define VIC_SET_OUTPUT_SURFACE_CHROMA_OFFSET 0x1c9
/* Config structure: only the output surface configuration (u64 at +0x20) is read. Bits 0-6 pixel format, 11-14 block kind (0 = pitch linear, else block linear),
 * 15-18 log2 block height in GOBs, 32-45 surface width - 1, 46-59 surface height - 1. Pixel formats: 0x1f RGBA8 (bytes R,G,B,A), 0x20 BGRA8, 0x23 RGBX8
 * (alpha 255), 0x44 NV12 (Y8 + interleaved U8V8, 4:2:0). Pitch-linear rows: width*4 for RGB, the width aligned to 256 for NV12 (as the emulator writes them).
 * Input: NV12 of the surface size, rows aligned to 256 (or packed when the input surface is too small for that). Colour: BT.601 limited range. */
#define VIC_CFG_OUTPUT_SURFACE 0x20
#define VIC_CFG_BYTES 0x28
enum { VIC_FMT_RGBA8 = 0x1f, VIC_FMT_BGRA8 = 0x20, VIC_FMT_RGBX8 = 0x23, VIC_FMT_NV12 = 0x44 };

/* NVJPG (class 0xC0) */
#define NVJPG_SET_DRV_PIC_SETUP_OFFSET 0x101
#define NVJPG_SET_IN_BUF_BASE_OFFSET 0x102
#define NVJPG_SET_OUT_LUMA_OFFSET 0x103 /* RGBA surface, or the luma plane of NV12 */
#define NVJPG_SET_OUT_CHROMA_OFFSET 0x104
#define NVJPG_SET_STATUS_OFFSET 0x109
/* Napple's reduced pic-setup structure: u32 in_size, out_cap (bytes of the luma/RGBA surface), out_w, out_h (0 = the JPEG's size), format (0 RGBA8 bytes R,G,B,A; 1 NV12), out_pitch (0 = tight).
 * Status written (0x10 bytes): u32 error (0 ok, else JPEGCORE_*), width, height, bytes written. */
enum { NVJPG_FMT_RGBA = 0, NVJPG_FMT_NV12 = 1 };
#define NVJPG_SETUP_WORDS 6
/* Encode (b8, Napple's extension of the reduced structure; the real nvjpg encode layout is not known): when the structure is NVJPG_SETUP_WORDS_ENC words long and
 * word 6 is NVJPG_MODE_ENCODE, the input surface (NVJPG_SET_IN_BUF_BASE_OFFSET) holds RGBA8 pixels and the JPEG is written to NVJPG_SET_OUT_LUMA_OFFSET (out_cap
 * bytes, word 1). Words: 6 mode (0 decode, 1 encode), 7 width, 8 height, 9 input pitch (0 = tight), 10 quality 1..100 (0 = 85), 11 4:2:0 flag (0 = 4:4:4).
 * Word 0 is the input size in bytes (0 = pitch * height). Status: error, width, height, JPEG bytes written. Only format 0 (RGBA) is accepted as input. */
#define NVJPG_SETUP_WORDS_ENC 12
enum { NVJPG_MODE_DECODE = 0, NVJPG_MODE_ENCODE = 1 };

/* MSENC (class 0x21), b8: H.264 encode of one NV12 frame per EXECUTE through the EncoderBackend (decoder_backend.h). The register layout is Napple's reduced
 * one (PROPOSED: the real NVENC method map is not known). Surface registers are iova >> 8. Without a readable setup structure an execute is still accepted and
 * counted and produces no bitstream (status: all zero), as before.
 * Setup structure words: 0 width, 1 height, 2 luma pitch (0 = width), 3 chroma pitch (0 = width), 4 bitrate (bit/s, 0 = default), 5 fps (0 = default),
 * 6 flags (bit0 force IDR), 7 output capacity in bytes (0 = what the mapping holds). Status (0x10 bytes): error (0 ok, MSENC_ERR_*), bytes written, flags
 * (bit0 IDR), frame count. */
#define MSENC_SET_SETUP_OFFSET 0x101
#define MSENC_SET_IN_LUMA_OFFSET 0x102
#define MSENC_SET_IN_CHROMA_OFFSET 0x103
#define MSENC_SET_OUT_BITSTREAM_OFFSET 0x104
#define MSENC_SET_STATUS_OFFSET 0x109
#define MSENC_SETUP_WORDS 8
enum { MSENC_ERR_ADDRESS = 2, MSENC_ERR_PARAMS = 4, MSENC_ERR_CODEC = 1, MSENC_ERR_FAILED = 3 };
