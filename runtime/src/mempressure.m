/* mempressure.m - reacts to memory pressure so an app stays under the system's per-process limit (docs/architecture/memory-swap.md, "Eviction policy").
 *
 * Signals: the kernel's memory-pressure dispatch source (warn / critical), the UIKit memory warning (frontend_ios.m calls napple_memory_warning), and a poll of
 * how much the system still allows (os_proc_available_memory on iOS; elsewhere NAPPLE_MEM_BUDGET_MB pretends a limit and reports budget - phys_footprint, which
 * is how the logic is exercised on a Mac). Actions, cheapest first: write back and deactivate swap-backed guest pages (swap.c), drop the Metal ring chunks that
 * are only kept for reuse (gfx_mtl_trim). On by default on iOS; NAPPLE_MEMPRESSURE=1 elsewhere, =0 off. */
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#include "napple.h"
#include "swap.h"
#if TARGET_OS_IOS
#include <os/proc.h>
#endif

extern void gfx_mtl_trim(unsigned level) __attribute__((weak_import)); /* gfx/src/mtl_core.m; absent with the stub back end */

#define MB(x) ((unsigned long long)(x) >> 20)

static _Atomic unsigned long g_events[3];
static unsigned warn_mb = 800, crit_mb = 400; /* available-memory thresholds: below warn_mb trim lightly, below crit_mb trim everything */

static void handle(unsigned level, const char *why) {
    g_events[level]++;
    if (level) LOG("mempressure", "level %u (%s): trimming", level, why);
    if (swap_enabled()) swap_trim(level);
    if (level && gfx_mtl_trim) gfx_mtl_trim(level);
}

void napple_memory_warning(void) { handle(2, "UIKit memory warning"); }

static uint64_t available_mb(void) {
    const char *b = getenv("NAPPLE_MEM_BUDGET_MB");
    if (b && atoi(b) > 0) {
        task_vm_info_data_t vi;
        mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS) return UINT64_MAX;
        uint64_t used = MB(vi.phys_footprint), budget = (uint64_t)atoi(b);
        return used < budget ? budget - used : 0;
    }
#if TARGET_OS_SIMULATOR
    return UINT64_MAX; /* the Simulator has no per-process limit: os_proc_available_memory() reports 0 there */
#elif TARGET_OS_IOS
    return MB(os_proc_available_memory());
#else
    return UINT64_MAX;
#endif
}

static void *poll_thread(void *arg) {
    (void)arg;
    unsigned idle = 0, since = 100; /* seconds since the last trim: a full-file sync each second would cost more than it saves */
    for (;;) {
        usleep(1000 * 1000);
        since++;
        uint64_t a = available_mb();
        if (a != UINT64_MAX && a < crit_mb && since >= 5) since = 0, handle(2, "available memory critical");
        else if (a != UINT64_MAX && a < warn_mb && a >= crit_mb && since >= 2) since = 0, handle(1, "available memory low");
        else if (a != UINT64_MAX && a < warn_mb) continue;
        else if (swap_enabled() && ++idle % 5 == 0) swap_trim(0); /* every few seconds: write back a slice so dirty pages are already clean when pressure comes */
    }
    return NULL;
}

void mempressure_start(void) {
    const char *e = getenv("NAPPLE_MEMPRESSURE");
    int on = e ? atoi(e) != 0 : TARGET_OS_IOS;
    if (!on) return;
    if (getenv("NAPPLE_MEM_WARN_MB")) warn_mb = (unsigned)atoi(getenv("NAPPLE_MEM_WARN_MB"));
    if (getenv("NAPPLE_MEM_CRIT_MB")) crit_mb = (unsigned)atoi(getenv("NAPPLE_MEM_CRIT_MB"));
    dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0, DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
                                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (s) {
        dispatch_source_set_event_handler(s, ^{
            handle(dispatch_source_get_data(s) & DISPATCH_MEMORYPRESSURE_CRITICAL ? 2 : 1, "kernel memory pressure");
        });
        dispatch_resume(s); /* the source is kept alive by the runtime for the life of the process */
    }
    pthread_t t;
    if (!pthread_create(&t, NULL, poll_thread, NULL)) pthread_detach(t);
    LOG("mempressure", "on: trim below %u MB available, everything below %u MB (swap %s)", warn_mb, crit_mb, swap_enabled() ? "on" : "off");
}

void mempressure_counts(unsigned long out[3]) {
    for (int i = 0; i < 3; i++) out[i] = g_events[i];
}
