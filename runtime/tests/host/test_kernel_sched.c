/* Host unit test (nx-kernel, 2026-10-26): scheduling rows of the nxnative review against the virtual-core model of threads.c.
 *   K3  waking / starting a better thread pinned to the caller's core runs it before the caller's svc returns (same-core handoff)
 *   K4  yield: returns at once when nobody waits, hands the core to an equal-priority waiter
 *   K5  SleepThread precision (macOS nanosleep overshoots ~10%)
 *   K6  svcGetInfo FreeThreadCount = 96 minus the live threads
 *   K8  SetThreadCoreMask with ideal core -3 moves the ideal core into the new mask
 * Real host threads are adopted as guest threads (host_thread_attach) and call the svc handlers through svc_dispatch. */
#include "napple.h"
#include "kthread.h"
#include "host_stubs.h"
#include "../check.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>

void svc_kernel_register(void);
static uint64_t svc(unsigned num, uint64_t *r) { svc_dispatch(num, r, "test", 0x40); return r[0]; }
static uint64_t svcv(unsigned num, uint64_t a, uint64_t b, uint64_t c, uint64_t d) { uint64_t r[9] = {a, b, c, d}; return svc(num, r); }
#define CUR HANDLE_CURRENT_THREAD
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void ms(int n) { usleep((useconds_t)n * 1000); }

static uint8_t tlr_main[0x200];
static uint32_t word_a __attribute__((aligned(4))), word_b __attribute__((aligned(4)));
static volatile int done, ready;

/* Worker: runs on core 0 at `prio`, waits on word_a, then does 5-20 ms of "work" holding its core, then waits on word_b for good. */
typedef struct { int prio, work_ms; uint8_t tlr[0x200]; } Worker;
static void *worker(void *p) {
    Worker *w = p;
    host_thread_attach(w->tlr);
    svcv(0x0D, CUR, (uint64_t)w->prio, 0, 0);
    svcv(0x0F, CUR, (uint64_t)-1, 1, 0);                 /* pinned to core 0 (waits for it when main holds it) */
    ready = 1;
    svcv(0x34, (uintptr_t)&word_a, 2, 0, (uint64_t)-1);  /* wait if equal 0 */
    ms(w->work_ms); done = 1;
    svcv(0x34, (uintptr_t)&word_b, 2, 0, (uint64_t)-1);
    return NULL;
}
static void run_handoff(int prio, int work_ms, int expect_done_at_return, const char *what) {
    Worker w = {.prio = prio, .work_ms = work_ms};
    word_a = 0; word_b = 0; done = 0; ready = 0;
    svcv(0x0F, CUR, (uint64_t)-1, 4, 0);                 /* main on core 2 while the worker sets itself up on core 0 */
    pthread_t pt; pthread_create(&pt, NULL, worker, &w);
    while (!ready) ms(1);
    ms(50);                                              /* the worker is blocked in WaitForAddress */
    svcv(0x0F, CUR, (uint64_t)-1, 1, 0);                 /* main takes core 0: the worker is pinned to the caller's core */
    CHECK(svcv(0x35, (uintptr_t)&word_a, 0, 0, 1) == 0, "SignalToAddress");
    CHECK(done == expect_done_at_return, what);
    ms(work_ms + 60);
    svcv(0x0B, 0, 0, 0, 0);                              /* a yield lets an equal-priority waiter have the core (it blocks again before main gets it back) */
    CHECK(done == 1, "the worker finished its work in the end");
    pthread_detach(pt);
    ms(30);
}

static void *yielder_peer(void *p) {
    host_thread_attach(p);
    svcv(0x0F, CUR, (uint64_t)-1, 1, 0);                 /* core 0, waits for main to yield it */
    done = 1;
    svcv(0x0A, 0, 0, 0, 0);                              /* ExitThread gives the core back */
    return NULL;
}

int main(void) {
    svc_kernel_register(); svc_threads_register();
    host_thread_attach(tlr_main);

    /* K6: FreeThreadCount */
    { uint64_t r[9] = {0, 24, 0, 0}; CHECK(svc(0x29, r) == 0 && r[1] == 95, "GetInfo FreeThreadCount: 96 minus the one live thread"); }

    /* K8: ideal core -3 */
    CHECK(svcv(0x0F, CUR, 0, 7, 0) == 0, "ideal 0, mask 7");
    CHECK(svcv(0x0F, CUR, (uint64_t)-3, 6, 0) == 0, "ideal -3 with a mask excluding the ideal core succeeds");
    { uint64_t r[9] = {0, 0, CUR}; r[2] = CUR; svc(0x0E, r); CHECK(r[0] == 0 && (int)r[1] == 1 && r[2] == 6, "the ideal core moved into the mask (lowest set bit)"); }
    CHECK(svcv(0x0F, CUR, (uint64_t)-3, 7, 0) == 0, "ideal -3 with a mask containing the ideal core");
    { uint64_t r[9] = {0, 0, CUR}; svc(0x0E, r); CHECK((int)r[1] == 1 && r[2] == 7, "ideal core unchanged"); }
    CHECK(svcv(0x0F, CUR, (uint64_t)-3, 0, 0) == R_INVALID_COMBINATION, "empty mask");
    CHECK(svcv(0x0F, CUR, 0, 6, 0) == R_INVALID_COMBINATION, "explicit ideal core outside the mask");
    CHECK(svcv(0x0F, CUR, (uint64_t)-3, 0x18, 0) == R_INVALID_PROCESSOR_ID, "mask bits beyond the application cores");
    CHECK(svcv(0x0F, CUR, 5, 7, 0) == R_INVALID_PROCESSOR_ID, "ideal core out of range");
    CHECK(svcv(0x0F, CUR, (uint64_t)-1, 4, 0) == 0, "no ideal core, core 2 only: main now runs on core 2");
    { uint64_t r[9] = {0}; svc(0x10, r); CHECK(r[0] == 2, "GetCurrentProcessorNumber follows the mask"); }

    /* K3: same-core handoff; main holds core 2 while the worker waits (they never share a core) */
    CHECK(svcv(0x0F, CUR, (uint64_t)-1, 1, 0) == 0, "main to core 0");
    CHECK(svcv(0x0D, CUR, 44, 0, 0) == 0, "main priority 44");
    run_handoff(20, 5, 1, "K3: a better thread woken on the caller's core has finished its work before SignalToAddress returns");
    /* equal priority: the caller keeps the core, the woken thread waits for it */
    run_handoff(44, 20, 0, "K3: an equal-priority thread woken on the caller's core does not run first");

    /* K4: yield */
    { uint64_t t0 = now_ns(); for (int i = 0; i < 20000; i++) svcv(0x0B, 0, 0, 0, 0); uint64_t dt = now_ns() - t0;
      CHECK(dt < 500000000ull, "K4: 20000 yields with no waiter return at once (no back-off sleeps)"); printf("# 20000 yields: %.1f ms\n", dt / 1e6); }
    done = 0; { pthread_t pt; static uint8_t tl[0x200]; pthread_create(&pt, NULL, yielder_peer, tl); pthread_detach(pt); }
    ms(60);
    CHECK(done == 0, "K4: the equal-priority peer waits for core 0 while main runs");
    svcv(0x0B, 0, 0, 0, 0);
    ms(1);
    CHECK(done == 1, "K4: yield hands the core to the equal-priority waiter");

    /* K5: SleepThread precision */
    { int n = 15; uint64_t sum = 0, worst = 0;
      for (int i = 0; i < n; i++) { uint64_t t0 = now_ns(); svcv(0x0B, 33000000, 0, 0, 0); uint64_t dt = now_ns() - t0; sum += dt; if (dt > worst) worst = dt; }
      printf("# SleepThread(33 ms): mean %.3f ms, worst %.3f ms\n", sum / (double)n / 1e6, worst / 1e6);
      CHECK(sum / n >= 32900000ull && sum / n < 35000000ull, "K5: SleepThread(33 ms) averages under 35 ms (the plain nanosleep path measured 36.4 ms on macOS)"); }

    printf("{\"test\":\"kernel_sched\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
