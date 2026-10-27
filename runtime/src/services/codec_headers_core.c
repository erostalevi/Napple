#include "codec_headers_core.h"
#include <string.h>

typedef struct {
    const uint8_t *d;
    size_t n, bit;
    int err;
} Br;
static unsigned br_bit(Br *b) {
    if (b->bit >= b->n * 8) {
        b->err = 1;
        return 0;
    }
    unsigned v = (b->d[b->bit >> 3] >> (7 - (b->bit & 7))) & 1;
    b->bit++;
    return v;
}
static uint32_t br_bits(Br *b, int n) {
    uint32_t v = 0;
    while (n-- > 0) v = (v << 1) | br_bit(b);
    return v;
}
static uint32_t br_ue(Br *b) {
    int z = 0;
    while (!br_bit(b) && !b->err && z < 32) z++;
    if (z >= 32) {
        b->err = 1;
        return 0;
    }
    return (uint32_t)(((uint64_t)1 << z) - 1 + br_bits(b, z));
}
static int32_t br_se(Br *b) {
    uint32_t k = br_ue(b);
    return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}
/* Removes emulation prevention bytes (00 00 03 -> 00 00) into out; returns the new length. */
static size_t unescape(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < n && o < cap; i++) {
        if (zeros >= 2 && in[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = in[i] == 0 ? zeros + 1 : 0;
        out[o++] = in[i];
    }
    return o;
}
/* Next NAL unit in an Annex B stream at or after *pos: sets *start (first byte after the start code) and *len; returns 0 at the end. */
static int next_nal(const uint8_t *d, size_t n, size_t *pos, size_t *start, size_t *len) {
    size_t i = *pos;
    while (i + 3 <= n && !(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)) i++;
    if (i + 3 > n) return 0;
    size_t s = i + 3, e = s;
    while (e + 3 <= n && !(d[e] == 0 && d[e + 1] == 0 && (d[e + 2] == 1 || d[e + 2] == 0))) e++;
    if (e + 3 > n) e = n;
    while (e > s && d[e - 1] == 0) e--; /* trailing_zero_8bits / the zero of a 4-byte start code */
    *start = s;
    *len = e - s;
    *pos = e;
    return 1;
}

static void h264_skip_scaling(Br *b, int count) {
    int last = 8, next = 8;
    for (int j = 0; j < count; j++) {
        if (next) { next = (last + br_se(b) + 256) % 256; }
        last = next ? next : last;
    }
}
int h264_parse_sps(const uint8_t *in, size_t n, H264Sps *o) {
    uint8_t rb[256];
    size_t len = unescape(in, n < sizeof rb ? n : sizeof rb, rb, sizeof rb);
    Br b = {rb, len, 0, 0};
    memset(o, 0, sizeof *o);
    o->profile_idc = (int)br_bits(&b, 8);
    br_bits(&b, 8); /* constraint flags + reserved */
    o->level_idc = (int)br_bits(&b, 8);
    br_ue(&b); /* seq_parameter_set_id */
    o->chroma_format_idc = 1;
    o->bit_depth = 8;
    int separate = 0;
    static const int high[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    for (unsigned i = 0; i < sizeof high / sizeof *high; i++)
        if (o->profile_idc == high[i]) {
            o->chroma_format_idc = (int)br_ue(&b);
            if (o->chroma_format_idc == 3) separate = (int)br_bit(&b);
            o->bit_depth = 8 + (int)br_ue(&b);
            br_ue(&b);
            br_bit(&b);       /* qpprime_y_zero_transform_bypass */
            if (br_bit(&b)) { /* seq_scaling_matrix_present */
                for (int i2 = 0; i2 < (o->chroma_format_idc != 3 ? 8 : 12); i2++)
                    if (br_bit(&b)) h264_skip_scaling(&b, i2 < 6 ? 16 : 64);
            }
            break;
        }
    br_ue(&b); /* log2_max_frame_num_minus4 */
    o->poc_type = (int)br_ue(&b);
    if (o->poc_type == 0) br_ue(&b);
    else if (o->poc_type == 1) {
        br_bit(&b);
        br_se(&b);
        br_se(&b);
        uint32_t k = br_ue(&b);
        for (uint32_t i = 0; i < k && i < 256 && !b.err; i++) br_se(&b);
    }
    o->num_ref_frames = (int)br_ue(&b);
    br_bit(&b); /* gaps_in_frame_num_allowed */
    uint32_t wmb = br_ue(&b) + 1, hmap = br_ue(&b) + 1;
    o->frame_mbs_only = (int)br_bit(&b);
    if (!o->frame_mbs_only) br_bit(&b); /* mb_adaptive_frame_field */
    br_bit(&b);                         /* direct_8x8_inference */
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (br_bit(&b)) {
        cl = br_ue(&b);
        cr = br_ue(&b);
        ct = br_ue(&b);
        cb = br_ue(&b);
    }
    if (b.err || wmb > 1024 || hmap > 1024) return -1;
    int cfi = separate ? 0 : o->chroma_format_idc;
    uint32_t ux = cfi == 1 || cfi == 2 ? 2 : 1, uy = (cfi == 1 ? 2 : 1) * (uint32_t)(2 - o->frame_mbs_only);
    uint32_t w = wmb * 16 - ux * (cl + cr), h = (2 - (uint32_t)o->frame_mbs_only) * hmap * 16 - uy * (ct + cb);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return -1;
    o->width = (int)w;
    o->height = (int)h;
    return 0;
}
int h264_find_sps(const uint8_t *d, size_t n, H264Sps *o) {
    size_t pos = 0, s, l;
    while (next_nal(d, n, &pos, &s, &l))
        if (l > 4 && (d[s] & 0x1f) == 7) return h264_parse_sps(d + s + 1, l - 1, o);
    return -1;
}
int h264_nal_histogram(const uint8_t *d, size_t n, unsigned hist[32]) {
    size_t pos = 0, s, l;
    int total = 0;
    memset(hist, 0, 32 * sizeof *hist);
    while (next_nal(d, n, &pos, &s, &l))
        if (l) {
            hist[d[s] & 0x1f]++;
            total++;
        }
    return total;
}

int vp9_parse_header(const uint8_t *f, size_t n, Vp9Header *o) {
    Br b = {f, n, 0, 0};
    memset(o, 0, sizeof *o);
    if (br_bits(&b, 2) != 2) return -1; /* frame_marker */
    int lo = (int)br_bit(&b), hi = (int)br_bit(&b);
    o->profile = (hi << 1) | lo;
    if (o->profile == 3 && br_bit(&b)) return -1; /* reserved_zero */
    o->show_existing_frame = (int)br_bit(&b);
    if (o->show_existing_frame) {
        br_bits(&b, 3);
        return b.err ? -1 : 0;
    }
    o->key_frame = br_bit(&b) == 0;
    o->show_frame = (int)br_bit(&b);
    o->error_resilient = (int)br_bit(&b);
    o->bit_depth = 8;
    o->color_space = 2;
    o->subsampling_x = o->subsampling_y = 1; /* profiles 0 and 2 */
    int sync_ok = 1;
    if (o->key_frame) {
        sync_ok = br_bits(&b, 24) == 0x498342;
    } else {
        o->intra_only = o->show_frame ? 0 : (int)br_bit(&b);
        if (!o->error_resilient) br_bits(&b, 2); /* reset_frame_context */
        if (o->intra_only) sync_ok = br_bits(&b, 24) == 0x498342;
    }
    if (!sync_ok) return -1;
    if (o->key_frame || o->intra_only) {
        if (o->key_frame || o->profile > 0) { /* color_config */
            if (o->profile >= 2) o->bit_depth = br_bit(&b) ? 12 : 10;
            o->color_space = (int)br_bits(&b, 3);
            if (o->color_space != 7) {
                o->color_range = (int)br_bit(&b);
                if (o->profile == 1 || o->profile == 3) {
                    o->subsampling_x = (int)br_bit(&b);
                    o->subsampling_y = (int)br_bit(&b);
                    br_bit(&b); /* reserved_zero */
                }
            } else {
                o->color_range = 1;
                if (o->profile == 1 || o->profile == 3) br_bit(&b); /* reserved_zero; RGB is 4:4:4 */
            }
        }
        o->refresh_frame_flags = o->key_frame ? 0xff : (int)br_bits(&b, 8);
        o->width = (int)br_bits(&b, 16) + 1;
        o->height = (int)br_bits(&b, 16) + 1;
    } else {
        o->refresh_frame_flags = (int)br_bits(&b, 8);
    }
    return b.err ? -1 : 0;
}

int vp8_parse_header(const uint8_t *f, size_t n, Vp8Header *o) {
    memset(o, 0, sizeof *o);
    if (n < 3) return -1;
    uint32_t tag = f[0] | (f[1] << 8) | ((uint32_t)f[2] << 16);
    o->key_frame = !(tag & 1);
    o->version = (int)((tag >> 1) & 7);
    o->show_frame = (int)((tag >> 4) & 1);
    if (!o->key_frame) return 0;
    if (n < 10 || f[3] != 0x9d || f[4] != 0x01 || f[5] != 0x2a) return -1;
    o->width = (f[6] | (f[7] << 8)) & 0x3fff;
    o->height = (f[8] | (f[9] << 8)) & 0x3fff;
    return o->width && o->height ? 0 : -1;
}

/* H.265: profile_tier_level(1, max_sub_layers_minus1) skipping, then the SPS fields up to the picture size and conformance window. */
static void h265_skip_ptl(Br *b, int max_sub_layers_minus1) {
    br_bits(b, 8); /* profile_space, tier, profile_idc */
    br_bits(b, 32);
    br_bits(b, 32); /* 48 bits of flags: compat (32) + 4 flags + 43 reserved + 1 */
    br_bits(b, 16); /* (the remaining 16 of the 48) */
    br_bits(b, 8);  /* general_level_idc */
    int pf[8] = {0}, lf[8] = {0};
    for (int i = 0; i < max_sub_layers_minus1; i++) {
        pf[i] = (int)br_bit(b);
        lf[i] = (int)br_bit(b);
    }
    if (max_sub_layers_minus1 > 0)
        for (int i = max_sub_layers_minus1; i < 8; i++) br_bits(b, 2);
    for (int i = 0; i < max_sub_layers_minus1; i++) {
        if (pf[i]) {
            br_bits(b, 32);
            br_bits(b, 32);
            br_bits(b, 24); /* 88 bits */
        }
        if (lf[i]) br_bits(b, 8);
    }
}
int h265_find_sps(const uint8_t *d, size_t n, H265Sps *o) {
    size_t pos = 0, s, l;
    memset(o, 0, sizeof *o);
    while (next_nal(d, n, &pos, &s, &l)) {
        if (l < 6 || ((d[s] >> 1) & 0x3f) != 33) continue;
        uint8_t rb[256];
        size_t len = unescape(d + s + 2, (l - 2) < sizeof rb ? l - 2 : sizeof rb, rb, sizeof rb);
        Br b = {rb, len, 0, 0};
        o->nal_type = 33;
        o->temporal_id_plus1 = d[s + 1] & 7;
        br_bits(&b, 4); /* sps_video_parameter_set_id */
        int msl = (int)br_bits(&b, 3);
        br_bit(&b); /* temporal_id_nesting */
        o->profile_idc = (int)(br_bits(&b, 8) & 0x1f);
        b.bit -= 8;
        h265_skip_ptl(&b, msl);
        o->level_idc = 0;
        br_ue(&b); /* sps_seq_parameter_set_id */
        o->chroma_format_idc = (int)br_ue(&b);
        if (o->chroma_format_idc == 3) br_bit(&b);
        uint32_t w = br_ue(&b), h = br_ue(&b);
        uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
        if (br_bit(&b)) {
            cl = br_ue(&b);
            cr = br_ue(&b);
            ct = br_ue(&b);
            cb = br_ue(&b);
        }
        if (b.err || !w || !h || w > 16384 || h > 16384) return -1;
        uint32_t sx = (o->chroma_format_idc == 1 || o->chroma_format_idc == 2) ? 2 : 1, sy = o->chroma_format_idc == 1 ? 2 : 1;
        if (sx * (cl + cr) >= w || sy * (ct + cb) >= h) return -1;
        o->width = (int)(w - sx * (cl + cr));
        o->height = (int)(h - sy * (ct + cb));
        return 0;
    }
    return -1;
}

/* ---- parameter sets ---- */
static int add_ps(H264ParamSets *o, int is_sps, const uint8_t *nal, size_t n) {
    if (is_sps) {
        if (o->nsps >= H264_PARAM_MAX) return 0;
        o->sps[o->nsps] = nal;
        o->sps_size[o->nsps++] = n;
    } else {
        if (o->npps >= H264_PARAM_MAX) return 0;
        o->pps[o->npps] = nal;
        o->pps_size[o->npps++] = n;
    }
    return 1;
}
int h264_find_param_sets(const uint8_t *d, size_t n, H264ParamSets *o) {
    size_t pos = 0, s, l;
    memset(o, 0, sizeof *o);
    while (next_nal(d, n, &pos, &s, &l))
        if (l > 1 && (d[s] & 0x1f) == 7) add_ps(o, 1, d + s, l);
        else if (l > 1 && (d[s] & 0x1f) == 8) add_ps(o, 0, d + s, l);
    if (o->nsps && o->npps) return 0;
    return o->nsps || o->npps ? 1 : -1;
}
int h264_parse_avcc(const uint8_t *d, size_t n, H264ParamSets *o) {
    memset(o, 0, sizeof *o);
    if (n < 7 || d[0] != 1) return -1;
    o->nal_length_size = (d[4] & 3) + 1;
    if (o->nal_length_size == 3) return -1;
    size_t p = 6;
    unsigned ns = d[5] & 0x1f;
    for (int pass = 0; pass < 2; pass++) {
        unsigned cnt = ns;
        if (pass) {
            if (p >= n) return -1;
            cnt = d[p++];
        }
        for (unsigned i = 0; i < cnt; i++) {
            if (p + 2 > n) return -1;
            size_t l = (size_t)(d[p] << 8 | d[p + 1]);
            p += 2;
            if (l < 2 || p + l > n || (d[p] & 0x1f) != (pass ? 8 : 7)) return -1;
            if (!add_ps(o, !pass, d + p, l)) return -1;
            p += l;
        }
    }
    return o->nsps && o->npps ? 0 : -1;
}
size_t h264_param_sets_annexb(const H264ParamSets *ps, uint8_t *out, size_t cap) {
    size_t at = 0;
    for (int pass = 0; pass < 2; pass++) {
        int cnt = pass ? ps->npps : ps->nsps;
        for (int i = 0; i < cnt; i++) {
            const uint8_t *nal = pass ? ps->pps[i] : ps->sps[i];
            size_t l = pass ? ps->pps_size[i] : ps->sps_size[i];
            if (at + 4 + l > cap) return 0;
            out[at++] = 0, out[at++] = 0, out[at++] = 0, out[at++] = 1;
            memcpy(out + at, nal, l);
            at += l;
        }
    }
    return at;
}
size_t h264_param_sets_avcc(const H264ParamSets *ps, uint8_t *out, size_t cap) {
    if (!ps->nsps || !ps->npps || ps->sps_size[0] < 4) return 0;
    size_t at = 6;
    if (cap < at) return 0;
    out[0] = 1;
    out[1] = ps->sps[0][1];
    out[2] = ps->sps[0][2];
    out[3] = ps->sps[0][3];
    out[4] = 0xff; /* 4-byte NAL lengths */
    out[5] = (uint8_t)(0xe0 | ps->nsps);
    for (int pass = 0; pass < 2; pass++) {
        if (pass) {
            if (at >= cap) return 0;
            out[at++] = (uint8_t)ps->npps;
        }
        int cnt = pass ? ps->npps : ps->nsps;
        for (int i = 0; i < cnt; i++) {
            const uint8_t *nal = pass ? ps->pps[i] : ps->sps[i];
            size_t l = pass ? ps->pps_size[i] : ps->sps_size[i];
            if (at + 2 + l > cap || l > 0xffff) return 0;
            out[at++] = (uint8_t)(l >> 8);
            out[at++] = (uint8_t)l;
            memcpy(out + at, nal, l);
            at += l;
        }
    }
    return at;
}

/* True when a bit after position `bit` of the RBSP is set before the trailing stop bit, i.e. there is more syntax to read. */
static int more_rbsp_data(const Br *b) {
    size_t last = b->n * 8;
    while (last > 0 && !((b->d[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1)) last--;
    return last > 0 && b->bit < last - 1;
}
int h264_parse_pps(const uint8_t *in, size_t n, H264Pps *o) {
    uint8_t rb[256];
    size_t len = unescape(in, n < sizeof rb ? n : sizeof rb, rb, sizeof rb);
    Br b = {rb, len, 0, 0};
    memset(o, 0, sizeof *o);
    o->pps_id = (int)br_ue(&b);
    o->sps_id = (int)br_ue(&b);
    o->cabac = (int)br_bit(&b);
    o->bottom_field_pic_order = (int)br_bit(&b);
    o->num_slice_groups = (int)br_ue(&b) + 1;
    if (o->num_slice_groups > 1) return -1; /* FMO: Baseline/Extended only, no hardware decoder takes it */
    o->num_ref_idx_l0 = (int)br_ue(&b) + 1;
    o->num_ref_idx_l1 = (int)br_ue(&b) + 1;
    o->weighted_pred = (int)br_bit(&b);
    o->weighted_bipred_idc = (int)br_bits(&b, 2);
    o->pic_init_qp = 26 + br_se(&b);
    o->pic_init_qs = 26 + br_se(&b);
    o->chroma_qp_index_offset = br_se(&b);
    o->deblocking_filter_control = (int)br_bit(&b);
    o->constrained_intra_pred = (int)br_bit(&b);
    o->redundant_pic_cnt = (int)br_bit(&b);
    o->second_chroma_qp_index_offset = o->chroma_qp_index_offset;
    if (b.err || o->pps_id > 255 || o->sps_id > 31 || o->num_ref_idx_l0 > 32 || o->num_ref_idx_l1 > 32) return -1;
    if (more_rbsp_data(&b)) {
        o->transform_8x8 = (int)br_bit(&b);
        if (br_bit(&b)) return b.err ? -1 : 0; /* pic_scaling_matrix_present: lists not read */
        o->second_chroma_qp_index_offset = br_se(&b);
        if (b.err) return -1;
    }
    return 0;
}

typedef struct {
    uint8_t *d;
    size_t cap, bit;
    int err;
} Bw;
static void bw_bit(Bw *w, unsigned v) {
    if (w->bit >= w->cap * 8) {
        w->err = 1;
        return;
    }
    if (v) w->d[w->bit >> 3] |= (uint8_t)(0x80 >> (w->bit & 7));
    w->bit++;
}
static void bw_bits(Bw *w, uint32_t v, int n) {
    while (n-- > 0) bw_bit(w, (v >> n) & 1);
}
static void bw_ue(Bw *w, uint32_t v) {
    uint64_t x = (uint64_t)v + 1;
    int z = 0;
    while ((x >> (z + 1)) != 0) z++;
    bw_bits(w, 0, z);
    for (int i = z; i >= 0; i--) bw_bit(w, (unsigned)((x >> i) & 1));
}
static void bw_se(Bw *w, int32_t v) { bw_ue(w, v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * (int64_t)v)); }
/* Appends the stop bit, pads to a byte, then writes header + escaped payload into out. Returns the NAL length or 0. */
static size_t finish_nal(Bw *w, uint8_t header, uint8_t *out, size_t cap) {
    bw_bit(w, 1);
    if (w->err) return 0;
    size_t n = (w->bit + 7) / 8, o = 0;
    int zeros = 0;
    if (cap < 1) return 0;
    out[o++] = header;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && w->d[i] <= 3) {
            if (o >= cap) return 0;
            out[o++] = 3;
            zeros = 0;
        }
        if (o >= cap) return 0;
        out[o++] = w->d[i];
        zeros = w->d[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}
static int h264_is_high(int profile) {
    static const int high[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    for (unsigned i = 0; i < sizeof high / sizeof *high; i++)
        if (profile == high[i]) return 1;
    return 0;
}
void h264_sps_fields_default(H264SpsFields *f, int width, int height, int num_ref_frames) {
    memset(f, 0, sizeof *f);
    f->profile_idc = 100;
    f->chroma_format_idc = 1;
    f->bit_depth = 8;
    f->log2_max_frame_num = 4;
    f->poc_type = 2;
    f->log2_max_poc_lsb = 4;
    f->num_ref_frames = num_ref_frames;
    f->width_mbs = (width + 15) / 16;
    f->height_map_units = (height + 15) / 16;
    f->frame_mbs_only = 1;
    f->direct_8x8_inference = 1;
    f->crop_right = (f->width_mbs * 16 - width) / 2;
    f->crop_bottom = (f->height_map_units * 16 - height) / 2;
    long mbs = (long)f->width_mbs * f->height_map_units; /* MaxFS of Table A-1 */
    f->level_idc = mbs <= 99      ? 10
                   : mbs <= 396   ? 20
                   : mbs <= 792   ? 21
                   : mbs <= 1620  ? 30
                   : mbs <= 3600  ? 31
                   : mbs <= 5120  ? 32
                   : mbs <= 8192  ? 40
                   : mbs <= 8704  ? 42
                   : mbs <= 22080 ? 50
                                  : 51;
}
size_t h264_build_sps(const H264SpsFields *f, uint8_t *out, size_t cap) {
    if (f->poc_type != 0 && f->poc_type != 2) return 0;
    if (f->sps_id < 0 || f->sps_id > 31 || f->log2_max_frame_num < 4 || f->log2_max_frame_num > 16 || f->width_mbs < 1 || f->width_mbs > 1024 ||
        f->height_map_units < 1 || f->height_map_units > 1024 || f->num_ref_frames < 0 || f->num_ref_frames > 16)
        return 0;
    if (f->poc_type == 0 && (f->log2_max_poc_lsb < 4 || f->log2_max_poc_lsb > 16)) return 0;
    uint8_t buf[64] = {0};
    Bw w = {buf, sizeof buf, 0, 0};
    bw_bits(&w, (uint32_t)f->profile_idc, 8);
    bw_bits(&w, (uint32_t)f->constraint_flags, 8);
    bw_bits(&w, (uint32_t)f->level_idc, 8);
    bw_ue(&w, (uint32_t)f->sps_id);
    if (h264_is_high(f->profile_idc)) {
        if (f->chroma_format_idc < 0 || f->chroma_format_idc > 3 || f->bit_depth < 8 || f->bit_depth > 14) return 0;
        bw_ue(&w, (uint32_t)f->chroma_format_idc);
        if (f->chroma_format_idc == 3) bw_bit(&w, 0); /* separate_colour_plane_flag */
        bw_ue(&w, (uint32_t)(f->bit_depth - 8));
        bw_ue(&w, (uint32_t)(f->bit_depth - 8));
        bw_bit(&w, 0); /* qpprime_y_zero_transform_bypass */
        bw_bit(&w, 0); /* seq_scaling_matrix_present */
    }
    bw_ue(&w, (uint32_t)(f->log2_max_frame_num - 4));
    bw_ue(&w, (uint32_t)f->poc_type);
    if (f->poc_type == 0) bw_ue(&w, (uint32_t)(f->log2_max_poc_lsb - 4));
    bw_ue(&w, (uint32_t)f->num_ref_frames);
    bw_bit(&w, (unsigned)(f->gaps_in_frame_num != 0));
    bw_ue(&w, (uint32_t)(f->width_mbs - 1));
    bw_ue(&w, (uint32_t)(f->height_map_units - 1));
    bw_bit(&w, (unsigned)(f->frame_mbs_only != 0));
    if (!f->frame_mbs_only) bw_bit(&w, (unsigned)(f->mb_adaptive_frame_field != 0));
    bw_bit(&w, (unsigned)(f->direct_8x8_inference != 0));
    int crop = f->crop_left || f->crop_right || f->crop_top || f->crop_bottom;
    bw_bit(&w, (unsigned)crop);
    if (crop) {
        bw_ue(&w, (uint32_t)f->crop_left);
        bw_ue(&w, (uint32_t)f->crop_right);
        bw_ue(&w, (uint32_t)f->crop_top);
        bw_ue(&w, (uint32_t)f->crop_bottom);
    }
    bw_bit(&w, 0); /* vui_parameters_present */
    return finish_nal(&w, 0x67, out, cap);
}
size_t h264_build_pps(const H264Pps *p, uint8_t *out, size_t cap) {
    if (p->pps_id < 0 || p->pps_id > 255 || p->sps_id < 0 || p->sps_id > 31 || p->num_ref_idx_l0 < 1 || p->num_ref_idx_l0 > 32 || p->num_ref_idx_l1 < 1 ||
        p->num_ref_idx_l1 > 32 || (p->num_slice_groups > 1))
        return 0;
    uint8_t buf[32] = {0};
    Bw w = {buf, sizeof buf, 0, 0};
    bw_ue(&w, (uint32_t)p->pps_id);
    bw_ue(&w, (uint32_t)p->sps_id);
    bw_bit(&w, (unsigned)(p->cabac != 0));
    bw_bit(&w, (unsigned)(p->bottom_field_pic_order != 0));
    bw_ue(&w, 0); /* num_slice_groups_minus1 */
    bw_ue(&w, (uint32_t)(p->num_ref_idx_l0 - 1));
    bw_ue(&w, (uint32_t)(p->num_ref_idx_l1 - 1));
    bw_bit(&w, (unsigned)(p->weighted_pred != 0));
    bw_bits(&w, (uint32_t)p->weighted_bipred_idc & 3, 2);
    bw_se(&w, p->pic_init_qp - 26);
    bw_se(&w, p->pic_init_qs - 26);
    bw_se(&w, p->chroma_qp_index_offset);
    bw_bit(&w, (unsigned)(p->deblocking_filter_control != 0));
    bw_bit(&w, (unsigned)(p->constrained_intra_pred != 0));
    bw_bit(&w, (unsigned)(p->redundant_pic_cnt != 0));
    if (p->transform_8x8) {
        bw_bit(&w, 1);
        bw_bit(&w, 0); /* pic_scaling_matrix_present */
        bw_se(&w, p->second_chroma_qp_index_offset);
    }
    return finish_nal(&w, 0x68, out, cap);
}

int vp9_level_for(int width, int height) {
    static const struct {
        long ps;
        int level;
    } t[] = {{36864, 10}, {73728, 11}, {122880, 20}, {245760, 21}, {552960, 30}, {983040, 31}, {2228224, 40}, {8912896, 50}, {35651584, 60}};
    long ps = (long)width * height;
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++)
        if (ps <= t[i].ps) return t[i].level;
    return 62;
}
size_t vp9_build_vpcc(const Vp9Header *h, uint8_t out[VP9_VPCC_SIZE]) {
    if (!h || (!h->key_frame && !h->intra_only) || h->width <= 0 || h->height <= 0) return 0;
    int chroma = h->subsampling_x && h->subsampling_y ? 1
                 : h->subsampling_x                   ? 2
                                                      : 3; /* 4:2:0 colocated with luma, 4:2:2, 4:4:4 (4:4:0 is not representable: 4:4:4) */
    /* ISO/IEC 23091-2 code points of the VP9 colour spaces: primaries, transfer, matrix */
    static const uint8_t cs[8][3] = {{2, 2, 2}, {6, 6, 6}, {1, 1, 1}, {6, 6, 6}, {7, 7, 7}, {9, 14, 9}, {2, 2, 2}, {1, 13, 0}};
    int c = h->color_space >= 0 && h->color_space < 8 ? h->color_space : 0;
    memset(out, 0, VP9_VPCC_SIZE);
    out[0] = 1; /* version 1, flags 0 */
    out[4] = (uint8_t)h->profile;
    out[5] = (uint8_t)vp9_level_for(h->width, h->height);
    out[6] = (uint8_t)((h->bit_depth << 4) | (chroma << 1) | (h->color_range ? 1 : 0));
    out[7] = cs[c][0];
    out[8] = (uint8_t)(c == 5 && h->bit_depth == 12 ? 15 : cs[c][1]);
    out[9] = cs[c][2];
    return VP9_VPCC_SIZE; /* codecInitializationDataSize (u16) = 0 in bytes 10..11 */
}
