/* audin capture sources: spec parsing, tone, looped WAV/raw file with resampling, the mic ring resampler, and the env default.  No device. */
#include "wire.h"
#include <unistd.h>

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

/* RIFF/WAVE, PCM16 */
static size_t make_wav(uint8_t *out, const int16_t *pcm, uint32_t frames, uint32_t ch, uint32_t rate) {
    uint32_t dlen = frames * ch * 2;
    memcpy(out, "RIFF", 4);
    put32(out + 4, 36 + dlen);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16);
    put16(out + 20, 1);
    put16(out + 22, (uint16_t)ch);
    put32(out + 24, rate);
    put32(out + 28, rate * ch * 2);
    put16(out + 32, (uint16_t)(ch * 2));
    put16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    put32(out + 40, dlen);
    memcpy(out + 44, pcm, dlen);
    return 44 + dlen;
}
static void write_file(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    fwrite(d, 1, n, f);
    fclose(f);
}

static napple_audin *open_in(uint32_t rate, uint32_t ch) {
    napple_audin *in;
    uint32_t gr, gc;
    napple_audin_open(rate, ch, NULL, &in, &gr, &gc);
    napple_audin_set_manual(in, 1);
    napple_audin_start(in);
    return in;
}
static void capture(napple_audin *in, int16_t *dst, uint32_t frames, uint32_t ch) {
    CHECK(napple_audin_append(in, 1, (uint64_t)(uintptr_t)dst, (uint64_t)frames * ch * 2, 0) == 0, "append");
    napple_audin_pump(in, frames);
    uint64_t tag;
    CHECK(napple_audin_get_released(in, &tag, 1) == 1 && tag == 1, "buffer released");
}

static void t_specs(void) {
    CHECK(napple_audin_install_source_spec("tone:bogus") == NAPPLE_AUDIO_E_INVALID, "bad tone");
    CHECK(napple_audin_install_source_spec("tone:440:2") == NAPPLE_AUDIO_E_INVALID, "amp out of range");
    CHECK(napple_audin_install_source_spec("tone:99999") == NAPPLE_AUDIO_E_INVALID, "hz out of range");
    CHECK(napple_audin_install_source_spec("nonsense") == NAPPLE_AUDIO_E_INVALID, "unknown spec");
    CHECK(napple_audin_install_source_spec("file:/nonexistent/x.wav") == NAPPLE_AUDIO_E_INVALID, "missing file");
    CHECK(!napple_audin_has_source(), "failed installs leave no source");
    CHECK(napple_audin_install_source_spec("tone") == 0 && napple_audin_has_source(), "default tone");
    CHECK(napple_audin_install_source_spec("silence") == 0 && !napple_audin_has_source(), "silence removes the source");
    CHECK(napple_audin_install_source_spec("mic") == NAPPLE_AUDIO_E_UNSUPPORTED && !napple_audin_has_source(), "mic is unsupported off macOS");
}

static void t_tone(void) {
    CHECK(napple_audin_install_source_spec("tone:1000:0.5") == 0, "tone installed");
    napple_audin *in = open_in(48000, 1);
    int16_t buf[480];
    capture(in, buf, 480, 1);
    int ok = 1;
    for (int k = 0; k < 480; k++) ok &= abs(buf[k] - (int)lrint(0.5 * sin(2 * M_PI * 1000.0 * k / 48000.0) * 32768.0)) <= 1;
    CHECK(ok, "1 kHz tone at amplitude 0.5, sample exact");
    napple_audin_close(in);
    in = open_in(16000, 2); /* the rate follows the stream */
    int16_t st[320 * 2];
    capture(in, st, 320, 2);
    ok = 1;
    for (int k = 0; k < 320; k++) ok &= st[2 * k] == st[2 * k + 1];
    CHECK(ok, "mono tone fills both channels");
    CHECK(abs(st[1 * 2] - (int)lrint(0.5 * sin(2 * M_PI * 1000.0 / 16000.0) * 32768.0)) <= 1, "tone phase advances per output rate");
    napple_audin_close(in);
    napple_audin_install_source_spec("silence");
}

static void t_file(void) {
    char path[] = "/tmp/napple_audin_XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    /* 16 kHz stereo WAV: left = ramp, right = -ramp -> mono mix is 0 */
    int16_t pcm[2 * 100];
    for (int k = 0; k < 100; k++) pcm[2 * k] = (int16_t)(k * 100), pcm[2 * k + 1] = (int16_t)(-k * 100);
    uint8_t wav[44 + sizeof pcm];
    write_file(path, wav, make_wav(wav, pcm, 100, 2, 16000));
    char spec[64];
    snprintf(spec, sizeof spec, "file:%s", path);
    CHECK(napple_audin_install_source_spec(spec) == 0, "stereo WAV loaded");
    napple_audin *in = open_in(48000, 1);
    int16_t out[300];
    capture(in, out, 300, 1);
    int ok = 1;
    for (int k = 0; k < 300; k++) ok &= out[k] == 0;
    CHECK(ok, "stereo downmix of opposite channels is silent");
    napple_audin_close(in);
    /* mono 48 kHz WAV ramp is reproduced exactly and loops */
    int16_t ramp[64];
    for (int k = 0; k < 64; k++) ramp[k] = (int16_t)(k * 200);
    uint8_t wav2[44 + sizeof ramp];
    write_file(path, wav2, make_wav(wav2, ramp, 64, 1, 48000));
    CHECK(napple_audin_install_source_spec(spec) == 0, "mono WAV loaded");
    in = open_in(48000, 1);
    int16_t o2[160];
    capture(in, o2, 160, 1);
    ok = 1;
    for (int k = 0; k < 160; k++) ok &= abs(o2[k] - ramp[k % 64]) <= 1;
    CHECK(ok, "48 kHz mono WAV reproduced and looped");
    napple_audin_close(in);
    /* double rate request: every other source sample, interpolated between */
    CHECK(napple_audin_install_source_spec(spec) == 0, "reinstall restarts the file");
    in = open_in(96000, 1);
    int16_t o3[8];
    capture(in, o3, 8, 1);
    CHECK(abs(o3[0] - 0) <= 1 && abs(o3[1] - 100) <= 1 && abs(o3[2] - 200) <= 1, "upsampling interpolates linearly (%d %d %d)", o3[0], o3[1], o3[2]);
    napple_audin_close(in);
    /* raw s16le */
    write_file(path, ramp, sizeof ramp);
    CHECK(napple_audin_install_source_spec(spec) == 0, "raw s16le loaded");
    in = open_in(48000, 1);
    capture(in, o2, 64, 1);
    CHECK(abs(o2[63] - ramp[63]) <= 1, "raw file read as 48 kHz mono");
    napple_audin_close(in);
    /* truncated / hostile WAV falls back to raw rather than crashing */
    memcpy(wav, "RIFF", 4);
    write_file(path, wav, 30);
    CHECK(napple_audin_install_source_spec(spec) == 0, "truncated WAV is treated as raw samples");
    napple_audin_install_source_spec("silence");
    unlink(path);
}

static void t_mic_ring(void) {
    nae_mic_ring_reset();
    float ramp[1000], got[2000];
    for (int k = 0; k < 1000; k++) ramp[k] = (float)k;
    CHECK(nae_mic_read(NULL, got, 10, 48000) == 0, "empty ring gives nothing");
    nae_mic_ring_push(ramp, 1000);
    uint32_t n = nae_mic_read(NULL, got, 400, 48000);
    CHECK(n == 400 && got[0] == 0.f && got[399] == 399.f, "same rate passes through (%u)", n);
    n = nae_mic_read(NULL, got, 2000, 48000);
    CHECK(n == 599, "reader stops one sample short of the write head (%u)", n); /* needs i+1 */
    nae_mic_ring_reset();
    nae_mic_ring_push(ramp, 1000);
    n = nae_mic_read(NULL, got, 100, 24000); /* half rate: every other sample */
    CHECK(n == 100 && got[1] == 2.f && got[50] == 100.f, "24 kHz takes every second 48 kHz sample");
    nae_mic_ring_reset();
    nae_mic_ring_push(ramp, 1000);
    n = nae_mic_read(NULL, got, 100, 96000); /* double rate: interpolated */
    CHECK(n == 100 && got[1] == 0.5f && got[2] == 1.f, "96 kHz interpolates");
    /* overflow: far more than the ring holds */
    static float big[40000];
    for (int k = 0; k < 40000; k++) big[k] = (float)k;
    nae_mic_ring_reset();
    nae_mic_ring_push(big, 20000);
    nae_mic_ring_push(big + 20000, 20000);
    n = nae_mic_read(NULL, got, 100, 48000);
    CHECK(n == 100 && got[0] > 30000.f, "a reader that fell behind jumps to recent audio (%g)", got[0]);
}

static void t_env(void) {
    napple_audin_install_source_spec("silence");
    unsetenv("NAPPLE_AUDIN_SOURCE");
    CHECK(napple_audin_install_default_source() == 0 && !napple_audin_has_source(), "unset env = silence");
    setenv("NAPPLE_AUDIN_SOURCE", "tone:220", 1);
    CHECK(napple_audin_install_default_source() == 0 && napple_audin_has_source(), "env tone installed");
    setenv("NAPPLE_AUDIN_SOURCE", "tone:880", 1);
    CHECK(napple_audin_install_default_source() == 0, "an installed source is kept");
    napple_audin_install_source_spec("silence");
    setenv("NAPPLE_AUDIN_SOURCE", "bogus", 1);
    CHECK(napple_audin_install_default_source() == NAPPLE_AUDIO_E_INVALID && !napple_audin_has_source(), "bad env falls back to silence");
    unsetenv("NAPPLE_AUDIN_SOURCE");
}

int main(void) {
    t_specs();
    t_tone();
    t_file();
    t_mic_ring();
    t_env();
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
