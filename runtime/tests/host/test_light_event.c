/* Host unit test (acnh-race-atomics, 2026-11-01): the SDK's auto-clear nn::os::LightEvent driven through svcWaitForAddress / svcSignalToAddress, in the
 * handshake of ACNH's job manager (main kicks the workers, then joins them by waiting on each worker's done event).
 * The event code below follows the SDK of ACNH 1.0.0 instruction for instruction (sdk+0xca500 SignalLightEventImpl, sdk+0xca690 WaitLightEventImpl):
 * word 0 = not signaled, 1 = a thread waits, 2 = signaled. Signal: 2 -> nothing; 1 -> SignalToAddress(ModifyByWaiterCountIfEqual, 1, count 1) (the
 * woken waiter consumes the signal); 0 -> CAS to 2. Wait: 2 -> CAS to 0 and return; 0 -> CAS to 1, then WaitForAddress(WaitIfEqual, 1); 1 -> wait;
 * InvalidState (0xfa01) -> retry; a successful wait returns WITHOUT re-reading the word, so one spurious success is an early join.
 *   L1  3 workers pinned to cores 0..2, main kicks and joins them N times: at every join each worker has finished that round (no early wake)
 *   L2  the same with two waiters on one event and a count-1 signal: exactly one waiter leaves per signal (no double wake, no lost wake) */
#include "napple.h"
#include "kthread.h"
#include "host_stubs.h"
#include "../check.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

void svc_kernel_register(void);
static uint64_t svc(unsigned num, uint64_t *r) { svc_dispatch(num, r, "test", 0x40); return r[0]; }
static uint64_t svcv(unsigned num, uint64_t a, uint64_t b, uint64_t c, uint64_t d) { uint64_t r[9] = {a, b, c, d}; return svc(num, r); }
#define CUR HANDLE_CURRENT_THREAD
#define INVALID_STATE 0xfa01u

typedef struct { _Atomic uint32_t w; } LightEvent; /* auto-clear */
static void le_signal(LightEvent *e) {
    for (;;) {
        uint32_t s = atomic_load_explicit(&e->w, memory_order_seq_cst);
        if (s == 2) return;
        if (s == 1) {
            uint32_t r = (uint32_t)svcv(0x35, (uintptr_t)&e->w, 2, 1, 1);
            if ((r & 0x3fffff) == INVALID_STATE) continue;
            return;
        }
        uint32_t z = 0;
        if (atomic_compare_exchange_strong_explicit(&e->w, &z, 2, memory_order_release, memory_order_relaxed)) return;
    }
}
static void le_wait(LightEvent *e) {
    for (;;) {
        uint32_t s = atomic_load_explicit(&e->w, memory_order_acquire);
        if (s == 2) {
            uint32_t two = 2;
            if (atomic_compare_exchange_strong_explicit(&e->w, &two, 0, memory_order_acq_rel, memory_order_relaxed)) return;
            continue;
        }
        if (s == 0) {
            uint32_t z = 0;
            if (!atomic_compare_exchange_strong_explicit(&e->w, &z, 1, memory_order_acq_rel, memory_order_relaxed)) continue;
        }
        uint32_t r = (uint32_t)svcv(0x34, (uintptr_t)&e->w, 2, 1, (uint64_t)-1);
        if ((r & 0x3fffff) == INVALID_STATE) continue;
        if (r == 0) return;
        CHECK(0, "WaitForAddress result %#x", r);
        return;
    }
}

#define WORKERS 3
#define ROUNDS 20000
static LightEvent kick[WORKERS], done_ev[WORKERS];
static _Atomic int round_done[WORKERS];
static _Atomic int early;
typedef struct { int idx; uint8_t tlr[0x200] __attribute__((aligned(16))); } W;
static void *worker(void *p) {
    W *w = p;
    host_thread_attach(w->tlr);
    svcv(0x0D, CUR, 44, 0, 0);
    svcv(0x0F, CUR, (uint64_t)-1, 1ull << w->idx, 0);
    for (int r = 1; r <= ROUNDS; r++) {
        le_wait(&kick[w->idx]);
        for (volatile int spin = 0; spin < (r * 7 + w->idx * 13) % 400; spin++) {} /* uneven work */
        atomic_store(&round_done[w->idx], r);
        le_signal(&done_ev[w->idx]);
    }
    svcv(0x0A, 0, 0, 0, 0);
    return NULL;
}

static LightEvent shared;
static _Atomic int passed;
static uint8_t tlr_a[0x200] __attribute__((aligned(16))), tlr_b[0x200] __attribute__((aligned(16)));
static void *waiter2(void *p) {
    host_thread_attach(p);
    svcv(0x0D, CUR, 44, 0, 0);
    for (;;) {
        le_wait(&shared);
        if (atomic_fetch_add(&passed, 1) >= 2 * 5000 - 1) break;
    }
    svcv(0x0A, 0, 0, 0, 0);
    return NULL;
}

static uint8_t tlr_main[0x200] __attribute__((aligned(16)));
int main(void) {
    svc_kernel_register();
    svc_threads_register();
    host_thread_attach(tlr_main);
    /* L1 */
    {
        static W ws[WORKERS];
        pthread_t pt[WORKERS];
        for (int i = 0; i < WORKERS; i++) {
            ws[i].idx = i;
            pthread_create(&pt[i], NULL, worker, &ws[i]);
        }
        svcv(0x0D, CUR, 44, 0, 0);
        for (int r = 1; r <= ROUNDS; r++) {
            for (int i = 0; i < WORKERS; i++) le_signal(&kick[i]);
            for (int i = 0; i < WORKERS; i++) {
                le_wait(&done_ev[i]);
                if (atomic_load(&round_done[i]) != r) atomic_fetch_add(&early, 1);
            }
        }
        for (int i = 0; i < WORKERS; i++) pthread_join(pt[i], NULL);
        CHECK(early == 0, "every join came after its worker finished the round (%d early of %d)", early, ROUNDS * WORKERS);
    }
    /* L2 */
    {
        pthread_t a, b;
        pthread_create(&a, NULL, waiter2, tlr_a);
        pthread_create(&b, NULL, waiter2, tlr_b);
        int sent = 0;
        while (atomic_load(&passed) < 2 * 5000) {
            int before = atomic_load(&passed);
            le_signal(&shared);
            sent++;
            for (int k = 0; k < 200000 && atomic_load(&passed) == before; k++) sched_yield();
            if (atomic_load(&passed) > sent) break; /* a double wake: more passes than signals */
        }
        CHECK(atomic_load(&passed) <= sent, "never more passes than signals (%d passes, %d signals)", atomic_load(&passed), sent);
        le_signal(&shared); /* the waiter still parked after the last pass */
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        CHECK(sent >= 2 * 5000, "every signal let exactly one waiter through (%d signals)", sent);
    }
    printf("{\"test\":\"light_event\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
