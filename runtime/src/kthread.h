/* Kernel-internal thread state shared by threads.c (scheduling, waits) and ksession.c (sessions, ports). Not for services: they use the
 * event/thread functions of napple.h. Everything here is protected by the one kernel lock (`G`). */
#pragma once
#include "napple.h"
#include "kmsg.h"

/* Wait kinds (Thread.wkind) */
enum { KW_NONE = 0, KW_LOCK = 1, KW_CONDVAR = 2, KW_ADDRESS = 3, KW_SYNC = 4, KW_IPC_REPLY = 5, KW_PARKED = 6 };

typedef struct Thread {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top, host_context[2];
    int prio, core; uint64_t mask;
    int started, exited, cancel, closed;
    pthread_t pt; pthread_cond_t cv;
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres, wseq;
    struct Thread *next, *all;
    int last_svc; uint64_t last_lr, last_fp, last_sp, last_pc, nsvc;
    int cur_core, preempt; uint64_t cseq, core_since; struct Thread *cnext;
    uint32_t wh[4]; int nwh;
    /* svc context: valid while the thread is inside an svc (GetThreadContext3, GetLastThreadInfo) */
    uint64_t *svc_regs; const char *svc_module; uint64_t svc_offset, svc_x18, svc_tpidr;
    void *tlr;
    int paused, parked;                  /* SetThreadActivity(Paused): the thread stops at its next svc boundary */
    /* kernel IPC (ksession.c): the request this thread waits on as a client */
    struct Thread *ipc_next; void *ipc_session; uint32_t *ipc_msg; uint32_t ipc_msg_size; uint64_t *ipc_light; KRecvList ipc_recv;
} Thread;

/* gate.S register area: x0..x30 at [0..30], q0..q31 at byte 256, nzcv/fpsr/fpcr at [96..98], the gate's guest stack pointer at [100]
 * (the svc site's sp is that + 16 + 160, see docs/rewrite-format.md). */
#define KREG_NZCV 96
#define KREG_FPSR 97
#define KREG_FPCR 98
#define KREG_GUEST_SP 100
#define KREG_COUNT 101

extern pthread_mutex_t kernel_lock;
#define G kernel_lock
Thread *kthread_self(void);              /* fails the runtime on a host thread */
Thread *kthread_self_or_null(void);
/* Suspends t (G held) until woken or timed out (ns < 0 forever, 0 immediately); returns the wake result or R_TIMED_OUT. Gives the
 * virtual core up while blocked. */
int64_t kthread_block(Thread *t, int kind, int64_t ns);
void kthread_wake(Thread *t, int64_t res);
void kthread_wake_sync_waiters(void);    /* every WaitSynchronization-style waiter re-checks its handles */
/* WaitSynchronization over hs[0..n) (G held): 0 and *idx, R_TIMED_OUT, R_CANCELLED or R_INVALID_HANDLE. */
int64_t kthread_wait_handles(Thread *t, const uint32_t *hs, int n, int64_t ns, int *idx);

/* ksession.c */
int ksession_sync_state(uint32_t h);     /* 1 signaled, 0 not, -1 not a kernel IPC object (G held) */
int ksession_close_object(ObjType type, void *obj);
