/* timeline.c - the submission timeline: sequence numbers, completion waits, retirement of resources the GPU may still use.
 *
 * Every unit of queue work (a submission, a present) gets a sequence number when it is accepted. A sequence completes when the GPU
 * finished it (Metal completion handler). NVN syncs wait on sequences; window images wait on a separate per-image counter. */
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "internal.h"
#include "timeline.h"

static _Atomic uint64_t g_submitted, g_completed;
static pthread_mutex_t g_tl_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tl_cv = PTHREAD_COND_INITIALIZER;

typedef struct retire_item { void (*fn)(void *); void *arg; uint64_t after; struct retire_item *next; } retire_item;
static retire_item *g_retire_head, *g_retire_tail;

uint64_t gfx_timeline_alloc(void) { return atomic_fetch_add(&g_submitted, 1) + 1; }
uint64_t gfx_timeline_last(void) { return atomic_load(&g_submitted); }
uint64_t gfx_completed(void) { return atomic_load(&g_completed); }

static void run_retired(uint64_t done) {
    for (;;) {
        retire_item *it = NULL;
        pthread_mutex_lock(&g_tl_mu);
        if (g_retire_head && g_retire_head->after <= done) {
            it = g_retire_head;
            g_retire_head = it->next;
            if (!g_retire_head) g_retire_tail = NULL;
        }
        pthread_mutex_unlock(&g_tl_mu);
        if (!it) return;
        it->fn(it->arg);
        free(it);
    }
}

void gfx_timeline_complete(uint64_t seq) {
    uint64_t cur = atomic_load(&g_completed);
    while (seq > cur && !atomic_compare_exchange_weak(&g_completed, &cur, seq)) {}
    run_retired(atomic_load(&g_completed));
    gfx_wake();
}

void gfx_wake(void) {
    pthread_mutex_lock(&g_tl_mu);
    pthread_cond_broadcast(&g_tl_cv);
    pthread_mutex_unlock(&g_tl_mu);
}

static int wait_pred(const _Atomic uint64_t *cell, uint64_t need, uint64_t timeout_ns) {
    if (atomic_load(cell) >= need) return 1;
    if (timeout_ns == 0) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t end_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + timeout_ns;
    if (timeout_ns == UINT64_MAX || end_ns < timeout_ns) end_ns = UINT64_MAX;
    int ok = 0;
    pthread_mutex_lock(&g_tl_mu);
    while (!(ok = atomic_load(cell) >= need)) {
        struct timespec until;
        if (end_ns == UINT64_MAX) {
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 1;                       /* wake once a second even without a signal (lost-wakeup safety) */
        } else {
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            if (now >= end_ns) break;
            uint64_t slice = end_ns - now > 50000000ull ? 50000000ull : end_ns - now;
            until.tv_sec = ts.tv_sec + (time_t)((ts.tv_nsec + slice) / 1000000000ull);
            until.tv_nsec = (long)((ts.tv_nsec + slice) % 1000000000ull);
        }
        pthread_cond_timedwait(&g_tl_cv, &g_tl_mu, &until);
    }
    ok = atomic_load(cell) >= need;
    pthread_mutex_unlock(&g_tl_mu);
    return ok;
}

int gfx_wait_cell(const _Atomic uint64_t *cell, uint64_t need, uint64_t timeout_ns) { return wait_pred(cell, need, timeout_ns); }
int gfx_wait_seq(uint64_t seq, uint64_t timeout_ns) { return wait_pred(&g_completed, seq, timeout_ns); }

void gfx_retire(void (*fn)(void *), void *arg) {
    uint64_t after = atomic_load(&g_submitted);
    if (atomic_load(&g_completed) >= after) { fn(arg); return; }
    retire_item *it = malloc(sizeof *it);
    if (!it) { fn(arg); return; }
    *it = (retire_item){ fn, arg, after, NULL };
    pthread_mutex_lock(&g_tl_mu);
    if (g_retire_tail) g_retire_tail->next = it; else g_retire_head = it;
    g_retire_tail = it;
    pthread_mutex_unlock(&g_tl_mu);
    /* completion may have raced with the insertion */
    run_retired(atomic_load(&g_completed));
}
