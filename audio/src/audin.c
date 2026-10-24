/* audin:u backend: PCM16 capture streams (microphone).

   The guest appends buffers to fill; a pacing thread captures in real time (5 ms slices) from the process-wide capture source and writes into
   the oldest queued buffer, releasing it (tag available through get_released, buffer_event fires) when it is full.  With no capture source, or
   when the source has nothing to give, the stream carries silence: the console lists its headset device whether or not a microphone is attached,
   and games poll it, so the device always works.  The structure follows audout.c. */
#include "internal.h"
#include <math.h>
#include <pthread/qos.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_QUEUED 32
#define SLICE_MS 5

/* ---- the process-wide capture source ---- */
static struct { pthread_mutex_t lock; napple_audin_source_fn read; void *user; } S = {.lock = PTHREAD_MUTEX_INITIALIZER};
void napple_audin_set_source(napple_audin_source_fn read, void *user) {
    pthread_mutex_lock(&S.lock); S.read = read; S.user = user; pthread_mutex_unlock(&S.lock);
}
int napple_audin_has_source(void) { pthread_mutex_lock(&S.lock); int has = S.read != NULL; pthread_mutex_unlock(&S.lock); return has; }
size_t napple_audin_device_name(char *out, size_t cap) {
    if (!out || !cap) return 0;
    size_t n = sizeof NAPPLE_AUDIO_INPUT_NAME - 1; if (n > cap - 1) n = cap - 1;
    memcpy(out, NAPPLE_AUDIO_INPUT_NAME, n); out[n] = 0;
    return n;
}

typedef struct { uint64_t tag; uint8_t *data; uint64_t frames, done; } Queued;

struct napple_audin {
    napple_audio_host host;
    pthread_mutex_t lock; pthread_t thread; _Atomic int running;
    uint32_t rate, channels; _Atomic uint32_t state;
    _Atomic int manual;
    _Atomic float gain;
    Queued q[MAX_QUEUED]; unsigned qhead, qcount;
    uint64_t released[MAX_QUEUED]; unsigned rcount;
    napple_audin_stats stats;
};

/* Fill `frames` frames of interleaved PCM16 from the source (mono -> every channel); returns the frames the source provided. */
static uint32_t capture(napple_audin *in, int16_t *dst, uint32_t frames) {
    float mono[2048]; uint32_t got = 0; int have_source;
    if (frames > 2048) frames = 2048;
    pthread_mutex_lock(&S.lock);
    have_source = S.read != NULL;
    if (have_source) got = S.read(S.user, mono, frames, in->rate);   /* the source must not call back into audin: in->lock is held */
    pthread_mutex_unlock(&S.lock);
    if (got > frames) got = frames;
    const float g = atomic_load(&in->gain);
    for (uint32_t k = 0; k < frames; k++) {
        float x = k < got ? mono[k] * g : 0.f;
        int32_t v = !isfinite(x) ? 0 : x >= 1.f ? 32767 : x <= -1.f ? -32768 : (int32_t)lrintf(x * 32768.f);
        if (v > 32767) v = 32767;
        for (uint32_t c = 0; c < in->channels; c++) dst[(size_t)k * in->channels + c] = (int16_t)v;
    }
    if (got < frames && have_source) in->stats.shortfalls++;
    in->stats.source_frames += got; in->stats.silent_frames += frames - got; in->stats.frames += frames;
    return got;
}

static void release_head(napple_audin *in) {
    Queued *b = &in->q[in->qhead];
    if (in->rcount < MAX_QUEUED) in->released[in->rcount++] = b->tag;
    in->qhead = (in->qhead + 1) % MAX_QUEUED; in->qcount--;
}

static void produce(napple_audin *in, uint32_t frames) {
    int released = 0;
    pthread_mutex_lock(&in->lock);
    while (frames && in->qcount) {
        Queued *b = &in->q[in->qhead];
        uint64_t left = b->frames - b->done; uint32_t n = left < frames ? (uint32_t)left : frames;
        if (n > 2048) n = 2048;
        int16_t tmp[2048 * 2];
        capture(in, tmp, n);
        uint8_t *dst = b->data + b->done * in->channels * 2;
        napple_audio_write_prepare(&in->host, dst, (size_t)n * in->channels * 2);
        memcpy(dst, tmp, (size_t)n * in->channels * 2);
        b->done += n; frames -= n;
        if (b->done >= b->frames) { release_head(in); released = 1; }
    }
    /* nothing queued: the device keeps capturing into the void, but the source still has to be drained */
    if (frames && !in->qcount) { int16_t tmp[2048 * 2]; while (frames) { uint32_t n = frames > 2048 ? 2048 : frames; capture(in, tmp, n); frames -= n; } }
    pthread_mutex_unlock(&in->lock);
    if (released && in->host.buffer_event) in->host.buffer_event(in->host.user);
}

static void *pacer(void *p) {
    napple_audin *in = p;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const uint32_t slice = in->rate * SLICE_MS / 1000;
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    while (atomic_load_explicit(&in->running, memory_order_acquire)) {
        if (atomic_load(&in->state) == 0 && !atomic_load(&in->manual)) produce(in, slice);
        next.tv_nsec += (long)nae_scaled_ns(SLICE_MS * 1000000LL);
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t wait = (int64_t)(next.tv_sec - now.tv_sec) * 1000000000LL + (next.tv_nsec - now.tv_nsec);
        if (wait > 0) { struct timespec d = {wait / 1000000000LL, wait % 1000000000LL}; nanosleep(&d, NULL); }
        else if (wait < -100000000LL) next = now;
    }
    return NULL;
}

int napple_audin_open(uint32_t rate, uint32_t channels, const napple_audio_host *host, napple_audin **out,
                      uint32_t *granted_rate, uint32_t *granted_channels) {
    if (!out) return NAPPLE_AUDIO_E_INVALID;
    if (!rate) rate = 48000;
    if (!channels) channels = 2;
    if (rate < 8000 || rate > 96000 || (channels != 1 && channels != 2)) return NAPPLE_AUDIO_E_INVALID;
    napple_audin *in = calloc(1, sizeof *in);
    if (!in) return NAPPLE_AUDIO_E_NOMEM;
    if (host) in->host = *host;
    in->rate = rate; in->channels = channels; atomic_store(&in->state, 1); atomic_store(&in->gain, 1.0f);
    pthread_mutex_init(&in->lock, NULL);
    atomic_store(&in->running, 1);
    if (pthread_create(&in->thread, NULL, pacer, in)) { pthread_mutex_destroy(&in->lock); free(in); return NAPPLE_AUDIO_E_NOMEM; }
    if (granted_rate) *granted_rate = rate;
    if (granted_channels) *granted_channels = channels;
    *out = in;
    return NAPPLE_AUDIO_OK;
}

void napple_audin_close(napple_audin *in) {
    if (!in) return;
    atomic_store(&in->running, 0); pthread_join(in->thread, NULL);
    pthread_mutex_destroy(&in->lock); free(in);
}

int napple_audin_start(napple_audin *in) { if (!in) return NAPPLE_AUDIO_E_INVALID; atomic_store(&in->state, 0); return 0; }
int napple_audin_stop(napple_audin *in) { if (!in) return NAPPLE_AUDIO_E_INVALID; atomic_store(&in->state, 1); return 0; }
uint32_t napple_audin_state(const napple_audin *in) { return atomic_load(&in->state); }

int napple_audin_append(napple_audin *in, uint64_t tag, uint64_t addr, uint64_t size, uint64_t offset) {
    if (!in || size > (1u << 28) || offset > (1u << 28)) return NAPPLE_AUDIO_E_INVALID;
    uint8_t *data = napple_audio_guest(&in->host, addr + offset, size);
    if (!data && size) return NAPPLE_AUDIO_E_INVALID;
    pthread_mutex_lock(&in->lock);
    /* the limit counts released buffers the guest has not collected yet: the released list has the same 32 slots, and a tag that did not fit
       there would be lost (the guest would wait for it for ever) */
    if (in->qcount + in->rcount >= MAX_QUEUED) { pthread_mutex_unlock(&in->lock); return NAPPLE_AUDIO_E_STATE; }
    Queued *b = &in->q[(in->qhead + in->qcount) % MAX_QUEUED];
    *b = (Queued){tag, data, size / (2ull * in->channels), 0};
    in->qcount++;
    pthread_mutex_unlock(&in->lock);
    return 0;
}

uint32_t napple_audin_get_released(napple_audin *in, uint64_t *tags, uint32_t max) {
    pthread_mutex_lock(&in->lock);
    uint32_t n = in->rcount < max ? in->rcount : max;
    if (tags) memcpy(tags, in->released, n * sizeof *tags);
    memmove(in->released, in->released + n, (in->rcount - n) * sizeof *in->released);
    in->rcount -= n;
    pthread_mutex_unlock(&in->lock);
    return n;
}

int napple_audin_contains(const napple_audin *in, uint64_t tag) {
    napple_audin *m = (napple_audin *)(uintptr_t)in;
    pthread_mutex_lock(&m->lock);
    int found = 0;
    for (unsigned i = 0; i < in->qcount; i++) if (in->q[(in->qhead + i) % MAX_QUEUED].tag == tag) found = 1;
    pthread_mutex_unlock(&m->lock);
    return found;
}

uint32_t napple_audin_buffer_count(const napple_audin *in) {
    napple_audin *m = (napple_audin *)(uintptr_t)in;
    pthread_mutex_lock(&m->lock); uint32_t n = in->qcount + in->rcount; pthread_mutex_unlock(&m->lock);
    return n;
}

int napple_audin_flush(napple_audin *in) {
    int any = 0;
    pthread_mutex_lock(&in->lock);
    while (in->qcount) {
        Queued *b = &in->q[in->qhead];
        uint8_t *dst = b->data + b->done * in->channels * 2; size_t rest = (size_t)(b->frames - b->done) * in->channels * 2;
        if (rest) { napple_audio_write_prepare(&in->host, dst, rest); memset(dst, 0, rest); }
        release_head(in); any = 1;
    }
    pthread_mutex_unlock(&in->lock);
    if (any && in->host.buffer_event) in->host.buffer_event(in->host.user);
    return any;
}

void napple_audin_set_gain(napple_audin *in, float gain) { atomic_store(&in->gain, !(gain >= 0) ? 0.f : gain > 2.f ? 2.f : gain); }
float napple_audin_get_gain(const napple_audin *in) { return atomic_load(&in->gain); }
void napple_audin_get_stats(napple_audin *in, napple_audin_stats *out) {
    pthread_mutex_lock(&in->lock); *out = in->stats; out->queued = in->qcount; out->released = in->rcount; pthread_mutex_unlock(&in->lock);
}
void napple_audin_set_manual(napple_audin *in, int manual) { atomic_store(&in->manual, manual != 0); }
void napple_audin_pump(napple_audin *in, uint32_t frames) { while (frames) { uint32_t n = frames > 2048 ? 2048 : frames; produce(in, n); frames -= n; } }
