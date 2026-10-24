/* audren:u, audout:u, hwopus: the IPC surface over libnapple_audio (audio/include/napple_audio.h, docs/audio-input-frontend-interface.md).
 * The library keeps its own state (the transfer memory work buffers the guest hands over are not used), mixes on its own threads and
 * pushes to one Core Audio unit; this file only parses requests, owns the Horizon events the library signals and maps errors. */
#include "../ipc.h"
#include "../gfx.h"
#include "napple_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

static int audio_ready; static pthread_mutex_t audio_mu = PTHREAD_MUTEX_INITIALIZER;
/* Output health (Core Audio device, the numbers that tell a stutter from a clean stream): logged every 15 s and when the process stops. */
void audio_report(void) {
    napple_audio_stats st; napple_audio_get_stats(&st);
    if (!st.device_open) return;
    LOG("audio", "output: %llu frames requested, %llu underruns (%llu frames missing), %llu overruns, %llu trims, latency %.1f ms",
        (unsigned long long)st.callback_frames, (unsigned long long)st.underruns, (unsigned long long)st.missing_frames, (unsigned long long)st.overruns, (unsigned long long)st.trims, st.latency_ms);
}
static void *audio_report_thread(void *arg) { (void)arg; for (;;) { sleep(15); audio_report(); } return NULL; }
/* Thread safe and idempotent: the first caller opens the engine (the frontend warms it up in the background while the window is shown, the guest
 * opens it lazily); later callers wait for it. Opening the Core Audio unit can take seconds on a cold audio daemon, which must never block the UI. */
void audio_init_once(void) {
    pthread_mutex_lock(&audio_mu);
    if (audio_ready) { pthread_mutex_unlock(&audio_mu); return; }
    audio_ready = 1;
    napple_audio_config c = {0};
    /* Headless runs use the null sink unless NAPPLE_AUDIO_DEVICE=1 (no sound in boot tests). */
    c.use_device = !g_cfg.headless || (getenv("NAPPLE_AUDIO_DEVICE") && atoi(getenv("NAPPLE_AUDIO_DEVICE")));
    if (getenv("NAPPLE_AUDIO_VOLUME")) c.master_volume = (float)atof(getenv("NAPPLE_AUDIO_VOLUME"));
    napple_audio_init(&c);
    LOG("audio", "engine started (%s)", c.use_device ? "Core Audio output" : "null sink");
    if (c.use_device) { pthread_t t; if (!pthread_create(&t, NULL, audio_report_thread, NULL)) pthread_detach(t); }
    pthread_mutex_unlock(&audio_mu);
}

static void noop(IpcObject *s, IpcCall *c) { (void)s; (void)c; }
static uint32_t map_err(int r) {
    switch (r) {
    case NAPPLE_AUDIO_OK: return 0;
    case NAPPLE_AUDIO_E_PROTOCOL: return 153u | (41u << 9);       /* ResultInvalidUpdateInfo */
    case NAPPLE_AUDIO_E_SIZE: return 153u | (4u << 9);            /* ResultInsufficientBuffer */
    case NAPPLE_AUDIO_E_UNSUPPORTED: return 153u | (3u << 9);     /* ResultInvalidParameter-ish: revision/format not implemented */
    case NAPPLE_AUDIO_E_STATE: return 153u | (7u << 9);
    case NAPPLE_AUDIO_E_NOMEM: return 153u | (6u << 9);
    default: return 153u | (2u << 9);                             /* ResultInvalidParameter */
    }
}

/* ---- host callbacks ------------------------------------------------------------------------------------------------------------ */
static void host_write_prepare(void *user, void *p, size_t n) { (void)user; gfx_guest_write_prepare(p, n); }
static void host_log(void *user, const char *line) { (void)user; LOGV("audio", "%s", line); }
typedef struct { uint32_t ev_w; uint32_t ev_r; } EventPair;
static void host_frame_event(void *user) { EventPair *e = user; event_signal_handle(e->ev_w); }
static void host_buffer_event(void *user) { EventPair *e = user; event_signal_handle(e->ev_w); }
static napple_audio_host make_host(EventPair *e, int frame) {
    napple_audio_host h = {0};
    h.user = e; h.write_prepare = host_write_prepare; h.log = host_log;
    if (frame) h.frame_event = host_frame_event; else h.buffer_event = host_buffer_event;
    return h;
}
static EventPair *new_event_pair(void) { EventPair *e = calloc(1, sizeof *e); e->ev_w = event_create(&e->ev_r); return e; }

/* ---- IAudioDevice ---------------------------------------------------------------------------------------------------------------- */
static const uint8_t name_none[1];
static void dev_list_names(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    int32_t n = 0;
    if (b && b->size >= 0x100) { uint8_t name[0x100] = {0}; napple_audio_device_name((char *)name, sizeof name); ipc_write_buf(c, b, name, sizeof name); n = 1; } else if (b) ipc_write_buf(c, b, name_none, 0);
    IPC_OUT(c, n);
}
static void dev_set_volume(IpcObject *s, IpcCall *c) { (void)s; float v = IPC_IN(c, float, 0); napple_audio_device_set_volume(v); }
static void dev_get_volume(IpcObject *s, IpcCall *c) { (void)s; float v = napple_audio_device_get_volume(); IPC_OUT(c, v); }
static void dev_active_name(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) { uint8_t name[0x100] = {0}; napple_audio_device_name((char *)name, sizeof name); ipc_write_buf(c, b, name, b->size < sizeof name ? b->size : sizeof name); }
}
static void dev_event(IpcObject *s, IpcCall *c) { (void)s; uint32_t r = 0; event_create(&r); ipc_out_copy_handle(c, r); }
static void dev_channels(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = (uint32_t)napple_audio_device_channel_count(); IPC_OUT(c, v); }
static void dev_master_volume(IpcObject *s, IpcCall *c) { (void)s; float v = 1.0f; IPC_OUT(c, v); }
static const IpcCmd dev_cmds[] = {
    {0, "ListAudioDeviceName", dev_list_names, IPC_IMPL}, {1, "SetAudioDeviceOutputVolume", dev_set_volume, IPC_IMPL}, {2, "GetAudioDeviceOutputVolume", dev_get_volume, IPC_IMPL},
    {3, "GetActiveAudioDeviceName", dev_active_name, IPC_IMPL}, {4, "QueryAudioDeviceSystemEvent", dev_event, IPC_STUB}, {5, "GetActiveChannelCount", dev_channels, IPC_IMPL},
    {6, "ListAudioDeviceNameAuto", dev_list_names, IPC_IMPL}, {7, "SetAudioDeviceOutputVolumeAuto", dev_set_volume, IPC_IMPL}, {8, "GetAudioDeviceOutputVolumeAuto", dev_get_volume, IPC_IMPL},
    {10, "GetActiveAudioDeviceNameAuto", dev_active_name, IPC_IMPL}, {11, "QueryAudioDeviceInputEvent", dev_event, IPC_STUB}, {12, "QueryAudioDeviceOutputEvent", dev_event, IPC_STUB},
    {13, "GetAudioSystemMasterVolumeSetting", dev_master_volume, IPC_STUB},
};
static IpcObject *make_device(void) { audio_init_once(); return IPC_OBJECT("IAudioDevice", dev_cmds, NULL, NULL); }

/* ---- IAudioRenderer ---------------------------------------------------------------------------------------------------------------- */
typedef struct { napple_audren *r; EventPair *ev; } Renderer;
static void rend_destroy(IpcObject *o) { Renderer *r = o->state; napple_audren_close(r->r); free(r); }
static void rend_sample_rate(IpcObject *s, IpcCall *c) { Renderer *r = s->state; uint32_t v = (uint32_t)napple_audren_sample_rate(r->r); IPC_OUT(c, v); }
static void rend_sample_count(IpcObject *s, IpcCall *c) { Renderer *r = s->state; uint32_t v = (uint32_t)napple_audren_sample_count(r->r); IPC_OUT(c, v); }
static void rend_mix_count(IpcObject *s, IpcCall *c) { Renderer *r = s->state; uint32_t v = (uint32_t)napple_audren_mix_buffer_count(r->r); IPC_OUT(c, v); }
static void rend_state(IpcObject *s, IpcCall *c) { Renderer *r = s->state; uint32_t v = napple_audren_state(r->r); IPC_OUT(c, v); }
static void rend_update(IpcObject *s, IpcCall *c) {
    Renderer *r = s->state;
    const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0), *perf = ipc_out_buf(c, 1);
    if (!in || !out) { c->result = 153u | (2u << 9); return; }
    void *ip = ipc_buf_ptr(in), *op = ipc_buf_ptr(out), *pp = perf && perf->size ? ipc_buf_ptr(perf) : NULL;
    if (!ip || !op) { c->result = 153u | (2u << 9); return; }
    guest_write_prepare(out->addr, (size_t)out->size);
    if (pp) guest_write_prepare(perf->addr, (size_t)perf->size);
    size_t written = 0;
    int rc = napple_audren_request_update(r->r, ip, (size_t)in->size, op, (size_t)out->size, &written, pp, pp ? (size_t)perf->size : 0);
    if (rc) { LOG("audio", "RequestUpdate failed (%d) revision %#x: input %#llx bytes, output %#llx bytes, library wants %#zx", rc, napple_audren_revision(r->r), (unsigned long long)in->size, (unsigned long long)out->size, written); summary_note("audren RequestUpdate error %d", rc); }
    /* RequestUpdateAuto receives the output through a receive-list buffer: report the written size. */
    if (out >= c->c && out < c->c + IPC_MAX_BUF && c->nxout < IPC_MAX_BUF) c->xout[c->nxout++] = (IpcBuf){out->addr, written, out->index};
    c->result = map_err(rc);
}
static void rend_start(IpcObject *s, IpcCall *c) { Renderer *r = s->state; c->result = map_err(napple_audren_start(r->r)); }
static void rend_stop(IpcObject *s, IpcCall *c) { Renderer *r = s->state; c->result = map_err(napple_audren_stop(r->r)); }
static void rend_event(IpcObject *s, IpcCall *c) { Renderer *r = s->state; ipc_out_copy_handle(c, r->ev->ev_r); }
static void rend_set_limit(IpcObject *s, IpcCall *c) { Renderer *r = s->state; napple_audren_set_time_limit(r->r, IPC_IN(c, uint32_t, 0)); }
static void rend_get_limit(IpcObject *s, IpcCall *c) { Renderer *r = s->state; uint32_t v = napple_audren_get_time_limit(r->r); IPC_OUT(c, v); }
static const IpcCmd rend_cmds[] = {
    {0, "GetSampleRate", rend_sample_rate, IPC_IMPL}, {1, "GetSampleCount", rend_sample_count, IPC_IMPL}, {2, "GetMixBufferCount", rend_mix_count, IPC_IMPL},
    {3, "GetState", rend_state, IPC_IMPL}, {4, "RequestUpdate", rend_update, IPC_IMPL}, {5, "Start", rend_start, IPC_IMPL}, {6, "Stop", rend_stop, IPC_IMPL},
    {7, "QuerySystemEvent", rend_event, IPC_IMPL}, {8, "SetRenderingTimeLimit", rend_set_limit, IPC_IMPL}, {9, "GetRenderingTimeLimit", rend_get_limit, IPC_IMPL},
    {10, "RequestUpdateAuto", rend_update, IPC_IMPL}, {11, "ExecuteAudioRendererRendering", noop, IPC_STUB},
};

/* ---- audren:u ------------------------------------------------------------------------------------------------------------------------ */
static void mgr_open(IpcObject *s, IpcCall *c) {
    (void)s; audio_init_once();
    const uint8_t *param = ipc_in(c, 0, NAPPLE_AUDREN_PARAM_SIZE);
    Renderer *r = calloc(1, sizeof *r);
    r->ev = new_event_pair();
    napple_audio_host h = make_host(r->ev, 1);
    int rc = napple_audren_open(param, NAPPLE_AUDREN_PARAM_SIZE, &h, 0, &r->r);
    if (rc) { LOG("audio", "OpenAudioRenderer failed (%d)", rc); free(r); c->result = map_err(rc); return; }
    LOG("audio", "audio renderer opened: %d Hz, %d samples per frame, %d mix buffers, revision %#x", napple_audren_sample_rate(r->r), napple_audren_sample_count(r->r), napple_audren_mix_buffer_count(r->r), napple_audren_revision(r->r));
    ipc_out_object(c, IPC_OBJECT("IAudioRenderer", rend_cmds, r, rend_destroy));
}
static void mgr_work_size(IpcObject *s, IpcCall *c) {
    (void)s; uint64_t size = 0;
    int rc = napple_audren_work_buffer_size(ipc_in(c, 0, NAPPLE_AUDREN_PARAM_SIZE), NAPPLE_AUDREN_PARAM_SIZE, &size);
    if (rc) { c->result = map_err(rc); return; }
    IPC_OUT(c, size);
}
static void mgr_device(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, make_device()); }
static const IpcCmd audren_cmds[] = {
    {0, "OpenAudioRenderer", mgr_open, IPC_IMPL}, {1, "GetWorkBufferSize", mgr_work_size, IPC_IMPL}, {2, "GetAudioDeviceService", mgr_device, IPC_IMPL},
    {3, "OpenAudioRendererForManualExecution", mgr_open, IPC_IMPL}, {4, "GetAudioDeviceServiceWithRevisionInfo", mgr_device, IPC_IMPL},
};
static IpcObject *audren_open(void) { return IPC_OBJECT("audren:u", audren_cmds, NULL, NULL); }

/* ---- IAudioOut / audout:u ------------------------------------------------------------------------------------------------------------- */
typedef struct { napple_audout *o; EventPair *ev; } AudOut;
static void out_destroy(IpcObject *o) { AudOut *a = o->state; napple_audout_close(a->o); free(a); }
static void out_get_state(IpcObject *s, IpcCall *c) { AudOut *a = s->state; uint32_t v = napple_audout_state(a->o); IPC_OUT(c, v); }
static void out_start(IpcObject *s, IpcCall *c) { AudOut *a = s->state; c->result = map_err(napple_audout_start(a->o)); }
static void out_stop(IpcObject *s, IpcCall *c) { AudOut *a = s->state; c->result = map_err(napple_audout_stop(a->o)); }
static void out_append(IpcObject *s, IpcCall *c) {
    AudOut *a = s->state;
    uint64_t tag = IPC_IN(c, uint64_t, 0);
    const IpcBuf *b = ipc_in_buf(c, 0);
    if (!b || b->size < 0x28) { c->result = 153u | (2u << 9); return; }
    const uint64_t *w = ipc_buf_ptr(b);                 /* {next, sample buffer, capacity, data size, data offset} */
    if (!w) { c->result = 153u | (2u << 9); return; }
    LOGV("audio", "audout append tag %#llx buffer %#llx capacity %#llx size %#llx offset %#llx", (unsigned long long)tag, (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3], (unsigned long long)w[4]);
    int rc = napple_audout_append(a->o, tag, w[1], w[3], w[4]);
    c->result = rc == NAPPLE_AUDIO_E_STATE ? (153u | (8u << 9)) : map_err(rc);   /* queue full: ResultBufferCountExceeded-like */
}
static void out_event(IpcObject *s, IpcCall *c) { AudOut *a = s->state; ipc_out_copy_handle(c, a->ev->ev_r); }
static void out_released(IpcObject *s, IpcCall *c) {
    AudOut *a = s->state;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint64_t tags[32] = {0}; uint32_t max = b ? (uint32_t)(b->size / 8) : 0; if (max > 32) max = 32;
    uint32_t n = max ? napple_audout_get_released(a->o, tags, max) : 0;
    /* The whole buffer is written, unused entries as 0: nn::audio::GetReleasedAudioOutBuffer returns the first array element without looking at the
     * count, so "none released" has to read as a null tag. */
    if (b) ipc_write_buf(c, b, tags, b->size < sizeof tags ? b->size : sizeof tags);
    if (log_verbose) for (unsigned i = 0; i < c->nb; i++) LOG("audio", "   B[%u] %#llx+%#llx", i, (unsigned long long)c->b[i].addr, (unsigned long long)c->b[i].size);
    if (log_verbose) for (unsigned i = 0; i < c->nc; i++) LOG("audio", "   C[%u] %#llx+%#llx", i, (unsigned long long)c->c[i].addr, (unsigned long long)c->c[i].size);
    LOGV("audio", "audout released %u buffers (out buffer %#llx bytes, state %u)%s", n, b ? (unsigned long long)b->size : 0ull, napple_audout_state(a->o), n ? "" : " -> none");
    if (n) LOGV("audio", "   first tag %#llx", (unsigned long long)tags[0]);
    IPC_OUT(c, n);
}
static void out_contains(IpcObject *s, IpcCall *c) { AudOut *a = s->state; uint8_t v = (uint8_t)napple_audout_contains(a->o, IPC_IN(c, uint64_t, 0)); IPC_OUT(c, v); }
static void out_count(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = 0; IPC_OUT(c, v); }
static void out_played(IpcObject *s, IpcCall *c) { (void)s; uint64_t v = 0; IPC_OUT(c, v); }
static void out_flush(IpcObject *s, IpcCall *c) { (void)s; uint8_t v = 0; IPC_OUT(c, v); }
static const IpcCmd out_cmds[] = {
    {0, "GetAudioOutState", out_get_state, IPC_IMPL}, {1, "StartAudioOut", out_start, IPC_IMPL}, {2, "StopAudioOut", out_stop, IPC_IMPL},
    {3, "AppendAudioOutBuffer", out_append, IPC_IMPL}, {4, "RegisterBufferEvent", out_event, IPC_IMPL}, {5, "GetReleasedAudioOutBuffer", out_released, IPC_IMPL},
    {6, "ContainsAudioOutBuffer", out_contains, IPC_IMPL}, {7, "AppendAudioOutBufferAuto", out_append, IPC_IMPL}, {8, "GetReleasedAudioOutBuffersAuto", out_released, IPC_IMPL},
    {9, "GetAudioOutBufferCount", out_count, IPC_STUB}, {10, "GetAudioOutPlayedSampleCount", out_played, IPC_STUB}, {11, "FlushAudioOutBuffers", out_flush, IPC_STUB},
};
static void aout_list(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0); int32_t n = 0;
    if (b && b->size >= 0x100) { uint8_t name[0x100] = {0}; snprintf((char *)name, sizeof name, "DeviceOut"); ipc_write_buf(c, b, name, sizeof name); n = 1; } else if (b) ipc_write_buf(c, b, name_none, 0);
    IPC_OUT(c, n);
}
static void aout_open(IpcObject *s, IpcCall *c) {
    (void)s; audio_init_once();
    uint32_t rate = IPC_IN(c, uint32_t, 0); uint16_t ch = IPC_IN(c, uint16_t, 4);
    AudOut *a = calloc(1, sizeof *a);
    a->ev = new_event_pair();
    napple_audio_host h = make_host(a->ev, 0);
    uint32_t grate = 0, gch = 0;
    int rc = napple_audout_open(rate, ch, &h, &a->o, &grate, &gch);
    if (rc) { free(a); c->result = map_err(rc); return; }
    const IpcBuf *nb = ipc_out_buf(c, 0);
    if (nb && nb->size >= 0x100) { uint8_t name[0x100] = {0}; snprintf((char *)name, sizeof name, "DeviceOut"); ipc_write_buf(c, nb, name, sizeof name); }
    uint32_t out[4] = {grate, gch, 2 /* PCM16 */, 0};
    ipc_out(c, out, sizeof out);
    LOG("audio", "audout opened: %u Hz, %u channels", grate, gch);
    ipc_out_object(c, IPC_OBJECT("IAudioOut", out_cmds, a, out_destroy));
}
static const IpcCmd audout_cmds[] = {
    {0, "ListAudioOuts", aout_list, IPC_IMPL}, {1, "OpenAudioOut", aout_open, IPC_IMPL}, {2, "ListAudioOutsAuto", aout_list, IPC_IMPL}, {3, "OpenAudioOutAuto", aout_open, IPC_IMPL},
};
static IpcObject *audout_open(void) { return IPC_OBJECT("audout:u", audout_cmds, NULL, NULL); }

/* ---- hwopus -------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { napple_hwopus *d; int multistream; } Opus;
static void opus_destroy(IpcObject *o) { Opus *p = o->state; napple_hwopus_close(p->d); free(p); }
/* Decode: in buffer = packet (Nintendo header + payload), out buffer = interleaved PCM16. */
static void opus_decode_common(IpcObject *s, IpcCall *c, int reset, int perf) {
    Opus *p = s->state;
    const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0);
    if (!in || !out) { c->result = 153u | (2u << 9); return; }
    const uint8_t *pk = ipc_buf_ptr(in); int16_t *pcm = ipc_buf_ptr(out);
    if (!pk || !pcm) { c->result = 153u | (2u << 9); return; }
    if (reset) napple_hwopus_reset(p->d);
    guest_write_prepare(out->addr, (size_t)out->size);   /* the library does not call write_prepare for the PCM buffer */
    size_t consumed = 0; int32_t samples = 0;
    int rc = napple_hwopus_decode(p->d, pk, (size_t)in->size, pcm, (size_t)out->size, &consumed, &samples);
    if (rc) { c->result = rc == NAPPLE_AUDIO_E_SIZE ? (111u | (3u << 9)) : (111u | (4u << 9)); LOGV("audio", "opus decode error %d", rc); return; }
    uint32_t cs = (uint32_t)consumed, sm = (uint32_t)samples;
    IPC_OUT(c, cs); IPC_OUT(c, sm);
    if (perf) { uint64_t t = 0; IPC_OUT(c, t); }
}
static void opus_dec_old(IpcObject *s, IpcCall *c) { opus_decode_common(s, c, 0, 0); }
static void opus_dec_perf_old(IpcObject *s, IpcCall *c) { opus_decode_common(s, c, 0, 1); }
static void opus_dec(IpcObject *s, IpcCall *c) { opus_decode_common(s, c, IPC_IN(c, uint8_t, 0) != 0, 1); }
static const IpcCmd opus_cmds[] = {
    {0, "DecodeInterleavedOld", opus_dec_old, IPC_IMPL}, {1, "SetContext", noop, IPC_STUB}, {2, "DecodeInterleavedForMultiStreamOld", opus_dec_old, IPC_IMPL},
    {3, "SetContextForMultiStream", noop, IPC_STUB}, {4, "DecodeInterleavedWithPerfOld", opus_dec_perf_old, IPC_IMPL},
    {5, "DecodeInterleavedForMultiStreamWithPerfOld", opus_dec_perf_old, IPC_IMPL}, {6, "DecodeInterleaved", opus_dec, IPC_IMPL}, {7, "DecodeInterleavedForMultiStream", opus_dec, IPC_IMPL},
};
static void opus_work_size(IpcObject *s, IpcCall *c) {
    (void)s; uint32_t rate = IPC_IN(c, uint32_t, 0), ch = IPC_IN(c, uint32_t, 4);
    uint32_t v = (uint32_t)napple_hwopus_work_buffer_size(rate, ch); IPC_OUT(c, v);
}
static void opus_open(IpcObject *s, IpcCall *c) {
    (void)s; audio_init_once();
    uint32_t rate = IPC_IN(c, uint32_t, 0), ch = IPC_IN(c, uint32_t, 4);
    Opus *p = calloc(1, sizeof *p);
    int rc = napple_hwopus_open(rate, ch, &p->d);
    if (rc) { free(p); c->result = 111u | (2u << 9); return; }
    ipc_out_object(c, IPC_OBJECT("IHardwareOpusDecoder", opus_cmds, p, opus_destroy));
}
static int ms_params(IpcCall *c, napple_hwopus_ms_params *p) {
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *g = b && b->size >= 16 ? ipc_buf_ptr(b) : NULL;
    if (!g) return -1;
    memset(p, 0, sizeof *p);
    memcpy(&p->sample_rate, g, 4); memcpy(&p->channel_count, g + 4, 4); memcpy(&p->stream_count, g + 8, 4); memcpy(&p->stereo_stream_count, g + 12, 4);
    size_t n = b->size - 16 < 255 ? b->size - 16 : 255; memcpy(p->mapping, g + 16, n);
    return 0;
}
static void opus_work_size_ms(IpcObject *s, IpcCall *c) {
    (void)s; napple_hwopus_ms_params p; if (ms_params(c, &p)) { c->result = 111u | (2u << 9); return; }
    uint32_t v = (uint32_t)napple_hwopus_ms_work_buffer_size(&p); IPC_OUT(c, v);
}
static void opus_open_ms(IpcObject *s, IpcCall *c) {
    (void)s; audio_init_once();
    napple_hwopus_ms_params mp; if (ms_params(c, &mp)) { c->result = 111u | (2u << 9); return; }
    Opus *p = calloc(1, sizeof *p); p->multistream = 1;
    if (napple_hwopus_open_ms(&mp, &p->d)) { free(p); c->result = 111u | (2u << 9); return; }
    ipc_out_object(c, IPC_OBJECT("IHardwareOpusDecoder", opus_cmds, p, opus_destroy));
}
static const IpcCmd hwopus_cmds[] = {
    {0, "OpenHardwareOpusDecoder", opus_open, IPC_IMPL}, {1, "GetWorkBufferSize", opus_work_size, IPC_IMPL},
    {2, "OpenHardwareOpusDecoderForMultiStream", opus_open_ms, IPC_IMPL}, {3, "GetWorkBufferSizeForMultiStream", opus_work_size_ms, IPC_IMPL},
    {4, "OpenHardwareOpusDecoderEx", opus_open, IPC_IMPL}, {5, "GetWorkBufferSizeEx", opus_work_size, IPC_IMPL},
    {6, "OpenHardwareOpusDecoderForMultiStreamEx", opus_open_ms, IPC_IMPL}, {7, "GetWorkBufferSizeForMultiStreamEx", opus_work_size_ms, IPC_IMPL},
};
static IpcObject *hwopus_open(void) { return IPC_OBJECT("hwopus", hwopus_cmds, NULL, NULL); }

void service_register_audio(void) {
    ipc_register_service("audren:u", audren_open);
    ipc_register_service("audout:u", audout_open);
    ipc_register_service("hwopus", hwopus_open);
}
