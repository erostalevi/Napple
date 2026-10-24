/* Guest-to-guest IPC: kernel sessions and ports between threads of the game itself (an SDK library serving another one in-process).
 * Host services stay in ipc.c; these objects never reach them.
 *   0x40 CreateSession  0x41 AcceptSession  0x42 ReplyAndReceiveLight  0x43 ReplyAndReceive  0x44 ReplyAndReceiveWithUserBuffer
 *   0x20 SendSyncRequestLight  0x70 CreatePort  0x71 ManageNamedPort  0x72 ConnectToPort; SendSyncRequest(WithUserBuffer) and
 *   ConnectToNamedPort reach this file from ipc.c for handles / names that are not host services.
 * Semantics follow the public Horizon kernel (KServerSession, KLightServerSession, KServerPort): a client blocks until its request is
 * replied to; a server session is signaled while a request waits and none is being served, or once the client end is closed. Messages
 * are copied by kmsg.c (handles pass through, X data lands in the receiver's receive list). All state is under the kernel lock G. */
#include "kthread.h"
#include <stdlib.h>
#include <string.h>

#define MSG_BYTES 0x100
#define LIGHT_REPLY_FLAG 0x80000000u     /* data[0] bit 31 of ReplyAndReceiveLight: reply to the request being served first */
#define LIGHT_WORDS 7
#define GUEST_PID 0x80                  /* what GetProcessId answers (kernel.c) */

typedef struct KPort KPort;
typedef struct KSession {
    int light, client_open, server_open;
    Thread *head, *tail;                 /* clients whose request waits to be received, FIFO */
    Thread *active;                      /* the request received and not yet replied to */
    KPort *port;                         /* port it was connected through (session count), or NULL */
    struct KSession *pending_next;       /* in the port's accept queue */
} KSession;
struct KPort {
    int light, client_open, server_open, max_sessions, sessions;
    KSession *pending;                   /* connected, not yet accepted (FIFO) */
    char name[12]; int named;
};
#define MAX_NAMED 16
static KPort *g_named[MAX_NAMED];

static void *guest_msg_ptr(uint64_t addr, uint64_t size) { return guest_ptr(addr, size); }
static uint32_t *tlr_message(void) { return pthread_getspecific(runtime_keys[2]); }

/* ---- object life time (G held) ------------------------------------------------------------------------------------ */
static void port_maybe_free(KPort *p) {
    if (p->client_open || p->server_open || p->sessions || p->named) return;
    free(p);
}
static void session_maybe_free(KSession *s) {
    if (s->client_open || s->server_open || s->head || s->active) return;
    if (s->port) { s->port->sessions--; port_maybe_free(s->port); }
    free(s);
}
static void unlink_request(KSession *s, Thread *t) {
    Thread *prev = NULL;
    for (Thread *c = s->head; c; prev = c, c = c->ipc_next) if (c == t) {
        if (prev) prev->ipc_next = c->ipc_next; else s->head = c->ipc_next;
        if (s->tail == c) s->tail = prev;
        c->ipc_next = NULL; return;
    }
}
/* The server end is gone: every client waiting on it gets SessionClosed. */
static void fail_clients(KSession *s) {
    while (s->head) { Thread *c = s->head; unlink_request(s, c); c->ipc_session = NULL; kthread_wake(c, R_SESSION_CLOSED); }
    if (s->active) { Thread *c = s->active; s->active = NULL; c->ipc_session = NULL; kthread_wake(c, R_SESSION_CLOSED); }
}
int ksession_close_object(ObjType type, void *obj) {
    switch (type) {
    case OBJ_KSERVER_SESSION: { KSession *s = obj; pthread_mutex_lock(&G); s->server_open = 0; fail_clients(s); session_maybe_free(s); pthread_mutex_unlock(&G); return 1; }
    case OBJ_KCLIENT_SESSION: { KSession *s = obj; pthread_mutex_lock(&G); s->client_open = 0; kthread_wake_sync_waiters(); session_maybe_free(s); pthread_mutex_unlock(&G); return 1; }
    case OBJ_KSERVER_PORT: {
        KPort *p = obj; pthread_mutex_lock(&G); p->server_open = 0;
        while (p->pending) { KSession *s = p->pending; p->pending = s->pending_next; s->server_open = 0; fail_clients(s); session_maybe_free(s); }
        port_maybe_free(p); pthread_mutex_unlock(&G); return 1;
    }
    case OBJ_KCLIENT_PORT: { KPort *p = obj; pthread_mutex_lock(&G); p->client_open = 0; port_maybe_free(p); pthread_mutex_unlock(&G); return 1; }
    default: return 0;
    }
}
int ksession_sync_state(uint32_t h) {
    switch (handle_type(h)) {
    case OBJ_KSERVER_SESSION: { KSession *s = handle_get(h, OBJ_KSERVER_SESSION); return s && ((s->head && !s->active) || !s->client_open); }
    case OBJ_KSERVER_PORT: { KPort *p = handle_get(h, OBJ_KSERVER_PORT); return p && p->pending != NULL; }
    default: return -1;
    }
}

/* ---- creation ------------------------------------------------------------------------------------------------------- */
static KSession *session_new(int light, KPort *port) {
    KSession *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->light = light; s->client_open = s->server_open = 1; s->port = port;
    return s;
}
/* CreateSession(w2 is_light, x3 name) -> w1 server session, w2 client session */
static void svc_create_session(uint64_t *r) {
    pthread_mutex_lock(&G);
    KSession *s = session_new(r[2] != 0, NULL);
    uint32_t sh = s ? handle_new(OBJ_KSERVER_SESSION, s) : 0, ch = sh ? handle_new(OBJ_KCLIENT_SESSION, s) : 0;
    if (!ch) { if (sh) handle_close(sh, NULL); free(s); pthread_mutex_unlock(&G); r[0] = R_OUT_OF_RESOURCE; return; }
    pthread_mutex_unlock(&G);
    r[0] = 0; r[1] = sh; r[2] = ch;
}
/* CreatePort(w2 max sessions, w3 is_light, x4 name) -> w1 server port, w2 client port */
static void svc_create_port(uint64_t *r) {
    int max = (int)r[2];
    if (max <= 0) { r[0] = R_OUT_OF_RANGE; return; }
    pthread_mutex_lock(&G);
    KPort *p = calloc(1, sizeof *p);
    uint32_t sh = 0, ch = 0;
    if (p) { p->light = r[3] != 0; p->max_sessions = max; p->client_open = p->server_open = 1; sh = handle_new(OBJ_KSERVER_PORT, p); ch = sh ? handle_new(OBJ_KCLIENT_PORT, p) : 0; }
    if (!ch) { if (sh) handle_close(sh, NULL); free(p); pthread_mutex_unlock(&G); r[0] = R_OUT_OF_RESOURCE; return; }
    pthread_mutex_unlock(&G);
    r[0] = 0; r[1] = sh; r[2] = ch;
}
/* ManageNamedPort(x1 name, w2 max sessions) -> w1 server port; max 0 removes the name. */
static void svc_manage_named_port(uint64_t *r) {
    char name[12] = {0}; const char *p = guest_ptr(r[1], 1);
    if (!p) { r[0] = R_INVALID_POINTER; return; }
    for (unsigned i = 0; i < 12; i++) { const char *q = guest_ptr(r[1] + i, 1); if (!q) { r[0] = R_INVALID_POINTER; return; } name[i] = *q; if (!*q) break; }
    if (name[11]) { r[0] = R_OUT_OF_RANGE; return; }   /* longer than 11 characters */
    int max = (int)r[2];
    if (max < 0) { r[0] = R_OUT_OF_RANGE; return; }
    pthread_mutex_lock(&G);
    int slot = -1;
    for (int i = 0; i < MAX_NAMED; i++) if (g_named[i] && !strcmp(g_named[i]->name, name)) slot = i;
    if (max == 0) {
        if (slot < 0) { pthread_mutex_unlock(&G); r[0] = R_NOT_FOUND; return; }
        KPort *old = g_named[slot]; g_named[slot] = NULL; old->named = 0; old->client_open = 0; port_maybe_free(old);
        pthread_mutex_unlock(&G); r[0] = 0; return;
    }
    if (slot >= 0 || !strcmp(name, "sm:")) { pthread_mutex_unlock(&G); r[0] = R_INVALID_STATE; return; }   /* already registered */
    for (int i = 0; i < MAX_NAMED && slot < 0; i++) if (!g_named[i]) slot = i;
    KPort *np = slot >= 0 ? calloc(1, sizeof *np) : NULL;
    uint32_t sh = np ? handle_new(OBJ_KSERVER_PORT, np) : 0;
    if (!sh) { free(np); pthread_mutex_unlock(&G); r[0] = R_OUT_OF_RESOURCE; return; }
    np->max_sessions = max; np->server_open = np->client_open = 1; np->named = 1; memcpy(np->name, name, 12); g_named[slot] = np;
    pthread_mutex_unlock(&G);
    r[0] = 0; r[1] = sh;
}
/* G held: a new session on p, queued for AcceptSession; its client handle in *out. */
static uint32_t port_connect(KPort *p, uint32_t *out) {
    if (!p->server_open) return R_PORT_CLOSED;
    if (p->sessions >= p->max_sessions) return R_OUT_OF_SESSIONS;
    KSession *s = session_new(p->light, p);
    uint32_t ch = s ? handle_new(OBJ_KCLIENT_SESSION, s) : 0;
    if (!ch) { free(s); return R_OUT_OF_RESOURCE; }
    p->sessions++;
    KSession **q = &p->pending; while (*q) q = &(*q)->pending_next; *q = s;
    kthread_wake_sync_waiters();
    *out = ch;
    return 0;
}
static void svc_connect_to_port(uint64_t *r) {
    pthread_mutex_lock(&G);
    KPort *p = handle_get((uint32_t)r[1], OBJ_KCLIENT_PORT); uint32_t h = 0;
    uint32_t res = p ? port_connect(p, &h) : R_INVALID_HANDLE;
    pthread_mutex_unlock(&G);
    r[0] = res; r[1] = h;
}
/* ConnectToNamedPort for a name the game registered itself (ipc.c handles "sm:"). Returns 0 when no such port exists. */
int ksession_connect_named(const char *name, uint64_t *r) {
    pthread_mutex_lock(&G);
    KPort *p = NULL;
    for (int i = 0; i < MAX_NAMED; i++) if (g_named[i] && !strncmp(g_named[i]->name, name, 12)) p = g_named[i];
    if (!p) { pthread_mutex_unlock(&G); return 0; }
    uint32_t h = 0; r[0] = port_connect(p, &h); r[1] = h;
    pthread_mutex_unlock(&G);
    return 1;
}
/* AcceptSession(w1 server port) -> w1 server session */
static void svc_accept_session(uint64_t *r) {
    pthread_mutex_lock(&G);
    KPort *p = handle_get((uint32_t)r[1], OBJ_KSERVER_PORT); uint32_t res = 0, h = 0;
    if (!p) res = R_INVALID_HANDLE;
    else if (!p->pending) res = R_NOT_FOUND;
    else {
        KSession *s = p->pending;
        h = handle_new(OBJ_KSERVER_SESSION, s);
        if (!h) res = R_OUT_OF_RESOURCE; else { p->pending = s->pending_next; s->pending_next = NULL; }
    }
    pthread_mutex_unlock(&G);
    r[0] = res; r[1] = h;
}

/* ---- requests (client side) ----------------------------------------------------------------------------------------- */
int ksession_is_client(uint32_t h) { return handle_type(h) == OBJ_KCLIENT_SESSION; }
static void enqueue(KSession *s, Thread *t) {
    t->ipc_session = s; t->ipc_next = NULL;
    if (s->tail) s->tail->ipc_next = t; else s->head = t;
    s->tail = t;
    kthread_wake_sync_waiters();
}
/* SendSyncRequest(WithUserBuffer) on a guest session: msg is the client's message buffer of `size` bytes. */
uint32_t ksession_send_request(uint32_t h, uint32_t *msg, uint32_t size) {
    Thread *t = kthread_self();
    pthread_mutex_lock(&G);
    KSession *s = handle_get(h, OBJ_KCLIENT_SESSION); uint32_t res;
    if (!s || s->light) res = R_INVALID_HANDLE;
    else if (!s->server_open) res = R_SESSION_CLOSED;
    else {
        KMsgLayout l;
        res = kmsg_layout(msg, size, &l);
        if (!res) {
            t->ipc_msg = msg; t->ipc_msg_size = size; t->ipc_light = NULL;
            kmsg_recv_list(msg, size, &t->ipc_recv);
            enqueue(s, t);
            res = (uint32_t)kthread_block(t, KW_IPC_REPLY, -1);
            t->ipc_session = NULL;
        }
    }
    pthread_mutex_unlock(&G);
    return res;
}
/* SendSyncRequestLight(w0 client session; w1..w7 data in and out) */
static void svc_send_sync_request_light(uint64_t *r) {
    Thread *t = kthread_self();
    pthread_mutex_lock(&G);
    KSession *s = handle_get((uint32_t)r[0], OBJ_KCLIENT_SESSION); uint32_t res;
    if (!s || !s->light) res = R_INVALID_HANDLE;
    else if (!s->server_open) res = R_SESSION_CLOSED;
    else { t->ipc_msg = NULL; t->ipc_light = r + 1; enqueue(s, t); res = (uint32_t)kthread_block(t, KW_IPC_REPLY, -1); t->ipc_session = NULL; t->ipc_light = NULL; }
    pthread_mutex_unlock(&G);
    r[0] = res;
}

/* ---- serving ---------------------------------------------------------------------------------------------------------- */
/* G held: copies the server's reply in `msg` to the client being served and wakes it. */
static uint32_t send_reply(KSession *s, const uint32_t *msg, uint32_t size) {
    Thread *c = s->active;
    if (!c) return R_INVALID_STATE;
    s->active = NULL;
    uint32_t res = kmsg_copy(c->ipc_msg, c->ipc_msg_size, &c->ipc_recv, msg, size, GUEST_PID, guest_msg_ptr);
    c->ipc_session = NULL;
    kthread_wake(c, res);              /* a reply that cannot be delivered fails the client's request with the same result */
    return res;
}
/* G held: takes the next request of s into the server's buffer. R_NOT_FOUND when none waits. */
static uint32_t receive_request(KSession *s, uint32_t *msg, uint32_t size) {
    if (!s->head || s->active) return s->client_open ? R_NOT_FOUND : R_SESSION_CLOSED;
    Thread *c = s->head;
    KRecvList recv; kmsg_recv_list(msg, size, &recv);
    uint32_t res = kmsg_copy(msg, size, &recv, c->ipc_msg, c->ipc_msg_size, GUEST_PID, guest_msg_ptr);
    unlink_request(s, c);
    if (res) { c->ipc_session = NULL; kthread_wake(c, res); return res; }   /* the request cannot be delivered: its client gets the error */
    s->active = c;
    return 0;
}
/* ReplyAndReceive(x1 handles, w2 count, w3 reply target, x4 timeout) -> w1 index; WithUserBuffer: x1 buffer, x2 size, x3 handles, w4 count,
 * w5 reply target, x6 timeout. */
static void reply_and_receive(uint64_t *r, int user_buffer) {
    uint64_t table = user_buffer ? r[3] : r[1]; int n = (int)(user_buffer ? r[4] : r[2]);
    uint32_t reply = (uint32_t)(user_buffer ? r[5] : r[3]); int64_t ns = (int64_t)(user_buffer ? r[6] : r[4]);
    uint32_t size = user_buffer ? (uint32_t)r[2] : MSG_BYTES;
    uint32_t *msg = user_buffer ? guest_ptr(r[1], size) : tlr_message();
    if (!msg || size < 8 || (user_buffer && (r[1] & 0xfff))) { r[0] = user_buffer ? R_INVALID_ADDRESS : R_INVALID_POINTER; r[1] = (uint64_t)-1; return; }
    if (n < 0 || n > 64 || (n && !table)) { r[0] = R_OUT_OF_RANGE; r[1] = (uint64_t)-1; return; }
    uint32_t hs[64];
    if (n) { const void *p = guest_ptr(table, (uint64_t)n * 4); if (!p) { r[0] = R_INVALID_POINTER; r[1] = (uint64_t)-1; return; } memcpy(hs, p, (size_t)n * 4); }
    Thread *t = kthread_self();
    pthread_mutex_lock(&G);
    if (reply) {
        KSession *s = handle_get(reply, OBJ_KSERVER_SESSION);
        uint32_t res = !s || s->light ? R_INVALID_HANDLE : send_reply(s, msg, size);
        if (res) { pthread_mutex_unlock(&G); r[0] = res; r[1] = (uint64_t)-1; return; }
    }
    for (;;) {
        int idx;
        int64_t res = kthread_wait_handles(t, hs, n, ns, &idx);
        if (res) { pthread_mutex_unlock(&G); r[0] = (uint64_t)res; r[1] = (uint64_t)-1; return; }
        KSession *s = handle_get(hs[idx], OBJ_KSERVER_SESSION);
        if (s && !s->light) {
            uint32_t rr = receive_request(s, msg, size);
            if (rr == R_NOT_FOUND) continue;          /* raced with another server thread on the same session */
            pthread_mutex_unlock(&G); r[0] = rr; r[1] = (uint64_t)idx; return;
        }
        pthread_mutex_unlock(&G); r[0] = 0; r[1] = (uint64_t)idx; return;
    }
}
static void svc_reply_and_receive(uint64_t *r) { reply_and_receive(r, 0); }
static void svc_reply_and_receive_user(uint64_t *r) { reply_and_receive(r, 1); }

/* ReplyAndReceiveLight(w0 server session; w1..w7 data): replies with the data when bit 31 of w1 is set, then waits for the next request
 * and returns its data in w1..w7. */
static void svc_reply_and_receive_light(uint64_t *r) {
    Thread *t = kthread_self(); uint32_t h = (uint32_t)r[0];
    pthread_mutex_lock(&G);
    KSession *s = handle_get(h, OBJ_KSERVER_SESSION);
    if (!s || !s->light) { pthread_mutex_unlock(&G); r[0] = R_INVALID_HANDLE; return; }
    if ((uint32_t)r[1] & LIGHT_REPLY_FLAG) {
        if (!s->server_open) { pthread_mutex_unlock(&G); r[0] = R_SESSION_CLOSED; return; }
        Thread *c = s->active;
        if (!c) { pthread_mutex_unlock(&G); r[0] = R_INVALID_STATE; return; }
        s->active = NULL;
        for (int i = 0; i < LIGHT_WORDS; i++) c->ipc_light[i] = (uint32_t)r[1 + i];
        c->ipc_session = NULL; kthread_wake(c, 0);
    }
    for (;;) {
        if (!s->head || s->active) {
            if (!s->client_open) { pthread_mutex_unlock(&G); r[0] = R_SESSION_CLOSED; return; }
            int idx; int64_t res = kthread_wait_handles(t, &h, 1, -1, &idx);
            if (res) { pthread_mutex_unlock(&G); r[0] = (uint64_t)res; return; }
            s = handle_get(h, OBJ_KSERVER_SESSION);
            if (!s) { pthread_mutex_unlock(&G); r[0] = R_SESSION_CLOSED; return; }
            continue;
        }
        Thread *c = s->head; unlink_request(s, c); s->active = c;
        for (int i = 0; i < LIGHT_WORDS; i++) r[1 + i] = (uint32_t)c->ipc_light[i];
        pthread_mutex_unlock(&G); r[0] = 0; return;
    }
}

void svc_ksession_register(void) {
    svc_register(0x20, "SendSyncRequestLight", svc_send_sync_request_light);
    svc_register(0x40, "CreateSession", svc_create_session);
    svc_register(0x41, "AcceptSession", svc_accept_session);
    svc_register(0x42, "ReplyAndReceiveLight", svc_reply_and_receive_light);
    svc_register(0x43, "ReplyAndReceive", svc_reply_and_receive);
    svc_register(0x44, "ReplyAndReceiveWithUserBuffer", svc_reply_and_receive_user);
    svc_register(0x70, "CreatePort", svc_create_port);
    svc_register(0x71, "ManageNamedPort", svc_manage_named_port);
    svc_register(0x72, "ConnectToPort", svc_connect_to_port);
}
