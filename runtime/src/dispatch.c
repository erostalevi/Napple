/* Gate dispatch: every rewritten `svc` and counter read in the guest lands here (via gate.S) with the registers saved.
 * The gate is identified by its return point inside the module's patch area; the tables file maps it to the original
 * instruction (svc number or the register a counter read targets).
 * The index is read without a lock on every svc of every thread; modules loaded later (NROs, ro.c) publish a new index with one atomic
 * pointer store. Replaced indexes are kept (a reader may still be searching one): a few KiB per NRO load. */
#include "napple.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t ret; const Module *module; const Gate *gate; } GateEntry;
typedef struct { unsigned n; GateEntry e[]; } GateIndex;
static GateIndex *gate_index;
static pthread_mutex_t index_lock = PTHREAD_MUTEX_INITIALIZER;

static int by_ret(const void *a, const void *b) {
    uint64_t x = ((const GateEntry *)a)->ret, y = ((const GateEntry *)b)->ret;
    return x < y ? -1 : x > y;
}
/* Builds the index over every live module (index_lock held) and publishes it. */
static void rebuild(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < g_nmodules; i++) if (!g_modules[i].gone) n += g_modules[i].ngate;
    GateIndex *x = calloc(1, sizeof *x + (n ? n : 1) * sizeof(GateEntry));
    if (!x) runtime_fail("gate index allocation");
    for (unsigned i = 0; i < g_nmodules; i++) {
        if (g_modules[i].gone) continue;
        for (unsigned j = 0; j < g_modules[i].ngate; j++) {
            const Gate *g = &g_modules[i].gates[j];
            x->e[x->n++] = (GateEntry){(uintptr_t)g_modules[i].base + g->ret, &g_modules[i], g};
        }
    }
    qsort(x->e, x->n, sizeof *x->e, by_ret);
    __atomic_store_n(&gate_index, x, __ATOMIC_RELEASE);
}
void dispatch_build_index(void) {
    pthread_mutex_lock(&index_lock); rebuild(); pthread_mutex_unlock(&index_lock);
    LOG("loader", "%u svc / counter gates indexed", gate_index->n);
}
/* A module was loaded or unloaded after start (module.c). */
void dispatch_modules_changed(void) { pthread_mutex_lock(&index_lock); rebuild(); pthread_mutex_unlock(&index_lock); }

void runtime_dispatch(uint64_t ret, uint64_t *regs) {
    const GateIndex *x = __atomic_load_n(&gate_index, __ATOMIC_ACQUIRE);
    unsigned lo = 0, hi = x->n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (x->e[mid].ret < ret) lo = mid + 1; else hi = mid;
    }
    if (lo == x->n || x->e[lo].ret != ret) runtime_fail("gate entered from unknown return address %#llx", (unsigned long long)ret);
    const GateEntry *e = &x->e[lo];
    if (e->gate->timer) {          /* mrs xN, cntpct_el0 / cntvct_el0 / cntfrq_el0: Horizon's 19.2 MHz system counter */
        if (e->gate->arg >= 31 || e->gate->arg == 18) runtime_fail("timer gate with register %u", e->gate->arg);
        regs[e->gate->arg] = e->gate->timer == 2 ? 19200000ull : runtime_guest_ticks();
        return;
    }
    svc_dispatch(e->gate->arg, regs, e->module->name, e->gate->original);
}
