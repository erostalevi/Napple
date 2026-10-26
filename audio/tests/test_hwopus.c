/* hwopus: encode with libopus, wrap in Nintendo's packet header, decode through the host backend. */
#include "wire.h"
#include <opus.h>
#include <opus_multistream.h>

static size_t make_packet(OpusEncoder *enc, const int16_t *pcm, int frame, uint8_t *out, uint32_t range_override, int use_override) {
    unsigned char data[1500];
    int n = opus_encode(enc, pcm, frame, data, sizeof data);
    opus_uint32 range = 0;
    opus_encoder_ctl(enc, OPUS_GET_FINAL_RANGE(&range));
    if (use_override) range = range_override;
    out[0] = (uint8_t)(n >> 24);
    out[1] = (uint8_t)(n >> 16);
    out[2] = (uint8_t)(n >> 8);
    out[3] = (uint8_t)n;
    out[4] = (uint8_t)(range >> 24);
    out[5] = (uint8_t)(range >> 16);
    out[6] = (uint8_t)(range >> 8);
    out[7] = (uint8_t)range;
    memcpy(out + 8, data, (size_t)n);
    return 8 + (size_t)n;
}

int main(void) {
    CHECK(napple_hwopus_work_buffer_size(48000, 2) > 0x1000, "work buffer size");
    CHECK(napple_hwopus_work_buffer_size(44100, 2) == 0 && napple_hwopus_work_buffer_size(48000, 3) == 0, "invalid formats rejected");
    int err;
    OpusEncoder *enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
    static int16_t pcm[960 * 2 * 8];
    for (int k = 0; k < 960 * 8; k++) pcm[2 * k] = pcm[2 * k + 1] = (int16_t)(12000 * sin(2 * M_PI * 440.0 * k / 48000.0));
    napple_hwopus *d;
    CHECK(napple_hwopus_open(48000, 2, &d) == 0, "open");
    uint8_t pkt[1600];
    static int16_t out[960 * 2 * 2];
    double dec_energy = 0;
    long dec_n = 0;
    for (int f = 0; f < 8; f++) {
        size_t n = make_packet(enc, pcm + f * 960 * 2, 960, pkt, 0, 0), consumed = 0;
        int32_t samples = 0;
        int rc = napple_hwopus_decode(d, pkt, n + 20 /* trailing bytes must not be consumed */, out, sizeof out, &consumed, &samples);
        CHECK(rc == 0 && consumed == n && samples == 960, "decode frame %d rc %d consumed %zu/%zu samples %d", f, rc, consumed, n, samples);
        if (f >= 3)
            for (int k = 0; k < 960; k++) {
                dec_energy += (double)out[2 * k] * out[2 * k];
                dec_n++;
            }
    }
    /* the decoded 440 Hz tone (amplitude 12000) must have the right RMS (8485) regardless of codec delay */
    double rms = sqrt(dec_energy / (double)dec_n);
    CHECK(rms > 7500 && rms < 9500, "decoded RMS %.0f ~ 8485", rms);
    /* a wrong final range is an integrity error; zero is not checked */
    size_t n = make_packet(enc, pcm, 960, pkt, 0x12345678u, 1), consumed;
    int32_t samples;
    CHECK(napple_hwopus_decode(d, pkt, n, out, sizeof out, &consumed, &samples) == NAPPLE_AUDIO_E_DECODE, "range mismatch detected");
    n = make_packet(enc, pcm, 960, pkt, 0, 1);
    CHECK(napple_hwopus_decode(d, pkt, n, out, sizeof out, &consumed, &samples) == 0, "range 0 not checked");
    /* malformed: payload size beyond the buffer, truncated header, tiny output */
    uint8_t bad[16] = {0, 0, 0xff, 0xff, 0, 0, 0, 0};
    CHECK(napple_hwopus_decode(d, bad, sizeof bad, out, sizeof out, &consumed, &samples) == NAPPLE_AUDIO_E_INVALID, "oversized payload rejected");
    CHECK(napple_hwopus_decode(d, bad, 4, out, sizeof out, &consumed, &samples) == NAPPLE_AUDIO_E_INVALID, "short packet rejected");
    n = make_packet(enc, pcm, 960, pkt, 0, 1);
    CHECK(napple_hwopus_decode(d, pkt, n, out, 64, &consumed, &samples) != 0, "output too small fails");
    CHECK(napple_hwopus_reset(d) == 0, "reset");
    napple_hwopus_close(d);
    /* mono + 24 kHz */
    OpusEncoder *e1 = opus_encoder_create(24000, 1, OPUS_APPLICATION_VOIP, &err);
    static int16_t m[480], mo[480];
    for (int k = 0; k < 480; k++) m[k] = (int16_t)(8000 * sin(k * 0.1));
    napple_hwopus *d1;
    CHECK(napple_hwopus_open(24000, 1, &d1) == 0, "mono open");
    n = make_packet(e1, m, 480, pkt, 0, 0);
    CHECK(napple_hwopus_decode(d1, pkt, n, mo, sizeof mo, &consumed, &samples) == 0 && samples == 480, "mono 24k decode");
    napple_hwopus_close(d1);
    /* multistream: 4 channels = 2 coupled streams */
    unsigned char map[4] = {0, 1, 2, 3};
    OpusMSEncoder *ms = opus_multistream_encoder_create(48000, 4, 2, 2, map, OPUS_APPLICATION_AUDIO, &err);
    static int16_t q[960 * 4], qo[960 * 4];
    for (int k = 0; k < 960; k++)
        for (int c = 0; c < 4; c++) q[4 * k + c] = (int16_t)(6000 * sin(k * 0.05 * (c + 1)));
    unsigned char data[2000];
    int dn = opus_multistream_encode(ms, q, 960, data, sizeof data);
    opus_uint32 fr = 0;
    opus_multistream_encoder_ctl(ms, OPUS_GET_FINAL_RANGE(&fr));
    static uint8_t mp[2100];
    mp[0] = 0;
    mp[1] = 0;
    mp[2] = (uint8_t)(dn >> 8);
    mp[3] = (uint8_t)dn;
    mp[4] = (uint8_t)(fr >> 24);
    mp[5] = (uint8_t)(fr >> 16);
    mp[6] = (uint8_t)(fr >> 8);
    mp[7] = (uint8_t)fr;
    memcpy(mp + 8, data, (size_t)dn);
    napple_hwopus_ms_params mp_params = {.sample_rate = 48000, .channel_count = 4, .stream_count = 2, .stereo_stream_count = 2};
    for (int i = 0; i < 4; i++) mp_params.mapping[i] = map[i];
    napple_hwopus *dm;
    CHECK(napple_hwopus_open_ms(&mp_params, &dm) == 0, "multistream open");
    CHECK(napple_hwopus_decode(dm, mp, 8 + (size_t)dn, qo, sizeof qo, &consumed, &samples) == 0 && samples == 960, "multistream decode");
    napple_hwopus_close(dm);
    opus_encoder_destroy(enc);
    opus_encoder_destroy(e1);
    opus_multistream_encoder_destroy(ms);
    printf("{\"test\":\"hwopus\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
