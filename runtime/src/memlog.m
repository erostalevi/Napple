/* memlog.m - the runtime's memory log: process footprint (the figure iOS jetsam judges), the Metal device's allocations and, on iOS, what the system still allows.
 * On by default on iOS, NAPPLE_MEMLOG=1 elsewhere; NAPPLE_MEMLOG_SECS (default 20) is the period. One "memlog:" line per sample in the normal runtime log. */
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include "napple.h"
#if TARGET_OS_IOS
#include <os/proc.h>
#endif

extern uint64_t gfx_mtl_allocated_bytes(void); /* gfx/src/mtl_memstat.m */

extern uint64_t memory_used_bytes(void);       /* guest heap + alias + module images that are mapped */

#define MB(x) ((unsigned long long)(x) >> 20)

static void *memlog_thread(void *arg) {
    unsigned secs = (unsigned)(intptr_t)arg;
    for (;;) {
        sleep(secs);
        task_vm_info_data_t vi;
        mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS) continue;
        unsigned long long avail = 0;
#if TARGET_OS_IOS
        avail = (unsigned long long)os_proc_available_memory();
#endif
        LOG("memlog", "footprint %llu MB (peak %llu MB) guest-mapped %llu MB metal %llu MB resident %llu MB", MB(vi.phys_footprint), MB(vi.ledger_phys_footprint_peak), MB(memory_used_bytes()),
            MB(gfx_mtl_allocated_bytes()), MB(vi.resident_size));
        if (avail) LOG("memlog", "system still allows %llu MB before the limit", MB(avail));
    }
    return NULL;
}

void memlog_start(void) {
    int on = TARGET_OS_IOS || getenv("NAPPLE_MEMLOG");
    if (!on) return;
    const char *e = getenv("NAPPLE_MEMLOG_SECS");
    unsigned secs = e && atoi(e) > 0 ? (unsigned)atoi(e) : 20;
    pthread_t t;
    if (!pthread_create(&t, NULL, memlog_thread, (void *)(intptr_t)secs)) pthread_detach(t);
}
