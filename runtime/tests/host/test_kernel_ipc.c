/* Host unit tests: HIPC message transfer (kmsg.c), guest-to-guest sessions, light sessions and ports (ksession.c), code memory checks
 * (kcode.c), thread pause / GetThreadContext3 / GetLastThreadInfo / SynchronizePreemptionState (threads.c). Real host threads are adopted
 * as guest threads and call the svc handlers through svc_dispatch, so the blocking paths run for real. */
#include "napple.h"
#include "kmsg.h"
#include "kthread.h"
#include "host_stubs.h"
#include "../check.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_FIXED_NOREPLACE   /* Linux only; elsewhere the address is a hint (macOS keeps it when free) */
#define MAP_FIXED_NOREPLACE 0
#endif

void svc_kernel_register(void);
void svc_kcode_register(void);
int ksession_connect_named(const char *name, uint64_t *r);
uint32_t ksession_send_request(uint32_t h, uint32_t *msg, uint32_t size);

static uint64_t svc(unsigned num, uint64_t *r) { svc_dispatch(num, r, "test", 0x40); return r[0]; }
static void *ptr_any(uint64_t a, uint64_t n) { return guest_ptr(a, n); }

/* ---- kmsg -------------------------------------------------------------------------------------------------------------- */
/* Guest addresses fit 39 bits (HIPC pointer descriptors carry no more): test buffers live in a low mapping like guest memory does. */
static uint8_t *low_mem(size_t n) {
    static uint8_t *base; static size_t used;
    if (!base) { base = mmap((void *)0x20000000, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0); if (base == MAP_FAILED) { perror("mmap"); exit(2); } }
    uint8_t *p = base + used; used += (n + 63) & ~(size_t)63; memset(p, 0, n); return p;
}
static void test_kmsg(void) {
    uint32_t src[64] = {0}, dst[64] = {0};
    uint8_t (*xdata)[32] = (void *)low_mem(64), (*recv)[64] = (void *)low_mem(128);
    memset(xdata[0], 0xA1, 24); memset(xdata[1], 0xB2, 8);
    /* type 4, 2 X descriptors, 3 data words, special header with pid and 1 copy handle */
    src[0] = 4 | (2u << 16); src[1] = 3 | (1u << 31);
    src[2] = 1 | (1u << 1); src[3] = 0xdead; src[4] = 0xbeef; src[5] = 0x1234;
    uint64_t a0 = (uintptr_t)xdata[0], a1 = (uintptr_t)xdata[1];
    src[6] = 0 | (uint32_t)(((a0 >> 36) & 0x3f) << 6) | (uint32_t)(((a0 >> 32) & 0xf) << 12) | (24u << 16); src[7] = (uint32_t)a0;
    src[8] = 1 | (uint32_t)(((a1 >> 36) & 0x3f) << 6) | (uint32_t)(((a1 >> 32) & 0xf) << 12) | (8u << 16); src[9] = (uint32_t)a1;
    src[10] = 0x11; src[11] = 0x22; src[12] = 0x33;
    KMsgLayout l;
    CHECK(kmsg_layout(src, 0x100, &l) == 0 && l.nx == 2 && l.has_pid && l.ncopy == 1 && l.data_word == 10 && l.total_words == 13, "layout of a special message");
    CHECK(kmsg_layout(src, 40, &l) == R_MESSAGE_TOO_LARGE, "message larger than its buffer");

    /* receiver without a receive list: descriptors keep the sender's addresses, pid is the sender's */
    KRecvList none = {0};
    CHECK(kmsg_copy(dst, 0x100, &none, src, 0x100, 0x80, ptr_any) == 0, "copy without receive list");
    CHECK(dst[3] == 0x80 && dst[4] == 0 && dst[5] == 0x1234 && dst[7] == (uint32_t)a0 && dst[12] == 0x33, "pid written, handle and data passed through");

    /* receive list mode 4 (two indexed buffers): X data lands in the receiver's buffers, descriptors point there */
    uint32_t rbuf[64] = {0};
    rbuf[0] = 4; rbuf[1] = (4u << 10);
    uint64_t r0 = (uintptr_t)recv[0], r1 = (uintptr_t)recv[1];
    rbuf[2] = (uint32_t)r0; rbuf[3] = (uint32_t)((r0 >> 32) & 0xffff) | (64u << 16);
    rbuf[4] = (uint32_t)r1; rbuf[5] = (uint32_t)((r1 >> 32) & 0xffff) | (64u << 16);
    KRecvList rl; kmsg_recv_list(rbuf, 0x100, &rl);
    CHECK(rl.mode == 4 && rl.n == 2 && rl.addr[1] == r1 && rl.size[0] == 64, "receive list parsed");
    memset(dst, 0, sizeof dst);
    CHECK(kmsg_copy(dst, 0x100, &rl, src, 0x100, 0x80, ptr_any) == 0, "copy into indexed receive buffers");
    CHECK(recv[0][0] == 0xA1 && recv[0][23] == 0xA1 && recv[1][0] == 0xB2 && dst[7] == (uint32_t)r0 && dst[9] == (uint32_t)r1, "X data copied, descriptors rewritten");

    /* mode 2 (one buffer): packed, 16-byte aligned */
    memset(recv, 0, 128);
    KRecvList one = {.mode = 2, .n = 1, .addr = {r0}, .size = {64}};
    CHECK(kmsg_copy(dst, 0x100, &one, src, 0x100, 0x80, ptr_any) == 0 && recv[0][0] == 0xA1 && recv[0][32] == 0xB2 && dst[9] == (uint32_t)(r0 + 32), "single receive buffer packs X data");
    one.size[0] = 30;
    CHECK(kmsg_copy(dst, 0x100, &one, src, 0x100, 0x80, ptr_any) == R_RECEIVE_LIST_BROKEN, "X data larger than the receive buffer");
    KRecvList bad = {.mode = 3, .n = 1, .addr = {r0}, .size = {64}};
    CHECK(kmsg_copy(dst, 0x100, &bad, src, 0x100, 0x80, ptr_any) == R_RECEIVE_LIST_BROKEN, "X index without a receive buffer");
}

/* ---- guest threads ------------------------------------------------------------------------------------------------------- */
typedef struct { void (*fn)(void *); void *arg; _Atomic uint32_t handle; uint8_t tlr[0x200]; } Helper;
static void *helper_main(void *p) {
    Helper *h = p;
    atomic_store(&h->handle, host_thread_attach(h->tlr));
    h->fn(h->arg);
    uint64_t r[KREG_COUNT] = {0}; svc(0x0A, r);                     /* ExitThread */
    return NULL;
}
static Helper *spawn(void (*fn)(void *), void *arg) {
    Helper *h = calloc(1, sizeof *h); h->fn = fn; h->arg = arg;
    pthread_t t; pthread_create(&t, NULL, helper_main, h); pthread_detach(t);
    while (!atomic_load(&h->handle)) usleep(100);
    return h;
}
static int join(Helper *h) {                                       /* WaitSynchronization on the thread handle */
    uint32_t hd = atomic_load(&h->handle); uint64_t r[KREG_COUNT] = {0};
    r[1] = (uintptr_t)&hd; r[2] = 1; r[3] = 5000000000ll;
    return (int)svc(0x18, r);
}
static uint8_t main_tlr[0x200];
static uint32_t *tlr_msg(void) { return pthread_getspecific(runtime_keys[2]); }

/* ---- sessions -------------------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t server, client; _Atomic int stage; uint64_t result, reply_word; } Sess;
static void client_request(void *p) {
    Sess *s = p; uint32_t *m = tlr_msg();
    memset(m, 0, 0x100); m[0] = 4; m[1] = 2; m[2] = 0x49434653; m[3] = 77;   /* two data words */
    uint64_t r[KREG_COUNT] = {0}; r[0] = s->client;
    s->result = svc(0x21, r);
    s->reply_word = m[3];
    atomic_store(&s->stage, 1);
}
static void client_request_expect_closed(void *p) {
    Sess *s = p; uint32_t *m = tlr_msg();
    memset(m, 0, 0x100); m[0] = 4; m[1] = 1;
    atomic_store(&s->stage, 1);
    s->result = ksession_send_request(s->client, m, 0x100);
    atomic_store(&s->stage, 2);
}
static void test_sessions(void) {
    Sess s = {0}; uint64_t r[KREG_COUNT] = {0};
    CHECK(svc(0x40, r) == 0 && r[1] && r[2] && r[1] != r[2], "CreateSession");
    s.server = (uint32_t)r[1]; s.client = (uint32_t)r[2];
    memset(r, 0, sizeof r); r[1] = (uintptr_t)&s.server; r[2] = 1; r[3] = 0; r[4] = 0;
    CHECK(svc(0x43, r) == R_TIMED_OUT && (int64_t)r[1] == -1, "ReplyAndReceive with nothing pending times out");
    memset(r, 0, sizeof r); r[3] = s.server;
    CHECK(svc(0x43, r) == R_INVALID_STATE, "reply without a received request");

    Helper *c = spawn(client_request, &s);
    memset(r, 0, sizeof r); r[1] = (uintptr_t)&s.server; r[2] = 1; r[3] = 0; r[4] = (uint64_t)-1;
    uint64_t res = svc(0x43, r);
    uint32_t *m = tlr_msg();
    CHECK(res == 0 && r[1] == 0 && m[0] == 4 && m[2] == 0x49434653 && m[3] == 77, "server receives the client's message");
    CHECK(atomic_load(&s.stage) == 0, "client still waits for the reply");
    m[0] = 0; m[1] = 2; m[3] = 78;
    memset(r, 0, sizeof r); r[1] = (uintptr_t)&s.server; r[2] = 0; r[3] = s.server; r[4] = 0;
    CHECK(svc(0x43, r) == R_TIMED_OUT, "reply only (no handles, timeout 0) returns TimedOut after replying");
    CHECK(join(c) == 0 && s.result == 0 && s.reply_word == 78, "client got the reply");

    /* server closes while a client waits */
    s.stage = 0;
    c = spawn(client_request_expect_closed, &s);
    while (atomic_load(&s.stage) < 1) usleep(100);
    usleep(20000);
    memset(r, 0, sizeof r); r[0] = s.server; svc(0x16, r);
    CHECK(join(c) == 0 && s.result == R_SESSION_CLOSED, "closing the server fails the waiting request with SessionClosed");
    memset(r, 0, sizeof r); r[0] = s.client;
    CHECK(svc(0x21, r) == R_SESSION_CLOSED, "request on a session whose server is gone");
    memset(r, 0, sizeof r); r[0] = s.client; svc(0x16, r);

    /* client closes: the server end becomes signaled and receiving reports SessionClosed */
    memset(r, 0, sizeof r); svc(0x40, r); s.server = (uint32_t)r[1]; s.client = (uint32_t)r[2];
    memset(r, 0, sizeof r); r[0] = s.client; svc(0x16, r);
    memset(r, 0, sizeof r); r[1] = (uintptr_t)&s.server; r[2] = 1; r[4] = (uint64_t)-1;
    CHECK(svc(0x43, r) == R_SESSION_CLOSED && r[1] == 0, "closed client: ReplyAndReceive returns SessionClosed with its index");
    memset(r, 0, sizeof r); r[0] = s.server; svc(0x16, r);
    memset(r, 0, sizeof r); r[0] = s.server;
    CHECK(svc(0x16, r) == R_INVALID_HANDLE, "both ends closed");
}

/* ---- light sessions -------------------------------------------------------------------------------------------------------- */
static void light_client(void *p) {
    Sess *s = p; uint64_t r[KREG_COUNT] = {0};
    r[0] = s->client; for (int i = 1; i <= 7; i++) r[i] = 100 + i;
    s->result = svc(0x20, r);
    s->reply_word = r[1] + r[7];
    memset(r, 0, sizeof r); r[0] = s->client; svc(0x16, r);          /* ends the server's next wait */
}
static void test_light(void) {
    Sess s = {0}; uint64_t r[KREG_COUNT] = {0};
    r[2] = 1; svc(0x40, r); s.server = (uint32_t)r[1]; s.client = (uint32_t)r[2];
    memset(r, 0, sizeof r); r[0] = s.client;
    CHECK(svc(0x21, r) == R_INVALID_HANDLE, "a light session takes no HIPC request");
    Helper *c = spawn(light_client, &s);
    memset(r, 0, sizeof r); r[0] = s.server;
    CHECK(svc(0x42, r) == 0 && r[1] == 101 && r[7] == 107, "light server receives w1..w7");
    r[0] = s.server; r[1] = 0x80000000u | 5; r[7] = 9;
    CHECK(svc(0x42, r) == R_SESSION_CLOSED, "reply, then the closed client ends the wait");
    CHECK(join(c) == 0 && s.result == 0 && s.reply_word == (0x80000005ull + 9), "client got the light reply");
    memset(r, 0, sizeof r); r[0] = s.server; svc(0x16, r);
}

/* ---- ports ----------------------------------------------------------------------------------------------------------------- */
static void test_ports(void) {
    uint64_t r[KREG_COUNT] = {0};
    r[2] = 0;
    CHECK(svc(0x70, r) == R_OUT_OF_RANGE, "CreatePort with no sessions allowed");
    memset(r, 0, sizeof r); r[2] = 1;
    CHECK(svc(0x70, r) == 0, "CreatePort");
    uint32_t sp = (uint32_t)r[1], cp = (uint32_t)r[2];
    memset(r, 0, sizeof r); r[1] = sp;
    CHECK(svc(0x41, r) == R_NOT_FOUND, "AcceptSession with nobody connected");
    memset(r, 0, sizeof r); r[1] = cp;
    CHECK(svc(0x72, r) == 0 && r[1], "ConnectToPort");
    uint32_t client = (uint32_t)r[1];
    memset(r, 0, sizeof r); r[1] = cp;
    CHECK(svc(0x72, r) == R_OUT_OF_SESSIONS, "max sessions reached");
    memset(r, 0, sizeof r); r[1] = (uintptr_t)&sp; r[2] = 1; r[3] = 0;
    CHECK(svc(0x18, r) == 0, "server port signaled while a session waits to be accepted");
    memset(r, 0, sizeof r); r[1] = sp;
    CHECK(svc(0x41, r) == 0 && r[1], "AcceptSession");
    uint32_t server = (uint32_t)r[1];
    memset(r, 0, sizeof r); r[1] = (uintptr_t)&sp; r[2] = 1; r[3] = 0;
    CHECK(svc(0x18, r) == R_TIMED_OUT, "queue empty again");
    memset(r, 0, sizeof r); r[0] = client; svc(0x16, r);
    memset(r, 0, sizeof r); r[0] = server; svc(0x16, r);
    memset(r, 0, sizeof r); r[1] = cp;
    CHECK(svc(0x72, r) == 0, "a freed session gives its slot back");
    memset(r, 0, sizeof r); r[0] = r[1]; svc(0x16, r);
    memset(r, 0, sizeof r); r[0] = sp; svc(0x16, r);
    memset(r, 0, sizeof r); r[1] = cp;
    CHECK(svc(0x72, r) == R_PORT_CLOSED, "connect after the server port closed");
    memset(r, 0, sizeof r); r[0] = cp; svc(0x16, r);

    /* named ports */
    static const char name[12] = "napple:t";
    memset(r, 0, sizeof r); r[1] = (uintptr_t)name; r[2] = 4;
    CHECK(svc(0x71, r) == 0 && r[1], "ManageNamedPort registers");
    uint32_t nsp = (uint32_t)r[1];
    memset(r, 0, sizeof r); r[1] = (uintptr_t)name; r[2] = 4;
    CHECK(svc(0x71, r) == R_INVALID_STATE, "a name is registered once");
    uint64_t cr[2] = {0};
    CHECK(ksession_connect_named("napple:t", cr) && cr[0] == 0 && cr[1], "ConnectToNamedPort reaches the guest port");
    CHECK(!ksession_connect_named("nothere", cr), "unknown names are left to ipc.c");
    memset(r, 0, sizeof r); r[1] = nsp;
    CHECK(svc(0x41, r) == 0, "accept the named connection");
    uint32_t ns = (uint32_t)r[1];
    memset(r, 0, sizeof r); r[1] = (uintptr_t)name; r[2] = 0;
    CHECK(svc(0x71, r) == 0, "ManageNamedPort(0) removes the name");
    CHECK(!ksession_connect_named("napple:t", cr), "removed name no longer connects");
    uint32_t closes[] = {(uint32_t)cr[1], ns, nsp};
    for (unsigned i = 0; i < 3; i++) { memset(r, 0, sizeof r); r[0] = closes[i]; CHECK(svc(0x16, r) == 0, "close named-port objects"); }
}

/* ---- code memory ----------------------------------------------------------------------------------------------------------- */
static void test_code_memory(void) {
    static uint8_t heap[0x10000] __attribute__((aligned(0x4000)));
    host_code.heap = (uintptr_t)heap; host_code.heap_size = sizeof heap; host_code.free_lo = 0x100000000ull; host_code.free_hi = 0x200000000ull;
    uint64_t r[KREG_COUNT] = {0};
    r[1] = (uintptr_t)heap + 1; r[2] = 0x4000;
    CHECK(svc(0x4B, r) == R_INVALID_ADDRESS, "CreateCodeMemory: unaligned address");
    memset(r, 0, sizeof r); r[1] = (uintptr_t)heap; r[2] = 0;
    CHECK(svc(0x4B, r) == R_INVALID_SIZE, "CreateCodeMemory: zero size");
    memset(r, 0, sizeof r); r[1] = 0x300000000ull; r[2] = 0x4000;
    CHECK(svc(0x4B, r) == R_INVALID_CURRENT_MEMORY, "CreateCodeMemory: not heap memory");
    memset(r, 0, sizeof r); r[1] = (uintptr_t)heap; r[2] = 0x8000;
    CHECK(svc(0x4B, r) == 0 && r[1], "CreateCodeMemory");
    uint32_t h = (uint32_t)r[1];
    uint64_t c[KREG_COUNT];
#define CTL(op, addr, size, perm) (memset(c, 0, sizeof c), c[0] = h, c[1] = (op), c[2] = (addr), c[3] = (size), c[4] = (perm), svc(0x4C, c))
    CHECK(CTL(0, 0x100000000ull, 0x8000, 5) == R_INVALID_MEMORY_PERMISSIONS, "Map must be read-write");
    CHECK(CTL(1, 0x100010000ull, 0x8000, 3) == R_INVALID_MEMORY_PERMISSIONS, "MapToOwner must be R or RX");
    CHECK(CTL(0, 0x100000000ull, 0x4000, 3) == R_INVALID_SIZE, "size must match the code memory");
    CHECK(CTL(0, 0x300000000ull, 0x8000, 3) == R_INVALID_MEMORY_REGION, "alias outside free address space");
    CHECK(CTL(0, 0x100000000ull, 0x8000, 3) == 0 && host_code.aliases == 1 && host_code.last_perm == 3, "Map (RW alias)");
    CHECK(CTL(0, 0x100010000ull, 0x8000, 3) == R_INVALID_STATE, "Map twice");
    CHECK(CTL(1, 0x100004000ull, 0x8000, 5) == R_INVALID_MEMORY_REGION, "owner alias overlapping the RW alias");
    CHECK(CTL(1, 0x100010000ull, 0x8000, 5) == 0 && host_code.aliases == 2 && host_code.last_perm == 5, "MapToOwner (RX alias)");
    CHECK(CTL(9, 0x100010000ull, 0x8000, 5) == R_INVALID_ENUM, "unknown operation");
    CHECK(CTL(2, 0x100020000ull, 0x8000, 0) == R_INVALID_MEMORY_REGION, "Unmap at the wrong address");
    CHECK(CTL(2, 0x100000000ull, 0x8000, 0) == 0 && CTL(3, 0x100010000ull, 0x8000, 0) == 0 && host_code.aliases == 0, "Unmap / UnmapFromOwner");
    memset(r, 0, sizeof r); r[0] = h;
    CHECK(svc(0x16, r) == 0, "close the code memory");
#undef CTL
}

/* ---- pause, thread context, last thread, preemption state ------------------------------------------------------------------- */
typedef struct { uint32_t ev_r; _Atomic int in_wait; uint64_t result; } Waiter;
static void waiter_main(void *p) {
    Waiter *w = p; uint64_t r[KREG_COUNT] = {0};
    for (int i = 0; i < 29; i++) r[i] = 0x1000 + i;
    r[1] = (uintptr_t)&w->ev_r; r[2] = 1; r[3] = (uint64_t)-1; r[29] = 0xf00d; r[30] = 0xcafe; r[KREG_GUEST_SP] = 0x70000;
    atomic_store(&w->in_wait, 1);
    w->result = svc(0x18, r);
}
static void test_pause_and_context(void) {
    uint64_t r[KREG_COUNT] = {0};
    svc(0x45, r); uint32_t ev_w = (uint32_t)r[1], ev_r = (uint32_t)r[2];
    Waiter w = {.ev_r = ev_r};
    Helper *h = spawn(waiter_main, &w);
    uint32_t th = atomic_load(&h->handle);
    while (!atomic_load(&w.in_wait)) usleep(100);
    usleep(20000);
    static uint8_t ctx[0x320];
    memset(r, 0, sizeof r); r[0] = (uintptr_t)ctx; r[1] = th;
    CHECK(svc(0x33, r) == R_INVALID_STATE, "GetThreadContext3 needs a paused thread");
    memset(r, 0, sizeof r); r[0] = HANDLE_CURRENT_THREAD; r[1] = 1;
    CHECK(svc(0x32, r) == R_BUSY, "a thread cannot pause itself");
    memset(r, 0, sizeof r); r[0] = th; r[1] = 1;
    CHECK(svc(0x32, r) == 0, "SetThreadActivity(paused)");
    memset(r, 0, sizeof r); r[0] = th; r[1] = 1;
    CHECK(svc(0x32, r) == R_INVALID_STATE, "pausing twice");
    memset(r, 0, sizeof r); r[0] = (uintptr_t)ctx; r[1] = th;
    CHECK(svc(0x33, r) == 0, "GetThreadContext3 of the paused thread");
    uint64_t x5, fp, lr, sp; memcpy(&x5, ctx + 5 * 8, 8); memcpy(&fp, ctx + 29 * 8, 8); memcpy(&lr, ctx + 30 * 8, 8); memcpy(&sp, ctx + 31 * 8, 8);
    CHECK(x5 == 0x1005 && fp == 0xf00d && lr == 0xcafe && sp == 0x70000 + 176, "context: registers, fp, lr, guest sp");
    memset(r, 0, sizeof r); r[0] = (uintptr_t)ctx; r[1] = HANDLE_CURRENT_THREAD;
    CHECK(svc(0x33, r) == R_BUSY, "GetThreadContext3 of the calling thread");

    /* signaled while paused: the wait ends but the thread stays stopped at the svc exit until resumed */
    memset(r, 0, sizeof r); r[0] = ev_w; svc(0x11, r);
    usleep(30000);
    memset(r, 0, sizeof r); r[1] = (uintptr_t)&th; r[2] = 1; r[3] = 0;
    CHECK(svc(0x18, r) == R_TIMED_OUT, "paused thread has not exited");
    memset(r, 0, sizeof r); r[0] = th; r[1] = 0;
    CHECK(svc(0x32, r) == 0, "SetThreadActivity(runnable)");
    CHECK(join(h) == 0 && w.result == 0, "resumed thread finishes its wait and exits");
    memset(r, 0, sizeof r); r[0] = th; r[1] = 0;
    CHECK(svc(0x32, r) == R_INVALID_STATE, "resume of an exited thread");
}
static void last_thread_main(void *p) {
    uint64_t *out = p, r[KREG_COUNT] = {0};
    out[0] = svc(0x2F, r); out[1] = r[1]; out[2] = r[3]; out[3] = r[6]; out[4] = r[5];
}
static void test_last_thread_info(void) {
    /* main blocks in an svc (giving its core up); the helper then runs on that core and asks who ran there before it */
    uint64_t out[5] = {0}, r[KREG_COUNT] = {0};
    Helper *h = spawn(last_thread_main, out);
    r[1] = (uintptr_t)&h->handle; r[2] = 1; r[3] = 5000000000ll; r[29] = 0xabc; r[30] = 0xdef;
    uint32_t hd = atomic_load(&h->handle); r[1] = (uintptr_t)&hd;
    CHECK(svc(0x18, r) == 0, "helper finished");
    CHECK(out[0] == 0 || out[0] == R_NO_THREAD, "GetLastThreadInfo answers");
    if (out[0] == 0) CHECK(out[4] != 0, "previous thread's TLR reported");
}
static void test_preemption_state(void) {
    uint16_t *flag = (uint16_t *)(main_tlr + 0x102);
    *flag = 1;
    uint64_t r[KREG_COUNT] = {0};
    CHECK(svc(0x36, r) == 0 && *flag == 0, "SynchronizePreemptionState clears the TLR interrupt flag");
    memset(r, 0, sizeof r);
    CHECK(svc(0x2E, r) == R_INVALID_HANDLE, "GetDebugFutureThreadInfo: no debug objects");
    memset(r, 0, sizeof r); r[0] = 7;
    svc(0x3C, r); CHECK(r[0] == 7, "KernelDebug leaves the registers alone");
}

/* ---- shared memory (ipc.c) ------------------------------------------------------------------------------------------------- */
static void test_shared_memory(void) {
    uint64_t r[KREG_COUNT] = {0};
    r[1] = 0x1001; r[2] = 3; r[3] = 1;
    CHECK(svc(0x50, r) == R_INVALID_SIZE, "CreateSharedMemory: unaligned size");
    memset(r, 0, sizeof r); r[1] = 0x4000; r[2] = 5; r[3] = 1;
    CHECK(svc(0x50, r) == R_INVALID_MEMORY_PERMISSIONS, "CreateSharedMemory: local permission must be R or RW");
    memset(r, 0, sizeof r); r[1] = 0x4000; r[2] = 3; r[3] = 0x10000000;
    CHECK(svc(0x50, r) == 0 && r[1], "CreateSharedMemory");
    uint32_t h = (uint32_t)r[1];
    uint8_t *at = mmap((void *)0x30000000, 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    memset(r, 0, sizeof r); r[0] = h; r[1] = (uintptr_t)at; r[2] = 0x4000; r[3] = 3;
    CHECK(svc(0x13, r) == 0 && at[0] == 0 && at[0x3fff] == 0, "MapSharedMemory of the new block (zeroed)");
    memset(r, 0, sizeof r); r[0] = h; r[1] = (uintptr_t)at; r[2] = 0x4000;
    CHECK(svc(0x14, r) == 0, "UnmapSharedMemory");
    memset(r, 0, sizeof r); r[0] = h;
    CHECK(svc(0x16, r) == 0, "close the shared memory");
}

int main(void) {
    svc_kernel_register(); svc_threads_register(); svc_ipc_register(); svc_kcode_register();   /* ipc.c's SendSyncRequest routes guest sessions */
    host_thread_attach(main_tlr);
    test_kmsg();
    test_sessions();
    test_light();
    test_ports();
    test_code_memory();
    test_pause_and_context();
    test_last_thread_info();
    test_preemption_state();
    test_shared_memory();
    printf("{\"test\":\"kernel_ipc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
