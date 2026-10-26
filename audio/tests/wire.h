/* Test helper: builds audren RequestUpdate blobs from structures (white-box: uses the internal wire types). */
#ifndef NAPPLE_AUDIO_TEST_WIRE_H
#define NAPPLE_AUDIO_TEST_WIRE_H
#include "internal.h"
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REVW(n) (0x30564552u + ((uint32_t)(n) << 24))
static int g_checked, g_failed;
#define CHECK(cond, ...)                                                                                                                                       \
    do {                                                                                                                                                       \
        g_checked++;                                                                                                                                           \
        if (!(cond)) {                                                                                                                                         \
            g_failed++;                                                                                                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                                                               \
            fprintf(stderr, __VA_ARGS__);                                                                                                                      \
            fprintf(stderr, "\n");                                                                                                                             \
        }                                                                                                                                                      \
    } while (0)
static int nearf(float a, float b, float eps) { return fabsf(a - b) <= eps; }

typedef struct {
    int rev;    /* 1..15 */
    int npools; /* pool entries (default effect_count + 4*voice_count) */
    AudrenPoolIn pools[64];
    AudrenChannelIn chans[16];
    int nchans;
    AudrenVoiceIn voices[8];
    AudrenBiquadFloat fbiq[8][2];
    int nvoices;
    int float_voices; /* nvoices must equal voice_count */
    AudrenEffectIn effects[8];
    int neffects;
    uint8_t splitter[512];
    uint32_t splitter_bytes;
    AudrenMixIn mixes[4];
    int nmixes;
    int dirty_mixes;
    AudrenSinkIn sinks[4];
    int nsinks;
    uint8_t behavior; /* 1 = include 0x10 behavior section */
} Update;

static AudrenParameter param_default(void) {
    AudrenParameter p = {.sample_rate = 48000,
                         .sample_count = 240,
                         .mix_buffer_count = 8,
                         .submix_count = 1,
                         .voice_count = 2,
                         .sink_count = 1,
                         .effect_count = 2,
                         .splitter_count = 1,
                         .splitter_destination_count = 2,
                         .revision = REVW(11)};
    return p;
}

static size_t build_update(const Update *u, uint8_t *out) {
    const int vc = u->nvoices;
    AudrenHeader h = {.revision = REVW(u->rev),
                      .behavior = u->behavior ? 0x10 : 0,
                      .memory_pools = (uint32_t)u->npools * sizeof(AudrenPoolIn),
                      .voice_resources = (uint32_t)u->nchans * sizeof(AudrenChannelIn),
                      .voices = (uint32_t)vc * (u->float_voices ? sizeof(AudrenVoiceInV2) : sizeof(AudrenVoiceIn)),
                      .effects = (uint32_t)u->neffects * sizeof(AudrenEffectIn),
                      .splitters = u->splitter_bytes,
                      .sinks = (uint32_t)u->nsinks * sizeof(AudrenSinkIn),
                      .performance = 0x10,
                      .render_info = 0x10};
    uint32_t mixbytes = u->dirty_mixes ? 0x20 + (uint32_t)u->nmixes * sizeof(AudrenMixIn) : (uint32_t)u->nmixes * sizeof(AudrenMixIn);
    h.mixes = mixbytes;
    uint8_t *w = out + sizeof h;
    if (u->behavior) {
        memset(w, 0, 0x10);
        w += 0x10;
    }
    memcpy(w, u->pools, h.memory_pools);
    w += h.memory_pools;
    memcpy(w, u->chans, h.voice_resources);
    w += h.voice_resources;
    for (int i = 0; i < vc; i++) {
        if (u->float_voices) {
            AudrenVoiceInV2 v2;
            memset(&v2, 0, sizeof v2);
            memcpy(v2.prefix, &u->voices[i], sizeof v2.prefix);
            memcpy(v2.tail, (const uint8_t *)&u->voices[i] + 0x3c, sizeof v2.tail);
            v2.biquads[0] = u->fbiq[i][0];
            v2.biquads[1] = u->fbiq[i][1];
            memcpy(w, &v2, sizeof v2);
            w += sizeof v2;
        } else {
            memcpy(w, &u->voices[i], sizeof(AudrenVoiceIn));
            w += sizeof(AudrenVoiceIn);
        }
    }
    memcpy(w, u->effects, h.effects);
    w += h.effects;
    memcpy(w, u->splitter, h.splitters);
    w += h.splitters;
    if (u->dirty_mixes) {
        uint32_t magic = 0xcafebabeu, n = (uint32_t)u->nmixes;
        memset(w, 0, 0x20);
        memcpy(w, &magic, 4);
        memcpy(w + 4, &n, 4);
        memcpy(w + 0x20, u->mixes, (size_t)n * sizeof(AudrenMixIn));
    } else memcpy(w, u->mixes, mixbytes);
    w += mixbytes;
    memcpy(w, u->sinks, h.sinks);
    w += h.sinks;
    memset(w, 0, h.performance + h.render_info);
    w += h.performance + h.render_info;
    h.total = (uint32_t)(w - out);
    memcpy(out, &h, sizeof h);
    return h.total;
}

static AudrenVoiceIn voice_pcm(uint32_t id, int fmt, const void *data, size_t bytes, int samples) {
    AudrenVoiceIn v = {.id = id,
                       .node_id = id,
                       .is_new = 1,
                       .in_use = 1,
                       .play_state = PLAY_STARTED,
                       .sample_format = (uint8_t)fmt,
                       .sample_rate = 48000,
                       .channel_count = 1,
                       .pitch = 1,
                       .volume = 1,
                       .mix_id = 0,
                       .splitter_id = 0xffffffffu,
                       .channel_ids = {(int32_t)id}};
    v.wave_buffers[0] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)data, .size = bytes, .start = 0, .end = samples};
    for (int i = 1; i < 4; i++) v.wave_buffers[i].sent = 1;
    return v;
}

/* A standard scene: mix 0 (2 buffers) -> device sink (buffers 0,1). */
static void scene_init(Update *u, int rev, const AudrenParameter *p) {
    memset(u, 0, sizeof *u);
    u->rev = rev;
    u->behavior = 1;
    u->npools = p->effect_count + 4 * p->voice_count;
    u->nvoices = p->voice_count;
    for (int i = 0; i < p->voice_count; i++)
        u->voices[i] = (AudrenVoiceIn){.id = (uint32_t)i, .splitter_id = 0xffffffffu, .mix_id = 0, .pitch = 1, .volume = 1, .sample_rate = 48000};
    u->neffects = p->effect_count;
    u->nmixes = p->submix_count + 1;
    u->mixes[0] = (AudrenMixIn){
        .volume = 1, .sample_rate = 48000, .buffer_count = 2, .in_use = 1, .mix_id = 0, .dest_mix_id = 0x7fffffff, .dest_splitter_id = 0xffffffffu};
    for (int i = 1; i < u->nmixes; i++) u->mixes[i] = (AudrenMixIn){.mix_id = i, .dest_mix_id = 0x7fffffff, .dest_splitter_id = 0xffffffffu};
    u->nsinks = p->sink_count;
    u->sinks[0] = (AudrenSinkIn){.type = SINK_DEVICE, .in_use = 1, .input_count = 2, .inputs = {0, 1}};
    u->nchans = 0;
}

typedef struct {
    napple_audren *r;
    AudrenParameter p;
    uint8_t in[16384], out[16384];
} Rig;
static int rig_open(Rig *g, AudrenParameter p) {
    g->p = p;
    int rc = napple_audren_open(&p, sizeof p, NULL, NAPPLE_AUDREN_MANUAL, &g->r);
    if (rc) return rc;
    return napple_audren_start(g->r);
}
static int rig_update(Rig *g, const Update *u, size_t *written) {
    size_t n = build_update(u, g->in), w = 0;
    int rc = napple_audren_request_update(g->r, g->in, n, g->out, sizeof g->out, &w, NULL, 0);
    if (written) *written = w;
    return rc;
}
/* Render one frame and read it. */
static void rig_frame(Rig *g, float *lr) {
    napple_audren_render_frame(g->r);
    napple_audren_read_output(g->r, lr, g->p.sample_count);
}
#endif
