/* Thread safety: a real render thread runs while two threads hammer RequestUpdate and a third toggles start/stop and
   the master volume / pause. Run under ThreadSanitizer with: make -C audio tsan */
#include "wire.h"
#include <pthread.h>
#include <unistd.h>

static napple_audren *g_r;
static int16_t g_pcm[4800];
static _Atomic int g_stop;
static _Atomic int g_updates, g_errors;

static void *hammer(void *arg) {
    int id = (int)(intptr_t)arg;
    AudrenParameter p = param_default();
    p.voice_count = 1;
    p.effect_count = 0;
    p.submix_count = 0;
    Update u;
    scene_init(&u, 11, &p);
    u.voices[0] = voice_pcm(0, FMT_PCM16, g_pcm, sizeof g_pcm, 4800);
    u.voices[0].wave_buffers[0].loop = 1;
    u.voices[0].wave_buffers[0].loop_count = -1;
    u.nchans = 1;
    u.chans[0].id = 0;
    u.chans[0].mix[0] = 1;
    static __thread uint8_t in[16384], out[16384];
    (void)id;
    while (!atomic_load(&g_stop)) {
        size_t n = build_update(&u, in), w;
        int rc = napple_audren_request_update(g_r, in, n, out, sizeof out, &w, NULL, 0);
        if (rc) atomic_fetch_add(&g_errors, 1);
        else atomic_fetch_add(&g_updates, 1);
        u.voices[0].is_new = 0;
        u.voices[0].wave_buffers[0].sent = 1;
        usleep(300);
    }
    return NULL;
}
static void *toggler(void *arg) {
    (void)arg;
    for (int i = 0; !atomic_load(&g_stop); i++) {
        napple_audren_stop(g_r);
        usleep(900);
        napple_audren_start(g_r);
        napple_audio_set_master_volume(i & 1 ? .5f : 1.f);
        napple_audio_set_paused(i % 7 == 0);
        usleep(900);
    }
    return NULL;
}
int main(void) {
    napple_audio_config c = {.use_device = 0};
    napple_audio_init(&c);
    for (int i = 0; i < 4800; i++) g_pcm[i] = (int16_t)(8000 * sin(i * 0.1));
    AudrenParameter p = param_default();
    p.voice_count = 1;
    p.effect_count = 0;
    p.submix_count = 0;
    napple_audren_open(&p, sizeof p, NULL, 0, &g_r);
    napple_audren_start(g_r);
    pthread_t t[3];
    pthread_create(&t[0], NULL, hammer, (void *)1);
    pthread_create(&t[1], NULL, hammer, (void *)2);
    pthread_create(&t[2], NULL, toggler, NULL);
    usleep(1000000);
    atomic_store(&g_stop, 1);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
    CHECK(atomic_load(&g_updates) > 100 && atomic_load(&g_errors) == 0, "updates %d errors %d", atomic_load(&g_updates), atomic_load(&g_errors));
    CHECK(napple_audren_elapsed_frames(g_r) > 20, "render thread advanced (%llu frames)", (unsigned long long)napple_audren_elapsed_frames(g_r));
    napple_audren_close(g_r);
    napple_audio_set_paused(0);
    napple_audio_shutdown();
    printf("{\"test\":\"threads\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
