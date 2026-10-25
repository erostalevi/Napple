/* The audio renderer core (audren:u / IAudioRenderer) independent of IPC.

   RequestUpdate parsing: the SDK's nn::audio client builds the update blob; this file validates every section
   against the header, keeps the server-side voice state, advances it in real time (one `sample_count` frame per
   render tick, then frame_event) and writes the output blob the client expects.  Mixing is in mix.c.
   The voice/wave-buffer state machine follows Ryujinx.Audio's VoiceInfo (MIT, see THIRD_PARTY_NOTICES.md). */
#include "internal.h"
#include <math.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REV(n) (0x30564552u + ((uint32_t)(n) << 24))   /* "REVn" */
#define BEHAVIOR_OUT 0xb0u
#define RENDER_INFO_OUT 0x10u
#define WORK_BUFFER_BYTES 0x10000u                      /* the host keeps its state in host memory */

static int revision_number(uint32_t rev) { return (int)(rev >> 24) - '0'; }

/* ---------- parameters ---------- */
static int check_param(const AudrenParameter *p) {
    if (p->sample_rate != 48000 && p->sample_rate != 32000) return 0;
    if (p->sample_count <= 0 || p->sample_count > 1024) return 0;
    if (p->mix_buffer_count <= 0 || p->mix_buffer_count > 512) return 0;
    if (p->voice_count <= 0 || p->voice_count > 1024) return 0;
    if (p->sink_count <= 0 || p->sink_count > 256) return 0;
    if (p->effect_count < 0 || p->effect_count > 4096) return 0;
    if (p->submix_count < 0 || p->submix_count >= 256) return 0;
    if (p->splitter_count < 0 || p->splitter_count > 1024 || p->splitter_destination_count < 0 || p->splitter_destination_count > 4096) return 0;
    return 1;
}

int napple_audren_work_buffer_size(const void *param, size_t n, uint64_t *out) {
    AudrenParameter p;
    if (!param || n < sizeof p || !out) return NAPPLE_AUDIO_E_INVALID;
    memcpy(&p, param, sizeof p);
    if (!check_param(&p)) return NAPPLE_AUDIO_E_INVALID;
    *out = WORK_BUFFER_BYTES;
    return NAPPLE_AUDIO_OK;
}

/* ---------- voices ---------- */
static void voice_reset(ServerVoice *v) {
    memset(v->valid, 0, sizeof v->valid); memset(v->pending, 0, sizeof v->pending);
    v->state = SV_STOPPED; v->dropped = 0; v->flush = 0; v->flush_from = 0;
    v->index = 0; v->offset = 0; v->fraction = 0; v->played = 0; v->consumed = 0; v->loops_done = 0;
    memset(&v->adpcm, 0, sizeof v->adpcm); v->adpcm.buffer = -1;
    memset(v->biquad_state, 0, sizeof v->biquad_state);
}

static int update_voice(AudioRenderer *r, const AudrenVoiceIn *in) {
    if (in->id >= (uint32_t)r->param.voice_count) return NAPPLE_AUDIO_E_PROTOCOL;
    ServerVoice *v = &r->voices[in->id];
    if (in->is_new || !in->in_use) voice_reset(v);
    for (int i = 0; i < 2; i++) if (!in->biquads[i].enable) memset(v->biquad_state[i], 0, sizeof v->biquad_state[i]);
    v->in = *in;
    v->use_float_biquads = 0;
    if (!in->in_use) return 0;
    if (in->wave_buffer_index >= 4) return NAPPLE_AUDIO_E_PROTOCOL;
    /* Stop on a voice that is not stopped becomes Stopping; the next render frame gives back every queued buffer. */
    if (in->play_state == PLAY_STARTED) v->state = SV_STARTED;
    else if (in->play_state == PLAY_PAUSED) v->state = SV_PAUSED;
    else if (v->state != SV_STOPPED) v->state = SV_STOPPING;
    for (unsigned i = 0; i < 4; i++) {   /* a new buffer waits until a render frame finds the voice playing */
        const AudrenWaveBuffer *w = &in->wave_buffers[i];
        if (!w->sent) { v->buffers[i] = *w; v->pending[i] = 1; }
    }
    v->flush += in->flush_wave_buffer_count; v->flush_from = in->wave_buffer_index;
    if (in->clear_voice_drop) v->drops = 0;
    return 0;
}

/* No samples (no memory, no size or an empty range): the DSP counts the buffer as played when it reaches it. */
int audren_buffer_empty(const AudrenWaveBuffer *b) { return !b->address || !b->size || b->end <= b->start; }

static void skip_empty(AudioRenderer *r, ServerVoice *v) {
    while (v->valid[v->index] && audren_buffer_empty(&v->buffers[v->index])) {
        const AudrenWaveBuffer *b = &v->buffers[v->index];
        if (b->loop && b->loop_count < 0) break;   /* an endless empty loop starves the voice, as on the DSP */
        v->valid[v->index] = 0; v->consumed++; v->offset = 0; v->loops_done = 0; v->index = (v->index + 1) & 3;
        if (r) r->empty_played++;
        if (b->end_of_stream) { v->played = 0; break; }
    }
}

/* Each render frame before mixing: flush, stop, start (VoiceInfo.UpdateParametersForCommandGeneration). */
void audren_voices_prepare(AudioRenderer *r) {
    for (int k = 0; k < r->param.voice_count; k++) {
        ServerVoice *v = &r->voices[k];
        if (!v->in.in_use) continue;
        if (v->flush) {   /* FlushWaveBuffers: from the client's current buffer, the queued ones count as played */
            unsigned i = v->flush_from & 3;
            for (uint32_t n = v->flush; n; n--, i = (i + 1) & 3) {
                v->pending[i] = 0;
                if (v->valid[i]) { v->valid[i] = 0; v->consumed++; v->index = (v->index + 1) & 3; v->offset = 0; v->loops_done = 0; r->flushed++; }
            }
            v->flush = 0;
        }
        if (v->state == SV_STOPPING) {
            for (int i = 0; i < 4; i++) {   /* every queued buffer counts as played; ones never sent are dropped */
                if (v->valid[i]) { v->consumed++; v->index = (v->index + 1) & 3; r->stop_released++; }
                v->valid[i] = 0; v->pending[i] = 0;
            }
            v->offset = 0; v->played = 0; v->fraction = 0; v->loops_done = 0;
            memset(&v->adpcm, 0, sizeof v->adpcm); v->adpcm.buffer = -1;
            v->state = SV_STOPPED;
            continue;
        }
        if (v->state == SV_STARTED) {
            for (int i = 0; i < 4; i++) if (v->pending[i]) {
                v->pending[i] = 0; v->valid[i] = 1;
                if ((unsigned)i == v->index) { v->offset = 0; v->loops_done = 0; }
            }
            skip_empty(r, v);
        }
    }
}

/* ---------- voice drop and render metrics ---------- */
static uint64_t default_clock(void *user) { (void)user; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint64_t now_ns(const AudioRenderer *r) { return r->clock ? r->clock(r->clock_user) : default_clock(NULL); }
static int drop_order(const AudioRenderer *r, int a, int b) {   /* > 0: a is dropped before b */
    const AudrenVoiceIn *x = &r->voices[a].in, *y = &r->voices[b].in;
    if (x->priority != y->priority) return x->priority > y->priority ? 1 : -1;
    if (x->sort_order != y->sort_order) return x->sort_order > y->sort_order ? 1 : -1;
    return a > b ? 1 : a < b ? -1 : 0;
}
static int voice_playing(const ServerVoice *v) { return v->in.in_use && v->state == SV_STARTED && v->in.sample_rate && v->valid[v->index]; }
/* Per frame, before mixing: mark the r->drop_target lowest-priority playing voices as dropped. */
static void select_drops(AudioRenderer *r) {
    int playing[1024], n = 0;
    for (int i = 0; i < r->param.voice_count; i++) { r->voices[i].dropped = 0; if (voice_playing(&r->voices[i]) && n < 1024) playing[n++] = i; }
    if (!r->param.voice_drop_enabled) { r->drop_target = 0; r->stats.voices_dropped_now = 0; return; }
    uint32_t drop = r->drop_target < (uint32_t)n ? r->drop_target : (uint32_t)n;
    for (uint32_t d = 0; d < drop; d++) {   /* selection by repeated maximum: drop counts are tiny */
        int best = -1;
        for (int k = 0; k < n; k++) if (!r->voices[playing[k]].dropped && (best < 0 || drop_order(r, playing[k], best) > 0)) best = playing[k];
        r->voices[best].dropped = 1; r->voices[best].drops++; r->stats.dropped_voice_frames++;
    }
    r->stats.voices_dropped_now = drop;
}
/* NAPPLE_AUDIO_STATS=1: the render health line, every 15 s of frames and at close. */
static void log_stats(AudioRenderer *r, const char *when) {
    const napple_audren_stats *s = &r->stats;
    napple_audio_log(&r->host, "renderer stats (%s): %llu frames, render avg %.0f us max %.0f us, over budget %llu, late %llu, late ticks %llu, resyncs %llu, "
                     "dropped voice-frames %llu (now %u), voice drop %s, time limit %u%%", when, (unsigned long long)s->frames,
                     s->frames ? (double)s->render_ns_total / (double)s->frames / 1e3 : 0.0, (double)s->render_ns_max / 1e3,
                     (unsigned long long)s->over_budget_frames, (unsigned long long)s->late_frames, (unsigned long long)s->late_ticks,
                     (unsigned long long)s->resyncs, (unsigned long long)s->dropped_voice_frames, s->voices_dropped_now,
                     r->param.voice_drop_enabled ? "on" : "off", r->time_limit ? r->time_limit : 100);
}
/* After a frame: account the render time and decide how many voices to drop in the next one. */
static void account_frame(AudioRenderer *r, uint64_t ns, double frame_seconds) {
    napple_audren_stats *s = &r->stats;
    const uint64_t period = (uint64_t)(frame_seconds * 1e9), limit = __atomic_load_n(&r->time_limit, __ATOMIC_RELAXED);
    const uint64_t budget = period * (limit ? (limit > 100 ? 100 : limit) : 100) / 100;
    s->frames++; s->render_ns_total += ns; s->render_ns_last = ns; if (ns > s->render_ns_max) s->render_ns_max = ns;
    if (ns > budget) s->over_budget_frames++;
    if (ns > period) s->late_frames++;
    if (!r->param.voice_drop_enabled) return;
    int playing = 0; for (int i = 0; i < r->param.voice_count; i++) playing += voice_playing(&r->voices[i]);
    if (ns > budget) {
        uint32_t more = (uint32_t)(((uint64_t)(playing ? playing : 1) * (ns - budget) + ns - 1) / ns);
        if (more < 1) more = 1;
        r->drop_target += more; if (r->drop_target > (uint32_t)playing) r->drop_target = (uint32_t)playing;   /* playing includes the voices dropped now */
    } else if (ns * 4 < budget * 3 && r->drop_target) r->drop_target--;
}

/* Moves the voice's clock by one frame (the mixer decoded exactly this sample path). */
void audren_advance_voice(AudioRenderer *r, ServerVoice *v, double seconds) {
    if (!v->in.in_use || v->state != SV_STARTED || !v->in.sample_rate) return;
    double want = v->fraction + seconds * v->in.sample_rate * (v->in.pitch > 0 ? v->in.pitch : 1.0);
    uint64_t samples = (uint64_t)want; v->fraction = want - (double)samples;
    while (samples && v->valid[v->index]) {
        AudrenWaveBuffer *b = &v->buffers[v->index];
        uint64_t length = (uint64_t)(b->end - b->start), left = length - v->offset;
        uint64_t step = samples < left ? samples : left;
        v->offset += step; v->played += step; samples -= step;
        if (v->offset < length) break;
        if (b->loop && (b->loop_count < 0 || v->loops_done < b->loop_count)) {
            v->loops_done++;
            v->offset = b->loop_start > b->start && b->loop_start < b->end ? (uint64_t)(b->loop_start - b->start) : 0;
            continue;
        }
        v->valid[v->index] = 0; v->consumed++; v->offset = 0; v->loops_done = 0; v->index = (v->index + 1) & 3;
        if (b->end_of_stream) { v->played = 0; break; }
        skip_empty(r, v);
    }
}

/* ---------- RequestUpdate ---------- */
typedef struct { const uint8_t *p, *end; } Reader;
static const uint8_t *take(Reader *in, uint32_t n, const char *what) {
    if ((size_t)(in->end - in->p) < n) {
        napple_audio_log(NULL, "input section %s (%u bytes) overruns the request", what, n);
        napple_audio_fatal("section overruns request");
    }
    const uint8_t *p = in->p; in->p += n; return p;
}
static void need(int cond, const char *msg) { if (!cond) napple_audio_fatal(msg); }
static uint32_t effect_out_bytes(const AudioRenderer *r) { return revision_number(r->revision) >= 9 ? 0x90u : 0x10u; }

/* Performance metrics. The renderer does no real timing, so the numbers are modelled: a fixed cost per started voice, per mix and per sink,
 * laid out back to back from start time 0 and capped at 1/4 of the 5 ms frame budget each. The 0x10 block in the update output carries the
 * byte count of the history written to the performance buffer; the buffer holds a 0x10 header {u32 bytes, u32 entries, u32 details, u32
 * next history start time} and 0x10-byte entries {s32 node id, u32 start time, u32 processing time, u32 type}. Layout is from the author's
 * memory of the public description and unverified; a title that never supplies a performance buffer sees only the byte count (0). */
enum { PERF_VOICE = 1, PERF_SUBMIX = 2, PERF_FINAL_MIX = 3, PERF_SINK = 4 };
enum { PERF_VOICE_US = 24, PERF_MIX_US = 40, PERF_SINK_US = 16, PERF_HEADER = 0x10, PERF_ENTRY = 0x10 };
static uint32_t write_performance(const AudioRenderer *r, uint8_t *perf, size_t perf_size) {
    if (!perf || perf_size < PERF_HEADER) return 0;
    const uint32_t max_entries = (uint32_t)((perf_size - PERF_HEADER) / PERF_ENTRY);
    uint32_t n = 0, t = 0;
    uint8_t *w = perf + PERF_HEADER;
#define PERF_ADD(node, us, kind) do { if (n < max_entries) { \
        uint32_t e[4] = {(uint32_t)(node), t, (us), (kind)}; memcpy(w, e, sizeof e); w += PERF_ENTRY; n++; t += (us); } } while (0)
    for (int i = 0; i < r->param.voice_count; i++) if (r->voices[i].state == SV_STARTED && !r->voices[i].dropped) PERF_ADD(i, PERF_VOICE_US, PERF_VOICE);
    for (int i = 0; i < r->param.submix_count; i++) PERF_ADD(i, PERF_MIX_US, PERF_SUBMIX);
    PERF_ADD(r->param.submix_count, PERF_MIX_US, PERF_FINAL_MIX);
    for (int i = 0; i < r->param.sink_count; i++) PERF_ADD(i, PERF_SINK_US, PERF_SINK);
#undef PERF_ADD
    uint32_t bytes = PERF_HEADER + n * PERF_ENTRY, hdr[4] = {bytes, n, 0, t};
    memcpy(perf, hdr, sizeof hdr);
    return bytes;
}

/* Body of RequestUpdate; runs under r->lock and the caller's trap. Returns bytes written. */
static size_t update_locked(AudioRenderer *r, const uint8_t *input, size_t in_size, uint8_t *output, size_t out_size,
                            int *status, uint8_t *perf, size_t perf_size) {
    need(in_size >= sizeof(AudrenHeader), "update input too small");
    AudrenHeader h; memcpy(&h, input, sizeof h);
    need(h.total <= in_size && h.total >= sizeof h, "update total size");
    need(revision_number(h.revision) >= 1 && revision_number(h.revision) <= 15 && (h.revision & 0xffffff) == 0x564552, "update revision");
    Reader in = {input + sizeof h, input + h.total};
    r->revision = h.revision;
    const int vc = r->param.voice_count, ec = r->param.effect_count, sc = r->param.sink_count, mc = r->param.submix_count + 1;

    need(h.behavior == 0 || h.behavior == 0x10, "behavior section size");
    const uint8_t *behavior = take(&in, h.behavior, "behavior");
    if (h.behavior == 0x10) memcpy(&r->behavior_flags, behavior + 8, 8);
    need(h.memory_pools % sizeof(AudrenPoolIn) == 0 && h.memory_pools / sizeof(AudrenPoolIn) <= 8192, "memory pool section size");
    const uint32_t npools = h.memory_pools / (uint32_t)sizeof(AudrenPoolIn);
    const uint8_t *pools = take(&in, h.memory_pools, "memory pools");   /* sections may be unaligned: memcpy only */
    need(h.voice_resources % sizeof(AudrenChannelIn) == 0 && h.voice_resources / sizeof(AudrenChannelIn) <= (uint32_t)vc * 6, "voice channel section size");
    const uint8_t *chans = take(&in, h.voice_resources, "voice channels");
    const int float_voices = h.voices == (uint32_t)vc * sizeof(AudrenVoiceInV2);
    need(float_voices || h.voices == (uint32_t)vc * sizeof(AudrenVoiceIn), "voice section size");
    const size_t voice_stride = float_voices ? sizeof(AudrenVoiceInV2) : sizeof(AudrenVoiceIn);
    const uint8_t *voices = take(&in, h.voices, "voices");
    need(h.effects == (uint32_t)ec * sizeof(AudrenEffectIn), "effect section size");
    const uint8_t *effects = take(&in, h.effects, "effects");
    const uint8_t *splitters = take(&in, h.splitters, "splitters");
    const uint8_t *mixes = take(&in, h.mixes, "mixes");
    need(h.sinks == (uint32_t)sc * sizeof(AudrenSinkIn), "sink section size");
    const uint8_t *sinks = take(&in, h.sinks, "sinks");
    take(&in, h.performance, "performance");
    take(&in, h.render_info, "render info");
    need(in.p == in.end, "unconsumed update input");

    for (uint32_t i = 0; i < h.voice_resources / sizeof(AudrenChannelIn); i++) {
        AudrenChannelIn ch; memcpy(&ch, chans + i * sizeof ch, sizeof ch);
        need(ch.id < (uint32_t)vc * 6, "channel resource id");
        r->channels[ch.id] = ch;
    }
    for (int i = 0; i < vc; i++) {
        AudrenVoiceIn v; AudrenVoiceInV2 v2;
        if (float_voices) {
            memcpy(&v2, voices + (size_t)i * voice_stride, sizeof v2); memset(&v, 0, sizeof v);
            memcpy(&v, v2.prefix, sizeof v2.prefix);
            memcpy((uint8_t *)&v + 0x3c, v2.tail, sizeof v2.tail);
            for (unsigned f = 0; f < 2; f++) {
                v.biquads[f].enable = v2.biquads[f].enable;
                if (v.biquads[f].enable) {
                    for (unsigned c = 0; c < 3; c++) need(isfinite(v2.biquads[f].numerator[c]), "nonfinite voice biquad numerator");
                    for (unsigned c = 0; c < 2; c++) need(isfinite(v2.biquads[f].denominator[c]), "nonfinite voice biquad denominator");
                }
            }
        } else memcpy(&v, voices + (size_t)i * voice_stride, sizeof v);
        need(update_voice(r, &v) == 0, "voice parameters");
        ServerVoice *server = &r->voices[v.id]; server->use_float_biquads = float_voices;
        if (float_voices) memcpy(server->float_biquads, v2.biquads, sizeof v2.biquads);
    }
    for (int i = 0; i < ec; i++) memcpy(&r->effects[i], effects + (size_t)i * sizeof r->effects[0], sizeof r->effects[0]);
    mixer_update_effects(r);
    mixer_update_routing(r, splitters, h.splitters, mixes, h.mixes, mc);
    for (int i = 0; i < sc; i++) memcpy(&r->sinks[i], sinks + (size_t)i * sizeof r->sinks[0], sizeof r->sinks[0]);

    /* ---- output ---- */
    const uint32_t eo = effect_out_bytes(r);
    AudrenHeader o = {0};
    o.revision = REV(revision_number(h.revision));
    o.memory_pools = npools * sizeof(AudrenPoolOut); o.voices = (uint32_t)vc * sizeof(AudrenVoiceOut);
    o.effects = (uint32_t)ec * eo; o.sinks = (uint32_t)sc * sizeof(AudrenSinkOut);
    o.performance = 0x10; o.behavior = BEHAVIOR_OUT; o.render_info = revision_number(h.revision) >= 5 ? RENDER_INFO_OUT : 0;   /* elapsed frame count: REV5+ */
    o.total = (uint32_t)sizeof o + o.memory_pools + o.voices + o.effects + o.sinks + o.performance + o.behavior + o.render_info;
    if (o.total > out_size) { *status = NAPPLE_AUDIO_E_SIZE; return o.total; }
    if (r->npools_alloc < npools) {
        AudrenPoolIn *np = realloc(r->pools, (size_t)npools * sizeof *np);
        need(np != NULL, "pool table allocation");
        r->pools = np; r->npools_alloc = npools;
    }
    memset(output, 0, out_size);
    uint8_t *w = output; memcpy(w, &o, sizeof o); w += sizeof o;
    for (uint32_t i = 0; i < npools; i++) {
        AudrenPoolIn pi; memcpy(&pi, pools + i * sizeof pi, sizeof pi);
        uint32_t s = pi.state;
        if (s == POOL_REQUEST_ATTACH) s = POOL_ATTACHED; else if (s == POOL_REQUEST_DETACH) s = POOL_DETACHED;
        r->pools[i] = pi; r->pools[i].state = s;
        AudrenPoolOut po = {s, {0}}; memcpy(w, &po, sizeof po); w += sizeof po;
    }
    for (int i = 0; i < vc; i++) {
        AudrenVoiceOut vo = {r->voices[i].played, r->voices[i].consumed, r->voices[i].drops};
        memcpy(w, &vo, sizeof vo); w += sizeof vo;
    }
    for (int i = 0; i < ec; i++) { w[0] = r->effects[i].enabled ? 3 : 4; if (eo > 16) mixer_effect_result(r, i, w + 16, eo - 16); w += eo; }
    for (int i = 0; i < sc; i++) {
        AudrenSinkOut so = {0};
        if (r->sinks[i].type == SINK_CIRCULAR) so.last_written_offset = mixer_sink_written(r, i);
        memcpy(w, &so, sizeof so); w += sizeof so;
    }
    { uint32_t pb = write_performance(r, perf, perf_size); memcpy(w, &pb, 4); }   /* performance: bytes of history in the perf buffer */
    w += 0x10;
    w += BEHAVIOR_OUT;                                    /* behavior: no errors */
    if (o.render_info) memcpy(w, &r->elapsed_frames, 8);
    *status = 0;
    return o.total;
}

int napple_audren_request_update(napple_audren *r, const void *in, size_t in_size, void *out, size_t out_size,
                                 size_t *out_written, void *perf, size_t perf_size) {
    if (!r || !in || !out) return NAPPLE_AUDIO_E_INVALID;
    jmp_buf jb; int status = NAPPLE_AUDIO_E_PROTOCOL; size_t written = 0;
    pthread_mutex_lock(&r->lock);
    if (NAPPLE_TRAP_BEGIN(jb) == 0) written = update_locked(r, in, in_size, out, out_size, &status, perf, perf_size);
    NAPPLE_TRAP_END();
    pthread_mutex_unlock(&r->lock);
    if (out_written) *out_written = written;
    return status;
}

/* ---------- render clock ---------- */
static void render_one(AudioRenderer *r, double frame) {
    if (r->state != 0 || r->faulted) return;
    jmp_buf jb;
    if (NAPPLE_TRAP_BEGIN(jb) == 0) {
        const uint64_t t0 = now_ns(r);
        audren_voices_prepare(r);
        select_drops(r);
        mixer_render(r);
        for (int i = 0; i < r->param.voice_count; i++) audren_advance_voice(r, &r->voices[i], frame);
        r->elapsed_frames++;
        account_frame(r, now_ns(r) - t0, frame);
        if (r->stats_every && r->stats.frames % r->stats_every == 0) log_stats(r, "periodic");
    } else {
        r->faulted = 1;
        napple_audio_log(&r->host, "renderer stopped after a protocol error in the render path (output muted)");
    }
    NAPPLE_TRAP_END();
}

void napple_audren_render_frame(napple_audren *r) {
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    render_one(r, (double)r->param.sample_count / r->param.sample_rate);
    pthread_mutex_unlock(&r->lock);
}

static void *render_thread(void *p) {
    AudioRenderer *r = p;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const double frame = (double)r->param.sample_count / r->param.sample_rate;
    const uint64_t period = (uint64_t)(frame * 1e9);
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    while (atomic_load_explicit(&r->running, memory_order_acquire)) {
        pthread_mutex_lock(&r->lock);
        render_one(r, frame);
        pthread_mutex_unlock(&r->lock);
        if (r->host.frame_event) r->host.frame_event(r->host.user);
        next.tv_nsec += (long)nae_scaled_ns((long long)period);
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t wait = (int64_t)(next.tv_sec - now.tv_sec) * 1000000000LL + (next.tv_nsec - now.tv_nsec);
        if (wait > 0) { struct timespec d = {wait / 1000000000LL, wait % 1000000000LL}; nanosleep(&d, NULL); }
        else {
            pthread_mutex_lock(&r->lock); r->stats.late_ticks++;
            if (wait < -100000000LL) { r->stats.resyncs++; next = now; }   /* fell far behind (debugger, sleep): resynchronise */
            pthread_mutex_unlock(&r->lock);
        }
    }
    return NULL;
}

/* ---------- lifecycle ---------- */
int napple_audren_open(const void *param, size_t n, const napple_audio_host *host, uint32_t flags, napple_audren **out) {
    AudrenParameter p;
    if (!param || n < sizeof p || !out) return NAPPLE_AUDIO_E_INVALID;
    memcpy(&p, param, sizeof p);
    if (!check_param(&p)) return NAPPLE_AUDIO_E_INVALID;
    AudioRenderer *r = calloc(1, sizeof *r);
    if (!r) return NAPPLE_AUDIO_E_NOMEM;
    r->param = p; r->revision = p.revision; if (host) r->host = *host;
    r->npools_alloc = (uint32_t)p.effect_count + 4u * (uint32_t)p.voice_count;
    r->voices = calloc((size_t)p.voice_count, sizeof *r->voices);
    r->pools = calloc(r->npools_alloc ? r->npools_alloc : 1, sizeof *r->pools);
    r->mixes = calloc((size_t)p.submix_count + 1, sizeof *r->mixes);
    r->sinks = calloc((size_t)p.sink_count, sizeof *r->sinks);
    r->effects = calloc((size_t)(p.effect_count ? p.effect_count : 1), sizeof *r->effects);
    r->channels = calloc((size_t)p.voice_count * 6, sizeof *r->channels);
    if (!r->voices || !r->pools || !r->mixes || !r->sinks || !r->effects || !r->channels) { napple_audren_close(r); return NAPPLE_AUDIO_E_NOMEM; }
    for (int i = 0; i < p.voice_count; i++) r->voices[i].adpcm.buffer = -1;
    pthread_mutex_init(&r->lock, NULL);
    r->state = 1;   /* stopped */
    r->manual = (flags & NAPPLE_AUDREN_MANUAL) != 0;
    { const char *e = getenv("NAPPLE_AUDIO_STATS"); if (e && *e && *e != '0') r->stats_every = (uint32_t)(15 * p.sample_rate / (p.sample_count ? p.sample_count : 240)); }
    if (mixer_open(r)) { napple_audren_close(r); return NAPPLE_AUDIO_E_NOMEM; }
    if (!r->manual) {
        atomic_store(&r->running, 1);
        if (pthread_create(&r->thread, NULL, render_thread, r)) { atomic_store(&r->running, 0); napple_audren_close(r); return NAPPLE_AUDIO_E_NOMEM; }
        r->thread_started = 1;
    }
    napple_audio_log(&r->host, "renderer open %d Hz x%d, voices %d effects %d submixes %d sinks %d splitters %d/%d REV%d",
                     p.sample_rate, p.sample_count, p.voice_count, p.effect_count, p.submix_count, p.sink_count,
                     p.splitter_count, p.splitter_destination_count, revision_number(p.revision));
    *out = r;
    return NAPPLE_AUDIO_OK;
}

void napple_audren_close(napple_audren *r) {
    if (!r) return;
    if (r->thread_started) { atomic_store(&r->running, 0); pthread_join(r->thread, NULL); }
    if (r->stats_every) log_stats(r, "close");
    mixer_close(r);
    pthread_mutex_destroy(&r->lock);
    free(r->voices); free(r->pools); free(r->mixes); free(r->sinks); free(r->effects); free(r->channels); free(r);
}

int napple_audren_start(napple_audren *r) {
    if (!r) return NAPPLE_AUDIO_E_INVALID;
    pthread_mutex_lock(&r->lock); r->state = 0; mixer_set_running(r, 1); pthread_mutex_unlock(&r->lock);
    return 0;
}
int napple_audren_stop(napple_audren *r) {
    if (!r) return NAPPLE_AUDIO_E_INVALID;
    pthread_mutex_lock(&r->lock); r->state = 1; mixer_set_running(r, 0); pthread_mutex_unlock(&r->lock);
    return 0;
}
uint32_t napple_audren_state(const napple_audren *r) {
    napple_audren *m = (napple_audren *)(uintptr_t)r; pthread_mutex_lock(&m->lock); uint32_t v = r->state; pthread_mutex_unlock(&m->lock); return v;
}
int32_t napple_audren_sample_rate(const napple_audren *r) { return r->param.sample_rate; }
int32_t napple_audren_sample_count(const napple_audren *r) { return r->param.sample_count; }
int32_t napple_audren_mix_buffer_count(const napple_audren *r) { return r->param.mix_buffer_count; }
uint32_t napple_audren_revision(const napple_audren *r) { return r->revision; }
void napple_audren_set_time_limit(napple_audren *r, uint32_t pct) { __atomic_store_n(&r->time_limit, pct, __ATOMIC_RELAXED); }
uint32_t napple_audren_get_time_limit(const napple_audren *r) { uint32_t v = __atomic_load_n(&r->time_limit, __ATOMIC_RELAXED); return v ? v : 100; }
void napple_audren_get_stats(napple_audren *r, napple_audren_stats *out) {
    if (!r || !out) return;
    pthread_mutex_lock(&r->lock); *out = r->stats; pthread_mutex_unlock(&r->lock);
}
void napple_audren_set_clock(napple_audren *r, uint64_t (*now)(void *user), void *user) {
    if (!r) return;
    pthread_mutex_lock(&r->lock); r->clock = now; r->clock_user = user; pthread_mutex_unlock(&r->lock);
}
uint64_t napple_audren_elapsed_frames(const napple_audren *r) {
    napple_audren *m = (napple_audren *)(uintptr_t)r; pthread_mutex_lock(&m->lock); uint64_t v = r->elapsed_frames; pthread_mutex_unlock(&m->lock); return v;
}
int napple_audren_read_output(napple_audren *r, float *lr, int frames) { return mixer_read(r, lr, frames); }

/* ---------- IAudioDevice ---------- */
_Atomic float napple_device_volume = 1.0f;
size_t napple_audio_device_name(char *out, size_t cap) {
    if (!out || !cap) return 0;
    size_t n = sizeof NAPPLE_AUDIO_DEVICE_NAME - 1; if (n > cap - 1) n = cap - 1;
    memcpy(out, NAPPLE_AUDIO_DEVICE_NAME, n); out[n] = 0;
    return n;
}
int32_t napple_audio_device_channel_count(void) { return 2; }
float napple_audio_device_get_volume(void) { return atomic_load(&napple_device_volume); }
void napple_audio_device_set_volume(float v) { atomic_store(&napple_device_volume, v < 0 ? 0 : v > 1 ? 1 : v); }
