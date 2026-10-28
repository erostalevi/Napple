/* Regression tests for the crashes the codec_headers fuzz target found (FINDINGS.md). Build with the sanitizers: make -C runtime fuzz-regress. */
#include "t_codec_headers.c"
#include "../check.h"

int main(void) {
    /* 1. h264_parse_pps: num_slice_groups_minus1 (an unbounded Exp-Golomb value) was cast to int and had 1 added: signed overflow when it is 0x7fffffff
     * (UBSan: codec_headers_core.c:380). The same for the ref index counts, the QP values (26 + se) and the SPS fields. Found by fuzz codec_headers,
     * seed 11, iteration 9867 (20 byte input, minimized to 10). */
    static const uint8_t pps_ue_overflow[] = {0x00, 0x05, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x01};
    fuzz_codec_headers(pps_ue_overflow, sizeof pps_ue_overflow);
    H264Pps p;
    static const uint8_t pps_big_se[] = {0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0xff, 0xff, 0xff};
    h264_parse_pps(pps_big_se, sizeof pps_big_se, &p);
    static const uint8_t sps_big[] = {100, 0, 10, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0xff, 0xff};
    H264Sps s;
    h264_parse_sps(sps_big, sizeof sps_big, &s);

    /* 2. h264_build_pps: the QP fields come from the guest's NVDEC picture info; `pic_init_qp - 26` and bw_se's `2 * v - 1` overflowed int for large values
     * (UBSan: codec_headers_core.c:435). Found by fuzz codec_headers, seed 2, iteration 10070 (109 byte input, minimized to 68). */
    static const uint8_t huge_qp[] = {0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0, 0, 0, 0, 0, '{', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    fuzz_codec_headers(huge_qp, sizeof huge_qp);
    memset(&p, 0, sizeof p);
    p.num_ref_idx_l0 = p.num_ref_idx_l1 = 1;
    uint8_t out[64];
    p.pic_init_qp = 0x7fffffff;
    CHECK(h264_build_pps(&p, out, sizeof out) == 0, "a QP of 2^31-1 is not writable");
    p.pic_init_qp = (int)0x80000000;
    CHECK(h264_build_pps(&p, out, sizeof out) == 0, "a QP of INT_MIN is not writable");
    p.pic_init_qp = 26;
    p.chroma_qp_index_offset = 0x40000000;
    CHECK(h264_build_pps(&p, out, sizeof out) == 0, "a huge chroma offset is not writable");
    p.chroma_qp_index_offset = -3;
    p.transform_8x8 = 1;
    p.second_chroma_qp_index_offset = 5;
    size_t n = h264_build_pps(&p, out, sizeof out);
    H264Pps back;
    CHECK(n > 1 && h264_parse_pps(out + 1, n - 1, &back) == 0 && back.pic_init_qp == 26 && back.chroma_qp_index_offset == -3 &&
              back.second_chroma_qp_index_offset == 5,
          "a PPS with sane values still round-trips");
    printf("{\"test\":\"regress_codec_headers\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
