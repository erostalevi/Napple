/* Engine: refcounting, master volume, resampling source, stats on the null sink. Optional device smoke test with
   NAPPLE_TEST_DEVICE=1 (plays 200 ms of a quiet sine). */
#include "wire.h"
#include <unistd.h>

int main(void) {
    napple_audio_config c = {.use_device = 0};
    CHECK(napple_audio_init(&c) == 0 && napple_audio_init(&c) == 0, "init twice");
    napple_audio_set_master_volume(0.25f); CHECK(nearf(napple_audio_get_master_volume(), 0.25f, 1e-6f), "master volume");
    napple_audio_set_master_volume(7); CHECK(nearf(napple_audio_get_master_volume(), 1.f, 1e-6f), "master volume clamp");
    char name[32]; CHECK(napple_audio_device_name(name, sizeof name) == 13 && !strcmp(name, "AudioTvOutput"), "device name");
    CHECK(napple_audio_device_name(name, 5) == 4 && !strcmp(name, "Audi"), "device name truncation");
    napple_audio_device_set_volume(0.5f); CHECK(nearf(napple_audio_device_get_volume(), .5f, 1e-6f), "device volume");
    nae_source *s = nae_source_create(32000);
    CHECK(s != NULL, "source");
    float in[2 * 320]; for (int k = 0; k < 320; k++) in[2 * k] = in[2 * k + 1] = (float)k / 320.f;
    nae_source_push(s, in, 160); nae_source_push(s, in + 320, 160);
    CHECK(nae_source_frames_written(s) >= 478 && nae_source_frames_written(s) <= 481, "32 kHz -> 48 kHz: 320 in, %llu out", (unsigned long long)nae_source_frames_written(s));
    float out[2 * 200]; CHECK(nae_source_peek(s, out, 200) == 200, "peek");
    float maxstep = 0; for (int k = 1; k < 200; k++) { float d = fabsf(out[2 * k] - out[2 * (k - 1)]); if (d > maxstep) maxstep = d; }
    CHECK(maxstep < 0.004f && out[2 * 199] > out[0], "ramp stays monotonic and continuous across pushes (step %f)", maxstep);
    nae_source_destroy(s);
    napple_audio_stats st; napple_audio_get_stats(&st); CHECK(!st.device_open, "null sink has no device");
    napple_audio_shutdown(); napple_audio_shutdown();
    if (getenv("NAPPLE_TEST_DEVICE")) {
        napple_audio_config dc = {.use_device = 1, .master_volume = 0.1f}; napple_audio_init(&dc);
        nae_source *d = nae_source_create(48000); float blk[480];
        for (int b = 0; b < 40; b++) { for (int k = 0; k < 240; k++) blk[2 * k] = blk[2 * k + 1] = (float)(0.2 * sin(2 * M_PI * 660 * (b * 240 + k) / 48000.0)); nae_source_push(d, blk, 240); usleep(5000); }
        napple_audio_get_stats(&st); printf("device smoke: device_open=%d callback_frames=%llu underruns=%llu latency=%.1fms\n", st.device_open, (unsigned long long)st.callback_frames, (unsigned long long)st.underruns, st.latency_ms);
        nae_source_destroy(d); napple_audio_shutdown();
    }
    printf("{\"test\":\"engine\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
