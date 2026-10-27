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
    int color_range, subsampling_x, subsampling_y; /* key frames and intra-only frames of profile > 0; profile 0 and 2 are always 4:2:0 */
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

/* ---- Parameter sets for a hardware decoder backend (VideoToolbox) ----------------------------------------------------------------------
 * VideoToolbox wants an H.264 format description built from the SPS and PPS NAL units (CMVideoFormatDescriptionCreateFromH264ParameterSets: pointers and
 * sizes of the NAL units, header byte included, emulation prevention bytes still in) and a vpcC record for VP9. These helpers find or build them from what
 * a guest gives the engine: an Annex B stream, an avcC record, or the fields of the NVDEC picture-info structure (the writers). Pointers in H264ParamSets
 * point into the buffer that was parsed and live as long as it does. vt_h264 takes the Annex B form (h264_param_sets_annexb -> vth264_config). */
enum { H264_PARAM_MAX = 8 };
typedef struct {
    const uint8_t *sps[H264_PARAM_MAX], *pps[H264_PARAM_MAX];
    size_t sps_size[H264_PARAM_MAX], pps_size[H264_PARAM_MAX];
    int nsps, npps;
    int nal_length_size; /* avcC only: 1, 2 or 4 bytes of length before each NAL of the access units; 0 for Annex B */
} H264ParamSets;
/* Collects every SPS (type 7) and PPS (type 8) NAL unit of an Annex B stream. Returns 0 when at least one of each was found, 1 when only some were, -1 when
 * there were none or the stream is malformed. */
int h264_find_param_sets(const uint8_t *annexb, size_t size, H264ParamSets *out);
/* Parses an AVCDecoderConfigurationRecord (the avcC box payload, or a MediaCodec csd blob in that form). 0 or -1. */
int h264_parse_avcc(const uint8_t *avcc, size_t size, H264ParamSets *out);
/* Writes the parameter sets as Annex B (4-byte start codes, SPS first); returns the length, 0 when `cap` is too small or there are none. */
size_t h264_param_sets_annexb(const H264ParamSets *ps, uint8_t *out, size_t cap);
/* Writes the parameter sets as an avcC record (4-byte NAL lengths); returns the length or 0. */
size_t h264_param_sets_avcc(const H264ParamSets *ps, uint8_t *out, size_t cap);

typedef struct {
    int pps_id, sps_id, cabac, bottom_field_pic_order, num_slice_groups;
    int num_ref_idx_l0, num_ref_idx_l1, weighted_pred, weighted_bipred_idc;
    int pic_init_qp, pic_init_qs, chroma_qp_index_offset, deblocking_filter_control, constrained_intra_pred, redundant_pic_cnt;
    int transform_8x8,
        second_chroma_qp_index_offset; /* the High-profile extension; transform_8x8 is 0 and the offset equals chroma_qp_index_offset without it */
} H264Pps;
/* Parses a PPS NAL unit payload (after the 1-byte NAL header). Scaling lists of the extension are not read. */
int h264_parse_pps(const uint8_t *rbsp, size_t size, H264Pps *out);

/* What an SPS is written from. width_mbs/height_map_units are in macroblocks; the crop values are in crop units as the syntax has them. Only poc_type 0 and 2
 * and no VUI and no scaling matrices are written (what a decoder session needs; the picture size, reference count and order type are what matter). */
typedef struct {
    int profile_idc, constraint_flags, level_idc, sps_id;
    int chroma_format_idc, bit_depth; /* written for the High profiles only */
    int log2_max_frame_num, poc_type, log2_max_poc_lsb, num_ref_frames, gaps_in_frame_num;
    int width_mbs, height_map_units, frame_mbs_only, mb_adaptive_frame_field, direct_8x8_inference;
    int crop_left, crop_right, crop_top, crop_bottom;
} H264SpsFields;
/* Defaults for a progressive 4:2:0 8-bit stream of the given picture size (High profile, level from the size, cropping to the exact size). */
void h264_sps_fields_default(H264SpsFields *f, int width, int height, int num_ref_frames);
/* Writes the NAL unit (header byte 0x67 included, emulation prevention applied); returns its length, 0 when the fields are not writable or `cap` is too small. */
size_t h264_build_sps(const H264SpsFields *f, uint8_t *out, size_t cap);
size_t h264_build_pps(const H264Pps *p, uint8_t *out, size_t cap); /* header byte 0x68; the extension is written when transform_8x8 is set */

/* VP9 has no parameter sets in the stream; VideoToolbox gets a vpcC record (a full box payload: version 1, flags 0). 12 bytes. */
enum { VP9_VPCC_SIZE = 12 };
/* The VP9 level (10..62) that covers the picture size; a coarse choice from the luma picture size limit. */
int vp9_level_for(int width, int height);
/* Builds the record from a key frame's header. Returns VP9_VPCC_SIZE, or 0 when the header is not a key/intra-only frame with a size. */
size_t vp9_build_vpcc(const Vp9Header *h, uint8_t out[VP9_VPCC_SIZE]);
