/* Renderer behaviour reviewed against audren: submix / splitter ordering, voice drop and render metrics, rejected effect parameters.
   Headless (manual render mode, null sink), no device. */
#include "wire.h"
#include "reverb.h"
#include "effects.h"

static float g_dc[240 * 64];

/* n splitters, splitter i has one destination i (volume vol0 on buffer 0) pointing at dest[i] (SNDH / SNDI / SNDD, REV11 destination stride 0x70). */
static void splitters(Update *u, int n, const int32_t *dest, float vol0) {
    memset(u->splitter, 0, sizeof u->splitter);
    uint32_t h[3] = {0x48444e53u, (uint32_t)n, (uint32_t)n};
    memcpy(u->splitter, h, 12);
    uint8_t *w = u->splitter + 0x20;
    for (int i = 0; i < n; i++) {
        uint32_t info[5] = {0x49444e53u, (uint32_t)i, 48000, 1, (uint32_t)i};
        memcpy(w, info, 20);
        w += 32;
    }
    for (int i = 0; i < n; i++) {
        uint32_t magic = 0x44444e53u, id = (uint32_t)i;
        float vol[24] = {vol0};
        int32_t d = dest[i];
        memcpy(w, &magic, 4);
        memcpy(w + 4, &id, 4);
        memcpy(w + 8, vol, sizeof vol);
        memcpy(w + 0x68, &d, 4);
        w[0x6c] = 1;
        w += 0x70;
    }
    u->splitter_bytes = (uint32_t)(w - u->splitter);
}
static void submix(Update *u, int id, int32_t dest_mix, uint32_t dest_splitter) {
    u->mixes[id] = (AudrenMixIn){
        .volume = 1, .sample_rate = 48000, .buffer_count = 2, .in_use = 1, .mix_id = id, .dest_mix_id = dest_mix, .dest_splitter_id = dest_splitter};
    u->mixes[id].mix[0][0] = 1;
    u->mixes[id].mix[1][1] = 1;
}
static void next_frame(Update *u) {
    for (int i = 0; i < u->nvoices; i++) {
        u->voices[i].is_new = 0;
        u->voices[i].wave_buffers[0].sent = 1;
    }
    for (int i = 0; i < u->neffects; i++) u->effects[i].is_new = 0;
}

/* Voice -> submix A --splitter--> submix B -> final mix.  B may have a lower id than A, which a depth-from-dest_mix_id sort puts first:
   B then folds before A has sent anything, and since the mix buffers are cleared every frame that audio never arrives. */
static void t_submix_splitter_order(void) {
    for (int reversed = 0; reversed < 2; reversed++) {
        AudrenParameter p = param_default();
        p.submix_count = 2;
        p.voice_count = 1;
        p.effect_count = 0;
        p.splitter_count = 1;
        p.splitter_destination_count = 1;
        Rig g;
        rig_open(&g, p);
        Update u;
        scene_init(&u, 11, &p);
        const int a = reversed ? 2 : 1, b = reversed ? 1 : 2; /* a feeds b */
        submix(&u, b, 0, 0xffffffffu);
        submix(&u, a, 0x7fffffff, 0);
        int32_t dest = b;
        splitters(&u, 1, &dest, 1.0f);
        u.voices[0] = voice_pcm(0, FMT_PCMF32, g_dc, sizeof g_dc, 240 * 64);
        u.voices[0].mix_id = a;
        u.nchans = 1;
        u.chans[0].id = 0;
        u.chans[0].mix[0] = 1;
        u.chans[0].in_use = 1;
        float lr[480];
        for (int f = 0; f < 3; f++) {
            CHECK(rig_update(&g, &u, NULL) == 0, "order update");
            rig_frame(&g, lr);
            next_frame(&u);
            CHECK(nearf(lr[0], .25f, 1e-5f) && nearf(lr[2 * 239], .25f, 1e-5f),
                  "mix %d -> splitter -> mix %d: frame %d first/last sample %f %f, want 0.25 (reached the output in the same frame)", a, b, f, lr[0],
                  lr[2 * 239]);
        }
        napple_audren_close(g.r);
    }
}
/* A routing cycle (1 -> 2 -> 1 through splitters) must neither hang nor fault the renderer. */
static void t_submix_cycle(void) {
    AudrenParameter p = param_default();
    p.submix_count = 2;
    p.voice_count = 1;
    p.effect_count = 0;
    p.splitter_count = 2;
    p.splitter_destination_count = 2;
    Rig g;
    rig_open(&g, p);
    Update u;
    scene_init(&u, 11, &p);
    submix(&u, 1, 0x7fffffff, 0);
    submix(&u, 2, 0x7fffffff, 1);
    int32_t dest[2] = {2, 1};
    splitters(&u, 2, dest, 1.0f);
    u.voices[0] = voice_pcm(0, FMT_PCMF32, g_dc, sizeof g_dc, 240 * 64);
    u.voices[0].mix_id = 1;
    u.nchans = 1;
    u.chans[0].id = 0;
    u.chans[0].mix[0] = 1;
    u.chans[0].in_use = 1;
    float lr[480];
    CHECK(rig_update(&g, &u, NULL) == 0, "cycle update");
    for (int f = 0; f < 4; f++) {
        rig_frame(&g, lr);
        next_frame(&u);
        rig_update(&g, &u, NULL);
    }
    CHECK(!g.r->faulted && napple_audren_elapsed_frames(g.r) == 4, "routing cycle faulted the renderer");
    napple_audren_close(g.r);
}

typedef struct {
    uint64_t calls, dur;
} Clock;
static uint64_t fake_now(void *user) {
    Clock *c = user;
    uint64_t n = c->calls++;
    return (n / 2) * 1000000000ull + ((n & 1) ? c->dur : 0);
}

static void drop_scene(Rig *g, Update *u, int enabled, const int32_t prio[3], Clock *clk) {
    AudrenParameter p = param_default();
    p.voice_count = 3;
    p.effect_count = 0;
    p.submix_count = 0;
    p.voice_drop_enabled = (uint8_t)enabled;
    rig_open(g, p);
    scene_init(u, 11, &p);
    static const float amp[3] = {.1f, .2f, .4f};
    static float dc[3][240 * 64];
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 240 * 64; k++) dc[v][k] = amp[v];
        u->voices[v] = voice_pcm((uint32_t)v, FMT_PCMF32, dc[v], sizeof dc[v], 240 * 64);
        u->voices[v].priority = prio[v];
        u->chans[v].id = (uint32_t)v;
        u->chans[v].mix[0] = 1;
        u->chans[v].in_use = 1;
    }
    u->nchans = 3;
    napple_audren_set_time_limit(g->r, 50); /* 50% of the 5 ms frame: 2.5 ms */
    napple_audren_set_clock(g->r, fake_now, clk);
}
static void t_voice_drop(void) {
    const int32_t prio[3] = {10, 20, 30}; /* 30 is the lowest priority */
    /* enabled: over budget once, then fine */
    Clock clk = {0, 4000000};
    Rig g;
    Update u;
    drop_scene(&g, &u, 1, prio, &clk);
    float lr[480];
    static const uint64_t dur[6] = {4000000, 1000000, 1000000, 1000000, 1000000, 1000000};
    static const float want[6] = {.7f, .1f, .3f, .7f, .7f, .7f}; /* frame 1 over budget -> 2 voices dropped in frame 2, one comes back after each good frame */
    for (int f = 0; f < 6; f++) {
        clk.dur = dur[f];
        CHECK(rig_update(&g, &u, NULL) == 0, "drop update");
        rig_frame(&g, lr);
        next_frame(&u);
        CHECK(nearf(lr[0], want[f], 1e-5f), "voice drop frame %d: output %f want %f", f, lr[0], want[f]);
    }
    CHECK(g.r->voices[0].drops == 0 && g.r->voices[1].drops == 1 && g.r->voices[2].drops == 2, "drops per voice %u %u %u, want 0 1 2", g.r->voices[0].drops,
          g.r->voices[1].drops, g.r->voices[2].drops);
    CHECK(g.r->voices[0].played == g.r->voices[1].played && g.r->voices[1].played == g.r->voices[2].played && g.r->voices[2].played == 6 * 240,
          "dropped voices stopped consuming their buffers (played %llu %llu %llu)", (unsigned long long)g.r->voices[0].played,
          (unsigned long long)g.r->voices[1].played, (unsigned long long)g.r->voices[2].played);
    AudrenHeader oh;
    memcpy(&oh, g.out, sizeof oh);
    AudrenVoiceOut vo[3];
    memcpy(vo, g.out + sizeof oh + oh.memory_pools, sizeof vo);
    CHECK(vo[1].voice_drops == 1 && vo[2].voice_drops == 2, "voice_drops in the update output: %u %u", vo[1].voice_drops, vo[2].voice_drops);
    u.voices[2].clear_voice_drop = 1;
    rig_update(&g, &u, NULL);
    CHECK(g.r->voices[2].drops == 0, "clear_voice_drop did not reset the count");
    napple_audren_stats st;
    napple_audren_get_stats(g.r, &st);
    CHECK(st.frames == 6 && st.over_budget_frames == 1 && st.late_frames == 0 && st.dropped_voice_frames == 3 && st.render_ns_max == 4000000,
          "stats: frames %llu over %llu late %llu dropped %llu max %llu", (unsigned long long)st.frames, (unsigned long long)st.over_budget_frames,
          (unsigned long long)st.late_frames, (unsigned long long)st.dropped_voice_frames, (unsigned long long)st.render_ns_max);
    napple_audren_close(g.r);
    /* equal priorities: the highest id goes first */
    const int32_t same[3] = {5, 5, 5};
    clk = (Clock){0, 4000000};
    drop_scene(&g, &u, 1, same, &clk);
    for (int f = 0; f < 2; f++) {
        rig_update(&g, &u, NULL);
        rig_frame(&g, lr);
        next_frame(&u);
        clk.dur = 1000000;
    }
    CHECK(nearf(lr[0], .1f, 1e-5f), "equal priority: kept %f, want only voice 0 (0.1)", lr[0]);
    napple_audren_close(g.r);
    /* not enabled by the client: never drops, however slow */
    clk = (Clock){0, 4000000};
    drop_scene(&g, &u, 0, prio, &clk);
    for (int f = 0; f < 4; f++) {
        rig_update(&g, &u, NULL);
        rig_frame(&g, lr);
        next_frame(&u);
        CHECK(nearf(lr[0], .7f, 1e-5f), "voice drop disabled but frame %d output %f", f, lr[0]);
    }
    napple_audren_stats st2;
    napple_audren_get_stats(g.r, &st2);
    CHECK(st2.dropped_voice_frames == 0 && st2.over_budget_frames == 4, "disabled drop stats: dropped %llu over %llu",
          (unsigned long long)st2.dropped_voice_frames, (unsigned long long)st2.over_budget_frames);
    napple_audren_close(g.r);
    /* a frame longer than the whole period is a real-time miss */
    clk = (Clock){0, 6000000};
    drop_scene(&g, &u, 0, prio, &clk);
    rig_update(&g, &u, NULL);
    rig_frame(&g, lr);
    napple_audren_get_stats(g.r, &st2);
    CHECK(st2.late_frames == 1, "6 ms frame in a 5 ms period not counted late");
    napple_audren_close(g.r);
}

/* Rejected effect parameters bypass the effect; they used to trap in the render path and mute the renderer for good. */
static void t_rejected_effect_parameters(void) {
    for (int kind = 0; kind < 4; kind++) {
        AudrenParameter p = param_default();
        p.voice_count = 1;
        p.effect_count = 1;
        p.submix_count = 0;
        Rig g;
        rig_open(&g, p);
        Update u;
        scene_init(&u, 11, &p);
        u.voices[0] = voice_pcm(0, FMT_PCMF32, g_dc, sizeof g_dc, 240 * 64);
        u.nchans = 1;
        u.chans[0].id = 0;
        u.chans[0].mix[0] = 1;
        u.chans[0].mix[1] = 1;
        u.chans[0].in_use = 1;
        u.effects[0] = (AudrenEffectIn){.is_new = 1, .enabled = 1, .mix_id = 0};
        if (kind == 0) {
            Reverb3dParam r = {.input = {0, 1},
                               .output = {0, 1},
                               .channel_max = 6,
                               .channels = 2,
                               .sample_rate = 48000,
                               .decay_time = 0,
                               .hf_decay_ratio = 1,
                               .density = 50,
                               .status = 1};
            u.effects[0].type = 5;
            memcpy(u.effects[0].specific, &r, sizeof r);
        }
        if (kind == 1) {
            AudrenDelayParam d = {
                .input = {0, 1}, .output = {0, 1}, .channel_max = 2, .channels = 2, .delay_max = 100, .delay = 500, .sample_rate = 48000u << 14, .status = 1};
            u.effects[0].type = 3;
            memcpy(u.effects[0].specific, &d, sizeof d);
        }
        if (kind == 2) {
            AudrenCompressorParam c = {.input = {0, 1}, .output = {0, 1}, .channel_max = 2, .channels = 2, .sample_rate = 48000, .ratio = 0, .status = 1};
            u.effects[0].type = 9;
            memcpy(u.effects[0].specific, &c, sizeof c);
        }
        if (kind == 3) {
            AudrenReverbParam r = {.input = {0, 1},
                                   .output = {0, 1},
                                   .channel_max = 2,
                                   .channels = 2,
                                   .sample_rate = 48 << 14,
                                   .early_mode = 9,
                                   .decay = 16384,
                                   .hf_decay = 16384,
                                   .status = 1};
            u.effects[0].type = 4;
            memcpy(u.effects[0].specific, &r, sizeof r);
        }
        float lr[480];
        for (int f = 0; f < 3; f++) {
            CHECK(rig_update(&g, &u, NULL) == 0, "kind %d update", kind);
            rig_frame(&g, lr);
            next_frame(&u);
            CHECK(nearf(lr[0], .25f, 1e-5f) && nearf(lr[1], .25f, 1e-5f), "kind %d frame %d: bypassed effect output %f %f, want the dry 0.25", kind, f, lr[0],
                  lr[1]);
        }
        CHECK(!g.r->faulted && napple_audren_elapsed_frames(g.r) == 3, "kind %d: renderer faulted on rejected effect parameters", kind);
        napple_audren_close(g.r);
    }
}


/* Delay effect: echoes repeat every `delay` samples (the reference line wraps one short, so 479 for 480) and fall by the feedback gain each time. */
static void t_delay_feedback(void) {
    AudrenDelayParam d = {.input = {0},
                          .output = {0},
                          .channel_max = 1,
                          .channels = 1,
                          .delay_max = 1000,
                          .delay = 10,
                          .sample_rate = 48000u << 14,
                          .in_gain = 16384,
                          .feedback = 8192,
                          .out_gain = 16384,
                          .dry_gain = 16384,
                          .status = 1};
    AudioEffectDsp *e = audio_effect_create(3, &d, 11);
    enum { N = 4000 };
    static float x[N];
    x[0] = 1;
    for (int k = 0; k < N; k += 240) {
        float *i6[6] = {x + k}, *o6[6] = {x + k};
        audio_effect_process(e, i6, o6, k + 240 <= N ? 240 : N - k);
    }
    int peaks[8], np = 0;
    float amp[8];
    for (int k = 0; k < N && np < 8; k++)
        if (fabsf(x[k]) > 1e-3f) {
            peaks[np] = k;
            amp[np++] = x[k];
        }
    CHECK(np >= 7 && peaks[0] == 0, "delay: %d echoes found", np);
    for (int i = 1; i < np && i < 7; i++) {
        CHECK(abs(peaks[i] - peaks[i - 1] - 480) <= 1, "delay: echo %d spacing %d, want 480 +-1", i, peaks[i] - peaks[i - 1]);
        if (i >= 2) CHECK(nearf(amp[i] / amp[i - 1], .5f, 1e-3f), "delay: echo %d ratio %f, want the feedback gain 0.5", i, amp[i] / amp[i - 1]);
    }
    audio_effect_free(e);
}
/* Standard reverb: the tail falls 60 dB in `decay` seconds (late mode 1, no high-frequency damping). */
static void t_standard_reverb_decay(void) {
    for (int mode = 0; mode < 4; mode++) {
        AudrenReverbParam p = {.input = {0, 1},
                               .output = {0, 1},
                               .channel_max = 2,
                               .channels = 2,
                               .sample_rate = 48 << 14,
                               .early_mode = mode,
                               .early_gain = 16384,
                               .late_mode = mode,
                               .late_gain = 16384,
                               .decay = 2 * 16384,
                               .hf_decay = 16384,
                               .coloration = 8192,
                               .reverb_gain = 16384,
                               .out_gain = 16384,
                               .dry_gain = 0,
                               .status = 1};
        AudioEffectDsp *e = audio_effect_create(4, &p, 11);
        enum { N = 48000 * 4 };
        static float b[2][N];
        memset(b, 0, sizeof b);
        b[0][0] = b[1][0] = 1;
        for (int k = 0; k < N; k += 240) {
            float *i6[6] = {b[0] + k, b[1] + k}, *o6[6] = {b[0] + k, b[1] + k};
            audio_effect_process(e, i6, o6, 240);
        }
        double db[2]; /* energy of 0.5 s windows at 0.5 s and 1.5 s: one second apart */
        for (int w = 0; w < 2; w++) {
            double en = 0;
            int from = 24000 + w * 48000;
            for (int c = 0; c < 2; c++)
                for (int k = from; k < from + 24000; k++) en += (double)b[c][k] * b[c][k];
            db[w] = 10 * log10(en + 1e-30);
        }
        double rt60 = 60.0 / (db[0] - db[1]);
        CHECK(rt60 > 1.6 && rt60 < 2.4, "standard reverb mode %d: decay 2 s measured RT60 %.2f s", mode, rt60);
        audio_effect_free(e);
    }
}

int main(void) {
    napple_audio_config c = {.use_device = 0};
    napple_audio_init(&c);
    for (unsigned k = 0; k < sizeof g_dc / sizeof *g_dc; k++) g_dc[k] = .25f;
    t_submix_splitter_order();
    t_submix_cycle();
    t_voice_drop();
    t_rejected_effect_parameters();
    t_delay_feedback();
    t_standard_reverb_decay();
    napple_audio_shutdown();
    printf("{\"test\":\"review\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
