/* Demo: plays a quiet two-tone through the real audren path (REV11 update blobs -> mixer -> Core Audio) for ~1.5 s.
   Run: make -C audio demo   (volume is capped at 0.1 master) */
#include "wire.h"
#include <time.h>
#include <unistd.h>

int main(void) {
    napple_audio_config c = {.use_device = 1, .master_volume = 0.1f};
    napple_audio_init(&c);
    static int16_t tone[48000];
    for (int i = 0; i < 48000; i++) tone[i] = (int16_t)(12000 * sin(2 * M_PI * 440.0 * i / 48000.0) + (i > 24000 ? 6000 * sin(2 * M_PI * 660.0 * i / 48000.0) : 0));
    AudrenParameter p = param_default(); p.voice_count = 1; p.effect_count = 0; p.submix_count = 0;
    napple_audren *r; if (napple_audren_open(&p, sizeof p, NULL, 0, &r)) return 1;   /* real render thread */
    Update u; scene_init(&u, 11, &p);
    u.voices[0] = voice_pcm(0, FMT_PCM16, tone, sizeof tone, 48000);
    u.voices[0].wave_buffers[0].loop = 1; u.voices[0].wave_buffers[0].loop_count = -1;
    u.nchans = 1; u.chans[0].id = 0; u.chans[0].mix[0] = 1; u.chans[0].mix[1] = 1; u.chans[0].in_use = 1;
    static uint8_t in[16384], out[16384]; size_t n = build_update(&u, in), w;
    napple_audren_start(r);
    for (int i = 0; i < 300; i++) {
        int rc = napple_audren_request_update(r, in, n, out, sizeof out, &w, NULL, 0);
        if (rc) { fprintf(stderr, "update failed %d\n", rc); break; }
        u.voices[0].is_new = 0; u.voices[0].wave_buffers[0].sent = 1; n = build_update(&u, in);
        usleep(5000);
    }
    napple_audio_stats st; napple_audio_get_stats(&st);
    printf("device_open=%d callback_frames=%llu underruns=%llu overruns=%llu latency=%.1f ms elapsed_frames=%llu\n", st.device_open,
           (unsigned long long)st.callback_frames, (unsigned long long)st.underruns, (unsigned long long)st.overruns, st.latency_ms, (unsigned long long)napple_audren_elapsed_frames(r));
    napple_audren_close(r); napple_audio_shutdown();
    return 0;
}
