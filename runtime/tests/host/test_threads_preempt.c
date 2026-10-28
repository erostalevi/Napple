/* Host unit test (kernel-threads, 2026-10-27): timer preemption of a guest thread that spins in user code and never reaches an svc.
 *   P1  a better-priority thread waiting for the spinner's core gets it (the SMO stall: prio 44 waiting for a prio 47 spinner on core 0)
 *   P2  equal-priority spinners on one core both run (10 ms slices)
 *   P3  a spinner inside a preemption-disabled section (TLR disable_count) is not switched; it is once the count is back to 0 and it calls
 *       SynchronizePreemptionState
 *   P4  a worse-priority waiter does not take the core of a spinner
 * Real host threads are adopted as guest threads; "guest code" is plain C here, so every pc counts as preemptible. */
#include "napple.h"
#include "kthread.h"
#include "host_stubs.h"
#include "../check.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>

void svc_kernel_register(void);
void threads_preempt_test_any_pc(int on);
static uint64_t svc(unsigned num, uint64_t *r) { svc_dispatch(num, r, "test", 0x40); return r[0]; }
static uint64_t svcv(unsigned num, uint64_t a, uint64_t b, uint64_t c, uint64_t d) { uint64_t r[9] = {a, b, c, d}; return svc(num, r); }
#define CUR HANDLE_CURRENT_THREAD
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void ms(int n) { usleep((useconds_t)n * 1000); }

static uint8_t tlr_main[0x200];
typedef struct { int prio; volatile int ready, stop; volatile uint64_t spins; uint8_t tlr[0x200] __attribute__((aligned(16))); } Spinner;
static void *spinner(void *p) {
    Spinner *s = p;
    host_thread_attach(s->tlr);
    svcv(0x0D, CUR, (uint64_t)s->prio, 0, 0);
    svcv(0x0F, CUR, (uint64_t)-1, 1, 0); /* pinned to core 0 */
    s->ready = 1;
    while (!s->stop) s->spins++;         /* user-mode spin: no svc */
    svcv(0x0A, 0, 0, 0, 0);
    return NULL;
}
static void start(Spinner *s, pthread_t *pt, int prio) {
    memset((void *)s, 0, sizeof *s);
    s->prio = prio;
    pthread_create(pt, NULL, spinner, s);
    while (!s->ready) ms(1);
    ms(20);
}
static void finish(Spinner *s, pthread_t pt) {
    s->stop = 1;
    pthread_join(pt, NULL);
}

static volatile int got;
static uint8_t tlr_w[0x200] __attribute__((aligned(16)));
static void *waiter(void *p) {
    (void)p;
    host_thread_attach(tlr_w);
    svcv(0x0D, CUR, 44, 0, 0);
    svcv(0x0F, CUR, (uint64_t)-1, 1, 0);
    got = 1;
    svcv(0x0F, CUR, (uint64_t)-1, 4, 0);
    svcv(0x0A, 0, 0, 0, 0);
    return NULL;
}

int main(void) {
    svc_kernel_register();
    svc_threads_register();
    host_thread_attach(tlr_main);
    threads_preempt_test_any_pc(1);
    svcv(0x0D, CUR, 44, 0, 0);
    svcv(0x0F, CUR, (uint64_t)-1, 4, 0); /* main on core 2 */

    /* P1 */
    {
        Spinner s;
        pthread_t pt;
        start(&s, &pt, 47);
        uint64_t t0 = now_ns();
        CHECK(svcv(0x0F, CUR, (uint64_t)-1, 1, 0) == 0, "main moves to core 0, which the prio-47 spinner holds");
        uint64_t dt = (now_ns() - t0) / 1000000;
        CHECK(dt < 500, "main got core 0 from the spinner within %llu ms", (unsigned long long)dt);
        svcv(0x0F, CUR, (uint64_t)-1, 4, 0);
        finish(&s, pt);
    }
    /* P4 */
    {
        Spinner s, w;
        pthread_t pt, pw;
        start(&s, &pt, 40);
        uint64_t before = s.spins;
        memset((void *)&w, 0, sizeof w);
        w.prio = 50;
        pthread_create(&pw, NULL, spinner, &w);
        ms(300);
        CHECK(!w.ready, "a worse-priority thread does not take the spinner's core");
        CHECK(s.spins > before, "the better spinner kept running");
        s.stop = 1;
        pthread_join(pt, NULL);
        while (!w.ready) ms(1);
        w.stop = 1;
        pthread_join(pw, NULL);
    }
    /* P2 */
    {
        Spinner a, b;
        pthread_t pa, pb;
        start(&a, &pa, 47);
        memset((void *)&b, 0, sizeof b);
        b.prio = 47;
        pthread_create(&pb, NULL, spinner, &b);
        ms(400);
        CHECK(b.ready && b.spins > 1000 && a.spins > 1000, "equal-priority spinners share the core (a %llu, b %llu)", (unsigned long long)a.spins, (unsigned long long)b.spins);
        a.stop = b.stop = 1;
        pthread_join(pa, NULL);
        pthread_join(pb, NULL);
    }
    /* P3 */
    {
        Spinner s;
        pthread_t pt;
        start(&s, &pt, 47);
        volatile uint16_t *dc = (volatile uint16_t *)(s.tlr + 0x100);
        *dc = 1; /* the spinner is in a preemption-disabled section */
        pthread_t pw;
        got = 0;
        pthread_create(&pw, NULL, waiter, NULL);
        ms(300);
        CHECK(!got, "a spinner with disable_count > 0 keeps its core");
        *dc = 0;
        ms(300);
        CHECK(got, "the waiter gets the core once the disabled section ended");
        pthread_join(pw, NULL);
        finish(&s, pt);
    }
    printf("{\"test\":\"threads_preempt\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
