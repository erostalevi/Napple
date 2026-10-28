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
#define NVDEC_SET_PICTURE_INDEX 0x103
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
#define NVDEC_H264_W_FLAGS2 45 /* b0-1 weighted_bipred_idc */
#define NVDEC_H264_PIC_WORDS 46
#define NVDEC_PIC_STREAM_LEN_WORD_H264 18
#define NVDEC_PIC_STREAM_LEN_WORD_VP9 12
#define NVDEC_PIC_STREAM_LEN_WORD_H265 12
#define NVDEC_PIC_STREAM_LEN_WORD_VP8 12
/* status structure written after an execute (0x20 bytes): u32 mbs_correct, u32 mbs_in_error, u32 reserved, u32 error (0 = ok), u32 width, u32 height, ... */

/* VIC (class 0x5D) */
/* Register numbers as MK8D's stream writes them (2026-10-27): slot 0 input luma/chroma at 0x100/0x101, config structure 0x1c1, output luma/chroma 0x1c8/0x1c9.
 * Its config structure address (0x610110 << 8) lies outside every pinned handle (see nvdrv.c nv_resolve), so the real layout is not read: with no readable
 * config the execute copies the NV12 input to the NV12 output surfaces (MK8D's output surfaces are NV12-sized). */
#define VIC_SET_INPUT0_LUMA_OFFSET 0x100
#define VIC_SET_INPUT0_CHROMA_OFFSET 0x101
#define VIC_SET_CONFIG_STRUCT_OFFSET 0x1c1
#define VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET 0x1c8
#define VIC_SET_OUTPUT_SURFACE_CHROMA_OFFSET 0x1c9
/* Napple's reduced configuration structure (the real one is larger; only these words are read): u32 src_w, src_h, dst_w, dst_h, pixel_format
 * (0 A8B8G8R8 = bytes R,G,B,A; 1 X8B8G8R8; 2 A8R8G8B8 = bytes B,G,R,A), matrix (0 BT.601, 1 BT.709), full_range, alpha (8 bit, for A formats), dst_pitch (bytes, 0 = tight). */
enum { VIC_FMT_ABGR = 0, VIC_FMT_XBGR = 1, VIC_FMT_ARGB = 2 };
#define VIC_CFG_WORDS 9

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

/* MSENC (class 0x21): executes are accepted and counted; no bitstream is produced (status: 0 bytes). */
#define MSENC_SET_STATUS_OFFSET 0x109
