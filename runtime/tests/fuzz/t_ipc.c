/* Fuzz target: the HIPC/CMIF/TIPC request decoder of ipc.c (dispatch, parse, domain messages, buffer descriptors A/B/C/X/W, handle and pid descriptors) and the
 * command handlers of the services behind it (every service that builds on Linux with the host stand-ins: tests/host, tests/linux).
 *
 * Input: up to 8 records, then the guest memory the buffer descriptors point into:
 *     [service u8][flags u8][len u16][message bytes]...   rest: copied to the start of the guest arena
 * A record is one 0x100-byte message buffer as the guest's TLR holds it (zero padded after `len`). `service` picks the root object of the session (0 = the
 * service manager "sm:", n = the nth registered service); flags: bit 0 converts the session to a domain first, bit 1 picks a session the target opened earlier
 * from a service manager reply (instead of a fresh one), bits 2-3 select which. runtime_fail (the runtime's way of rejecting a request it cannot serve) is a
 * clean outcome here: it unwinds to the harness. Guest memory is an arena at a fixed address; descriptor addresses are folded into it. */
#include "../../src/ipc.c"
#include "fuzz.h"
#include "host_stubs.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif
#define ARENA_BASE 0x20000000ULL
#define ARENA_SIZE (1u << 18)
#define MAX_RECORDS 8

extern jmp_buf *fuzz_jmp;
void svc_kernel_register(void);
void svc_kcode_register(void);
void svc_threads_register(void);

/* Guest memory: one arena. These replace the pass-through versions of tests/host/host_stubs.c (the first definition wins at link time). */
uint64_t guest_addr_to_host(uint64_t a) { return ARENA_BASE + (a & (ARENA_SIZE - 1)); }
uint64_t host_addr_to_guest(uint64_t a) { return a >= ARENA_BASE && a < ARENA_BASE + ARENA_SIZE ? a - ARENA_BASE : a; }
void *guest_ptr(uint64_t addr, uint64_t size) {
    if (addr < ARENA_BASE || addr > ARENA_BASE + ARENA_SIZE || size > ARENA_BASE + ARENA_SIZE - addr) return NULL;
    return (void *)(uintptr_t)addr;
}
int memory_contains(uint64_t addr, uint64_t size) { return guest_ptr(addr, size) != NULL; }

#define REGISTER_LIST(X)                                                                                                                                       \
    X(lm)                                                                                                                                                      \
    X(apm)                                                                                                                                                     \
    X(applet) X(applet_ae) X(fatal) X(mii_img) X(firmware_stubs) X(fs) X(settings) X(account) X(misc) X(time) X(vi) X(pl) X(offline) X(nvdrv) X(ovln) X(btm)   \
        X(ngc) X(fgm) X(ectx) X(olsc) X(push) X(setfd) X(pdm) X(hidbus) X(irs) X(ldn) X(bcat) X(spsm) X(sfdnsres) X(ns_vm) X(ns) X(ncm) X(caps) X(mii) X(lbl)  \
            X(psm) X(audrec)
#define DECLARE(n) void service_register_##n(void);
REGISTER_LIST(DECLARE)
#define CALL(n) service_register_##n();
static uint8_t *arena;
static uint8_t tlr[0x200];

/* A runtime_fail unwinds past services that hold a lock (the real runtime ends the process there). The pthread mutex calls are wrapped (link flags --wrap) to
 * remember what this thread holds, so the harness can release it after an unwind instead of deadlocking on the next input. */
extern int __real_pthread_mutex_lock(pthread_mutex_t *);
extern int __real_pthread_mutex_unlock(pthread_mutex_t *);
static __thread pthread_mutex_t *held[64];
static __thread int nheld;
int __wrap_pthread_mutex_lock(pthread_mutex_t *m) {
    int r = __real_pthread_mutex_lock(m);
    if (!r && nheld < 64) held[nheld++] = m;
    return r;
}
int __wrap_pthread_mutex_unlock(pthread_mutex_t *m) {
    for (int i = nheld; i-- > 0;)
        if (held[i] == m) {
            memmove(held + i, held + i + 1, (size_t)(nheld - i - 1) * sizeof *held);
            nheld--;
            break;
        }
    return __real_pthread_mutex_unlock(m);
}
static void unlock_held(void) {
    while (nheld > 0) __real_pthread_mutex_unlock(held[--nheld]);
}

static void init_once(void) {
    static int done;
    if (done) return;
    done = 1;
    arena = mmap((void *)ARENA_BASE, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (arena != (void *)ARENA_BASE) {
        fprintf(stderr, "ipc fuzz: cannot map the guest arena at %#llx\n", (unsigned long long)ARENA_BASE);
        exit(2);
    }
    static char save_root[600], data_dir[600];
    snprintf(save_root, sizeof save_root, "%s/save", fuzz_workdir());
    snprintf(data_dir, sizeof data_dir, "%s/data", fuzz_workdir());
    g_cfg.save_root = save_root;
    g_cfg.support_dir = save_root;
    g_cfg.data_dir = data_dir;
    g_cfg.op_mode_handheld = 1;
    snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "0100000000010000");
    snprintf(g_cfg.build_id, sizeof g_cfg.build_id, "0123456789abcdef");
    host_thread_attach(tlr);
    svc_kernel_register();
    svc_kcode_register();
    svc_threads_register();
    svc_ipc_register();
    REGISTER_LIST(CALL)
}

typedef struct {
    IpcSession *s;
    uint32_t handle; /* 0: the harness owns the session */
} Sess;
static Sess pool[16];
static unsigned npool;

int threads_close_object(ObjType type, void *obj);
int ksession_close_object(ObjType type, void *obj);
int kcode_close_object(ObjType type, void *obj);
/* CloseHandle as kernel.c does it (not through svc_dispatch: it records the caller's registers in the thread, which a runtime_fail unwind leaves dangling). */
static void close_handle(uint32_t h) {
    ObjType type = OBJ_FREE;
    void *o = handle_close(h, &type);
    if (o && o != (void *)1 && !threads_close_object(type, o) && !ksession_close_object(type, o) && !kcode_close_object(type, o)) ipc_close_object(type, o);
}
/* The handles in a reply message: sessions are kept for later records, everything else is closed (the services close their own ends later, harmlessly). */
static void take_reply_handles(const uint32_t *m) {
    if (!(m[1] >> 31)) return;
    unsigned nc = (m[2] >> 1) & 0xf, nm = (m[2] >> 5) & 0xf;
    for (unsigned i = 0; i < nc + nm && 3 + i < 64; i++) {
        uint32_t h = m[3 + i];
        IpcSession *s = handle_get(h, OBJ_SESSION);
        if (s && i >= nc && npool < sizeof pool / sizeof *pool) pool[npool++] = (Sess){s, h};
        else close_handle(h);
    }
}
static IpcObject *open_service(unsigned sel) {
    if (sel == 0 || nservices == 0) return sm_open();
    return services[(sel - 1) % nservices].open();
}

static int fuzz_ipc_run(const uint8_t *d, size_t n) {
    init_once();
    typedef struct {
        unsigned svc, flags;
        uint8_t msg[0x100];
    } Rec;
    Rec recs[MAX_RECORDS];
    unsigned nrec = 0;
    FzIn r = {d, n, 0};
    while (nrec < MAX_RECORDS && fz_left(&r) >= 4) {
        Rec *rc = &recs[nrec++];
        rc->svc = fz_u8(&r);
        rc->flags = fz_u8(&r);
        size_t len = fz_u16(&r);
        if (len > 0x100) len = 0x100;
        if (len > fz_left(&r)) len = fz_left(&r);
        memset(rc->msg, 0, sizeof rc->msg);
        memcpy(rc->msg, d + r.at, len);
        r.at += len;
    }
    memset(arena, 0, ARENA_SIZE);
    size_t rest = fz_left(&r);
    memcpy(arena, d + r.at, rest < ARENA_SIZE ? rest : ARENA_SIZE);

    npool = 0;
    int failed = 0;
    for (unsigned i = 0; i < nrec; i++) {
        Rec *rc = &recs[i];
        jmp_buf jb;
        Sess cur = {0, 0};
        uint32_t *m = malloc(0x100); /* exactly the message buffer: reads past it are reported */
        memcpy(m, rc->msg, 0x100);
        if (setjmp(jb) != 0) failed = 1, unlock_held();
        else {
            fuzz_jmp = &jb;
            if ((rc->flags & 2) && npool) cur = pool[(rc->flags >> 2) % npool];
            else {
                cur.s = session_new(open_service(rc->svc));
                if (npool < sizeof pool / sizeof *pool) pool[npool++] = cur;
            }
            if (rc->flags & 1) { /* convert to a domain, as the guest's first request on a domain-capable object does */
                IpcCall c;
                memset(&c, 0, sizeof c);
                control(cur.s, &c, NULL);
            }
            dispatch(cur.s, m);
            take_reply_handles(m);
        }
        fuzz_jmp = NULL;
        free(m);
        if (failed) break; /* runtime_fail ends the process in the real runtime; a lock the service held is not released either */
    }
    for (unsigned i = 0; i < npool; i++) {
        if (pool[i].handle) close_handle(pool[i].handle);
        else session_free(pool[i].s);
    }
    return 0;
}

/* The services keep process-wide state (locks, tables, files). By default inputs run in-process one after the other (fast: state carries over, and a crash may need
 * the inputs before it: the driver saves the last four). FUZZ_FORK=1 runs every input in a child process instead: slow (ASan makes a fork cost milliseconds), but
 * state never leaks from one input into the next, which tells a real crash from one that needs earlier state. The coverage the child reaches comes back through
 * the shared map of fuzz_mut.c. */
int fuzz_ipc(const uint8_t *d, size_t n) {
    static int fork_mode = -1;
    if (fork_mode < 0) fork_mode = getenv("FUZZ_FORK") != NULL;
    if (!fork_mode) return fuzz_ipc_run(d, n);
    init_once(); /* the parent maps the arena and registers the services once */
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        alarm(getenv("FUZZ_TIMEOUT") ? (unsigned)atoi(getenv("FUZZ_TIMEOUT"))
                                     : 20); /* alarms are not inherited: the child's own hang report (with its stack) */
        fuzz_ipc_run(d, n);
        _exit(0);
    }
    int st = 0;
    alarm(0); /* the parent only waits */
    waitpid(pid, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 0;
    _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 70); /* the child already printed its report and saved the input (death callback / signal handler) */
}

/* ---- seeds: a valid request for every command of every service, with and without buffers, plus domain and control messages ------------------------------ */
static size_t build_msg(uint8_t *out, unsigned type, unsigned cmd, unsigned argbytes, int bufs, int domain, int fill) {
    uint32_t *m = (uint32_t *)out;
    memset(out, 0, 0x100);
    unsigned nx = bufs ? 1 : 0, na = bufs ? 1 : 0, nb = bufs ? 1 : 0, nrecv = bufs ? 1 : 0;
    m[0] = type | nx << 16 | na << 20 | nb << 24;
    unsigned w = 2;
    uint64_t base = ARENA_BASE;
    for (unsigned i = 0; i < nx; i++, w += 2) { /* X: index 0, size 0x40 at arena+0x400 */
        uint64_t a = base + 0x400;
        m[w] = 0 | (uint32_t)(((a >> 36) & 0x3f) << 6) | (uint32_t)(((a >> 32) & 0xf) << 12) | 0x40u << 16;
        m[w + 1] = (uint32_t)a;
    }
    for (unsigned k = 0; k < na + nb; k++, w += 3) { /* A at arena+0x100 (0x100 bytes), B at arena+0x800 (0x100 bytes) */
        uint64_t a = base + (k < na ? 0x100 : 0x800);
        m[w] = 0x100;
        m[w + 1] = (uint32_t)a;
        m[w + 2] = 1 | (uint32_t)(((a >> 32) & 0xf) << 28);
    }
    unsigned data_off = w * 4, payload_off = (data_off + 15) & ~15u;
    unsigned head = domain ? 16 : 0, total = payload_off - data_off + head + 16 + argbytes;
    unsigned ndata = (total + 3) / 4;
    m[1] = ndata | (nrecv ? 2u << 10 : 0);
    uint8_t *p = out + payload_off;
    if (domain) {
        p[0] = 1; /* send */
        fz_put16(p + 2, (uint16_t)(16 + argbytes));
        fz_put32(p + 4, 1); /* the converted root object */
        p += 16;
    }
    fz_put32(p, 0x49434653u); /* SFCI */
    fz_put32(p + 8, cmd);
    for (unsigned i = 0; i < argbytes; i++)
        p[16 + i] = fill == 0 ? 0x11 : fill == 1 ? (uint8_t)((i & 3) == 0 ? i / 4 + 1 : 0) : 0; /* bytes / small counters / zeros */
    unsigned end = data_off + ndata * 4;
    if (nrecv) { /* receive list: one C buffer at arena+0xc00, 0x100 bytes */
        uint32_t *rl = (uint32_t *)(out + end);
        rl[0] = (uint32_t)(base + 0xc00);
        rl[1] = 0x100u << 16;
        end += 8;
    }
    return end;
}
static void add_record(FuzzSeedFn add, void *ctx, unsigned svc, unsigned flags, const uint8_t *msg, size_t len, const uint8_t *tail, size_t tail_len) {
    uint8_t *b = malloc(4 + len + tail_len);
    b[0] = (uint8_t)svc;
    b[1] = (uint8_t)flags;
    fz_put16(b + 2, (uint16_t)len);
    memcpy(b + 4, msg, len);
    memcpy(b + 4 + len, tail, tail_len);
    add(ctx, b, 4 + len + tail_len);
    free(b);
}
void fuzz_ipc_seeds(FuzzSeedFn add, void *ctx) {
    init_once();
    uint8_t msg[0x100], tail[0x200];
    for (unsigned i = 0; i < sizeof tail; i++) tail[i] = (uint8_t)(i * 7 + (i >> 5));
    /* the service manager: Initialize, GetServiceHandle("set"), GetServiceHandle("fsp-srv") */
    size_t len = build_msg(msg, 4, 1, 8, 0, 0, 0);
    memcpy(msg + 0x30, "fsp-srv", 8);
    for (unsigned i = 0; i < 2; i++) add_record(add, ctx, 0, 0, msg, len, tail, sizeof tail);
    len = build_msg(msg, 4, 0, 8, 0, 0, 0);
    add_record(add, ctx, 0, 0, msg, len, tail, sizeof tail);
    /* control messages: ConvertCurrentObjectToDomain (0), CloneCurrentObject (2), QueryPointerBufferSize (3), CloneCurrentObjectEx (4) */
    for (unsigned cmd = 0; cmd <= 4; cmd++) {
        len = build_msg(msg, 5, cmd, 4, 0, 0, 0);
        add_record(add, ctx, 0, 0, msg, len, tail, 0);
    }
    /* TIPC: the command is the message type minus 16 */
    for (unsigned cmd = 0; cmd < 4; cmd++) {
        memset(msg, 0, sizeof msg);
        fz_put32(msg, 16 + cmd);
        fz_put32(msg + 4, 4);
        memset(msg + 8, 0x22, 16);
        add_record(add, ctx, 0, 0, msg, 8 + 16, tail, 0);
    }
    /* every command of every registered service */
    for (unsigned s = 0; s < nservices; s++) {
        jmp_buf jb;
        IpcObject *o = NULL;
        if (setjmp(jb) == 0) {
            fuzz_jmp = &jb;
            o = services[s].open();
        }
        fuzz_jmp = NULL;
        if (!o) continue;
        for (size_t c = 0; c < o->ncmds; c++) {
            unsigned cmd = o->cmds[c].cmd;
            for (int fill = 0; fill < 3; fill++) {
                len = build_msg(msg, 4, cmd, 0x30, 0, 0, fill);
                add_record(add, ctx, s + 1, 0, msg, len, tail, sizeof tail);
                len = build_msg(msg, 4, cmd, 0x30, 1, 0, fill);
                add_record(add, ctx, s + 1, 0, msg, len, tail, sizeof tail);
            }
            if (c < 4) { /* a few through a domain */
                len = build_msg(msg, 4, cmd, 0x30, 1, 1, 0);
                add_record(add, ctx, s + 1, 1, msg, len, tail, sizeof tail);
            }
        }
        object_unref(o);
    }
}
FUZZ_TARGET(ipc);
