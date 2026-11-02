/* Register map of the host1x multimedia engines (word offsets inside the engine class = method byte offset / 4). Everything the engines read or write goes
 * through the names here. 2026-11-02 (r10): checked against public NVIDIA headers, used as layout facts only (numbers, sizes, offsets):
 *   [ogd]      NVIDIA open-gpu-doc classes/video: clc5b0.h (NVDEC methods), nvdec_drv.h (picture-info / status structures), cle7d0.h (NVJPG methods),
 *              nvjpg_drv.h (NVJPG picture parameters / status), clc5b7.h (NVENC methods), nvenc_drv.h (NVENC H.264 picture setup / status), clceb6.h (VIC
 *              methods) - MIT licensed. These are later engine generations than the Tegra X1's (NVDEC 2, VIC 4.0, NVJPG 1, NVENC 5); where a T210 stream
 *              was traced (MK8D, 2026-10-27) it agrees with them, and the table in docs/reference/nvdrv-layouts.md says per item which source pins it.
 *   [falcon]   Linux drivers/gpu/drm/tegra/falcon.h: FALCON_UCLASS_METHOD_OFFSET 0x40 / METHOD_DATA 0x44 (the THI method FIFO, words 0x10 / 0x11).
 *   [l4t]      NVIDIA L4T drivers/video/tegra/host/class_ids.h (host1x class ids) and host1x01_hardware.h (opcodes, see nvdrv_media_core.c).
 * Offsets of surfaces are iova >> 8. */
#pragma once

/* all engines [ogd: SET_APPLICATION_ID 0x200, EXECUTE 0x300 in every class header] */
#define NVM_SET_APPLICATION_ID 0x080 /* NVDEC: codec id (decoder_backend.h, = the clc5b0 ids); NVJPG: 1 decoder, 2 encoder; VIC / MSENC: ignored */
#define NVM_EXECUTE 0x0c0
/* THI (host1x client interface) method FIFO [falcon]: METHOD0 takes the method (the register's word offset in the table below), METHOD1 the data. A real SDK
 * stream (MK8D, 2026-10-27) writes every engine register this way, as INCR 0x10 count 2 pairs. */
#define NVM_THI_METHOD0 0x010
#define NVM_THI_METHOD1 0x011

/* NVDEC (class 0xF0) [ogd clc5b0.h: byte offsets 0x400.., all 17 picture slots and the per-codec buffers] */
#define NVDEC_SET_CONTROL_PARAMS 0x100
#define NVDEC_SET_DRV_PIC_SETUP_OFFSET 0x101 /* picture-info structure */
#define NVDEC_SET_IN_BUF_BASE_OFFSET 0x102   /* coded frame */
#define NVDEC_SET_PICTURE_INDEX 0x103 /* a running frame number in MK8D's stream (0, 1, 2, ... past the 17 slots), not the output slot: H.264 takes curr_pic_idx below */
#define NVDEC_SET_SLICE_OFFSETS_BUF_OFFSET 0x104
#define NVDEC_SET_COLOC_DATA_OFFSET 0x105
#define NVDEC_SET_HISTORY_OFFSET 0x106
#define NVDEC_SET_DISPLAY_BUF_SIZE 0x107
#define NVDEC_SET_HISTOGRAM_OFFSET 0x108
#define NVDEC_SET_STATUS_OFFSET 0x109 /* SET_NVDEC_STATUS_OFFSET */
#define NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET 0x10a   /* the output surface when the slot register of the picture is empty */
#define NVDEC_SET_DISPLAY_BUF_CHROMA_OFFSET 0x10b
#define NVDEC_SET_PICTURE_LUMA_OFFSET0 0x10c   /* + picture index, 17 slots (MK8D's stream, 2026-10-27: all 17 written, luma 0x10c.., chroma 0x11d..) */
#define NVDEC_SET_PICTURE_CHROMA_OFFSET0 0x11d /* + picture index */
#define NVDEC_PIC_SLOTS 17
#define NVDEC_SET_PIC_SCRATCH_BUF_OFFSET 0x12e
#define NVDEC_SET_EXTERNAL_MVBUFFER_OFFSET 0x12f
#define NVDEC_H264_SET_MBHIST_BUF_OFFSET 0x140 /* MK8D writes one buffer address here before every execute (0x100210 << 8); the decode does not need it */
#define NVDEC_VP8_SET_PROB_DATA_OFFSET 0x150
#define NVDEC_VP8_SET_HEADER_PARTITION_BUF_BASE_OFFSET 0x151
#define NVDEC_VP9_SET_PROB_TAB_BUF_OFFSET 0x170 /* .. 0x177 VP9_SET_FILTER_BUFFER_OFFSET: eight VP9 work buffers */
#define NVDEC_VP9_SET_FILTER_BUFFER_OFFSET 0x177
/* Words of the H.264 picture-info structure [ogd nvdec_drv.h nvdec_h264_pic_s; the same words were matched on MK8D's 512x608 CABAC stream: 0xa8fe = the
 * 43262-byte slice at word 18, 32 x 38 macroblocks at words 25/26, pitches 0x200 at 35/36]. */
#define NVDEC_H264_W_STREAM_LEN 18
#define NVDEC_H264_W_SLICE_COUNT 19
#define NVDEC_H264_W_LOG2_MAX_POC_LSB_M4 22
#define NVDEC_H264_W_DELTA_POC_ZERO 23
#define NVDEC_H264_W_FRAME_MBS_ONLY 24
#define NVDEC_H264_W_WIDTH_MBS 25
#define NVDEC_H264_W_HEIGHT_MAP_UNITS 26
#define NVDEC_H264_W_TILE_FORMAT 27 /* bits 0-1: 0 = pitch linear (TBL), bits 2-4 GOB height */
#define NVDEC_H264_W_CABAC 28
#define NVDEC_H264_W_POC_PRESENT 29
#define NVDEC_H264_W_NUM_REF_L0 30
#define NVDEC_H264_W_NUM_REF_L1 31
#define NVDEC_H264_W_DEBLOCK_CTRL 32
#define NVDEC_H264_W_REDUNDANT_PIC_CNT 33
#define NVDEC_H264_W_TRANSFORM_8X8 34
#define NVDEC_H264_W_PITCH_LUMA 35
#define NVDEC_H264_W_PITCH_CHROMA 36
#define NVDEC_H264_W_HIST_BUFFER_SIZE 43
#define NVDEC_H264_W_FLAGS 44 /* b0 mbaff, b1 direct_8x8_inference, b2 weighted_pred, b3 constrained_intra, b4 ref_pic, b5 field_pic, b6 bottom_field, b7 second_field,
                               * b8-11 log2_max_frame_num-4, b12-13 chroma_format_idc, b14-15 poc type, b16-21 pic_init_qp-26 (signed), b22-26 chroma_qp_index_offset
                               * (signed), b27-31 second_chroma_qp_index_offset */
#define NVDEC_H264_W_FLAGS2 45 /* b0-1 weighted_bipred_idc, b2-8 CurrPicIdx (the DPB slot this picture is decoded into), b9-13 CurrColIdx, b14-29 frame_num,
                                * b30 frame_surfaces, b31 output_memory_layout (0 NV12) */
#define NVDEC_H264_CURR_PIC_IDX(w45) (((w45) >> 2) & 0x7f)
#define NVDEC_H264_PIC_WORDS 46
#define NVDEC_H264_PIC_BYTES 764 /* sizeof(nvdec_h264_pic_s) */
/* VP9 picture-info [ogd nvdec_vp9_pic_s]: word 12 stream_len, word 24 = u16 width | u16 height << 16 of the current frame, word 25 = u16 luma stride |
 * u16 chroma stride << 16 (framestride[2]); words 18-23 are the three reference frames' sizes and strides. */
#define NVDEC_VP9_W_SIZE 24
#define NVDEC_VP9_W_STRIDES 25
#define NVDEC_VP9_PIC_BYTES 256
/* VP8 picture-info [ogd nvdec_vp8_pic_s, 0xC0 bytes]: word 14 = u16 FrameWidth | u16 FrameHeight << 16, word 18 VLDBufferSize (the coded frame's size in
 * bytes), words 19/20 FrameStride luma / chroma. */
#define NVDEC_VP8_W_SIZE 14
#define NVDEC_VP8_W_STRIDE_LUMA 19
#define NVDEC_VP8_W_STRIDE_CHROMA 20
#define NVDEC_PIC_STREAM_LEN_WORD_H264 NVDEC_H264_W_STREAM_LEN
#define NVDEC_PIC_STREAM_LEN_WORD_VP9 12
#define NVDEC_PIC_STREAM_LEN_WORD_H265 12 /* nvdec_hevc_pic_s.stream_len */
#define NVDEC_PIC_STREAM_LEN_WORD_VP8 18  /* VLDBufferSize */
/* Status structure written after an execute [ogd nvdec_status_s, 0x38 bytes]: u32 mbs_correctly_decoded, u32 mbs_in_error, u32 cycle_count, u32 error_status
 * (0 = ok; Napple's codes: 1 codec refused, 2 unmapped surface, 3 decode failed, 4 parameter sets refused), a 9-word HEVC / VP9 statistics union
 * (intra / inter / skip block counts, motion sums, error positions), u32 slice_header_error_code. */
#define NVDEC_STATUS_WORDS 14
#define NVDEC_STATUS_W_ERROR 3
#define NVDEC_STATUS_W_INTRA_CNT 4 /* HEVC / VP9: 8x8 intra blocks */
#define NVDEC_STATUS_W_INTER_CNT 5 /* HEVC / VP9: 8x8 inter blocks */

/* VIC (class 0x5D). Methods 0x1c1-0x1ce as [ogd clceb6.h] names them (unchanged from the VIC 4.0 the emulators describe): 0x1c1 SetControlParams, 0x1c2
 * SetConfigStructOffset, 0x1c8 / 0x1c9 / 0x1ca output surface luma / chroma U / chroma V, 0x1ce SetStatusOffset. Slot 0's input surface luma / chroma are the
 * words MK8D writes at 0x100 / 0x101 (2026-10-27 trace; VIC 4.0 keeps the slot surfaces at 0x400 + slot * 0x20 bytes, the later clceb6 moved them to 0x1200).
 * Addresses are device addresses >> 8. */
#define VIC_SET_INPUT0_LUMA_OFFSET 0x100
#define VIC_SET_INPUT0_CHROMA_OFFSET 0x101
#define VIC_SET_CONTROL_PARAMS 0x1c1
#define VIC_SET_CONFIG_STRUCT_OFFSET 0x1c2
#define VIC_SET_FILTER_STRUCT_OFFSET 0x1c3
#define VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET 0x1c8
#define VIC_SET_OUTPUT_SURFACE_CHROMA_OFFSET 0x1c9
#define VIC_SET_OUTPUT_SURFACE_CHROMA_V_OFFSET 0x1ca
#define VIC_SET_STATUS_OFFSET 0x1ce
/* Config structure (VIC 4.0: no public NVIDIA header; the layout below is the emulators' and matches MK8D's stream; clceb6's VIC 4.2+ config differs). Only
 * the output surface configuration (u64 at +0x20) is read. Bits 0-6 pixel format, 11-14 block kind (0 = pitch linear, else block linear), 15-18 log2 block
 * height in GOBs, 32-45 surface width - 1, 46-59 surface height - 1. Pixel formats: 0x1f RGBA8 (bytes R,G,B,A), 0x20 BGRA8, 0x23 RGBX8 (alpha 255), 0x44
 * NV12 (Y8 + interleaved U8V8, 4:2:0). Pitch-linear rows: width*4 for RGB, the width aligned to 256 for NV12 (as the emulator writes them).
 * Input: NV12 of the surface size, rows aligned to 256 (or packed when the input surface is too small for that). Colour: BT.601 limited range. */
#define VIC_CFG_OUTPUT_SURFACE 0x20
#define VIC_CFG_BYTES 0x28
enum { VIC_FMT_RGBA8 = 0x1f, VIC_FMT_BGRA8 = 0x20, VIC_FMT_RGBX8 = 0x23, VIC_FMT_NV12 = 0x44 };

/* NVJPG (class 0xC0) [ogd cle7d0.h NVE7D0_VIDEO_NVJPG: byte offsets 0x700..0x71c] */
#define NVJPG_SET_CONTROL_PARAMS 0x1c0
#define NVJPG_SET_PICTURE_INDEX 0x1c1
#define NVJPG_SET_IN_DRV_PIC_SETUP 0x1c2  /* nvjpg_drv_pic_param_s */
#define NVJPG_SET_OUT_STATUS 0x1c3        /* nvjpg_dec_status / nvjpg_enc_status */
#define NVJPG_SET_BITSTREAM 0x1c4         /* decode: the JPEG; encode: where the entropy-coded data goes */
#define NVJPG_SET_CUR_PIC 0x1c5           /* decode: output luma / packed RGB; encode: input luma */
#define NVJPG_SET_CUR_PIC_CHROMA_U 0x1c6  /* chroma U plane (or interleaved UV when semi-planar) */
#define NVJPG_SET_CUR_PIC_CHROMA_V 0x1c7
enum { NVJPG_APP_DECODER = 1, NVJPG_APP_ENCODER = 2 }; /* SET_APPLICATION_ID values [ogd cle7d0.h] */
/* nvjpg_drv_pic_param_s [ogd nvjpg_drv.h, built with NV_NVJPG_SUPPORT_ENC: the Tegra X1 NVJPG encodes and decodes; 0x1240 bytes, 256-bit aligned groups]: */
#define NVJPG_PIC_ENC_TOP_CTL 0x000        /* b0-1 chroma_format (0 444, 1 422H, 2 422V, 3 420), b2-13 mcu_width, b14-25 mcu_height, b26 stuff_disable */
#define NVJPG_PIC_ENC_RST_INTERVAL 0x004   /* b0-15 restart_interval (MCUs) */
#define NVJPG_PIC_ENC_QUANT 0x020          /* u16 quantLumaFactor[64], u16 quantChromaFactor[64]: (1 << 15) / q, raster order */
#define NVJPG_PIC_ENC_HUFF 0x120           /* {u16 length, u16 value}: hfDcLuma[12], hfAcLuma[176], hfDcChroma[12], hfAcChroma[176], indexed by symbol */
#define NVJPG_PIC_PDMA_PIC_INFO 0x700      /* b0-2 input_type, b3-5 chroma_mode, b6-17 width_in_8x8, b18-29 height_in_8x8, b30-31 power2_scale */
#define NVJPG_PIC_PDMA_PIC_INFO1 0x704     /* b15-16 tiling_mode, b17-19 blklinear_gob, b21-24 width_pad, b25-28 height_pad, b29-31 memory_mode */
#define NVJPG_PIC_LUMA_STRIDE 0x708
#define NVJPG_PIC_CHROMA_STRIDE 0x70c
#define NVJPG_PIC_BITSTREAM_OFFSET 0x720
#define NVJPG_PIC_BITSTREAM_SIZE 0x724
#define NVJPG_PIC_DEC_FRAME_SIZE 0x740     /* b0-11 mcu_width, b16-27 mcu_height */
#define NVJPG_PIC_DEC_FRAME_PARAM 0x744    /* b0-2 component, b3-18 restart_interval, b19-21 stream_chroma_mode (0 mono, 1 420, 2 422H, 3 422V, 4 444), b22
                                            * motion_jpeg_type, b23-25 output_type (0 YUV, 1 YUY2, 2 RGBA, 3 BGRA, 4 ABGR, 5 ARGB), b26-27 power2_downscale,
                                            * b28-29 chroma_subsample (0 off, 1 to 420, 2 to 422H) */
#define NVJPG_PIC_DEC_BLOCK_CTL 0x748      /* [4] */
#define NVJPG_PIC_DEC_HUFF_MINCODE 0x760   /* [6][16] */
#define NVJPG_PIC_DEC_HUFF_SYMBOL 0x8e0    /* [3][174] */
#define NVJPG_PIC_DEC_OUTPUT_INFO 0x1120   /* b24-31 alpha_val */
#define NVJPG_PIC_DEC_YUV2RGB 0x1124       /* u32[6] */
#define NVJPG_PIC_DEC_QUANT 0x1140         /* u8 [64][4] */
#define NVJPG_PIC_BYTES 0x1240
#define NVJPG_PIC_ENC_BYTES 0x700          /* the encoder half; the decoder half starts at the PDMA group */
/* Status [ogd nvjpg_dec_status / nvjpg_enc_status, 0x20 bytes]: decode {bytes_offset, mcu_x, mcu_y, cycle_count, error_status, reserved[12]};
 * encode {bitstream_size, mcu_x, mcu_y, cycle_count, error_status, reserved[12]}. Error values [ogd clc9d1.h NVC9D1_ERROR_JPG*]. */
#define NVJPG_STATUS_BYTES 0x20
#define NVJPG_STATUS_W_ERROR 4
enum { NVJPG_ERR_APPTIMER_EXPIRED = 0x30000001, NVJPG_ERR_INVALID_INPUT = 0x30000002, NVJPG_ERR_HWERR = 0x30000003, NVJPG_ERR_BAD_MAGIC = 0x30000004 };
enum { NVJPG_OUT_YUV = 0, NVJPG_OUT_YUY2 = 1, NVJPG_OUT_RGBA = 2, NVJPG_OUT_BGRA = 3, NVJPG_OUT_ABGR = 4, NVJPG_OUT_ARGB = 5 };
enum { NVJPG_CHROMA_MONO = 0, NVJPG_CHROMA_420 = 1, NVJPG_CHROMA_422H = 2, NVJPG_CHROMA_422V = 3, NVJPG_CHROMA_444 = 4 }; /* decoder numbering */

/* MSENC / NVENC (class 0x21) [ogd clc5b7.h: byte offsets 0x700..0x744; reference pictures 0x400.., the method block the MSENC 5 of the X1 shares]. One NV12
 * frame per EXECUTE through the EncoderBackend (decoder_backend.h). Surface registers are iova >> 8. Without a readable setup structure an execute is accepted
 * and counted and produces no bitstream (status all zero). */
#define MSENC_SET_CONTROL_PARAMS 0x1c0
#define MSENC_SET_PICTURE_INDEX 0x1c1
#define MSENC_SET_IN_RCDATA 0x1c3
#define MSENC_SET_IN_DRV_PIC_SETUP 0x1c4      /* nvenc_h264_drv_pic_setup_s */
#define MSENC_SET_OUT_ENC_STATUS 0x1c6        /* nvenc_pic_stat_s */
#define MSENC_SET_OUT_BITSTREAM 0x1c7
#define MSENC_SET_IOHISTORY 0x1c8
#define MSENC_SET_OUT_REF_PIC_LUMA 0x1cc
#define MSENC_SET_IN_CUR_PIC 0x1cd            /* input luma */
#define MSENC_SET_IN_CUR_PIC_CHROMA_U 0x1d0   /* input chroma (interleaved UV when semi-planar) */
#define MSENC_SET_IN_CUR_PIC_CHROMA_V 0x1d1
#define MSENC_SET_OUT_REF_PIC_CHROMA 0x1d3
#define MSENC_SET_IN_REF_PIC0_LUMA 0x100      /* .. 0x11f: 16 luma then 16 chroma reference pictures */
/* nvenc_h264_drv_pic_setup_s [ogd nvenc_drv.h, 512 bytes]: u32 magic, then surface configs of 32 bytes (refpic +0x04, input +0x24, outputpic +0x44), sps +0x64,
 * pps +0x68, rate_control +0x70 (88 bytes: framerate +0x18, nal_bitrate +0x24, gop_length +0x30), pic_control +0xC8 (276 bytes: pic_type 0 P, 1 B, 2 I, 3 IDR in
 * bits 2-3 of byte +0xE0). nvenc_h264_surface_cfg_s: u16 frame_width_minus1, u16 frame_height_minus1, u16 sfc_pitch, u16 sfc_pitch_chroma, u16 trans_mode,
 * u16 pad, u32 luma_top_frm_offset, luma_bot, chroma_top_frm, chroma_bot (units of 256), u32 b0-6 block_height, b7 tiled_16x16, b8-9 memory_mode (0 semi-planar,
 * 1 planar), b10 nv21. */
#define MSENC_PIC_MAGIC 0x00
#define MSENC_PIC_INPUT_CFG 0x24
#define MSENC_CFG_WIDTH_M1 0x00
#define MSENC_CFG_HEIGHT_M1 0x02
#define MSENC_CFG_PITCH 0x04
#define MSENC_CFG_PITCH_CHROMA 0x06
#define MSENC_CFG_FLAGS 0x1c
#define MSENC_PIC_RC_FRAMERATE (0x70 + 0x18)
#define MSENC_PIC_RC_NAL_BITRATE (0x70 + 0x24)
#define MSENC_PIC_RC_GOP_LENGTH (0x70 + 0x30)
#define MSENC_PIC_PIC_TYPE_BYTE (0xc8 + 0xe0)
#define MSENC_PIC_BYTES 512
enum { MSENC_PIC_TYPE_P = 0, MSENC_PIC_TYPE_B = 1, MSENC_PIC_TYPE_I = 2, MSENC_PIC_TYPE_IDR = 3 };
/* nvenc_pic_stat_s [ogd, 128 bytes]: u32 picture_index, u32 b0-1 error_status / b2-31 ucode_error_status, u32 total_bit_count, u32 type1_bit_count, u16 pic_type,
 * u16 num_slices, u16 ave_activity, u16 avgQP, u32 cycle_count (+0x18), int hrdFullness, u32 bitstream_start_pos (+0x20), u32 last_valid_byte_offset (+0x24). */
#define MSENC_STAT_BYTES 128
#define MSENC_STAT_PICTURE_INDEX 0x00
#define MSENC_STAT_ERROR 0x04
#define MSENC_STAT_TOTAL_BITS 0x08
#define MSENC_STAT_PIC_TYPE 0x10
#define MSENC_STAT_NUM_SLICES 0x12
#define MSENC_STAT_LAST_VALID_BYTE 0x24
enum { MSENC_ERR_CODEC = 1, MSENC_ERR_ADDRESS = 2, MSENC_ERR_FAILED = 3, MSENC_ERR_PARAMS = 4 }; /* Napple codes, reported in ucode_error_status (error_status = 1) */
