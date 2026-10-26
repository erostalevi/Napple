/* Unit tests: codec header parsers (H.264 SPS, VP9 uncompressed header, H.265 SPS) against tiny ffmpeg-encoded streams and hand-built bit strings. */
#include "../src/services/codec_headers_core.h"
#include "check.h"
#include "codec_vectors.h"

int main(void) {
    H264Sps s;
    CHECK(h264_find_sps(h264_64x48, sizeof h264_64x48, &s) == 0 && s.width == 64 && s.height == 48 && s.profile_idc == 66 && s.frame_mbs_only, "h264 64x48: %dx%d profile %d", s.width, s.height, s.profile_idc);
    CHECK(h264_find_sps(h264_208x112_high, sizeof h264_208x112_high, &s) == 0 && s.width == 208 && s.height == 112 && s.profile_idc == 100 && s.chroma_format_idc == 1 && s.bit_depth == 8, "h264 high profile 208x112: %dx%d profile %d", s.width, s.height, s.profile_idc);
    CHECK(h264_find_sps(h264_50x30_crop, sizeof h264_50x30_crop, &s) == 0 && s.width == 50 && s.height == 30, "h264 cropped 50x30 (coded 64x32): %dx%d", s.width, s.height);
    unsigned hist[32];
    int n = h264_nal_histogram(h264_64x48, sizeof h264_64x48, hist);
    CHECK(n >= 3 && hist[7] == 1 && hist[8] == 1 && hist[5] == 1, "nal histogram: %d NALs, SPS %u PPS %u IDR %u", n, hist[7], hist[8], hist[5]);
    CHECK(h264_find_sps(h264_64x48, 9, &s) == -1, "truncated stream: SPS cut short is refused");
    CHECK(h264_find_sps((const uint8_t *)"", 0, &s) == -1, "empty stream");
    { /* 4-byte start code and emulation prevention bytes keep working: wrap the SPS with 00 00 00 01 */
        uint8_t buf[64] = {0, 0, 0, 1}; size_t i = 0; while (!(h264_64x48[i] == 0 && h264_64x48[i + 1] == 0 && h264_64x48[i + 2] == 1)) i++;
        memcpy(buf + 4, h264_64x48 + i + 3, 20); CHECK(h264_find_sps(buf, 24, &s) == 0 && s.width == 64, "4-byte start code");
    }
    { /* a hand-built SPS: baseline, 1280x720 (80x45 MBs) -> 720 = 45*16, no cropping */
        const uint8_t sps[] = {66, 0xc0, 31, 0xf4, 0x05, 0x01, 0x40, 0x7b, 0x20}; (void)sps; /* only length-checked below */
        CHECK(h264_parse_sps(sps, 3, &s) == -1, "short SPS refused");
    }
    Vp9Header v;
    CHECK(vp9_parse_header(vp9_key_70x38, sizeof vp9_key_70x38, &v) == 0 && v.key_frame && v.width == 70 && v.height == 38 && v.profile == 0 && v.bit_depth == 8 && v.refresh_frame_flags == 0xff, "vp9 key frame 70x38: %dx%d", v.width, v.height);
    CHECK(vp9_parse_header(vp9_inter, sizeof vp9_inter, &v) == 0 && !v.key_frame && v.width == 0, "vp9 inter frame: no size");
    uint8_t bad[4] = {0x00, 0, 0, 0}; CHECK(vp9_parse_header(bad, 4, &v) == -1, "bad frame marker");
    uint8_t shortk[3] = {0x82, 0x49, 0x83}; CHECK(vp9_parse_header(shortk, 3, &v) == -1, "truncated key frame header");
    uint8_t show[2] = {0x88, 0x00}; CHECK(vp9_parse_header(show, 2, &v) == 0 && v.show_existing_frame, "show_existing_frame");
    H265Sps h;
    CHECK(h265_find_sps(h265_96x64, sizeof h265_96x64, &h) == 0 && h.width == 96 && h.height == 64 && h.chroma_format_idc == 1 && h.profile_idc == 1, "h265 96x64: %dx%d profile %d", h.width, h.height, h.profile_idc);
    CHECK(h265_find_sps(h264_64x48, sizeof h264_64x48, &h) == -1, "an H.264 stream has no H.265 SPS");
    printf("{\"test\":\"svc_codec_headers\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
