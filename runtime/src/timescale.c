/* timescale.c - see timescale.h. The guest clock is a piecewise linear function of the real clock:
 *     guest(real) = guest0 + (real - real0) * f
 * Changing f re-bases (real0, guest0) at the current instant so the guest clock never jumps or runs backwards. Readers take a lock-free
 * snapshot (sequence counter); writers are serialised by a mutex. */
#include "napple.h"
#include "timescale.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define Q 32 /* fixed point fraction bits of the factor */

static struct {
    _Atomic uint32_t seq;        /* odd while a writer updates */
    uint64_t real0, guest0, f_q; /* f_q = clock factor * 2^Q */
} C = {0, 0, 0, 1ull << Q};
static pthread_mutex_t W = PTHREAD_MUTEX_INITIALIZER;
static double g_speed = 1.0;
static int g_mode = TS_MODE_CLOCK_AND_VSYNC;
static _Atomic uint64_t g_vsync_q = 1ull << Q;
static void (*g_observer)(double, int);
static _Atomic uint64_t g_presents, g_vsyncs;
void timescale_count_present(void) {
    atomic_fetch_add(&g_presents, 1);
    timescale_trace('P');
}
void timescale_count_vsync(void) {
    atomic_fetch_add(&g_vsyncs, 1);
    timescale_trace('V');
}

static uint64_t guest_at(uint64_t real);
static _Atomic int g_fps_mode = FPS_MODE_GAME, g_fps_hz = 60;
void timescale_set_fps(int mode, int hz) {
    if (mode < FPS_MODE_GAME || mode > FPS_MODE_UNCAPPED) mode = FPS_MODE_GAME;
    atomic_store(&g_fps_hz, hz < 15 ? 15 : hz > 999 ? 999 : hz);
    if (atomic_exchange(&g_fps_mode, mode) != mode)
        LOG("time", "present rate: %s", mode == FPS_MODE_GAME ? "game pacing" : mode == FPS_MODE_DISPLAY ? "match display" : mode == FPS_MODE_CUSTOM ? "custom" : "uncapped");
}
int timescale_fps_mode(void) { return atomic_load(&g_fps_mode); }
uint64_t timescale_present_period_ns(int mode, int hz, double display_hz) {
    if (display_hz < 1) display_hz = 60;
    switch (mode) {
        case FPS_MODE_DISPLAY: return (uint64_t)(1e9 / display_hz);
        case FPS_MODE_CUSTOM: return hz > 0 ? (uint64_t)(1e9 / hz) : 0;
        default: return 0;
    }
}
uint64_t timescale_present_period_now_ns(double display_hz) { return timescale_present_period_ns(atomic_load(&g_fps_mode), atomic_load(&g_fps_hz), display_hz); }
uint64_t timescale_deadline_step(uint64_t *next, uint64_t period, uint64_t now) {
    *next += period;
    if (*next > now) return *next - now;
    if (now - *next > 8 * period) *next = now;
    return 0;
}

static FILE *g_trace;
static pthread_once_t g_trace_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_trace_mu = PTHREAD_MUTEX_INITIALIZER;
static void trace_open(void) {
    const char *p = getenv("NAPPLE_FRAME_TRACE");
    if (p && *p && (g_trace = fopen(p, "w"))) fputs("kind,real_ns,game_clock_ns\n", g_trace);
}
void timescale_trace(char kind) {
    pthread_once(&g_trace_once, trace_open);
    if (!g_trace) return;
    uint64_t r = runtime_now_ns(), g = guest_at(r);
    pthread_mutex_lock(&g_trace_mu);
    fprintf(g_trace, "%c,%llu,%llu\n", kind, (unsigned long long)r, (unsigned long long)g);
    fflush(g_trace);
    pthread_mutex_unlock(&g_trace_mu);
}

/* NAPPLE_TIME_LOG=<seconds>: every period log guest seconds per real second, presents and vsync ticks per real second (measurements of the time scale). */
static void *time_logger(void *arg) {
    double period = *(double *)arg;
    uint64_t r0 = runtime_now_ns(), g0 = timescale_guest_ns(), p0 = 0, v0 = 0;
    for (;;) {
        struct timespec ts = {(time_t)period, (long)((period - (time_t)period) * 1e9)};
        nanosleep(&ts, NULL);
        uint64_t r1 = runtime_now_ns(), g1 = timescale_guest_ns(), p1 = atomic_load(&g_presents), v1 = atomic_load(&g_vsyncs);
        double dr = (double)(r1 - r0) / 1e9;
        LOG("time", "guest %.3f s per real second (speed %.2fx) | presents %.1f/s | vsync ticks %.1f/s | guest elapsed %.2f s, real %.2f s",
            (double)(g1 - g0) / 1e9 / dr, g_speed, (double)(p1 - p0) / dr, (double)(v1 - v0) / dr, (double)g1 / 1e9, (double)r1 / 1e9);
        r0 = r1;
        g0 = g1;
        p0 = p1;
        v0 = v1;
    }
    return NULL;
}


static uint64_t guest_at(uint64_t real) {
    for (;;) {
        uint32_t s0 = atomic_load_explicit(&C.seq, memory_order_acquire);
        if (s0 & 1) continue;
        uint64_t r0 = C.real0, g0 = C.guest0, f = C.f_q;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&C.seq, memory_order_relaxed) != s0) continue;
        if (real < r0) return g0; /* never earlier than the base: a caller racing a re-base */
        return g0 + (uint64_t)(((__uint128_t)(real - r0) * f) >> Q);
    }
}
uint64_t timescale_guest_ns_at(uint64_t real_ns) { return guest_at(real_ns); }
uint64_t timescale_guest_ns(void) { return guest_at(runtime_now_ns()); }

static double clamp_speed(double s) { return !(s >= TS_MIN_SPEED) ? TS_MIN_SPEED : s > TS_MAX_SPEED ? TS_MAX_SPEED : s; }

void timescale_set(double speed, int mode) {
    speed = clamp_speed(speed);
    if (mode < 0 || mode > TS_MODE_CLOCK_ONLY) mode = TS_MODE_CLOCK_AND_VSYNC;
    pthread_mutex_lock(&W);
    double fc = mode == TS_MODE_VSYNC_ONLY ? 1.0 : speed, fv = mode == TS_MODE_CLOCK_ONLY ? 1.0 : speed;
    uint64_t now = runtime_now_ns(), g = guest_at(now);
    atomic_fetch_add_explicit(&C.seq, 1, memory_order_release); /* odd: readers retry */
    C.real0 = now;
    C.guest0 = g;
    C.f_q = (uint64_t)(fc * (double)(1ull << Q) + 0.5);
    atomic_fetch_add_explicit(&C.seq, 1, memory_order_release);
    atomic_store(&g_vsync_q, (uint64_t)(fv * (double)(1ull << Q) + 0.5));
    static int logger_started;
    static double logger_period;
    if (!logger_started && getenv("NAPPLE_TIME_LOG") && atof(getenv("NAPPLE_TIME_LOG")) > 0) {
        logger_started = 1;
        logger_period = atof(getenv("NAPPLE_TIME_LOG"));
        pthread_t t;
        if (!pthread_create(&t, NULL, time_logger, &logger_period)) pthread_detach(t);
    }
    int changed = speed != g_speed || mode != g_mode;
    g_speed = speed;
    g_mode = mode;
    void (*obs)(double, int) = g_observer;
    pthread_mutex_unlock(&W);
    if (changed) {
        LOG("time", "game speed %.2fx (%s)", speed,
            mode == TS_MODE_CLOCK_AND_VSYNC ? "game clock and vsync"
            : mode == TS_MODE_VSYNC_ONLY    ? "vsync only, clocks real"
                                            : "game clock only, vsync unchanged");
        if (obs) obs(speed, mode);
    }
}
double timescale_speed(void) { return g_speed; }
int timescale_mode(void) { return g_mode; }
double timescale_clock_factor(void) { return (double)C.f_q / (double)(1ull << Q); }
double timescale_vsync_factor(void) { return (double)atomic_load(&g_vsync_q) / (double)(1ull << Q); }
void timescale_set_observer(void (*fn)(double, int)) { g_observer = fn; }

int64_t timescale_real_wait_ns(int64_t guest_ns) {
    if (guest_ns <= 0) return guest_ns;
    uint64_t f = C.f_q;
    if (f == (1ull << Q)) return guest_ns;
    __uint128_t r = ((__uint128_t)guest_ns << Q) / f;
    return r ? (int64_t)r : 1;
}
uint64_t timescale_vsync_period_ns(uint64_t guest_period_ns) {
    uint64_t f = atomic_load(&g_vsync_q);
    uint64_t r = (uint64_t)(((__uint128_t)guest_period_ns << Q) / f);
    return r ? r : 1;
}
int64_t timescale_wall_skew_ns(void) {
    uint64_t now = runtime_now_ns();
    return (int64_t)(guest_at(now) - now);
}
