/* Output engine: per-source lock-free rings summed by one Core Audio unit at 48 kHz stereo float.

   Producers (audren render threads, audout pumps) push interleaved stereo frames with nae_source_push.  The device
   callback (real time, no locks, no allocation) sums every active source, applies master volume and clips.
   Latency policy: a source starts playing once `prefill` frames are queued (default 20 ms, four renderer frames) and
   again after an underrun; if a device stall lets a backlog build up, everything beyond prefill + 30 ms is dropped. */
#include "internal.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OUT_RATE 48000
#define RING_FRAMES 16384            /* stereo frames per source, ~340 ms */
#define MAX_SOURCES 16

struct nae_source {
    float ring[RING_FRAMES * 2];
    _Atomic uint64_t wr, rd;
    _Atomic int active, flush_req;
    int ready;                        /* callback thread only */
    int rate;
    double phase; float last[2];      /* push-side resampler state (producer thread only) */
    _Atomic uint64_t underruns, overruns, missing, trims;
    _Atomic uint64_t fill_hint;
};

static struct {
    pthread_mutex_t lock;
    int refs, prefill_ms, device_wanted, device_ok;
    AudioComponentInstance unit;
    nae_source *_Atomic src[MAX_SOURCES];
    _Atomic uint32_t cb_gen;          /* odd while the callback runs */
    _Atomic float master; _Atomic int paused;
    _Atomic uint64_t cb_frames, underruns, missing, overruns, trims;
    _Atomic double latency_ms;
    UInt32 device_frames;
} E = {.lock = PTHREAD_MUTEX_INITIALIZER, .master = 1.0f};

/* ---- game speed ---- */
static _Atomic double g_speed = 1.0; static _Atomic int g_speed_mute;
double nae_speed(void) { return atomic_load_explicit(&g_speed, memory_order_relaxed); }
int nae_speed_muted(void) { return atomic_load_explicit(&g_speed_mute, memory_order_relaxed); }
long long nae_scaled_ns(long long ns) { double v = (double)ns / nae_speed(); return v < 1 ? 1 : (long long)v; }
double napple_audio_get_speed(void) { return nae_speed(); }
void napple_audio_set_speed(double speed, int mode) {
    if (!(speed >= 0.1)) speed = 0.1;
    if (speed > 10.0) speed = 10.0;
    int mute = mode == NAPPLE_AUDIO_SPEED_MUTE || (mode == NAPPLE_AUDIO_SPEED_AUTO && (speed > 2.0 || speed < 0.5));
    atomic_store(&g_speed, speed); atomic_store(&g_speed_mute, mute);
}

static int env_int(const char *name, int fallback, int lo, int hi) {
    const char *e = getenv(name);
    if (!e || !*e) return fallback;
    char *end; long v = strtol(e, &end, 10);
    if (*end || v < lo || v > hi) { napple_audio_log(NULL, "%s=%s ignored (want %d..%d)", name, e, lo, hi); return fallback; }
    return (int)v;
}

/* ---- device callback ---- */
static OSStatus pull(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                     AudioBufferList *io) {
    (void)ctx; (void)flags; (void)ts; (void)bus;
    float *out = io->mBuffers[0].mData;
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    atomic_fetch_add_explicit(&E.cb_gen, 1, memory_order_acq_rel);
    atomic_fetch_add_explicit(&E.cb_frames, frames, memory_order_relaxed);
    const int paused = atomic_load_explicit(&E.paused, memory_order_acquire) || nae_speed_muted();
    const uint64_t prefill = (uint64_t)(E.prefill_ms * OUT_RATE / 1000) > frames ? (uint64_t)(E.prefill_ms * OUT_RATE / 1000) : frames;
    uint64_t max_fill = 0;
    for (int i = 0; i < MAX_SOURCES; i++) {
        nae_source *s = atomic_load_explicit(&E.src[i], memory_order_acquire);
        if (!s) continue;
        uint64_t rd = atomic_load_explicit(&s->rd, memory_order_relaxed);
        uint64_t wr = atomic_load_explicit(&s->wr, memory_order_acquire);
        if (!atomic_load_explicit(&s->active, memory_order_acquire) || paused) {
            atomic_store_explicit(&s->rd, wr, memory_order_release); s->ready = 0; continue;
        }
        if (atomic_exchange_explicit(&s->flush_req, 0, memory_order_acq_rel)) { rd = wr; s->ready = 0; }
        uint64_t have = wr - rd;
        if (!s->ready) {
            if (have < prefill) { atomic_store_explicit(&s->rd, rd, memory_order_release); continue; }
            s->ready = 1;
        }
        const uint64_t max_backlog = prefill + (uint64_t)(OUT_RATE * 30 / 1000);
        if (have > max_backlog) {
            atomic_fetch_add(&s->trims, 1); atomic_fetch_add(&E.trims, 1);
            rd = wr - prefill; have = prefill;
        }
        UInt32 n = frames < have ? frames : (UInt32)have;
        for (UInt32 k = 0; k < n; k++) {
            uint64_t at = (rd + k) % RING_FRAMES;
            out[2 * k] += s->ring[2 * at]; out[2 * k + 1] += s->ring[2 * at + 1];
        }
        if (n < frames) {   /* underrun: output what we have, wait for a fresh prefill */
            atomic_fetch_add(&s->underruns, 1); atomic_fetch_add(&E.underruns, 1);
            atomic_fetch_add(&E.missing, frames - n); s->ready = 0;
        }
        atomic_store_explicit(&s->rd, rd + n, memory_order_release);
        if (have - n > max_fill) max_fill = have - n;
    }
    const float g = paused ? 0.f : atomic_load_explicit(&E.master, memory_order_relaxed);
    for (UInt32 k = 0; k < frames * 2; k++) {
        float v = out[k] * g;
        out[k] = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
    }
    atomic_store_explicit(&E.latency_ms, (double)(max_fill + E.device_frames) * 1000.0 / OUT_RATE, memory_order_relaxed);
    atomic_fetch_add_explicit(&E.cb_gen, 1, memory_order_acq_rel);
    return noErr;
}

static void device_close_locked(void) {
    if (!E.device_ok) return;
    E.device_ok = 0;
    AudioOutputUnitStop(E.unit); AudioUnitUninitialize(E.unit); AudioComponentInstanceDispose(E.unit);
}

/* Ask the default output device for a small buffer (the system may clamp it). Best effort. */
static UInt32 tune_device_buffer(void) {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioDeviceID dev = 0; UInt32 sz = sizeof dev;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev) || !dev) return 512;
    AudioObjectPropertyAddress fs = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 want = 256, cur = 0; sz = sizeof cur;
    AudioObjectPropertyAddress range = {kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioValueRange r; UInt32 rs = sizeof r;
    if (!AudioObjectGetPropertyData(dev, &range, 0, NULL, &rs, &r)) { if (want < r.mMinimum) want = (UInt32)r.mMinimum; if (want > r.mMaximum) want = (UInt32)r.mMaximum; }
    AudioObjectSetPropertyData(dev, &fs, 0, NULL, sizeof want, &want);
    if (AudioObjectGetPropertyData(dev, &fs, 0, NULL, &sz, &cur)) return 512;
    return cur;
}

static int device_open_locked(void) {
    if (!E.device_wanted) return 0;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &E.unit)) { napple_audio_log(NULL, "no default output device: mixing without sound"); return -1; }
    AudioStreamBasicDescription f = {OUT_RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AURenderCallbackStruct cb = {pull, NULL};
    E.device_frames = tune_device_buffer();
    if (AudioUnitSetProperty(E.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(E.unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(E.unit) || AudioOutputUnitStart(E.unit)) {
        napple_audio_log(NULL, "Core Audio output setup failed: mixing without sound");
        AudioComponentInstanceDispose(E.unit); return -1;
    }
    E.device_ok = 1;
    napple_audio_log(NULL, "Core Audio output %d Hz stereo, device buffer %u frames, prefill %d ms", OUT_RATE, (unsigned)E.device_frames, E.prefill_ms);
    return 0;
}

/* ---- public engine API ---- */
int napple_audio_init(const napple_audio_config *config) {
    pthread_mutex_lock(&E.lock);
    if (E.refs++ == 0) {
        napple_audio_config c = config ? *config : (napple_audio_config){0};
        E.prefill_ms = env_int("NAPPLE_AUDIO_PREFILL_MS", c.prefill_ms > 0 ? c.prefill_ms : 20, 0, 500);
        if (E.prefill_ms < 5) E.prefill_ms = 5;
        E.device_wanted = c.use_device && !getenv("NAPPLE_AUDIO_OFF");
        atomic_store(&E.master, c.master_volume > 0 ? (c.master_volume > 1 ? 1.f : c.master_volume) : 1.f);
        device_open_locked();
    }
    pthread_mutex_unlock(&E.lock);
    return NAPPLE_AUDIO_OK;
}

void napple_audio_shutdown(void) {
    pthread_mutex_lock(&E.lock);
    if (E.refs > 0 && --E.refs == 0) device_close_locked();
    pthread_mutex_unlock(&E.lock);
}

int nae_engine_acquire(void) {
    pthread_mutex_lock(&E.lock);
    int ok = E.refs > 0;
    pthread_mutex_unlock(&E.lock);
    return ok;
}
void nae_engine_release(void) {}
int nae_engine_has_device(void) { return E.device_ok; }

void napple_audio_set_master_volume(float v) { atomic_store(&E.master, v < 0 ? 0 : v > 1 ? 1 : v); }
float napple_audio_get_master_volume(void) { return atomic_load(&E.master); }
void napple_audio_set_paused(int p) { atomic_store_explicit(&E.paused, p != 0, memory_order_release); }

void napple_audio_output_restart(int recreate) {
    pthread_mutex_lock(&E.lock);
    if (E.refs > 0 && E.device_wanted) {
        if (E.device_ok && !recreate && !AudioOutputUnitStart(E.unit)) { napple_audio_log(NULL, "output started again"); }
        else { device_close_locked(); device_open_locked(); napple_audio_log(NULL, "output rebuilt: %s", E.device_ok ? "playing" : "no sound"); }
    }
    pthread_mutex_unlock(&E.lock);
}

void napple_audio_get_stats(napple_audio_stats *o) {
    memset(o, 0, sizeof *o);
    o->callback_frames = atomic_load(&E.cb_frames); o->underruns = atomic_load(&E.underruns);
    o->missing_frames = atomic_load(&E.missing); o->overruns = atomic_load(&E.overruns); o->trims = atomic_load(&E.trims);
    o->latency_ms = atomic_load(&E.latency_ms); o->device_open = E.device_ok;
}

/* ---- sources ---- */
nae_source *nae_source_create(int rate) {
    if (!nae_engine_acquire() || rate < 4000 || rate > 192000) return NULL;
    nae_source *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->rate = rate; atomic_store(&s->active, 1);
    pthread_mutex_lock(&E.lock);
    int placed = 0;
    for (int i = 0; i < MAX_SOURCES && !placed; i++)
        if (!atomic_load(&E.src[i])) { atomic_store_explicit(&E.src[i], s, memory_order_release); placed = 1; }
    pthread_mutex_unlock(&E.lock);
    if (!placed) { free(s); napple_audio_log(NULL, "too many audio sources"); return NULL; }
    return s;
}

void nae_source_destroy(nae_source *s) {
    if (!s) return;
    pthread_mutex_lock(&E.lock);
    for (int i = 0; i < MAX_SOURCES; i++) if (atomic_load(&E.src[i]) == s) atomic_store_explicit(&E.src[i], NULL, memory_order_release);
    pthread_mutex_unlock(&E.lock);
    /* wait for a callback that may still be reading the source */
    if (E.device_ok) {
        uint32_t g = atomic_load(&E.cb_gen);
        for (int spin = 0; (g & 1) && atomic_load(&E.cb_gen) == g && spin < 200000; spin++) usleep(10);
    }
    free(s);
}

void nae_source_set_active(nae_source *s, int active) {
    if (!s) return;
    if (!active) atomic_store(&s->flush_req, 1);
    atomic_store_explicit(&s->active, active != 0, memory_order_release);
}

static void ring_put(nae_source *s, const float *lr, int frames) {
    uint64_t wr = atomic_load_explicit(&s->wr, memory_order_relaxed);
    uint64_t rd = atomic_load_explicit(&s->rd, memory_order_acquire);
    if (E.device_ok && wr + (uint64_t)frames - rd > RING_FRAMES) {   /* device stalled: drop this block */
        atomic_fetch_add(&s->overruns, 1); atomic_fetch_add(&E.overruns, 1); return;
    }
    for (int i = 0; i < frames; i++) {
        uint64_t k = (wr + (uint64_t)i) % RING_FRAMES;
        s->ring[2 * k] = lr[2 * i]; s->ring[2 * k + 1] = lr[2 * i + 1];
    }
    atomic_store_explicit(&s->wr, wr + (uint64_t)frames, memory_order_release);
    if (!E.device_ok) atomic_store_explicit(&s->rd, wr + (uint64_t)frames, memory_order_release);   /* no device: keep only the latest */
}

void nae_source_push(nae_source *s, const float *lr, int frames) {
    if (!s || frames <= 0) return;
    const double speed = nae_speed();
    if (nae_speed_muted()) return;                                   /* turbo without sound: the callback drops the ring anyway */
    if (s->rate == OUT_RATE && speed == 1.0) { ring_put(s, lr, frames); return; }
    /* linear resampler to 48 kHz, state carried across pushes; at game speed s the source is consumed s times faster (pitch shifts with it) */
    float tmp[2 * 1024]; int n = 0;
    const double step = (double)s->rate / OUT_RATE * speed;
    /* position p in input samples, relative to this block, may start negative (between last and lr[0]) */
    double p = s->phase;
    while (p < frames) {
        int i0 = (int)floor(p); double f = p - i0;
        float a0 = i0 < 0 ? s->last[0] : lr[2 * i0], a1 = i0 < 0 ? s->last[1] : lr[2 * i0 + 1];
        int i1 = i0 + 1;
        if (i1 >= frames) break;
        float b0 = lr[2 * i1], b1 = lr[2 * i1 + 1];
        tmp[2 * n] = (float)(a0 + (b0 - a0) * f); tmp[2 * n + 1] = (float)(a1 + (b1 - a1) * f);
        if (++n == 1024) { ring_put(s, tmp, n); n = 0; }
        p += step;
    }
    if (n) ring_put(s, tmp, n);
    s->phase = p - frames;                      /* may be in [-1, step) relative to the next block */
    s->last[0] = lr[2 * (frames - 1)]; s->last[1] = lr[2 * (frames - 1) + 1];
}

int nae_source_peek(nae_source *s, float *lr, int frames) {
    uint64_t wr = atomic_load(&s->wr);
    if (!s || (uint64_t)frames > wr || frames > RING_FRAMES) return 0;
    for (int i = 0; i < frames; i++) {
        uint64_t k = (wr - (uint64_t)frames + (uint64_t)i) % RING_FRAMES;
        lr[2 * i] = s->ring[2 * k]; lr[2 * i + 1] = s->ring[2 * k + 1];
    }
    return frames;
}
uint64_t nae_source_frames_written(const nae_source *s) { return atomic_load(&s->wr); }
