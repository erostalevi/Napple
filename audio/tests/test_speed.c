/* Game speed in the output engine: pitch-shifting resampler, auto mute, pacer periods. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, ...)                                                                                                                                          \
    do {                                                                                                                                                       \
        if (!(c)) {                                                                                                                                            \
            fails++;                                                                                                                                           \
            printf("FAIL line %d: ", __LINE__);                                                                                                                \
            printf(__VA_ARGS__);                                                                                                                               \
            printf("\n");                                                                                                                                      \
        }                                                                                                                                                      \
    } while (0)

static uint64_t push_block(nae_source *s, int frames, double hz, double *phase) {
    float *b = malloc((size_t)frames * 2 * sizeof(float));
    for (int i = 0; i < frames; i++) {
        float v = (float)sin(*phase);
        *phase += 2 * M_PI * hz / 48000.0;
        b[2 * i] = b[2 * i + 1] = v;
    }
    uint64_t before = nae_source_frames_written(s);
    nae_source_push(s, b, frames);
    free(b);
    return nae_source_frames_written(s) - before;
}
/* zero crossings of the last `n` frames written: frequency estimate */
static double peek_hz(nae_source *s, int n) {
    float *b = malloc((size_t)n * 2 * sizeof(float));
    if (!nae_source_peek(s, b, n)) {
        free(b);
        return -1;
    }
    int z = 0;
    for (int i = 1; i < n; i++)
        if (b[2 * (i - 1)] < 0 && b[2 * i] >= 0) z++;
    free(b);
    return z * 48000.0 / n;
}
int main(void) {
    napple_audio_init(&(napple_audio_config){.use_device = 0});
    nae_source *s = nae_source_create(48000);
    double ph = 0;
    napple_audio_set_speed(1.0, NAPPLE_AUDIO_SPEED_AUTO);
    CHECK(push_block(s, 4800, 440, &ph) == 4800, "1x passes through untouched");
    napple_audio_set_speed(2.0, NAPPLE_AUDIO_SPEED_AUTO);
    uint64_t w = 0;
    for (int i = 0; i < 10; i++) w += push_block(s, 480, 440, &ph);
    CHECK(w >= 2395 && w <= 2405, "2x consumes the source twice as fast: 4800 in -> %llu out", (unsigned long long)w);
    double f = peek_hz(s, 2000);
    CHECK(f > 800 && f < 960, "pitch of a 440 Hz tone at 2x is ~880 Hz (got %.0f)", f);
    napple_audio_set_speed(0.5, NAPPLE_AUDIO_SPEED_AUTO);
    w = 0;
    for (int i = 0; i < 10; i++) w += push_block(s, 480, 440, &ph);
    CHECK(w >= 9595 && w <= 9605, "0.5x: 4800 in -> %llu out", (unsigned long long)w);
    f = peek_hz(s, 4000);
    CHECK(f > 200 && f < 240, "pitch at 0.5x is ~220 Hz (got %.0f)", f);
    napple_audio_set_speed(4.0, NAPPLE_AUDIO_SPEED_AUTO);
    CHECK(nae_speed_muted() && push_block(s, 480, 440, &ph) == 0, "auto mode mutes at 4x");
    napple_audio_set_speed(4.0, NAPPLE_AUDIO_SPEED_PITCH);
    CHECK(!nae_speed_muted() && push_block(s, 480, 440, &ph) > 100, "pitch mode keeps sound at 4x");
    napple_audio_set_speed(1.0, NAPPLE_AUDIO_SPEED_MUTE);
    CHECK(nae_speed_muted() && push_block(s, 480, 440, &ph) == 0, "mute mode mutes at 1x");
    napple_audio_set_speed(1.0, NAPPLE_AUDIO_SPEED_AUTO);
    CHECK(!nae_speed_muted() && push_block(s, 480, 440, &ph) == 480, "back to normal");
    napple_audio_set_speed(4.0, NAPPLE_AUDIO_SPEED_PITCH);
    CHECK(nae_scaled_ns(5000000) == 1250000, "renderer period at 4x is 1.25 ms");
    napple_audio_set_speed(99.0, 0);
    CHECK(napple_audio_get_speed() == 10.0, "clamped");
    napple_audio_set_speed(1.0, 0);
    nae_source_destroy(s);
    printf("test_speed: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
