/* nn::codec hardware Opus decoder backend on top of libopus (the console's hwopus service is a hardware libopus).

   Input packets carry Nintendo's 8-byte big-endian header {payload size, final range}.  The decoder's final range
   is compared with the header's (a value of 0 is not a recorded range and is not checked: some encoders write 0).
   Each napple_hwopus owns its libopus state; handles are independent and may be used from different threads, a
   single handle is serialised by its own lock. */
#include "internal.h"
#include <limits.h>
#include <opus.h>
#include <opus_multistream.h>
#include <stdlib.h>
#include <string.h>

struct napple_hwopus {
    OpusDecoder *dec;
    OpusMSDecoder *ms;
    uint32_t rate, channels;
    pthread_mutex_t lock;
};

static int valid_rate(uint32_t r) { return r == 48000 || r == 24000 || r == 16000 || r == 12000 || r == 8000; }

size_t napple_hwopus_work_buffer_size(uint32_t rate, uint32_t channels) {
    if (!valid_rate(rate) || (channels != 1 && channels != 2)) return 0;
    return (size_t)opus_decoder_get_size((int)channels) + 0x1000; /* the game's buffer is reserved but unused */
}

size_t napple_hwopus_ms_work_buffer_size(const napple_hwopus_ms_params *p) {
    if (!p || !valid_rate(p->sample_rate) || p->channel_count < 1 || p->channel_count > 255 || p->stream_count < 1 || p->stereo_stream_count > p->stream_count)
        return 0;
    return (size_t)opus_multistream_decoder_get_size((int)p->stream_count, (int)p->stereo_stream_count) + 0x1000;
}

int napple_hwopus_open(uint32_t rate, uint32_t channels, napple_hwopus **out) {
    if (!out || !napple_hwopus_work_buffer_size(rate, channels)) return NAPPLE_AUDIO_E_INVALID;
    napple_hwopus *d = calloc(1, sizeof *d);
    if (!d) return NAPPLE_AUDIO_E_NOMEM;
    int err = 0;
    d->dec = opus_decoder_create((opus_int32)rate, (int)channels, &err);
    if (!d->dec || err != OPUS_OK) {
        free(d);
        return NAPPLE_AUDIO_E_DECODE;
    }
    d->rate = rate;
    d->channels = channels;
    pthread_mutex_init(&d->lock, NULL);
    *out = d;
    return 0;
}

int napple_hwopus_open_ms(const napple_hwopus_ms_params *p, napple_hwopus **out) {
    if (!out || !napple_hwopus_ms_work_buffer_size(p)) return NAPPLE_AUDIO_E_INVALID;
    napple_hwopus *d = calloc(1, sizeof *d);
    if (!d) return NAPPLE_AUDIO_E_NOMEM;
    int err = 0;
    d->ms =
        opus_multistream_decoder_create((opus_int32)p->sample_rate, (int)p->channel_count, (int)p->stream_count, (int)p->stereo_stream_count, p->mapping, &err);
    if (!d->ms || err != OPUS_OK) {
        free(d);
        return NAPPLE_AUDIO_E_INVALID;
    }
    d->rate = p->sample_rate;
    d->channels = p->channel_count;
    pthread_mutex_init(&d->lock, NULL);
    *out = d;
    return 0;
}

void napple_hwopus_close(napple_hwopus *d) {
    if (!d) return;
    if (d->dec) opus_decoder_destroy(d->dec);
    if (d->ms) opus_multistream_decoder_destroy(d->ms);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

int napple_hwopus_reset(napple_hwopus *d) {
    if (!d) return NAPPLE_AUDIO_E_INVALID;
    pthread_mutex_lock(&d->lock);
    int rc = d->dec ? opus_decoder_ctl(d->dec, OPUS_RESET_STATE) : opus_multistream_decoder_ctl(d->ms, OPUS_RESET_STATE);
    pthread_mutex_unlock(&d->lock);
    return rc == OPUS_OK ? 0 : NAPPLE_AUDIO_E_DECODE;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int napple_hwopus_decode(napple_hwopus *d, const uint8_t *packet, size_t packet_size, int16_t *pcm, size_t pcm_bytes, size_t *consumed, int32_t *samples) {
    if (!d || !packet || !pcm || !consumed || !samples || packet_size < 8) return NAPPLE_AUDIO_E_INVALID;
    const uint32_t n = be32(packet), range = be32(packet + 4);
    if (n > packet_size - 8 || n > INT_MAX) return NAPPLE_AUDIO_E_INVALID;
    const size_t capacity = pcm_bytes / (sizeof(int16_t) * d->channels);
    if (capacity > INT_MAX) return NAPPLE_AUDIO_E_INVALID;
    pthread_mutex_lock(&d->lock);
    int got;
    opus_uint32 final = 0;
    if (d->dec) {
        got = opus_decode(d->dec, packet + 8, (opus_int32)n, pcm, (int)capacity, 0);
        if (got >= 0) opus_decoder_ctl(d->dec, OPUS_GET_FINAL_RANGE(&final));
    } else {
        got = opus_multistream_decode(d->ms, packet + 8, (opus_int32)n, pcm, (int)capacity, 0);
        if (got >= 0) opus_multistream_decoder_ctl(d->ms, OPUS_GET_FINAL_RANGE(&final));
    }
    pthread_mutex_unlock(&d->lock);
    if (got < 0) {
        napple_audio_log(NULL, "opus_decode: %s", opus_strerror(got));
        return NAPPLE_AUDIO_E_DECODE;
    }
    if (n && range && final != range) {
        napple_audio_log(NULL, "Opus final range %#x != stream %#x", (unsigned) final, (unsigned)range);
        return NAPPLE_AUDIO_E_DECODE;
    }
    *consumed = 8 + (size_t)n;
    *samples = got;
    return 0;
}
