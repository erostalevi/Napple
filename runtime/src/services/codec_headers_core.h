/* codec headers core: portable parsers for the few header fields the media engines need to size an output surface (nvdrv_media): H.264 sequence
 * parameter sets, VP9 uncompressed headers, H.265 sequence parameter sets (skeleton: NAL header, profile/tier/level and the picture size). Pure
 * bit reading, no allocation, no macOS headers. All return 0 on success, -1 on malformed/truncated data. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int profile_idc, level_idc, chroma_format_idc, bit_depth;
    int width, height; /* after cropping */
    int frame_mbs_only, poc_type, num_ref_frames;
} H264Sps;
/* Finds the first SPS (nal_unit_type 7) in an Annex B stream (start codes) and parses it. */
int h264_find_sps(const uint8_t *annexb, size_t size, H264Sps *out);
/* Parses an SPS NAL unit payload (after the 1-byte NAL header, emulation prevention bytes still in). */
int h264_parse_sps(const uint8_t *rbsp, size_t size, H264Sps *out);
/* Counts the NAL units of each type in an Annex B stream (index = nal_unit_type, 0..31); returns the total. */
int h264_nal_histogram(const uint8_t *annexb, size_t size, unsigned hist[32]);

typedef struct {
    int profile, show_existing_frame, key_frame, show_frame, error_resilient, intra_only;
    int bit_depth, color_space, width, height; /* width/height 0 when the frame carries no size (inter frame: reused from a reference) */
    int refresh_frame_flags;
} Vp9Header;
int vp9_parse_header(const uint8_t *frame, size_t size, Vp9Header *out);

/* VP8 (RFC 6386 frame tag): key frames carry the size. */
typedef struct {
    int key_frame, version, show_frame, width, height; /* width/height 0 on inter frames */
} Vp8Header;
int vp8_parse_header(const uint8_t *frame, size_t size, Vp8Header *out);

typedef struct {
    int nal_type, temporal_id_plus1;
    int profile_idc, level_idc, chroma_format_idc;
    int width, height; /* after the conformance window */
} H265Sps;
/* Annex B stream: first SPS (nal_unit_type 33). Skeleton: sub-layer ordering info and the rest of the SPS are not read. */
int h265_find_sps(const uint8_t *annexb, size_t size, H265Sps *out);
