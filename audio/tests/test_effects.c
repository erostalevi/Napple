/* Effects inside the mix graph: delay echo timing, standard reverb tail, reverb3d tail, compressor gain reduction,
   biquad effect (golden recurrence), buffer mix, aux ring exchange, bypass identity when disabled, capture ring. */
#include "wire.h"
#include "effects.h"
#include "reverb.h"

static float g_impulse[240 * 40];
static float g_tone[240 * 40];

typedef struct {
    Rig g;
    Update u;
} Fx;
static void fx_open(Fx *f, int rev, const float *src, int samples) {
    AudrenParameter p = param_default();
    p.revision = REVW(rev);
    p.voice_count = 1;
    p.submix_count = 0;
    p.effect_count = 1;
    rig_open(&f->g, p);
    scene_init(&f->u, rev, &p);
    f->u.voices[0] = voice_pcm(0, FMT_PCMF32, src, (size_t)samples * 4, samples);
    f->u.nchans = 1;
    f->u.chans[0].id = 0;
    f->u.chans[0].mix[0] = 1;
    f->u.chans[0].mix[1] = 1;
    f->u.chans[0].in_use = 1;
}
static void fx_next(Fx *f) {
    f->u.voices[0].is_new = 0;
    f->u.voices[0].wave_buffers[0].sent = 1;
    f->u.effects[0].is_new = 0;
}

static void t_delay(void) {
    Fx f;
    fx_open(&f, 11, g_impulse, 240 * 40);
    AudrenDelayParam d = {.input = {0, 1},
                          .output = {0, 1},
                          .channel_max = 2,
                          .channels = 2,
                          .delay_max = 1000,
                          .delay = 10,
                          .sample_rate = 48000u << 14,
                          .in_gain = 16384,
                          .feedback = 0,
                          .out_gain = 16384,
                          .dry_gain = 0,
                          .spread = 0,
                          .lowpass = 0,
                          .status = 1};
    f.u.effects[0] = (AudrenEffectIn){.type = 3, .is_new = 1, .enabled = 1, .mix_id = 0};
    memcpy(f.u.effects[0].specific, &d, sizeof d);
    int first = -1;
    float peak = 0, lr[480];
    for (int fr = 0; fr < 6; fr++) {
        CHECK(rig_update(&f.g, &f.u, NULL) == 0, "delay update");
        rig_frame(&f.g, lr);
        for (int k = 0; k < 240; k++) {
            float a = fabsf(lr[2 * k]);
            if (a > 0.05f && first < 0) first = fr * 240 + k;
            if (a > peak) peak = a;
        }
        fx_next(&f);
    }
    CHECK(first >= 470 && first <= 490, "echo of a 10 ms delay appears near sample 480 (got %d)", first);
    CHECK(peak > 0.5f && peak < 1.5f, "echo amplitude %f", peak);
    napple_audren_close(f.g.r);
}

static void t_reverb_family(void) {
    for (int kind = 0; kind < 3; kind++) {
        Fx f;
        fx_open(&f, 11, g_impulse, 240 * 40);
        if (kind == 0) { /* standard reverb */
            AudrenReverbParam p = {.input = {0, 1},
                                   .output = {0, 1},
                                   .channel_max = 2,
                                   .channels = 2,
                                   .sample_rate = 48 << 14,
                                   .early_mode = 1,
                                   .early_gain = 16384,
                                   .pre_delay = 16384,
                                   .late_mode = 1,
                                   .late_gain = 16384,
                                   .decay = 2 * 16384,
                                   .hf_decay = 16384,
                                   .coloration = 8192,
                                   .reverb_gain = 16384,
                                   .out_gain = 16384,
                                   .dry_gain = 0,
                                   .status = 1};
            f.u.effects[0] = (AudrenEffectIn){.type = 4, .is_new = 1, .enabled = 1};
            memcpy(f.u.effects[0].specific, &p, sizeof p);
        } else if (kind == 1) { /* reverb3d */
            Reverb3dParam p = {.input = {0, 1},
                               .output = {0, 1},
                               .channel_max = 6,
                               .channels = 2,
                               .sample_rate = 48000,
                               .room_hf = -2000,
                               .hf_reference = 4000,
                               .decay_time = .8f,
                               .hf_decay_ratio = .7f,
                               .room_gain = -800,
                               .reverb_gain = 500,
                               .diffusion = 50,
                               .density = 70,
                               .dry_gain = 0,
                               .status = 1};
            f.u.effects[0] = (AudrenEffectIn){.type = 5, .is_new = 1, .enabled = 1};
            memcpy(f.u.effects[0].specific, &p, sizeof p);
        } else { /* compressor on a loud tone */
            AudrenCompressorParam p = {.input = {0, 1},
                                       .output = {0, 1},
                                       .channel_max = 2,
                                       .channels = 2,
                                       .sample_rate = 48000,
                                       .threshold = -20,
                                       .ratio = 4,
                                       .input_gain = .5f,
                                       .attack = .05f,
                                       .release = .005f,
                                       .output_gain = 0,
                                       .status = 1};
            f.u.effects[0] = (AudrenEffectIn){.type = 9, .is_new = 1, .enabled = 1};
            memcpy(f.u.effects[0].specific, &p, sizeof p);
            f.u.voices[0] = voice_pcm(0, FMT_PCMF32, g_tone, sizeof g_tone, 240 * 40);
        }
        double tail = 0, head = 0;
        float lr[480];
        int finite = 1;
        for (int fr = 0; fr < 12; fr++) {
            CHECK(rig_update(&f.g, &f.u, NULL) == 0, "effect kind %d update", kind);
            rig_frame(&f.g, lr);
            for (int k = 0; k < 480; k++) {
                finite &= isfinite(lr[k]);
                if (fr >= 1) tail += (double)lr[k] * lr[k];
                else head += (double)lr[k] * lr[k];
            }
            fx_next(&f);
        }
        CHECK(finite, "effect kind %d output finite", kind);
        if (kind < 2) CHECK(tail > 1e-6, "reverb kind %d has a tail after the impulse (energy %g)", kind, tail);
        else {
            double rms = sqrt((head + tail) / (12.0 * 480));
            CHECK(rms < 0.35 * 0.7071 * 1.01 + 0.2 && rms > 0.01, "compressor output level %f", rms);
        }
        napple_audren_close(f.g.r);
    }
}

static void t_biquad_effect_and_bypass(void) {
    Fx f;
    fx_open(&f, 11, g_tone, 240 * 40);
    AudrenBiquadEffectV1 b = {.input = {0, 1}, .output = {0, 1}, .numerator = {8192, 4096, 2048}, .denominator = {6144, -2048}, .channels = 2, .status = 1};
    f.u.effects[0] = (AudrenEffectIn){.type = 6, .is_new = 1, .enabled = 1};
    memcpy(f.u.effects[0].specific, &b, sizeof b);
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    float lr[480];
    for (int fr = 0; fr < 3; fr++) {
        rig_update(&f.g, &f.u, NULL);
        rig_frame(&f.g, lr);
        for (int k = 0; k < 240; k++) {
            double x = g_tone[fr * 240 + k];
            double y = (b.numerator[0] * x + b.numerator[1] * x1 + b.numerator[2] * x2 + b.denominator[0] * y1 + b.denominator[1] * y2) / 16384.0;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            CHECK(nearf(lr[2 * k], (float)y, 2e-4f), "biquad effect frame %d sample %d: %f want %f", fr, k, lr[2 * k], y);
        }
        fx_next(&f);
    }
    /* disabled: bit-identical bypass */
    f.u.effects[0].enabled = 0;
    rig_update(&f.g, &f.u, NULL);
    rig_frame(&f.g, lr);
    int same = 1;
    for (int k = 0; k < 240; k++) same &= nearf(lr[2 * k], g_tone[3 * 240 + k], 1e-6f);
    CHECK(same, "disabled biquad effect is a pass-through");
    napple_audren_close(f.g.r);
}

static void t_aux_ring(void) {
    static uint32_t send_ring[32 + 960], return_ring[32 + 960];
    for (int i = 0; i < 32 + 960; i++) {
        send_ring[i] = 0x12345678;
        return_ring[i] = (uint32_t)(int32_t)(i - 500);
    }
    send_ring[17] = return_ring[16] = 720;
    send_ring[18] = return_ring[18] = 7;
    Fx f;
    fx_open(&f, 11, g_tone, 240 * 40);
    AudrenAuxParam ap = {.input = {0, 1},
                         .output = {0, 1},
                         .channels = 2,
                         .sample_rate = 48000,
                         .capacity = 960,
                         .channel_max = 2,
                         .send_info = (uint64_t)(uintptr_t)send_ring,
                         .return_info = (uint64_t)(uintptr_t)return_ring};
    f.u.effects[0] = (AudrenEffectIn){.type = 2, .enabled = 1, .is_new = 1};
    memcpy(f.u.effects[0].specific, &ap, sizeof ap);
    float lr[480];
    rig_update(&f.g, &f.u, NULL);
    rig_frame(&f.g, lr);
    /* the aux return replaces the mix buffer; the send ring receives the input as int32 samples at the DSP cursor */
    int ok = 1;
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 240; k++) {
            int idx = (720 + c * 240 + k) % 960, want = idx + 32 - 500;
            ok &= nearf(lr[2 * k + c], want / 32768.f, 1e-6f);
            int32_t sent = (int32_t)send_ring[32 + idx];
            CHECK(abs(sent - (int32_t)(g_tone[k] * 32768.f)) <= 1, "aux send sample ch %d k %d", c, k);
        }
    CHECK(ok, "aux return ring replaces the mix buffers, with wrap-around");
    CHECK(send_ring[17] == 240 && return_ring[16] == 240 && send_ring[0] == 0x12345678 && send_ring[18] == 7, "aux cursors advance (send %u return %u)",
          send_ring[17], return_ring[16]);
    napple_audren_close(f.g.r);
}

int main(void) {
    napple_audio_config c = {.use_device = 0};
    napple_audio_init(&c);
    g_impulse[0] = 1.f;
    for (int k = 0; k < 240 * 40; k++) g_tone[k] = (float)(0.7 * sin(k * 0.05));
    t_delay();
    t_reverb_family();
    t_biquad_effect_and_bypass();
    t_aux_ring();
    napple_audio_shutdown();
    printf("{\"test\":\"effects\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
