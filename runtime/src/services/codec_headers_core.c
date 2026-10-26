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
                br_bit(&b); /* color_range */
                if (o->profile == 1 || o->profile == 3) br_bits(&b, 3);
            } else if (o->profile == 1 || o->profile == 3) br_bit(&b);
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
