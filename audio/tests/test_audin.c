/* audin: stream parameters, silence without a capture source, sample-exact capture from a source (mono -> channels, gain, clamping, shortfalls),
   buffer queue semantics (order, limit, contains, flush), the pacing thread, and the guest write hooks.  No device. */
#include "wire.h"
#include <unistd.h>

typedef struct {
    uint64_t next;
    uint32_t rate_seen, calls;
    uint32_t limit;
    float amp;
} Ramp; /* mono ramp: sample n = n * amp */
static uint32_t ramp_read(void *user, float *mono, uint32_t frames, uint32_t rate) {
    Ramp *r = user;
    r->calls++;
    r->rate_seen = rate;
    uint32_t n = r->limit && r->limit < frames ? r->limit : frames;
    for (uint32_t k = 0; k < n; k++) mono[k] = (float)(r->next++) * r->amp;
    return n;
}

static void t_parameters(void) {
    napple_audin *in;
    uint32_t rate = 1, ch = 1;
    CHECK(napple_audin_open(0, 0, NULL, &in, &rate, &ch) == 0 && rate == 48000 && ch == 2, "defaults 48000 Hz stereo (got %u/%u)", rate, ch);
    CHECK(napple_audin_state(in) == 1, "created stopped");
    napple_audin_start(in);
    CHECK(napple_audin_state(in) == 0, "started");
    napple_audin_stop(in);
    CHECK(napple_audin_state(in) == 1, "stopped");
    CHECK(napple_audin_get_gain(in) == 1.f, "default gain 1");
    napple_audin_set_gain(in, 5.f);
    CHECK(napple_audin_get_gain(in) == 2.f, "gain clamped to 2");
    napple_audin_set_gain(in, -1.f);
    CHECK(napple_audin_get_gain(in) == 0.f, "negative gain clamped to 0");
    napple_audin_close(in);
    CHECK(napple_audin_open(16000, 1, NULL, &in, &rate, &ch) == 0 && rate == 16000 && ch == 1, "16 kHz mono accepted");
    napple_audin_close(in);
    CHECK(napple_audin_open(48000, 6, NULL, &in, &rate, &ch) == NAPPLE_AUDIO_E_INVALID, "6 channels rejected");
    CHECK(napple_audin_open(4000, 1, NULL, &in, &rate, &ch) == NAPPLE_AUDIO_E_INVALID, "4 kHz rejected");
    CHECK(napple_audin_open(48000, 2, NULL, NULL, &rate, &ch) == NAPPLE_AUDIO_E_INVALID, "NULL out rejected");
    char name[64];
    size_t n = napple_audin_device_name(name, sizeof name);
    CHECK(n == strlen(NAPPLE_AUDIO_INPUT_NAME) && !strcmp(name, "BuiltInHeadset"), "device name %s", name);
    n = napple_audin_device_name(name, 5);
    CHECK(n == 4 && name[4] == 0, "name truncated safely");
}

static void t_silence_without_source(void) {
    napple_audin_set_source(NULL, NULL);
    CHECK(!napple_audin_has_source(), "no source installed");
    static int16_t a[2 * 480], b[2 * 240];
    memset(a, 0x55, sizeof a);
    memset(b, 0x55, sizeof b);
    napple_audin *in;
    napple_audin_open(48000, 2, NULL, &in, NULL, NULL);
    napple_audin_set_manual(in, 1);
    CHECK(napple_audin_append(in, 0xA, (uint64_t)(uintptr_t)a, sizeof a, 0) == 0 && napple_audin_append(in, 0xB, (uint64_t)(uintptr_t)b, sizeof b, 0) == 0,
          "append");
    CHECK(napple_audin_contains(in, 0xA) && napple_audin_contains(in, 0xB) && !napple_audin_contains(in, 0xC) && napple_audin_buffer_count(in) == 2,
          "contains / count");
    uint64_t tags[8];
    napple_audin_pump(in, 100);
    CHECK(napple_audin_get_released(in, tags, 8) == 0, "a partly filled buffer is not released");
    napple_audin_pump(in, 380);
    CHECK(napple_audin_get_released(in, tags, 8) == 1 && tags[0] == 0xA && !napple_audin_contains(in, 0xA), "first buffer released when full");
    int zero = 1;
    for (unsigned k = 0; k < sizeof a / 2; k++) zero &= a[k] == 0;
    CHECK(zero, "first buffer is silence (no capture source)");
    int untouched = 1;
    for (unsigned k = 0; k < sizeof b / 2; k++) untouched &= b[k] == 0x5555;
    CHECK(untouched, "second buffer was written before its turn");
    napple_audin_pump(in, 1000); /* b, then nothing queued: must not fault */
    CHECK(napple_audin_get_released(in, tags, 8) == 1 && tags[0] == 0xB, "second buffer released");
    zero = 1;
    for (unsigned k = 0; k < sizeof b / 2; k++) zero &= b[k] == 0;
    CHECK(zero, "second buffer is silence");
    napple_audin_stats st;
    napple_audin_get_stats(in, &st);
    CHECK(st.source_frames == 0 && st.silent_frames == st.frames && st.frames == 480 + 240 + 760 && st.shortfalls == 0 && st.queued == 0 && st.released == 0,
          "stats: frames %llu silent %llu source %llu shortfalls %llu", (unsigned long long)st.frames, (unsigned long long)st.silent_frames,
          (unsigned long long)st.source_frames, (unsigned long long)st.shortfalls);
    napple_audin_close(in);
}

static void t_capture_from_source(void) {
    static int16_t a[2 * 300], b[2 * 300], m[300];
    Ramp ramp = {.amp = 1.f / 40000.f};
    napple_audin_set_source(ramp_read, &ramp);
    CHECK(napple_audin_has_source(), "source installed");
    napple_audin *in;
    napple_audin_open(16000, 2, NULL, &in, NULL, NULL);
    napple_audin_set_manual(in, 1);
    napple_audin_append(in, 1, (uint64_t)(uintptr_t)a, sizeof a, 0);
    napple_audin_append(in, 2, (uint64_t)(uintptr_t)b, sizeof b, 0);
    napple_audin_pump(in, 450); /* crosses the buffer boundary mid-pump: the ramp must be continuous */
    napple_audin_pump(in, 150);
    CHECK(ramp.rate_seen == 16000, "source asked for the stream rate (%u)", ramp.rate_seen);
    int bad = 0;
    for (int k = 0; k < 600; k++) {
        const int16_t *p = k < 300 ? &a[2 * k] : &b[2 * (k - 300)];
        int want = (int)lrintf((float)k / 40000.f * 32768.f);
        if (p[0] != want || p[1] != want) bad++;
    }
    CHECK(bad == 0, "%d of 600 frames differ from the source ramp (mono must be copied to both channels, in order)", bad);
    uint64_t tags[4];
    CHECK(napple_audin_get_released(in, tags, 4) == 2 && tags[0] == 1 && tags[1] == 2, "released in append order");
    napple_audin_close(in);

    /* gain, clamping and a source that runs dry */
    Ramp loud = {.amp = 1.f / 100.f, .limit = 60};
    napple_audin_set_source(ramp_read, &loud);
    napple_audin_open(48000, 1, NULL, &in, NULL, NULL);
    napple_audin_set_manual(in, 1);
    napple_audin_set_gain(in, 2.f);
    napple_audin_append(in, 7, (uint64_t)(uintptr_t)m, sizeof m, 0);
    napple_audin_pump(in, 300);
    int clipped_ok = 1;
    for (int k = 0; k < 60; k++) {
        float x = (float)k / 100.f * 2.f;
        int want = x >= 1.f ? 32767 : (int)lrintf(x * 32768.f);
        clipped_ok &= m[k] == want;
    }
    CHECK(clipped_ok && m[59] == 32767, "gain 2 with clipping at full scale (sample 59 = %d)", m[59]);
    int tail_silent = 1;
    for (int k = 60; k < 300; k++) tail_silent &= m[k] == 0;
    napple_audin_stats st;
    napple_audin_get_stats(in, &st);
    CHECK(tail_silent && st.source_frames == 60 && st.silent_frames == 240 && st.shortfalls >= 1,
          "short source: silence after sample 60 (source %llu silent %llu shortfalls %llu)", (unsigned long long)st.source_frames,
          (unsigned long long)st.silent_frames, (unsigned long long)st.shortfalls);
    napple_audin_close(in);

    /* removing the source returns to silence on the next buffer */
    napple_audin_set_source(NULL, NULL);
    napple_audin_open(48000, 1, NULL, &in, NULL, NULL);
    napple_audin_set_manual(in, 1);
    memset(m, 0x7f, sizeof m);
    napple_audin_append(in, 8, (uint64_t)(uintptr_t)m, sizeof m, 0);
    napple_audin_pump(in, 300);
    int z = 1;
    for (int k = 0; k < 300; k++) z &= m[k] == 0;
    CHECK(z, "silence again after the source was removed");
    napple_audin_close(in);
}

static int g_events;
static size_t g_prepared;
static void on_buf(void *u) {
    (void)u;
    __atomic_add_fetch(&g_events, 1, __ATOMIC_SEQ_CST);
}
static void on_prepare(void *u, void *p, size_t n) {
    (void)u;
    (void)p;
    __atomic_add_fetch(&g_prepared, n, __ATOMIC_SEQ_CST);
}
static void *translate_plus(void *u, uint64_t addr, uint64_t size) {
    (void)size;
    return (void *)(uintptr_t)(addr + *(uint64_t *)u);
}

static void t_queue_flush_and_hooks(void) {
    static int16_t pool[4096];
    uint64_t bias = 16; /* guest address = host address - bias */
    napple_audio_host h = {.user = &bias, .translate = translate_plus, .write_prepare = on_prepare, .buffer_event = on_buf};
    h.translate = translate_plus;
    napple_audin_set_source(NULL, NULL);
    napple_audin *in;
    napple_audin_open(48000, 1, &h, &in, NULL, NULL);
    napple_audin_set_manual(in, 1);
    memset(pool, 0x33, sizeof pool);
    /* offset and translate: data lands at translate(addr + offset) */
    CHECK(napple_audin_append(in, 1, (uint64_t)(uintptr_t)pool - bias, 200, 64) == 0, "append with offset through translate");
    napple_audin_pump(in, 100);
    int ok = 1;
    for (int k = 0; k < 32; k++) ok &= pool[k] == 0x3333;
    for (int k = 32; k < 132; k++) ok &= pool[k] == 0;
    for (int k = 132; k < 200; k++) ok &= pool[k] == 0x3333;
    CHECK(ok, "capture wrote exactly [offset, offset + size) of the translated buffer");
    CHECK(g_prepared == 200 && g_events == 1, "write_prepare covered %zu bytes, %d buffer events", g_prepared, g_events);
    uint64_t tags[40];
    CHECK(napple_audin_get_released(in, tags, 40) == 1 && tags[0] == 1, "released");
    g_prepared = 0;
    g_events = 0;
    /* limit of 32 queued buffers */
    int accepted = 0;
    for (int i = 0; i < 40; i++) accepted += napple_audin_append(in, 100 + i, (uint64_t)(uintptr_t)pool - bias, 64, 0) == 0;
    CHECK(accepted == 32 && napple_audin_buffer_count(in) == 32, "queue holds 32 buffers (accepted %d)", accepted);
    CHECK(napple_audin_append(in, 999, (uint64_t)(uintptr_t)pool - bias, 64, 0) == NAPPLE_AUDIO_E_STATE, "33rd buffer refused with E_STATE");
    napple_audin_pump(in, 16); /* part of the first */
    CHECK(napple_audin_flush(in) == 1, "flush with queued buffers");
    CHECK(napple_audin_buffer_count(in) == 32 && napple_audin_get_released(in, tags, 40) == 32, "flush released all 32 in order");
    CHECK(tags[0] == 100 && tags[31] == 131, "flush kept the order (%llu..%llu)", (unsigned long long)tags[0], (unsigned long long)tags[31]);
    CHECK(napple_audin_flush(in) == 0 && napple_audin_buffer_count(in) == 0, "flush with nothing queued");
    /* released but not collected buffers count against the limit, so no released tag is ever lost */
    for (int i = 0; i < 32; i++) napple_audin_append(in, 200 + i, (uint64_t)(uintptr_t)pool - bias, 64, 0);
    napple_audin_pump(in, 32 * 32);
    CHECK(napple_audin_buffer_count(in) == 32, "32 released, none collected (count %u)", napple_audin_buffer_count(in));
    CHECK(napple_audin_append(in, 300, (uint64_t)(uintptr_t)pool - bias, 64, 0) == NAPPLE_AUDIO_E_STATE, "append refused while 32 released are uncollected");
    CHECK(napple_audin_get_released(in, tags, 1) == 1 && tags[0] == 200, "collect one");
    CHECK(napple_audin_append(in, 300, (uint64_t)(uintptr_t)pool - bias, 64, 0) == 0, "a collected slot can be reused");
    napple_audin_pump(in, 32);
    uint32_t got = napple_audin_get_released(in, tags, 40);
    CHECK(got == 32 && tags[0] == 201 && tags[30] == 231 && tags[31] == 300, "every released tag kept, in order (%u, last %llu)", got,
          (unsigned long long)tags[got ? got - 1 : 0]);
    CHECK(g_events >= 1, "flush signalled the buffer event");
    /* invalid appends */
    CHECK(napple_audin_append(in, 5, 0, 64, 0) == NAPPLE_AUDIO_E_INVALID, "unmapped address refused");
    CHECK(napple_audin_append(in, 5, 1, 1ull << 30, 0) == NAPPLE_AUDIO_E_INVALID, "oversized buffer refused");
    napple_audin_close(in);
}

/* The pacing thread: nothing happens while stopped, a started stream fills and releases buffers in real time. */
static void t_pacing_thread(void) {
    static int16_t buf[2 * 480];
    memset(buf, 0x44, sizeof buf);
    napple_audin_set_source(NULL, NULL);
    g_events = 0;
    napple_audio_host h = {.buffer_event = on_buf};
    napple_audin *in;
    napple_audin_open(48000, 2, &h, &in, NULL, NULL);
    napple_audin_append(in, 1, (uint64_t)(uintptr_t)buf, sizeof buf, 0);
    usleep(60000);
    uint64_t tags[2];
    CHECK(napple_audin_get_released(in, tags, 2) == 0 && buf[0] == 0x4444, "a stopped stream captured");
    napple_audin_start(in);
    int released = 0;
    for (int i = 0; i < 100 && !released; i++) {
        usleep(5000);
        released = (int)napple_audin_get_released(in, tags, 2);
    }
    CHECK(released == 1 && tags[0] == 1 && buf[0] == 0 && buf[2 * 479 + 1] == 0 && __atomic_load_n(&g_events, __ATOMIC_SEQ_CST) >= 1,
          "10 ms buffer was not filled and released by the pacing thread");
    napple_audin_stop(in);
    napple_audin_close(in);
}

int main(void) {
    napple_audio_config c = {.use_device = 0};
    napple_audio_init(&c);
    t_parameters();
    t_silence_without_source();
    t_capture_from_source();
    t_queue_flush_and_hooks();
    t_pacing_thread();
    napple_audio_shutdown();
    printf("{\"test\":\"audin\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
