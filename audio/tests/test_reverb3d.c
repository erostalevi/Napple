/* I3DL2 reverb (effect type 5, src/reverb.c) against analytic impulse responses.  Runs the DSP directly, no mixer, no device.
   The model below restates the documented structure (Ryujinx.Audio Reverb3d, MIT): 20 early taps on a 400 ms pre-delay line at
   n_i = round(khz * (reflection_delay + early_time_i * (reverb_delay * .9998 + .02)) * 1000) samples, summed into the output
   channel the channel layout assigns them, scaled by the early gain; the late FDN is fed from the pre line at reflection_time. */
#include "wire.h"
#include "reverb.h"

#define RATE 48000
static const float early_times[20] = {.017136f, .059154f, .16173f, .39019f, .42526f, .45541f, .68974f, .74591f, .83384f, .8595f,
                                      0,        .075024f, .16879f, .2999f,  .33744f, .3719f,  .59901f, .71674f, .81786f, .85166f};
static const float early_gains[20] = {.67096f, .61027f, 1,       .35680f, .68361f, .65978f, .51939f, .24712f, .45945f, .45021f,
                                      .64196f, .54879f, .92925f, .38270f, .72867f, .69794f, .5464f,  .24563f, .45214f, .44042f};
static const unsigned stereo_map[20] = {0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1};

static Reverb3dParam base(int channels) {
    Reverb3dParam p = {.channel_max = 6,
                       .channels = (uint16_t)channels,
                       .sample_rate = RATE,
                       .room_hf = 0,
                       .hf_reference = 5000,
                       .decay_time = 1.0f,
                       .hf_decay_ratio = 1.0f,
                       .room_gain = 0,
                       .reflections_gain = 0,
                       .reverb_gain = -20000 /* late path off */,
                       .diffusion = 100,
                       .density = 100,
                       .reflection_delay = 0,
                       .reverb_delay = .04f,
                       .dry_gain = 0,
                       .status = 1};
    for (int i = 0; i < 6; i++) p.input[i] = p.output[i] = (uint8_t)i;
    return p;
}
/* Runs `frames` samples in blocks of 240 (the renderer's frame). in == NULL: out-of-place into zeroed buffers pre-filled with `fill`. */
static void run(const Reverb3dParam *p, float *const in[6], float *const out[6], int frames) {
    Reverb3d *r = audio_reverb_create(p);
    for (int k = 0; k < frames; k += 240) {
        float *i6[6], *o6[6];
        int n = frames - k < 240 ? frames - k : 240;
        for (int c = 0; c < 6; c++) {
            i6[c] = in[c] + k;
            o6[c] = out[c] + k;
        }
        audio_reverb_process(r, i6, o6, n);
    }
    audio_reverb_free(r);
}
static float *buf(int n) { return calloc((size_t)n, sizeof(float)); }

static void t_early_reflections_match_model(void) {
    const int N = 40000; /* longer than the 400 ms pre-delay ring (19200): a stale slot would replay the impulse inside this window */
    Reverb3dParam p = base(2);
    float *b[6], *want[6];
    for (int c = 0; c < 6; c++) {
        b[c] = buf(N);
        want[c] = buf(N);
    }
    b[0][0] = 1;
    run(&p, b, b, N);
    for (int i = 0; i < 20; i++) {
        unsigned n = (unsigned)nearbyintf(48 * (p.reflection_delay * 1000 + early_times[i] * 1000 * (p.reverb_delay * .9998f + .02f)));
        want[stereo_map[i]][n + 1] += early_gains[i]; /* the tap reads the sample written n + 1 updates ago */
    }
    int bad = 0;
    double worst = 0;
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < N; k++) {
            double d = fabs(b[c][k] - want[c][k]);
            if (d > worst) worst = d;
            if (d > 1e-5) bad++;
        }
    CHECK(bad == 0, "early reflection impulse response differs from the model at %d samples (worst %g)", bad, worst);
    double stale = 0;
    for (int c = 0; c < 2; c++)
        for (int k = 5000; k < N; k++) stale += (double)b[c][k] * b[c][k];
    CHECK(stale < 1e-12, "impulse replays after the pre-delay ring wrapped (energy %g)", stale);
    for (int c = 0; c < 6; c++) {
        free(b[c]);
        free(want[c]);
    }
}

static double window_db(float *const b[6], int channels, int from, int n) {
    double e = 0;
    for (int c = 0; c < channels; c++)
        for (int k = from; k < from + n; k++) e += (double)b[c][k] * b[c][k];
    return 10 * log10(e + 1e-30);
}
static void t_decay_time(void) {
    const float times[3] = {.5f, 1.f, 2.f};
    for (int t = 0; t < 3; t++)
        for (int ch = 1; ch <= 4; ch *= 2) {
            const int N = RATE * 6;
            Reverb3dParam p = base(ch);
            p.reverb_gain = 0;
            p.decay_time = times[t];
            float *b[6];
            for (int c = 0; c < 6; c++) b[c] = buf(N);
            b[0][0] = 1;
            run(&p, b, b, N);
            /* least-squares slope of 25 ms window energy (dB) from 120 ms (after the early reflections) over 60% of the decay time */
            const int w = RATE / 40, first = 5, last = first + (int)(times[t] * 40 * 0.6f);
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            int n = 0;
            for (int i = first; i < last && (i + 1) * w < N; i++) {
                double y = window_db(b, ch, i * w, w), x = i * .025;
                sx += x;
                sy += y;
                sxx += x * x;
                sxy += x * y;
                n++;
            }
            double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx), rt60 = -60 / slope;
            CHECK(n >= 5 && rt60 > times[t] * .8 && rt60 < times[t] * 1.25, "decay_time %.1f s, %d channels: measured RT60 %.2f s (slope %.1f dB/s)", times[t],
                  ch, rt60, slope);
            for (int c = 0; c < 6; c++) free(b[c]);
        }
}

static void t_dry_path_and_aliasing(void) {
    const int N = 4800;
    for (int ch = 1; ch <= 6; ch = ch == 2 ? 4 : ch == 4 ? 6 : ch + 1) {
        Reverb3dParam p = base(ch);
        p.dry_gain = 1;
        float *in[6], *a[6], *o[6];
        float noise[6][4800];
        for (int c = 0; c < 6; c++)
            for (int k = 0; k < N; k++) noise[c][k] = ((k * 7 + c * 13) % 17 - 8) / 8.f;
        for (int c = 0; c < 6; c++) {
            in[c] = buf(N);
            a[c] = buf(N);
            o[c] = buf(N);
            memcpy(in[c], noise[c], sizeof noise[c]);
            memcpy(a[c], noise[c], sizeof noise[c]);
            for (int k = 0; k < N; k++) o[c][k] = 0.5f;
        }
        run(&p, a, a, N);  /* in place, as the mixer does */
        run(&p, in, o, N); /* out of place, destination pre-filled with unrelated data */
        int diff = 0;
        double dry_err = 0;
        for (int c = 0; c < ch; c++)
            for (int k = 0; k < N; k++) {
                if (fabsf(a[c][k] - o[c][k]) > 1e-6f) diff++;
                if (k < 1) dry_err = fmax(dry_err, fabs(o[c][k] - noise[c][k])); /* before the first early tap: output is exactly the dry input */
            }
        CHECK(diff == 0, "%d channels: in-place and out-of-place output differ at %d samples (the output buffer must be overwritten, not accumulated)", ch,
              diff);
        CHECK(dry_err < 1e-6, "%d channels: first sample is not the dry input (error %g)", ch, dry_err);
        /* dry_gain 0 with the wet path off: nothing of the input may remain, in any channel (including the 5.1 centre) */
        p.dry_gain = 0;
        p.room_gain = -20000; /* early gain 0 as well */
        for (int c = 0; c < 6; c++) { memcpy(a[c], noise[c], sizeof noise[c]); }
        run(&p, a, a, N);
        double left = 0;
        for (int c = 0; c < ch; c++)
            for (int k = 0; k < N; k++) left += (double)a[c][k] * a[c][k];
        CHECK(left < 1e-9, "%d channels: dry_gain 0 and wet off still outputs energy %g", ch, left);
        for (int c = 0; c < 6; c++) {
            free(in[c]);
            free(a[c]);
            free(o[c]);
        }
    }
}

static void t_linear_time_invariant(void) {
    const int N = 24000, D = 777;
    Reverb3dParam p = base(2);
    p.reverb_gain = 0;
    p.room_hf = -1500;
    p.hf_decay_ratio = .6f;
    float *x[6], *y[6], *z[6];
    for (int c = 0; c < 6; c++) {
        x[c] = buf(N);
        y[c] = buf(N);
        z[c] = buf(N);
    }
    x[0][0] = 1;
    run(&p, x, x, N);
    y[0][D] = 2.5f;
    run(&p, y, y, N);
    double worst = 0;
    for (int c = 0; c < 2; c++)
        for (int k = D; k < N; k++) worst = fmax(worst, fabs(y[c][k] - 2.5 * x[c][k - D]));
    CHECK(worst < 1e-4, "response is not linear and time invariant (worst deviation %g)", worst);
    for (int c = 0; c < 6; c++) {
        free(x[c]);
        free(y[c]);
        free(z[c]);
    }
}

static void t_stable_everywhere(void) {
    const int N = RATE * 4;
    for (int ch = 1; ch <= 6; ch = ch == 2 ? 4 : ch == 4 ? 6 : ch + 1)
        for (int mode = 0; mode < 4; mode++) {
            Reverb3dParam p = base(ch);
            p.reverb_gain = 0;
            p.dry_gain = 1;
            p.density = (mode & 1) ? 0 : 100;
            p.decay_time = (mode & 2) ? 20 : .1f;
            p.hf_decay_ratio = (mode & 1) ? .1f : 2;
            p.room_hf = (mode & 2) ? -4000 : 0;
            p.diffusion = (mode & 1) ? 0 : 100;
            p.reverb_delay = .1f;
            p.reflection_delay = .3f; /* the 400 ms ceiling */
            float *b[6];
            for (int c = 0; c < 6; c++) {
                b[c] = buf(N);
                for (int k = 0; k < N; k++) b[c][k] = k < RATE ? ((int)((k * 2654435761u + c * 40503u) >> 7) % 2001 - 1000) / 1000.f : 0;
            }
            run(&p, b, b, N);
            double peak = 0;
            int finite = 1;
            for (int c = 0; c < ch; c++)
                for (int k = 0; k < N; k++) {
                    finite &= isfinite(b[c][k]);
                    peak = fmax(peak, fabs(b[c][k]));
                }
            CHECK(finite && peak < 200, "%d channels, mode %d: unstable (peak %g)", ch, mode, peak);
            double tail = window_db(b, ch, N - RATE / 2, RATE / 2), head = window_db(b, ch, RATE / 4, RATE / 2);
            if (p.decay_time < 1)
                CHECK(tail < head - 20, "%d channels, mode %d: short decay is not decaying (head %.1f dB, tail %.1f dB)", ch, mode, head, tail);
            for (int c = 0; c < 6; c++) free(b[c]);
        }
}

static void t_invalid_parameters(void) {
    Reverb3dParam p = base(2);
    CHECK(audio_reverb_valid(&p), "valid parameters accepted");
    p.decay_time = 0;
    CHECK(!audio_reverb_valid(&p), "zero decay time rejected");
    p = base(2);
    p.channels = 3;
    CHECK(!audio_reverb_valid(&p), "3 channels rejected");
    p = base(2);
    p.sample_rate = 0;
    CHECK(!audio_reverb_valid(&p), "zero sample rate rejected");
    p = base(2);
    p.reflection_delay = .3f;
    p.reverb_delay = .2f;
    CHECK(!audio_reverb_valid(&p), "delays beyond 400 ms rejected");
    p = base(2);
    p.decay_time = NAN;
    CHECK(!audio_reverb_valid(&p), "NaN decay time rejected");
}

int main(void) {
    t_early_reflections_match_model();
    t_decay_time();
    t_dry_path_and_aliasing();
    t_linear_time_invariant();
    t_stable_everywhere();
    t_invalid_parameters();
    printf("{\"test\":\"reverb3d\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
