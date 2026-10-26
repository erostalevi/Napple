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
#include "kthread.h"
#include "timescale.h"
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#include <mach/mach.h>
#define thread_set_interactive() pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)
#define cond_wait_relative(cv, m, rel) pthread_cond_timedwait_relative_np((cv), (m), (rel))
#else /* the host unit tests (runtime/tests/host) build this file on Linux */
#define thread_set_interactive() ((void)0)
static int cond_wait_relative(pthread_cond_t *cv, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += rel->tv_sec;
    ts.tv_nsec += rel->tv_nsec;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    return pthread_cond_timedwait(cv, m, &ts);
}
#endif

#define WAITERS_BIT 0x40000000u
#define NCORE 3
#define CORE_MASK_ALL 7ull

typedef struct Event {
    int signaled;
    int refs;
} Event;

static Thread *g_all, *g_wait, *g_core_wait, *g_core_owner[NCORE];
static Thread *g_core_resv[NCORE];
static int64_t g_core_resv_ns[NCORE];                   /* core kept for a woken / started higher-priority thread pinned to it (same-core handoff) */
static Thread *g_core_last[NCORE], *g_core_prev[NCORE]; /* GetLastThreadInfo: who held each core before its current owner */
pthread_mutex_t kernel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_park_cv = PTHREAD_COND_INITIALIZER;
static pthread_key_t g_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
static int g_key_ready;
static void key_create(void) {
    if (pthread_key_create(&g_key, NULL)) runtime_fail("thread key");
    g_key_ready = 1;
}
static uint64_t g_next_id = 0x51, g_seq;
static int g_cores_on = -1;

static Thread *self_or_null(void) { return g_key_ready ? pthread_getspecific(g_key) : NULL; }
static Thread *self(void) {
    Thread *t = self_or_null();
    if (!t) runtime_fail("thread svc from a host thread");
    return t;
}
static int cores_enabled(void) {
    if (g_cores_on < 0) g_cores_on = getenv("NAPPLE_FREE_CORES") ? 0 : 1;
    return g_cores_on;
}
static int core_allowed(const Thread *t, int c) { return (int)((t->mask >> c) & 1); }
static Thread *thread_of(uint32_t h) {
    if (h == HANDLE_CURRENT_THREAD) return self_or_null();
    return handle_get(h, OBJ_THREAD);
}

/* ---- waiting (G held) ---------------------------------------------------------------------------------------- */
static void wait_unlink(Thread *t) {
    for (Thread **p = &g_wait; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
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
static void core_handoff_note(Thread *t);
static void wake(Thread *t, int64_t res) {
    wait_unlink(t);
    t->wkind = 0;
    t->woke = 1;
    t->wres = res;
    pthread_cond_signal(&t->cv);
    core_handoff_note(t);
}
Thread *kthread_self(void) { return self(); }
Thread *kthread_self_or_null(void) { return self_or_null(); }
void kthread_wake(Thread *t, int64_t res) { wake(t, res); }

static void core_assign(Thread *t, int c) {
    if (g_core_resv[c] == t) g_core_resv[c] = NULL;
    t->cur_core = c;
    t->core_since = runtime_now_ns();
    g_core_owner[c] = t;
    if (g_core_last[c] != t) {
        g_core_prev[c] = g_core_last[c];
        g_core_last[c] = t;
    }
}
static void core_grant(int c) {
    Thread *b = NULL, **bp = NULL;
    for (Thread **p = &g_core_wait; *p; p = &(*p)->cnext)
        if (g_core_resv[c]) {
            if (*p == g_core_resv[c]) {
                b = *p;
                bp = p;
                break;
            }
        } /* reserved: only its thread may take it (it may not have run yet) */
        else if (core_allowed(*p, c) && (!b || (*p)->prio < b->prio || ((*p)->prio == b->prio && (*p)->cseq < b->cseq))) {
            b = *p;
            bp = p;
        }
    if (!b) return;
    *bp = b->cnext;
    b->cnext = NULL;
    core_assign(b, c);
    pthread_cond_signal(&b->cv);
}
static void core_put(Thread *t) {
    if (t->cur_core < 0) return;
    int c = t->cur_core;
    t->cur_core = -1;
    t->preempt = 0;
    g_core_owner[c] = NULL;
    core_grant(c);
}
static void core_take(Thread *t) {
    if (!cores_enabled() || t->cur_core >= 0) return;
    uint64_t mask = t->mask & CORE_MASK_ALL;
    if (!mask) runtime_fail("thread %#x has a core mask (%#llx) without an application core", t->handle, (unsigned long long)t->mask);
#define CORE_FREE(i) (!g_core_owner[i] && (!g_core_resv[i] || g_core_resv[i] == t))
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && CORE_FREE(t->core) ? t->core : -1;
    for (int i = 0; c < 0 && i < NCORE; i++)
        if (core_allowed(t, i) && CORE_FREE(i)) c = i;
#undef CORE_FREE
    if (c >= 0) {
        core_assign(t, c);
        return;
    }
    t->cseq = ++g_seq;
    t->cnext = g_core_wait;
    g_core_wait = t;
    for (int i = 0; i < NCORE; i++)
        if (core_allowed(t, i) && g_core_owner[i] && g_core_owner[i]->prio > t->prio) g_core_owner[i]->preempt = 1;
    while (t->cur_core < 0) {
        /* a reservation whose thread never came for it (it should always) must not hold a core for good */
        struct timespec rel = {0, 50000000};
        if (cond_wait_relative(&t->cv, &G, &rel) == 0 || t->cur_core >= 0) continue;
        for (int i = 0; i < NCORE; i++)
            if (g_core_resv[i] && !g_core_owner[i] && runtime_now_ns() - g_core_resv_ns[i] > 50000000ll) {
                g_core_resv[i] = NULL;
                core_grant(i);
            }
    }
}
/* Same-core handoff (Horizon runs the better thread at once): waking or starting a thread that can only run on the caller's core, with a
 * better priority than the caller, reserves that core for it and flags the caller, which yields the core at its svc exit (core_checkpoint)
 * and gets it back when that thread blocks again. Threads that may run on another core do not take the caller's. G held. */
static void core_handoff_note(Thread *t) {
    Thread *me = self_or_null();
    if (!me || me == t || !cores_enabled() || me->cur_core < 0 || t->cur_core >= 0 || t->exited) return;
    int c = me->cur_core;
    if (t->mask != (1ull << c) || t->prio >= me->prio) return;
    if (g_core_resv[c] && g_core_resv[c]->prio <= t->prio) return;
    g_core_resv[c] = t;
    g_core_resv_ns[c] = runtime_now_ns();
    me->preempt = 1;
}
void thread_core_away(int *token) {
    Thread *t = self_or_null();
    *token = 0;
    if (!t || t->cur_core < 0) return;
    pthread_mutex_lock(&G);
    core_put(t);
    pthread_mutex_unlock(&G);
    *token = 1;
}
void thread_core_back(int *token) {
    Thread *t = self_or_null();
    if (!*token || !t) return;
    pthread_mutex_lock(&G);
    core_take(t);
    pthread_mutex_unlock(&G);
    *token = 0;
}
/* ThreadLocalRegion words of the preemption protocol: user mode raises disable_count around short critical sections; a thread preempted
 * meanwhile is pinned instead (the kernel sets interrupt_flag) and calls SynchronizePreemptionState once the count is back to zero. */
#define TLR_DISABLE_COUNT 0x100
#define TLR_INTERRUPT_FLAG 0x102
static volatile uint16_t *tlr_word(const Thread *t, unsigned off) { return t->tlr ? (volatile uint16_t *)((uint8_t *)t->tlr + off) : NULL; }
/* Preemption point at svc entry: a better thread waits for this core. */
static void core_checkpoint(void) {
    Thread *t = self_or_null();
    if (!t || !t->preempt) return;
    volatile uint16_t *disable = tlr_word(t, TLR_DISABLE_COUNT);
    if (disable && *disable) {
        *tlr_word(t, TLR_INTERRUPT_FLAG) = 1;
        return;
    } /* pinned: the switch waits for SynchronizePreemptionState */
    pthread_mutex_lock(&G);
    t->preempt = 0;
    core_put(t);
    core_take(t);
    pthread_mutex_unlock(&G);
}
int threads_current_core(void) {
    Thread *t = self_or_null();
    return t ? (t->cur_core >= 0 ? t->cur_core : t->core) : -1;
}

/* Guest-visible monotonic time (ns): deadlines of guest timeouts live on the guest clock, so a game speed change (timescale.c) moves them consistently. */
static int64_t mono_ns(void) { return (int64_t)timescale_guest_ns(); }
/* A timed wait never sleeps longer than this in one go, so a change of the game speed reaches threads that wait with a long timeout. */
#define WAIT_SLICE_NS 100000000ll
#define SLEEP_PRECISE_NS 20000ll
#define TIMEOUT_CAP_NS 4000000000000000000ll /* ~126 years: keeps now + timeout from overflowing for INT64_MAX timeouts */

/* Suspends until woken or timed out (ns < 0 forever, 0 immediately); returns wres or R_TIMED_OUT. G held. */
static int64_t block_inner(Thread *t, int kind, int64_t ns) {
    t->wkind = kind;
    t->woke = 0;
    t->next = g_wait;
    g_wait = t;
    t->wseq = (int64_t)++g_seq;
    if (ns == 0) {
        wait_unlink(t);
        t->wkind = 0;
        return R_TIMED_OUT;
    }
    if (ns > TIMEOUT_CAP_NS) ns = TIMEOUT_CAP_NS;
    int64_t deadline = ns > 0 ? mono_ns() + ns : 0;
    while (!t->woke) {
        if (ns > 0) {
            int64_t left = deadline - mono_ns();
            if (left <= 0) {
                wait_unlink(t);
                t->wkind = 0;
                return R_TIMED_OUT;
            }
            left = timescale_real_wait_ns(left);
            if (left > WAIT_SLICE_NS) left = WAIT_SLICE_NS;
            struct timespec rel = {left / 1000000000, left % 1000000000};
            cond_wait_relative(&t->cv, &G, &rel);
        } else pthread_cond_wait(&t->cv, &G);
    }
    return t->wres;
}
static int64_t block(Thread *t, int kind, int64_t ns) {
    if (ns == 0) return block_inner(t, kind, ns);
    core_put(t);
    int64_t r = block_inner(t, kind, ns);
    core_take(t);
    return r;
}
int64_t kthread_block(Thread *t, int kind, int64_t ns) { return block(t, kind, ns); }

/* ---- user-mode words ---------------------------------------------------------------------------------------- */
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)(uintptr_t)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) {
    return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static int user_word_ok(uint64_t a) { return a && !(a & 3) && guest_ptr(a, 4); }

/* Releases the lock at addr: to the first waiter in kernel order, or clears it. */
static void lock_handoff(uint64_t addr) {
    Thread *w = pick(1, addr);
    int more = 0;
    if (!w) {
        st32(addr, 0);
        return;
    }
    for (Thread *t = g_wait; t; t = t->next)
        if (t != w && t->wkind == 1 && t->waddr == addr) {
            more = 1;
            break;
        }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
/* Makes t take the lock at addr: free, it gets it and wakes; otherwise it sets the waiters bit and becomes a lock waiter. */
static void lock_acquire_for(Thread *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) {
            if (cas32(addr, &v, tag)) {
                wake(t, 0);
                return;
            }
            continue;
        }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1;
    t->waddr = addr;
    t->wtag = tag;
    t->wseq = (int64_t)++g_seq;
}

/* ---- thread life cycle -------------------------------------------------------------------------------------- */
extern void runtime_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp, uint64_t *host_context);
extern _Noreturn void runtime_thread_leave(const uint64_t *host_context, void *arg, void (*fn)(void *));
void runtime_thread_cleanup(void);

uint8_t *memory_a32_base(void);
void a32_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);
/* AArch32 threads keep their thread local region in the guest window: point the kernel's view of it there. */
void threads_set_tlr_current(void *tlr) {
    Thread *t = pthread_getspecific(g_key);
    if (t) t->tlr = tlr;
}
static void *thread_main(void *p) {
    Thread *t = p;
    pthread_setspecific(g_key, t);
    thread_set_interactive();
    runtime_thread_init();
    t->tlr = pthread_getspecific(runtime_keys[2]);
    pthread_mutex_lock(&G);
    while (!t->started) pthread_cond_wait(&t->cv, &G);
    core_take(t);
    pthread_mutex_unlock(&G);
    {
        volatile char marker = 0;
        runtime_hle_stack_set((uintptr_t)&marker - 1024);
    }                                                                        /* host services of this thread run below here */
    if (memory_a32_base()) a32_thread_enter(t->entry, t->arg, t->stack_top); /* AArch32 process: the interpreter runs this thread (never returns: ExitThread) */
    runtime_thread_enter(t->entry, t->arg, t->stack_top, t->host_context);   /* never returns: ExitThread */
    runtime_fail("guest thread entry returned without ExitThread");
}
static void thread_free(Thread *t) {
    for (Thread **p = &g_all; *p; p = &(*p)->all)
        if (*p == t) {
            *p = t->all;
            break;
        }
    for (int c = 0; c < NCORE; c++) {
        if (g_core_last[c] == t) g_core_last[c] = NULL;
        if (g_core_prev[c] == t) g_core_prev[c] = NULL;
    }
    pthread_cond_destroy(&t->cv);
    free(t);
}
static void wake_sync_waiters(void) {
    for (Thread *w = g_wait; w;) {
        Thread *n = w->next;
        if (w->wkind == 4) wake(w, -1);
        w = n;
    }
}
void kthread_wake_sync_waiters(void) { wake_sync_waiters(); }
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

static void *pcprof_main(void *arg);
/* Registers the calling host thread as a guest thread (the main thread; the host unit tests adopt more threads this way). */
uint32_t threads_register_main(void) {
    pthread_once(&g_key_once, key_create);
    Thread *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cv, NULL);
    t->started = 1;
    t->prio = 44;
    t->core = g_npdm.valid ? g_npdm.main_core : 0;
    t->mask = CORE_MASK_ALL;
    t->pt = pthread_self();
    t->cur_core = -1;
    if (g_npdm.valid) t->prio = g_npdm.main_priority;
    pthread_mutex_lock(&G);
    t->handle = handle_new(OBJ_THREAD, t);
    t->id = g_next_id++;
    t->all = g_all;
    g_all = t;
    core_take(t);
    pthread_mutex_unlock(&G);
    pthread_setspecific(g_key, t);
    t->tlr = pthread_getspecific(runtime_keys[2]);
    if (getenv("NAPPLE_PCPROF")) {
        pthread_t pt;
        pthread_create(&pt, NULL, pcprof_main, NULL);
        pthread_detach(pt);
    }
    return t->handle;
}
/* Threads of the process that have not exited (GetInfo FreeThreadCount = the 96-thread application limit minus this). */
int threads_live_count(void) {
    int n = 0;
    pthread_mutex_lock(&G);
    for (Thread *t = g_all; t; t = t->all)
        if (!t->exited) n++;
    pthread_mutex_unlock(&G);
    return n;
}
uint32_t threads_current_handle(void) {
    Thread *t = self_or_null();
    return t ? t->handle : 0;
}
uint64_t threads_current_id(void) {
    Thread *t = self_or_null();
    return t ? t->id : 0;
}

/* NAPPLE_PCPROF=1: debug aid, samples the pc of every running (not waiting) guest thread every ms and logs the hottest (thread id, pc) pairs every 5 s. */
#define PCP_N 512
static struct {
    uint64_t id, pc;
    unsigned n;
} pcp[PCP_N];
static void *pcprof_main(void *arg) {
    (void)arg;
    unsigned tick = 0;
    for (;;) {
        usleep(1000);
        pthread_mutex_lock(&G);
        for (Thread *t = g_all; t; t = t->all) {
            if (t->exited || t->wkind || !t->pt || pthread_equal(t->pt, pthread_self())) continue;
            mach_port_t mt = pthread_mach_thread_np(t->pt);
            arm_thread_state64_t st;
            mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
            uint64_t pc = 0;
            if (thread_suspend(mt) != KERN_SUCCESS) continue;
            if (thread_get_state(mt, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) pc = arm_thread_state64_get_pc(st);
            thread_resume(mt);
            if (!pc) continue;
            if (t->id == 0x51 && getenv("NAPPLE_PCPROF_REGS") && tick % 300 == 0 && tick < 6000) {
                LOG("pcprof",
                    "regs pc=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx x8=%#llx x9=%#llx x10=%#llx x11=%#llx x14=%#llx x15=%#llx x16=%#llx x17=%#llx x19=%#llx x21=%#llx x22=%#llx",
                    (unsigned long long)pc, (unsigned long long)st.__x[0], (unsigned long long)st.__x[1], (unsigned long long)st.__x[2],
                    (unsigned long long)st.__x[3], (unsigned long long)st.__x[8], (unsigned long long)st.__x[9], (unsigned long long)st.__x[10],
                    (unsigned long long)st.__x[11], (unsigned long long)st.__x[14], (unsigned long long)st.__x[15], (unsigned long long)st.__x[16],
                    (unsigned long long)st.__x[17], (unsigned long long)st.__x[19], (unsigned long long)st.__x[21], (unsigned long long)st.__x[22]);
            }
            unsigned h = (unsigned)((pc >> 2) ^ (t->id * 31)) % PCP_N, k;
            for (k = 0; k < PCP_N; k++, h = (h + 1) % PCP_N)
                if (!pcp[h].n || (pcp[h].pc == pc && pcp[h].id == t->id)) break;
            if (k < PCP_N) {
                pcp[h].id = t->id;
                pcp[h].pc = pc;
                pcp[h].n++;
            }
        }
        pthread_mutex_unlock(&G);
        if (++tick % 5000) continue;
        for (int r = 0; r < 200; r++) {
            int best = -1;
            for (int i = 0; i < PCP_N; i++)
                if (pcp[i].n && (best < 0 || pcp[i].n > pcp[best].n)) best = i;
            if (best < 0) break;
            char w[96];
            module_describe(pcp[best].pc, w, sizeof w);
            LOG("pcprof", "thread %#llx pc %s : %u samples", (unsigned long long)pcp[best].id, w, pcp[best].n);
            pcp[best].n = 0;
        }
        memset(pcp, 0, sizeof pcp);
    }
    return NULL;
}
static int sync_state(uint32_t h);
void thread_dump_all(void) {
    pthread_mutex_lock(&G);
    for (Thread *t = g_all; t; t = t->all) {
        char where[96];
        module_describe(t->last_lr, where, sizeof where);
        LOG("threads", "thread %#x id=%#llx prio=%d core=%d cur_core=%d wait=%d svc=%#x calls=%llu last caller %s%s", t->handle, (unsigned long long)t->id,
            t->prio, t->core, t->cur_core, t->wkind, t->last_svc, (unsigned long long)t->nsvc, where, t->exited ? " (exited)" : "");
        if (t->wkind == 4) {
            for (int i = 0; i < t->nwh && i < 4; i++)
                LOG("threads", "  waits on handle %#x (type %d, state %d)", t->wh[i], (int)handle_type(t->wh[i]), sync_state(t->wh[i]));
        } else if (t->wkind) LOG("threads", "  waits on address %#llx key %#llx", (unsigned long long)t->waddr, (unsigned long long)t->wkey);
        if (!t->exited && t->last_fp) runtime_trace_guest_n(t->last_fp, t->last_lr, 7);
        if (!t->exited && !t->wkind && t->pt && !pthread_equal(t->pt, pthread_self())) { /* running in guest code: live pc + frame chain */
            mach_port_t mt = pthread_mach_thread_np(t->pt);
            arm_thread_state64_t st;
            mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
            if (thread_suspend(mt) == KERN_SUCCESS) {
                if (thread_get_state(mt, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
                    char w[96];
                    module_describe(arm_thread_state64_get_pc(st), w, sizeof w);
                    LOG("threads", "  RUNNING in guest at pc %s (x0=%#llx x1=%#llx x2=%#llx)", w, (unsigned long long)st.__x[0], (unsigned long long)st.__x[1],
                        (unsigned long long)st.__x[2]);
                    runtime_trace_guest_n(arm_thread_state64_get_fp(st), arm_thread_state64_get_lr(st), 24);
                }
                thread_resume(mt);
            }
        }
    }
    pthread_mutex_unlock(&G);
}

/* ---- events (readable/writable pair) ------------------------------------------------------------------------- */
uint32_t event_create(uint32_t *readable_out) {
    Event *e = calloc(1, sizeof *e);
    e->refs = 2;
    uint32_t w = handle_new(OBJ_EVENT_W, e), r = handle_new(OBJ_EVENT_R, e);
    if (readable_out) *readable_out = r;
    return w;
}
void event_signal_handle(uint32_t writable) {
    pthread_mutex_lock(&G);
    Event *e = handle_get(writable, OBJ_EVENT_W);
    if (e) {
        e->signaled = 1;
        wake_sync_waiters();
    }
    pthread_mutex_unlock(&G);
}
void event_clear_handle(uint32_t any) {
    pthread_mutex_lock(&G);
    Event *e = handle_get(any, OBJ_EVENT_W);
    if (!e) e = handle_get(any, OBJ_EVENT_R);
    if (e) e->signaled = 0;
    pthread_mutex_unlock(&G);
}
int event_is_signaled(uint32_t readable) {
    pthread_mutex_lock(&G);
    Event *e = handle_get(readable, OBJ_EVENT_R);
    int s = e ? e->signaled : 0;
    pthread_mutex_unlock(&G);
    return s;
}
/* 1 = signaled, 0 = not, -1 = not a waitable handle. G held. */
static int sync_state(uint32_t h) {
    switch (handle_type(h)) {
        case OBJ_THREAD: {
            Thread *t = handle_get(h, OBJ_THREAD);
            return t && t->exited;
        }
        case OBJ_EVENT_R: {
            Event *e = handle_get(h, OBJ_EVENT_R);
            return e && e->signaled;
        }
        default: return h == HANDLE_CURRENT_THREAD ? 0 : ksession_sync_state(h);
    }
}
/* CloseHandle for the kinds owned here; returns 1 when handled. */
int threads_close_object(ObjType type, void *obj) {
    switch (type) {
        case OBJ_EVENT_R:
        case OBJ_EVENT_W: {
            Event *e = obj;
            if (--e->refs == 0) free(e);
            return 1;
        }
        case OBJ_THREAD: {
            Thread *t = obj;
            pthread_mutex_lock(&G);
            t->closed = 1;
            if (t->exited) thread_free(t);
            pthread_mutex_unlock(&G);
            return 1;
        }
        default: return 0;
    }
}

/* ---- svc handlers -------------------------------------------------------------------------------------------- */
static void svc_create_thread(uint64_t *r) {
    int prio = (int)r[4], core = (int)r[5];
    if (!r[1] || !r[3] || (r[3] & 15)) {
        r[0] = R_INVALID_ADDRESS;
        return;
    }
    if (prio < 0 || prio > 63) {
        r[0] = R_INVALID_THREAD_PRIORITY;
        return;
    }
    if (core < -2 || core > 3) {
        r[0] = R_INVALID_PROCESSOR_ID;
        return;
    }
    Thread *t = calloc(1, sizeof *t);
    if (!t) {
        r[0] = R_OUT_OF_RESOURCE;
        return;
    }
    t->cur_core = -1;
    pthread_cond_init(&t->cv, NULL);
    t->entry = r[1];
    t->arg = r[2];
    t->stack_top = r[3] & ~0xFull;
    t->prio = prio;
    if (core == -2) core = g_npdm.valid ? g_npdm.main_core : 0;
    t->core = core;
    t->mask = core >= 0 ? 1ull << core : CORE_MASK_ALL;
    pthread_mutex_lock(&G);
    t->handle = handle_new(OBJ_THREAD, t);
    t->id = g_next_id++;
    t->all = g_all;
    g_all = t;
    pthread_mutex_unlock(&G);
    if (!t->handle) {
        free(t);
        r[0] = R_OUT_OF_RESOURCE;
        return;
    }
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 << 20); /* also the host-service stack of this guest thread */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->pt, &a, thread_main, t)) runtime_fail("pthread_create for guest thread");
    pthread_attr_destroy(&a);
    LOGV("kernel", "CreateThread %#x entry=%#llx sp=%#llx prio=%d core=%d", t->handle, (unsigned long long)t->entry, (unsigned long long)t->stack_top, prio,
         core);
    r[0] = 0;
    r[1] = t->handle;
}
static void svc_start_thread(uint64_t *r) {
    pthread_mutex_lock(&G);
    Thread *t = thread_of((uint32_t)r[0]);
    if (t) {
        int first = !t->started;
        t->started = 1;
        pthread_cond_signal(&t->cv);
        if (first) core_handoff_note(t);
    }
    pthread_mutex_unlock(&G);
    r[0] = t ? 0 : R_INVALID_HANDLE;
}
static void svc_exit_thread(uint64_t *r) {
    (void)r;
    thread_exit(self());
}
static void svc_sleep_thread(uint64_t *r) {
    int64_t ns = (int64_t)r[0];
    Thread *me = self();
    if (ns > 0) {
        pthread_mutex_lock(&G);
        core_put(me);
        pthread_mutex_unlock(&G);
        if (ns > TIMEOUT_CAP_NS) ns = TIMEOUT_CAP_NS;
        int64_t deadline = mono_ns() + ns, left;
        while ((left = deadline - mono_ns()) > 0) { /* guest time left, re-evaluated so a speed change applies to a sleeping thread */
            left = timescale_real_wait_ns(left);
            if (left > WAIT_SLICE_NS) left = WAIT_SLICE_NS;
            /* sleep_precise: macOS nanosleep overshoots by ~10% (33 ms -> 36.4 ms, a 30 Hz thread ran at 27 Hz), so sleep 3/4 of what is left per
             * step and stop within SLEEP_PRECISE_NS of the deadline. Under that margin (e.g. SleepThread(100)) the plain short sleep stays. */
            if (left > SLEEP_PRECISE_NS) {
                left = left * 3 / 4;
                if (left < SLEEP_PRECISE_NS) left = SLEEP_PRECISE_NS;
            }
            struct timespec ts = {left / 1000000000, left % 1000000000};
            nanosleep(&ts, NULL);
        }
        pthread_mutex_lock(&G);
        core_take(me);
        pthread_mutex_unlock(&G);
        return;
    }
    /* Yield (0, -1, -2): hand the core to a waiting thread of equal or better priority. */
    pthread_mutex_lock(&G);
    int give = 0;
    for (Thread *w = g_core_wait; w && !give; w = w->cnext) give = me->cur_core >= 0 && core_allowed(w, me->cur_core) && w->prio <= me->prio;
    if (give) {
        core_put(me);
        core_take(me);
    }
    pthread_mutex_unlock(&G);
    if (!give) sched_yield();
}
static void svc_get_thread_priority(uint64_t *r) {
    pthread_mutex_lock(&G);
    Thread *t = thread_of((uint32_t)r[1]);
    r[0] = t ? 0 : R_INVALID_HANDLE;
    r[1] = t ? (uint64_t)t->prio : 0;
    pthread_mutex_unlock(&G);
}
static void svc_set_thread_priority(uint64_t *r) {
    if (r[1] > 63) {
        r[0] = R_INVALID_THREAD_PRIORITY;
        return;
    }
    pthread_mutex_lock(&G);
    Thread *t = thread_of((uint32_t)r[0]);
    if (t) t->prio = (int)r[1];
    r[0] = t ? 0 : R_INVALID_HANDLE;
    pthread_mutex_unlock(&G);
}
static void svc_get_thread_core_mask(uint64_t *r) {
    pthread_mutex_lock(&G);
    Thread *t = thread_of((uint32_t)r[2]);
    r[0] = t ? 0 : R_INVALID_HANDLE;
    r[1] = t ? (uint64_t)(uint32_t)t->core : 0;
    r[2] = t ? t->mask : 0;
    pthread_mutex_unlock(&G);
}
/* SetThreadCoreMask(w0 thread, w1 ideal core: -1 none, -2 process default, -3 unchanged; x2 affinity mask). */
static void svc_set_thread_core_mask(uint64_t *r) {
    pthread_mutex_lock(&G);
    Thread *t = thread_of((uint32_t)r[0]);
    uint32_t res = t ? 0 : R_INVALID_HANDLE;
    if (t) {
        int c = (int)r[1];
        uint64_t m = r[2];
        if (c == -2) {
            c = g_npdm.valid ? g_npdm.main_core : 0;
            m = 1ull << c;
        } else if ((m | CORE_MASK_ALL) != CORE_MASK_ALL) res = R_INVALID_PROCESSOR_ID;
        else if (!m) res = R_INVALID_COMBINATION;
        else if (c >= 0 && c < NCORE) {
            if (!((m >> c) & 1)) res = R_INVALID_COMBINATION;
        } else if (c == -3) {
            c = t->core;
            if (c >= 0 && !((m >> c) & 1)) c = __builtin_ctzll(m);
        } /* ideal core kept, unless the new mask excludes it: it then moves into the mask (Horizon; nxnative docs/reference/threads.md) */
        else if (c != -1)
            res = R_INVALID_PROCESSOR_ID;
        if (!res) {
            t->core = c;
            t->mask = m;
            if (t == self_or_null() && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) {
                core_put(t);
                core_take(t);
            }
        }
    }
    pthread_mutex_unlock(&G);
    r[0] = res;
}
static void svc_get_processor_number(uint64_t *r) {
    Thread *t = self();
    r[0] = (uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core);
}
static void svc_get_thread_id(uint64_t *r) {
    pthread_mutex_lock(&G);
    Thread *t = thread_of((uint32_t)r[1]);
    r[0] = t ? 0 : R_INVALID_HANDLE;
    r[1] = t ? t->id : 0;
    pthread_mutex_unlock(&G);
}
/* SetThreadActivity(w0 thread, w1 0 runnable / 1 paused). A paused thread stops at its next svc boundary (entry or exit; a thread blocked
 * inside an svc is already stopped there): cooperative, so a thread spinning in guest code without svcs keeps running until it issues one.
 * Pausing waits briefly (PARK_WAIT_NS) for that, as the kernel waits for the thread to leave its core. */
#define PARK_WAIT_NS 50000000ll
/* G held: the thread sits inside an svc (parked, or blocked in a wait) so its svc register area is stable. */
static int stopped_in_svc(const Thread *t) { return t->svc_regs && (t->parked || t->wkind); }
static void wait_parked(Thread *t) {
    int64_t deadline = mono_ns() + PARK_WAIT_NS;
    while (t->paused && !stopped_in_svc(t) && !t->exited) {
        int64_t left = deadline - mono_ns();
        if (left <= 0) break;
        struct timespec rel = {0, left > 5000000 ? 5000000 : left};
        cond_wait_relative(&g_park_cv, &G, &rel);
    }
}
static void svc_set_thread_activity(uint64_t *r) {
    pthread_mutex_lock(&G);
    Thread *t = thread_of((uint32_t)r[0]);
    uint32_t res = 0;
    if (!t) res = R_INVALID_HANDLE;
    else if (r[1] > 1) res = R_INVALID_ENUM;
    else if (t == self_or_null()) res = R_BUSY;
    else if (t->exited) res = R_INVALID_STATE;
    else if (r[1] == 1) {
        if (t->paused) res = R_INVALID_STATE;
        else {
            t->paused = 1;
            wait_parked(t);
        }
    } else if (!t->paused) res = R_INVALID_STATE;
    else {
        t->paused = 0;
        if (t->parked) pthread_cond_signal(&t->cv);
    }
    pthread_mutex_unlock(&G);
    r[0] = res;
}
/* G held: a paused thread stops here, without its core, until resumed. Its svc register area stays valid meanwhile. */
static void park_if_paused(Thread *t) {
    if (!t->paused) return;
    core_put(t);
    t->parked = 1;
    pthread_cond_broadcast(&g_park_cv);
    while (t->paused) pthread_cond_wait(&t->cv, &G);
    t->parked = 0;
    core_take(t);
}

/* GetThreadContext3(x0 ThreadContext*, w1 thread): the context of a paused thread, taken from the register area of the svc it stopped in. */
typedef struct {
    uint64_t r[29], fp, lr, sp, pc;
    uint32_t pstate, pad;
    uint8_t v[32][16];
    uint32_t fpcr, fpsr;
    uint64_t tpidr;
} ThreadContext;
_Static_assert(sizeof(ThreadContext) == 0x320, "svc::ThreadContext layout");
static uint64_t svc_site_pc(const Thread *t) {
    const Module *m = t->svc_module ? module_by_name(t->svc_module) : NULL;
    return m ? (uintptr_t)m->base + t->svc_offset + 4 : 0;
}
static void fill_context(const Thread *t, ThreadContext *c) {
    const uint64_t *g = t->svc_regs;
    memset(c, 0, sizeof *c);
    memcpy(c->r, g, sizeof c->r);
    c->r[18] = t->svc_x18; /* the guest x18 lives in a thread slot; regs[18] is the host platform register */
    c->fp = g[29];
    c->lr = g[30];
    c->sp = g[KREG_GUEST_SP] + 16 + 160;
    c->pc = svc_site_pc(t);
    c->pstate = (uint32_t)g[KREG_NZCV];
    c->fpsr = (uint32_t)g[KREG_FPSR];
    c->fpcr = (uint32_t)g[KREG_FPCR];
    memcpy(c->v, (const uint8_t *)g + 256, sizeof c->v);
    c->tpidr = t->svc_tpidr;
}
static void svc_get_thread_context3(uint64_t *r) {
    void *out = guest_ptr(r[0], sizeof(ThreadContext));
    pthread_mutex_lock(&G);
    Thread *t = thread_of((uint32_t)r[1]);
    uint32_t res = 0;
    if (!t) res = R_INVALID_HANDLE;
    else if (t == self_or_null()) res = R_BUSY;
    else if (!t->paused) res = R_INVALID_STATE;
    else {
        if (!stopped_in_svc(t)) wait_parked(t);
        if (!stopped_in_svc(t)) res = R_INVALID_STATE; /* still in guest code (cooperative: it has not reached an svc yet) */
        else if (!out) res = R_INVALID_POINTER;
        else {
            ThreadContext c;
            fill_context(t, &c);
            memcpy(out, &c, sizeof c);
        }
    }
    pthread_mutex_unlock(&G);
    r[0] = res;
}
/* GetLastThreadInfo -> x1..x4 {fp, sp, lr, pc} of the thread that ran on this core before the caller, x5 its TLR, w6 flags (bit 0: it is
 * inside an svc). Outside an svc only the frame recorded at its last svc is known. */
static void svc_get_last_thread_info(uint64_t *r) {
    Thread *me = self();
    pthread_mutex_lock(&G);
    Thread *p = me->cur_core >= 0 ? g_core_prev[me->cur_core] : NULL;
    if (!p) {
        pthread_mutex_unlock(&G);
        r[0] = R_NO_THREAD;
        return;
    }
    if (stopped_in_svc(p)) {
        const uint64_t *g = p->svc_regs;
        r[1] = g[29];
        r[2] = g[KREG_GUEST_SP] + 16 + 160;
        r[3] = g[30];
        r[4] = svc_site_pc(p);
        r[6] = 1;
    } else {
        r[1] = p->last_fp;
        r[2] = p->last_sp;
        r[3] = p->last_lr;
        r[4] = p->last_pc;
        r[6] = 0;
    }
    r[5] = (uintptr_t)p->tlr;
    pthread_mutex_unlock(&G);
    r[0] = 0;
}
/* SynchronizePreemptionState: the thread finished a preemption-disabled section (TLR disable_count back to 0) after the kernel flagged a
 * deferred switch; clear the flag and take the switch now. */
static void svc_synchronize_preemption_state(uint64_t *r) {
    Thread *t = self();
    volatile uint16_t *flag = tlr_word(t, TLR_INTERRUPT_FLAG);
    if (flag) *flag = 0;
    pthread_mutex_lock(&G);
    if (t->preempt) {
        t->preempt = 0;
        core_put(t);
        core_take(t);
    }
    pthread_mutex_unlock(&G);
    r[0] = 0;
}

static void svc_arbitrate_lock(uint64_t *r) {
    Thread *t = self();
    uint64_t addr = r[1];
    if (!user_word_ok(addr)) {
        r[0] = R_INVALID_ADDRESS;
        return;
    }
    pthread_mutex_lock(&G);
    int64_t res = 0;
    if (ld32(addr) == ((uint32_t)r[0] | WAITERS_BIT)) {
        t->waddr = addr;
        t->wtag = (uint32_t)r[2];
        res = block(t, 1, -1);
    }
    pthread_mutex_unlock(&G);
    r[0] = (uint64_t)res;
}
static void svc_arbitrate_unlock(uint64_t *r) {
    if (!user_word_ok(r[0])) {
        r[0] = R_INVALID_ADDRESS;
        return;
    }
    pthread_mutex_lock(&G);
    lock_handoff(r[0]);
    pthread_mutex_unlock(&G);
    r[0] = 0;
}
/* WaitProcessWideKeyAtomic(x0 lock address, x1 condvar key, w2 tag, x3 timeout). The key is set to 1 before the mutex is
 * released: the SDK's Signal reads the key in user mode and skips svc 0x1D when it is 0, and taking the mutex in user mode
 * does not go through G either, so the opposite order loses wake-ups. */
static void svc_wait_process_wide_key(uint64_t *r) {
    Thread *t = self();
    uint64_t addr = r[0], key = r[1];
    if (!user_word_ok(addr) || !user_word_ok(key)) {
        r[0] = R_INVALID_ADDRESS;
        return;
    }
    pthread_mutex_lock(&G);
    st32(key, 1);
    t->wkey = key;
    t->waddr = addr;
    t->wtag = (uint32_t)r[2];
    lock_handoff(addr);
    int64_t res = block(t, 2, (int64_t)r[3]); /* on timeout the lock is not re-taken here: user mode takes it */
    pthread_mutex_unlock(&G);
    r[0] = (uint64_t)res;
}
static void svc_signal_process_wide_key(uint64_t *r) {
    uint64_t key = r[0];
    int cnt = (int)r[1], n = 0;
    pthread_mutex_lock(&G);
    for (Thread *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
    int left = 0;
    for (Thread *w = g_wait; w; w = w->next)
        if (w->wkind == 2 && w->wkey == key) {
            left = 1;
            break;
        }
    if (!left && user_word_ok(key)) st32(key, 0);
    pthread_mutex_unlock(&G);
    r[0] = 0;
}
/* WaitForAddress(x0 address, w1 arbitration type: 0 wait if less than, 1 decrement-and-wait if less than, 2 wait if equal, 3 wait if equal (64-bit); w2 value, x3 timeout) */
static void svc_wait_for_address(uint64_t *r) {
    Thread *t = self();
    uint64_t addr = r[0];
    int type = (int)r[1];
    int32_t val = (int32_t)r[2];
    if (!user_word_ok(addr)) {
        r[0] = R_INVALID_ADDRESS;
        return;
    }
    if (type < 0 || type > 3) {
        r[0] = R_INVALID_ENUM;
        return;
    }
    pthread_mutex_lock(&G);
    int64_t res;
    uint32_t v = ld32(addr);
    /* type 3 (Horizon 18, SDK 19 nn::os::WaitForAddress(const long *, long)): wait if the 64-bit value equals */
    int ok = type == 3 ? *(volatile uint64_t *)(uintptr_t)addr == r[2] : type == 2 ? (int32_t)v == val : (int32_t)v < val;
    if (ok && type == 1) {
        while (!cas32(addr, &v, v - 1))
            if (!((int32_t)v < val)) {
                ok = 0;
                break;
            }
    }
    if (!ok) res = R_INVALID_STATE;
    else {
        t->waddr = addr;
        res = block(t, 3, (int64_t)r[3]);
    }
    pthread_mutex_unlock(&G);
    r[0] = (uint64_t)res;
}
/* SignalToAddress(x0 address, w1 signal type: 0 signal, 1 increment-if-equal, 2 modify-by-waiter-count-if-equal; w2 value, w3 count) */
static void svc_signal_to_address(uint64_t *r) {
    uint64_t addr = r[0];
    int type = (int)r[1];
    int32_t val = (int32_t)r[2];
    int cnt = (int)r[3];
    if (!user_word_ok(addr)) {
        r[0] = R_INVALID_ADDRESS;
        return;
    }
    if (type < 0 || type > 2) {
        r[0] = R_INVALID_ENUM;
        return;
    }
    pthread_mutex_lock(&G);
    int nw = 0;
    for (Thread *w = g_wait; w; w = w->next)
        if (w->wkind == 3 && w->waddr == addr) nw++;
    int64_t res = 0;
    if (type != 0) {
        int32_t nv = val + 1;
        if (type == 2) {
            if (cnt <= 0)
                nv = nw > 0 ? val - 2 : val + 1; /* mesosphere KAddressArbiter: value - 2 with waiters and no count limit (found by the nxnative review, K1) */
            else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
        }
        uint32_t e = (uint32_t)val;
        if (!cas32(addr, &e, (uint32_t)nv)) res = R_INVALID_STATE;
    }
    if (!res) {
        int n = 0;
        for (Thread *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0);
    }
    pthread_mutex_unlock(&G);
    r[0] = (uint64_t)res;
}

static void svc_create_event(uint64_t *r) {
    uint32_t rd = 0;
    pthread_mutex_lock(&G);
    uint32_t w = event_create(&rd);
    pthread_mutex_unlock(&G);
    r[0] = 0;
    r[1] = w;
    r[2] = rd;
}
/* SignalEvent accepts only the writable end, ResetSignal only the readable end, ClearEvent either. */
static void svc_event_op(unsigned num, uint64_t *r) {
    uint32_t h = (uint32_t)r[0];
    pthread_mutex_lock(&G);
    Event *w = handle_get(h, OBJ_EVENT_W), *rd = handle_get(h, OBJ_EVENT_R);
    Event *e = num == 0x11 ? w : num == 0x17 ? rd : (w ? w : rd);
    uint32_t res = 0;
    if (!e) res = (num == 0x17 && handle_get(h, OBJ_THREAD)) ? R_INVALID_STATE : R_INVALID_HANDLE;
    else if (num == 0x11) {
        e->signaled = 1;
        wake_sync_waiters();
    } else if (num == 0x17 && !e->signaled) res = R_INVALID_STATE;
    else e->signaled = 0;
    pthread_mutex_unlock(&G);
    r[0] = res;
}
static void svc_signal_event(uint64_t *r) { svc_event_op(0x11, r); }
static void svc_clear_event(uint64_t *r) { svc_event_op(0x12, r); }
static void svc_reset_signal(uint64_t *r) { svc_event_op(0x17, r); }

int64_t kthread_wait_handles(Thread *t, const uint32_t *hs, int n, int64_t ns, int *idx) {
    if (ns > TIMEOUT_CAP_NS) ns = TIMEOUT_CAP_NS;
    int64_t deadline = ns > 0 ? mono_ns() + ns : 0, res;
    *idx = -1;
    t->nwh = n;
    for (int i = 0; i < n && i < 4; i++) t->wh[i] = hs[i];
    for (;;) {
        if (t->cancel) {
            t->cancel = 0;
            res = R_CANCELLED;
            break;
        }
        int bad = 0;
        for (int i = 0; i < n && *idx < 0; i++) {
            int s = sync_state(hs[i]);
            if (s < 0) bad = 1;
            else if (s) *idx = i;
        }
        if (*idx >= 0) {
            res = 0;
            break;
        }
        if (bad) {
            res = R_INVALID_HANDLE;
            break;
        }
        int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - mono_ns();
        if (ns >= 0 && left <= 0) {
            res = R_TIMED_OUT;
            break;
        }
        if (block(t, KW_SYNC, left) == R_TIMED_OUT) {
            res = R_TIMED_OUT;
            break;
        }
    }
    t->nwh = 0;
    return res;
}
static void svc_wait_synchronization(uint64_t *r) {
    Thread *t = self();
    uint64_t table = r[1];
    int n = (int)r[2];
    int64_t ns = (int64_t)r[3];
    if (n < 0 || n > 64 || (!table && n)) {
        r[0] = R_OUT_OF_RESOURCE;
        return;
    }
    uint32_t hs[64];
    if (n) {
        const void *p = guest_ptr(table, (uint64_t)n * 4);
        if (!p) {
            r[0] = R_INVALID_POINTER;
            return;
        }
        memcpy(hs, p, (size_t)n * 4);
    }
    int idx;
    pthread_mutex_lock(&G);
    int64_t res = kthread_wait_handles(t, hs, n, ns, &idx);
    pthread_mutex_unlock(&G);
    r[0] = (uint64_t)res;
    r[1] = idx >= 0 ? (uint64_t)idx : 0;
}
static void svc_cancel_synchronization(uint64_t *r) {
    pthread_mutex_lock(&G);
    Thread *t = thread_of((uint32_t)r[0]);
    if (t) {
        t->cancel = 1;
        if (t->wkind == 4) wake(t, -1);
    }
    pthread_mutex_unlock(&G);
    r[0] = t ? 0 : R_INVALID_HANDLE;
}

void threads_checkpoint(void) { core_checkpoint(); }
/* svc entry / exit (kernel.c): records where the thread is, and stops it there when it was paused. */
void threads_svc_enter(unsigned num, uint64_t *regs, const char *module, uint64_t offset) {
    Thread *me = self_or_null();
    if (!me) return;
    me->last_svc = (int)num;
    me->last_lr = regs[30];
    me->last_fp = regs[29];
    me->nsvc++;
    me->svc_module = module;
    me->svc_offset = offset;
    me->svc_x18 = (uintptr_t)pthread_getspecific(runtime_keys[0]);
    me->svc_tpidr = (uintptr_t)pthread_getspecific(runtime_keys[1]);
    __atomic_store_n(&me->svc_regs, regs, __ATOMIC_RELEASE);
    if (me->paused) {
        pthread_mutex_lock(&G);
        park_if_paused(me);
        pthread_mutex_unlock(&G);
    }
}
void threads_svc_exit(void) {
    Thread *me = self_or_null();
    if (!me) return;
    if (me->paused) {
        pthread_mutex_lock(&G);
        park_if_paused(me);
        pthread_mutex_unlock(&G);
    }
    if (me->preempt) core_checkpoint(); /* a wake / start above handed this core to a better thread: it runs before the caller continues */
    const uint64_t *g = me->svc_regs;
    if (g) {
        me->last_fp = g[29];
        me->last_lr = g[30];
        me->last_sp = g[KREG_GUEST_SP] + 16 + 160;
        me->last_pc = svc_site_pc(me);
    }
    __atomic_store_n(&me->svc_regs, NULL, __ATOMIC_RELEASE); /* no lock on the hot path: readers only trust it for blocked or parked threads */
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
    svc_register(0x2F, "GetLastThreadInfo", svc_get_last_thread_info);
    svc_register(0x33, "GetThreadContext3", svc_get_thread_context3);
    svc_register(0x36, "SynchronizePreemptionState", svc_synchronize_preemption_state);
}
