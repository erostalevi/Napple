/* litmus_excl_claim.c: ACNH 1.0.0's parallel-for item claim (main+0x19b65d4..0x19b6638) on the host CPU. Spinlock acquire with plain ldxr/stxr (no acquire),
 * claim [next, next+4) with plain loads/stores, release with dmb ish + ldxr/stxr 0; counts items claimed by two threads.
 * clang -O2 -o /tmp/claim tools/soak/litmus_excl_claim.c && /tmp/claim        (ldxr: thousands claimed twice on an Apple M5)
 * clang -O2 -DACQ -o /tmp/claim_acq tools/soak/litmus_excl_claim.c && /tmp/claim_acq   (ldaxr, what napple-rewrite emits: 0) */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ITEMS 4096
#define ROUNDS 20000
#define THREADS 3
typedef struct { uint32_t lock; uint32_t pad[7]; uint32_t total, next; uint32_t pad2[6]; } Job;
static Job job __attribute__((aligned(64)));
static _Atomic uint8_t owner[ITEMS];
static _Atomic long dup, claimed;
static _Atomic int bar_count, bar_gen;
static void barrier(void) { int g = atomic_load(&bar_gen); if (atomic_fetch_add(&bar_count, 1) == THREADS - 1) { atomic_store(&bar_count, 0); atomic_fetch_add(&bar_gen, 1); } else while (atomic_load(&bar_gen) == g) {} }
static inline void lock(uint32_t *l) {
    uint32_t v, st;
    for (;;) {
        while (__atomic_load_n(l, __ATOMIC_RELAXED) == 1) {}
#ifdef ACQ
        __asm__ volatile("ldaxr %w0, [%2]\n" : "=&r"(v) : "r"(1), "r"(l) : "memory");
#else
        __asm__ volatile("ldxr %w0, [%2]\n" : "=&r"(v) : "r"(1), "r"(l) : "memory");
#endif
        if (v) continue;
        __asm__ volatile("stxr %w0, %w1, [%2]\n" : "=&r"(st) : "r"(1), "r"(l) : "memory");
        if (!st) return;
    }
}
static inline void unlock(uint32_t *l) {
    uint32_t v, st;
    __asm__ volatile("dmb ish" ::: "memory");
    do {
        __asm__ volatile("ldxr %w0, [%1]\n" : "=&r"(v) : "r"(l) : "memory");
        __asm__ volatile("stxr %w0, wzr, [%1]\n" : "=&r"(st) : "r"(l) : "memory");
    } while (st);
}
static void *worker(void *arg) {
    long me = (long)arg + 1;
    for (int r = 0; r < ROUNDS; r++) {
        barrier();
        for (;;) {
            lock(&job.lock);
            /* the guest's claim: plain loads of total/next after the lock (ldp w9, w26, [x21, #0xc0]) */
            uint32_t total = *(volatile uint32_t *)&job.total, next = *(volatile uint32_t *)&job.next;
            uint32_t n = total - next > 4 ? 4 : total - next;
            *(volatile uint32_t *)&job.next = next + n;
            unlock(&job.lock);
            if (!n) break;
            for (uint32_t i = next; i < next + n; i++) {
                uint8_t z = 0;
                if (!atomic_compare_exchange_strong(&owner[i], &z, (uint8_t)me)) atomic_fetch_add(&dup, 1);
                atomic_fetch_add(&claimed, 1);
            }
        }
        barrier();
        if (me == 1) { memset((void *)owner, 0, sizeof owner); job.next = 0; job.total = 64 + (unsigned)(r * 37) % (ITEMS - 64); }
        barrier();
    }
    return NULL;
}
int main(void) {
    job.total = 1000;
    pthread_t t[THREADS];
    for (long i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    printf("%s: claimed %ld items, %ld claimed twice\n",
#ifdef ACQ
           "ldaxr",
#else
           "ldxr ",
#endif
           (long)claimed, (long)dup);
    return 0;
}
