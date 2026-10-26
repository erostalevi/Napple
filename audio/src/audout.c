/* audout:u backend: simple PCM16 output streams.

   The guest appends buffers of interleaved PCM16 (1, 2 or 6 channels).  A pacing thread consumes them in real time
   (5 ms slices), converts to stereo float and pushes into the output engine; a buffer is released (its tag becomes
   available through get_released and buffer_event fires) when its last frame has been handed to the engine, which is
   when the device is about to play it.  Up to 32 buffers may be queued, as on the console. */
#include "internal.h"
#include <math.h>
#include <pthread/qos.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_QUEUED 32
#define SLICE_MS 5

typedef struct {
    uint64_t tag;
    const uint8_t *data;
    uint64_t frames, done;
} Queued;

struct napple_audout {
    napple_audio_host host;
    pthread_mutex_t lock;
    pthread_t thread;
    _Atomic int running;
    uint32_t rate, channels;
    _Atomic uint32_t state;
    _Atomic int manual;
    Queued q[MAX_QUEUED];
    unsigned qhead, qcount;
    uint64_t released[MAX_QUEUED];
    unsigned rcount;
    nae_source *out;
};

/* Convert `frames` PCM16 frames (c channels) to interleaved stereo float. 5.1 order: FL FR C LFE RL RR. */
static void to_stereo(const int16_t *in, uint32_t c, uint32_t frames, float *lr) {
    for (uint32_t k = 0; k < frames; k++) {
        const int16_t *f = in + (size_t)k * c;
        float l, r;
        if (c == 1) l = r = f[0] / 32768.f;
        else if (c == 2) {
            l = f[0] / 32768.f;
            r = f[1] / 32768.f;
        } else {
            float cc = f[2] / 32768.f * 0.7071f;
            l = f[0] / 32768.f + cc + f[4] / 32768.f * 0.7071f;
            r = f[1] / 32768.f + cc + f[5] / 32768.f * 0.7071f;
        }
        lr[2 * k] = l > 1 ? 1 : l < -1 ? -1 : l;
        lr[2 * k + 1] = r > 1 ? 1 : r < -1 ? -1 : r;
    }
}

static void consume(napple_audout *o, uint32_t frames) {
    float lr[2 * 2048];
    uint32_t produced = 0;
    int released = 0;
    pthread_mutex_lock(&o->lock);
    while (frames && produced < 2048) {
        uint32_t want = frames < 2048 - produced ? frames : 2048 - produced;
        if (!o->qcount) {
            memset(lr + 2 * produced, 0, (size_t)want * 2 * sizeof(float));
            produced += want;
            frames -= want;
            break;
        }
        Queued *b = &o->q[o->qhead];
        uint64_t left = b->frames - b->done;
        uint32_t n = left < want ? (uint32_t)left : want;
        to_stereo((const int16_t *)(const void *)(b->data + b->done * o->channels * 2), o->channels, n, lr + 2 * produced);
        b->done += n;
        produced += n;
        frames -= n;
        if (b->done >= b->frames) {
            if (o->rcount < MAX_QUEUED) o->released[o->rcount++] = b->tag;
            o->qhead = (o->qhead + 1) % MAX_QUEUED;
            o->qcount--;
            released = 1;
        }
    }
    pthread_mutex_unlock(&o->lock);
    nae_source_push(o->out, lr, (int)produced);
    if (released && o->host.buffer_event) o->host.buffer_event(o->host.user);
}

static void *pacer(void *p) {
    napple_audout *o = p;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const uint32_t slice = o->rate * SLICE_MS / 1000;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (atomic_load_explicit(&o->running, memory_order_acquire)) {
        if (atomic_load(&o->state) == 0 && !atomic_load(&o->manual)) consume(o, slice);
        next.tv_nsec += (long)nae_scaled_ns(SLICE_MS * 1000000LL);
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t wait = (int64_t)(next.tv_sec - now.tv_sec) * 1000000000LL + (next.tv_nsec - now.tv_nsec);
        if (wait > 0) {
            struct timespec d = {wait / 1000000000LL, wait % 1000000000LL};
            nanosleep(&d, NULL);
        } else if (wait < -100000000LL) next = now;
    }
    return NULL;
}

int napple_audout_open(uint32_t rate, uint32_t channels, const napple_audio_host *host, napple_audout **out, uint32_t *granted_rate,
                       uint32_t *granted_channels) {
    if (!out) return NAPPLE_AUDIO_E_INVALID;
    if (!rate) rate = 48000;
    if (!channels) channels = 2;
    /* the console's audout accepts 48 kHz (and converts others); we resample everything in the engine */
    if (rate < 8000 || rate > 96000 || (channels != 1 && channels != 2 && channels != 6)) return NAPPLE_AUDIO_E_INVALID;
    napple_audout *o = calloc(1, sizeof *o);
    if (!o) return NAPPLE_AUDIO_E_NOMEM;
    if (host) o->host = *host;
    o->rate = rate;
    o->channels = channels;
    atomic_store(&o->state, 1);
    pthread_mutex_init(&o->lock, NULL);
    o->out = nae_source_create((int)rate);
    if (!o->out) {
        pthread_mutex_destroy(&o->lock);
        free(o);
        return NAPPLE_AUDIO_E_STATE;
    }
    atomic_store(&o->running, 1);
    if (pthread_create(&o->thread, NULL, pacer, o)) {
        nae_source_destroy(o->out);
        free(o);
        return NAPPLE_AUDIO_E_NOMEM;
    }
    if (granted_rate) *granted_rate = rate;
    if (granted_channels) *granted_channels = channels;
    *out = o;
    return NAPPLE_AUDIO_OK;
}

void napple_audout_close(napple_audout *o) {
    if (!o) return;
    atomic_store(&o->running, 0);
    pthread_join(o->thread, NULL);
    nae_source_destroy(o->out);
    pthread_mutex_destroy(&o->lock);
    free(o);
}

int napple_audout_start(napple_audout *o) {
    if (!o) return NAPPLE_AUDIO_E_INVALID;
    atomic_store(&o->state, 0);
    nae_source_set_active(o->out, 1);
    return 0;
}
int napple_audout_stop(napple_audout *o) {
    if (!o) return NAPPLE_AUDIO_E_INVALID;
    atomic_store(&o->state, 1);
    nae_source_set_active(o->out, 0);
    return 0;
}
uint32_t napple_audout_state(const napple_audout *o) { return atomic_load(&o->state); }

int napple_audout_append(napple_audout *o, uint64_t tag, uint64_t addr, uint64_t size, uint64_t offset) {
    if (!o || size > (1u << 28) || offset > (1u << 28)) return NAPPLE_AUDIO_E_INVALID;
    const uint8_t *data = napple_audio_guest(&o->host, addr + offset, size);
    if (!data && size) return NAPPLE_AUDIO_E_INVALID;
    pthread_mutex_lock(&o->lock);
    if (o->qcount >= MAX_QUEUED) {
        pthread_mutex_unlock(&o->lock);
        return NAPPLE_AUDIO_E_STATE;
    }
    Queued *b = &o->q[(o->qhead + o->qcount) % MAX_QUEUED];
    *b = (Queued){tag, data, size / (2ull * o->channels), 0};
    o->qcount++;
    pthread_mutex_unlock(&o->lock);
    return 0;
}

uint32_t napple_audout_get_released(napple_audout *o, uint64_t *tags, uint32_t max) {
    pthread_mutex_lock(&o->lock);
    uint32_t n = o->rcount < max ? o->rcount : max;
    if (tags) memcpy(tags, o->released, n * sizeof *tags);
    memmove(o->released, o->released + n, (o->rcount - n) * sizeof *o->released);
    o->rcount -= n;
    pthread_mutex_unlock(&o->lock);
    return n;
}

int napple_audout_contains(const napple_audout *o, uint64_t tag) {
    napple_audout *m = (napple_audout *)(uintptr_t)o;
    pthread_mutex_lock(&m->lock);
    int found = 0;
    for (unsigned i = 0; i < o->qcount; i++)
        if (o->q[(o->qhead + i) % MAX_QUEUED].tag == tag) found = 1;
    pthread_mutex_unlock(&m->lock);
    return found;
}

void napple_audout_set_manual(napple_audout *o, int manual) { atomic_store(&o->manual, manual != 0); }
void napple_audout_pump(napple_audout *o, uint32_t frames) {
    while (frames) {
        uint32_t n = frames > 2048 ? 2048 : frames;
        consume(o, n);
        frames -= n;
    }
}

/* Test/diagnostic hook: latest frames handed to the output engine. */
int audout_peek(napple_audout *o, float *lr, int frames) { return nae_source_peek(o->out, lr, frames); }
