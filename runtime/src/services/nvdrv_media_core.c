#include "nvdrv_media_core.h"
#include "jpeg_core.h"
#include "nvdrv_media_regs.h"
#include <stdlib.h>
#include <string.h>

#define NREGS 0x1000
#define MAX_GATHER_DEPTH 4
struct MediaEngine {
    uint32_t class_id, cur_class;
    NvMediaMem mem;
    const DecoderBackend *backend;
    void *dec;
    int dec_codec;
    uint32_t reg[NREGS];
    MediaStats st;
};

MediaEngine *media_engine_new(uint32_t class_id, const NvMediaMem *mem) {
    if (class_id != MEDIA_CLASS_NVDEC && class_id != MEDIA_CLASS_VIC && class_id != MEDIA_CLASS_NVJPG && class_id != MEDIA_CLASS_MSENC) return NULL;
    MediaEngine *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->class_id = e->cur_class = class_id;
    if (mem) e->mem = *mem;
    e->backend = decoder_backend_current();
    return e;
}
void media_engine_free(MediaEngine *e) {
    if (!e) return;
    if (e->dec) e->backend->close(e->dec);
    free(e);
}
const MediaStats *media_engine_stats(const MediaEngine *e) { return &e->st; }
uint32_t media_engine_reg(const MediaEngine *e, uint32_t off) { return off < NREGS ? e->reg[off] : 0; }
uint32_t media_engine_class(const MediaEngine *e) { return e->class_id; }

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
static void nvdec_execute(MediaEngine *e) {
    int codec = (int)e->reg[NVM_SET_APPLICATION_ID];
    uint32_t status[8] = {0};
    if (e->dec && e->dec_codec != codec) {
        e->backend->close(e->dec);
        e->dec = NULL;
    }
    if (!e->dec) {
        e->dec = e->backend->open(codec);
        e->dec_codec = codec;
    }
    if (!e->dec) {
        fail(e, 1); /* codec not supported by the backend */
        status[3] = 1;
        status_write(e, NVDEC_SET_STATUS_OFFSET, status, 8);
        return;
    }
    size_t pic_avail = 0, bs_avail = 0, luma_avail = 0, chroma_avail = 0;
    uint8_t *pic = surf(e, e->reg[NVDEC_SET_DRV_PIC_SETUP_OFFSET], &pic_avail);
    uint8_t *bs = surf(e, e->reg[NVDEC_SET_IN_BUF_BASE_OFFSET], &bs_avail);
    uint32_t idx = e->reg[NVDEC_SET_PICTURE_INDEX];
    uint32_t lreg = idx < NVDEC_PIC_SLOTS ? e->reg[NVDEC_SET_PICTURE_LUMA_OFFSET0 + idx] : 0,
             creg = idx < NVDEC_PIC_SLOTS ? e->reg[NVDEC_SET_PICTURE_CHROMA_OFFSET0 + idx] : 0;
    if (!lreg) lreg = e->reg[NVDEC_SET_DISPLAY_BUF_LUMA_OFFSET];
    if (!creg) creg = e->reg[NVDEC_SET_DISPLAY_BUF_CHROMA_OFFSET];
    uint8_t *luma = surf(e, lreg, &luma_avail), *chroma = surf(e, creg, &chroma_avail);
    size_t bs_len = bs_avail;
    static const int len_word_h264 = NVDEC_PIC_STREAM_LEN_WORD_H264, len_word_other = NVDEC_PIC_STREAM_LEN_WORD_VP9;
    int lw = codec == DEC_CODEC_H264 ? len_word_h264 : len_word_other;
    if (pic && pic_avail >= (size_t)(lw + 1) * 4) {
        uint32_t l = rd32(pic + (size_t)lw * 4);
        if (l && l <= bs_avail) bs_len = l;
    }
    if (!bs || !luma || !chroma) {
        fail(e, 2);
        status[3] = 2;
        status_write(e, NVDEC_SET_STATUS_OFFSET, status, 8);
        return;
    }
    DecodeRequest rq = {codec, pic, pic_avail, bs, bs_len, idx, luma, chroma, luma_avail, chroma_avail};
    DecodeResult rs = {0};
    int rc = e->backend->decode(e->dec, &rq, &rs);
    if (rc < 0) {
        fail(e, 3);
        status[3] = 3;
    } else if (rc == 0) {
        e->st.frames++;
        status[0] = (uint32_t)(((rs.width + 15) / 16) * ((rs.height + 15) / 16)); /* macroblocks correct */
        status[4] = (uint32_t)rs.width;
        status[5] = (uint32_t)rs.height;
    }
    status_write(e, NVDEC_SET_STATUS_OFFSET, status, 8);
}

/* ---- VIC: NV12 -> RGBA/BGRA ---- */
static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void vic_execute(MediaEngine *e) {
    size_t cfg_avail, l_avail, c_avail, o_avail;
    uint8_t *cfg = surf(e, e->reg[VIC_SET_CONFIG_STRUCT_OFFSET], &cfg_avail);
    uint8_t *luma = surf(e, e->reg[VIC_SET_INPUT0_LUMA_OFFSET], &l_avail), *chroma = surf(e, e->reg[VIC_SET_INPUT0_CHROMA_OFFSET], &c_avail);
    uint8_t *out = surf(e, e->reg[VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET], &o_avail);
    if (!cfg || cfg_avail < VIC_CFG_WORDS * 4 || !luma || !chroma || !out) {
        fail(e, 2);
        return;
    }
    uint32_t sw = rd32(cfg), sh = rd32(cfg + 4), dw = rd32(cfg + 8), dh = rd32(cfg + 12), fmt = rd32(cfg + 16), matrix = rd32(cfg + 20), full = rd32(cfg + 24),
             alpha = rd32(cfg + 28), pitch = rd32(cfg + 32);
    if (!dw) dw = sw;
    if (!dh) dh = sh;
    if (!pitch) pitch = dw * 4;
    if (!sw || !sh || sw > 8192 || sh > 8192 || dw > 8192 || dh > 8192 || fmt > VIC_FMT_ARGB || pitch < dw * 4 || l_avail < (size_t)sw * sh ||
        c_avail < (size_t)sw * ((sh + 1) / 2) || o_avail < (size_t)pitch * dh) {
        fail(e, 4);
        return;
    }
    /* 16.16 fixed point coefficients (as movie.c) */
    const int rv = matrix ? 103206 : 91881, gu = matrix ? 12276 : 22554, gv = matrix ? 30679 : 46802, bu = matrix ? 121609 : 116130;
    const int ys = full ? 65536 : 76309, yo = full ? 0 : 16, cscale = full ? 65536 : 74898;
    for (uint32_t y = 0; y < dh; y++) {
        uint32_t sy = (uint32_t)((uint64_t)y * sh / dh);
        const uint8_t *yr = luma + (size_t)sy * sw, *uv = chroma + (size_t)(sy >> 1) * sw;
        uint8_t *o = out + (size_t)y * pitch;
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t sx = (uint32_t)((uint64_t)x * sw / dw);
            int Y = ((yr[sx] - yo) * ys) >> 16, U = ((uv[sx & ~1u] - 128) * cscale) >> 16, V = ((uv[(sx & ~1u) + 1] - 128) * cscale) >> 16;
            uint8_t r = clamp8(Y + ((rv * V) >> 16)), g = clamp8(Y - ((gu * U + gv * V) >> 16)), b = clamp8(Y + ((bu * U) >> 16)),
                    a = fmt == VIC_FMT_XBGR ? 255 : (uint8_t)alpha;
            if (fmt == VIC_FMT_ARGB) {
                o[4 * x] = b;
                o[4 * x + 1] = g;
                o[4 * x + 2] = r;
            } else {
                o[4 * x] = r;
                o[4 * x + 1] = g;
                o[4 * x + 2] = b;
            }
            o[4 * x + 3] = a;
        }
    }
    e->st.frames++;
}

/* ---- NVJPG ---- */
static void nvjpg_execute(MediaEngine *e) {
    size_t setup_avail, in_avail, out_avail, chroma_avail;
    uint8_t *setup = surf(e, e->reg[NVJPG_SET_DRV_PIC_SETUP_OFFSET], &setup_avail), *in = surf(e, e->reg[NVJPG_SET_IN_BUF_BASE_OFFSET], &in_avail);
    uint8_t *out = surf(e, e->reg[NVJPG_SET_OUT_LUMA_OFFSET], &out_avail), *chroma = surf(e, e->reg[NVJPG_SET_OUT_CHROMA_OFFSET], &chroma_avail);
    uint32_t status[4] = {0};
    if (!setup || setup_avail < NVJPG_SETUP_WORDS * 4 || !in || !out) {
        fail(e, 2);
        status[0] = 100;
        status_write(e, NVJPG_SET_STATUS_OFFSET, status, 4);
        return;
    }
    uint32_t in_size = rd32(setup), out_cap = rd32(setup + 4), ow = rd32(setup + 8), oh = rd32(setup + 12), fmt = rd32(setup + 16), pitch = rd32(setup + 20);
    if (!in_size || in_size > in_avail) in_size = (uint32_t)in_avail;
    int w = 0, h = 0, err = 0;
    uint8_t *rgba = napjpeg_decode_rgba(in, in_size, &w, &h, &err);
    if (!rgba) {
        fail(e, (uint32_t)err);
        status[0] = (uint32_t)err ? (uint32_t)err : 1;
        status_write(e, NVJPG_SET_STATUS_OFFSET, status, 4);
        return;
    }
    if (ow && oh && ((int)ow != w || (int)oh != h)) {
        uint8_t *s = napjpeg_scale_rgba(rgba, w, h, (int)ow, (int)oh);
        free(rgba);
        rgba = s;
        w = (int)ow;
        h = (int)oh;
        if (!rgba) {
            fail(e, 3);
            return;
        }
    }
    size_t cap = out_cap && out_cap < out_avail ? out_cap : out_avail, written = 0;
    if (fmt == NVJPG_FMT_RGBA) {
        uint32_t p = pitch ? pitch : (uint32_t)w * 4;
        if (p < (uint32_t)w * 4 || cap < (size_t)p * (size_t)h) {
            err = 4;
        } else {
            for (int y = 0; y < h; y++) memcpy(out + (size_t)y * p, rgba + (size_t)y * w * 4, (size_t)w * 4);
            written = (size_t)p * h;
        }
    } else if (fmt == NVJPG_FMT_NV12) {
        uint32_t p = pitch ? pitch : (uint32_t)w;
        size_t csz = (size_t)p * ((h + 1) / 2);
        if (!chroma || p < (uint32_t)w || cap < (size_t)p * h || chroma_avail < csz) {
            err = 4;
        } else {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const uint8_t *s = rgba + ((size_t)y * w + x) * 4;
                    out[(size_t)y * p + x] = clamp8((int)(0.299f * s[0] + 0.587f * s[1] + 0.114f * s[2] + 0.5f));
                }
            for (int y = 0; y < (h + 1) / 2; y++)
                for (int x = 0; x < (w + 1) / 2; x++) { /* chroma from the top-left sample of each 2x2 block averaged */
                    int r = 0, g = 0, b = 0, n = 0;
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++) {
                            int yy = 2 * y + dy, xx = 2 * x + dx;
                            if (yy >= h || xx >= w) continue;
                            const uint8_t *s = rgba + ((size_t)yy * w + xx) * 4;
                            r += s[0], g += s[1], b += s[2], n++;
                        }
                    r /= n, g /= n, b /= n;
                    chroma[(size_t)y * p + 2 * x] = clamp8((int)(-0.168736f * r - 0.331264f * g + 0.5f * b + 128.5f));
                    chroma[(size_t)y * p + 2 * x + 1] = clamp8((int)(0.5f * r - 0.418688f * g - 0.081312f * b + 128.5f));
                }
            written = (size_t)p * h + csz;
        }
    } else {
        err = 5;
    }
    free(rgba);
    if (err) {
        fail(e, (uint32_t)err);
        status[0] = (uint32_t)err;
    } else {
        e->st.frames++;
        status[1] = (uint32_t)w;
        status[2] = (uint32_t)h;
        status[3] = (uint32_t)written;
    }
    status_write(e, NVJPG_SET_STATUS_OFFSET, status, 4);
}

static void execute(MediaEngine *e) {
    e->st.executes++;
    switch (e->class_id) {
        case MEDIA_CLASS_NVDEC: nvdec_execute(e); break;
        case MEDIA_CLASS_VIC: vic_execute(e); break;
        case MEDIA_CLASS_NVJPG: nvjpg_execute(e); break;
        case MEDIA_CLASS_MSENC: {
            uint32_t z[4] = {0};
            status_write(e, MSENC_SET_STATUS_OFFSET, z, 4); /* no bitstream produced */
            break;
        }
    }
}
static int known_reg(const MediaEngine *e, uint32_t off) {
    if (off == NVM_SET_APPLICATION_ID || off == NVM_EXECUTE) return 1;
    if (off < 0x100 || off > 0x1ff) return 0;
    switch (e->class_id) {
        case MEDIA_CLASS_NVDEC:
            return off <= NVDEC_SET_DISPLAY_BUF_CHROMA_OFFSET ||
                   (off >= NVDEC_SET_PICTURE_LUMA_OFFSET0 && off < NVDEC_SET_PICTURE_LUMA_OFFSET0 + NVDEC_PIC_SLOTS) ||
                   (off >= NVDEC_SET_PICTURE_CHROMA_OFFSET0 && off < NVDEC_SET_PICTURE_CHROMA_OFFSET0 + NVDEC_PIC_SLOTS);
        case MEDIA_CLASS_VIC: return off <= VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET || off == VIC_SET_INPUT0_LUMA_OFFSET || off == VIC_SET_INPUT0_CHROMA_OFFSET;
        case MEDIA_CLASS_NVJPG: return off <= NVJPG_SET_OUT_CHROMA_OFFSET || off == NVJPG_SET_STATUS_OFFSET;
        default: return off == MSENC_SET_STATUS_OFFSET;
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
