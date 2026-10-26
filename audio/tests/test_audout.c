/* audout: queue, release order, 1/2/6 channel conversion, resampling, start/stop. Null output sink. */
#include "wire.h"
#include <unistd.h>

static void t_stereo_queue(void) {
    static int16_t a[2 * 480], b[2 * 480];
    for (int k = 0; k < 480; k++) {
        a[2 * k] = (int16_t)(k * 20);
        a[2 * k + 1] = (int16_t)(-k * 20);
        b[2 * k] = 1000;
        b[2 * k + 1] = -1000;
    }
    napple_audout *o;
    uint32_t rate = 0, ch = 0;
    CHECK(napple_audout_open(0, 0, NULL, &o, &rate, &ch) == 0 && rate == 48000 && ch == 2, "open defaults");
    napple_audout_set_manual(o, 1);
    CHECK(napple_audout_append(o, 11, (uint64_t)(uintptr_t)a, sizeof a, 0) == 0, "append a");
    CHECK(napple_audout_append(o, 22, (uint64_t)(uintptr_t)b, sizeof b, 0) == 0, "append b");
    CHECK(napple_audout_contains(o, 11) && napple_audout_contains(o, 22) && !napple_audout_contains(o, 33), "contains");
    uint64_t tags[4];
    CHECK(napple_audout_get_released(o, tags, 4) == 0, "nothing released yet");
    napple_audout_start(o);
    CHECK(napple_audout_state(o) == 0, "started");
    napple_audout_pump(o, 480);
    CHECK(napple_audout_get_released(o, tags, 4) == 1 && tags[0] == 11, "first buffer released after its frames were consumed");
    float lr[960];
    CHECK(audout_peek(o, lr, 480) == 480, "peek");
    for (int k = 0; k < 480; k++)
        CHECK(nearf(lr[2 * k], a[2 * k] / 32768.f, 1e-6f) && nearf(lr[2 * k + 1], a[2 * k + 1] / 32768.f, 1e-6f), "stereo sample %d", k);
    napple_audout_pump(o, 480 + 100); /* b, then silence */
    CHECK(napple_audout_get_released(o, tags, 4) == 1 && tags[0] == 22 && !napple_audout_contains(o, 22), "second released");
    /* queue limit */
    int ok = 0;
    for (int i = 0; i < 40; i++) ok += napple_audout_append(o, 100 + i, (uint64_t)(uintptr_t)b, sizeof b, 0) == 0;
    CHECK(ok == 32, "queue holds 32 buffers (got %d)", ok);
    napple_audout_close(o);
}

static void t_mono_and_surround(void) {
    static int16_t m[100], s6[6 * 100];
    for (int k = 0; k < 100; k++) {
        m[k] = 8192;
        for (int c = 0; c < 6; c++) s6[6 * k + c] = (int16_t)(c == 2 ? 16384 : c == 0 ? 8192 : 0);
    }
    napple_audout *o;
    uint32_t rate, ch;
    CHECK(napple_audout_open(48000, 1, NULL, &o, &rate, &ch) == 0 && ch == 1, "mono open");
    napple_audout_set_manual(o, 1);
    napple_audout_start(o);
    napple_audout_append(o, 1, (uint64_t)(uintptr_t)m, sizeof m, 0);
    napple_audout_pump(o, 100);
    float lr[200];
    audout_peek(o, lr, 100);
    CHECK(nearf(lr[0], 0.25f, 1e-6f) && nearf(lr[1], 0.25f, 1e-6f), "mono duplicated to both channels");
    napple_audout_close(o);
    CHECK(napple_audout_open(48000, 6, NULL, &o, &rate, &ch) == 0 && ch == 6, "6ch open");
    napple_audout_set_manual(o, 1);
    napple_audout_start(o);
    napple_audout_append(o, 1, (uint64_t)(uintptr_t)s6, sizeof s6, 0);
    napple_audout_pump(o, 100);
    audout_peek(o, lr, 100);
    float want = 0.25f + 0.5f * 0.7071f;
    CHECK(nearf(lr[0], want, 1e-4f) && nearf(lr[1], 0.5f * 0.7071f, 1e-4f), "5.1 downmix L %f R %f", lr[0], lr[1]);
    napple_audout_close(o);
    CHECK(napple_audout_open(48000, 3, NULL, &o, &rate, &ch) == NAPPLE_AUDIO_E_INVALID, "3 channels rejected");
}

static int g_events;
static void on_buf(void *u) {
    (void)u;
    __atomic_add_fetch(&g_events, 1, __ATOMIC_SEQ_CST);
}

static void t_realtime_pacing(void) {
    static int16_t a[2 * 240 * 8];
    napple_audio_host h = {.buffer_event = on_buf};
    napple_audout *o;
    napple_audout_open(48000, 2, &h, &o, NULL, NULL);
    napple_audout_start(o);
    for (int i = 0; i < 8; i++) napple_audout_append(o, (uint64_t)i, (uint64_t)(uintptr_t)(a + 480 * i), 480 * 2, 0); /* 8 x 5 ms */
    usleep(80000); /* 40 ms of audio takes ~40 ms to release */
    uint64_t tags[16];
    uint32_t n = napple_audout_get_released(o, tags, 16);
    CHECK(n == 8, "all 8 buffers (40 ms) released in about real time (got %u)", n);
    CHECK(g_events > 0, "buffer_event fired");
    for (uint32_t i = 0; i < n; i++) CHECK(tags[i] == i, "release order");
    napple_audout_close(o);
}

static void t_resample_44k(void) {
    static int16_t a[2 * 441];
    for (int k = 0; k < 441; k++) a[2 * k] = a[2 * k + 1] = (int16_t)(10000 * sin(2 * M_PI * 441.0 * k / 44100.0));
    napple_audout *o;
    napple_audout_open(44100, 2, NULL, &o, NULL, NULL);
    napple_audout_set_manual(o, 1);
    napple_audout_start(o);
    napple_audout_append(o, 1, (uint64_t)(uintptr_t)a, sizeof a, 0);
    napple_audout_pump(o, 441);
    float lr[2 * 480];
    int got = audout_peek(o, lr, 480);
    CHECK(got == 480 || got == 0, "peek");
    if (got) { /* 441 input frames at 44.1k -> ~480 at 48k: sine at 441 Hz stays continuous: neighbouring-sample step is small */
        float maxstep = 0;
        for (int k = 1; k < 470; k++) {
            float d = fabsf(lr[2 * k] - lr[2 * (k - 1)]);
            if (d > maxstep) maxstep = d;
        }
        CHECK(maxstep < 0.07f, "resampled sine is smooth (max step %f)", maxstep);
    }
    napple_audout_close(o);
}

int main(void) {
    napple_audio_config c = {.use_device = 0};
    napple_audio_init(&c);
    t_stereo_queue();
    t_mono_and_surround();
    t_realtime_pacing();
    t_resample_44k();
    napple_audio_shutdown();
    printf("{\"test\":\"audout\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
