#include "nvdrv_media_core.h"
#include "jpeg_core.h"
#include "nvdrv_media_regs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NREGS 0x1000
#define MAX_GATHER_DEPTH 4
void (*media_write_prepare)(void *addr, size_t n);
int media_trace(void) {
    static int v = -1;
    if (v < 0) v = getenv("NAPPLE_MEDIA_TRACE") && *getenv("NAPPLE_MEDIA_TRACE") != '0';
    return v;
}
struct MediaEngine {
    uint32_t class_id, cur_class;
    NvMediaMem mem;
    const DecoderBackend *backend;
    void *dec;
    int dec_codec, vp9_width; /* width of the last VP9 key frame (inter frames carry none) */
    const EncoderBackend *encoder;
    void *enc;
    uint32_t enc_frames;
    uint64_t cfg_key; /* hash of the parameter sets last handed to configure(); 0 = none yet */
    unsigned seq;     /* engines created so far in this process, this one included (names the NAPPLE_VIC_DUMP files) */
    uint32_t reg[NREGS];
    uint32_t thi_method; /* latched by the THI_METHOD0 write, consumed by the THI_METHOD1 write */
    MediaStats st;
};

/* NAPPLE_NVDEC_DUMP=<file>: the H.264 stream exactly as the backend gets it (the synthesized SPS/PPS at each configure, then every access unit), Annex B,
 * for replaying a title's video offline. */
static void media_dump(const uint8_t *p, size_t n) {
    static FILE *f;
    static int tried;
    if (!tried) {
        tried = 1;
        const char *path = getenv("NAPPLE_NVDEC_DUMP");
        if (path && *path) f = fopen(path, "wb");
    }
    if (f && p && n) {
        fwrite(p, 1, n, f);
        fflush(f);
    }
}
/* NAPPLE_MEDIA_TRACE: the engine's non-zero method registers at an EXECUTE (the first few per class) */
static void trace_regs(const MediaEngine *e) {
    static int said[4];
    int k = e->class_id == MEDIA_CLASS_NVDEC ? 0 : e->class_id == MEDIA_CLASS_VIC ? 1 : e->class_id == MEDIA_CLASS_NVJPG ? 2 : 3;
    if (!media_trace() || said[k]++ >= 6) return;
    fprintf(stderr, "[media] class %#x execute:", e->class_id);
    for (uint32_t r = 0x40; r < 0x200; r++)
        if (e->reg[r]) fprintf(stderr, " %#x=%#x", r, e->reg[r]);
    fprintf(stderr, "\n");
}
MediaEngine *media_engine_new(uint32_t class_id, const NvMediaMem *mem) {
    if (class_id != MEDIA_CLASS_NVDEC && class_id != MEDIA_CLASS_VIC && class_id != MEDIA_CLASS_NVJPG && class_id != MEDIA_CLASS_MSENC) return NULL;
    MediaEngine *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->class_id = e->cur_class = class_id;
    static unsigned created;
    e->seq = __atomic_add_fetch(&created, 1, __ATOMIC_RELAXED);
    if (mem) e->mem = *mem;
    e->backend = decoder_backend_current();
    e->encoder = encoder_backend_current();
    return e;
}
void media_engine_free(MediaEngine *e) {
    if (!e) return;
    if (e->dec) e->backend->close(e->dec);
    if (e->enc) e->encoder->close(e->enc);
    free(e);
}
const MediaStats *media_engine_stats(const MediaEngine *e) { return &e->st; }
uint32_t media_engine_reg(const MediaEngine *e, uint32_t off) { return off < NREGS ? e->reg[off] : 0; }
uint32_t media_engine_class(const MediaEngine *e) { return e->class_id; }

/* Pitches NVDEC wrote its output surfaces with, by surface register value (iova >> 8). VIC reads its input surfaces back with the pitch NVDEC used for them (the
 * guest hands the same surfaces from one engine to the other), instead of guessing between 256-aligned and packed rows. Engines run under nvdrv's lock. */
#define PITCH_HINTS 64
static struct { uint32_t reg, pitch; } pitch_hint[PITCH_HINTS];
static unsigned pitch_hint_next;
void media_pitch_hints_reset(void) {
    memset(pitch_hint, 0, sizeof pitch_hint);
    pitch_hint_next = 0;
}
static void pitch_hint_set(uint32_t reg, uint32_t pitch) {
    if (!reg || !pitch) return;
    for (unsigned i = 0; i < PITCH_HINTS; i++)
        if (pitch_hint[i].reg == reg) {
            pitch_hint[i].pitch = pitch;
            return;
        }
    pitch_hint[pitch_hint_next % PITCH_HINTS].reg = reg;
    pitch_hint[pitch_hint_next++ % PITCH_HINTS].pitch = pitch;
}
static uint32_t pitch_hint_get(uint32_t reg) {
    for (unsigned i = 0; i < PITCH_HINTS; i++)
        if (reg && pitch_hint[i].reg == reg) return pitch_hint[i].pitch;
    return 0;
}

static uint8_t *surf(MediaEngine *e, uint32_t reg_value, size_t *avail) {
    size_t a = 0;
    uint8_t *p = reg_value && e->mem.resolve ? e->mem.resolve(e->mem.ctx, (uint64_t)reg_value << 8, &a) : NULL;
    if (avail) *avail = p ? a : 0;
    return p;
}
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
/* Writes `n` words into the status structure when the guest gave one. */
static void status_write(MediaEngine *e, uint32_t reg_off, const uint32_t *w, unsigned n) {
    size_t avail;
    uint8_t *p = surf(e, e->reg[reg_off], &avail);
    if (!p || avail < n * 4) return;
    for (unsigned i = 0; i < n; i++) wr32(p + i * 4, w[i]);
}
static void fail(MediaEngine *e, uint32_t code) {
    e->st.errors++;
    e->st.last_error = code;
}

/* ---- NVDEC ---- */
static uint64_t fnv64(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
/* Hands the backend the parameter sets of this frame's stream when they are new: H.264 SPS+PPS NAL units found in the bitstream, a VP9 key frame with a
 * size. Returns 0 when the backend refused them, 1 otherwise (including "nothing new"). */
static int sext(uint32_t v, int bits) { return (int)(v << (32 - bits)) >> (32 - bits); }
/* H.264 as the SDK sends it for the hardware: slice NAL units only, the sequence and picture parameters live in the picture-info structure. Writes the SPS and
 * PPS those fields describe (what the slice headers need: frame number and POC widths, field/frame, CABAC, reference defaults, weighting, deblocking, 8x8). The
 * reference-frame count is not in the structure: the maximum for the picture size is claimed, which only delays nothing here (the backend outputs per request). */
static int nvdec_synth_h264(const uint8_t *pic, size_t pic_avail, DecoderConfig *cfg, uint64_t *key) {
    if (!pic || pic_avail < NVDEC_H264_PIC_WORDS * 4) return 0;
    uint32_t w[NVDEC_H264_PIC_WORDS];
    memcpy(w, pic, sizeof w);
    int wm = (int)w[NVDEC_H264_W_WIDTH_MBS], hm = (int)w[NVDEC_H264_W_HEIGHT_MAP_UNITS];
    if (wm < 1 || hm < 1 || wm > 512 || hm > 512) return 0;
    uint32_t fl = w[NVDEC_H264_W_FLAGS];
    H264SpsFields f;
    h264_sps_fields_default(&f, wm * 16, hm * 16, 4);
    f.frame_mbs_only = w[NVDEC_H264_W_FRAME_MBS_ONLY] != 0;
    if (!f.frame_mbs_only) f.height_map_units = hm, f.crop_bottom = 0;
    f.log2_max_frame_num = (int)((fl >> 8) & 15) + 4;
    f.poc_type = (int)((fl >> 14) & 3);
    f.log2_max_poc_lsb = (int)w[NVDEC_H264_W_LOG2_MAX_POC_LSB_M4] + 4;
    f.direct_8x8_inference = (fl >> 1) & 1;
    f.chroma_format_idc = 1;
    /* 15, not 16: the DPB holds at most 16 frames, and with 16 references VideoToolbox has no room left for the current non-reference (B) picture and
     * evicts a reference the stream still uses (MK8D's preview: macroblock corruption mid-GOP; a replay of its stream decodes identically with 6..15, the
     * stream needs 6, and breaks only at 16). The picture-info structure does not carry the stream's real count. */
    f.num_ref_frames = 15;
    { /* level_idc whose DPB (MaxDpbMbs, Table A-1) holds 16 frames (the references plus the current picture): the size-only level of the defaults (3.0 for
       * MK8D's 1216 MBs) holds 6 */
        static const struct {
            int level;
            long dpb_mbs;
        } L[] = {{30, 8100}, {31, 18000}, {32, 20480}, {40, 32768}, {42, 34816}, {50, 110400}, {51, 184320}};
        long need = 16L * wm * hm * (f.frame_mbs_only ? 1 : 2);
        int lv = 51;
        for (unsigned i = sizeof L / sizeof *L; i-- > 0;)
            if (L[i].dpb_mbs >= need && L[i].level >= f.level_idc) lv = L[i].level; /* smallest qualifying level not below the size's own */
        f.level_idc = lv;
    }
    f.crop_left = f.crop_right = f.crop_top = f.crop_bottom = 0;
    uint8_t sps[64], pps[32];
    size_t sn = h264_build_sps(&f, sps, sizeof sps);
    H264Pps p;
    memset(&p, 0, sizeof p);
    p.cabac = w[NVDEC_H264_W_CABAC] != 0;
    p.bottom_field_pic_order = w[NVDEC_H264_W_POC_PRESENT] != 0;
    p.num_ref_idx_l0 = (int)(w[NVDEC_H264_W_NUM_REF_L0] & 31) + 1;
    p.num_ref_idx_l1 = (int)(w[NVDEC_H264_W_NUM_REF_L1] & 31) + 1;
    p.weighted_pred = (fl >> 2) & 1;
    p.weighted_bipred_idc = (int)(w[NVDEC_H264_W_FLAGS2] & 3);
    p.pic_init_qp = 26 + sext((fl >> 16) & 63, 6);
    p.pic_init_qs = 26;
    p.chroma_qp_index_offset = sext((fl >> 22) & 31, 5);
    p.second_chroma_qp_index_offset = sext((fl >> 27) & 31, 5);
    p.deblocking_filter_control = w[NVDEC_H264_W_DEBLOCK_CTRL] != 0;
    p.constrained_intra_pred = (fl >> 3) & 1;
    p.redundant_pic_cnt = w[NVDEC_H264_W_REDUNDANT_PIC_CNT] != 0;
    p.transform_8x8 = w[NVDEC_H264_W_TRANSFORM_8X8] != 0;
    size_t pn = h264_build_pps(&p, pps, sizeof pps);
    if (!sn || !pn) return 0;
    uint8_t *o = cfg->h264_annexb;
    static const uint8_t sc[4] = {0, 0, 0, 1};
    if (sn + pn + 8 > sizeof cfg->h264_annexb) return 0;
    memcpy(o, sc, 4);
    memcpy(o + 4, sps, sn);
    memcpy(o + 4 + sn, sc, 4);
    memcpy(o + 8 + sn, pps, pn);
    cfg->h264_annexb_size = sn + pn + 8;
    cfg->width = wm * 16;
    cfg->height = hm * 16 * (f.frame_mbs_only ? 1 : 2);
    cfg->h264.nsps = cfg->h264.npps = 1;
    cfg->h264.sps[0] = o + 4;
    cfg->h264.sps_size[0] = sn;
    cfg->h264.pps[0] = o + 8 + sn;
    cfg->h264.pps_size[0] = pn;
    *key = fnv64(*key, o, cfg->h264_annexb_size);
    return 1;
}
static int nvdec_configure(MediaEngine *e, int codec, const uint8_t *bs, size_t n, const uint8_t *pic, size_t pic_avail) {
    DecoderConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.codec = codec;
    uint64_t key = 1469598103934665603ull;
    if (codec == DEC_CODEC_H264) {
        H264Sps sps;
        if (h264_find_param_sets(bs, n, &cfg.h264) != 0) {
            /* nothing (or only half) in this access unit: the picture-info structure describes the sequence (the SDK's way); else keep what the backend has */
            if (!nvdec_synth_h264(pic, pic_avail, &cfg, &key)) return 1;
            goto have_key;
        }
        for (int i = 0; i < cfg.h264.nsps; i++) key = fnv64(key, cfg.h264.sps[i], cfg.h264.sps_size[i]);
        for (int i = 0; i < cfg.h264.npps; i++) key = fnv64(key, cfg.h264.pps[i], cfg.h264.pps_size[i]);
        cfg.h264_annexb_size = h264_param_sets_annexb(&cfg.h264, cfg.h264_annexb, sizeof cfg.h264_annexb);
        if (!h264_parse_sps(cfg.h264.sps[0] + 1, cfg.h264.sps_size[0] - 1, &sps)) cfg.width = sps.width, cfg.height = sps.height;
    } else if (codec == DEC_CODEC_VP9) {
        if (vp9_parse_header(bs, n, &cfg.vp9) || !cfg.vp9.key_frame || !cfg.vp9.width) return 1;
        key = fnv64(key, &cfg.vp9.profile, sizeof cfg.vp9.profile);
        key = fnv64(key, &cfg.vp9.width, sizeof cfg.vp9.width);
        key = fnv64(key, &cfg.vp9.height, sizeof cfg.vp9.height);
        key = fnv64(key, &cfg.vp9.bit_depth, sizeof cfg.vp9.bit_depth);
        key = fnv64(key, &cfg.vp9.color_space, sizeof cfg.vp9.color_space);
        key = fnv64(key, &cfg.vp9.color_range, sizeof cfg.vp9.color_range);
        key = fnv64(key, &cfg.vp9.subsampling_x, sizeof cfg.vp9.subsampling_x);
        key = fnv64(key, &cfg.vp9.subsampling_y, sizeof cfg.vp9.subsampling_y);
        cfg.width = cfg.vp9.width;
        cfg.height = cfg.vp9.height;
        cfg.vpcc_size = vp9_build_vpcc(&cfg.vp9, cfg.vpcc);
    } else {
        return 1;
    }
have_key:
    if (key == e->cfg_key) return 1;
    if (e->backend->reset) e->backend->reset(e->dec);
    if (e->backend->configure(e->dec, &cfg) < 0) return 0;
    media_dump(cfg.h264_annexb, codec == DEC_CODEC_H264 ? cfg.h264_annexb_size : 0);
    e->cfg_key = key;
    e->st.configures++;
    return 1;
}
static void nvdec_execute(MediaEngine *e) {
    int codec = (int)e->reg[NVM_SET_APPLICATION_ID];
    uint32_t status[NVDEC_STATUS_WORDS] = {0}; /* nvdec_status_s: mbs correct, mbs in error, cycles, error_status, HEVC/VP9 union, slice header error */
    if (e->dec && e->dec_codec != codec) {
        e->backend->close(e->dec);
        e->dec = NULL;
    }
    if (!e->dec) {
        e->dec = e->backend->open(codec);
        e->dec_codec = codec;
        e->cfg_key = 0;
    }
    if (!e->dec) {
        fail(e, 1); /* codec not supported by the backend */
        status[NVDEC_STATUS_W_ERROR] = 1;
        status_write(e, NVDEC_SET_STATUS_OFFSET, status, NVDEC_STATUS_WORDS);
        return;
    }
    size_t pic_avail = 0, bs_avail = 0, luma_avail = 0, chroma_avail = 0;
    uint8_t *pic = surf(e, e->reg[NVDEC_SET_DRV_PIC_SETUP_OFFSET], &pic_avail);
    uint8_t *bs = surf(e, e->reg[NVDEC_SET_IN_BUF_BASE_OFFSET], &bs_avail);
    uint32_t idx = e->reg[NVDEC_SET_PICTURE_INDEX];
    if (codec == DEC_CODEC_H264 && pic && pic_avail >= NVDEC_H264_PIC_WORDS * 4) { /* the slot comes from the picture-info structure */
        uint32_t cur = NVDEC_H264_CURR_PIC_IDX(rd32(pic + NVDEC_H264_W_FLAGS2 * 4));
        if (media_trace()) {
            static int said;
            if (said++ < 30) fprintf(stderr, "[nvdec] frame register %u, curr_pic_idx %u, frame_number %u\n", idx, cur, (rd32(pic + NVDEC_H264_W_FLAGS2 * 4) >> 14) & 0xffff);
        }
        if (cur < NVDEC_PIC_SLOTS) idx = cur;
    }
    uint32_t lreg = idx < NVDEC_PIC_SLOTS ? e->reg[NVDEC_SET_PICTURE_LUMA_OFFSET0 + idx] : 0,
             creg = idx < NVDEC_PIC_SLOTS ? e->reg[NVDEC_SET_PICTURE_CHROMA_OFFSET0 + idx] : 0;
    if (!lreg) lreg = e->reg[NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET];
    if (!creg) creg = e->reg[NVDEC_SET_DISPLAY_BUF_CHROMA_OFFSET];
    uint8_t *luma = surf(e, lreg, &luma_avail), *chroma = surf(e, creg, &chroma_avail);
    size_t bs_len = bs_avail;
    int lw = codec == DEC_CODEC_H264 ? NVDEC_PIC_STREAM_LEN_WORD_H264
             : codec == DEC_CODEC_VP8 ? NVDEC_PIC_STREAM_LEN_WORD_VP8
             : codec == DEC_CODEC_H265 ? NVDEC_PIC_STREAM_LEN_WORD_H265
                                       : NVDEC_PIC_STREAM_LEN_WORD_VP9;
    if (pic && pic_avail >= (size_t)(lw + 1) * 4) {
        uint32_t l = rd32(pic + (size_t)lw * 4);
        if (l && l <= bs_avail) bs_len = l;
    }
    if (!bs || !luma || !chroma) {
        if (media_trace()) {
            static int said;
            if (said++ < 12)
                fprintf(stderr, "[nvdec] unmapped surface: picture index %u, bitstream reg %#x (%s), luma reg %#x (%s), chroma reg %#x (%s), pic-info reg %#x\n", idx,
                        e->reg[NVDEC_SET_IN_BUF_BASE_OFFSET], bs ? "ok" : "MISSING", lreg, luma ? "ok" : "MISSING", creg, chroma ? "ok" : "MISSING",
                        e->reg[NVDEC_SET_DRV_PIC_SETUP_OFFSET]);
        }
        fail(e, 2);
        status[NVDEC_STATUS_W_ERROR] = 2;
        status_write(e, NVDEC_SET_STATUS_OFFSET, status, NVDEC_STATUS_WORDS);
        return;
    }
    if (e->backend->configure && !nvdec_configure(e, codec, bs, bs_len, pic, pic_avail)) {
        fail(e, 4); /* the backend refused the parameter sets */
        status[NVDEC_STATUS_W_ERROR] = 4;
        status_write(e, NVDEC_SET_STATUS_OFFSET, status, NVDEC_STATUS_WORDS);
        return;
    }
    DecodeRequest rq = {codec, pic, pic_avail, bs, bs_len, idx, luma, chroma, luma_avail, chroma_avail, 0, 0};
    if (codec == DEC_CODEC_H264 && pic && pic_avail >= NVDEC_H264_PIC_WORDS * 4) { /* the guest's surface pitches (MK8D: 0x200 for its 512-wide video) */
        rq.luma_pitch = rd32(pic + NVDEC_H264_W_PITCH_LUMA * 4);
        rq.chroma_pitch = rd32(pic + NVDEC_H264_W_PITCH_CHROMA * 4);
        if (rq.luma_pitch > 0x10000) rq.luma_pitch = 0;
        if (rq.chroma_pitch > 0x10000) rq.chroma_pitch = 0;
    } else if (codec == DEC_CODEC_VP9 && pic && pic_avail >= (NVDEC_VP9_W_STRIDES + 1) * 4) {
        /* nvdec_vp9_pic_s: word 24 = width | height << 16, word 25 = luma stride | chroma stride << 16 (u16 each). The guard stays: a stride below the
         * picture width, above 0x10000 or not a multiple of 16 is ignored (packed rows), so a stream that leaves the words 0 still decodes. */
        Vp9Header vh;
        uint32_t sz = rd32(pic + NVDEC_VP9_W_SIZE * 4), st = rd32(pic + NVDEC_VP9_W_STRIDES * 4), lp = st & 0xffff, cp = st >> 16;
        int known = (int)(sz & 0xffff);
        if (!known) known = !vp9_parse_header(bs, bs_len, &vh) && vh.key_frame && vh.width ? vh.width : e->vp9_width;
        if (known) e->vp9_width = known;
        if (known && lp >= (uint32_t)known && !(lp & 15)) rq.luma_pitch = lp;
        if (known && cp >= (uint32_t)known && !(cp & 15)) rq.chroma_pitch = cp;
    } else if (codec == DEC_CODEC_VP8 && pic && pic_avail >= (NVDEC_VP8_W_STRIDE_CHROMA + 1) * 4) { /* nvdec_vp8_pic_s FrameStride[2], same guard */
        uint32_t w8 = rd32(pic + NVDEC_VP8_W_SIZE * 4) & 0xffff, lp = rd32(pic + NVDEC_VP8_W_STRIDE_LUMA * 4), cp = rd32(pic + NVDEC_VP8_W_STRIDE_CHROMA * 4);
        if (w8 && lp >= w8 && lp <= 0x10000 && !(lp & 15)) rq.luma_pitch = lp;
        if (w8 && cp >= w8 && cp <= 0x10000 && !(cp & 15)) rq.chroma_pitch = cp;
    }
    DecodeResult rs = {0};
    if (media_write_prepare) {
        media_write_prepare(luma, luma_avail);
        media_write_prepare(chroma, chroma_avail);
    }
    if (codec == DEC_CODEC_H264) media_dump(bs, bs_len);
    int rc = e->backend->decode(e->dec, &rq, &rs);
    if (media_trace()) {
        static int said;
        int nal = -1, nals = 0, idr = 0;
        for (size_t i = 0; i + 3 < bs_len; i++)
            if (bs[i] == 0 && bs[i + 1] == 0 && bs[i + 2] == 1) {
                int t = bs[i + 3] & 31;
                if (nal < 0) nal = bs[i + 3];
                nals++;
                idr |= t == 5;
            }
        int beyond = 0; /* start codes in the buffer after the stream length (slices the length word does not cover?) */
        for (size_t i = bs_len; i + 3 < bs_avail && i < bs_len + 0x40000; i++)
            if (bs[i] == 0 && bs[i + 1] == 0 && bs[i + 2] == 1) beyond++;
        if (said < 400 && pic && pic_avail >= 88 && (beyond || (e->st.executes > 140 && e->st.executes < 200)))
            fprintf(stderr, "[nvdec] execute %llu: %d NAL units in %zu bytes, %d start codes after it (buffer %zu), pic words 18-21 %#x %#x %#x %#x, slice-offsets reg %#x\n",
                    (unsigned long long)e->st.executes, nals, bs_len, beyond, bs_avail, rd32(pic + 72), rd32(pic + 76), rd32(pic + 80), rd32(pic + 84),
                    e->reg[NVDEC_SET_SLICE_OFFSETS_BUF_OFFSET]);
        if (said < 400 && (idr || said < 20 || rc != 0))
            fprintf(stderr, "[nvdec] execute %llu slot %u: %d NAL units, first header %#x%s, %zu bytes, configures %llu\n", (unsigned long long)e->st.executes, idx, nals, nal,
                    idr ? " (IDR)" : "", bs_len, (unsigned long long)e->st.configures);
        if (said++ < 20 || rc != 0)
            fprintf(stderr, "[nvdec] backend %s codec %d bitstream %zu luma_cap %zu chroma_cap %zu -> rc %d %dx%d\n", e->backend->name, codec, bs_len, luma_avail,
                    chroma_avail, rc, rs.width, rs.height);
    }
    if (rc < 0) {
        fail(e, 3);
        status[NVDEC_STATUS_W_ERROR] = 3;
    } else if (rc == 0) {
        e->st.frames++;
        pitch_hint_set(lreg, rq.luma_pitch >= (uint32_t)rs.width ? rq.luma_pitch : (uint32_t)rs.width);
        pitch_hint_set(creg, rq.chroma_pitch >= (uint32_t)rs.width ? rq.chroma_pitch : (uint32_t)rs.width);
        status[0] = (uint32_t)(((rs.width + 15) / 16) * ((rs.height + 15) / 16)); /* macroblocks correctly decoded */
        if (codec == DEC_CODEC_VP9 || codec == DEC_CODEC_H265) { /* the HEVC / VP9 statistics: every 8x8 block intra on a key frame, inter otherwise */
            uint32_t blocks = (uint32_t)(((rs.width + 7) / 8) * ((rs.height + 7) / 8));
            Vp9Header vh;
            int key = codec == DEC_CODEC_VP9 ? !vp9_parse_header(bs, bs_len, &vh) && vh.key_frame : 1;
            status[key ? NVDEC_STATUS_W_INTRA_CNT : NVDEC_STATUS_W_INTER_CNT] = blocks;
        }
    }
    status_write(e, NVDEC_SET_STATUS_OFFSET, status, NVDEC_STATUS_WORDS);
}

/* ---- VIC: NV12 -> RGBA/BGRA/NV12, pitch linear or block linear ---- */
static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
/* Tegra block-linear layout: 64-byte x 8-row GOBs of 512 bytes, stacked (1 << bh) GOBs high per block, blocks row-major. (x in bytes, y in rows.) */
static size_t bl_offset(size_t x, size_t y, size_t gobs_w, unsigned bh) {
    size_t brows = (size_t)8 << bh, block = (y / brows) * gobs_w + x / 64;
    return block * ((size_t)512 << bh) + ((y % brows) / 8) * 512 + ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}
static size_t bl_size(size_t row_bytes, size_t rows, unsigned bh) {
    size_t brows = (size_t)8 << bh;
    return ((row_bytes + 63) / 64) * ((rows + brows - 1) / brows) * ((size_t)512 << bh);
}
/* Bytes a plane of `rows` rows of `row_bytes` needs in the output layout (`pitch` for pitch linear). */
static size_t plane_need(size_t row_bytes, size_t rows, size_t pitch, int block, unsigned bh) {
    return block ? bl_size(row_bytes, rows, bh) : pitch * (rows - 1) + row_bytes;
}
/* Stores one row of a plane: pitch linear at row*pitch, block linear in 16-byte runs (a GOB keeps 16 consecutive bytes of a row together). */
static void put_row(uint8_t *dst, const uint8_t *row, size_t row_bytes, size_t y, size_t pitch, int block, unsigned bh) {
    if (!block) {
        memcpy(dst + y * pitch, row, row_bytes);
        return;
    }
    size_t gobs_w = (row_bytes + 63) / 64;
    for (size_t x = 0; x < row_bytes; x += 16) memcpy(dst + bl_offset(x, y, gobs_w, bh), row + x, row_bytes - x < 16 ? row_bytes - x : 16);
}
static size_t align256(size_t v) { return (v + 255) & ~(size_t)255; }
/* NAPPLE_VIC_DUMP=<prefix>: the NV12 picture a VIC execute converts, as <prefix>-<engine>-<frame>.ppm (engine: creation order; RGB, BT.601 limited
 * range as below), for the VIC frame numbers listed in NAPPLE_VIC_DUMP_FRAMES (comma separated; default 1,60,120,240). Headless evidence that a
 * title's video decodes to real pictures; the files hold game frames: keep them out of the repository. */
static int vic_dump_wanted(uint64_t frame, const char **prefix) {
    static const char *pre;
    static uint64_t list[16];
    static int n = -1;
    if (n < 0) {
        n = 0;
        pre = getenv("NAPPLE_VIC_DUMP");
        if (pre && !*pre) pre = NULL;
        const char *l = getenv("NAPPLE_VIC_DUMP_FRAMES");
        if (!l || !*l) l = "1,60,120,240";
        while (*l && n < 16) {
            char *end;
            unsigned long long v = strtoull(l, &end, 10);
            if (end == l) break;
            list[n++] = v;
            l = *end == ',' ? end + 1 : end;
        }
    }
    *prefix = pre;
    if (!pre) return 0;
    for (int i = 0; i < n; i++)
        if (list[i] == frame) return 1;
    return 0;
}
static void vic_dump(unsigned seq, uint64_t frame, const uint8_t *luma, const uint8_t *chroma, size_t ip, size_t w, size_t h) {
    const char *pre;
    if (!vic_dump_wanted(frame, &pre)) return;
    char path[1024];
    snprintf(path, sizeof path, "%s-%u-%llu.ppm", pre, seq, (unsigned long long)frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%zu %zu\n255\n", w, h);
    uint8_t *row = malloc(w * 3);
    for (size_t y = 0; row && y < h; y++) {
        const uint8_t *yr = luma + y * ip, *uv = chroma + (y >> 1) * ip;
        for (size_t x = 0; x < w; x++) {
            int Y = ((yr[x] - 16) * 76310) >> 16, U = uv[x & ~(size_t)1] - 128, V = uv[(x & ~(size_t)1) + 1] - 128;
            row[3 * x] = clamp8(Y + ((104597 * V) >> 16));
            row[3 * x + 1] = clamp8(Y - ((25675 * U + 53279 * V) >> 16));
            row[3 * x + 2] = clamp8(Y + ((132201 * U) >> 16));
        }
        fwrite(row, 1, w * 3, f);
    }
    free(row);
    fclose(f);
    fprintf(stderr, "[vic] frame %llu (%zux%zu) -> %s\n", (unsigned long long)frame, w, h, path);
}
static void vic_execute(MediaEngine *e) {
    size_t cfg_avail, l_avail, c_avail, o_avail, oc_avail = 0;
    uint8_t *cfg = surf(e, e->reg[VIC_SET_CONFIG_STRUCT_OFFSET], &cfg_avail);
    uint8_t *luma = surf(e, e->reg[VIC_SET_INPUT0_LUMA_OFFSET], &l_avail), *chroma = surf(e, e->reg[VIC_SET_INPUT0_CHROMA_OFFSET], &c_avail);
    uint8_t *out = surf(e, e->reg[VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET], &o_avail), *ochroma = surf(e, e->reg[VIC_SET_OUTPUT_SURFACE_CHROMA_OFFSET], &oc_avail);
    if (!luma || !chroma || !out) {
        fail(e, 2);
        return;
    }
    if (!cfg || cfg_avail < VIC_CFG_BYTES) { /* no readable config structure: plain NV12 -> NV12 copy of what fits */
        if (!ochroma) {
            fail(e, 2);
            return;
        }
        if (media_write_prepare) {
            media_write_prepare(out, o_avail);
            media_write_prepare(ochroma, oc_avail);
        }
        memcpy(out, luma, l_avail < o_avail ? l_avail : o_avail);
        memcpy(ochroma, chroma, c_avail < oc_avail ? c_avail : oc_avail);
        e->st.vic_copies++;
        return;
    }
    uint64_t oc;
    memcpy(&oc, cfg + VIC_CFG_OUTPUT_SURFACE, 8);
    uint32_t fmt = (uint32_t)(oc & 0x7f), kind = (uint32_t)((oc >> 11) & 15), bh = (uint32_t)((oc >> 15) & 15);
    size_t w = (size_t)((oc >> 32) & 0x3fff) + 1, h = (size_t)((oc >> 46) & 0x3fff) + 1, ch = (h + 1) / 2;
    int block = kind != 0, rgb = fmt == VIC_FMT_RGBA8 || fmt == VIC_FMT_BGRA8 || fmt == VIC_FMT_RGBX8;
    if (media_trace()) {
        static int said;
        if (said++ < 6) fprintf(stderr, "[vic] output %zux%zu format %#x %s (block height %u GOBs)\n", w, h, fmt, block ? "block linear" : "pitch linear", 1u << bh);
    }
    /* input rows: 256-aligned when the surfaces are large enough for that (NVDEC's surfaces), else packed */
    size_t ip = l_avail >= align256(w) * (h - 1) + w && c_avail >= align256(w) * (ch - 1) + w ? align256(w) : w;
    uint32_t hl = pitch_hint_get(e->reg[VIC_SET_INPUT0_LUMA_OFFSET]), hc = pitch_hint_get(e->reg[VIC_SET_INPUT0_CHROMA_OFFSET]);
    if (hl && hl == hc && hl >= w && l_avail >= (size_t)hl * (h - 1) + w && c_avail >= (size_t)hl * (ch - 1) + w) ip = hl; /* what NVDEC wrote the surfaces with */
    if ((!rgb && fmt != VIC_FMT_NV12) || bh > 5 || l_avail < ip * (h - 1) + w || c_avail < ip * (ch - 1) + w) {
        fail(e, 4);
        return;
    }
    if (fmt == VIC_FMT_NV12) {
        size_t op = align256(w);
        if (!ochroma || o_avail < plane_need(w, h, op, block, bh) || oc_avail < plane_need(w, ch, op, block, bh)) {
            fail(e, 4);
            return;
        }
        if (media_write_prepare) {
            media_write_prepare(out, o_avail);
            media_write_prepare(ochroma, oc_avail);
        }
        for (size_t y = 0; y < h; y++) put_row(out, luma + y * ip, w, y, op, block, bh);
        for (size_t y = 0; y < ch; y++) put_row(ochroma, chroma + y * ip, w, y, op, block, bh);
        e->st.frames++;
        vic_dump(e->seq, e->st.frames, luma, chroma, ip, w, h);
        if (media_trace() && e->st.frames % 60 == 1)
            fprintf(stderr, "[vic] NV12 frame %llu to luma %p: centre Y %u U %u V %u\n", (unsigned long long)e->st.frames, (void *)out, out[(h / 2) * op + w / 2],
                    ochroma[(ch / 2) * op + (w / 2 & ~(size_t)1)], ochroma[(ch / 2) * op + (w / 2 & ~(size_t)1) + 1]);
        return;
    }
    size_t op = w * 4;
    if (o_avail < plane_need(op, h, op, block, bh)) {
        fail(e, 4);
        return;
    }
    uint8_t *row = malloc(op);
    if (!row) {
        fail(e, 3);
        return;
    }
    if (media_write_prepare) media_write_prepare(out, o_avail);
    for (size_t y = 0; y < h; y++) { /* BT.601 limited range, 16.16 fixed point (as movie.c) */
        const uint8_t *yr = luma + y * ip, *uv = chroma + (y >> 1) * ip;
        for (size_t x = 0; x < w; x++) {
            int Y = ((yr[x] - 16) * 76310) >> 16, U = uv[x & ~(size_t)1] - 128, V = uv[(x & ~(size_t)1) + 1] - 128;
            uint8_t r = clamp8(Y + ((104597 * V) >> 16)), g = clamp8(Y - ((25675 * U + 53279 * V) >> 16)), b = clamp8(Y + ((132201 * U) >> 16));
            uint8_t *o = row + 4 * x;
            o[0] = fmt == VIC_FMT_BGRA8 ? b : r;
            o[1] = g;
            o[2] = fmt == VIC_FMT_BGRA8 ? r : b;
            o[3] = 255;
        }
        put_row(out, row, op, y, op, block, bh);
    }
    free(row);
    e->st.frames++;
    vic_dump(e->seq, e->st.frames, luma, chroma, ip, w, h);
}

/* ---- NVJPG: nvjpg_drv_pic_param_s at SET_IN_DRV_PIC_SETUP, application id 1 decode / 2 encode (nvdrv_media_regs.h) ---- */
static uint16_t rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static void nvjpg_status(MediaEngine *e, uint32_t w0, uint32_t mcu_x, uint32_t mcu_y, uint32_t err) {
    uint32_t st[NVJPG_STATUS_BYTES / 4] = {w0, mcu_x, mcu_y, 0 /* cycle_count */, err};
    if (err) fail(e, err);
    status_write(e, NVJPG_SET_OUT_STATUS, st, NVJPG_STATUS_BYTES / 4);
}
/* MCU size of a decoder chroma mode (NVJPG_CHROMA_*). */
static void nvjpg_mcu(uint32_t mode, uint32_t *mw, uint32_t *mh) {
    *mw = mode == NVJPG_CHROMA_420 || mode == NVJPG_CHROMA_422H ? 16 : 8;
    *mh = mode == NVJPG_CHROMA_420 || mode == NVJPG_CHROMA_422V ? 16 : 8;
}
/* Decode. The decoder half of the structure carries the driver's parse of the file (Huffman minimum codes, quantisers, block layout); the bitstream buffer is
 * the JPEG the driver parsed, bitstream_offset its entropy-coded data. Napple decodes the file itself from its SOI (at or before bitstream_offset) with
 * jpeg_core, and honours the output controls: output_type, power2_downscale, chroma_subsample, alpha, strides. A buffer with no SOI in front of the scan
 * (headers dropped by the driver) is refused with BAD_MAGIC: rebuilding a file from the hardware tables is not done (docs/reference/nvdrv-layouts.md). */
static void nvjpg_decode(MediaEngine *e, const uint8_t *pic) {
    size_t bs_avail = 0, o_avail = 0, u_avail = 0, v_avail = 0;
    uint8_t *bs = surf(e, e->reg[NVJPG_SET_BITSTREAM], &bs_avail), *out = surf(e, e->reg[NVJPG_SET_CUR_PIC], &o_avail);
    uint8_t *pu = surf(e, e->reg[NVJPG_SET_CUR_PIC_CHROMA_U], &u_avail), *pv = surf(e, e->reg[NVJPG_SET_CUR_PIC_CHROMA_V], &v_avail);
    uint32_t boff = rd32(pic + NVJPG_PIC_BITSTREAM_OFFSET), bsize = rd32(pic + NVJPG_PIC_BITSTREAM_SIZE), param = rd32(pic + NVJPG_PIC_DEC_FRAME_PARAM);
    uint32_t smode = (param >> 19) & 7, otype = (param >> 23) & 7, down = (param >> 26) & 3, sub = (param >> 28) & 3;
    uint32_t ls = rd32(pic + NVJPG_PIC_LUMA_STRIDE), cs = rd32(pic + NVJPG_PIC_CHROMA_STRIDE);
    uint8_t alpha = pic[NVJPG_PIC_DEC_OUTPUT_INFO + 3];
    if (!bs || !out || boff >= bs_avail) {
        nvjpg_status(e, 0, 0, 0, NVJPG_ERR_INVALID_INPUT);
        return;
    }
    size_t end = bsize && (uint64_t)boff + bsize <= bs_avail ? (size_t)boff + bsize : bs_avail, soi = (size_t)-1;
    for (size_t i = 0; i + 1 < end && i <= boff; i++)
        if (bs[i] == 0xff && bs[i + 1] == 0xd8) {
            soi = i;
            break;
        }
    if (soi == (size_t)-1) {
        nvjpg_status(e, 0, 0, 0, NVJPG_ERR_BAD_MAGIC);
        return;
    }
    int w = 0, h = 0, jerr = 0;
    JpegInfo ji;
    uint8_t *rgba = napjpeg_info(bs + soi, end - soi, &ji) == JPEGCORE_OK ? napjpeg_decode_rgba(bs + soi, end - soi, &w, &h, &jerr) : NULL;
    if (!rgba) {
        nvjpg_status(e, 0, 0, 0, NVJPG_ERR_INVALID_INPUT);
        return;
    }
    if (ji.ncomp == 1) smode = NVJPG_CHROMA_MONO;
    uint32_t mw, mh;
    nvjpg_mcu(smode, &mw, &mh);
    uint32_t mcu_x = ((uint32_t)w + mw - 1) / mw, mcu_y = ((uint32_t)h + mh - 1) / mh;
    if (down) { /* power2_downscale: 1/2, 1/4, 1/8 */
        int dw = (w + (1 << down) - 1) >> down, dh = (h + (1 << down) - 1) >> down;
        uint8_t *sc = napjpeg_scale_rgba(rgba, w, h, dw, dh);
        free(rgba);
        rgba = sc;
        w = dw;
        h = dh;
        if (!rgba) {
            nvjpg_status(e, 0, mcu_x, mcu_y, NVJPG_ERR_HWERR);
            return;
        }
    }
    uint32_t err = 0;
    if (otype >= NVJPG_OUT_RGBA && otype <= NVJPG_OUT_ARGB) { /* packed 32-bit pixels in byte order R,G,B,A / B,G,R,A / A,B,G,R / A,R,G,B */
        static const uint8_t order[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {3, 0, 1, 2}};
        const uint8_t *ord = order[otype - NVJPG_OUT_RGBA];
        size_t pitch = ls ? ls : (size_t)w * 4;
        if (pitch < (size_t)w * 4 || o_avail < pitch * (size_t)(h - 1) + (size_t)w * 4) err = NVJPG_ERR_INVALID_INPUT;
        else {
            if (media_write_prepare) media_write_prepare(out, pitch * (size_t)h);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    uint8_t px[4], *s0 = rgba + ((size_t)y * w + x) * 4, *d = out + (size_t)y * pitch + (size_t)x * 4;
                    memcpy(px, s0, 3);
                    px[3] = alpha ? alpha : 0xff;
                    for (int k = 0; k < 4; k++) d[k] = px[ord[k]];
                }
        }
    } else if (otype == NVJPG_OUT_YUV || otype == NVJPG_OUT_YUY2) {
        uint32_t cm = sub == 1 ? NVJPG_CHROMA_420 : sub == 2 ? NVJPG_CHROMA_422H : smode > NVJPG_CHROMA_444 ? NVJPG_CHROMA_444 : smode;
        if (otype == NVJPG_OUT_YUY2) cm = NVJPG_CHROMA_422H;
        int hs = cm == NVJPG_CHROMA_420 || cm == NVJPG_CHROMA_422H ? 2 : 1, vs = cm == NVJPG_CHROMA_420 || cm == NVJPG_CHROMA_422V ? 2 : 1;
        int cw = (w + hs - 1) / hs, ch = (h + vs - 1) / vs;
        /* JFIF full-range BT.601 */
#define NVJ_Y(p) clamp8((int)(0.299f * (p)[0] + 0.587f * (p)[1] + 0.114f * (p)[2] + 0.5f))
        if (otype == NVJPG_OUT_YUY2) { /* Y0 U Y1 V */
            size_t pitch = ls ? ls : (size_t)cw * 4;
            if (pitch < (size_t)cw * 4 || o_avail < pitch * (size_t)h) err = NVJPG_ERR_INVALID_INPUT;
            for (int y = 0; !err && y < h; y++)
                for (int x = 0; x < cw; x++) {
                    const uint8_t *a0 = rgba + ((size_t)y * w + 2 * x) * 4, *a1 = 2 * x + 1 < w ? a0 + 4 : a0;
                    int r = (a0[0] + a1[0]) / 2, g = (a0[1] + a1[1]) / 2, b = (a0[2] + a1[2]) / 2;
                    uint8_t *d = out + (size_t)y * pitch + (size_t)x * 4;
                    d[0] = NVJ_Y(a0);
                    d[1] = clamp8((int)(-0.168736f * r - 0.331264f * g + 0.5f * b + 128.5f));
                    d[2] = NVJ_Y(a1);
                    d[3] = clamp8((int)(0.5f * r - 0.418688f * g - 0.081312f * b + 128.5f));
                }
        } else {
            size_t yp = ls ? ls : (size_t)w, planar = pv != NULL, step = planar ? 1 : 2, cp = cs ? cs : (size_t)cw * step;
            if (yp < (size_t)w || o_avail < yp * (size_t)(h - 1) + (size_t)w) err = NVJPG_ERR_INVALID_INPUT;
            else if (cm != NVJPG_CHROMA_MONO && (!pu || cp < (size_t)cw * step || u_avail < cp * (size_t)(ch - 1) + (size_t)cw * step ||
                                                 (planar && v_avail < cp * (size_t)(ch - 1) + (size_t)cw)))
                err = NVJPG_ERR_INVALID_INPUT;
            for (int y = 0; !err && y < h; y++)
                for (int x = 0; x < w; x++) out[(size_t)y * yp + x] = NVJ_Y(rgba + ((size_t)y * w + x) * 4);
            for (int y = 0; !err && cm != NVJPG_CHROMA_MONO && y < ch; y++)
                for (int x = 0; x < cw; x++) { /* the average of the block of samples each chroma sample covers */
                    int r = 0, g = 0, b = 0, n = 0;
                    for (int dy = 0; dy < vs; dy++)
                        for (int dx = 0; dx < hs; dx++) {
                            int yy = y * vs + dy, xx = x * hs + dx;
                            if (yy >= h || xx >= w) continue;
                            const uint8_t *q = rgba + ((size_t)yy * w + xx) * 4;
                            r += q[0], g += q[1], b += q[2], n++;
                        }
                    r /= n, g /= n, b /= n;
                    uint8_t U = clamp8((int)(-0.168736f * r - 0.331264f * g + 0.5f * b + 128.5f)), V = clamp8((int)(0.5f * r - 0.418688f * g - 0.081312f * b + 128.5f));
                    if (planar) {
                        pu[(size_t)y * cp + x] = U;
                        pv[(size_t)y * cp + x] = V;
                    } else {
                        pu[(size_t)y * cp + 2 * x] = U;
                        pu[(size_t)y * cp + 2 * x + 1] = V;
                    }
                }
        }
#undef NVJ_Y
    } else {
        err = NVJPG_ERR_INVALID_INPUT; /* output types 6, 7 are not defined */
    }
    free(rgba);
    if (!err) e->st.frames++;
    nvjpg_status(e, (uint32_t)(end > boff ? end - boff : 0), mcu_x, mcu_y, err);
}
/* Encode: YUV from CUR_PIC (+ chroma) with the caller's quantisers and Huffman codes; the entropy-coded data alone goes to the bitstream buffer at
 * bitstream_offset (the driver writes the headers in front). Width / height: pdma width_in_8x8 / height_in_8x8 minus the pad fields; chroma format from
 * ENC_TOP_CTL; memory_mode 0 semi-planar (interleaved UV in CHROMA_U), 1 planar (the nvenc_drv.h convention of the same field). The AC tables have 176 entries:
 * 16 runs x 11 sizes, index = run * 11 + size (162 used: sizes 1..10 plus EOB 0x00 and ZRL 0xF0), DC tables 12 entries indexed by category. */
static void nvjpg_encode(MediaEngine *e, const uint8_t *pic) {
    size_t y_avail = 0, u_avail = 0, v_avail = 0, o_avail = 0;
    uint8_t *py = surf(e, e->reg[NVJPG_SET_CUR_PIC], &y_avail), *pu = surf(e, e->reg[NVJPG_SET_CUR_PIC_CHROMA_U], &u_avail);
    uint8_t *pv = surf(e, e->reg[NVJPG_SET_CUR_PIC_CHROMA_V], &v_avail), *out = surf(e, e->reg[NVJPG_SET_BITSTREAM], &o_avail);
    uint32_t top = rd32(pic + NVJPG_PIC_ENC_TOP_CTL), info = rd32(pic + NVJPG_PIC_PDMA_PIC_INFO), info1 = rd32(pic + NVJPG_PIC_PDMA_PIC_INFO1);
    uint32_t chroma = top & 3, mcu_w = (top >> 2) & 0xfff, mcu_h = (top >> 14) & 0xfff;
    int hs = chroma == JPEGSCAN_422H || chroma == JPEGSCAN_420 ? 2 : 1, vs = chroma == JPEGSCAN_422V || chroma == JPEGSCAN_420 ? 2 : 1;
    int w = (int)(((info >> 6) & 0xfff) * 8) - (int)((info1 >> 21) & 15), h = (int)(((info >> 18) & 0xfff) * 8) - (int)((info1 >> 25) & 15);
    if (!((info >> 6) & 0xfff)) w = (int)mcu_w * 8 * hs, h = (int)mcu_h * 8 * vs;
    uint32_t mem = (info1 >> 29) & 7, boff = rd32(pic + NVJPG_PIC_BITSTREAM_OFFSET), bcap = rd32(pic + NVJPG_PIC_BITSTREAM_SIZE);
    JpegScanParams sp;
    memset(&sp, 0, sizeof sp);
    sp.width = w;
    sp.height = h;
    sp.chroma = (int)chroma;
    sp.y = py;
    sp.u = pu;
    sp.v = pv;
    sp.uv_interleaved = mem == 0;
    sp.y_stride = (int)(rd32(pic + NVJPG_PIC_LUMA_STRIDE) ? rd32(pic + NVJPG_PIC_LUMA_STRIDE) : (uint32_t)w);
    int cw = (w + hs - 1) / hs, ch = (h + vs - 1) / vs;
    sp.uv_stride = (int)(rd32(pic + NVJPG_PIC_CHROMA_STRIDE) ? rd32(pic + NVJPG_PIC_CHROMA_STRIDE) : (uint32_t)(cw * (sp.uv_interleaved ? 2 : 1)));
    sp.restart_interval = (int)(rd32(pic + NVJPG_PIC_ENC_RST_INTERVAL) & 0xffff);
    uint32_t mcus_x = w > 0 ? (uint32_t)((w + 8 * hs - 1) / (8 * hs)) : 0, mcus_y = h > 0 ? (uint32_t)((h + 8 * vs - 1) / (8 * vs)) : 0, err = 0;
    for (int t = 0; t < 2 && !err; t++)
        for (int i = 0; i < 64; i++) {
            uint32_t f = rd16(pic + NVJPG_PIC_ENC_QUANT + (size_t)t * 128 + (size_t)i * 2), q = f ? (32768u + f / 2) / f : 0;
            if (!q) err = NVJPG_ERR_INVALID_INPUT;
            sp.qt[t][i] = (uint16_t)(q > 255 ? 255 : q);
        }
    static const unsigned base[4] = {0, 12, 188, 200}; /* hfDcLuma[12], hfAcLuma[176], hfDcChroma[12], hfAcChroma[176] */
    for (int t = 0; t < 2; t++) {
        const uint8_t *dc = pic + NVJPG_PIC_ENC_HUFF + (size_t)base[2 * t] * 4, *ac = pic + NVJPG_PIC_ENC_HUFF + (size_t)base[2 * t + 1] * 4;
        for (int c = 0; c < 12; c++) {
            sp.dc[t].len[c] = (uint8_t)(rd16(dc + c * 4) & 31);
            sp.dc[t].code[c] = rd16(dc + c * 4 + 2);
        }
        for (int run = 0; run < 16; run++)
            for (int size = 0; size <= 10; size++) {
                if (!size && run != 0 && run != 15) continue;
                const uint8_t *it = ac + (size_t)(run * 11 + size) * 4;
                sp.ac[t].len[(run << 4) | size] = (uint8_t)(rd16(it) & 31);
                sp.ac[t].code[(run << 4) | size] = rd16(it + 2);
            }
    }
    size_t need_y = w > 0 && h > 0 ? (size_t)sp.y_stride * (size_t)(h - 1) + (size_t)w : 0;
    size_t need_c = ch > 0 ? (size_t)sp.uv_stride * (size_t)(ch - 1) + (size_t)cw * (sp.uv_interleaved ? 2 : 1) : 0;
    if (!err && (w <= 0 || h <= 0 || !py || !pu || !out || (!sp.uv_interleaved && mem != 1) || (mem == 1 && !pv) || y_avail < need_y || u_avail < need_c ||
                 (mem == 1 && v_avail < need_c) || boff >= o_avail || sp.y_stride < w))
        err = NVJPG_ERR_INVALID_INPUT;
    size_t n = 0;
    uint8_t *data = err ? NULL : napjpeg_encode_scan(&sp, &n);
    if (!err && !data) err = NVJPG_ERR_INVALID_INPUT;
    size_t cap = o_avail - (err ? 0 : boff);
    if (!err && bcap && bcap < cap) cap = bcap;
    if (!err && n > cap) err = NVJPG_ERR_INVALID_INPUT; /* the bitstream buffer is too small */
    if (!err) {
        if (media_write_prepare) media_write_prepare(out + boff, n);
        memcpy(out + boff, data, n);
        e->st.frames++;
    }
    free(data);
    nvjpg_status(e, err ? 0 : (uint32_t)n, mcus_x, mcus_y, err);
}
static void nvjpg_execute(MediaEngine *e) {
    size_t avail = 0;
    const uint8_t *pic = surf(e, e->reg[NVJPG_SET_IN_DRV_PIC_SETUP], &avail);
    int enc = e->reg[NVM_SET_APPLICATION_ID] == NVJPG_APP_ENCODER;
    if (!pic || avail < (enc ? (size_t)NVJPG_PIC_BITSTREAM_SIZE + 4 : (size_t)NVJPG_PIC_BYTES)) {
        nvjpg_status(e, 0, 0, 0, NVJPG_ERR_INVALID_INPUT);
        return;
    }
    if (enc) nvjpg_encode(e, pic);
    else nvjpg_decode(e, pic);
}

/* ---- MSENC: one NV12 frame per execute through the EncoderBackend (nvenc_h264_drv_pic_setup_s in, nvenc_pic_stat_s out) ---- */
static void msenc_status(MediaEngine *e, uint32_t err, size_t bytes, int idr) {
    uint8_t st[MSENC_STAT_BYTES] = {0};
    wr32(st + MSENC_STAT_PICTURE_INDEX, e->reg[MSENC_SET_PICTURE_INDEX]);
    wr32(st + MSENC_STAT_ERROR, err ? (1u | (err << 2)) : 0); /* b0-1 error_status, b2-31 ucode_error_status (Napple's code) */
    wr32(st + MSENC_STAT_TOTAL_BITS, (uint32_t)(bytes * 8));
    uint16_t pt = idr ? MSENC_PIC_TYPE_IDR : MSENC_PIC_TYPE_P, slices = bytes ? 1 : 0;
    memcpy(st + MSENC_STAT_PIC_TYPE, &pt, 2);
    memcpy(st + MSENC_STAT_NUM_SLICES, &slices, 2);
    wr32(st + MSENC_STAT_LAST_VALID_BYTE, bytes ? (uint32_t)(bytes - 1) : 0);
    size_t avail;
    uint8_t *p = surf(e, e->reg[MSENC_SET_OUT_ENC_STATUS], &avail);
    if (p && avail >= MSENC_STAT_BYTES) memcpy(p, st, MSENC_STAT_BYTES);
}
static void msenc_execute(MediaEngine *e) {
    size_t su_avail, l_avail, c_avail, v_avail, o_avail;
    uint8_t *su = surf(e, e->reg[MSENC_SET_IN_DRV_PIC_SETUP], &su_avail), *luma = surf(e, e->reg[MSENC_SET_IN_CUR_PIC], &l_avail),
            *chroma = surf(e, e->reg[MSENC_SET_IN_CUR_PIC_CHROMA_U], &c_avail), *cv = surf(e, e->reg[MSENC_SET_IN_CUR_PIC_CHROMA_V], &v_avail),
            *out = surf(e, e->reg[MSENC_SET_OUT_BITSTREAM], &o_avail);
    if (!su && !e->reg[MSENC_SET_IN_DRV_PIC_SETUP]) { /* nothing configured: accepted and counted, no bitstream */
        msenc_status(e, 0, 0, 0);
        return;
    }
    uint32_t err = 0;
    if (!su || su_avail < MSENC_PIC_BYTES || !luma || !chroma || !out) err = MSENC_ERR_ADDRESS;
    uint32_t w = 0, h = 0, lp = 0, cp = 0, br = 0, fps = 0, planar = 0;
    int idr = 0;
    uint8_t *nv12c = NULL;
    if (!err) {
        const uint8_t *cfg = su + MSENC_PIC_INPUT_CFG;
        w = (uint32_t)rd16(cfg + MSENC_CFG_WIDTH_M1) + 1, h = (uint32_t)rd16(cfg + MSENC_CFG_HEIGHT_M1) + 1;
        lp = rd16(cfg + MSENC_CFG_PITCH), cp = rd16(cfg + MSENC_CFG_PITCH_CHROMA), planar = (rd32(cfg + MSENC_CFG_FLAGS) >> 8) & 3;
        int32_t f = (int32_t)rd32(su + MSENC_PIC_RC_FRAMERATE), b = (int32_t)rd32(su + MSENC_PIC_RC_NAL_BITRATE);
        fps = f > 0 && f <= 240 ? (uint32_t)f : 0;
        br = b > 0 ? (uint32_t)b : 0;
        idr = ((su[MSENC_PIC_PIC_TYPE_BYTE] >> 2) & 3) == MSENC_PIC_TYPE_IDR;
        size_t lpe = lp ? lp : w, cpe = cp ? cp : (planar == 1 ? (w + 1) / 2 : w);
        if ((w & 1) || (h & 1) || w > 4096 || h > 4096 || lpe < w || lpe > 0x10000 || cpe > 0x10000 || l_avail < lpe * (h - 1) + w || planar > 1 ||
            (planar == 0 && (cpe < w || c_avail < cpe * (h / 2 - 1) + w)) ||
            (planar == 1 && (!cv || cpe < w / 2 || c_avail < cpe * (h / 2 - 1) + w / 2 || v_avail < cpe * (h / 2 - 1) + w / 2)))
            err = MSENC_ERR_PARAMS;
        if (!err && planar == 1) { /* planar input: interleave U and V for the NV12 backend */
            nv12c = malloc((size_t)w * (h / 2));
            if (!nv12c) err = MSENC_ERR_FAILED;
            for (uint32_t y = 0; !err && y < h / 2; y++)
                for (uint32_t x = 0; x < w / 2; x++) {
                    nv12c[(size_t)y * w + 2 * x] = chroma[(size_t)y * cpe + x];
                    nv12c[(size_t)y * w + 2 * x + 1] = cv[(size_t)y * cpe + x];
                }
            if (!err) {
                chroma = nv12c;
                c_avail = (size_t)w * (h / 2);
                cp = w;
            }
        }
    }
    if (!err && !e->enc) {
        e->enc = e->encoder->open(DEC_CODEC_H264);
        if (!e->enc) err = MSENC_ERR_CODEC;
    }
    EncodeResult rs = {0};
    if (!err) {
        EncodeRequest rq = {DEC_CODEC_H264, (int)w, (int)h, luma, chroma, l_avail, c_avail, lp, cp, br, fps, idr, out, o_avail};
        if (media_write_prepare) media_write_prepare(out, rq.out_cap);
        if (e->encoder->encode(e->enc, &rq, &rs) < 0 || rs.size > rq.out_cap) err = MSENC_ERR_FAILED;
    }
    free(nv12c);
    if (err) fail(e, err);
    else {
        e->st.frames++;
        e->enc_frames++;
    }
    msenc_status(e, err, err ? 0 : rs.size, !err && rs.idr);
}

static void execute(MediaEngine *e) {
    e->st.executes++;
    trace_regs(e);
    if (media_trace() && e->st.executes % 60 == 0)
        fprintf(stderr, "[media] class %#x: %llu executes, %llu frames, %llu errors (last %u), %llu configures\n", e->class_id, (unsigned long long)e->st.executes,
                (unsigned long long)e->st.frames, (unsigned long long)e->st.errors, e->st.last_error, (unsigned long long)e->st.configures);
    switch (e->class_id) {
        case MEDIA_CLASS_NVDEC: nvdec_execute(e); break;
        case MEDIA_CLASS_VIC: vic_execute(e); break;
        case MEDIA_CLASS_NVJPG: nvjpg_execute(e); break;
        case MEDIA_CLASS_MSENC: msenc_execute(e); break;
    }
}
static int known_reg(const MediaEngine *e, uint32_t off) {
    if (off == NVM_SET_APPLICATION_ID || off == NVM_EXECUTE) return 1;
    if (off < 0x100 || off > 0x1ff) return 0;
    switch (e->class_id) {
        case MEDIA_CLASS_NVDEC:
            return off <= NVDEC_SET_EXTERNAL_MVBUFFER_OFFSET || off == NVDEC_H264_SET_MBHIST_BUF_OFFSET || off == NVDEC_VP8_SET_PROB_DATA_OFFSET ||
                   off == NVDEC_VP8_SET_HEADER_PARTITION_BUF_BASE_OFFSET || (off >= NVDEC_VP9_SET_PROB_TAB_BUF_OFFSET && off <= NVDEC_VP9_SET_FILTER_BUFFER_OFFSET);
        case MEDIA_CLASS_VIC:
            return off == VIC_SET_INPUT0_LUMA_OFFSET || off == VIC_SET_INPUT0_CHROMA_OFFSET || off == VIC_SET_CONTROL_PARAMS || off == VIC_SET_CONFIG_STRUCT_OFFSET ||
                   off == VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET || off == VIC_SET_OUTPUT_SURFACE_CHROMA_OFFSET || (off >= VIC_SET_CONFIG_STRUCT_OFFSET && off <= 0x1cf);
        case MEDIA_CLASS_NVJPG: return off >= NVJPG_SET_CONTROL_PARAMS && off <= NVJPG_SET_CUR_PIC_CHROMA_V;
        default: /* MSENC: the reference pictures and the 0x1c0 block of clc5b7 */
            return (off >= MSENC_SET_IN_REF_PIC0_LUMA && off < MSENC_SET_IN_REF_PIC0_LUMA + 32) || (off >= 0x140 && off <= 0x152) ||
                   (off >= MSENC_SET_CONTROL_PARAMS && off <= 0x1d9);
    }
}
static void write_reg(MediaEngine *e, uint32_t off, uint32_t v) {
    e->st.regwrites++;
    if (e->cur_class == 0) { /* host1x itself: offset 0 is INCR_SYNCPT (the SUBMIT's declared increments carry the counts), the rest is ignored */
        if (off == 0) e->st.syncpt_incrs++;
        else e->st.foreign_writes++;
        return;
    }
    if (e->cur_class != e->class_id || off >= NREGS) {
        e->st.foreign_writes++;
        return;
    }
    if (off == 0) { /* the engine class's THI word 0 is INCR_SYNCPT too (MK8D's NVDEC and VIC streams end with one per submit): counted, as above */
        e->st.syncpt_incrs++;
        return;
    }
    if (off == NVM_THI_METHOD0) { /* the SDK drives the engines through the THI: a (method, data) pair of writes to words 0x10 / 0x11 */
        e->thi_method = v & 0xfff;
        e->st.thi_pairs++;
        return;
    }
    if (off == NVM_THI_METHOD1) {
        write_reg(e, e->thi_method, v);
        return;
    }
    e->reg[off] = v;
    if (off == NVM_EXECUTE) {
        execute(e);
        return;
    }
    if (!known_reg(e, off)) {
        e->st.unknown_regs++;
        e->st.last_unknown_reg = off;
    }
}

static int run(MediaEngine *e, const uint32_t *w, uint32_t n, int depth) {
    uint32_t i = 0;
    while (i < n) {
        uint32_t op = w[i++], code = op >> 28, off = (op >> 16) & 0xfff;
        e->st.ops++;
        e->st.words++;
        switch (code) {
            case 0: { /* SETCL offset:12 classid:10 mask:6 */
                e->cur_class = (op >> 6) & 0x3ff;
                uint32_t mask = op & 0x3f;
                for (uint32_t b = 0; b < 6; b++)
                    if (mask & (1u << b)) {
                        if (i >= n) return MEDIA_BAD_STREAM;
                        e->st.words++;
                        write_reg(e, off + b, w[i++]);
                    }
                break;
            }
            case 1:
            case 2: { /* INCR / NONINCR offset:12 count:16 */
                uint32_t cnt = op & 0xffff;
                if (n - i < cnt) return MEDIA_BAD_STREAM;
                for (uint32_t k = 0; k < cnt; k++) {
                    e->st.words++;
                    write_reg(e, code == 1 ? off + k : off, w[i++]);
                }
                break;
            }
            case 3: { /* MASK offset:12 mask:16 */
                uint32_t mask = op & 0xffff;
                for (uint32_t b = 0; b < 16; b++)
                    if (mask & (1u << b)) {
                        if (i >= n) return MEDIA_BAD_STREAM;
                        e->st.words++;
                        write_reg(e, off + b, w[i++]);
                    }
                break;
            }
            case 4: write_reg(e, off, op & 0xffff); break; /* IMM */
            case 5: break;                                 /* RESTART: stream position bookkeeping of the real engine, no register effect */
            case 6: { /* GATHER: followed by the buffer's device address. Without bit 15 the buffer holds more commands; with it (register gather) its
                       * count words are data for the register at offset (bit 14: incrementing). */
                uint32_t cnt = op & 0x3fff, regdata = (op >> 15) & 1, incr = (op >> 14) & 1;
                if (i >= n) return MEDIA_BAD_STREAM;
                e->st.words++;
                uint32_t addr = w[i++];
                size_t avail = 0;
                uint8_t *p = e->mem.resolve ? e->mem.resolve(e->mem.ctx, addr, &avail) : NULL;
                if (!p || avail < (size_t)cnt * 4 || depth >= MAX_GATHER_DEPTH) return MEDIA_BAD_ADDRESS;
                uint32_t *tmp = malloc((size_t)cnt * 4 + 4);
                if (!tmp) return MEDIA_BAD_STREAM;
                memcpy(tmp, p, (size_t)cnt * 4);
                int r = MEDIA_OK;
                if (regdata) {
                    for (uint32_t k = 0; k < cnt; k++) write_reg(e, incr ? off + k : off, tmp[k]);
                    e->st.words += cnt;
                } else r = run(e, tmp, cnt, depth + 1);
                free(tmp);
                if (r) return r;
                break;
            }
            case 14: break; /* EXTEND: ACQUIRE_MUTEX / RELEASE_MUTEX: the engine is never contended */
            default: return MEDIA_BAD_STREAM;
        }
    }
    return MEDIA_OK;
}
int media_engine_run(MediaEngine *e, const uint32_t *w, uint32_t n) {
    e->cur_class = e->class_id;
    return run(e, w, n, 0);
}

/* ---- /dev/nvdisp-disp0: tegra_dc_ext (see nvdrv_media_core.h for the layouts and the source) ------------------------------------------ */
void nvdisp_init(NvDisp *d, uint64_t now_ns, int docked) {
    memset(d, 0, sizeof *d);
    d->docked = docked ? 1 : 0;
    d->width = docked ? 1920 : 1280;
    d->height = docked ? 1080 : 720;
    d->refresh_mhz = 60000;
    d->t0_ns = now_ns;
    d->nvmap_fd = -1;
    d->winmask = 1; /* window A drives the head */
}
uint64_t nvdisp_vblank_count(const NvDisp *d, uint64_t now_ns) {
    uint64_t el = now_ns > d->t0_ns ? now_ns - d->t0_ns : 0, sec = el / 1000000000u, rem = el % 1000000000u;
    return (sec * d->refresh_mhz + rem * d->refresh_mhz / 1000000000u) / 1000u;
}
static uint64_t vblank_time(const NvDisp *d, uint64_t count) { return d->t0_ns + count * 1000000000000ull / d->refresh_mhz; }
/* Vertical total of the CEA-861 timing of the mode (720p: 750 lines, 1080p: 1125): the scanline counter runs over it once per frame. */
static uint32_t disp_vtotal(const NvDisp *d) { return d->height == 720 ? 750 : d->height == 1080 ? 1125 : d->height + d->height / 24; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t get16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
int nvdisp_ioctl(NvDisp *d, unsigned nr, uint8_t *data, unsigned size, uint64_t now_ns) {
    switch (nr) {
        case NVDISP_SET_NVMAP_FD:
            if (size < NVDISP_SZ_S32) return NVDISP_BAD_PARAMETER;
            d->nvmap_fd = (int32_t)rd32(data);
            return NVDISP_OK;
        case NVDISP_GET_WINDOW: /* claim window n of the head (the driver's -EBUSY when another client holds it) */
        case NVDISP_PUT_WINDOW: {
            if (size < NVDISP_SZ_S32) return NVDISP_BAD_PARAMETER;
            uint32_t n = rd32(data);
            if (n >= NVDISP_WINDOWS) return NVDISP_BAD_PARAMETER;
            int owned = (d->windows >> n) & 1;
            if (nr == NVDISP_GET_WINDOW ? owned : !owned) return NVDISP_INVALID_STATE;
            d->windows ^= 1u << n;
            return NVDISP_OK;
        }
        case NVDISP_GET_CURSOR:
        case NVDISP_PUT_CURSOR:
            if (nr == NVDISP_GET_CURSOR ? d->cursor_owned : !d->cursor_owned) return NVDISP_INVALID_STATE;
            d->cursor_owned = nr == NVDISP_GET_CURSOR;
            return NVDISP_OK;
        case NVDISP_SET_CURSOR_IMAGE: /* {fg rgb, bg rgb, pad, buff_id, flags (size), x, y, alpha, colorfmt}: only the cursor's owner */
            if (size < NVDISP_SZ_CURSOR_IMAGE) return NVDISP_BAD_PARAMETER;
            if (!d->cursor_owned) return NVDISP_INVALID_STATE;
            if ((rd32(data + 12) & 7) > 4) return NVDISP_BAD_PARAMETER; /* SIZE field: 0 none, 1 32x32 .. 4 256x256 */
            memcpy(d->cursor_fg, data, 3);
            memcpy(d->cursor_bg, data + 3, 3);
            d->cursor_buff_id = rd32(data + 8);
            d->cursor_image_flags = rd32(data + 12);
            d->cursor_x = (int16_t)get16(data + 16);
            d->cursor_y = (int16_t)get16(data + 18);
            d->cursor_alpha = rd32(data + 20);
            d->cursor_colorfmt = rd32(data + 24);
            return NVDISP_OK;
        case NVDISP_SET_CURSOR: /* {s16 x, s16 y, u32 flags (bit0 visible)} */
            if (size < NVDISP_SZ_CURSOR) return NVDISP_BAD_PARAMETER;
            if (!d->cursor_owned) return NVDISP_INVALID_STATE;
            d->cursor_x = (int16_t)get16(data);
            d->cursor_y = (int16_t)get16(data + 2);
            d->cursor_flags = rd32(data + 4);
            return NVDISP_OK;
        case NVDISP_CURSOR_CLIP:
            if (size < NVDISP_SZ_S32) return NVDISP_BAD_PARAMETER;
            if (!d->cursor_owned) return NVDISP_INVALID_STATE;
            d->cursor_clip = (int32_t)rd32(data);
            return NVDISP_OK;
        case NVDISP_SET_CSC: { /* {u32 win_index, u16 yof, kyrgb, kur, kvr, kug, kvg, kub, kvb} */
            if (size < NVDISP_SZ_CSC) return NVDISP_BAD_PARAMETER;
            uint32_t w = rd32(data);
            if (w >= NVDISP_WINDOWS) return NVDISP_BAD_PARAMETER;
            for (unsigned i = 0; i < 8; i++) d->csc[w][i] = get16(data + 4 + 2 * i);
            return NVDISP_OK;
        }
        case NVDISP_GET_STATUS: /* nr 9 twice: GET_VBLANK_SYNCPT is the 4-byte form, GET_STATUS the 16-byte one */
            if (size == NVDISP_SZ_S32) {
                put32(data, d->syncpt);
                return NVDISP_OK;
            }
            if (size < NVDISP_SZ_STATUS) return NVDISP_BAD_PARAMETER;
            memset(data, 0, NVDISP_SZ_STATUS);
            put32(data, NVDISP_STATUS_ENABLED); /* the head is always on: the host presents every frame */
            return NVDISP_OK;
        case NVDISP_SET_LUT: { /* {win_index, flags, start, len, u64 r, g, b}: the tables live in guest memory and are not read (no scanout here) */
            if (size < NVDISP_SZ_LUT) return NVDISP_BAD_PARAMETER;
            uint32_t w = rd32(data), start = rd32(data + 8), len = rd32(data + 12);
            if (w >= NVDISP_WINDOWS || start > 256 || len > 256 - start) return NVDISP_BAD_PARAMETER;
            d->lut_flags[w] = rd32(data + 4);
            d->lut_start[w] = start;
            d->lut_len[w] = len;
            return NVDISP_OK;
        }
        case NVDISP_SET_CMU:
        case NVDISP_SET_CMU_ALIGNED:
            if (size < NVDISP_SZ_CMU) return NVDISP_BAD_PARAMETER;
            memcpy(&d->cmu, data, NVDISP_SZ_CMU);
            d->cmu_custom = 1;
            return NVDISP_OK;
        case NVDISP_GET_CMU:        /* the CMU in effect: the custom one once set, else disabled (enable 0, tables 0) */
        case NVDISP_GET_CUSTOM_CMU: /* the one the client set (zero before) */
        case NVDISP_GET_CMU_ADBRGB:
            if (size < NVDISP_SZ_CMU) return NVDISP_BAD_PARAMETER;
            if (d->cmu_custom) memcpy(data, &d->cmu, NVDISP_SZ_CMU);
            else memset(data, 0, NVDISP_SZ_CMU);
            return NVDISP_OK;
        case NVDISP_SET_VBLANK: /* {u8 enable, u8 reserved[3] (must be 0)} */
            if (size < NVDISP_SZ_SET_VBLANK) return NVDISP_BAD_PARAMETER;
            if (data[1] || data[2] || data[3]) return NVDISP_BAD_PARAMETER;
            d->vblank_enabled = data[0] != 0;
            return NVDISP_OK;
        case NVDISP_GET_WINMASK:
            if (size < NVDISP_SZ_S32) return NVDISP_BAD_PARAMETER;
            put32(data, d->winmask);
            return NVDISP_OK;
        case NVDISP_SET_WINMASK:
            if (size < NVDISP_SZ_S32) return NVDISP_BAD_PARAMETER;
            if (rd32(data) & ~((1u << NVDISP_WINDOWS) - 1)) return NVDISP_BAD_PARAMETER;
            d->winmask = rd32(data);
            return NVDISP_OK;
        case NVDISP_GET_SCANLINE: { /* the line the raster is on now: position inside the current frame scaled to the vertical total */
            if (size < NVDISP_SZ_S32) return NVDISP_BAD_PARAMETER;
            uint64_t n = nvdisp_vblank_count(d, now_ns), t0 = vblank_time(d, n), period = 1000000000000ull / d->refresh_mhz;
            uint64_t in = now_ns > t0 ? now_ns - t0 : 0;
            uint32_t line = (uint32_t)(in * disp_vtotal(d) / period);
            put32(data, line < disp_vtotal(d) ? line : disp_vtotal(d) - 1);
            return NVDISP_OK;
        }
        default: return NVDISP_NOT_IMPLEMENTED;
    }
}
