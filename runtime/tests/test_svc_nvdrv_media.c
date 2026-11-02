/* Unit tests: nvdrv multimedia engines (nvdrv_media_core behind nvdrv.c): host1x command streams submitted through the real ioctl handlers,
 * NVDEC with the null backend and a recording backend, VIC colour conversion, NVJPG decode (synthetic JPEGs), MSENC, relocations, gathers,
 * malformed streams, unmapped surfaces, /dev/nvdisp-disp0. Register numbers and structures come from nvdrv_media_regs.h / nvdrv_media_core.h; the mirrors
 * below restate the public NVIDIA layouts (open-gpu-doc classes/video, tegra_dc_ext.h), one source comment per struct, and pin sizes and offsets. */
#include "../src/services/nvdrv.c"
#include "../src/services/nvdrv_media_regs.h"
#include "../src/services/codec_headers_core.h"
#include "../src/services/jpeg_core.h"
#include "check.h"
#include "codec_vectors.h"
#include <math.h>

#ifdef NAPPLE_HOST_TEST /* make test-host: kernel and thread stand-ins of tests/host */
#include "host/host_stubs.h"
#elif defined(__linux__) /* tools/linux-check: ipc_mock.c has everything but the summary hook */
void summary_ipc(const char *service, unsigned cmd, int status, unsigned request_bytes) { (void)service; (void)cmd; (void)status; (void)request_bytes; }
#endif

static IpcObject *nv;
static IpcCall call;
static uint8_t io[0x4000];
static uint32_t args[2];
static uint32_t cmd_id(unsigned dir, unsigned size, unsigned type, unsigned nr) { return (dir << 30) | (size << 16) | (type << 8) | nr; }
static uint32_t ioctl_(uint32_t fd, uint32_t cmd, void *data, uint32_t size) {
    memset(&call, 0, sizeof call);
    args[0] = fd; args[1] = cmd;
    call.object = nv; call.in = (const uint8_t *)args; call.in_size = 8;
    memcpy(io, data, size);
    call.na = 1; call.a[0] = (IpcBuf){(uintptr_t)io, size, 0};
    call.nb = 1; call.b[0] = (IpcBuf){(uintptr_t)io, size, 0};
    nv_ioctl(nv, &call);
    memcpy(data, io, size);
    uint32_t err; memcpy(&err, call.out + call.out_size - 4, 4);
    return err;
}
static uint32_t open_(const char *path) {
    memset(&call, 0, sizeof call);
    static char p[64]; snprintf(p, sizeof p, "%s", path);
    call.object = nv; call.na = 1; call.a[0] = (IpcBuf){(uintptr_t)p, strlen(p), 0};
    nv_open(nv, &call);
    uint32_t fd; memcpy(&fd, call.out, 4); return fd;
}
static void close_(uint32_t fd) { memset(&call, 0, sizeof call); args[0] = fd; call.object = nv; call.in = (const uint8_t *)args; call.in_size = 4; nv_close(nv, &call); }

typedef struct { uint32_t handle, iova; uint8_t *mem; uint32_t size; } Buf;
static uint32_t nvmap_fd;
/* nvmap handle backed by host memory, pinned through MAP_CMD_BUFFER on `chan`. */
static Buf mkbuf(uint32_t chan, uint32_t size) {
    Buf b = {0}; b.size = size; b.mem = aligned_alloc(4096, (size + 4095) & ~4095u); memset(b.mem, 0, size);
    uint32_t create[2] = {size, 0}; ioctl_(nvmap_fd, cmd_id(3, 8, 1, 0x01), create, 8); b.handle = create[1];
    uint8_t al[0x20] = {0}; uint64_t addr = (uintptr_t)b.mem; memcpy(al, &b.handle, 4); memcpy(al + 12, &(uint32_t){4096}, 4); memcpy(al + 24, &addr, 8);
    ioctl_(nvmap_fd, cmd_id(3, 0x20, 1, 0x04), al, 0x20);
    uint32_t mb[5] = {1, 0, 0, b.handle, 0}; ioctl_(chan, cmd_id(3, 20, 0, 0x09), mb, sizeof mb); b.iova = mb[4];
    return b;
}
/* A SUBMIT of one command buffer (words in `cb`) with optional relocations {cmdbuf offset, target handle, target offset, shift}. */
typedef struct { uint32_t at, target, toff, shift; } RelocSpec;
static uint32_t submit(uint32_t chan, Buf *cb, const uint32_t *words, uint32_t n, const RelocSpec *rel, uint32_t nrel) {
    memcpy(cb->mem, words, (size_t)n * 4);
    uint8_t sub[16 + 12 + 16 * 4 + 4 * 4] = {0};
    uint32_t hdr[4] = {1, nrel, 0, 0}; memcpy(sub, hdr, 16);
    uint32_t c[3] = {cb->handle, 0, n}; memcpy(sub + 16, c, 12);
    for (uint32_t i = 0; i < nrel; i++) {
        uint32_t r[4] = {cb->handle, rel[i].at, rel[i].target, rel[i].toff}; memcpy(sub + 28 + i * 16, r, 16);
        memcpy(sub + 28 + nrel * 16 + i * 4, &rel[i].shift, 4);
    }
    return ioctl_(chan, cmd_id(3, 16, 0, 0x01), sub, 28 + nrel * 20);
}
#define SETCL(cls) ((0u << 28) | ((cls) << 6))
#define INCR(off, n) ((1u << 28) | ((off) << 16) | (n))
#define NONINCR(off, n) ((2u << 28) | ((off) << 16) | (n))
#define IMM(off, v) ((4u << 28) | ((off) << 16) | (v))
#define GATHER(n) ((6u << 28) | (n))
#define GATHER_REG(off, incr, n) ((6u << 28) | ((off) << 16) | (1u << 15) | ((incr) << 14) | (n))

/* ---- layout mirrors (public NVIDIA headers, layout facts only) --------------------------------------------------------------------------------------- */
#include <stddef.h>
typedef struct { /* open-gpu-doc nvdec_drv.h nvdec_h264_pic_s (764 bytes) */
    uint8_t encryption_params[52], eos[16], explicit_eos, hint_dump_en, reserved0[2];
    uint32_t stream_len, slice_count, mbhist_buffer_size, gptimer_timeout_value;
    int32_t log2_max_poc_lsb_m4, delta_pic_order_always_zero, frame_mbs_only, pic_width_in_mbs, frame_height_in_mbs; uint32_t tile_format;
    int32_t entropy_coding_mode, pic_order_present, num_ref_l0_m1, num_ref_l1_m1, deblocking_ctrl_present, redundant_pic_cnt_present, transform_8x8;
    uint32_t pitch_luma, pitch_chroma, luma_top, luma_bot, luma_frame, chroma_top, chroma_bot, chroma_frame, hist_buffer_size, flags, flags2;
    int32_t curr_field_order_cnt[2]; uint8_t dpb[16][16]; uint8_t rest[764 - 0xc0 - 256];
} MH264Pic;
typedef struct { /* open-gpu-doc nvdec_drv.h nvdec_vp9_pic_s (256 bytes, leading part) */
    uint32_t keys[12], stream_len, enable_encryption, bits, gptimer; uint8_t tileformat, reserved1[3]; uint32_t bsd_ctrl_offset;
    uint16_t ref0[4], ref1[4], ref2[4], width, height, framestride[2]; uint8_t rest[256 - 0x68];
} MVp9Pic;
typedef struct { /* open-gpu-doc nvdec_drv.h nvdec_vp8_pic_s (0xC0 bytes, leading part) */
    uint8_t encryption_params[52]; uint32_t gptimer; uint16_t frame_width, frame_height; uint8_t key_frame, version, tile, error_conceal;
    uint32_t first_part_size, hist_buffer_size, vld_buffer_size, frame_stride[2]; uint8_t rest[0xc0 - 0x54];
} MVp8Pic;
typedef struct { uint32_t mbs_correct, mbs_in_error, cycle_count, error_status, stats[9], slice_header_error_code; } MNvdecStatus; /* nvdec_drv.h nvdec_status_s */
typedef struct { /* open-gpu-doc nvjpg_drv.h nvjpg_drv_pic_param_s with NV_NVJPG_SUPPORT_ENC (0x1240 bytes) */
    uint32_t enc_top_ctl, enc_restart_interval, enc_fdct_rc, enc_vle_rc, enc_align256[4];
    uint16_t quant_luma[64], quant_chroma[64];
    struct { uint16_t length, value; } hf_dc_luma[12], hf_ac_luma[176], hf_dc_chroma[12], hf_ac_chroma[176];
    uint32_t pdma_pic_info, pdma_pic_info1, luma_stride, chroma_stride, pdma_dec_packet_info, pdma_align256[3];
    uint32_t bitstream_offset, bitstream_size, sdma_align256[6];
    uint32_t dec_frame_size, dec_frame_param, dec_block_ctl[4], dec_align256_0[2], dec_huff_mincode[6][16], dec_huff_symbol[3][174], dec_align256_1[6];
    uint32_t dec_output_info, dec_yuv2rgb_param[6], dec_align256_2; uint8_t dec_quant[64][4];
} MNvjpgPic;
typedef struct { uint32_t w0 /* bytes_offset | bitstream_size */, mcu_x, mcu_y, cycle_count, error_status; uint8_t reserved[12]; } MNvjpgStatus; /* nvjpg_drv.h */
typedef struct { uint16_t width_m1, height_m1, pitch, pitch_chroma, trans_mode, reserved2; uint32_t luma_top, luma_bot, chroma_top, chroma_bot, flags; } MNvencSurfaceCfg; /* nvenc_drv.h */
typedef struct { /* open-gpu-doc nvenc_drv.h nvenc_h264_rc_s (88 bytes, leading part) */
    uint8_t hrd_type, qp[3], min_qp[3], max_qp[3], max_qpd, base_qpd; int32_t rhopbi[3], framerate; uint32_t buffersize;
    int32_t nal_cpb_size, nal_bitrate, vcl_cpb_size, vcl_bitrate; uint32_t gop_length; uint8_t rest[88 - 0x34];
} MNvencRc;
typedef struct { /* open-gpu-doc nvenc_drv.h nvenc_h264_drv_pic_setup_s (512 bytes) */
    uint32_t magic; MNvencSurfaceCfg refpic_cfg, input_cfg, outputpic_cfg; uint32_t sps_data, pps_data[2]; MNvencRc rate_control; uint8_t pic_control[276];
    MNvencSurfaceCfg half_scaled_outputpic_cfg; uint32_t gptimer_timeout_val;
} MNvencSetup;
typedef struct { /* open-gpu-doc nvenc_drv.h nvenc_pic_stat_s (128 bytes) */
    uint32_t picture_index, error, total_bit_count, type1_bit_count; uint16_t pic_type, num_slices, ave_activity, avg_qp; uint32_t cycle_count; int32_t hrd_fullness;
    uint32_t bitstream_start_pos, last_valid_byte_offset; uint8_t rest[128 - 0x28];
} MNvencStat;
typedef struct { uint8_t fg[3], bg[3]; uint32_t buff_id, flags; int16_t x, y; uint32_t alpha, colorfmt; } MDcCursorImage; /* tegra_dc_ext.h tegra_dc_ext_cursor_image */
typedef struct { int16_t x, y; uint32_t flags; } MDcCursor;                                                          /* tegra_dc_ext_cursor */
typedef struct { uint32_t win_index; uint16_t yof, kyrgb, kur, kvr, kug, kvg, kub, kvb; } MDcCsc;                     /* tegra_dc_ext_csc */
typedef struct { uint32_t win_index, flags, start, len; uint64_t r, g, b; } MDcLut;                                   /* tegra_dc_ext_lut (LP64) */
typedef struct { uint32_t flags, pad[3]; } MDcStatus;                                                                /* tegra_dc_ext_status */
typedef struct { uint8_t enable, reserved[3]; } MDcSetVblank;                                                        /* tegra_dc_ext_set_vblank */
typedef struct { uint16_t cmu_enable, csc[9], lut1[256], lut2[960]; } MDcCmu;                                        /* tegra_dc_ext_cmu */
#define PIN(t, sz) CHECK(sizeof(t) == (sz), "layout: sizeof(" #t ") == %#x (got %#zx)", (unsigned)(sz), sizeof(t))
#define OFF(t, f, o) CHECK(offsetof(t, f) == (size_t)(o), "layout: " #t "." #f " at %#x (got %#zx)", (unsigned)(o), offsetof(t, f))
static void layout_pins(void) {
    PIN(MH264Pic, NVDEC_H264_PIC_BYTES); OFF(MH264Pic, stream_len, NVDEC_H264_W_STREAM_LEN * 4); OFF(MH264Pic, log2_max_poc_lsb_m4, NVDEC_H264_W_LOG2_MAX_POC_LSB_M4 * 4);
    OFF(MH264Pic, pic_width_in_mbs, NVDEC_H264_W_WIDTH_MBS * 4); OFF(MH264Pic, frame_height_in_mbs, NVDEC_H264_W_HEIGHT_MAP_UNITS * 4); OFF(MH264Pic, tile_format, NVDEC_H264_W_TILE_FORMAT * 4);
    OFF(MH264Pic, entropy_coding_mode, NVDEC_H264_W_CABAC * 4); OFF(MH264Pic, transform_8x8, NVDEC_H264_W_TRANSFORM_8X8 * 4); OFF(MH264Pic, pitch_luma, NVDEC_H264_W_PITCH_LUMA * 4);
    OFF(MH264Pic, pitch_chroma, NVDEC_H264_W_PITCH_CHROMA * 4); OFF(MH264Pic, hist_buffer_size, NVDEC_H264_W_HIST_BUFFER_SIZE * 4); OFF(MH264Pic, flags, NVDEC_H264_W_FLAGS * 4);
    OFF(MH264Pic, flags2, NVDEC_H264_W_FLAGS2 * 4); OFF(MH264Pic, dpb, 0xc0);
    PIN(MVp9Pic, NVDEC_VP9_PIC_BYTES); OFF(MVp9Pic, stream_len, NVDEC_PIC_STREAM_LEN_WORD_VP9 * 4); OFF(MVp9Pic, width, NVDEC_VP9_W_SIZE * 4); OFF(MVp9Pic, framestride, NVDEC_VP9_W_STRIDES * 4);
    PIN(MVp8Pic, 0xc0); OFF(MVp8Pic, frame_width, NVDEC_VP8_W_SIZE * 4); OFF(MVp8Pic, vld_buffer_size, NVDEC_PIC_STREAM_LEN_WORD_VP8 * 4); OFF(MVp8Pic, frame_stride, NVDEC_VP8_W_STRIDE_LUMA * 4);
    PIN(MNvdecStatus, NVDEC_STATUS_WORDS * 4); OFF(MNvdecStatus, error_status, NVDEC_STATUS_W_ERROR * 4); OFF(MNvdecStatus, stats, NVDEC_STATUS_W_INTRA_CNT * 4);
    PIN(MNvjpgPic, NVJPG_PIC_BYTES); OFF(MNvjpgPic, enc_restart_interval, NVJPG_PIC_ENC_RST_INTERVAL); OFF(MNvjpgPic, quant_luma, NVJPG_PIC_ENC_QUANT); OFF(MNvjpgPic, hf_dc_luma, NVJPG_PIC_ENC_HUFF);
    OFF(MNvjpgPic, pdma_pic_info, NVJPG_PIC_PDMA_PIC_INFO); OFF(MNvjpgPic, pdma_pic_info1, NVJPG_PIC_PDMA_PIC_INFO1); OFF(MNvjpgPic, luma_stride, NVJPG_PIC_LUMA_STRIDE);
    OFF(MNvjpgPic, chroma_stride, NVJPG_PIC_CHROMA_STRIDE); OFF(MNvjpgPic, bitstream_offset, NVJPG_PIC_BITSTREAM_OFFSET); OFF(MNvjpgPic, bitstream_size, NVJPG_PIC_BITSTREAM_SIZE);
    OFF(MNvjpgPic, dec_frame_size, NVJPG_PIC_DEC_FRAME_SIZE); OFF(MNvjpgPic, dec_frame_param, NVJPG_PIC_DEC_FRAME_PARAM); OFF(MNvjpgPic, dec_block_ctl, NVJPG_PIC_DEC_BLOCK_CTL);
    OFF(MNvjpgPic, dec_huff_mincode, NVJPG_PIC_DEC_HUFF_MINCODE); OFF(MNvjpgPic, dec_huff_symbol, NVJPG_PIC_DEC_HUFF_SYMBOL); OFF(MNvjpgPic, dec_output_info, NVJPG_PIC_DEC_OUTPUT_INFO);
    OFF(MNvjpgPic, dec_yuv2rgb_param, NVJPG_PIC_DEC_YUV2RGB); OFF(MNvjpgPic, dec_quant, NVJPG_PIC_DEC_QUANT);
    PIN(MNvjpgStatus, NVJPG_STATUS_BYTES); OFF(MNvjpgStatus, error_status, NVJPG_STATUS_W_ERROR * 4);
    PIN(MNvencSurfaceCfg, 32); OFF(MNvencSurfaceCfg, pitch, MSENC_CFG_PITCH); OFF(MNvencSurfaceCfg, pitch_chroma, MSENC_CFG_PITCH_CHROMA); OFF(MNvencSurfaceCfg, flags, MSENC_CFG_FLAGS);
    PIN(MNvencRc, 88); PIN(MNvencSetup, MSENC_PIC_BYTES); OFF(MNvencSetup, input_cfg, MSENC_PIC_INPUT_CFG); OFF(MNvencSetup, rate_control.framerate, MSENC_PIC_RC_FRAMERATE);
    OFF(MNvencSetup, rate_control.nal_bitrate, MSENC_PIC_RC_NAL_BITRATE); OFF(MNvencSetup, rate_control.gop_length, MSENC_PIC_RC_GOP_LENGTH); OFF(MNvencSetup, pic_control, 0xc8);
    PIN(MNvencStat, MSENC_STAT_BYTES); OFF(MNvencStat, error, MSENC_STAT_ERROR); OFF(MNvencStat, total_bit_count, MSENC_STAT_TOTAL_BITS); OFF(MNvencStat, pic_type, MSENC_STAT_PIC_TYPE);
    OFF(MNvencStat, num_slices, MSENC_STAT_NUM_SLICES); OFF(MNvencStat, last_valid_byte_offset, MSENC_STAT_LAST_VALID_BYTE);
    PIN(MDcCursorImage, NVDISP_SZ_CURSOR_IMAGE); OFF(MDcCursorImage, buff_id, 8); OFF(MDcCursorImage, x, 16); OFF(MDcCursorImage, colorfmt, 24);
    PIN(MDcCursor, NVDISP_SZ_CURSOR); PIN(MDcCsc, NVDISP_SZ_CSC); PIN(MDcLut, NVDISP_SZ_LUT); OFF(MDcLut, r, 16); PIN(MDcStatus, NVDISP_SZ_STATUS);
    PIN(MDcSetVblank, NVDISP_SZ_SET_VBLANK); PIN(MDcCmu, NVDISP_SZ_CMU); PIN(NvDispCmu, NVDISP_SZ_CMU);
    /* method word offsets = byte offsets of the class headers / 4 */
    CHECK(NVM_SET_APPLICATION_ID * 4 == 0x200 && NVM_EXECUTE * 4 == 0x300 && NVM_THI_METHOD0 * 4 == 0x40 && NVM_THI_METHOD1 * 4 == 0x44, "layout: common methods 0x200 / 0x300, THI 0x40 / 0x44");
    CHECK(NVDEC_SET_STATUS_OFFSET * 4 == 0x424 && NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET * 4 == 0x428 && NVDEC_SET_PICTURE_LUMA_OFFSET0 * 4 == 0x430 && NVDEC_SET_PICTURE_CHROMA_OFFSET0 * 4 == 0x474 &&
          NVDEC_H264_SET_MBHIST_BUF_OFFSET * 4 == 0x500 && NVDEC_VP9_SET_FILTER_BUFFER_OFFSET * 4 == 0x5dc, "layout: NVDEC methods (clc5b0)");
    CHECK(NVJPG_SET_IN_DRV_PIC_SETUP * 4 == 0x708 && NVJPG_SET_OUT_STATUS * 4 == 0x70c && NVJPG_SET_BITSTREAM * 4 == 0x710 && NVJPG_SET_CUR_PIC * 4 == 0x714 && NVJPG_SET_CUR_PIC_CHROMA_V * 4 == 0x71c,
          "layout: NVJPG methods (cle7d0)");
    CHECK(MSENC_SET_IN_DRV_PIC_SETUP * 4 == 0x710 && MSENC_SET_OUT_ENC_STATUS * 4 == 0x718 && MSENC_SET_OUT_BITSTREAM * 4 == 0x71c && MSENC_SET_IN_CUR_PIC * 4 == 0x734 &&
          MSENC_SET_IN_CUR_PIC_CHROMA_U * 4 == 0x740, "layout: NVENC methods (clc5b7)");
    CHECK(VIC_SET_CONFIG_STRUCT_OFFSET * 4 == 0x708 && VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET * 4 == 0x720 && VIC_SET_OUTPUT_SURFACE_CHROMA_OFFSET * 4 == 0x724 && VIC_SET_STATUS_OFFSET * 4 == 0x738,
          "layout: VIC methods (clceb6)");
    CHECK(cmd_id(1, 4, 'D', NVDISP_SET_NVMAP_FD) == 0x40044400u && cmd_id(2, 16, 'D', NVDISP_GET_STATUS) == 0x80104409u && cmd_id(2, 4, 'D', NVDISP_GET_VBLANK_SYNCPT) == 0x80044409u &&
          cmd_id(1, 40, 'D', NVDISP_SET_LUT) == 0x4028440au && cmd_id(2, 4, 'D', NVDISP_GET_WINMASK) == 0x8004441eu, "layout: tegra_dc_ext command words");
}
static const MediaStats *stats(uint32_t fd) { return media_engine_stats(fds[fd].ch.eng); }
/* VIC: a flat NV12 picture with rows of `pitch` bytes, and the config structure's output surface word (+0x20). */
static void vic_fill(Buf *y, Buf *c, int w, int h, int pitch, uint8_t Y, uint8_t U, uint8_t V) {
    memset(y->mem, 0, y->size); memset(c->mem, 0, c->size);
    for (int r = 0; r < h; r++) memset(y->mem + r * pitch, Y, (size_t)w);
    for (int r = 0; r < (h + 1) / 2; r++) for (int x = 0; x < w; x += 2) { c->mem[r * pitch + x] = U; c->mem[r * pitch + x + 1] = V; }
}
static void vic_cfg(Buf *cfg, uint32_t fmt, uint32_t kind, uint32_t bh, uint32_t w, uint32_t h) {
    memset(cfg->mem, 0, VIC_CFG_BYTES);
    uint64_t oc = fmt | ((uint64_t)kind << 11) | ((uint64_t)bh << 15) | ((uint64_t)(w - 1) << 32) | ((uint64_t)(h - 1) << 46);
    memcpy(cfg->mem + VIC_CFG_OUTPUT_SURFACE, &oc, 8);
}


/* NVJPG helpers: a decode picture-parameter block, and the JFIF headers a driver writes in front of the encoder's entropy-coded data. */
static void nvjpg_dec_pic(MNvjpgPic *p, uint32_t boff, uint32_t bsize, uint32_t smode, uint32_t otype, uint32_t down, uint32_t sub, uint32_t ls, uint32_t cs, uint8_t alpha) {
    memset(p, 0, sizeof *p);
    p->bitstream_offset = boff; p->bitstream_size = bsize; p->luma_stride = ls; p->chroma_stride = cs;
    p->dec_frame_param = (smode << 19) | (otype << 23) | (down << 26) | (sub << 28);
    p->dec_output_info = (uint32_t)alpha << 24;
}
static size_t sos_data_offset(const uint8_t *j, size_t n) { /* where the entropy-coded data of the first scan starts */
    for (size_t i = 2; i + 4 < n;) {
        if (j[i] != 0xff) return 0;
        size_t len = (size_t)(j[i + 2] << 8 | j[i + 3]);
        if (j[i + 1] == 0xda) return i + 2 + len;
        i += 2 + len;
    }
    return 0;
}
static const uint8_t zz[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                               35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static size_t put_marker(uint8_t *o, int m, const uint8_t *pl, int len) {
    o[0] = 0xff; o[1] = (uint8_t)m;
    if (len < 0) return 2;
    o[2] = (uint8_t)((len + 2) >> 8); o[3] = (uint8_t)(len + 2); memcpy(o + 4, pl, (size_t)len); return 4 + (size_t)len;
}
/* SOI, DQT (from the quantisers the engine derives from the factors), SOF0, DHT x4, DRI, SOS; returns the header length. chroma: JPEGSCAN_*. */
static size_t jfif_headers(uint8_t *o, const MNvjpgPic *p, int w, int h, int chroma, int ri, const JpegHuffCode dc[2], const JpegHuffCode ac[2]) {
    size_t n = put_marker(o, 0xd8, NULL, -1);
    for (int t = 0; t < 2; t++) {
        uint8_t q[65]; q[0] = (uint8_t)t;
        const uint16_t *f = t ? p->quant_chroma : p->quant_luma;
        for (int i = 0; i < 64; i++) { unsigned v = (32768u + f[zz[i]] / 2) / f[zz[i]]; q[1 + i] = (uint8_t)(v > 255 ? 255 : v); }
        n += put_marker(o + n, 0xdb, q, 65);
    }
    uint8_t samp = chroma == JPEGSCAN_420 ? 0x22 : chroma == JPEGSCAN_422H ? 0x21 : chroma == JPEGSCAN_422V ? 0x12 : 0x11;
    uint8_t sof[15] = {8, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(w >> 8), (uint8_t)w, 3, 1, samp, 0, 2, 0x11, 1, 3, 0x11, 1};
    n += put_marker(o + n, 0xc0, sof, 15);
    for (int t = 0; t < 4; t++) {
        uint8_t d[17 + 256], bits[17], vals[256];
        int k = napjpeg_huff_to_dht(t & 1 ? &ac[t >> 1] : &dc[t >> 1], bits, vals);
        d[0] = (uint8_t)(((t & 1) << 4) | (t >> 1)); memcpy(d + 1, bits + 1, 16); memcpy(d + 17, vals, (size_t)k);
        n += put_marker(o + n, 0xc4, d, 17 + k);
    }
    if (ri) { uint8_t dri[2] = {(uint8_t)(ri >> 8), (uint8_t)ri}; n += put_marker(o + n, 0xdd, dri, 2); }
    static const uint8_t sos[10] = {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
    return n + put_marker(o + n, 0xda, sos, 10);
}
/* Encoder-side picture parameters: Annex K tables at a quality, the Huffman codes in the 176-entry (16 runs x 11 sizes) arrangement. */
static void nvjpg_enc_pic(MNvjpgPic *p, int w, int h, int chroma, int ri, int mem, uint32_t ls, uint32_t cs, uint32_t boff, uint32_t bsize, JpegHuffCode dc[2], JpegHuffCode ac[2]) {
    static const uint8_t ql[64] = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
                                   18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
    static const uint8_t qc[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                   99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
    memset(p, 0, sizeof *p);
    int hs = chroma == JPEGSCAN_420 || chroma == JPEGSCAN_422H ? 2 : 1, vs = chroma == JPEGSCAN_420 || chroma == JPEGSCAN_422V ? 2 : 1;
    p->enc_top_ctl = (uint32_t)chroma | (uint32_t)((w + 8 * hs - 1) / (8 * hs)) << 2 | (uint32_t)((h + 8 * vs - 1) / (8 * vs)) << 14;
    p->enc_restart_interval = (uint32_t)ri;
    for (int i = 0; i < 64; i++) { /* quality 90: scale 20 */
        unsigned a = (ql[i] * 20u + 50) / 100, b = (qc[i] * 20u + 50) / 100;
        p->quant_luma[i] = (uint16_t)(32768u / (a ? a : 1)); p->quant_chroma[i] = (uint16_t)(32768u / (b ? b : 1));
    }
    napjpeg_std_huff(dc, ac);
    for (int c = 0; c < 12; c++) {
        p->hf_dc_luma[c].length = dc[0].len[c]; p->hf_dc_luma[c].value = dc[0].code[c];
        p->hf_dc_chroma[c].length = dc[1].len[c]; p->hf_dc_chroma[c].value = dc[1].code[c];
    }
    for (int r = 0; r < 16; r++) for (int z = 0; z <= 10; z++) {
        int sym = r << 4 | z;
        p->hf_ac_luma[r * 11 + z].length = ac[0].len[sym]; p->hf_ac_luma[r * 11 + z].value = ac[0].code[sym];
        p->hf_ac_chroma[r * 11 + z].length = ac[1].len[sym]; p->hf_ac_chroma[r * 11 + z].value = ac[1].code[sym];
    }
    p->pdma_pic_info = (uint32_t)((w + 7) / 8) << 6 | (uint32_t)((h + 7) / 8) << 18;
    p->pdma_pic_info1 = (uint32_t)(((w + 7) / 8) * 8 - w) << 21 | (uint32_t)(((h + 7) / 8) * 8 - h) << 25 | (uint32_t)mem << 29;
    p->luma_stride = ls; p->chroma_stride = cs; p->bitstream_offset = boff; p->bitstream_size = bsize;
}

/* A backend that records what it is asked and produces a 16x16 frame. */
static struct { int opened, closed, decoded, codec, resets, configures, refuse; DecodeRequest last; DecoderConfig cfg; uint8_t pic_head[8]; } rec;
static void *rec_open(int codec) { rec.opened++; rec.codec = codec; return &rec; }
static void rec_close(void *c) { (void)c; rec.closed++; }
static int rec_decode(void *c, const DecodeRequest *r, DecodeResult *o) {
    (void)c; rec.decoded++; rec.last = *r; if (r->pic) memcpy(rec.pic_head, r->pic, 8);
    memset(r->luma, 0x55, 16 * 16); memset(r->chroma, 0x66, 16 * 8); *o = (DecodeResult){16, 16, 16, 0, 0}; return 0;
}
static void rec_reset(void *c) { (void)c; rec.resets++; }
/* The parameter-set pointers in a DecoderConfig live only during the call (the engine synthesises them on its stack): keep copies, as a real backend must. */
static uint8_t rec_sets[2][H264_PARAM_MAX][256];
static int rec_configure(void *c, const DecoderConfig *cfg) {
    (void)c; rec.configures++; rec.cfg = *cfg;
    for (int i = 0; i < cfg->h264.nsps && i < H264_PARAM_MAX; i++) {
        size_t n = cfg->h264.sps_size[i] < 256 ? cfg->h264.sps_size[i] : 256; memcpy(rec_sets[0][i], cfg->h264.sps[i], n); rec.cfg.h264.sps[i] = rec_sets[0][i];
    }
    for (int i = 0; i < cfg->h264.npps && i < H264_PARAM_MAX; i++) {
        size_t n = cfg->h264.pps_size[i] < 256 ? cfg->h264.pps_size[i] : 256; memcpy(rec_sets[1][i], cfg->h264.pps[i], n); rec.cfg.h264.pps[i] = rec_sets[1][i];
    }
    return rec.refuse ? -1 : 0;
}
static const DecoderBackend rec_backend = {"recording", rec_open, rec_close, rec_decode, rec_reset, rec_configure, DECODER_BACKEND_ABI};

static double psnr(const uint8_t *a, const uint8_t *b, size_t n4) {
    double se = 0; size_t k = 0;
    for (size_t i = 0; i < n4; i++) if (i % 4 != 3) { double d = (double)a[i] - b[i]; se += d * d; k++; }
    return se == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / (se / (double)k));
}

/* b8: backends for the media-devices tests */
static void *refuse_vp9_open(int codec) { return codec == DEC_CODEC_VP9 ? NULL : decoder_backend_null.open(codec); }
static struct { int opened, closed, encoded; EncodeRequest last; } recenc;
static void *recenc_open(int codec) { recenc.opened++; return codec == DEC_CODEC_H264 ? &recenc : NULL; }
static void recenc_close(void *c) { (void)c; recenc.closed++; }
static int recenc_encode(void *c, const EncodeRequest *r, EncodeResult *o) { (void)c; recenc.encoded++; recenc.last = *r; memcpy(r->out, "\0\0\0\1\x65", 5); o->size = 5; o->idr = 1; return 0; }

int main(void) {
#ifdef NAPPLE_HOST_TEST
    host_thread_attach(calloc(1, 0x200));
#endif
    { /* the engines read guest memory through guest_ptr: the runtime build's one only accepts the guest address space, not this process's heap */
        static uint8_t probe[64];
        if (!guest_ptr((uintptr_t)probe, sizeof probe)) {
            printf("{\"test\":\"svc_nvdrv_media\",\"skipped\":\"guest_ptr rejects host memory in this build; run make test-host or tools/linux-check\"}\n");
            return 0;
        }
    }
    layout_pins();
    decoder_backend_install(NULL); /* the runtime build installs the VideoToolbox backend at startup (decoder_vt.c); these checks are about the null backend */
    nv = nv_new();
    nvmap_fd = open_("/dev/nvmap");
    uint32_t dec = open_("/dev/nvhost-nvdec"), vic = open_("/dev/nvhost-vic"), jpg = open_("/dev/nvhost-nvjpg"), enc = open_("/dev/nvhost-msenc");
    CHECK(fds[dec].ch.eng && fds[vic].ch.eng && fds[jpg].ch.eng && fds[enc].ch.eng, "every media channel has an engine");
    uint32_t ctrl = open_("/dev/nvhost-ctrl"); CHECK(!fds[ctrl].ch.eng && !fds[nvmap_fd].ch.eng, "nvmap and ctrl have none");

    /* ---- NVDEC with the null backend ---- */
    Buf cb = mkbuf(dec, 0x1000), bs = mkbuf(dec, 0x2000), pic = mkbuf(dec, 0x1000), luma = mkbuf(dec, 0x4000), chroma = mkbuf(dec, 0x2000), st = mkbuf(dec, 0x100);
    CHECK(cb.iova && bs.iova && luma.iova && cb.iova != bs.iova, "buffers pinned at distinct device addresses");
    memcpy(bs.mem, h264_64x48, sizeof h264_64x48);
    uint32_t len = sizeof h264_64x48; memcpy(pic.mem + 18 * 4, &len, 4);
    uint32_t prog[] = {SETCL(MEDIA_CLASS_NVDEC), NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_H264,
                       INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), pic.iova >> 8, bs.iova >> 8, 0,
                       INCR(NVDEC_SET_STATUS_OFFSET, 1), st.iova >> 8,
                       INCR(NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET, 2), luma.iova >> 8, chroma.iova >> 8,
                       IMM(NVM_EXECUTE, 0)};
    CHECK(submit(dec, &cb, prog, sizeof prog / 4, NULL, 0) == NV_OK, "NVDEC submit with an H.264 frame");
    CHECK(stats(dec)->executes == 1 && stats(dec)->frames == 1 && stats(dec)->errors == 0 && stats(dec)->unknown_regs == 0, "one execute, one frame, no errors (exec %llu frames %llu err %llu unk %llu)",
          (unsigned long long)stats(dec)->executes, (unsigned long long)stats(dec)->frames, (unsigned long long)stats(dec)->errors, (unsigned long long)stats(dec)->unknown_regs);
    uint32_t sw[NVDEC_STATUS_WORDS]; memcpy(sw, st.mem, sizeof sw);
    CHECK(sw[3] == 0 && sw[0] == 12 && sw[1] == 0 && sw[4] == 0 && sw[5] == 0 && sw[13] == 0, "nvdec_status_s: error 0, 12 macroblocks of 64x48, no HEVC/VP9 statistics for H.264 (%u %u)", sw[3], sw[0]);
    { MNvdecStatus ms; memcpy(&ms, st.mem, sizeof ms); CHECK(ms.mbs_correct == 12 && ms.error_status == 0, "nvdec_status_s round trip through the mirror"); }
    int flat = 1; for (int i = 1; i < 64 * 48; i++) flat &= luma.mem[i] == luma.mem[0];
    CHECK(flat && luma.mem[0] >= 32 && chroma.mem[0] == 128 && chroma.mem[64 * 24 - 1] == 128, "NV12 output written: luma %u, chroma 128", luma.mem[0]);
    uint8_t first = luma.mem[0];
    bs.mem[sizeof h264_64x48 - 1] ^= 0xff; /* a different frame content */
    prog[2] = DEC_CODEC_H264;
    CHECK(submit(dec, &cb, prog, sizeof prog / 4, NULL, 0) == NV_OK && luma.mem[0] != first && stats(dec)->frames == 2, "a different bitstream gives a different picture");
    memset(luma.mem, 0, 0x4000);

    /* high profile / cropping sizes come from the SPS */
    memset(bs.mem, 0, 0x2000); memcpy(bs.mem, h264_50x30_crop, sizeof h264_50x30_crop); len = sizeof h264_50x30_crop; memcpy(pic.mem + 18 * 4, &len, 4);
    submit(dec, &cb, prog, sizeof prog / 4, NULL, 0); memcpy(sw, st.mem, sizeof sw);
    CHECK(sw[3] == 0 && sw[0] == 4 * 2, "cropped 50x30 H.264 stream: 4x2 macroblocks");

    /* VP9: key frame sizes the surface, the following inter frame reuses it */
    uint32_t vp9prog[sizeof prog / 4]; memcpy(vp9prog, prog, sizeof prog); vp9prog[2] = DEC_CODEC_VP9;
    memset(bs.mem, 0, 0x2000); memcpy(bs.mem, vp9_key_70x38, sizeof vp9_key_70x38); len = sizeof vp9_key_70x38; memset(pic.mem, 0, 0x100); memcpy(pic.mem + 12 * 4, &len, 4);
    CHECK(submit(dec, &cb, vp9prog, sizeof vp9prog / 4, NULL, 0) == NV_OK, "VP9 key frame"); memcpy(sw, st.mem, sizeof sw);
    CHECK(sw[3] == 0 && sw[0] == 5 * 3 && sw[NVDEC_STATUS_W_INTRA_CNT] == 9 * 5 && sw[NVDEC_STATUS_W_INTER_CNT] == 0, "VP9 key frame 70x38: 15 MBs, 45 intra 8x8 blocks (%u, %u)", sw[0], sw[4]);
    memset(bs.mem, 0, 0x2000); memcpy(bs.mem, vp9_inter, sizeof vp9_inter); len = sizeof vp9_inter; memcpy(pic.mem + 12 * 4, &len, 4);
    memset(st.mem, 0, 0x40); submit(dec, &cb, vp9prog, sizeof vp9prog / 4, NULL, 0); memcpy(sw, st.mem, sizeof sw);
    CHECK(sw[3] == 0 && sw[0] == 15 && sw[NVDEC_STATUS_W_INTER_CNT] == 45 && sw[NVDEC_STATUS_W_INTRA_CNT] == 0, "VP9 inter frame reuses the size: 45 inter blocks");
    /* H.265 */
    uint32_t hprog[sizeof prog / 4]; memcpy(hprog, prog, sizeof prog); hprog[2] = DEC_CODEC_H265;
    memset(bs.mem, 0, 0x2000); memcpy(bs.mem, h265_96x64, sizeof h265_96x64); len = sizeof h265_96x64; memset(pic.mem, 0, 0x100); memcpy(pic.mem + 12 * 4, &len, 4);
    submit(dec, &cb, hprog, sizeof hprog / 4, NULL, 0); memcpy(sw, st.mem, sizeof sw);
    CHECK(sw[3] == 0 && sw[0] == 6 * 4 && sw[NVDEC_STATUS_W_INTRA_CNT] == 12 * 8, "H.265 stream 96x64 from the SPS: 24 MBs, 96 intra blocks (%u, %u)", sw[0], sw[4]);
    /* unsupported codec: error in the status, submit still completes */
    uint32_t badcodec[sizeof prog / 4]; memcpy(badcodec, prog, sizeof prog); badcodec[2] = 77;
    uint64_t errs = stats(dec)->errors;
    CHECK(submit(dec, &cb, badcodec, sizeof badcodec / 4, NULL, 0) == NV_OK && stats(dec)->errors == errs + 1, "unsupported codec: counted, submit completes"); memcpy(sw, st.mem, sizeof sw);
    CHECK(sw[3] == 1, "unsupported codec reported in the status structure");

    /* picture slots: output goes to the slot's surfaces when set */
    uint32_t slot[] = {SETCL(MEDIA_CLASS_NVDEC), NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_H264, INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), pic.iova >> 8, bs.iova >> 8, 2,
                       INCR(NVDEC_SET_STATUS_OFFSET, 1), st.iova >> 8, INCR(NVDEC_SET_PICTURE_LUMA_OFFSET0 + 2, 1), (luma.iova + 0x1000) >> 8, INCR(NVDEC_SET_PICTURE_CHROMA_OFFSET0 + 2, 1), (chroma.iova + 0x1000) >> 8,
                       IMM(NVM_EXECUTE, 0)};
    memset(bs.mem, 0, 0x2000); memcpy(bs.mem, h264_64x48, sizeof h264_64x48); len = sizeof h264_64x48; memset(pic.mem, 0, 0x100); memcpy(pic.mem + 18 * 4, &len, 4);
    uint32_t cur2 = 2u << 2; memcpy(pic.mem + NVDEC_H264_W_FLAGS2 * 4, &cur2, 4); /* curr_pic_idx 2 */
    memset(luma.mem, 0, 0x4000);
    submit(dec, &cb, slot, sizeof slot / 4, NULL, 0);
    CHECK(luma.mem[0] == 0 && luma.mem[0x1000] != 0 && chroma.mem[0x1000] == 128, "curr_pic_idx 2 writes the slot-2 surfaces");
    { /* the frame register (0x103) counts past the 17 slots (MK8D): the slot still comes from curr_pic_idx */
        uint32_t past[sizeof slot / 4]; memcpy(past, slot, sizeof slot); past[6] = 20; memset(luma.mem, 0, 0x4000); errs = stats(dec)->errors;
        CHECK(submit(dec, &cb, past, sizeof past / 4, NULL, 0) == NV_OK && stats(dec)->errors == errs && luma.mem[0x1000] != 0, "frame register 20, curr_pic_idx 2: slot 2, no error");
    }
    memset(pic.mem + NVDEC_H264_W_FLAGS2 * 4, 0, 4);

    /* unmapped output surface: error in status, no crash */
    uint32_t unmapped[sizeof prog / 4]; memcpy(unmapped, prog, sizeof prog); unmapped[10] = 0x7fff0000u >> 8; errs = stats(dec)->errors;
    CHECK(submit(dec, &cb, unmapped, sizeof unmapped / 4, NULL, 0) == NV_OK && stats(dec)->errors == errs + 1, "unmapped output surface: counted as an engine error"); memcpy(sw, st.mem, sizeof sw);
    CHECK(sw[3] == 2, "unmapped surface reported in the status (2)");

    /* relocation: the input-buffer word is patched from a (handle, offset, shift) entry */
    uint32_t rprog[sizeof prog / 4]; memcpy(rprog, prog, sizeof prog); rprog[5] = 0xdeadbeef; /* in-buf offset placeholder at word 5 */
    memset(luma.mem, 0, 0x4000);
    RelocSpec rl = {5 * 4, bs.handle, 0, 8};
    CHECK(submit(dec, &cb, rprog, sizeof rprog / 4, &rl, 1) == NV_OK && luma.mem[0] != 0, "relocation resolves the bitstream address");
    RelocSpec rl_bad = {5 * 4, 0x4321, 0, 8};
    CHECK(submit(dec, &cb, rprog, sizeof rprog / 4, &rl_bad, 1) == NV_BAD_PARAMETER, "relocation to an unknown handle fails the submit");
    RelocSpec rl_off = {5 * 4, bs.handle, 0x100, 8}; memset(luma.mem, 0, 0x4000);
    submit(dec, &cb, rprog, sizeof rprog / 4, &rl_off, 1); CHECK(media_engine_reg(fds[dec].ch.eng, NVDEC_SET_IN_BUF_BASE_OFFSET) == (bs.iova + 0x100) >> 8, "relocation adds the target offset and shifts");

    /* GATHER: commands living in another buffer; register gather: data words for one register */
    Buf g = mkbuf(dec, 0x1000);
    uint32_t inner[] = {NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_H264, INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), pic.iova >> 8, bs.iova >> 8, 0,
                        INCR(NVDEC_SET_STATUS_OFFSET, 1), st.iova >> 8, INCR(NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET, 2), luma.iova >> 8, chroma.iova >> 8, IMM(NVM_EXECUTE, 0)};
    memcpy(g.mem, inner, sizeof inner);
    uint32_t gprog[] = {SETCL(MEDIA_CLASS_NVDEC), GATHER(sizeof inner / 4), g.iova};
    memset(luma.mem, 0, 0x4000); memcpy(bs.mem, h264_64x48, sizeof h264_64x48); memcpy(pic.mem + 18 * 4, &(uint32_t){sizeof h264_64x48}, 4);
    CHECK(submit(dec, &cb, gprog, 3, NULL, 0) == NV_OK && luma.mem[0] != 0, "GATHER runs the commands of another buffer");
    uint32_t data[] = {0x11111, 0x22222};
    memcpy(g.mem + 0x800, data, 8);
    uint32_t rgprog[] = {SETCL(MEDIA_CLASS_NVDEC), GATHER_REG(NVDEC_SET_SLICE_OFFSETS_BUF_OFFSET, 1, 2), g.iova + 0x800};
    CHECK(submit(dec, &cb, rgprog, 3, NULL, 0) == NV_OK && media_engine_reg(fds[dec].ch.eng, NVDEC_SET_SLICE_OFFSETS_BUF_OFFSET) == 0x11111 && media_engine_reg(fds[dec].ch.eng, NVDEC_SET_COLOC_DATA_OFFSET) == 0x22222, "register GATHER writes consecutive registers");
    uint32_t ngprog[] = {SETCL(MEDIA_CLASS_NVDEC), GATHER(4), 0x1234000};
    CHECK(submit(dec, &cb, ngprog, 3, NULL, 0) == NV_BAD_PARAMETER, "GATHER of an unmapped address fails the submit");

    /* malformed streams */
    uint32_t trunc[] = {SETCL(MEDIA_CLASS_NVDEC), INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 5), 1, 2};
    CHECK(submit(dec, &cb, trunc, 4, NULL, 0) == NV_BAD_PARAMETER, "INCR running past the command buffer");
    uint32_t badop[] = {0x9u << 28};
    CHECK(submit(dec, &cb, badop, 1, NULL, 0) == NV_BAD_PARAMETER, "unknown host1x opcode");
    uint32_t unk[] = {SETCL(MEDIA_CLASS_NVDEC), IMM(0x1ee, 5), 0xe1000000u, SETCL(0), IMM(0, 1)}; uint64_t u0 = stats(dec)->unknown_regs;
    CHECK(submit(dec, &cb, unk, 5, NULL, 0) == NV_OK && stats(dec)->unknown_regs == u0 + 1 && stats(dec)->last_unknown_reg == 0x1ee && stats(dec)->syncpt_incrs >= 1, "unknown registers are counted and logged, EXTEND and host1x INCR_SYNCPT accepted");
    uint32_t thi_incr[] = {SETCL(MEDIA_CLASS_NVDEC), IMM(0, 0x105), IMM(0, 0x105)}; uint64_t u1 = stats(dec)->unknown_regs, s1 = stats(dec)->syncpt_incrs;
    CHECK(submit(dec, &cb, thi_incr, 3, NULL, 0) == NV_OK && stats(dec)->unknown_regs == u1 && stats(dec)->syncpt_incrs == s1 + 2,
          "the engine class's THI word 0 is INCR_SYNCPT, not an unknown register (as MK8D's submits end)");
    uint32_t mk8d_regs[] = {SETCL(MEDIA_CLASS_NVDEC), IMM(NVDEC_H264_SET_MBHIST_BUF_OFFSET, 0x210), IMM(NVDEC_SET_HISTOGRAM_OFFSET, 1), IMM(NVDEC_SET_PIC_SCRATCH_BUF_OFFSET, 2),
                            IMM(NVDEC_VP8_SET_PROB_DATA_OFFSET, 3), IMM(NVDEC_VP9_SET_PROB_TAB_BUF_OFFSET + 3, 4)}; uint64_t u2 = stats(dec)->unknown_regs;
    CHECK(submit(dec, &cb, mk8d_regs, 6, NULL, 0) == NV_OK && stats(dec)->unknown_regs == u2, "NVDEC 0x140 (H264_SET_MBHIST_BUF_OFFSET, MK8D writes it), histogram, scratch, VP8 / VP9 buffers are known");
    uint32_t not_nvdec[] = {SETCL(MEDIA_CLASS_NVDEC), IMM(0x130, 1)}; u2 = stats(dec)->unknown_regs;
    CHECK(submit(dec, &cb, not_nvdec, 2, NULL, 0) == NV_OK && stats(dec)->unknown_regs == u2 + 1, "NVDEC 0x130 (the old invented display-buffer word) is not a method");
    uint32_t vic_ctl[] = {SETCL(MEDIA_CLASS_VIC), IMM(VIC_SET_CONTROL_PARAMS, 0x110)}; uint64_t u3 = stats(vic)->unknown_regs;
    CHECK(submit(vic, &cb, vic_ctl, 2, NULL, 0) == NV_OK && stats(vic)->unknown_regs == u3, "VIC SetControlParams (0x1c1) is a known register");
    uint32_t mask[] = {SETCL(MEDIA_CLASS_NVDEC), (3u << 28) | (NVDEC_SET_DRV_PIC_SETUP_OFFSET << 16) | 0x5, 0xaaa, 0xbbb};
    submit(dec, &cb, mask, 4, NULL, 0);
    CHECK(media_engine_reg(fds[dec].ch.eng, NVDEC_SET_DRV_PIC_SETUP_OFFSET) == 0xaaa && media_engine_reg(fds[dec].ch.eng, NVDEC_SET_DRV_PIC_SETUP_OFFSET + 2) == 0xbbb, "MASK writes the set bits");
    uint32_t setcl_mask[] = {SETCL(MEDIA_CLASS_NVDEC) | (NVDEC_SET_HISTORY_OFFSET << 16) | 0x3, 0x31, 0x32};
    submit(dec, &cb, setcl_mask, 3, NULL, 0);
    CHECK(media_engine_reg(fds[dec].ch.eng, NVDEC_SET_HISTORY_OFFSET) == 0x31 && media_engine_reg(fds[dec].ch.eng, NVDEC_SET_HISTORY_OFFSET + 1) == 0x32, "SETCL with a register mask");
    uint32_t foreign[] = {SETCL(MEDIA_CLASS_VIC), IMM(NVM_EXECUTE, 0)}; u0 = stats(dec)->executes;
    submit(dec, &cb, foreign, 2, NULL, 0); CHECK(stats(dec)->executes == u0 && stats(dec)->foreign_writes > 0, "writes to another engine's class do not run this engine");

    /* the recording backend sees the request the engine built */
    decoder_backend_install(&rec_backend);
    uint32_t dec2 = open_("/dev/nvhost-nvdec");
    Buf cb2 = mkbuf(dec2, 0x1000);
    memcpy(bs.mem, h264_64x48, sizeof h264_64x48); memset(pic.mem, 0, 0x100); memcpy(pic.mem, "PICINFO!", 8); len = 100; memcpy(pic.mem + 18 * 4, &len, 4);
    { uint32_t c4 = 4u << 2; memcpy(pic.mem + NVDEC_H264_W_FLAGS2 * 4, &c4, 4); } /* curr_pic_idx 4 */
    uint32_t p2[] = {SETCL(MEDIA_CLASS_NVDEC), NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_H264, INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), pic.iova >> 8, bs.iova >> 8, 4,
                     INCR(NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET, 2), luma.iova >> 8, chroma.iova >> 8, IMM(NVM_EXECUTE, 0)};
    CHECK(submit(dec2, &cb2, p2, sizeof p2 / 4, NULL, 0) == NV_OK, "submit with the recording backend");
    CHECK(rec.opened == 1 && rec.decoded == 1 && rec.codec == DEC_CODEC_H264 && rec.last.bitstream_size == 100 && rec.last.picture_index == 4 &&
              rec.last.pic_size >= 0x1000 && !memcmp(rec.pic_head, "PICINFO!", 8) && rec.last.bitstream == bs.mem && rec.last.luma == luma.mem && rec.last.luma_cap >= 0x4000,
          "backend request: codec %d, stream %zu bytes, picture %u, surfaces mapped", rec.codec, rec.last.bitstream_size, rec.last.picture_index);
    CHECK(luma.mem[0] == 0x55 && chroma.mem[0] == 0x66, "the backend's output lands in the guest surfaces");
    uint32_t p3[sizeof p2 / 4]; memcpy(p3, p2, sizeof p2); p3[2] = DEC_CODEC_VP9; submit(dec2, &cb2, p3, sizeof p3 / 4, NULL, 0);
    CHECK(rec.opened == 2 && rec.closed == 1, "a codec change reopens the decoder");
    close_(dec2); CHECK(rec.closed == 2, "closing the channel closes the backend decoder");
    decoder_backend_install(NULL);

    /* the configure hand-off: parameter sets found in the stream reach the backend once per change, reset first; a refusal stops the decode */
    {
        memset(&rec, 0, sizeof rec); decoder_backend_install(&rec_backend);
        uint32_t d3 = open_("/dev/nvhost-nvdec"); Buf cb3 = mkbuf(d3, 0x1000);
        memcpy(bs.mem, h264_64x48, sizeof h264_64x48); len = sizeof h264_64x48; memcpy(pic.mem + 18 * 4, &len, 4);
        uint32_t p4[] = {SETCL(MEDIA_CLASS_NVDEC), NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_H264, INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), pic.iova >> 8, bs.iova >> 8, 0,
                         INCR(NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET, 2), luma.iova >> 8, chroma.iova >> 8, INCR(NVDEC_SET_STATUS_OFFSET, 1), st.iova >> 8, IMM(NVM_EXECUTE, 0)};
        submit(d3, &cb3, p4, sizeof p4 / 4, NULL, 0);
        CHECK(rec.configures == 1 && rec.resets == 1 && rec.decoded == 1 && stats(d3)->configures == 1, "H.264: parameter sets configure the backend before the first decode (%d configures, %d resets)", rec.configures, rec.resets);
        CHECK(rec.cfg.codec == DEC_CODEC_H264 && rec.cfg.h264.nsps == 1 && rec.cfg.h264.npps == 1 && rec.cfg.width == 64 && rec.cfg.height == 48 && rec.cfg.h264.sps[0][0] == 0x67 &&
              rec.cfg.h264_annexb_size == 8 + 22 + 5 && !memcmp(rec.cfg.h264_annexb + 4, h264_64x48 + 4, 22), "H.264 config: one SPS, one PPS, 64x48, Annex B form for vth264_config");
        submit(d3, &cb3, p4, sizeof p4 / 4, NULL, 0);
        CHECK(rec.configures == 1 && rec.decoded == 2, "the same parameter sets again: no new configure");
        memcpy(bs.mem, h264_208x112_high, sizeof h264_208x112_high); len = sizeof h264_208x112_high; memcpy(pic.mem + 18 * 4, &len, 4);
        submit(d3, &cb3, p4, sizeof p4 / 4, NULL, 0);
        CHECK(rec.configures == 2 && rec.resets == 2 && rec.cfg.width == 208 && rec.cfg.height == 112, "new parameter sets: reset then configure again (%dx%d)", rec.cfg.width, rec.cfg.height);
        memcpy(bs.mem, h264_64x48 + 30, 64); len = 64; memcpy(pic.mem + 18 * 4, &len, 4); /* slices only: nothing to hand over */
        submit(d3, &cb3, p4, sizeof p4 / 4, NULL, 0);
        CHECK(rec.configures == 2 && rec.decoded == 4, "a stream without parameter sets keeps the backend's configuration");
        rec.refuse = 1; memcpy(bs.mem, h264_50x30_crop, sizeof h264_50x30_crop); len = sizeof h264_50x30_crop; memcpy(pic.mem + 18 * 4, &len, 4); int dec_before = rec.decoded; memset(st.mem, 0, 32);
        submit(d3, &cb3, p4, sizeof p4 / 4, NULL, 0); uint32_t stw[NVDEC_STATUS_WORDS]; memcpy(stw, st.mem, sizeof stw);
        CHECK(rec.configures == 3 && rec.decoded == dec_before && stw[3] == 4 && stats(d3)->errors > 0, "a refused configure stops the decode and reports error 4 (status word %u)", stw[3]);
        rec.refuse = 0;
        /* VP9: a key frame configures with a vpcC record, an inter frame does not */
        uint32_t p5[sizeof p4 / 4]; memcpy(p5, p4, sizeof p4); p5[2] = DEC_CODEC_VP9;
        memset(bs.mem, 0, 0x2000); memcpy(bs.mem, vp9_key_70x38, sizeof vp9_key_70x38); len = sizeof vp9_key_70x38; memset(pic.mem, 0, 0x100); memcpy(pic.mem + 12 * 4, &len, 4);
        int c0 = rec.configures; submit(d3, &cb3, p5, sizeof p5 / 4, NULL, 0);
        CHECK(rec.configures == c0 + 1 && rec.cfg.codec == DEC_CODEC_VP9 && rec.cfg.width == 70 && rec.cfg.height == 38 && rec.cfg.vpcc_size == VP9_VPCC_SIZE && rec.cfg.vpcc[0] == 1 && rec.cfg.vpcc[5] == 10,
              "VP9 key frame: configure with %dx%d and a vpcC record", rec.cfg.width, rec.cfg.height);
        memset(bs.mem, 0, 0x2000); memcpy(bs.mem, vp9_inter, sizeof vp9_inter); len = sizeof vp9_inter; memcpy(pic.mem + 12 * 4, &len, 4);
        submit(d3, &cb3, p5, sizeof p5 / 4, NULL, 0);
        CHECK(rec.configures == c0 + 1, "VP9 inter frame: no configure");
        /* the SDK's way (MK8D, 2026-10-27): every register goes through the THI method FIFO (INCR 0x10 count 2: method, data) and the H.264 sequence lives in the
         * picture-info structure; the stream carries only slice NAL units, so the sets are synthesised from the structure */
        {
            memset(&rec, 0, sizeof rec);
            uint32_t pw[NVDEC_H264_PIC_WORDS] = {0};
            pw[18] = 6; pw[19] = 1; pw[22] = 1; pw[24] = 1; pw[25] = 32; pw[26] = 38; pw[28] = 1; pw[30] = 4; pw[32] = 1; pw[35] = pw[36] = 0x200;
            pw[44] = 0x16u | (1u << 12) | (0x3fu << 16) | (28u << 22) | (28u << 27); /* direct_8x8, weighted_pred, 4:2:0, qp -1, chroma offsets -4 */
            pw[45] = 2 | (1u << 2); /* weighted_bipred 2, curr_pic_idx 1 */
            memcpy(pic.mem, pw, sizeof pw);
            static const uint8_t slice[6] = {0, 0, 1, 0x65, 0x88, 0x84};
            memset(bs.mem, 0, 0x2000); memcpy(bs.mem, slice, sizeof slice);
#define THI(reg, val) INCR(NVM_THI_METHOD0, 2), (reg), (val)
            uint32_t p6[] = {SETCL(MEDIA_CLASS_NVDEC), THI(NVM_SET_APPLICATION_ID, DEC_CODEC_H264), THI(NVDEC_SET_DRV_PIC_SETUP_OFFSET, pic.iova >> 8), THI(NVDEC_SET_IN_BUF_BASE_OFFSET, bs.iova >> 8),
                             THI(NVDEC_SET_PICTURE_INDEX, 1), THI(NVDEC_SET_PICTURE_LUMA_OFFSET0 + 1, luma.iova >> 8), THI(NVDEC_SET_PICTURE_CHROMA_OFFSET0 + 1, chroma.iova >> 8),
                             THI(NVDEC_SET_STATUS_OFFSET, st.iova >> 8), THI(NVM_EXECUTE, 0)};
#undef THI
            submit(d3, &cb3, p6, sizeof p6 / 4, NULL, 0);
            H264Sps sps; H264Pps pps;
            CHECK(stats(d3)->thi_pairs == 8 && rec.configures == 1 && rec.decoded == 1 && rec.last.picture_index == 1, "THI pairs reach the registers: %llu pairs, configured %d, decoded %d", (unsigned long long)stats(d3)->thi_pairs, rec.configures, rec.decoded);
            CHECK(rec.cfg.width == 512 && rec.cfg.height == 608 && rec.cfg.h264.nsps == 1 && rec.cfg.h264.npps == 1 && !h264_parse_sps(rec.cfg.h264.sps[0] + 1, rec.cfg.h264.sps_size[0] - 1, &sps) && sps.width == 512 && sps.height == 608,
                  "H.264 without in-band sets: SPS synthesised from the picture-info structure (%dx%d)", rec.cfg.width, rec.cfg.height);
            CHECK(sps.num_ref_frames == 15 && sps.level_idc == 32, "synthesised SPS: 15 reference frames (16 leaves VideoToolbox no slot for a B picture), level %d holds 16 frames of 1216 MBs",
                  sps.level_idc);
            CHECK(!h264_parse_pps(rec.cfg.h264.pps[0] + 1, rec.cfg.h264.pps_size[0] - 1, &pps) && pps.cabac == 1 && pps.num_ref_idx_l0 == 5 && pps.weighted_pred == 1 && pps.weighted_bipred_idc == 2 && pps.pic_init_qp == 25 && pps.chroma_qp_index_offset == -4 && pps.deblocking_filter_control == 1,
                  "synthesised PPS: CABAC, 5 refs, weighted pred/bipred 2, QP 25, chroma offset %d", pps.chroma_qp_index_offset);
            submit(d3, &cb3, p6, sizeof p6 / 4, NULL, 0);
            CHECK(rec.configures == 1 && rec.decoded == 2, "the same structure again: no new configure");
            pw[25] = 20; memcpy(pic.mem, pw, sizeof pw); submit(d3, &cb3, p6, sizeof p6 / 4, NULL, 0);
            CHECK(rec.configures == 2 && rec.cfg.width == 320, "a changed picture size reconfigures (%d)", rec.cfg.width);
        }
        close_(d3);
        CHECK(stats(dec)->configures >= 1 && stats(dec)->frames > 0, "the null backend takes the configure hand-off too (%llu configures on the first channel)", (unsigned long long)stats(dec)->configures);
        /* contract checks of install */
        DecoderBackend bad = rec_backend; bad.abi = 2; CHECK(decoder_backend_install(&bad) == -1 && decoder_backend_current() == &rec_backend, "install refuses a backend with another ABI and keeps the current one");
        bad = rec_backend; bad.decode = NULL; CHECK(decoder_backend_install(&bad) == -1, "install refuses a backend without decode");
        bad = rec_backend; bad.configure = NULL; bad.reset = NULL; CHECK(decoder_backend_install(&bad) == 0, "configure and reset are optional");
        CHECK(decoder_backend_install(NULL) == 0 && decoder_backend_current() == &decoder_backend_null && decoder_backend_null.abi == DECODER_BACKEND_ABI, "NULL restores the null backend");
    }

    /* ---- VIC ---- */
    Buf vcb = mkbuf(vic, 0x1000), cfg = mkbuf(vic, 0x1000), yin = mkbuf(vic, 0x8000), cin = mkbuf(vic, 0x4000), out = mkbuf(vic, 0x10000), ochroma = mkbuf(vic, 0x8000);
    uint32_t vprog[] = {SETCL(MEDIA_CLASS_VIC), INCR(VIC_SET_CONTROL_PARAMS, 2), 0x610110, cfg.iova >> 8, INCR(VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET, 2), out.iova >> 8, ochroma.iova >> 8,
                        INCR(VIC_SET_INPUT0_LUMA_OFFSET, 2), yin.iova >> 8, cin.iova >> 8, IMM(NVM_EXECUTE, 0)};
    vic_fill(&yin, &cin, 16, 8, 256, 81, 90, 240); /* BT.601 limited red: Y 81, U 90, V 240 (input rows 256-aligned, as NVDEC writes them) */
    vic_cfg(&cfg, VIC_FMT_RGBA8, 0, 0, 16, 8);
    CHECK(submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0) == NV_OK && stats(vic)->frames == 1 && stats(vic)->errors == 0, "VIC submit (control word at 0x1c1, config at 0x1c2)");
    CHECK(out.mem[0] >= 250 && out.mem[1] <= 6 && out.mem[2] <= 6 && out.mem[3] == 255, "NV12 red -> RGBA8 bytes R,G,B,A = %u %u %u %u", out.mem[0], out.mem[1], out.mem[2], out.mem[3]);
    CHECK(out.mem[16 * 4 * 7 + 15 * 4] >= 250 && out.mem[16 * 4 * 8] == 0, "RGBA8 pitch linear: rows of width*4, last pixel written, nothing after");
    vic_cfg(&cfg, VIC_FMT_BGRA8, 0, 0, 16, 8); memset(out.mem, 0, 0x10000); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
    CHECK(out.mem[2] >= 250 && out.mem[0] <= 6 && out.mem[3] == 255, "BGRA8: red in byte 2");
    vic_fill(&yin, &cin, 16, 8, 256, 235, 128, 128); vic_cfg(&cfg, VIC_FMT_RGBX8, 0, 0, 16, 8); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
    CHECK(out.mem[0] == 255 && out.mem[1] == 255 && out.mem[2] == 255 && out.mem[3] == 255, "limited-range white (Y 235) -> 255, RGBX alpha 255");
    vic_fill(&yin, &cin, 16, 8, 16, 16, 128, 128); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
    CHECK(out.mem[0] == 0 && out.mem[1] == 0 && out.mem[2] == 0, "packed input rows (surface too small for 256-aligned rows) are read packed; black Y 16 -> 0");
    { /* NV12 out: luma and chroma at 256-aligned rows, pitch linear */
        for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) yin.mem[y * 256 + x] = (uint8_t)(x + 16 * y);
        for (int y = 0; y < 4; y++) for (int x = 0; x < 16; x++) cin.mem[y * 256 + x] = (uint8_t)(200 + x);
        vic_cfg(&cfg, VIC_FMT_NV12, 0, 0, 16, 8); memset(out.mem, 0, 0x10000); memset(ochroma.mem, 0, 0x8000); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
        CHECK(out.mem[0] == 0 && out.mem[15] == 15 && out.mem[256 + 3] == 19 && out.mem[256 * 7 + 15] == 127 && out.mem[16] == 0 && ochroma.mem[256 * 3 + 5] == 205,
              "NV12 output, pitch 256: luma row 1 at 256, chroma row 3 at 768");
    }
    { /* block linear: NV12 128x32, 2 GOBs per block. GOB layout: (16,0)->32 (0,1)->16 (0,2)->64 (32,0)->256 (0,8)->512; next block (64,0)->1024; next block row (0,16)->2048 */
        for (int y = 0; y < 32; y++) for (int x = 0; x < 128; x++) yin.mem[y * 256 + x] = (uint8_t)(x + 3 * y + 1);
        vic_cfg(&cfg, VIC_FMT_NV12, 1, 1, 128, 32); memset(out.mem, 0, 0x10000); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
#define LY(x, y) (uint8_t)((x) + 3 * (y) + 1)
        CHECK(out.mem[0] == LY(0, 0) && out.mem[32] == LY(16, 0) && out.mem[16] == LY(0, 1) && out.mem[64] == LY(0, 2) && out.mem[256] == LY(32, 0) && out.mem[512] == LY(0, 8) &&
              out.mem[1024] == LY(64, 0) && out.mem[2048] == LY(0, 16) && out.mem[2048 + 1024 + 512 + 256 + 64 + 32 + 16 + 5] == LY(64 + 32 + 16 + 5, 16 + 8 + 2 + 1),
              "block-linear NV12 luma lands at the GOB offsets (%u %u %u %u)", out.mem[32], out.mem[16], out.mem[1024], out.mem[2048]);
#undef LY
        vic_fill(&yin, &cin, 32, 16, 256, 81, 90, 240); vic_cfg(&cfg, VIC_FMT_RGBA8, 1, 0, 32, 16); memset(out.mem, 0, 0x10000); submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0);
        CHECK(out.mem[0] >= 250 && out.mem[3] == 255 && out.mem[512 * 2 + 3] == 255 && out.mem[512 * 4 - 1] == 255 && out.mem[512 * 4] == 0,
              "block-linear RGBA8 32x16 (1 GOB high blocks): 4 GOBs written, nothing after");
    }
    errs = stats(vic)->errors;
    vic_cfg(&cfg, 0x05, 0, 0, 16, 8); CHECK(submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0) == NV_OK && stats(vic)->errors == errs + 1, "unknown output format: counted error");
    vic_cfg(&cfg, VIC_FMT_RGBA8, 0, 0, 160, 128); CHECK(submit(vic, &vcb, vprog, sizeof vprog / 4, NULL, 0) == NV_OK && stats(vic)->errors == errs + 2, "output surface too small: counted error");
    { /* no readable config structure (0x1c2 not written or unmapped): the execute copies NV12 to the NV12 output surfaces */
        for (int i = 0; i < 16 * 8; i++) yin.mem[i] = (uint8_t)(i + 1);
        memset(cin.mem, 77, 16 * 4); memset(out.mem, 0, 0x100); memset(ochroma.mem, 0, 64);
        uint32_t nprog[] = {SETCL(MEDIA_CLASS_VIC), INCR(VIC_SET_CONTROL_PARAMS, 2), 0x610110, 0, INCR(VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET, 2), out.iova >> 8, ochroma.iova >> 8,
                            INCR(VIC_SET_INPUT0_LUMA_OFFSET, 2), yin.iova >> 8, cin.iova >> 8, IMM(NVM_EXECUTE, 0)};
        errs = stats(vic)->errors;
        CHECK(submit(vic, &vcb, nprog, sizeof nprog / 4, NULL, 0) == NV_OK && stats(vic)->errors == errs && stats(vic)->vic_copies == 1 && out.mem[0] == 1 && out.mem[100] == 101 && ochroma.mem[10] == 77,
              "VIC with no readable config structure: NV12 copy (%llu copies)", (unsigned long long)stats(vic)->vic_copies);
    }

    /* ---- NVJPG decode: cle7d0 methods, nvjpg_drv_pic_param_s, nvjpg_dec_status ---- */
    Buf jcb = mkbuf(jpg, 0x1000), jpic = mkbuf(jpg, 0x2000), jin = mkbuf(jpg, 0x8000), jout = mkbuf(jpg, 0x8000), jchroma = mkbuf(jpg, 0x4000), jv = mkbuf(jpg, 0x4000), jst = mkbuf(jpg, 0x100);
    int w = 40, h = 24; uint8_t *src = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t *p = src + ((size_t)y * w + x) * 4; p[0] = (uint8_t)(x * 6); p[1] = (uint8_t)(y * 10); p[2] = 90; p[3] = 255; }
    size_t jn; uint8_t *jfile = napjpeg_encode_rgba(src, w, h, w * 4, 92, 0, &jn); memcpy(jin.mem, jfile, jn);
    uint32_t jboff = (uint32_t)sos_data_offset(jfile, jn), jbsize = (uint32_t)jn - jboff;
    CHECK(jboff > 100 && jboff < jn, "the test JPEG's scan data starts at %u", jboff);
    MNvjpgPic jp; MNvjpgStatus js;
#define JDEC(...) do { nvjpg_dec_pic(&jp, __VA_ARGS__); memcpy(jpic.mem, &jp, sizeof jp); memset(jst.mem, 0xee, 0x20); } while (0)
    uint32_t jprog[] = {SETCL(MEDIA_CLASS_NVJPG), NONINCR(NVM_SET_APPLICATION_ID, 1), NVJPG_APP_DECODER,
                        INCR(NVJPG_SET_IN_DRV_PIC_SETUP, 6), jpic.iova >> 8, jst.iova >> 8, jin.iova >> 8, jout.iova >> 8, jchroma.iova >> 8, jv.iova >> 8, IMM(NVM_EXECUTE, 0)};
    JDEC(jboff, jbsize, NVJPG_CHROMA_444, NVJPG_OUT_RGBA, 0, 0, 0, 0, 0);
    CHECK(submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0) == NV_OK && stats(jpg)->frames == 1 && stats(jpg)->unknown_regs == 0, "NVJPG decode submit (methods 0x1c2..0x1c7 known)");
    memcpy(&js, jst.mem, sizeof js);
    CHECK(js.error_status == 0 && js.w0 == jbsize && js.mcu_x == 5 && js.mcu_y == 3 && js.cycle_count == 0, "nvjpg_dec_status: ok, %u bytes consumed, 5x3 MCUs of 8x8", js.w0);
    double pr = psnr(src, jout.mem, (size_t)w * h * 4); CHECK(pr > 32.0 && jout.mem[3] == 255, "RGBA output matches the source image (PSNR %.1f dB), alpha 255", pr);
    JDEC(jboff, jbsize, NVJPG_CHROMA_444, NVJPG_OUT_BGRA, 0, 0, 0, 0, 0); memset(jout.mem, 0, 0x8000); submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0);
    CHECK(abs((int)jout.mem[39 * 4 + 2] - 39 * 6) <= 8 && abs((int)jout.mem[39 * 4] - 90) <= 8, "BGRA: red in byte 2, blue in byte 0 (%u %u)", jout.mem[39 * 4 + 2], jout.mem[39 * 4]);
    JDEC(jboff, jbsize, NVJPG_CHROMA_444, NVJPG_OUT_ARGB, 0, 0, 0, 0, 0x80); memset(jout.mem, 0, 0x8000); submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0);
    CHECK(jout.mem[0] == 0x80 && abs((int)jout.mem[39 * 4 + 1] - 39 * 6) <= 8, "ARGB with alpha_val 0x80: alpha in byte 0, red in byte 1");
    JDEC(jboff, jbsize, NVJPG_CHROMA_444, NVJPG_OUT_ABGR, 0, 0, 0, 0, 0); memset(jout.mem, 0, 0x8000); submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0);
    CHECK(jout.mem[0] == 0xff && abs((int)jout.mem[39 * 4 + 3] - 39 * 6) <= 8, "ABGR: red in byte 3");
    JDEC(jboff, jbsize, NVJPG_CHROMA_444, NVJPG_OUT_RGBA, 1, 0, 128, 0, 0); memset(jout.mem, 0, 0x8000); submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0); memcpy(&js, jst.mem, sizeof js);
    CHECK(js.error_status == 0 && jout.mem[128 * 11 + 19 * 4 + 3] == 255 && jout.mem[128 * 11 + 20 * 4 + 3] == 0 && jout.mem[128 * 12] == 0, "power2_downscale 1: 20x12 written with stride 128");
    { /* YUV 4:2:0 planar (chroma_subsample 1): Y, U, V planes with the given strides */
        JDEC(jboff, jbsize, NVJPG_CHROMA_444, NVJPG_OUT_YUV, 0, 1, 48, 32, 0); memset(jout.mem, 0, 0x8000); memset(jchroma.mem, 0, 0x4000); memset(jv.mem, 0, 0x4000);
        submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0); memcpy(&js, jst.mem, sizeof js);
        uint8_t y0 = (uint8_t)(0.299f * (39 * 6) + 0.587f * 0 + 0.114f * 90 + 0.5f);
        CHECK(js.error_status == 0 && abs((int)jout.mem[39] - (int)y0) <= 6 && jout.mem[40] == 0 && jout.mem[48 + 3] != 0, "YUV planar: luma BT.601 of the source at stride 48 (%u vs %u)", jout.mem[39], y0);
        CHECK(jchroma.mem[19] != 0 && jchroma.mem[20] == 0 && jchroma.mem[32 * 11 + 19] != 0 && jv.mem[32 * 11 + 19] > 128 && jchroma.mem[32 * 12] == 0, "YUV planar 4:2:0: 20x12 U and V planes at stride 32, V > 128 for reddish pixels");
    }
    { /* semi-planar when no V surface is given */
        uint32_t sp[sizeof jprog / 4]; memcpy(sp, jprog, sizeof jprog); sp[9] = 0;
        JDEC(jboff, jbsize, NVJPG_CHROMA_444, NVJPG_OUT_YUV, 0, 1, 0, 64, 0); memset(jchroma.mem, 0, 0x4000);
        submit(jpg, &jcb, sp, sizeof sp / 4, NULL, 0); memcpy(&js, jst.mem, sizeof js);
        CHECK(js.error_status == 0 && jchroma.mem[38] != 0 && jchroma.mem[39] > 128 && jchroma.mem[40] == 0 && jchroma.mem[64 + 39] > 128, "YUV without a V surface: interleaved UV (40 bytes of a 64-byte row)");
    }
    JDEC(jboff, jbsize, NVJPG_CHROMA_444, NVJPG_OUT_YUY2, 0, 0, 0, 0, 0); memset(jout.mem, 0, 0x8000); submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0); memcpy(&js, jst.mem, sizeof js);
    CHECK(js.error_status == 0 && jout.mem[80 * 23 + 79] != 0 && jout.mem[80 * 24] == 0 && jout.mem[3] > 100, "YUY2: Y0 U Y1 V, 80 bytes a row");
    /* errors: no SOI in front of the scan, corrupt file, output too small, undefined output type, picture block too short */
    errs = stats(jpg)->errors;
    memset(jin.mem, 0x5a, 0x200); JDEC(0x10, 0x100, NVJPG_CHROMA_444, NVJPG_OUT_RGBA, 0, 0, 0, 0, 0);
    CHECK(submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0) == NV_OK && stats(jpg)->errors == errs + 1 && ((MNvjpgStatus *)jst.mem)->error_status == NVJPG_ERR_BAD_MAGIC,
          "a bitstream without SOI: BAD_MAGIC in the status");
    memcpy(jin.mem, jfile, jn); memset(jin.mem + jboff - 40, 0, 40); JDEC(jboff, jbsize, NVJPG_CHROMA_444, NVJPG_OUT_RGBA, 0, 0, 0, 0, 0);
    submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0);
    CHECK(((MNvjpgStatus *)jst.mem)->error_status == NVJPG_ERR_INVALID_INPUT, "a corrupt header: INVALID_INPUT");
    memcpy(jin.mem, jfile, jn); JDEC(jboff, jbsize, NVJPG_CHROMA_444, NVJPG_OUT_RGBA, 0, 0, 0x8000, 0, 0);
    submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0);
    CHECK(((MNvjpgStatus *)jst.mem)->error_status == NVJPG_ERR_INVALID_INPUT, "an output surface too small for the stride: INVALID_INPUT");
    JDEC(jboff, jbsize, NVJPG_CHROMA_444, 6, 0, 0, 0, 0, 0); submit(jpg, &jcb, jprog, sizeof jprog / 4, NULL, 0);
    CHECK(((MNvjpgStatus *)jst.mem)->error_status == NVJPG_ERR_INVALID_INPUT, "output type 6 is not defined: INVALID_INPUT");
    { Buf small = mkbuf(jpg, 0x100); uint32_t sp[sizeof jprog / 4]; memcpy(sp, jprog, sizeof jprog); sp[4] = small.iova >> 8;
      submit(jpg, &jcb, sp, sizeof sp / 4, NULL, 0);
      CHECK(((MNvjpgStatus *)jst.mem)->error_status == NVJPG_ERR_INVALID_INPUT, "a picture-parameter surface shorter than 0x1240 bytes: INVALID_INPUT"); }
    { uint32_t old[] = {SETCL(MEDIA_CLASS_NVJPG), IMM(0x101, 1)}; uint64_t u = stats(jpg)->unknown_regs;
      CHECK(submit(jpg, &jcb, old, 2, NULL, 0) == NV_OK && stats(jpg)->unknown_regs == u + 1, "NVJPG 0x101 (the old invented setup word) is not a method"); }
#undef JDEC
    free(src); free(jfile);

    /* ---- MSENC ---- */
    Buf ecb = mkbuf(enc, 0x1000), est = mkbuf(enc, 0x100); memset(est.mem, 0xff, MSENC_STAT_BYTES);
    uint32_t eprog[] = {SETCL(MEDIA_CLASS_MSENC), INCR(MSENC_SET_PICTURE_INDEX, 1), 7, INCR(MSENC_SET_OUT_ENC_STATUS, 1), est.iova >> 8, IMM(NVM_EXECUTE, 0)};
    { MNvencStat ns;
      CHECK(submit(enc, &ecb, eprog, 6, NULL, 0) == NV_OK && stats(enc)->executes == 1 && stats(enc)->unknown_regs == 0 && (memcpy(&ns, est.mem, sizeof ns), ns.picture_index == 7) &&
            ns.error == 0 && ns.total_bit_count == 0 && ns.num_slices == 0, "MSENC execute without a setup: accepted, nvenc_pic_stat_s reports picture 7 and no bitstream"); }
    { uint32_t old[] = {SETCL(MEDIA_CLASS_MSENC), IMM(0x120, 1), IMM(MSENC_SET_IN_REF_PIC0_LUMA + 9, 1)}; uint64_t u = stats(enc)->unknown_regs;
      CHECK(submit(enc, &ecb, old, 3, NULL, 0) == NV_OK && stats(enc)->unknown_regs == u + 1, "MSENC: 0x120 is not a method, 0x109 (the old invented status word) is IN_REF_PIC9_LUMA"); }

    { /* command table depth: only the undocumented command 13 stays a stub */
        int stubs = 0, rows = (int)(sizeof nv_cmds / sizeof *nv_cmds); unsigned stub_cmd = 0;
        for (int i = 0; i < rows; i++) if (nv_cmds[i].status == IPC_STUB) { stubs++; stub_cmd = nv_cmds[i].cmd; }
        CHECK(rows == 14 && stubs == 1 && stub_cmd == 13, "nvdrv command table: %d rows, %d stub (cmd %u)", rows, stubs, stub_cmd);
        memset(&call, 0, sizeof call); args[0] = 0; call.object = nv; call.in = (const uint8_t *)args; call.in_size = 8; nv_cmds[8].fn(nv, &call);
        CHECK(call.out_size == 0, "DumpGraphicsMemoryInfo replies with nothing");
        memset(&call, 0, sizeof call); call.object = nv; nv_cmds[6].fn(nv, &call);
        CHECK(call.out_size >= 0x20, "GetStatus replies with a 0x20-byte status");
    }
    /* ---- parameter sets for a VideoToolbox backend (codec_headers_core) ---- */
    {
        H264ParamSets ps; uint8_t ann[256], avcc[256], nal[128];
        CHECK(h264_find_param_sets(h264_64x48, sizeof h264_64x48, &ps) == 0 && ps.nsps == 1 && ps.npps == 1, "param sets: SPS and PPS found in an Annex B stream");
        CHECK(ps.sps[0][0] == 0x67 && ps.sps_size[0] == 22 && ps.pps[0][0] == 0x68 && ps.pps_size[0] == 5, "param sets: NAL units start at their header byte (%zu, %zu bytes)", ps.sps_size[0], ps.pps_size[0]);
        H264Sps sp; CHECK(h264_parse_sps(ps.sps[0] + 1, ps.sps_size[0] - 1, &sp) == 0 && sp.width == 64 && sp.height == 48, "param sets: the found SPS parses (%dx%d)", sp.width, sp.height);
        size_t an = h264_param_sets_annexb(&ps, ann, sizeof ann);
        H264Sps sp2; CHECK(an == 8 + ps.sps_size[0] + ps.pps_size[0] && !h264_find_sps(ann, an, &sp2) && sp2.width == 64, "param sets: Annex B form for vth264_config (%zu bytes)", an);
        CHECK(h264_param_sets_annexb(&ps, ann, 10) == 0, "param sets: too small a buffer gives 0");
        size_t ac = h264_param_sets_avcc(&ps, avcc, sizeof avcc);
        H264ParamSets back; CHECK(ac && h264_parse_avcc(avcc, ac, &back) == 0 && back.nal_length_size == 4 && back.nsps == 1 && back.npps == 1 &&
              back.sps_size[0] == ps.sps_size[0] && !memcmp(back.sps[0], ps.sps[0], ps.sps_size[0]) && !memcmp(back.pps[0], ps.pps[0], ps.pps_size[0]), "param sets: avcC round trip (%zu bytes)", ac);
        CHECK(avcc[1] == 66 && avcc[3] == 10 && avcc[4] == 0xff && avcc[5] == 0xe1, "param sets: avcC profile/level/length-size bytes");
        for (size_t cut = 0; cut < ac; cut += 3) { uint8_t t[256]; memcpy(t, avcc, cut); if (h264_parse_avcc(t, cut, &back) == 0) { CHECK(0, "truncated avcC (%zu of %zu) accepted", cut, ac); break; } }
        uint8_t bad[8] = {2, 0, 0, 0, 0xff, 0xe0, 0, 0}; CHECK(h264_parse_avcc(bad, 8, &back) == -1, "param sets: avcC with a wrong version refused");
        static const uint8_t only_sps[] = {0,0,0,1,0x67,66,192,10,218,17,236,4,64,0,0,3,0,64,0,0,3,2,131,196,137,168};
        CHECK(h264_find_param_sets(only_sps, sizeof only_sps, &ps) == 1 && ps.nsps == 1 && ps.npps == 0, "param sets: an SPS without a PPS is reported as incomplete (1)");
        CHECK(h264_find_param_sets(h265_96x64, sizeof h265_96x64, &ps) == -1, "param sets: a stream without any H.264 parameter sets: -1");
        H264Pps pp; CHECK(h264_parse_pps(h264_64x48 + 4 + 22 + 4 + 1, 4, &pp) == 0 && pp.pps_id == 0 && pp.sps_id == 0 && pp.cabac == 0 && pp.num_ref_idx_l0 == 1 && !pp.transform_8x8,
                           "PPS parse of the x264 vector: ids 0/0, CAVLC, one reference");
        /* writers: SPS and PPS built from fields come back through the parsers */
        static const struct { int w, h, refs; } sizes[] = {{64, 48, 1}, {1280, 720, 4}, {1920, 1080, 4}, {50, 30, 2}, {3840, 2160, 5}, {16, 16, 0}, {854, 480, 3}};
        for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
            H264SpsFields f; h264_sps_fields_default(&f, sizes[i].w, sizes[i].h, sizes[i].refs);
            size_t n = h264_build_sps(&f, nal, sizeof nal); H264Sps r;
            CHECK(n > 4 && nal[0] == 0x67 && !h264_parse_sps(nal + 1, n - 1, &r) && r.width == sizes[i].w && r.height == sizes[i].h && r.num_ref_frames == sizes[i].refs &&
                  r.profile_idc == 100 && r.chroma_format_idc == 1 && r.bit_depth == 8 && r.frame_mbs_only == 1 && r.poc_type == 2,
                  "built SPS %dx%d parses back (%zu bytes, %dx%d, level %d)", sizes[i].w, sizes[i].h, n, r.width, r.height, r.level_idc);
        }
        { H264SpsFields f; h264_sps_fields_default(&f, 1280, 720, 4); f.profile_idc = 66; f.poc_type = 0; f.log2_max_poc_lsb = 8; f.frame_mbs_only = 0; f.height_map_units = 23; f.mb_adaptive_frame_field = 1; f.crop_bottom = 0;
          size_t n = h264_build_sps(&f, nal, sizeof nal); H264Sps r;
          CHECK(n && !h264_parse_sps(nal + 1, n - 1, &r) && r.profile_idc == 66 && r.poc_type == 0 && r.frame_mbs_only == 0 && r.width == 1280 && r.height == 2 * 23 * 16 && r.chroma_format_idc == 1,
                "built SPS: Baseline profile, poc type 0, interlaced (%dx%d)", r.width, r.height);
          f.poc_type = 1; CHECK(h264_build_sps(&f, nal, sizeof nal) == 0, "built SPS: poc type 1 is not writable"); f.poc_type = 2; f.width_mbs = 0; CHECK(h264_build_sps(&f, nal, sizeof nal) == 0, "built SPS: zero width refused");
          h264_sps_fields_default(&f, 640, 480, 2); CHECK(h264_build_sps(&f, nal, 5) == 0, "built SPS: too small an output buffer gives 0"); }
        { H264SpsFields f; h264_sps_fields_default(&f, 1920, 1080, 4); f.profile_idc = 110; f.bit_depth = 10; f.chroma_format_idc = 2; f.crop_bottom = 8; /* 4:2:2 crop unit is one row */ size_t n = h264_build_sps(&f, nal, sizeof nal); H264Sps r;
          CHECK(n && !h264_parse_sps(nal + 1, n - 1, &r) && r.bit_depth == 10 && r.chroma_format_idc == 2 && r.width == 1920 && r.height == 1080, "built SPS: High 4:2:2 10-bit (%dx%d, depth %d)", r.width, r.height, r.bit_depth); }
        for (int ext = 0; ext < 2; ext++) {
            H264Pps in = {.pps_id = 3, .sps_id = 1, .cabac = 1, .bottom_field_pic_order = 0, .num_slice_groups = 1, .num_ref_idx_l0 = 4, .num_ref_idx_l1 = 2, .weighted_pred = 1, .weighted_bipred_idc = 2,
                          .pic_init_qp = 20, .pic_init_qs = 30, .chroma_qp_index_offset = -3, .deblocking_filter_control = 1, .constrained_intra_pred = 0, .redundant_pic_cnt = 0,
                          .transform_8x8 = ext, .second_chroma_qp_index_offset = ext ? 5 : -3};
            size_t n = h264_build_pps(&in, nal, sizeof nal); H264Pps out;
            CHECK(n && nal[0] == 0x68 && !h264_parse_pps(nal + 1, n - 1, &out) && out.pps_id == 3 && out.sps_id == 1 && out.cabac == 1 && out.num_ref_idx_l0 == 4 && out.num_ref_idx_l1 == 2 &&
                  out.weighted_pred == 1 && out.weighted_bipred_idc == 2 && out.pic_init_qp == 20 && out.pic_init_qs == 30 && out.chroma_qp_index_offset == -3 && out.deblocking_filter_control == 1 &&
                  out.transform_8x8 == ext && out.second_chroma_qp_index_offset == (ext ? 5 : -3), "built PPS (extension %d) parses back (%zu bytes)", ext, n);
        }
        { H264Pps bad_pps = {.num_slice_groups = 1, .num_ref_idx_l0 = 0, .num_ref_idx_l1 = 1}; CHECK(h264_build_pps(&bad_pps, nal, sizeof nal) == 0, "built PPS: zero reference count refused"); }
        { /* a built SPS + PPS pair makes a stream the null backend and the finder both accept */
            H264SpsFields f; h264_sps_fields_default(&f, 320, 240, 2); uint8_t st[128]; size_t at = 0;
            st[at++] = 0; st[at++] = 0; st[at++] = 0; st[at++] = 1; at += h264_build_sps(&f, st + at, sizeof st - at);
            H264Pps p = {.num_slice_groups = 1, .num_ref_idx_l0 = 1, .num_ref_idx_l1 = 1, .pic_init_qp = 26, .pic_init_qs = 26};
            st[at++] = 0; st[at++] = 0; st[at++] = 0; st[at++] = 1; at += h264_build_pps(&p, st + at, sizeof st - at);
            H264Sps r; CHECK(h264_find_param_sets(st, at, &ps) == 0 && !h264_find_sps(st, at, &r) && r.width == 320 && r.height == 240, "built SPS + PPS in one Annex B stream: found and parsed (%zu bytes)", at);
        }
        /* VP9: vpcC from a key frame */
        Vp9Header vh; uint8_t vpcc[VP9_VPCC_SIZE];
        CHECK(vp9_parse_header(vp9_key_70x38, sizeof vp9_key_70x38, &vh) == 0 && vh.key_frame && vh.width == 70 && vh.height == 38 && vh.subsampling_x == 1 && vh.subsampling_y == 1, "VP9 key frame: 4:2:0");
        CHECK(vp9_build_vpcc(&vh, vpcc) == VP9_VPCC_SIZE && vpcc[0] == 1 && vpcc[1] == 0 && vpcc[4] == 0 && vpcc[5] == 10 && (vpcc[6] >> 4) == 8 && ((vpcc[6] >> 1) & 7) == 1 && !vpcc[10] && !vpcc[11],
              "vpcC: version 1, profile 0, level 1.0, 8-bit 4:2:0 (%02x %02x %02x)", vpcc[4], vpcc[5], vpcc[6]);
        Vp9Header inter; vp9_parse_header(vp9_inter, sizeof vp9_inter, &inter);
        CHECK(vp9_build_vpcc(&inter, vpcc) == 0, "vpcC: an inter frame has no size, no record");
        CHECK(vp9_level_for(1280, 720) == 31 && vp9_level_for(1920, 1080) == 40 && vp9_level_for(3840, 2160) == 50 && vp9_level_for(64, 64) == 10, "VP9 levels by picture size");
    }

    /* ---- /dev/nvdisp-disp0: the tegra_dc_ext surface (nvdrv_media_core.h; layouts pinned in layout_pins) ---- */
    {
        NvDisp d; uint8_t b[NVDISP_SZ_CMU]; uint64_t T = 5000000000ull;
        nvdisp_init(&d, T, 0);
        CHECK(d.width == 1280 && d.height == 720 && d.refresh_mhz == 60000 && d.winmask == 1 && d.nvmap_fd == -1, "disp core: handheld 1280x720 at 60 Hz, window A on the head");
        CHECK(nvdisp_vblank_count(&d, T) == 0 && nvdisp_vblank_count(&d, T + 1000000000ull) == 60 && nvdisp_vblank_count(&d, T + 500000000ull) == 30, "disp core: 60 vblanks per second");
        CHECK(nvdisp_vblank_count(&d, T - 1) == 0, "disp core: a clock before the start reads 0");
        int32_t fd = 9; memcpy(b, &fd, 4);
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_NVMAP_FD, b, 4, T) == NVDISP_OK && d.nvmap_fd == 9, "SET_NVMAP_FD (0x00)");
        uint32_t n = 1; memcpy(b, &n, 4);
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_WINDOW, b, 4, T) == NVDISP_OK && d.windows == 2, "GET_WINDOW 1 claims window B");
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_WINDOW, b, 4, T) == NVDISP_INVALID_STATE, "GET_WINDOW of a held window is refused (busy)");
        CHECK(nvdisp_ioctl(&d, NVDISP_PUT_WINDOW, b, 4, T) == NVDISP_OK && d.windows == 0 && nvdisp_ioctl(&d, NVDISP_PUT_WINDOW, b, 4, T) == NVDISP_INVALID_STATE, "PUT_WINDOW releases it once");
        n = 3; memcpy(b, &n, 4);
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_WINDOW, b, 4, T) == NVDISP_BAD_PARAMETER, "a head has three windows: index 3 is refused");
        MDcCursor cur = {-5, 40, NVDISP_CURSOR_VISIBLE};
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_CURSOR, (uint8_t *)&cur, sizeof cur, T) == NVDISP_INVALID_STATE, "SET_CURSOR without GET_CURSOR is refused");
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_CURSOR, b, 0, T) == NVDISP_OK && nvdisp_ioctl(&d, NVDISP_GET_CURSOR, b, 0, T) == NVDISP_INVALID_STATE, "GET_CURSOR once");
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_CURSOR, (uint8_t *)&cur, sizeof cur, T) == NVDISP_OK && d.cursor_x == -5 && d.cursor_y == 40 && d.cursor_flags == 1, "SET_CURSOR round trip (s16 x, s16 y, flags)");
        MDcCursorImage ci = {{1, 2, 3}, {4, 5, 6}, 77, 2 /* 64x64 */, 10, -3, 200, 1};
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_CURSOR_IMAGE, (uint8_t *)&ci, sizeof ci, T) == NVDISP_OK && d.cursor_buff_id == 77 && d.cursor_image_flags == 2 && d.cursor_x == 10 &&
              d.cursor_y == -3 && d.cursor_alpha == 200 && d.cursor_colorfmt == 1 && d.cursor_fg[2] == 3 && d.cursor_bg[0] == 4, "SET_CURSOR_IMAGE round trip (28 bytes)");
        ci.flags = 7;
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_CURSOR_IMAGE, (uint8_t *)&ci, sizeof ci, T) == NVDISP_BAD_PARAMETER, "SET_CURSOR_IMAGE with an undefined size is refused");
        int32_t clip = 2; memcpy(b, &clip, 4);
        CHECK(nvdisp_ioctl(&d, NVDISP_CURSOR_CLIP, b, 4, T) == NVDISP_OK && d.cursor_clip == 2, "CURSOR_CLIP (0x0C)");
        CHECK(nvdisp_ioctl(&d, NVDISP_PUT_CURSOR, b, 0, T) == NVDISP_OK && !d.cursor_owned && nvdisp_ioctl(&d, NVDISP_CURSOR_CLIP, b, 4, T) == NVDISP_INVALID_STATE, "PUT_CURSOR, then cursor calls are refused");
        MDcCsc csc = {2, 0x10, 0x12a, 0x198, 0x80, 0x64, 0x3d0, 0x204, 0x7};
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_CSC, (uint8_t *)&csc, sizeof csc, T) == NVDISP_OK && d.csc[2][0] == 0x10 && d.csc[2][1] == 0x12a && d.csc[2][7] == 7, "SET_CSC round trip (window C)");
        csc.win_index = 3;
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_CSC, (uint8_t *)&csc, sizeof csc, T) == NVDISP_BAD_PARAMETER && nvdisp_ioctl(&d, NVDISP_SET_CSC, (uint8_t *)&csc, 16, T) == NVDISP_BAD_PARAMETER, "SET_CSC bad window / short");
        MDcStatus stt; memset(&stt, 0xee, sizeof stt);
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_STATUS, (uint8_t *)&stt, sizeof stt, T) == NVDISP_OK && stt.flags == NVDISP_STATUS_ENABLED && stt.pad[2] == 0, "GET_STATUS (16 bytes): enabled");
        d.syncpt = 26; uint32_t sp = 0;
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_VBLANK_SYNCPT, (uint8_t *)&sp, 4, T) == NVDISP_OK && sp == 26, "GET_VBLANK_SYNCPT: nr 9 in its 4-byte form");
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_STATUS, b, 8, T) == NVDISP_BAD_PARAMETER, "nr 9 with 8 bytes is neither form");
        MDcLut lut = {1, 1, 16, 32, 0x1000, 0x2000, 0x3000};
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_LUT, (uint8_t *)&lut, sizeof lut, T) == NVDISP_OK && d.lut_start[1] == 16 && d.lut_len[1] == 32 && d.lut_flags[1] == 1, "SET_LUT round trip (40 bytes, LP64)");
        lut.start = 250; lut.len = 10;
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_LUT, (uint8_t *)&lut, sizeof lut, T) == NVDISP_BAD_PARAMETER, "SET_LUT past 256 entries is refused");
        MDcCmu cmu; memset(&cmu, 0, sizeof cmu);
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_CMU, (uint8_t *)&cmu, sizeof cmu, T) == NVDISP_OK && cmu.cmu_enable == 0 && cmu.lut2[959] == 0, "GET_CMU before any SET_CMU: disabled");
        cmu.cmu_enable = 1; cmu.csc[0] = 0x100; cmu.lut1[255] = 0xffff; cmu.lut2[959] = 0x3ff;
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_CMU, (uint8_t *)&cmu, sizeof cmu, T) == NVDISP_OK, "SET_CMU (2452 bytes)");
        MDcCmu back; memset(&back, 0, sizeof back);
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_CUSTOM_CMU, (uint8_t *)&back, sizeof back, T) == NVDISP_OK && !memcmp(&back, &cmu, sizeof cmu), "GET_CUSTOM_CMU returns what was set");
        CHECK(nvdisp_ioctl(&d, NVDISP_GET_CMU, (uint8_t *)&back, 100, T) == NVDISP_BAD_PARAMETER, "GET_CMU short buffer");
        MDcSetVblank sv = {1, {0, 0, 0}};
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_VBLANK, (uint8_t *)&sv, sizeof sv, T) == NVDISP_OK && d.vblank_enabled, "SET_VBLANK enable");
        sv.reserved[1] = 1;
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_VBLANK, (uint8_t *)&sv, sizeof sv, T) == NVDISP_BAD_PARAMETER, "SET_VBLANK with non-zero reserved bytes is refused");
        uint32_t wm = 5; memcpy(b, &wm, 4);
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_WINMASK, b, 4, T) == NVDISP_OK && nvdisp_ioctl(&d, NVDISP_GET_WINMASK, b, 4, T) == NVDISP_OK && b[0] == 5, "SET_WINMASK / GET_WINMASK round trip");
        wm = 8; memcpy(b, &wm, 4);
        CHECK(nvdisp_ioctl(&d, NVDISP_SET_WINMASK, b, 4, T) == NVDISP_BAD_PARAMETER, "SET_WINMASK with a fourth window is refused");
        uint32_t line0, line1;
        nvdisp_ioctl(&d, NVDISP_GET_SCANLINE, (uint8_t *)&line0, 4, T + 1000000000ull / 60 / 2);
        nvdisp_ioctl(&d, NVDISP_GET_SCANLINE, (uint8_t *)&line1, 4, T + 1000000000ull / 60 + 1000);
        CHECK(line0 >= 370 && line0 <= 376 && line1 < 5, "GET_SCANLINE: mid-frame line %u of 750 (720p CEA total), line %u just after a vblank", line0, line1);
        CHECK(nvdisp_ioctl(&d, 0x03, b, 64, T) == NVDISP_NOT_IMPLEMENTED && nvdisp_ioctl(&d, 0x1d, b, 64, T) == NVDISP_NOT_IMPLEMENTED && nvdisp_ioctl(&d, 0x77, b, 32, T) == NVDISP_NOT_IMPLEMENTED,
              "FLIP (0x03, gone from the header), FLIP4 and unknown numbers are NotImplemented");
        nvdisp_init(&d, T, 1);
        CHECK(d.width == 1920 && d.height == 1080 && d.docked == 1, "disp core: docked 1920x1080");
        nvdisp_ioctl(&d, NVDISP_GET_SCANLINE, (uint8_t *)&line0, 4, T + 1000000000ull / 60 / 2);
        CHECK(line0 >= 558 && line0 <= 566, "docked GET_SCANLINE runs over 1125 lines (%u)", line0);
    }
    {
        uint32_t d0 = open_("/dev/nvdisp-disp0"), d1 = open_("/dev/nvdisp-disp1");
        CHECK(d0 && d1 && d0 != d1 && fds[d0].kind == DEV_DISP && fds[d1].kind == DEV_DISP, "nvdisp-disp0/1 open as display devices (fds %u %u)", d0, d1);
        uint32_t sp = 0, sp1 = 0;
        uint32_t e = ioctl_(d0, cmd_id(2, 4, NVDISP_IOCTL_TYPE, NVDISP_GET_VBLANK_SYNCPT), &sp, 4);
        ioctl_(d1, cmd_id(2, 4, NVDISP_IOCTL_TYPE, NVDISP_GET_VBLANK_SYNCPT), &sp1, 4);
        CHECK(e == 0 && sp == 26 && sp1 == 27, "nvdisp GET_VBLANK_SYNCPT through the handler: 26 and 27 (got %u %u)", sp, sp1);
        MDcStatus stt = {0, {0}};
        CHECK(ioctl_(d0, cmd_id(2, 16, NVDISP_IOCTL_TYPE, NVDISP_GET_STATUS), &stt, sizeof stt) == 0 && stt.flags == 1, "nvdisp GET_STATUS through the handler");
        uint32_t win = 0;
        CHECK(ioctl_(d0, cmd_id(1, 4, NVDISP_IOCTL_TYPE, NVDISP_GET_WINDOW), &win, 4) == 0 && ioctl_(d0, cmd_id(1, 4, NVDISP_IOCTL_TYPE, NVDISP_GET_WINDOW), &win, 4) == NV_INVALID_STATE,
              "nvdisp GET_WINDOW through the handler; a second claim is InvalidState");
        uint32_t junk[4] = {0};
        CHECK(ioctl_(d0, cmd_id(3, 16, 'X', 1), junk, 16) == NV_NOT_IMPLEMENTED, "nvdisp: another ioctl type is NotImplemented");
        CHECK(ioctl_(d0, cmd_id(1, 8, NVDISP_IOCTL_TYPE, NVDISP_SET_CSC), junk, 8) == NV_BAD_PARAMETER, "nvdisp: a too-small struct is BadParameter");
        close_(d0); close_(d1);
    }

    /* ---- b8 nvdrv-media-devices ---- */
    { /* NVDEC output with pitch != width (H.264 pitches 0x80 for a 64-wide picture), then VIC reads those surfaces back with the pitch NVDEC used */
        Buf dl = mkbuf(dec, 0x4000), dc = mkbuf(dec, 0x2000), dpic = mkbuf(dec, 0x1000), dbs = mkbuf(dec, 0x2000), dst = mkbuf(dec, 0x100);
        memcpy(dbs.mem, h264_64x48, sizeof h264_64x48);
        uint32_t l = sizeof h264_64x48, pitch = 0x80;
        memcpy(dpic.mem + 18 * 4, &l, 4); memcpy(dpic.mem + NVDEC_H264_W_PITCH_LUMA * 4, &pitch, 4); memcpy(dpic.mem + NVDEC_H264_W_PITCH_CHROMA * 4, &pitch, 4);
        memset(dl.mem, 0xab, dl.size); memset(dc.mem, 0xab, dc.size);
        media_pitch_hints_reset();
        uint32_t p1[] = {SETCL(MEDIA_CLASS_NVDEC), NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_H264, INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), dpic.iova >> 8, dbs.iova >> 8, 0,
                         INCR(NVDEC_SET_STATUS_OFFSET, 1), dst.iova >> 8, INCR(NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET, 2), dl.iova >> 8, dc.iova >> 8, IMM(NVM_EXECUTE, 0)};
        CHECK(submit(dec, &cb, p1, sizeof p1 / 4, NULL, 0) == NV_OK, "NVDEC 64x48 with pitch 0x80 submitted");
        uint8_t g = dl.mem[0];
        CHECK(dl.mem[63] == g && dl.mem[0x80 + 5] == g && dl.mem[47 * 0x80 + 63] == g && dl.mem[64] == 0xab && dl.mem[0x80 - 1] == 0xab && dl.mem[48 * 0x80] == 0xab,
              "NVDEC luma rows at 0x80 byte steps, padding between width and pitch untouched, nothing past the last row");
        CHECK(dc.mem[0] == 128 && dc.mem[0x80 + 63] == 128 && dc.mem[64] == 0xab && dc.mem[23 * 0x80 + 63] == 128 && dc.mem[24 * 0x80] == 0xab, "NVDEC chroma rows at 0x80 byte steps");
        Buf vcfg = mkbuf(vic, 0x1000), vout = mkbuf(vic, 0x4000), voc = mkbuf(vic, 0x2000);
        vic_cfg(&vcfg, VIC_FMT_NV12, 0, 0, 64, 48);
        uint32_t p2[] = {SETCL(MEDIA_CLASS_VIC), INCR(VIC_SET_INPUT0_LUMA_OFFSET, 2), dl.iova >> 8, dc.iova >> 8, INCR(VIC_SET_CONFIG_STRUCT_OFFSET, 1), vcfg.iova >> 8,
                         INCR(VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET, 2), vout.iova >> 8, voc.iova >> 8, IMM(NVM_EXECUTE, 0)};
        uint64_t verr = stats(vic)->errors;
        CHECK(submit(vic, &cb, p2, sizeof p2 / 4, NULL, 0) == NV_OK && stats(vic)->errors == verr, "VIC executes on the NVDEC surfaces");
        CHECK(vout.mem[0] == g && vout.mem[256 + 3] == g && vout.mem[47 * 256 + 63] == g && voc.mem[256] == 128, "VIC input read at NVDEC's pitch 0x80 (the 256-aligned guess would read padding), output at 256");
        /* a VIC surface NVDEC never wrote falls back to 256-aligned rows (the old behaviour) */
        media_pitch_hints_reset();
        vic_fill(&dl, &dc, 64, 48, 256, 90, 128, 128);
        CHECK(submit(vic, &cb, p2, sizeof p2 / 4, NULL, 0) == NV_OK && vout.mem[256 + 3] == 90 && vout.mem[47 * 256 + 63] == 90, "VIC without a pitch hint keeps the 256-aligned input rows");
        /* VIC RGBA out from the 0x80-pitch input */
        media_pitch_hints_reset();
        vic_fill(&dl, &dc, 64, 48, 0x80, 180, 128, 128);
        { uint32_t pp = 0x80; memcpy(dpic.mem + NVDEC_H264_W_PITCH_LUMA * 4, &pp, 4); }
        submit(dec, &cb, p1, sizeof p1 / 4, NULL, 0); /* sets the hint again (overwrites the picture with the null backend's grey) */
        g = dl.mem[0];
        vic_cfg(&vcfg, VIC_FMT_RGBA8, 0, 0, 64, 48);
        CHECK(submit(vic, &cb, p2, sizeof p2 / 4, NULL, 0) == NV_OK && vout.mem[47 * 256 + 63 * 4] == vout.mem[0] && vout.mem[3] == 255 && vout.mem[0] > 0, "VIC RGBA from pitch-0x80 input: last pixel equals the first");
    }
    { /* VP9 through the null backend with the surface strides of nvdec_vp9_pic_s (u16 pair at word 25), the key frame's width remembered for the inter frame */
        Buf vcb = mkbuf(dec, 0x1000), vbs = mkbuf(dec, 0x2000), vpic = mkbuf(dec, 0x1000), vl = mkbuf(dec, 0x4000), vc = mkbuf(dec, 0x2000), vst = mkbuf(dec, 0x100);
        uint32_t vp[] = {SETCL(MEDIA_CLASS_NVDEC), NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_VP9, INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), vpic.iova >> 8, vbs.iova >> 8, 0,
                         INCR(NVDEC_SET_STATUS_OFFSET, 1), vst.iova >> 8, INCR(NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET, 2), vl.iova >> 8, vc.iova >> 8, IMM(NVM_EXECUTE, 0)};
        uint32_t pitch = 0x80, bad = 60, l;
        for (int round = 0; round < 3; round++) {
            const uint8_t *fr = round == 1 ? vp9_inter : vp9_key_70x38;
            l = round == 1 ? sizeof vp9_inter : sizeof vp9_key_70x38;
            uint32_t pl = round == 2 ? bad : pitch;
            memset(vbs.mem, 0, 0x2000); memcpy(vbs.mem, fr, l); memset(vpic.mem, 0, 0x100); memcpy(vpic.mem + 12 * 4, &l, 4);
            MVp9Pic vpw; memset(&vpw, 0, sizeof vpw); vpw.stream_len = l; vpw.framestride[0] = (uint16_t)pl; vpw.framestride[1] = (uint16_t)pitch; memcpy(vpic.mem, &vpw, sizeof vpw);
            memset(vl.mem, 0xab, vl.size); memset(vc.mem, 0xab, vc.size);
            CHECK(submit(dec, &vcb, vp, sizeof vp / 4, NULL, 0) == NV_OK, "VP9 round %d submitted", round);
            uint8_t g = vl.mem[0];
            size_t lp = round == 2 ? 70 : 0x80; /* a pitch below the width is ignored: packed rows */
            CHECK(vl.mem[69] == g && vl.mem[lp + 5] == g && vl.mem[37 * lp + 69] == g && vl.mem[70] == (round == 2 ? g : 0xab),
                  "VP9 round %d (%s): luma rows %zu bytes apart", round, round == 0 ? "key frame" : round == 1 ? "inter frame reuses the key frame's width" : "pitch below width", lp);
        }
    }
    { /* VP9 refused by the backend: a defined error in the status structure and the counters, and the channel keeps working for H.264 */
        DecoderBackend refuse = decoder_backend_null; refuse.name = "refuse-vp9"; refuse.open = refuse_vp9_open;
        CHECK(decoder_backend_install(&refuse) == 0, "refusing backend installed");
        uint32_t rdec = open_("/dev/nvhost-nvdec");
        Buf rcb = mkbuf(rdec, 0x1000), rbs = mkbuf(rdec, 0x2000), rpic = mkbuf(rdec, 0x1000), rl = mkbuf(rdec, 0x4000), rc_ = mkbuf(rdec, 0x2000), rst = mkbuf(rdec, 0x100);
        uint32_t l = sizeof vp9_key_70x38;
        memcpy(rbs.mem, vp9_key_70x38, l); memcpy(rpic.mem + 12 * 4, &l, 4); memset(rst.mem, 0xff, 0x40);
        uint32_t rp[] = {SETCL(MEDIA_CLASS_NVDEC), NONINCR(NVM_SET_APPLICATION_ID, 1), DEC_CODEC_VP9, INCR(NVDEC_SET_DRV_PIC_SETUP_OFFSET, 3), rpic.iova >> 8, rbs.iova >> 8, 0,
                         INCR(NVDEC_SET_STATUS_OFFSET, 1), rst.iova >> 8, INCR(NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET, 2), rl.iova >> 8, rc_.iova >> 8, IMM(NVM_EXECUTE, 0)};
        CHECK(submit(rdec, &rcb, rp, sizeof rp / 4, NULL, 0) == NV_OK, "VP9 execute on a backend that refuses it is accepted by the ioctl");
        uint32_t sw[NVDEC_STATUS_WORDS]; memcpy(sw, rst.mem, sizeof sw);
        CHECK(stats(rdec)->errors == 1 && stats(rdec)->last_error == 1 && stats(rdec)->frames == 0 && sw[3] == 1 && sw[0] == 0 && sw[4] == 0 && rl.mem[0] == 0,
              "VP9 refused: status error 1 (codec not supported), no size, no frame, nothing written (errors %llu last %u err %u)", (unsigned long long)stats(rdec)->errors, stats(rdec)->last_error, sw[3]);
        memset(rbs.mem, 0, 0x2000); memcpy(rbs.mem, h264_64x48, sizeof h264_64x48); l = sizeof h264_64x48; memcpy(rpic.mem + 18 * 4, &l, 4);
        rp[2] = DEC_CODEC_H264;
        CHECK(submit(rdec, &rcb, rp, sizeof rp / 4, NULL, 0) == NV_OK && stats(rdec)->frames == 1 && stats(rdec)->errors == 1, "the same channel still decodes H.264 afterwards");
        close_(rdec);
        decoder_backend_install(NULL);
    }
    { /* VP9 superframe splitting (what a one-frame-per-sample backend needs) */
        const uint8_t sf[] = {1, 2, 3, 4, 5, 0xc1, 3, 2, 0xc1};
        size_t o[8], z[8];
        CHECK(vp9_superframe_split(sf, sizeof sf, o, z) == 2 && o[0] == 0 && z[0] == 3 && o[1] == 3 && z[1] == 2, "superframe with a 1-byte-size index: frames 3 + 2 bytes");
        CHECK(vp9_superframe_split(vp9_key_70x38, sizeof vp9_key_70x38, o, z) == 1 && z[0] == sizeof vp9_key_70x38, "no index: one frame");
        const uint8_t bad[] = {1, 2, 0xc1, 9, 9, 0xc1};
        CHECK(vp9_superframe_split(bad, sizeof bad, o, z) == -1, "a size that overruns the data is refused");
    }
    { /* NVJPG encode (application id 2): YUV in with the caller's tables, entropy-coded data out at bitstream_offset; the test writes the headers a driver would
       * write and decodes the result with jpeg_core */
        enum { W = 40, H = 24 };
        Buf jcb = mkbuf(jpg, 0x1000), jpic = mkbuf(jpg, 0x2000), jy = mkbuf(jpg, 0x1000), ju = mkbuf(jpg, 0x1000), jv = mkbuf(jpg, 0x1000), jout = mkbuf(jpg, 0x4000), jst = mkbuf(jpg, 0x100);
        uint8_t src[W * H * 4];
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t *q = src + (y * W + x) * 4; q[0] = (uint8_t)(x * 6); q[1] = (uint8_t)(y * 10); q[2] = (uint8_t)(100 + x + y); q[3] = 255; }
        /* 4:2:0 semi-planar input, strides 64 (luma) and 64 (interleaved chroma), full-range BT.601 */
        memset(jy.mem, 0, 0x1000); memset(ju.mem, 0, 0x1000);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { const uint8_t *q = src + (y * W + x) * 4; jy.mem[y * 64 + x] = (uint8_t)(0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2] + 0.5f); }
        for (int y = 0; y < H / 2; y++) for (int x = 0; x < W / 2; x++) {
            float r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) { const uint8_t *q = src + ((2 * y + k / 2) * W + 2 * x + k % 2) * 4; r += q[0] / 4.f; g += q[1] / 4.f; b += q[2] / 4.f; }
            ju.mem[y * 64 + 2 * x] = (uint8_t)(-0.168736f * r - 0.331264f * g + 0.5f * b + 128.5f); ju.mem[y * 64 + 2 * x + 1] = (uint8_t)(0.5f * r - 0.418688f * g - 0.081312f * b + 128.5f);
            jv.mem[y * 32 + x] = ju.mem[y * 64 + 2 * x]; jv.mem[0x800 + y * 32 + x] = ju.mem[y * 64 + 2 * x + 1]; /* planar copies for the 4:2:0 planar case */
        }
        MNvjpgPic p; JpegHuffCode dc[2], ac[2]; MNvjpgStatus es;
        const uint32_t boff = 0x300;
        nvjpg_enc_pic(&p, W, H, JPEGSCAN_420, 2, 0, 64, 64, boff, 0, dc, ac); memcpy(jpic.mem, &p, sizeof p);
        uint32_t ep[] = {SETCL(MEDIA_CLASS_NVJPG), NONINCR(NVM_SET_APPLICATION_ID, 1), NVJPG_APP_ENCODER,
                         INCR(NVJPG_SET_IN_DRV_PIC_SETUP, 6), jpic.iova >> 8, jst.iova >> 8, jout.iova >> 8, jy.iova >> 8, ju.iova >> 8, 0, IMM(NVM_EXECUTE, 0)};
        memset(jout.mem, 0, 0x4000); uint64_t f0 = stats(jpg)->frames;
        CHECK(submit(jpg, &jcb, ep, sizeof ep / 4, NULL, 0) == NV_OK && stats(jpg)->frames == f0 + 1, "NVJPG encode submitted");
        memcpy(&es, jst.mem, sizeof es);
        CHECK(es.error_status == 0 && es.w0 > 100 && es.w0 < 0x4000 - boff && es.mcu_x == 3 && es.mcu_y == 2, "nvjpg_enc_status: %u bytes, 3x2 MCUs of 16x16 (err %#x)", es.w0, es.error_status);
        int rst = 0; for (uint32_t i = 0; i + 1 < es.w0; i++) if (jout.mem[boff + i] == 0xff && jout.mem[boff + i + 1] >= 0xd0 && jout.mem[boff + i + 1] <= 0xd7) rst++;
        CHECK(rst == 2 && jout.mem[0] == 0 && jout.mem[boff - 1] == 0, "restart interval 2 over 6 MCUs: RST0, RST1; nothing written before bitstream_offset");
        { /* the driver's part: headers in front of the data, EOI behind; then decode */
            uint8_t *file = malloc(0x1000 + es.w0); size_t hn = jfif_headers(file, &p, W, H, JPEGSCAN_420, 2, dc, ac);
            memcpy(file + hn, jout.mem + boff, es.w0); file[hn + es.w0] = 0xff; file[hn + es.w0 + 1] = 0xd9;
            int dw = 0, dh = 0, derr = 0; uint8_t *rgb = napjpeg_decode_rgba(file, hn + es.w0 + 2, &dw, &dh, &derr);
            double ps = rgb ? psnr(src, rgb, (size_t)W * H * 4) : 0;
            CHECK(rgb && dw == W && dh == H && ps > 28.0, "NVJPG 4:2:0 scan + driver headers decodes to the source (%dx%d, PSNR %.1f dB, err %d)", dw, dh, ps, derr);
            free(rgb); free(file);
        }
        { /* 4:4:4 planar (memory_mode 1): Y, U, V surfaces */
            Buf py = mkbuf(jpg, 0x1000), pu = mkbuf(jpg, 0x1000), pv = mkbuf(jpg, 0x1000);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                const uint8_t *q = src + (y * W + x) * 4;
                py.mem[y * W + x] = (uint8_t)(0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2] + 0.5f);
                pu.mem[y * W + x] = (uint8_t)(-0.168736f * q[0] - 0.331264f * q[1] + 0.5f * q[2] + 128.5f); pv.mem[y * W + x] = (uint8_t)(0.5f * q[0] - 0.418688f * q[1] - 0.081312f * q[2] + 128.5f);
            }
            nvjpg_enc_pic(&p, W, H, JPEGSCAN_444, 0, 1, 0, 0, 0, 0, dc, ac); memcpy(jpic.mem, &p, sizeof p);
            uint32_t pp[sizeof ep / 4]; memcpy(pp, ep, sizeof ep); pp[7] = py.iova >> 8; pp[8] = pu.iova >> 8; pp[9] = pv.iova >> 8;
            submit(jpg, &jcb, pp, sizeof pp / 4, NULL, 0); memcpy(&es, jst.mem, sizeof es);
            uint8_t *file = malloc(0x1000 + es.w0); size_t hn = jfif_headers(file, &p, W, H, JPEGSCAN_444, 0, dc, ac);
            memcpy(file + hn, jout.mem, es.w0); file[hn + es.w0] = 0xff; file[hn + es.w0 + 1] = 0xd9;
            int dw = 0, dh = 0, derr = 0; uint8_t *rgb = es.error_status ? NULL : napjpeg_decode_rgba(file, hn + es.w0 + 2, &dw, &dh, &derr);
            double ps = rgb ? psnr(src, rgb, (size_t)W * H * 4) : 0;
            CHECK(rgb && es.mcu_x == 5 && es.mcu_y == 3 && ps > 30.0, "NVJPG 4:4:4 planar encode decodes to the source (PSNR %.1f dB, %ux%u MCUs)", ps, es.mcu_x, es.mcu_y);
            free(rgb); free(file);
            pp[9] = 0; submit(jpg, &jcb, pp, sizeof pp / 4, NULL, 0);
            CHECK(((MNvjpgStatus *)jst.mem)->error_status == NVJPG_ERR_INVALID_INPUT, "planar encode without a V surface: INVALID_INPUT");
        }
        /* errors: a zero quantiser factor, a missing Huffman code, a bitstream buffer too small */
        nvjpg_enc_pic(&p, W, H, JPEGSCAN_420, 0, 0, 64, 64, 0, 0, dc, ac); p.quant_chroma[5] = 0; memcpy(jpic.mem, &p, sizeof p);
        submit(jpg, &jcb, ep, sizeof ep / 4, NULL, 0);
        CHECK(((MNvjpgStatus *)jst.mem)->error_status == NVJPG_ERR_INVALID_INPUT && ((MNvjpgStatus *)jst.mem)->w0 == 0, "a zero quantiser factor: INVALID_INPUT");
        nvjpg_enc_pic(&p, W, H, JPEGSCAN_420, 0, 0, 64, 64, 0, 0, dc, ac); memset(p.hf_ac_luma, 0, sizeof p.hf_ac_luma); memcpy(jpic.mem, &p, sizeof p);
        submit(jpg, &jcb, ep, sizeof ep / 4, NULL, 0);
        CHECK(((MNvjpgStatus *)jst.mem)->error_status == NVJPG_ERR_INVALID_INPUT, "an AC table without codes: INVALID_INPUT");
        nvjpg_enc_pic(&p, W, H, JPEGSCAN_420, 0, 0, 64, 64, 0, 16, dc, ac); memcpy(jpic.mem, &p, sizeof p);
        submit(jpg, &jcb, ep, sizeof ep / 4, NULL, 0);
        CHECK(((MNvjpgStatus *)jst.mem)->error_status == NVJPG_ERR_INVALID_INPUT, "bitstream_size 16 cannot hold the data: INVALID_INPUT");
        { /* jpeg_core: the standard tables survive the DHT round trip; a table that is not canonical is refused */
            uint8_t bits[17], vals[256]; JpegHuffCode d2[2], a2[2]; napjpeg_std_huff(d2, a2);
            CHECK(napjpeg_huff_to_dht(&a2[0], bits, vals) == 162 && bits[16] == 0x7d && vals[0] == 0x01 && napjpeg_huff_to_dht(&d2[1], bits, vals) == 12, "std AC luma -> DHT: 162 codes; DC chroma: 12");
            a2[0].code[0x01] ^= 1; CHECK(napjpeg_huff_to_dht(&a2[0], bits, vals) == -1, "a non-canonical table has no DHT form");
            JpegScanParams bad = {0}; size_t n = 0; CHECK(napjpeg_encode_scan(&bad, &n) == NULL && napjpeg_encode_scan(NULL, &n) == NULL, "encode_scan refuses empty parameters");
        }
    }
    { /* MSENC: nvenc_h264_drv_pic_setup_s in, the EncoderBackend (null encoder, then a recording one), nvenc_pic_stat_s out; input pitch != width */
        enum { W = 32, H = 16 };
        const uint32_t pitch = 0x40;
        MNvencSetup su; memset(&su, 0, sizeof su);
        su.magic = 0xd0b70006; /* NV_NVENC_5_0 */
        su.input_cfg.width_m1 = W - 1; su.input_cfg.height_m1 = H - 1; su.input_cfg.pitch = pitch; su.input_cfg.pitch_chroma = pitch;
        su.rate_control.framerate = 30; su.rate_control.nal_bitrate = 1000000;
        su.pic_control[0xe0] = MSENC_PIC_TYPE_IDR << 2;
        MNvencStat ns;
        encoder_backend_install(NULL);
        uint32_t enc2 = open_("/dev/nvhost-msenc"); /* engines take the installed encoder when the channel opens */
        Buf ecb2 = mkbuf(enc2, 0x1000), esu = mkbuf(enc2, 0x200), elu = mkbuf(enc2, 0x1000), ech = mkbuf(enc2, 0x800), eout = mkbuf(enc2, 0x1000), est2 = mkbuf(enc2, 0x100);
        memcpy(esu.mem, &su, sizeof su); memset(elu.mem, 0x40, elu.size); memset(ech.mem, 0x80, ech.size);
        uint32_t ep[] = {SETCL(MEDIA_CLASS_MSENC), INCR(MSENC_SET_PICTURE_INDEX, 1), 1, INCR(MSENC_SET_IN_DRV_PIC_SETUP, 1), esu.iova >> 8, INCR(MSENC_SET_OUT_ENC_STATUS, 2), est2.iova >> 8, eout.iova >> 8,
                         INCR(MSENC_SET_IN_CUR_PIC, 1), elu.iova >> 8, INCR(MSENC_SET_IN_CUR_PIC_CHROMA_U, 1), ech.iova >> 8, IMM(NVM_EXECUTE, 0)};
#define MS() memcpy(&ns, est2.mem, sizeof ns)
        CHECK(submit(enc2, &ecb2, ep, sizeof ep / 4, NULL, 0) == NV_OK && stats(enc2)->unknown_regs == 0, "MSENC encode submitted (null encoder, clc5b7 methods)");
        MS();
        CHECK(ns.error == 0 && ns.picture_index == 1 && ns.total_bit_count > 17 * 8 && ns.pic_type == MSENC_PIC_TYPE_IDR && ns.num_slices == 1 && ns.last_valid_byte_offset == ns.total_bit_count / 8 - 1 &&
              !memcmp(eout.mem, "\0\0\0\1\x67", 5), "first frame: IDR with SPS/PPS in front, %u bits", ns.total_bit_count);
        uint32_t first = ns.total_bit_count;
        su.pic_control[0xe0] = MSENC_PIC_TYPE_P << 2; memcpy(esu.mem, &su, sizeof su);
        memset(eout.mem, 0, 64); memset(elu.mem, 0x90, elu.size); /* different picture */
        submit(enc2, &ecb2, ep, sizeof ep / 4, NULL, 0); MS();
        CHECK(ns.error == 0 && ns.pic_type == MSENC_PIC_TYPE_P && ns.total_bit_count < first && eout.mem[4] == 0x41, "second frame is a P slice (%u bits < %u)", ns.total_bit_count, first);
        su.pic_control[0xe0] = MSENC_PIC_TYPE_IDR << 2; memcpy(esu.mem, &su, sizeof su);
        submit(enc2, &ecb2, ep, sizeof ep / 4, NULL, 0); MS();
        CHECK(ns.error == 0 && ns.pic_type == MSENC_PIC_TYPE_IDR, "pic_type IDR (pic_control +0xE0 bits 2-3) gives a key frame again");
        /* the padding bytes between width and pitch are not part of the picture: poking them must not change the stream */
        uint8_t a[16]; memcpy(a, eout.mem, 16);
        memset(elu.mem + W, 0x11, pitch - W); submit(enc2, &ecb2, ep, sizeof ep / 4, NULL, 0);
        CHECK(!memcmp(a, eout.mem, 16), "bytes between width and the input pitch do not reach the encoder's picture");
        { /* planar input (memory_mode 1): U and V planes are interleaved for the NV12 backend; the stream matches the semi-planar one */
            Buf pu = mkbuf(enc2, 0x400), pv = mkbuf(enc2, 0x400);
            memset(pu.mem, 0x80, 0x400); memset(pv.mem, 0x80, 0x400);
            MNvencSetup pl = su; pl.input_cfg.flags = 1u << 8; pl.input_cfg.pitch_chroma = pitch / 2; memcpy(esu.mem, &pl, sizeof pl);
            uint32_t pp[] = {SETCL(MEDIA_CLASS_MSENC), INCR(MSENC_SET_IN_DRV_PIC_SETUP, 1), esu.iova >> 8, INCR(MSENC_SET_OUT_ENC_STATUS, 2), est2.iova >> 8, eout.iova >> 8,
                             INCR(MSENC_SET_IN_CUR_PIC, 1), elu.iova >> 8, INCR(MSENC_SET_IN_CUR_PIC_CHROMA_U, 2), pu.iova >> 8, pv.iova >> 8, IMM(NVM_EXECUTE, 0)};
            submit(enc2, &ecb2, pp, sizeof pp / 4, NULL, 0); MS();
            CHECK(ns.error == 0 && !memcmp(a, eout.mem, 16), "planar 4:2:0 input encodes like the semi-planar picture");
            pp[10] = 0; submit(enc2, &ecb2, pp, sizeof pp / 4, NULL, 0); MS();
            CHECK((ns.error & 3) == 1 && ns.error >> 2 == MSENC_ERR_PARAMS, "planar input without a V surface: error_status 1, ucode error MSENC_ERR_PARAMS");
            memcpy(esu.mem, &su, sizeof su);
        }
        /* errors (error_status bit 0, Napple's code in ucode_error_status) */
        MNvencSetup odd = su; odd.input_cfg.width_m1 = 30; memcpy(esu.mem, &odd, sizeof odd);
        submit(enc2, &ecb2, ep, sizeof ep / 4, NULL, 0); MS();
        CHECK((ns.error & 3) == 1 && ns.error >> 2 == MSENC_ERR_PARAMS && ns.total_bit_count == 0, "odd width is MSENC_ERR_PARAMS");
        odd = su; odd.input_cfg.pitch = 8; memcpy(esu.mem, &odd, sizeof odd);
        submit(enc2, &ecb2, ep, sizeof ep / 4, NULL, 0); MS();
        CHECK(ns.error >> 2 == MSENC_ERR_PARAMS, "a pitch below the width is MSENC_ERR_PARAMS");
        memcpy(esu.mem, &su, sizeof su);
        { Buf tiny = mkbuf(enc2, 8); uint32_t tp[sizeof ep / 4]; memcpy(tp, ep, sizeof ep); tp[7] = tiny.iova >> 8;
          submit(enc2, &ecb2, tp, sizeof tp / 4, NULL, 0); MS();
          CHECK(ns.error >> 2 == MSENC_ERR_FAILED && ns.total_bit_count == 0, "an 8-byte bitstream buffer is MSENC_ERR_FAILED"); }
        { Buf shortsu = mkbuf(enc2, 0x100); memcpy(shortsu.mem, &su, 0x100); uint32_t tp[sizeof ep / 4]; memcpy(tp, ep, sizeof ep); tp[4] = shortsu.iova >> 8;
          submit(enc2, &ecb2, tp, sizeof tp / 4, NULL, 0); MS();
          CHECK(ns.error >> 2 == MSENC_ERR_ADDRESS, "a setup surface shorter than 512 bytes is MSENC_ERR_ADDRESS"); }
        close_(enc2);
        /* a recording encoder sees the request fields */
        EncoderBackend rb = {"rec-enc", recenc_open, recenc_close, recenc_encode, ENCODER_BACKEND_ABI};
        CHECK(encoder_backend_install(&rb) == 0, "recording encoder installed");
        uint32_t enc3 = open_("/dev/nvhost-msenc");
        Buf c3 = mkbuf(enc3, 0x1000), s3 = mkbuf(enc3, 0x200), l3 = mkbuf(enc3, 0x1000), h3 = mkbuf(enc3, 0x800), o3 = mkbuf(enc3, 0x1000), t3 = mkbuf(enc3, 0x100);
        memcpy(s3.mem, &su, sizeof su); memset(l3.mem, 0x40, l3.size);
        uint32_t rp[] = {SETCL(MEDIA_CLASS_MSENC), INCR(MSENC_SET_IN_DRV_PIC_SETUP, 1), s3.iova >> 8, INCR(MSENC_SET_OUT_ENC_STATUS, 2), t3.iova >> 8, o3.iova >> 8,
                         INCR(MSENC_SET_IN_CUR_PIC, 1), l3.iova >> 8, INCR(MSENC_SET_IN_CUR_PIC_CHROMA_U, 1), h3.iova >> 8, IMM(NVM_EXECUTE, 0)};
        CHECK(submit(enc3, &c3, rp, sizeof rp / 4, NULL, 0) == NV_OK && recenc.opened == 1 && recenc.encoded == 1 && recenc.last.width == W && recenc.last.height == H && recenc.last.luma_pitch == pitch &&
              recenc.last.chroma_pitch == pitch && recenc.last.bitrate == 1000000 && recenc.last.fps == 30 && recenc.last.force_idr && recenc.last.out_cap == 0x1000,
              "the encoder gets size, pitches, bitrate, fps, the IDR picture type and the output room");
        close_(enc3);
        CHECK(recenc.closed == 1, "the encoder context is closed with the channel");
        encoder_backend_install(NULL);
#undef MS
    }

    /* close frees the engines; a submit on a channel without addresses still behaves as before */
    close_(dec); close_(vic); close_(jpg); close_(enc);
    CHECK(fds[dec].ch.eng == NULL, "engine released with the channel");
    for (int i = 0; i < 5; i++) {} /* (buffers are intentionally leaked: test process) */
    printf("{\"test\":\"svc_nvdrv_media\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
