/* Horizon thread and synchronization svc, on host pthreads.
 *
 * Every guest thread is one pthread running on the stack the SDK gave it; the SDK's own nn::os runs unchanged and only
 * the kernel under it is replaced:
 *   threads   0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D priority 0x0E/0x0F core mask
 *             0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x32 SetThreadActivity
 *   mutex     0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 when there are waiters)
 *   condvar   0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
 *   address   0x34 WaitForAddress 0x35 SignalToAddress
 *   events    0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x19 CancelSynchronization
 * Semantics follow the public Horizon kernel (KConditionVariable, KAddressArbiter, KSynchronization): waiters are woken in
 * priority order, then first come first served. All waits hang on one global lock G with one condition variable per thread.
 *
 * Virtual cores: Horizon runs at most one thread per core and games key data by GetCurrentProcessorNumber without locks, so
 * a guest thread holds one of three application cores while it runs guest code and gives it up only where Horizon would
 * switch threads (waits, sleep, yield, exit, blocking host services). NAPPLE_FREE_CORES=1 turns this off. */
#include "napple.h"
#include "timescale.h"
#include <errno.h>
#include <mach/mach.h>
#include <pthread/qos.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WAITERS_BIT 0x40000000u
#define NCORE 3
#define CORE_MASK_ALL 7ull

typedef struct Event { int signaled; int refs; } Event;

typedef struct Thread {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top, host_context[2];
    int prio, core; uint64_t mask;
    int started, exited, cancel, closed;
    pthread_t pt; pthread_cond_t cv;
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres, wseq;   /* wkind: 0 none, 1 lock, 2 condvar, 3 address, 4 sync */
    struct Thread *next, *all;
    int last_svc; uint64_t last_lr, last_fp, nsvc;
    int cur_core, preempt; uint64_t cseq, core_since; struct Thread *cnext;
    uint32_t wh[4]; int nwh;
} Thread;

static Thread *g_all, *g_wait, *g_core_wait, *g_core_owner[NCORE];
static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_key;
static uint64_t g_next_id = 0x51, g_seq;
static int g_cores_on = -1;

static Thread *self_or_null(void) { return g_key ? pthread_getspecific(g_key) : NULL; }
static Thread *self(void) {
    Thread *t = self_or_null();
    if (!t) runtime_fail("thread svc from a host thread");
    return t;
}
static int cores_enabled(void) { if (g_cores_on < 0) g_cores_on = getenv("NAPPLE_FREE_CORES") ? 0 : 1; return g_cores_on; }
static int core_allowed(const Thread *t, int c) { return (int)((t->mask >> c) & 1); }
static Thread *thread_of(uint32_t h) {
    if (h == HANDLE_CURRENT_THREAD) return self_or_null();
    return handle_get(h, OBJ_THREAD);
}

/* ---- waiting (G held) ---------------------------------------------------------------------------------------- */
static void wait_unlink(Thread *t) {
    for (Thread **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
/* Waiter in kernel order: lowest priority value first, then first registered. kind 2 matches by condvar key, else address. */
static Thread *pick(int kind, uint64_t a) {
    Thread *b = NULL;
    for (Thread *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(Thread *t, int64_t res) { wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv); }

static void core_grant(int c) {
    Thread *b = NULL, **bp = NULL;
    for (Thread **p = &g_core_wait; *p; p = &(*p)->cnext)
        if (core_allowed(*p, c) && (!b || (*p)->prio < b->prio || ((*p)->prio == b->prio && (*p)->cseq < b->cseq))) { b = *p; bp = p; }
    if (!b) return;
    *bp = b->cnext; b->cnext = NULL; b->cur_core = c; b->core_since = runtime_now_ns(); g_core_owner[c] = b; pthread_cond_signal(&b->cv);
}
static void core_put(Thread *t) {
    if (t->cur_core < 0) return;
    int c = t->cur_core; t->cur_core = -1; t->preempt = 0; g_core_owner[c] = NULL; core_grant(c);
}
static void core_take(Thread *t) {
    if (!cores_enabled() || t->cur_core >= 0) return;
    uint64_t mask = t->mask & CORE_MASK_ALL;
    if (!mask) runtime_fail("thread %#x has a core mask (%#llx) without an application core", t->handle, (unsigned long long)t->mask);
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && !g_core_owner[t->core] ? t->core : -1;
    for (int i = 0; c < 0 && i < NCORE; i++) if (core_allowed(t, i) && !g_core_owner[i]) c = i;
    if (c >= 0) { t->cur_core = c; t->core_since = runtime_now_ns(); g_core_owner[c] = t; return; }
    t->cseq = ++g_seq; t->cnext = g_core_wait; g_core_wait = t;
    for (int i = 0; i < NCORE; i++) if (core_allowed(t, i) && g_core_owner[i] && g_core_owner[i]->prio > t->prio) g_core_owner[i]->preempt = 1;
    while (t->cur_core < 0) pthread_cond_wait(&t->cv, &G);
}
void thread_core_away(int *token) {
    Thread *t = self_or_null();
    *token = 0;
    if (!t || t->cur_core < 0) return;
    pthread_mutex_lock(&G); core_put(t); pthread_mutex_unlock(&G); *token = 1;
}
void thread_core_back(int *token) {
    Thread *t = self_or_null();
    if (!*token || !t) return;
    pthread_mutex_lock(&G); core_take(t); pthread_mutex_unlock(&G); *token = 0;
}
/* Preemption point at svc entry: a better thread waits for this core. */
static void core_checkpoint(void) {
    Thread *t = self_or_null();
    if (!t || !t->preempt) return;
    pthread_mutex_lock(&G); t->preempt = 0; core_put(t); core_take(t); pthread_mutex_unlock(&G);
}
int threads_current_core(void) { Thread *t = self_or_null(); return t ? (t->cur_core >= 0 ? t->cur_core : t->core) : -1; }

/* Guest-visible monotonic time (ns): deadlines of guest timeouts live on the guest clock, so a game speed change (timescale.c) moves them consistently. */
static int64_t mono_ns(void) { return (int64_t)timescale_guest_ns(); }
/* A timed wait never sleeps longer than this in one go, so a change of the game speed reaches threads that wait with a long timeout. */
#define WAIT_SLICE_NS 100000000ll
#define TIMEOUT_CAP_NS 4000000000000000000ll   /* ~126 years: keeps now + timeout from overflowing for INT64_MAX timeouts */

/* Suspends until woken or timed out (ns < 0 forever, 0 immediately); returns wres or R_TIMED_OUT. G held. */
static int64_t block_inner(Thread *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = (int64_t)++g_seq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMED_OUT; }
    if (ns > TIMEOUT_CAP_NS) ns = TIMEOUT_CAP_NS;
    int64_t deadline = ns > 0 ? mono_ns() + ns : 0;
    while (!t->woke) {
        if (ns > 0) {
            int64_t left = deadline - mono_ns();
            if (left <= 0) { wait_unlink(t); t->wkind = 0; return R_TIMED_OUT; }
            left = timescale_real_wait_ns(left);
            if (left > WAIT_SLICE_NS) left = WAIT_SLICE_NS;
            struct timespec rel = {left / 1000000000, left % 1000000000};
            pthread_cond_timedwait_relative_np(&t->cv, &G, &rel);
        } else pthread_cond_wait(&t->cv, &G);
    }
    return t->wres;
}
static int64_t block(Thread *t, int kind, int64_t ns) {
    if (ns == 0) return block_inner(t, kind, ns);
    core_put(t); int64_t r = block_inner(t, kind, ns); core_take(t); return r;
}

/* ---- user-mode words ---------------------------------------------------------------------------------------- */
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)(uintptr_t)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
static int user_word_ok(uint64_t a) { return a && !(a & 3) && guest_ptr(a, 4); }

/* Releases the lock at addr: to the first waiter in kernel order, or clears it. */
static void lock_handoff(uint64_t addr) {
    Thread *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (Thread *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
/* Makes t take the lock at addr: free, it gets it and wakes; otherwise it sets the waiters bit and becomes a lock waiter. */
static void lock_acquire_for(Thread *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag; t->wseq = (int64_t)++g_seq;
}

/* ---- thread life cycle -------------------------------------------------------------------------------------- */
extern void runtime_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp, uint64_t *host_context);
extern _Noreturn void runtime_thread_leave(const uint64_t *host_context, void *arg, void (*fn)(void *));
void runtime_thread_cleanup(void);

static void *thread_main(void *p) {
    Thread *t = p;
    pthread_setspecific(g_key, t);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    runtime_thread_init();
    pthread_mutex_lock(&G);
    while (!t->started) pthread_cond_wait(&t->cv, &G);
    core_take(t);
    pthread_mutex_unlock(&G);
    { volatile char marker = 0; runtime_hle_stack_set((uintptr_t)&marker - 1024); }   /* host services of this thread run below here */
    runtime_thread_enter(t->entry, t->arg, t->stack_top, t->host_context);            /* never returns: ExitThread */
    runtime_fail("guest thread entry returned without ExitThread");
}
static void thread_free(Thread *t) {
    for (Thread **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); free(t);
}
static void wake_sync_waiters(void) {
    for (Thread *w = g_wait; w;) { Thread *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }
}
/* No logging here: this runs on the guest thread's own (possibly tiny) stack. */
static _Noreturn void thread_exit_on_host(void *argument) {
    Thread *t = argument;
    pthread_mutex_lock(&G);
    core_put(t);
    t->exited = 1;
    wake_sync_waiters();
    pthread_setspecific(g_key, NULL);
    if (t->closed) thread_free(t);
    pthread_mutex_unlock(&G);
    runtime_thread_cleanup();
    pthread_exit(NULL);
}
static _Noreturn void thread_exit(Thread *t) {
    if (t->host_context[0]) runtime_thread_leave(t->host_context, t, thread_exit_on_host);
    thread_exit_on_host(t);
}
void runtime_thread_returned(void) { runtime_fail("guest thread entry function returned (Horizon requires ExitThread)"); }

uint32_t threads_register_main(void) {
    pthread_key_create(&g_key, NULL);
    Thread *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cv, NULL);
    t->started = 1; t->prio = 44; t->core = g_npdm.valid ? g_npdm.main_core : 0; t->mask = CORE_MASK_ALL; t->pt = pthread_self(); t->cur_core = -1;
    if (g_npdm.valid) t->prio = g_npdm.main_priority;
    pthread_mutex_lock(&G);
    t->handle = handle_new(OBJ_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; core_take(t);
    pthread_mutex_unlock(&G);
    pthread_setspecific(g_key, t);
    return t->handle;
}
uint32_t threads_current_handle(void) { Thread *t = self_or_null(); return t ? t->handle : 0; }
uint64_t threads_current_id(void) { Thread *t = self_or_null(); return t ? t->id : 0; }

static int sync_state(uint32_t h);
void thread_dump_all(void) {
    pthread_mutex_lock(&G);
    for (Thread *t = g_all; t; t = t->all) {
        char where[96]; module_describe(t->last_lr, where, sizeof where);
        LOG("threads", "thread %#x id=%#llx prio=%d core=%d cur_core=%d wait=%d svc=%#x calls=%llu last caller %s%s", t->handle,
            (unsigned long long)t->id, t->prio, t->core, t->cur_core, t->wkind, t->last_svc, (unsigned long long)t->nsvc, where, t->exited ? " (exited)" : "");
        if (t->wkind == 4) { for (int i = 0; i < t->nwh && i < 4; i++) LOG("threads", "  waits on handle %#x (type %d, state %d)", t->wh[i], (int)handle_type(t->wh[i]), sync_state(t->wh[i])); }
        else if (t->wkind) LOG("threads", "  waits on address %#llx key %#llx", (unsigned long long)t->waddr, (unsigned long long)t->wkey);
        if (!t->exited && t->last_fp) runtime_trace_guest_n(t->last_fp, t->last_lr, 7);
    }
    pthread_mutex_unlock(&G);
}

/* ---- events (readable/writable pair) ------------------------------------------------------------------------- */
uint32_t event_create(uint32_t *readable_out) {
    Event *e = calloc(1, sizeof *e); e->refs = 2;
    uint32_t w = handle_new(OBJ_EVENT_W, e), r = handle_new(OBJ_EVENT_R, e);
    if (readable_out) *readable_out = r;
    return w;
}
void event_signal_handle(uint32_t writable) {
    pthread_mutex_lock(&G);
    Event *e = handle_get(writable, OBJ_EVENT_W);
    if (e) { e->signaled = 1; wake_sync_waiters(); }
    pthread_mutex_unlock(&G);
}
void event_clear_handle(uint32_t any) {
    pthread_mutex_lock(&G);
    Event *e = handle_get(any, OBJ_EVENT_W); if (!e) e = handle_get(any, OBJ_EVENT_R);
    if (e) e->signaled = 0;
    pthread_mutex_unlock(&G);
}
int event_is_signaled(uint32_t readable) {
    pthread_mutex_lock(&G);
    Event *e = handle_get(readable, OBJ_EVENT_R); int s = e ? e->signaled : 0;
    pthread_mutex_unlock(&G);
    return s;
}
/* 1 = signaled, 0 = not, -1 = not a waitable handle. G held. */
static int sync_state(uint32_t h) {
    switch (handle_type(h)) {
    case OBJ_THREAD: { Thread *t = handle_get(h, OBJ_THREAD); return t && t->exited; }
    case OBJ_EVENT_R: { Event *e = handle_get(h, OBJ_EVENT_R); return e && e->signaled; }
    default: return h == HANDLE_CURRENT_THREAD ? 0 : -1;
    }
}
/* CloseHandle for the kinds owned here; returns 1 when handled. */
int threads_close_object(ObjType type, void *obj) {
    switch (type) {
    case OBJ_EVENT_R: case OBJ_EVENT_W: { Event *e = obj; if (--e->refs == 0) free(e); return 1; }
    case OBJ_THREAD: { Thread *t = obj; pthread_mutex_lock(&G); t->closed = 1; if (t->exited) thread_free(t); pthread_mutex_unlock(&G); return 1; }
    default: return 0;
    }
}

/* ---- svc handlers -------------------------------------------------------------------------------------------- */
static void svc_create_thread(uint64_t *r) {
    int prio = (int)r[4], core = (int)r[5];
    if (!r[1] || !r[3] || (r[3] & 15)) { r[0] = R_INVALID_ADDRESS; return; }
    if (prio < 0 || prio > 63) { r[0] = R_INVALID_THREAD_PRIORITY; return; }
    if (core < -2 || core > 3) { r[0] = R_INVALID_PROCESSOR_ID; return; }
    Thread *t = calloc(1, sizeof *t);
    if (!t) { r[0] = R_OUT_OF_RESOURCE; return; }
    t->cur_core = -1; pthread_cond_init(&t->cv, NULL);
    t->entry = r[1]; t->arg = r[2]; t->stack_top = r[3] & ~0xFull; t->prio = prio;
    if (core == -2) core = g_npdm.valid ? g_npdm.main_core : 0;
    t->core = core; t->mask = core >= 0 ? 1ull << core : CORE_MASK_ALL;
    pthread_mutex_lock(&G);
    t->handle = handle_new(OBJ_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t;
    pthread_mutex_unlock(&G);
    if (!t->handle) { free(t); r[0] = R_OUT_OF_RESOURCE; return; }
    pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 << 20);      /* also the host-service stack of this guest thread */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->pt, &a, thread_main, t)) runtime_fail("pthread_create for guest thread");
    pthread_attr_destroy(&a);
    LOGV("kernel", "CreateThread %#x entry=%#llx sp=%#llx prio=%d core=%d", t->handle, (unsigned long long)t->entry, (unsigned long long)t->stack_top, prio, core);
    r[0] = 0; r[1] = t->handle;
}
static void svc_start_thread(uint64_t *r) {
    pthread_mutex_lock(&G);
    Thread *t = thread_of((uint32_t)r[0]);
    if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
    pthread_mutex_unlock(&G);
    r[0] = t ? 0 : R_INVALID_HANDLE;
}
static void svc_exit_thread(uint64_t *r) { (void)r; thread_exit(self()); }
static void svc_sleep_thread(uint64_t *r) {
    int64_t ns = (int64_t)r[0];
    Thread *me = self();
    if (ns > 0) {
        pthread_mutex_lock(&G); core_put(me); pthread_mutex_unlock(&G);
        if (ns > TIMEOUT_CAP_NS) ns = TIMEOUT_CAP_NS;
        int64_t deadline = mono_ns() + ns, left;
        while ((left = deadline - mono_ns()) > 0) {      /* guest time left, re-evaluated so a speed change applies to a sleeping thread */
            left = timescale_real_wait_ns(left); if (left > WAIT_SLICE_NS) left = WAIT_SLICE_NS;
            struct timespec ts = {left / 1000000000, left % 1000000000}; nanosleep(&ts, NULL);
        }
        pthread_mutex_lock(&G); core_take(me); pthread_mutex_unlock(&G);
        return;
    }
    /* Yield (0, -1, -2): hand the core to a waiting thread of equal or better priority. */
    pthread_mutex_lock(&G);
    int give = 0;
    for (Thread *w = g_core_wait; w && !give; w = w->cnext) give = me->cur_core >= 0 && core_allowed(w, me->cur_core) && w->prio <= me->prio;
    if (give) { core_put(me); core_take(me); }
    pthread_mutex_unlock(&G);
    if (!give) sched_yield();
}
static void svc_get_thread_priority(uint64_t *r) {
    pthread_mutex_lock(&G); Thread *t = thread_of((uint32_t)r[1]);
    r[0] = t ? 0 : R_INVALID_HANDLE; r[1] = t ? (uint64_t)t->prio : 0; pthread_mutex_unlock(&G);
}
static void svc_set_thread_priority(uint64_t *r) {
    if (r[1] > 63) { r[0] = R_INVALID_THREAD_PRIORITY; return; }
    pthread_mutex_lock(&G); Thread *t = thread_of((uint32_t)r[0]);
    if (t) t->prio = (int)r[1];
    r[0] = t ? 0 : R_INVALID_HANDLE; pthread_mutex_unlock(&G);
}
static void svc_get_thread_core_mask(uint64_t *r) {
    pthread_mutex_lock(&G); Thread *t = thread_of((uint32_t)r[2]);
    r[0] = t ? 0 : R_INVALID_HANDLE; r[1] = t ? (uint64_t)(uint32_t)t->core : 0; r[2] = t ? t->mask : 0; pthread_mutex_unlock(&G);
}
/* SetThreadCoreMask(w0 thread, w1 ideal core: -1 none, -2 process default, -3 unchanged; x2 affinity mask). */
static void svc_set_thread_core_mask(uint64_t *r) {
    pthread_mutex_lock(&G); Thread *t = thread_of((uint32_t)r[0]);
    uint32_t res = t ? 0 : R_INVALID_HANDLE;
    if (t) {
        int c = (int)r[1]; uint64_t m = r[2];
        if (c == -2) { c = g_npdm.valid ? g_npdm.main_core : 0; m = 1ull << c; }
        else if ((m | CORE_MASK_ALL) != CORE_MASK_ALL) res = R_INVALID_PROCESSOR_ID;
        else if (!m) res = R_INVALID_COMBINATION;
        else if (c >= 0 && c < NCORE) { if (!((m >> c) & 1)) res = R_INVALID_COMBINATION; }
        else if (c == -3) { c = t->core; if (c >= 0 && !((m >> c) & 1)) res = R_INVALID_COMBINATION; }
        else if (c != -1) res = R_INVALID_PROCESSOR_ID;
        if (!res) {
            t->core = c; t->mask = m;
            if (t == self_or_null() && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) { core_put(t); core_take(t); }
        }
    }
    pthread_mutex_unlock(&G);
    r[0] = res;
}
static void svc_get_processor_number(uint64_t *r) { Thread *t = self(); r[0] = (uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core); }
static void svc_get_thread_id(uint64_t *r) {
    pthread_mutex_lock(&G); Thread *t = thread_of((uint32_t)r[1]);
    r[0] = t ? 0 : R_INVALID_HANDLE; r[1] = t ? t->id : 0; pthread_mutex_unlock(&G);
}
static void svc_set_thread_activity(uint64_t *r) {   /* pause/resume: only the existence of the thread is checked */
    pthread_mutex_lock(&G); Thread *t = thread_of((uint32_t)r[0]); pthread_mutex_unlock(&G);
    r[0] = t ? 0 : R_INVALID_HANDLE;
    if (t && r[1] == 1) LOG("kernel", "SetThreadActivity(pause) ignored for thread %#x", (uint32_t)(uintptr_t)t->handle);
}

static void svc_arbitrate_lock(uint64_t *r) {
    Thread *t = self(); uint64_t addr = r[1];
    if (!user_word_ok(addr)) { r[0] = R_INVALID_ADDRESS; return; }
    pthread_mutex_lock(&G);
    int64_t res = 0;
    if (ld32(addr) == ((uint32_t)r[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)r[2]; res = block(t, 1, -1); }
    pthread_mutex_unlock(&G);
    r[0] = (uint64_t)res;
}
static void svc_arbitrate_unlock(uint64_t *r) {
    if (!user_word_ok(r[0])) { r[0] = R_INVALID_ADDRESS; return; }
    pthread_mutex_lock(&G); lock_handoff(r[0]); pthread_mutex_unlock(&G); r[0] = 0;
}
/* WaitProcessWideKeyAtomic(x0 lock address, x1 condvar key, w2 tag, x3 timeout). The key is set to 1 before the mutex is
 * released: the SDK's Signal reads the key in user mode and skips svc 0x1D when it is 0, and taking the mutex in user mode
 * does not go through G either, so the opposite order loses wake-ups. */
static void svc_wait_process_wide_key(uint64_t *r) {
    Thread *t = self(); uint64_t addr = r[0], key = r[1];
    if (!user_word_ok(addr) || !user_word_ok(key)) { r[0] = R_INVALID_ADDRESS; return; }
    pthread_mutex_lock(&G);
    st32(key, 1);
    t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)r[2];
    lock_handoff(addr);
    int64_t res = block(t, 2, (int64_t)r[3]);   /* on timeout the lock is not re-taken here: user mode takes it */
    pthread_mutex_unlock(&G);
    r[0] = (uint64_t)res;
}
static void svc_signal_process_wide_key(uint64_t *r) {
    uint64_t key = r[0]; int cnt = (int)r[1], n = 0;
    pthread_mutex_lock(&G);
    for (Thread *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
    int left = 0;
    for (Thread *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
    if (!left && user_word_ok(key)) st32(key, 0);
    pthread_mutex_unlock(&G);
    r[0] = 0;
}
/* WaitForAddress(x0 address, w1 arbitration type: 0 wait if less than, 1 decrement-and-wait if less than, 2 wait if equal; w2 value, x3 timeout) */
static void svc_wait_for_address(uint64_t *r) {
    Thread *t = self(); uint64_t addr = r[0]; int type = (int)r[1]; int32_t val = (int32_t)r[2];
    if (!user_word_ok(addr)) { r[0] = R_INVALID_ADDRESS; return; }
    if (type < 0 || type > 2) { r[0] = R_INVALID_ENUM; return; }
    pthread_mutex_lock(&G);
    int64_t res; uint32_t v = ld32(addr);
    int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
    if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
    if (!ok) res = R_INVALID_STATE;
    else { t->waddr = addr; res = block(t, 3, (int64_t)r[3]); }
    pthread_mutex_unlock(&G);
    r[0] = (uint64_t)res;
}
/* SignalToAddress(x0 address, w1 signal type: 0 signal, 1 increment-if-equal, 2 modify-by-waiter-count-if-equal; w2 value, w3 count) */
static void svc_signal_to_address(uint64_t *r) {
    uint64_t addr = r[0]; int type = (int)r[1]; int32_t val = (int32_t)r[2]; int cnt = (int)r[3];
    if (!user_word_ok(addr)) { r[0] = R_INVALID_ADDRESS; return; }
    if (type < 0 || type > 2) { r[0] = R_INVALID_ENUM; return; }
    pthread_mutex_lock(&G);
    int nw = 0;
    for (Thread *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
    int64_t res = 0;
    if (type != 0) {
        int32_t nv = val + 1;
        if (type == 2) {
            if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
            else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
        }
        uint32_t e = (uint32_t)val;
        if (!cas32(addr, &e, (uint32_t)nv)) res = R_INVALID_STATE;
    }
    if (!res) { int n = 0; for (Thread *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
    pthread_mutex_unlock(&G);
    r[0] = (uint64_t)res;
}

static void svc_create_event(uint64_t *r) {
    uint32_t rd = 0; pthread_mutex_lock(&G); uint32_t w = event_create(&rd); pthread_mutex_unlock(&G);
    r[0] = 0; r[1] = w; r[2] = rd;
}
/* SignalEvent accepts only the writable end, ResetSignal only the readable end, ClearEvent either. */
static void svc_event_op(unsigned num, uint64_t *r) {
    uint32_t h = (uint32_t)r[0];
    pthread_mutex_lock(&G);
    Event *w = handle_get(h, OBJ_EVENT_W), *rd = handle_get(h, OBJ_EVENT_R);
    Event *e = num == 0x11 ? w : num == 0x17 ? rd : (w ? w : rd);
    uint32_t res = 0;
    if (!e) res = (num == 0x17 && handle_get(h, OBJ_THREAD)) ? R_INVALID_STATE : R_INVALID_HANDLE;
    else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(); }
    else if (num == 0x17 && !e->signaled) res = R_INVALID_STATE;
    else e->signaled = 0;
    pthread_mutex_unlock(&G);
    r[0] = res;
}
static void svc_signal_event(uint64_t *r) { svc_event_op(0x11, r); }
static void svc_clear_event(uint64_t *r) { svc_event_op(0x12, r); }
static void svc_reset_signal(uint64_t *r) { svc_event_op(0x17, r); }

static void svc_wait_synchronization(uint64_t *r) {
    Thread *t = self(); uint64_t table = r[1]; int n = (int)r[2]; int64_t ns = (int64_t)r[3];
    if (n < 0 || n > 64 || (!table && n)) { r[0] = R_OUT_OF_RESOURCE; return; }
    uint32_t hs[64];
    if (n) { const void *p = guest_ptr(table, (uint64_t)n * 4); if (!p) { r[0] = R_INVALID_POINTER; return; } memcpy(hs, p, (size_t)n * 4); }
    if (ns > TIMEOUT_CAP_NS) ns = TIMEOUT_CAP_NS;
    int64_t deadline = ns > 0 ? mono_ns() + ns : 0, res; int idx = -1;
    pthread_mutex_lock(&G);
    t->nwh = n; for (int i = 0; i < n && i < 4; i++) t->wh[i] = hs[i];
    for (;;) {
        if (t->cancel) { t->cancel = 0; res = R_CANCELLED; break; }
        int bad = 0;
        for (int i = 0; i < n && idx < 0; i++) { int s = sync_state(hs[i]); if (s < 0) bad = 1; else if (s) idx = i; }
        if (idx >= 0) { res = 0; break; }
        if (bad) { res = R_INVALID_HANDLE; break; }
        int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - mono_ns();
        if (ns >= 0 && left <= 0) { res = R_TIMED_OUT; break; }
        if (block(t, 4, left) == R_TIMED_OUT) { res = R_TIMED_OUT; break; }
    }
    t->nwh = 0;
    pthread_mutex_unlock(&G);
    r[0] = (uint64_t)res; r[1] = idx >= 0 ? (uint64_t)idx : 0;
}
static void svc_cancel_synchronization(uint64_t *r) {
    pthread_mutex_lock(&G);
    Thread *t = thread_of((uint32_t)r[0]);
    if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
    pthread_mutex_unlock(&G);
    r[0] = t ? 0 : R_INVALID_HANDLE;
}

void threads_checkpoint(void) { core_checkpoint(); }
void threads_note_svc(unsigned num, uint64_t *regs) {
    Thread *me = self_or_null();
    if (me) { me->last_svc = (int)num; me->last_lr = regs[30]; me->last_fp = regs[29]; me->nsvc++; }
}

void svc_threads_register(void) {
    svc_register(0x08, "CreateThread", svc_create_thread);
    svc_register(0x09, "StartThread", svc_start_thread);
    svc_register(0x0A, "ExitThread", svc_exit_thread);
    svc_register(0x0B, "SleepThread", svc_sleep_thread);
    svc_register(0x0C, "GetThreadPriority", svc_get_thread_priority);
    svc_register(0x0D, "SetThreadPriority", svc_set_thread_priority);
    svc_register(0x0E, "GetThreadCoreMask", svc_get_thread_core_mask);
    svc_register(0x0F, "SetThreadCoreMask", svc_set_thread_core_mask);
    svc_register(0x10, "GetCurrentProcessorNumber", svc_get_processor_number);
    svc_register(0x11, "SignalEvent", svc_signal_event);
    svc_register(0x12, "ClearEvent", svc_clear_event);
    svc_register(0x17, "ResetSignal", svc_reset_signal);
    svc_register(0x18, "WaitSynchronization", svc_wait_synchronization);
    svc_register(0x19, "CancelSynchronization", svc_cancel_synchronization);
    svc_register(0x1A, "ArbitrateLock", svc_arbitrate_lock);
    svc_register(0x1B, "ArbitrateUnlock", svc_arbitrate_unlock);
    svc_register(0x1C, "WaitProcessWideKeyAtomic", svc_wait_process_wide_key);
    svc_register(0x1D, "SignalProcessWideKey", svc_signal_process_wide_key);
    svc_register(0x25, "GetThreadId", svc_get_thread_id);
    svc_register(0x32, "SetThreadActivity", svc_set_thread_activity);
    svc_register(0x34, "WaitForAddress", svc_wait_for_address);
    svc_register(0x35, "SignalToAddress", svc_signal_to_address);
    svc_register(0x45, "CreateEvent", svc_create_event);
}
