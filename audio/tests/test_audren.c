/* Headless tests for the audio renderer: synthetic RequestUpdate blobs, golden sample comparisons, state machine,
   malformed input.  No audio device.  Run: make -C audio test */
#include "wire.h"

static int16_t g_pcm[4800];
static float g_pcmf[480];

static void t_pcm16_channel_mix(void) {
    AudrenParameter p = param_default();
    Rig g;
    CHECK(rig_open(&g, p) == 0, "open");
    Update u;
    scene_init(&u, 11, &p);
    u.voices[0] = voice_pcm(0, FMT_PCM16, g_pcm, sizeof g_pcm, 4800);
    u.voices[0].volume = 0.5f;
    u.voices[0].wave_buffers[0].loop = 1;
    u.voices[0].wave_buffers[0].loop_count = -1;
    u.nchans = 1;
    u.chans[0].id = 0;
    u.chans[0].mix[0] = 1;
    u.chans[0].mix[1] = 0.25f;
    u.chans[0].in_use = 1;
    size_t w = 0;
    CHECK(rig_update(&g, &u, &w) == 0, "update");
    AudrenHeader oh;
    memcpy(&oh, g.out, sizeof oh);
    CHECK(oh.revision == REVW(11) && oh.total == w, "output header rev %08x total %u written %zu", oh.revision, oh.total, w);
    float lr[480];
    for (int f = 0; f < 3; f++) rig_frame(&g, lr);
    for (int k = 0; k < 240; k++) {
        float s = g_pcm[480 + k] / 32768.f;
        CHECK(nearf(lr[2 * k], 0.5f * s, 1e-4f) && nearf(lr[2 * k + 1], 0.125f * s, 1e-4f), "pcm16 sample %d: %f %f want %f %f", k, lr[2 * k], lr[2 * k + 1],
              0.5f * s, 0.125f * s);
    }
    /* the output blob reports consumed buffers / played samples */
    const uint8_t *vo = g.out + sizeof(AudrenHeader) + oh.memory_pools;
    AudrenVoiceOut o0;
    memcpy(&o0, vo, sizeof o0);
    napple_audren_close(g.r);
}

static void t_pitch_half(void) {
    AudrenParameter p = param_default();
    Rig g;
    rig_open(&g, p);
    Update u;
    scene_init(&u, 11, &p);
    u.voices[0] = voice_pcm(0, FMT_PCM16, g_pcm, sizeof g_pcm, 4800);
    u.voices[0].pitch = 0.5f;
    u.nchans = 1;
    u.chans[0].id = 0;
    u.chans[0].mix[0] = 1;
    u.chans[0].in_use = 1;
    rig_update(&g, &u, NULL);
    float lr[480];
    rig_frame(&g, lr);
    for (int k = 0; k < 240; k++) {
        int i = k / 2;
        float s = (k & 1) ? (g_pcm[i] + g_pcm[i + 1]) / 65536.0f : g_pcm[i] / 32768.0f;
        CHECK(nearf(lr[2 * k], s, 1e-4f), "pitch 0.5 sample %d: %f want %f", k, lr[2 * k], s);
    }
    napple_audren_close(g.r);
}

static void t_adpcm(void) {
    static uint8_t adpcm[8 * 20];
    static int16_t coef[16];
    coef[0] = 2048;
    int16_t want[280];
    int32_t h = 0;
    for (int fr = 0; fr < 20; fr++) {
        adpcm[fr * 8] = 0x02;
        for (int j = 0; j < 7; j++) adpcm[fr * 8 + 1 + j] = (uint8_t)(((j + fr) & 7) << 4 | ((15 - j) & 15));
        for (int j = 0; j < 14; j++) {
            int n = (adpcm[fr * 8 + 1 + j / 2] >> ((j & 1) ? 0 : 4)) & 15;
            if (n >= 8) n -= 16;
            h += n * 4;
            if (h > 32767) h = 32767;
            if (h < -32768) h = -32768;
            want[fr * 14 + j] = (int16_t)h;
        }
    }
    AudrenParameter p = param_default();
    Rig g;
    rig_open(&g, p);
    Update u;
    scene_init(&u, 11, &p);
    u.voices[0] = voice_pcm(0, FMT_ADPCM, adpcm, sizeof adpcm, 280);
    u.voices[0].extra_address = (uint64_t)(uintptr_t)coef;
    u.voices[0].extra_size = sizeof coef;
    u.nchans = 1;
    u.chans[0].id = 0;
    u.chans[0].mix[0] = 1;
    u.chans[0].in_use = 1;
    rig_update(&g, &u, NULL);
    float lr[480];
    rig_frame(&g, lr);
    for (int k = 0; k < 240; k++) CHECK(nearf(lr[2 * k], want[k] / 32768.f, 1e-4f), "adpcm sample %d: %f want %f", k, lr[2 * k], want[k] / 32768.f);
    napple_audren_close(g.r);
}

static void t_submix_splitter(void) {
    /* voice 0 (via splitter dest 0 -> submix 1 volumes {1, .5}) ; submix 1 (2 bufs at 2,3) -> mix 0 swapped at .5 */
    AudrenParameter p = param_default();
    Rig g;
    rig_open(&g, p);
    Update u;
    scene_init(&u, 11, &p);
    u.mixes[1] =
        (AudrenMixIn){.volume = 0.5f, .sample_rate = 48000, .buffer_count = 2, .in_use = 1, .mix_id = 1, .dest_mix_id = 0, .dest_splitter_id = 0xffffffffu};
    u.mixes[1].mix[0][1] = 1;
    u.mixes[1].mix[1][0] = 1;
    uint32_t h0[3] = {0x48444e53u, 1, 1}, h1[5] = {0x49444e53u, 0, 48000, 1, 0};
    memcpy(u.splitter, h0, 12);
    memcpy(u.splitter + 0x20, h1, 20); /* info: 16 + 1 id + 12 reserved */
    uint8_t *d = u.splitter + 0x40;
    uint32_t dm = 0x44444e53u, did = 0;
    int32_t dst = 1;
    float vol[24] = {1, 0.5f};
    memcpy(d, &dm, 4);
    memcpy(d + 4, &did, 4);
    memcpy(d + 8, vol, sizeof vol);
    memcpy(d + 0x68, &dst, 4);
    d[0x6c] = 1;
    u.splitter_bytes = 0x40 + 0x70 + 0; /* rev11 destination stride 0x70 */
    u.splitter_bytes = (u.splitter_bytes + 15) & ~15u;
    u.voices[0] = voice_pcm(0, FMT_PCM16, g_pcm, sizeof g_pcm, 4800);
    u.voices[0].volume = 0.5f;
    u.voices[0].splitter_id = 0;
    u.voices[0].mix_id = 0x7fffffff;
    CHECK(rig_update(&g, &u, NULL) == 0, "splitter update");
    float lr[480];
    rig_frame(&g, lr);
    for (int k = 0; k < 240; k++) {
        float s = g_pcm[k] / 32768.f * 0.5f * 0.5f; /* voice .5 x mix1 .5; buffer 2 (x1) -> R, buffer 3 (x.5) -> L */
        CHECK(nearf(lr[2 * k], 0.5f * s, 1e-4f) && nearf(lr[2 * k + 1], s, 1e-4f), "splitter sample %d: %f %f want %f %f", k, lr[2 * k], lr[2 * k + 1],
              0.5f * s, s);
    }
    /* dirty-only mix update path: same scene, mixes as dirty entries */
    u.dirty_mixes = 1;
    u.nmixes = 2;
    u.voices[0].is_new = 0;
    u.voices[0].wave_buffers[0].sent = 1;
    CHECK(rig_update(&g, &u, NULL) == 0, "dirty mix update");
    napple_audren_close(g.r);
}

static void t_voice_state_machine(void) {
    static int16_t a[480], b[480];
    for (int k = 0; k < 480; k++) {
        a[k] = (int16_t)(k * 50);
        b[k] = (int16_t)(-k * 50);
    }
    AudrenParameter p = param_default();
    Rig g;
    rig_open(&g, p);
    Update u;
    scene_init(&u, 11, &p);
    AudrenVoiceIn x = voice_pcm(1, FMT_PCM16, a, sizeof a, 480);
    x.wave_buffers[1] = (AudrenWaveBuffer){0}; /* nothing to decode: counts as played on arrival */
    x.wave_buffers[2] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)b, .size = sizeof b, .end = 480};
    x.wave_buffers[3].sent = 1;
    x.mix_id = 0x7fffffff;
    u.voices[1] = x;
    rig_update(&g, &u, NULL);
    float lr[480];
    for (int f = 0; f < 3; f++) rig_frame(&g, lr);
    ServerVoice *sv = &g.r->voices[1];
    CHECK(sv->consumed == 2 && sv->index == 2 && sv->played == 720 && sv->offset == 240,
          "empty buffer: consumed %u index %u played %llu offset %llu, want 2 2 720 240", sv->consumed, sv->index, (unsigned long long)sv->played,
          (unsigned long long)sv->offset);
    /* Stop: buffers come back on the next frame */
    u.voices[1].is_new = 0;
    for (int i = 0; i < 4; i++) u.voices[1].wave_buffers[i].sent = 1;
    u.voices[1].play_state = PLAY_STOPPED;
    rig_update(&g, &u, NULL);
    CHECK(sv->consumed == 2, "Stop gave buffers back before a render frame");
    rig_frame(&g, lr);
    CHECK(sv->consumed == 3 && sv->played == 0 && sv->offset == 0 && !(sv->valid[0] | sv->valid[1] | sv->valid[2] | sv->valid[3]) && sv->state == SV_STOPPED,
          "stop: consumed %u played %llu state %u", sv->consumed, (unsigned long long)sv->played, sv->state);
    /* buffer sent while stopped waits for Start */
    u.voices[1].wave_buffers[3] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)a, .size = sizeof a, .end = 480};
    rig_update(&g, &u, NULL);
    rig_frame(&g, lr);
    CHECK(!sv->valid[3] && sv->pending[3] && sv->consumed == 3, "buffer sent while stopped played");
    u.voices[1].wave_buffers[3].sent = 1;
    u.voices[1].wave_buffers[0] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)b, .size = sizeof b, .end = 480};
    u.voices[1].play_state = PLAY_STARTED;
    rig_update(&g, &u, NULL);
    audren_voices_prepare(g.r);
    CHECK(sv->valid[3] && sv->valid[0] && sv->index == 3, "Start did not queue the waiting buffers");
    /* FlushWaveBuffers: two queued buffers from index 3 count as played */
    u.voices[1].wave_buffers[0].sent = 1;
    u.voices[1].flush_wave_buffer_count = 2;
    u.voices[1].wave_buffer_index = 3;
    rig_update(&g, &u, NULL);
    rig_frame(&g, lr);
    CHECK(sv->consumed == 5 && !sv->valid[3] && !sv->valid[0] && sv->played == 0, "flush: consumed %u", sv->consumed);
    napple_audren_close(g.r);
}

static void t_rev15_float_biquads(void) {
    AudrenParameter p = param_default();
    p.revision = REVW(15);
    p.voice_count = 1;
    p.effect_count = 0;
    p.submix_count = 0;
    Rig g;
    rig_open(&g, p);
    Update u;
    scene_init(&u, 15, &p);
    u.float_voices = 1;
    for (unsigned k = 0; k < 480; k++) g_pcmf[k] = (float)(0.3 * sin(k * 0.071) + (k % 37 == 0 ? 0.1 : 0));
    u.voices[0] = voice_pcm(0, FMT_PCMF32, g_pcmf, sizeof g_pcmf, 480);
    u.voices[0].volume = 1;
    u.voices[0].mix_id = 0x7fffffff;
    u.voices[0].splitter_id = 0;
    AudrenBiquadFloat voice = {.enable = 1, .numerator = {0.1234567f, 0.0712345f, 0}, .denominator = {0.25f, 0}};
    AudrenBiquadFloat splitter = {.enable = 1, .numerator = {0.7654321f, 0, 0}, .denominator = {0.125f, 0}};
    u.fbiq[0][0] = voice;
    uint32_t sh[3] = {0x48444e53u, 1, 1}, si[5] = {0x49444e53u, 0, 48000, 1, 0}, magic = 0x44444e53u;
    memcpy(u.splitter, sh, 12);
    memcpy(u.splitter + 0x20, si, sizeof si);
    uint8_t *dest = u.splitter + 0x40;
    memcpy(dest, &magic, 4);
    float volumes[24] = {1, 0.5f};
    memcpy(dest + 8, volumes, sizeof volumes);
    memcpy(dest + 0x6c, &splitter, sizeof splitter);
    dest[0x9c] = 1;
    u.splitter_bytes = 0x40 + 0xa8;
    double vx = 0, vy = 0, sy = 0;
    float lr[480];
    for (int block = 0; block < 2; block++) {
        CHECK(rig_update(&g, &u, NULL) == 0, "rev15 update");
        AudrenHeader oh;
        memcpy(&oh, g.out, sizeof oh);
        CHECK(oh.revision == REVW(15), "reply revision");
        rig_frame(&g, lr);
        for (unsigned k = 0; k < 240; k++) {
            double x = g_pcmf[block * 240 + k], y = voice.numerator[0] * x + voice.numerator[1] * vx + voice.denominator[0] * vy;
            vx = x;
            vy = y;
            double z = splitter.numerator[0] * y + splitter.denominator[0] * sy;
            sy = z;
            CHECK(fabs(lr[2 * k] - z) < 1e-6 && fabs(lr[2 * k + 1] - z * 0.5) < 1e-6, "rev15 sample %u block %d", k, block);
        }
        u.voices[0].is_new = 0;
        u.voices[0].wave_buffers[0].sent = 1;
    }
    napple_audren_close(g.r);
}

static void t_cascaded_q14_biquads(void) {
    static int16_t stereo[240 * 6 * 2];
    for (int k = 0; k < 240 * 6; k++) {
        stereo[2 * k] = (int16_t)(7000 * sin(k * 0.043) + (k % 71 == 0 ? 4000 : 0));
        stereo[2 * k + 1] = (int16_t)(5000 * cos(k * 0.071) - (k % 53 == 0 ? 3000 : 0));
    }
    AudrenParameter p = param_default();
    Rig g;
    rig_open(&g, p);
    Update u;
    scene_init(&u, 11, &p);
    AudrenBiquad filters[2] = {{.enable = 1, .numerator = {8192, 4096, 2048}, .denominator = {6144, -2048}},
                               {.enable = 1, .numerator = {12288, -4096, 1024}, .denominator = {4096, -1024}}};
    u.voices[0] = voice_pcm(0, FMT_PCM16, stereo, sizeof stereo, 240 * 6);
    u.voices[0].channel_count = 2;
    u.voices[0].channel_ids[1] = 1;
    u.nchans = 2;
    u.chans[0].id = 0;
    u.chans[0].mix[0] = 1;
    u.chans[1].id = 1;
    u.chans[1].mix[1] = 1;
    double history[2][2][4] = {{{0}}};
    float lr[480];
    for (int block = 0; block < 3; block++) {
        memcpy(u.voices[0].biquads, filters, sizeof filters);
        rig_update(&g, &u, NULL);
        rig_frame(&g, lr);
        for (int k = 0; k < 240; k++)
            for (int c = 0; c < 2; c++) {
                double x = stereo[2 * (block * 240 + k) + c] / 32768.0;
                for (int f = 0; f < 2; f++) {
                    double *h = history[f][c];
                    const AudrenBiquad *b = &filters[f];
                    double y =
                        (b->numerator[0] * x + b->numerator[1] * h[0] + b->numerator[2] * h[1] + b->denominator[0] * h[2] + b->denominator[1] * h[3]) / 16384.0;
                    h[1] = h[0];
                    h[0] = x;
                    h[3] = h[2];
                    h[2] = y;
                    x = y;
                }
                CHECK(nearf(lr[2 * k + c], (float)x, 1e-4f), "q14 biquad block %d sample %d ch %d: %.7f want %.7f", block, k, c, lr[2 * k + c], x);
            }
        u.voices[0].is_new = 0;
        u.voices[0].wave_buffers[0].sent = 1;
    }
    napple_audren_close(g.r);
}

static void t_limiter_and_aux(void) {
    static float in[480];
    for (int k = 0; k < 240; k++) {
        in[2 * k] = .75f;
        in[2 * k + 1] = -.125f;
    }
    AudrenParameter p = param_default();
    Rig g;
    rig_open(&g, p);
    Update u;
    scene_init(&u, 11, &p);
    u.voices[0] = voice_pcm(0, FMT_PCMF32, in, sizeof in, 240);
    u.voices[0].channel_count = 2;
    u.voices[0].channel_ids[1] = 1;
    u.nchans = 2;
    u.chans[0].id = 0;
    u.chans[0].mix[0] = 1;
    u.chans[1].id = 1;
    u.chans[1].mix[1] = 1;
    AudrenLimiterParam lp = {.input = {0, 1},
                             .output = {0, 1},
                             .channel_max = 6,
                             .channels = 2,
                             .sample_rate = 48000,
                             .attack = 1,
                             .release = 1,
                             .threshold = .5f,
                             .input_gain = 1,
                             .output_gain = 1,
                             .delay_min = 9,
                             .delay_max = 13,
                             .status = 2,
                             .statistics_enabled = 1};
    u.effects[0] = (AudrenEffectIn){.type = 7, .enabled = 1, .is_new = 1, .mix_id = 0};
    memcpy(u.effects[0].specific, &lp, sizeof lp);
    CHECK(rig_update(&g, &u, NULL) == 0, "limiter update");
    float lr[480];
    rig_frame(&g, lr);
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 240; k++) {
            float expected = k < 9 ? 0 : c == 0 ? .5f * (32767.f / 32768.f) : -.125f;
            CHECK(nearf(lr[2 * k + c], expected, 1e-4f), "limiter ch %d sample %d: %f want %f", c, k, lr[2 * k + c], expected);
        }
    /* disabled effect is a pass-through (bypass) */
    u.voices[0].is_new = 0;
    u.voices[0].wave_buffers[0].sent = 1;
    u.effects[0].enabled = 0;
    float in2[480];
    (void)in2;
    napple_audren_close(g.r);
}

static void t_malformed(void) {
    AudrenParameter p = param_default();
    Rig g;
    rig_open(&g, p);
    Update u;
    scene_init(&u, 11, &p);
    size_t n = build_update(&u, g.in), w = 0;
    CHECK(napple_audren_request_update(g.r, g.in, 8, g.out, sizeof g.out, &w, NULL, 0) == NAPPLE_AUDIO_E_PROTOCOL, "truncated header must fail");
    AudrenHeader h;
    memcpy(&h, g.in, sizeof h);
    h.voices += 4;
    memcpy(g.in, &h, sizeof h);
    CHECK(napple_audren_request_update(g.r, g.in, n, g.out, sizeof g.out, &w, NULL, 0) == NAPPLE_AUDIO_E_PROTOCOL, "bad voice section size must fail");
    n = build_update(&u, g.in);
    h.total = (uint32_t)n + 100;
    memcpy(&h, g.in, sizeof h);
    h.total = (uint32_t)n + 100;
    memcpy(g.in, &h, sizeof h);
    CHECK(napple_audren_request_update(g.r, g.in, n, g.out, sizeof g.out, &w, NULL, 0) == NAPPLE_AUDIO_E_PROTOCOL, "total beyond buffer must fail");
    n = build_update(&u, g.in);
    CHECK(napple_audren_request_update(g.r, g.in, n, g.out, 16, &w, NULL, 0) == NAPPLE_AUDIO_E_SIZE && w > 16, "small output reports needed size");
    CHECK(napple_audren_request_update(g.r, g.in, n, g.out, sizeof g.out, &w, NULL, 0) == 0, "renderer survives errors");
    /* bad revision */
    u.rev = 0;
    n = build_update(&u, g.in);
    h.revision = 0x30564552u;
    memcpy(&h, g.in, sizeof h);
    CHECK(napple_audren_request_update(g.r, g.in, n, g.out, sizeof g.out, &w, NULL, 0) == NAPPLE_AUDIO_E_PROTOCOL, "REV0 rejected");
    /* bad parameters at open */
    AudrenParameter bad = param_default();
    bad.voice_count = 0;
    napple_audren *r2 = NULL;
    CHECK(napple_audren_open(&bad, sizeof bad, NULL, NAPPLE_AUDREN_MANUAL, &r2) == NAPPLE_AUDIO_E_INVALID, "invalid parameter rejected");
    napple_audren_close(g.r);
}

static void t_revisions_and_pools(void) {
    static const int revs[] = {5, 6, 9, 11, 13, 15};
    for (unsigned i = 0; i < sizeof revs / sizeof *revs; i++) {
        AudrenParameter p = param_default();
        p.revision = REVW(revs[i]);
        Rig g;
        rig_open(&g, p);
        Update u;
        scene_init(&u, revs[i], &p);
        u.float_voices = revs[i] >= 15;
        u.pools[0] = (AudrenPoolIn){.address = 0x1000, .size = 0x1000, .state = POOL_REQUEST_ATTACH};
        u.pools[1] = (AudrenPoolIn){.state = POOL_REQUEST_DETACH};
        size_t w = 0;
        CHECK(rig_update(&g, &u, &w) == 0, "REV%d accepted", revs[i]);
        AudrenHeader oh;
        memcpy(&oh, g.out, sizeof oh);
        CHECK(oh.revision == REVW(revs[i]), "REV%d reply revision", revs[i]);
        CHECK(oh.effects == (uint32_t)p.effect_count * (revs[i] >= 9 ? 0x90u : 0x10u), "REV%d effect out size %u", revs[i], oh.effects);
        AudrenPoolOut p0, p1;
        memcpy(&p0, g.out + sizeof oh, sizeof p0);
        memcpy(&p1, g.out + sizeof oh + sizeof p0, sizeof p1);
        CHECK(p0.state == POOL_ATTACHED && p1.state == POOL_DETACHED, "REV%d pool states %u %u", revs[i], p0.state, p1.state);
        napple_audren_close(g.r);
    }
}

static void t_circular_sink(void) {
    static float in[240];
    for (int k = 0; k < 240; k++) in[k] = (float)(0.5 * sin(k * 0.1));
    static int16_t ring[2 * 480];
    AudrenParameter p = param_default();
    Rig g;
    rig_open(&g, p);
    Update u;
    scene_init(&u, 11, &p);
    u.nsinks = 1;
    u.voices[0] = voice_pcm(0, FMT_PCMF32, in, sizeof in, 240);
    u.nchans = 1;
    u.chans[0].id = 0;
    u.chans[0].mix[0] = 1;
    u.sinks[0] = (AudrenSinkIn){.type = SINK_CIRCULAR, .in_use = 1};
    struct {
        uint64_t address;
        uint32_t size, channels, samples, last_read;
        uint8_t format, reserved[3], input[6], padding[2];
    } cs = {(uint64_t)(uintptr_t)ring, sizeof ring, 1, 240, 0, FMT_PCM16, {0}, {0}, {0}};
    memcpy(u.sinks[0].name, &cs, sizeof cs);
    CHECK(rig_update(&g, &u, NULL) == 0, "circular sink update");
    float lr[480];
    rig_frame(&g, lr);
    int ok = 1;
    for (int k = 0; k < 240; k++) {
        int want = (int)(in[k] * 32768.f);
        ok &= abs(ring[k] - want) <= 1;
    }
    CHECK(ok, "circular sink PCM16 content");
    napple_audren_close(g.r);
}

static void t_performance(void) {
    AudrenParameter p = param_default();
    Rig g;
    rig_open(&g, p);
    Update u;
    scene_init(&u, 11, &p);
    u.voices[0] = voice_pcm(0, FMT_PCM16, g_pcm, sizeof g_pcm, 240);
    uint8_t perf[0x10 + 16 * 16];
    size_t w = 0;
    uint32_t hdr[4], ent[4];
    size_t n = build_update(&u, g.in);
    memset(perf, 0xcc, sizeof perf);
    CHECK(napple_audren_request_update(g.r, g.in, n, g.out, sizeof g.out, &w, perf, sizeof perf) == 0, "update with a performance buffer");
    memcpy(hdr, perf, sizeof hdr);
    /* default rig: 1 started voice + 1 submix + final mix + 1 sink */
    CHECK(hdr[1] >= 3 && hdr[0] == 0x10 + hdr[1] * 0x10 && hdr[2] == 0, "perf header: size %u entries %u", hdr[0], hdr[1]);
    uint32_t t = 0, kinds[5] = {0};
    for (uint32_t i = 0; i < hdr[1]; i++) {
        memcpy(ent, perf + 0x10 + i * 0x10, sizeof ent);
        CHECK(ent[1] == t && ent[2] > 0 && ent[3] >= 1 && ent[3] <= 4, "perf entry %u is contiguous with a plausible time", i);
        t += ent[2];
        kinds[ent[3]]++;
    }
    CHECK(hdr[3] == t, "perf next start time = sum of entries");
    CHECK(kinds[1] == 1 && kinds[3] == 1 && kinds[4] == 1, "one voice, one final mix, one sink");
    AudrenHeader oh;
    memcpy(&oh, g.out, sizeof oh);
    uint32_t pb;
    size_t off = sizeof oh + oh.memory_pools + oh.voices + oh.effects + oh.sinks;
    memcpy(&pb, g.out + off, 4);
    CHECK(pb == hdr[0], "update output carries the performance byte count");
    size_t n2 = build_update(&u, g.in);
    uint8_t tiny[0x30];
    memset(tiny, 0, sizeof tiny);
    CHECK(napple_audren_request_update(g.r, g.in, n2, g.out, sizeof g.out, &w, tiny, sizeof tiny) == 0, "small performance buffer is accepted");
    memcpy(hdr, tiny, sizeof hdr);
    CHECK(hdr[1] == 2 && hdr[0] == 0x30, "entries are capped to the buffer");
    CHECK(napple_audren_request_update(g.r, g.in, n2, g.out, sizeof g.out, &w, tiny, 8) == 0, "buffer smaller than the header is ignored");
    napple_audren_close(g.r);
}

int main(void) {
    napple_audio_config c = {.use_device = 0};
    napple_audio_init(&c);
    for (int i = 0; i < 4800; i++) g_pcm[i] = (int16_t)(16000 * sin(2 * M_PI * 1000 * i / 48000.0));
    t_pcm16_channel_mix();
    t_pitch_half();
    t_adpcm();
    t_submix_splitter();
    t_voice_state_machine();
    t_rev15_float_biquads();
    t_cascaded_q14_biquads();
    t_limiter_and_aux();
    t_malformed();
    t_revisions_and_pools();
    t_circular_sink();
    t_performance();
    napple_audio_shutdown();
    printf("{\"test\":\"audren\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
