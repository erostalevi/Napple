/* movie audio core: portable Vorbis (stb_vorbis, public domain, vendored in src/third_party) and Opus (libopus when the build has it) decoders for
 * the movie::Decoder audio types (movie.c). The movie library hands over raw codec packets the way Android's MediaCodec does, not Ogg pages:
 * Vorbis wants the three header packets (identification 0x01, comment 0x03, setup 0x05; as csd-0 in Xiph lacing or as the first input buffers),
 * then one audio packet per input. The helper wraps each packet in a minimal Ogg page so stb_vorbis' push-data decoder can run on it. Output is
 * interleaved signed 16-bit PCM. No allocation per packet beyond stb_vorbis' own. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct VorbisPcm VorbisPcm;
enum { AUDIO_OK = 0, AUDIO_NEED_MORE = 1, AUDIO_BAD = -1, AUDIO_NOMEM = -2, AUDIO_UNSUPPORTED = -3 };

VorbisPcm *vorbispcm_create(void);
void vorbispcm_destroy(VorbisPcm *v);
/* csd-0 style blob: u8 count-1 (2), Xiph-laced lengths of the first two packets, then the three header packets back to back. Returns AUDIO_OK once
 * the decoder is ready, AUDIO_BAD on a malformed blob. */
int vorbispcm_set_headers_xiph(VorbisPcm *v, const uint8_t *blob, size_t size);
/* Out-of-band headers as the movie library hands them over in the format metadata (csd-0, csd-1, csd-2; NULL/0 for a missing key). Each blob is a Xiph-laced bundle
 * of all three packets, or one header packet; the usual Android layouts all work: csd-0 = bundle, csd-0 + csd-1 = identification + setup (the comment header is
 * synthesised), csd-0..2 = the three packets. Returns AUDIO_OK once the decoder is ready, AUDIO_NEED_MORE when the headers are incomplete so far (the rest may
 * still arrive as the first input buffers), AUDIO_BAD on a malformed blob. */
int vorbispcm_set_headers_csd(VorbisPcm *v, const uint8_t *const blobs[3], const size_t sizes[3]);
/* One packet: a header packet (first byte odd, "vorbis" at 1..6) is collected until all three are in, an audio packet is decoded. Writes up to
 * max_frames interleaved sample frames to out and sets *frames (0 for headers and for the first audio packet, which only primes the overlap).
 * Returns AUDIO_OK, AUDIO_BAD (corrupt packet), AUDIO_NEED_MORE (audio before the headers were complete), AUDIO_NOMEM. */
int vorbispcm_packet(VorbisPcm *v, const uint8_t *pkt, size_t size, int16_t *out, int max_frames, int *frames);
int vorbispcm_ready(const VorbisPcm *v);
int vorbispcm_channels(const VorbisPcm *v);
int vorbispcm_rate(const VorbisPcm *v);
/* Seek / flush: forget the overlap state, keep the headers. */
void vorbispcm_flush(VorbisPcm *v);
/* Upper bound of sample frames one audio packet can yield (both block sizes are at most 8192 samples). */
#define VORBISPCM_MAX_FRAMES 8192

/* Opus (RFC 6716 packets, 48 kHz output). Built only with -DNAPPLE_HAVE_OPUS (the runtime Makefile sets it and links the Homebrew libopus); elsewhere create returns
 * NULL and opuspcm_available() is 0. `pre_skip` samples (from OpusHead) are dropped from the start of the stream. */
typedef struct OpusPcm OpusPcm;
OpusPcm *opuspcm_create(int channels, int pre_skip);
void opuspcm_destroy(OpusPcm *o);
/* Parses an OpusHead blob (csd-0) into channels and pre-skip; returns 0 or -1. */
int opuspcm_parse_head(const uint8_t *head, size_t size, int *channels, int *pre_skip);
int opuspcm_packet(OpusPcm *o, const uint8_t *pkt, size_t size, int16_t *out, int max_frames, int *frames);
void opuspcm_flush(OpusPcm *o);
int opuspcm_available(void);
#define OPUSPCM_MAX_FRAMES 5760 /* 120 ms at 48 kHz */

/* AAC (movie::Decoder type 4, mime audio/mp4a-latm). The library hands over raw access units as MediaCodec does, with the AudioSpecificConfig as csd-0;
 * ADTS-framed units (sync word 0xFFF) are accepted too, their header is stripped and configures the decoder when no csd-0 came. The parsers are portable;
 * the decoder is AudioToolbox (macOS / iOS: create returns NULL elsewhere and aacpcm_available() is 0). Output is interleaved s16 in WAVE channel order
 * (L R C LFE Ls Rs ...), the order Android's SoftAAC2 (the movie library's own decoder) produces. */
typedef struct {
    int object_type; /* 2 = AAC-LC, 5 = SBR (HE-AAC), 29 = PS (HE-AAC v2), ... */
    int rate;        /* core sampling rate (Hz) */
    int channels;    /* channel configuration 1..7 (7 = 8 channels), 0 = in a program config element (not supported) */
    int sbr;         /* explicit SBR / PS signalling: output at twice the core rate */
} AacConfig;
/* AudioSpecificConfig (ISO 14496-3 1.6.2.1): object type (5 bits, escape 31), frequency index (4 bits, 15 = explicit 24-bit rate), channel config. 0 or -1. */
int aac_parse_asc(const uint8_t *asc, size_t size, AacConfig *out);
/* ADTS header: returns its length (7, or 9 with CRC) and the whole frame length in *frame_len, 0 when p does not start with one. */
size_t aac_parse_adts(const uint8_t *p, size_t size, AacConfig *out, size_t *frame_len);
/* Two-byte AudioSpecificConfig for a plain configuration (object type < 31, a standard rate, channels 1..7): writes it and returns 2, or 0. */
size_t aac_make_asc(const AacConfig *c, uint8_t out[2]);
/* channels 1..8 -> channel configuration (7 for 8 channels), 0 for others */
int aac_channel_config(int channels);

typedef struct AacPcm AacPcm;
/* From an AudioSpecificConfig (csd-0), or with asc NULL from rate / channels (AAC-LC); with neither the decoder waits for an ADTS header. */
AacPcm *aacpcm_create(const uint8_t *asc, size_t asc_size, int rate, int channels);
void aacpcm_destroy(AacPcm *a);
/* One access unit (raw, or ADTS-framed). *frames = sample frames written (0 while the decoder primes). AUDIO_OK, AUDIO_BAD, AUDIO_NEED_MORE (no configuration
 * yet: raw unit before any csd-0 / ADTS header), AUDIO_UNSUPPORTED. */
int aacpcm_packet(AacPcm *a, const uint8_t *pkt, size_t size, int16_t *out, int max_frames, int *frames);
int aacpcm_ready(const AacPcm *a);
int aacpcm_rate(const AacPcm *a);     /* output rate (twice the core rate with SBR) */
int aacpcm_channels(const AacPcm *a);
void aacpcm_flush(AacPcm *a);
int aacpcm_available(void);
#define AACPCM_MAX_FRAMES 2048 /* 1024 per access unit, 2048 with SBR */
