/* napple_audio.h - host side of Horizon audio for Napple.

   Plain C API, independent of IPC marshaling: the runtime's IPC layer parses requests, hands raw guest
   bytes to this library and marshals the results back.  See docs/audio-input-frontend-interface.md.

   Contents
     - engine      : shared Core Audio output (48 kHz stereo float), master volume, pause, statistics
     - audren      : the audio renderer (audren:u IAudioRenderer): RequestUpdate parser, voices, mixes, effects
     - audio device: IAudioDevice constants (names, volume, channel count)
     - audout      : audout:u simple PCM16 output
     - hwopus      : hardware Opus decoder (hwopus) backend, libopus based

   Threading: every function may be called from any thread unless noted.  Guest memory is reached only
   through napple_audio_host.translate; a NULL translate means guest addresses are host addresses
   (Napple runs guest code natively, so this is the normal case). */
#ifndef NAPPLE_AUDIO_H
#define NAPPLE_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAPPLE_AUDIO_API_VERSION 1

/* ---- result codes (negative = error). The runtime maps them to Horizon results. ---- */
enum {
    NAPPLE_AUDIO_OK = 0,
    NAPPLE_AUDIO_E_INVALID = -1,      /* bad argument or parameter out of range           */
    NAPPLE_AUDIO_E_PROTOCOL = -2,     /* malformed RequestUpdate input                    */
    NAPPLE_AUDIO_E_NOMEM = -3,
    NAPPLE_AUDIO_E_SIZE = -4,         /* output buffer too small (needed size is returned in *out_written) */
    NAPPLE_AUDIO_E_UNSUPPORTED = -5,  /* revision / format outside what is implemented    */
    NAPPLE_AUDIO_E_STATE = -6,        /* wrong state (not started, unknown decoder, ...)  */
    NAPPLE_AUDIO_E_DECODE = -7        /* codec failure (hwopus)                           */
};

/* ---- host callbacks ---- */
typedef struct napple_audio_host {
    void *user;
    /* Host pointer for [addr, addr+size) of guest memory, or NULL if unmapped. NULL member = identity. */
    void *(*translate)(void *user, uint64_t guest_addr, uint64_t size);
    /* Called before the library writes into guest memory (circular sinks, aux/capture rings, hwopus output):
       forward to napple_gfx_guest_write_prepare(addr, len). NULL = nothing to do. */
    void (*write_prepare)(void *user, void *host_ptr, size_t len);
    /* audren: called on the render thread after every rendered frame (signal the guest's system event). */
    void (*frame_event)(void *user);
    /* audout: called (any thread) whenever at least one buffer was released. */
    void (*buffer_event)(void *user);
    /* Optional log sink; NULL = stderr. Lines are already prefixed with "[napple]". */
    void (*log)(void *user, const char *line);
} napple_audio_host;

/* ---- engine ---- */
typedef struct napple_audio_config {
    int use_device;         /* 1 = open the default Core Audio output; 0 = null sink (tests, CI)      */
    int prefill_ms;         /* audio queued before output starts; 0 = default (20 ms); 5..500          */
    float master_volume;    /* 0..1; 0 = default 1.0                                                   */
} napple_audio_config;

typedef struct napple_audio_stats {
    uint64_t callback_frames;   /* frames asked for by the device                                   */
    uint64_t underruns;         /* device callbacks that found less audio than requested            */
    uint64_t missing_frames;
    uint64_t overruns;          /* producer pushes dropped because the ring was full                */
    uint64_t trims;             /* backlog trims after a device stall                               */
    double latency_ms;          /* ring fill + device buffer, as measured at the last callback      */
    int device_open;
} napple_audio_stats;

/* Initialise (reference counted; later calls with the first call's config are no-ops). */
int napple_audio_init(const napple_audio_config *config);
void napple_audio_shutdown(void);
void napple_audio_set_master_volume(float volume);   /* 0..1 */
float napple_audio_get_master_volume(void);
/* Focus handling: while paused the output is silent and queued audio is dropped when resumed. */
void napple_audio_set_paused(int paused);
/* Rebuild/restart the device (default device changed, media services reset). */
void napple_audio_output_restart(int recreate);
/* Game speed (turbo / slow motion). The renderers and audout pacers run `speed` times faster than real time (the guest sees the audio
 * events at that rate, as it sees every other clock), and the output engine either shifts the pitch by the same factor (resampling,
 * mode NAPPLE_AUDIO_SPEED_PITCH) or drops the audio (NAPPLE_AUDIO_SPEED_MUTE). NAPPLE_AUDIO_SPEED_AUTO keeps the pitch shift inside
 * 0.5x..2x and mutes outside (a chipmunk at 8x is not useful). speed is clamped to 0.1..10; 1.0 restores normal playback. */
enum { NAPPLE_AUDIO_SPEED_AUTO = 0, NAPPLE_AUDIO_SPEED_PITCH = 1, NAPPLE_AUDIO_SPEED_MUTE = 2 };
void napple_audio_set_speed(double speed, int mode);
double napple_audio_get_speed(void);
void napple_audio_get_stats(napple_audio_stats *out);

/* ---- audren ---- */
typedef struct napple_audren napple_audren;
#define NAPPLE_AUDREN_PARAM_SIZE 0x34   /* nn::audio::AudioRendererParameterInternal */
enum { NAPPLE_AUDREN_MANUAL = 1 };      /* no render thread: the caller drives napple_audren_render_frame */

/* Bytes of work buffer the guest must be told to allocate (GetAudioRendererWorkBufferSize). The host keeps
   its own state, so this is a small constant independent of the parameters; param is validated. */
int napple_audren_work_buffer_size(const void *param, size_t param_size, uint64_t *out_size);
/* OpenAudioRenderer: param = the 0x34-byte parameter block. */
int napple_audren_open(const void *param, size_t param_size, const napple_audio_host *host, uint32_t flags,
                       napple_audren **out);
void napple_audren_close(napple_audren *r);
int napple_audren_start(napple_audren *r);   /* StartAudioRenderer */
int napple_audren_stop(napple_audren *r);    /* StopAudioRenderer  */
uint32_t napple_audren_state(const napple_audren *r);  /* 0 = started, 1 = stopped (IAudioRenderer::GetState) */
int32_t napple_audren_sample_rate(const napple_audren *r);
int32_t napple_audren_sample_count(const napple_audren *r);
int32_t napple_audren_mix_buffer_count(const napple_audren *r);
uint32_t napple_audren_revision(const napple_audren *r);  /* 'REVn' word of the last request, or open param */
void napple_audren_set_time_limit(napple_audren *r, uint32_t percent);
uint32_t napple_audren_get_time_limit(const napple_audren *r);
/* RequestUpdate / RequestUpdateAuto. in = the input blob, out = output blob area, perf = performance area
   (may be NULL). *out_written receives the number of output bytes produced (or needed on E_SIZE).
   Revision tolerant: REV1..REV15 headers are accepted (REV5..15 are the tested range). */
int napple_audren_request_update(napple_audren *r, const void *in, size_t in_size, void *out, size_t out_size,
                                 size_t *out_written, void *perf, size_t perf_size);
/* Manual mode / tests. render_frame mixes one frame of sample_count samples. read_output copies the latest
   `frames` stereo frames (interleaved L,R) of the final mix; returns frames copied. */
void napple_audren_render_frame(napple_audren *r);
int napple_audren_read_output(napple_audren *r, float *lr, int frames);
uint64_t napple_audren_elapsed_frames(const napple_audren *r);

/* ---- IAudioDevice ---- */
#define NAPPLE_AUDIO_DEVICE_NAME "AudioTvOutput"
size_t napple_audio_device_name(char *out, size_t cap);   /* writes the NUL-terminated device name, returns length */
int32_t napple_audio_device_channel_count(void);          /* 2 */
float napple_audio_device_get_volume(void);
void napple_audio_device_set_volume(float volume);

/* ---- audout ---- */
typedef struct napple_audout napple_audout;
/* sample_rate/channels 0 = default (48000 / 2). Granted rate/channels are written back (PCM16 always). */
int napple_audout_open(uint32_t sample_rate, uint32_t channels, const napple_audio_host *host, napple_audout **out,
                       uint32_t *granted_rate, uint32_t *granted_channels);
void napple_audout_close(napple_audout *o);
int napple_audout_start(napple_audout *o);
int napple_audout_stop(napple_audout *o);
uint32_t napple_audout_state(const napple_audout *o);   /* 0 = started, 1 = stopped */
/* AppendAudioOutBuffer: PCM16 interleaved at guest address `addr + offset`, `size` bytes. `tag` is returned
   from get_released when played. Up to 32 buffers may be queued. */
int napple_audout_append(napple_audout *o, uint64_t tag, uint64_t addr, uint64_t size, uint64_t offset);
uint32_t napple_audout_get_released(napple_audout *o, uint64_t *tags, uint32_t max);
int napple_audout_contains(const napple_audout *o, uint64_t tag);
/* Manual mode for tests: the pacing thread idles and napple_audout_pump consumes `frames` frames from the queue
   immediately (releasing buffers as they finish). */
void napple_audout_set_manual(napple_audout *o, int manual);
void napple_audout_pump(napple_audout *o, uint32_t frames);

/* ---- hwopus (nn::codec::HardwareOpusDecoder) ---- */
typedef struct napple_hwopus napple_hwopus;
typedef struct napple_hwopus_ms_params {   /* OpusMultiStreamParameters(WithWorkBuffer) */
    uint32_t sample_rate;                  /* 8000, 12000, 16000, 24000, 48000 */
    uint32_t channel_count;                /* 1..255                            */
    uint32_t stream_count;                 /* total streams                     */
    uint32_t stereo_stream_count;          /* coupled streams                   */
    uint8_t mapping[255];
} napple_hwopus_ms_params;
size_t napple_hwopus_work_buffer_size(uint32_t sample_rate, uint32_t channels);
size_t napple_hwopus_ms_work_buffer_size(const napple_hwopus_ms_params *p);
int napple_hwopus_open(uint32_t sample_rate, uint32_t channels, napple_hwopus **out);
int napple_hwopus_open_ms(const napple_hwopus_ms_params *p, napple_hwopus **out);
void napple_hwopus_close(napple_hwopus *d);
int napple_hwopus_reset(napple_hwopus *d);
/* DecodeInterleaved. `packet` starts with Nintendo's 8-byte big-endian header {payload size, final range}.
   pcm receives interleaved int16; *consumed = bytes of `packet` used; *samples = samples per channel decoded.
   A header final range of 0 is not verified. A mismatching non-zero range is E_DECODE. */
int napple_hwopus_decode(napple_hwopus *d, const uint8_t *packet, size_t packet_size, int16_t *pcm,
                         size_t pcm_bytes, size_t *consumed, int32_t *samples);

#ifdef __cplusplus
}
#endif
#endif
