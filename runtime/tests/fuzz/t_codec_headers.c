/* Fuzz target: codec header parsers and writers (services/codec_headers_core.c): H.264 SPS/PPS/avcC/Annex B, VP9, VP8 and H.265 headers, and the
 * parameter-set builders. These read guest-supplied NVDEC picture info and bitstreams. */
#include "../../src/services/codec_headers_core.h"
#include "../codec_vectors.h"
#include "fuzz.h"
#include <string.h>

int fuzz_codec_headers(const uint8_t *d, size_t n) {
    H264Sps s;
    H265Sps h;
    Vp9Header v;
    Vp8Header v8;
    H264Pps pps;
    H264ParamSets ps;
    unsigned hist[32];
    uint8_t out[512], vpcc[VP9_VPCC_SIZE];
    h264_find_sps(d, n, &s);
    h264_parse_sps(d, n, &s);
    h264_nal_histogram(d, n, hist);
    h265_find_sps(d, n, &h);
    if (!vp9_parse_header(d, n, &v)) {
        vp9_build_vpcc(&v, vpcc);
        vp9_level_for(v.width, v.height);
    }
    vp8_parse_header(d, n, &v8);
    h264_parse_pps(d, n, &pps);
    if (h264_find_param_sets(d, n, &ps) >= 0 || !h264_parse_avcc(d, n, &ps)) { /* the writers take what the parsers found */
        h264_param_sets_annexb(&ps, out, sizeof out);
        h264_param_sets_avcc(&ps, out, sizeof out);
        h264_param_sets_annexb(&ps, out, 16);
        h264_param_sets_avcc(&ps, out, 16);
    }
    /* builders from fuzzed field values: 4 bytes per int field, so a short input leaves the rest at the defaults */
    H264SpsFields f;
    h264_sps_fields_default(&f, 1 + (n > 0 ? d[0] : 0) * 8, 1 + (n > 1 ? d[1] : 0) * 8, n > 2 ? d[2] % 17 : 4);
    FzIn r = {d, n, 3};
    int *fi = (int *)&f;
    for (size_t i = 0; i < sizeof f / sizeof(int) && fz_left(&r) >= 5; i++)
        if (fz_u8(&r) & 1) fi[i] = (int)fz_u32(&r);
    h264_build_sps(&f, out, sizeof out);
    h264_build_sps(&f, out, 8);
    if (n >= sizeof pps) {
        memcpy(&pps, d + n - sizeof pps, sizeof pps);
        h264_build_pps(&pps, out, sizeof out);
        h264_build_pps(&pps, out, 4);
    }
    return 0;
}

void fuzz_codec_headers_seeds(FuzzSeedFn add, void *ctx) {
    add(ctx, h264_64x48, sizeof h264_64x48);
    add(ctx, h264_208x112_high, sizeof h264_208x112_high);
    add(ctx, h264_50x30_crop, sizeof h264_50x30_crop);
    add(ctx, h265_96x64, sizeof h265_96x64);
    add(ctx, vp9_key_70x38, sizeof vp9_key_70x38);
    add(ctx, vp9_inter, sizeof vp9_inter);
    /* an avcC record built from the SPS/PPS of the first vector, a raw SPS payload and a PPS payload */
    H264ParamSets ps;
    uint8_t buf[256];
    if (h264_find_param_sets(h264_64x48, sizeof h264_64x48, &ps) >= 0) {
        size_t m = h264_param_sets_avcc(&ps, buf, sizeof buf);
        if (m) add(ctx, buf, m);
        add(ctx, ps.sps[0] + 1, ps.sps_size[0] - 1);
        add(ctx, ps.pps[0] + 1, ps.pps_size[0] - 1);
    }
    const uint8_t vp8[] = {0x50, 0x42, 0x00, 0x9d, 0x01, 0x2a, 0x40, 0x01, 0xf0, 0x00, 0x00, 0x00};
    add(ctx, vp8, sizeof vp8);
}
FUZZ_TARGET(codec_headers);
