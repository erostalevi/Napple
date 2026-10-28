/* audin capture sources selected by a spec string (env NAPPLE_AUDIN_SOURCE): silence, tone:HZ[:AMP], file:PATH (looped WAV or raw s16le mono
   48 kHz) and mic (the default input device, audin_mic.c).  Installed with napple_audin_set_source; the pacing threads of open streams pull
   from it in 5 ms slices.  Portable C; only the device capture of `mic` is Apple-specific. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE_BYTES (64u << 20)
#define RING_FRAMES 16384u /* power of two: mic ring, 48 kHz mono float (~340 ms) */

/* ---- linear resampling pull over a mono float buffer ---- */
typedef struct {
    float *pcm;
    size_t frames;
    double rate; /* source sample rate */
    double pos;  /* fractional read position in source frames */
} FileSrc;

static uint32_t file_read(void *user, float *mono, uint32_t frames, uint32_t rate) {
    FileSrc *f = user;
    if (!f->frames || !rate) return 0;
    const double step = f->rate / rate;
    for (uint32_t k = 0; k < frames; k++) {
        size_t i = (size_t)f->pos;
        const double fr = f->pos - (double)i;
        const float a = f->pcm[i % f->frames], b = f->pcm[(i + 1) % f->frames];
        mono[k] = (float)(a + (b - a) * fr);
        f->pos += step;
        if (f->pos >= (double)f->frames) f->pos -= (double)f->frames;
    }
    return frames;
}

/* WAV: RIFF/WAVE, fmt (PCM16 or IEEE float32, 1 or 2 channels), data. Returns a malloc'd mono float buffer. */
static float *wav_decode(const uint8_t *d, size_t n, size_t *frames, double *rate) {
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return NULL;
    uint32_t fmt_tag = 0, ch = 0, sr = 0, bits = 0;
    const uint8_t *data = NULL;
    size_t dlen = 0;
    for (size_t o = 12; o + 8 <= n;) {
        uint32_t len;
        memcpy(&len, d + o + 4, 4);
        const uint8_t *body = d + o + 8;
        size_t avail = n - (o + 8);
        if (!memcmp(d + o, "fmt ", 4) && len >= 16 && avail >= 16) {
            uint16_t t, c, b;
            memcpy(&t, body, 2);
            memcpy(&c, body + 2, 2);
            memcpy(&sr, body + 4, 4);
            memcpy(&b, body + 14, 2);
            fmt_tag = t, ch = c, bits = b;
        } else if (!memcmp(d + o, "data", 4)) {
            data = body;
            dlen = len < avail ? len : avail;
            break;
        }
        o += 8 + (size_t)len + (len & 1);
    }
    const int pcm16 = fmt_tag == 1 && bits == 16, f32 = fmt_tag == 3 && bits == 32;
    if (!data || (ch != 1 && ch != 2) || !(pcm16 || f32) || sr < 8000 || sr > 192000) return NULL;
    const size_t bps = bits / 8, nf = dlen / (bps * ch);
    if (!nf) return NULL;
    float *out = malloc(nf * sizeof *out);
    if (!out) return NULL;
    for (size_t i = 0; i < nf; i++) {
        float acc = 0;
        for (uint32_t c = 0; c < ch; c++) {
            const uint8_t *p = data + (i * ch + c) * bps;
            if (pcm16) {
                int16_t s;
                memcpy(&s, p, 2);
                acc += (float)s / 32768.f;
            } else {
                float s;
                memcpy(&s, p, 4);
                acc += isfinite(s) ? s : 0.f;
            }
        }
        out[i] = acc / (float)ch;
    }
    *frames = nf;
    *rate = sr;
    return out;
}

static FileSrc *file_load(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || (unsigned long)sz > MAX_FILE_BYTES) {
        fclose(fp);
        return NULL;
    }
    uint8_t *raw = malloc((size_t)sz);
    if (!raw || fread(raw, 1, (size_t)sz, fp) != (size_t)sz) {
        free(raw);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    FileSrc *f = calloc(1, sizeof *f);
    if (f) {
        f->pcm = wav_decode(raw, (size_t)sz, &f->frames, &f->rate);
        if (!f->pcm) { /* raw s16le mono 48 kHz */
            f->frames = (size_t)sz / 2;
            f->rate = 48000;
            f->pcm = f->frames ? malloc(f->frames * sizeof(float)) : NULL;
            for (size_t i = 0; f->pcm && i < f->frames; i++) {
                int16_t s;
                memcpy(&s, raw + i * 2, 2);
                f->pcm[i] = (float)s / 32768.f;
            }
        }
        if (!f->pcm) {
            free(f);
            f = NULL;
        }
    }
    free(raw);
    return f;
}

/* ---- tone ---- */
typedef struct {
    double phase, hz, amp;
} Tone;
static uint32_t tone_read(void *user, float *mono, uint32_t frames, uint32_t rate) {
    Tone *t = user;
    if (!rate) return 0;
    const double inc = 2.0 * M_PI * t->hz / rate;
    for (uint32_t k = 0; k < frames; k++) {
        mono[k] = (float)(t->amp * sin(t->phase));
        t->phase += inc;
        if (t->phase > 2.0 * M_PI) t->phase -= 2.0 * M_PI;
    }
    return frames;
}

/* ---- mic ring: device thread pushes 48 kHz mono float, the pacing thread pulls resampled ---- */
static struct {
    float buf[RING_FRAMES];
    _Atomic uint64_t w, r;
    double pos;
} M;
void nae_mic_ring_push(const float *mono, uint32_t frames) {
    uint64_t w = atomic_load_explicit(&M.w, memory_order_relaxed);
    for (uint32_t k = 0; k < frames; k++) M.buf[(w + k) & (RING_FRAMES - 1)] = mono[k];
    atomic_store_explicit(&M.w, w + frames, memory_order_release);
}
void nae_mic_ring_reset(void) {
    atomic_store(&M.r, atomic_load(&M.w));
    M.pos = 0;
}
uint32_t nae_mic_read(void *user, float *mono, uint32_t frames, uint32_t rate) {
    (void)user;
    if (!rate) return 0;
    uint64_t w = atomic_load_explicit(&M.w, memory_order_acquire), r = atomic_load_explicit(&M.r, memory_order_relaxed);
    if (w - r > RING_FRAMES - 64) r = w - (RING_FRAMES / 2); /* the reader fell behind: drop the oldest */
    const double step = 48000.0 / rate;
    uint32_t k = 0;
    for (; k < frames; k++) {
        const uint64_t i = (uint64_t)M.pos;
        if (r + i + 1 >= w) break;
        const double fr = M.pos - (double)i;
        const float a = M.buf[(r + i) & (RING_FRAMES - 1)], b = M.buf[(r + i + 1) & (RING_FRAMES - 1)];
        mono[k] = (float)(a + (b - a) * fr);
        M.pos += step;
    }
    const uint64_t whole = (uint64_t)M.pos;
    r += whole;
    M.pos -= (double)whole;
    atomic_store_explicit(&M.r, r, memory_order_relaxed);
    return k;
}

/* ---- installation ---- */
static struct {
    Tone tone;
    FileSrc *file;
    int mic;
} I;

static void uninstall(void) {
    napple_audin_set_source(NULL, NULL);
    if (I.mic) nae_mic_stop();
    I.mic = 0;
    if (I.file) {
        free(I.file->pcm);
        free(I.file);
        I.file = NULL;
    }
}

static napple_audin_permission_fn g_perm;
static void *g_perm_user;
void napple_audin_set_permission_hook(napple_audin_permission_fn fn, void *user) {
    g_perm = fn;
    g_perm_user = user;
}

int napple_audin_install_source_spec(const char *spec) {
    if (!spec || !*spec || !strcmp(spec, "silence")) {
        uninstall();
        return NAPPLE_AUDIO_OK;
    }
    if (!strncmp(spec, "tone:", 5) || !strcmp(spec, "tone")) {
        char *end = NULL;
        double hz = spec[4] ? strtod(spec + 5, &end) : 440.0, amp = 0.25;
        if (end && *end == ':') amp = strtod(end + 1, &end);
        if (!(hz > 0 && hz < 24000) || !(amp >= 0 && amp <= 1) || (end && *end)) return NAPPLE_AUDIO_E_INVALID;
        uninstall();
        I.tone = (Tone){0, hz, amp};
        napple_audin_set_source(tone_read, &I.tone);
        return NAPPLE_AUDIO_OK;
    }
    if (!strncmp(spec, "file:", 5)) {
        FileSrc *f = file_load(spec + 5);
        if (!f) return NAPPLE_AUDIO_E_INVALID;
        uninstall();
        I.file = f;
        napple_audin_set_source(file_read, f);
        return NAPPLE_AUDIO_OK;
    }
    if (!strcmp(spec, "mic")) {
        uninstall();
        if (g_perm && !g_perm(g_perm_user)) return NAPPLE_AUDIO_E_DENIED;
        nae_mic_ring_reset();
        if (nae_mic_start()) return NAPPLE_AUDIO_E_UNSUPPORTED;
        I.mic = 1;
        napple_audin_set_source(nae_mic_read, NULL);
        return NAPPLE_AUDIO_OK;
    }
    return NAPPLE_AUDIO_E_INVALID;
}

int napple_audin_install_default_source(void) {
    if (napple_audin_has_source()) return NAPPLE_AUDIO_OK;
    const char *spec = getenv("NAPPLE_AUDIN_SOURCE");
    if (!spec || !*spec) return NAPPLE_AUDIO_OK; /* silence: a headset with no audio */
    int rc = napple_audin_install_source_spec(spec);
    if (rc) napple_audio_log(NULL, "NAPPLE_AUDIN_SOURCE=%s not usable (%d): audin records silence", spec, rc);
    else napple_audio_log(NULL, "audin source: %s", spec);
    return rc;
}
