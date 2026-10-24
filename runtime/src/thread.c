/* Per-thread guest environment: the Darwin TSD slots the rewritten code reads (x18 stand-in, TLS, TLR, gate), a signal
 * stack so faults are handled off a possibly tiny guest stack, and the Horizon ThreadLocalRegion (IPC message buffer
 * in the first 0x100 bytes, ThreadType pointer at +0x1f8). */
#include "napple.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>

pthread_key_t runtime_keys[8];
static pthread_key_t environment_key;
#define SIGNAL_STACK_SIZE (256u << 10)
typedef struct { void *tls, *tlr, *dummy, *signal_stack; } ThreadEnvironment;

static void environment_destroy(void *p) {
    ThreadEnvironment *e = p;
    if (!e) return;
    if (e->signal_stack) { stack_t off = {.ss_flags = SS_DISABLE}; sigaltstack(&off, NULL); free(e->signal_stack); }
    free(e->dummy); free(e->tlr); free(e->tls); free(e);
}

void thread_env_init_keys(void) {
    for (unsigned i = 0; i < 8; i++)
        if (pthread_key_create(&runtime_keys[i], NULL) || runtime_keys[i] == 0) runtime_fail("cannot allocate Darwin TSD key %u", i);
    runtime_hle_stack_init((uint64_t)runtime_keys[4] * 8);
    if (pthread_key_create(&environment_key, environment_destroy)) runtime_fail("thread environment key");
}

void runtime_thread_init(void) {
    if (pthread_getspecific(environment_key)) runtime_fail("duplicate guest thread environment");
    ThreadEnvironment *e = calloc(1, sizeof *e);
    if (!e) runtime_fail("thread environment");
    e->tls = calloc(1, 8192); e->tlr = calloc(1, 0x200); e->dummy = calloc(1, 0x400);
    e->signal_stack = malloc(SIGNAL_STACK_SIZE);
    if (!e->tls || !e->tlr || !e->dummy || !e->signal_stack) runtime_fail("thread environment allocation");
    stack_t ss = {.ss_sp = e->signal_stack, .ss_size = SIGNAL_STACK_SIZE, .ss_flags = 0};
    if (sigaltstack(&ss, NULL)) runtime_fail("cannot install signal stack");
    memcpy((uint8_t *)e->tlr + 0x1f8, &e->dummy, 8);
    if (pthread_setspecific(environment_key, e) || pthread_setspecific(runtime_keys[0], NULL) ||
        pthread_setspecific(runtime_keys[1], e->tls) || pthread_setspecific(runtime_keys[2], e->tlr) ||
        pthread_setspecific(runtime_keys[3], (void *)runtime_gate))
        runtime_fail("thread slots");
    uintptr_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    if (tp & 7) runtime_fail("raw Darwin TSD base is not 8-byte aligned (rewritten code indexes it directly)");
    for (unsigned i = 0; i < 4; i++) {
        void *raw; memcpy(&raw, (void *)(tp + runtime_keys[i] * 8), 8);
        if (raw != pthread_getspecific(runtime_keys[i])) runtime_fail("Darwin TSD layout differs from the raw slot assumption (key %u)", i);
    }
}

void runtime_thread_cleanup(void) {
    ThreadEnvironment *e = pthread_getspecific(environment_key);
    pthread_setspecific(environment_key, NULL);
    for (unsigned i = 0; i < 4; i++) pthread_setspecific(runtime_keys[i], NULL);
    environment_destroy(e);
}
