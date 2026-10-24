/* Gate dispatch: every rewritten `svc` and counter read in the guest lands here (via gate.S) with the registers saved.
 * The gate is identified by its return point inside the module's patch area; the tables file maps it to the original
 * instruction (svc number or the register a counter read targets). */
#include "napple.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t ret; const Module *module; const Gate *gate; } GateEntry;
static GateEntry *gate_index;
static unsigned ngate_index;

static int by_ret(const void *a, const void *b) {
    uint64_t x = ((const GateEntry *)a)->ret, y = ((const GateEntry *)b)->ret;
    return x < y ? -1 : x > y;
}
void dispatch_build_index(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < g_nmodules; i++) n += g_modules[i].ngate;
    gate_index = calloc(n ? n : 1, sizeof *gate_index);
    for (unsigned i = 0; i < g_nmodules; i++)
        for (unsigned j = 0; j < g_modules[i].ngate; j++) {
            const Gate *g = &g_modules[i].gates[j];
            gate_index[ngate_index++] = (GateEntry){(uintptr_t)g_modules[i].base + g->ret, &g_modules[i], g};
        }
    qsort(gate_index, ngate_index, sizeof *gate_index, by_ret);
    LOG("loader", "%u svc / counter gates indexed", ngate_index);
}

void runtime_dispatch(uint64_t ret, uint64_t *regs) {
    unsigned lo = 0, hi = ngate_index;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (gate_index[mid].ret < ret) lo = mid + 1; else hi = mid;
    }
    if (lo == ngate_index || gate_index[lo].ret != ret) runtime_fail("gate entered from unknown return address %#llx", (unsigned long long)ret);
    const GateEntry *e = &gate_index[lo];
    if (e->gate->timer) {          /* mrs xN, cntpct_el0 / cntvct_el0 / cntfrq_el0: Horizon's 19.2 MHz system counter */
        if (e->gate->arg >= 31 || e->gate->arg == 18) runtime_fail("timer gate with register %u", e->gate->arg);
        regs[e->gate->arg] = e->gate->timer == 2 ? 19200000ull : runtime_guest_ticks();
        return;
    }
    svc_dispatch(e->gate->arg, regs, e->module->name, e->gate->original);
}
