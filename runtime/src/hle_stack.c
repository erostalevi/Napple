/* Host functions handed to guest code run on the calling guest thread's host stack (hle_stack.S): the guest stack may be
 * tiny (games create threads with 4 KiB stacks) and Horizon services run on their own stacks anyway. Every host function
 * the guest can reach (import overrides, NVN entry points, callbacks) is wrapped by a fixed 16-byte thunk.
 *
 * Diagnostics: NAPPLE_HLE_TRACE=1 counts host calls, =2 also logs the first call of each. A summary is printed by
 * runtime_hle_trace_set(0). */
#include "napple.h"
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"

#define HLE_THUNKS 16384
#define HLE_THUNK_BYTES 16
extern char runtime_hle_thunks[];
void *runtime_hle_targets[HLE_THUNKS];
uint64_t runtime_hle_tsd_off;
int runtime_hle_trace; /* read by hle_stack.S on every wrapped call */
extern void runtime_unresolved_entry(void);
static const char *names[HLE_THUNKS];
static unsigned char is_trap[HLE_THUNKS];
static _Atomic uint64_t counts[HLE_THUNKS];
static _Atomic unsigned char seen[HLE_THUNKS];
static unsigned used;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void runtime_hle_stack_init(uint64_t tsd_offset) { runtime_hle_tsd_off = tsd_offset; }

/* Same host function -> same wrapper, so pointers the game compares stay consistent. */
void *runtime_hle_wrap_named(void *fn, const char *name) {
    if (!fn) return NULL;
    pthread_mutex_lock(&lock);
    unsigned i = 0;
    while (i < used && runtime_hle_targets[i] != fn) i++;
    if (i == used) {
        if (used == HLE_THUNKS) runtime_fail("host-stack wrapper table full");
        __atomic_store_n(&runtime_hle_targets[used++], fn, __ATOMIC_RELEASE);
    }
    if (!names[i] && name) names[i] = strdup(name);
    pthread_mutex_unlock(&lock);
    return runtime_hle_thunks + (size_t)i * HLE_THUNK_BYTES;
}

/* One wrapper per unresolved import (deduplicated by name), all entering runtime_unresolved_entry. */
void *runtime_hle_wrap_trap(const char *name) {
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < used; i++)
        if (is_trap[i] && !strcmp(names[i], name)) {
            pthread_mutex_unlock(&lock);
            return runtime_hle_thunks + (size_t)i * HLE_THUNK_BYTES;
        }
    if (used == HLE_THUNKS) runtime_fail("host-stack wrapper table full");
    unsigned i = used++;
    names[i] = strdup(name);
    is_trap[i] = 1;
    __atomic_store_n(&runtime_hle_targets[i], (void *)runtime_unresolved_entry, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock);
    return runtime_hle_thunks + (size_t)i * HLE_THUNK_BYTES;
}

static int demangle_name(const char *n, char *out, size_t cap) {
    typedef char *(*demangle_t)(const char *, char *, size_t *, int *);
    static demangle_t demangle;
    static int looked;
    if (!looked) {
        demangle = (demangle_t)dlsym(RTLD_DEFAULT, "__cxa_demangle");
        looked = 1;
    }
    int status = -1;
    char *d = demangle && n[0] == '_' && n[1] == 'Z' ? demangle(n, NULL, NULL, &status) : NULL;
    snprintf(out, cap, "%s", d && !status ? d : n);
    free(d);
    return 1;
}
static void name_of(unsigned i, char *out, size_t cap) {
    if (i < HLE_THUNKS && names[i]) {
        demangle_name(names[i], out, cap);
        return;
    }
    Dl_info info;
    void *fn = i < HLE_THUNKS ? runtime_hle_targets[i] : NULL;
    if (fn && dladdr(fn, &info) && info.dli_sname) snprintf(out, cap, "%s (host)", info.dli_sname);
    else snprintf(out, cap, "wrapper %u -> %p", i, fn);
}
void runtime_hle_describe(unsigned i, char *out, size_t cap) { name_of(i, out, cap); }

/* Reads the guest frame record of the host-stack switch that called us: {guest x29, guest x30}. */
static int safe_read(uint64_t addr, void *out, size_t n) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), addr, n, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}

/* A subsystem (gfx) can claim traps for the names it hands out: return non-zero when handled (it normally never returns). */
int (*runtime_trap_hook)(const char *name);

/* Called through runtime_unresolved_entry (hle_stack.S) with the wrapper index. */
_Noreturn void runtime_unresolved_call(uint64_t index) {
    char text[512];
    name_of((unsigned)index, text, sizeof text);
    if (runtime_trap_hook && names[index] && runtime_trap_hook(names[index])) runtime_fail("trap hook returned");
    uint64_t *frame = __builtin_frame_address(0), link[2] = {0, 0}, outer[2] = {0, 0};
    if (safe_read((uint64_t)(uintptr_t)frame, link, 16) && safe_read(link[0], outer, 16)) {
        char where[96];
        module_describe(outer[1], where, sizeof where);
        LOG("hle", "guest called unresolved import %s from %s", text, where);
        runtime_trace_guest(outer[0], outer[1]);
    }
    trace_event("unresolved_call", "\"symbol\":\"%s\"", names[index] ? names[index] : "?");
    summary_note("unresolved import called: %s", names[index] ? names[index] : "?");
    runtime_fail("guest called unresolved import %s", text);
}

/* hle_stack.S calls this before each wrapped call while tracing (on the host stack, arguments kept). */
void runtime_hle_note(uint64_t i) {
    if (i >= HLE_THUNKS) return;
    atomic_fetch_add_explicit(&counts[i], 1, memory_order_relaxed);
    if (runtime_hle_trace != 2 || atomic_exchange_explicit(&seen[i], 1, memory_order_relaxed)) return;
    char text[256];
    name_of((unsigned)i, text, sizeof text);
    LOG("hle", "first call of %s", text);
}

void runtime_hle_trace_set(int mode) {
    if (mode < 0 || mode > 2) runtime_fail("hle trace mode must be 0, 1 or 2");
    if (mode) {
        __atomic_store_n(&runtime_hle_trace, mode, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&runtime_hle_trace, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < used; i++) {
        uint64_t n = atomic_load_explicit(&counts[i], memory_order_relaxed);
        if (!n) continue;
        char text[256];
        name_of(i, text, sizeof text);
        LOG("hle", "%10llu calls  %s", (unsigned long long)n, text);
    }
}

/* Called on a guest thread's own host stack before it enters guest code; `top` must leave room for the caller's frame. */
void runtime_hle_stack_set(uintptr_t top) {
    uintptr_t tp;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    if (!runtime_hle_tsd_off) runtime_fail("host-stack slot not initialised");
    *(volatile uintptr_t *)(tp + runtime_hle_tsd_off) = top & ~(uintptr_t)15;
}
