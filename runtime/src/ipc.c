/* In-process Horizon IPC server: svc 0x1f/0x21/0x22/0x40.. entry points, HIPC/CMIF framing (plain sessions and domains),
 * the "sm:" service manager and the registry of host services. See ipc.h. */
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include "mach_vm_compat.h"
#endif
#include <sys/mman.h>

#define CMIF_IN_MAGIC 0x49434653u  /* "SFCI" */
#define CMIF_OUT_MAGIC 0x4f434653u /* "SFCO" */
#define MSG_BYTES 0x100
#define POINTER_BUFFER_SIZE 0x8000
#define DOMAIN_MAX 512

int ksession_connect_named(const char *name, uint64_t *r); /* ksession.c */
int ksession_is_client(uint32_t h);
uint32_t ksession_send_request(uint32_t h, uint32_t *msg, uint32_t size);
void svc_ksession_register(void);

enum { T_CLOSE = 2, T_REQUEST = 4, T_CONTROL = 5, T_REQUEST_CTX = 6, T_CONTROL_CTX = 7, T_TIPC_CLOSE = 15, T_TIPC_BASE = 16 };

/* A domain is shared by every session cloned from the converted one. */
typedef struct IpcDomain {
    IpcObject *objects[DOMAIN_MAX + 1]; /* object id -> object (id 1 is the converted root) */
    unsigned next_id, refs;
    pthread_mutex_t lock;
} IpcDomain;
typedef struct IpcSession {
    IpcObject *object; /* the session's own object (non-domain) */
    int is_domain;
    IpcDomain *dom;
} IpcSession;

typedef struct {
    char name[12];
    IpcOpen open;
} Service;
static Service services[128]; /* 64 overflowed once the 2026-10-24 services landed */
static unsigned nservices;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread uint64_t trace_fp, trace_lr;                                       /* guest frame of the current request, for failure reports */
void ipc_trace_guest(unsigned n) { runtime_trace_guest_n(trace_fp, trace_lr, n); } /* guest call stack of the request being served (debug aid) */

/* ---- objects and sessions ---------------------------------------------------------------------------------------- */
IpcObject *ipc_object_new(const char *iface, const IpcCmd *cmds, size_t ncmds, void *state, void (*destroy)(IpcObject *)) {
    IpcObject *o = calloc(1, sizeof *o);
    if (!o) runtime_fail("IPC object allocation");
    *o = (IpcObject){iface, cmds, ncmds, state, destroy, 1};
    return o;
}
static IpcObject *object_ref(IpcObject *o) {
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
    return o;
}
static void object_unref(IpcObject *o) {
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        if (o->destroy) o->destroy(o);
        free(o);
    }
}
static IpcSession *session_new(IpcObject *o) {
    IpcSession *s = calloc(1, sizeof *s);
    if (!s) runtime_fail("IPC session allocation");
    s->object = o;
    return s;
}
static uint32_t session_handle(IpcObject *o) {
    uint32_t h = handle_new(OBJ_SESSION, session_new(o));
    if (!h) runtime_fail("handle table exhausted creating a session");
    return h;
}
static void session_free(IpcSession *s) {
    if (s->object) object_unref(s->object);
    if (s->dom && __atomic_sub_fetch(&s->dom->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        for (unsigned i = 1; i <= DOMAIN_MAX; i++)
            if (s->dom->objects[i]) object_unref(s->dom->objects[i]);
        free(s->dom);
    }
    free(s);
}
int ipc_close_object(ObjType type, void *obj) {
    if (type == OBJ_SESSION) {
        session_free(obj);
        return 1;
    }
    if (type == OBJ_TMEM) {
        free(obj);
        return 1;
    }
    if (type == OBJ_SHMEM) {
        IpcSharedMemory *m = obj;
        if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL) == 0) {
            if (m->owned) munmap(m->host, m->size);
            free(m);
        }
        return 1;
    }
    return 0;
}

/* ---- registry ---------------------------------------------------------------------------------------------------- */
void ipc_register_service(const char *name, IpcOpen open) {
    pthread_mutex_lock(&registry_lock);
    if (nservices == sizeof services / sizeof *services) runtime_fail("IPC service registry full");
    snprintf(services[nservices].name, sizeof services[nservices].name, "%s", name);
    services[nservices++].open = open;
    pthread_mutex_unlock(&registry_lock);
}
void ipc_service_names(char names[][12], unsigned max, unsigned *count) {
    *count = 0;
    for (unsigned i = 0; i < nservices && i < max; i++) {
        memcpy(names[i], services[i].name, 12);
        (*count)++;
    }
}

/* ---- request helpers --------------------------------------------------------------------------------------------- */
const void *ipc_in(const IpcCall *c, uint32_t off, uint32_t n) {
    if (off + n > c->in_size) {
        LOG("ipc", "%s cmd %u: argument %u+%u beyond the %u request bytes", c->object->iface, c->cmd, off, n, c->in_size);
        runtime_fail("IPC argument out of range");
    }
    return c->in + off;
}
/* AutoSelect arguments are sent with both descriptors (pointer and map alias), the unused one empty: count only
 * non-empty buffers, falling back to an empty one so zero-length transfers still find their slot. */
static const IpcBuf *pick_buf(const IpcBuf *l1, unsigned n1, const IpcBuf *l2, unsigned n2, unsigned n) {
    unsigned k = 0;
    for (unsigned i = 0; i < n1 + n2; i++) {
        const IpcBuf *b = i < n1 ? &l1[i] : &l2[i - n1];
        if (b->size && k++ == n) return b;
    }
    if (n < n1) return &l1[n];
    return n - n1 < n2 ? &l2[n - n1] : NULL;
}
const IpcBuf *ipc_in_buf(const IpcCall *c, unsigned n) { return pick_buf(c->a, c->na, c->x, c->nx, n); }
const IpcBuf *ipc_out_buf(const IpcCall *c, unsigned n) { return pick_buf(c->b, c->nb, c->c, c->nc, n); }
void *ipc_buf_ptr(const IpcBuf *b) { return b ? guest_ptr(b->addr, b->size) : NULL; }
uint64_t ipc_write_buf(IpcCall *c, const IpcBuf *b, const void *data, uint64_t n) {
    if (!b) return 0;
    uint64_t w = n < b->size ? n : b->size;
    void *p = guest_ptr(b->addr, w);
    if (!p && w) runtime_fail("IPC output buffer %#llx+%#llx is not guest memory", (unsigned long long)b->addr, (unsigned long long)b->size);
    if (w) {
        guest_write_prepare(b->addr, (size_t)w);
        memcpy(p, data, w);
    }
    if (b >= c->c && b < c->c + IPC_MAX_BUF && c->nxout < IPC_MAX_BUF) /* receive-list buffer: tell the client how much arrived */
        c->xout[c->nxout++] = (IpcBuf){b->addr, w, b->index};
    return w;
}
void ipc_out(IpcCall *c, const void *p, uint32_t n) {
    uint32_t a = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
    c->out_size = (c->out_size + a - 1) & ~(a - 1);
    if (c->out_size + n > sizeof c->out) runtime_fail("IPC response data too large");
    memcpy(c->out + c->out_size, p, n);
    c->out_size += n;
}
void ipc_out_zero(IpcCall *c, uint32_t n) {
    uint8_t z[0x100] = {0};
    if (n > sizeof z) runtime_fail("IPC zero fill too large");
    ipc_out(c, z, n);
}
void ipc_out_copy_handle(IpcCall *c, uint32_t h) {
    if (c->nout_copy == IPC_MAX_HANDLES) runtime_fail("too many copy handles");
    c->out_copy[c->nout_copy++] = h;
}
void ipc_out_move_handle(IpcCall *c, uint32_t h) {
    if (c->nout_move == IPC_MAX_HANDLES) runtime_fail("too many move handles");
    c->out_move[c->nout_move++] = h;
}
void ipc_out_object(IpcCall *c, IpcObject *o) {
    if (c->nout_objects == IPC_MAX_HANDLES) runtime_fail("too many out objects");
    c->out_objects[c->nout_objects++] = o;
}
void ipc_fail(IpcCall *c, uint32_t result) { c->result = result; }

IpcSharedMemory *ipc_shared_memory_new(uint64_t size) {
    IpcSharedMemory *m = calloc(1, sizeof *m);
    if (!m) runtime_fail("shared memory allocation");
    size = (size + PAGE_16K - 1) & ~(PAGE_16K - 1);
    m->host = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m->host == MAP_FAILED) runtime_fail("shared memory mapping of %llu bytes", (unsigned long long)size);
    m->size = size;
    m->refs = 1;
    m->owned = 1;
    return m;
}
uint32_t ipc_shared_memory_handle(IpcSharedMemory *m) {
    __atomic_add_fetch(&m->refs, 1, __ATOMIC_RELAXED);
    return handle_new(OBJ_SHMEM, m);
}

/* MapSharedMemory(x0 handle, x1 address, x2 size, w3 permission). The host pages are aliased into the guest range when it is
 * host-page aligned (writes either way are shared); otherwise the contents are copied in and ipc_shared_memory_flush keeps
 * the copy current. */
static void svc_map_shared_memory(uint64_t *r) {
    IpcSharedMemory *m = handle_get((uint32_t)r[0], OBJ_SHMEM);
    uint64_t addr = r[1], size = r[2];
    if (!m) {
        r[0] = R_INVALID_HANDLE;
        return;
    }
    if ((addr & 4095) || !size || (size & 4095) || size > m->size) {
        r[0] = R_INVALID_SIZE;
        return;
    }
    if (m->nmaps == 4) {
        r[0] = R_OUT_OF_RESOURCE;
        return;
    }
    IpcShmemMap *map = &m->maps[m->nmaps++];
    map->addr = addr;
    map->size = size;
#ifdef __APPLE__ /* other hosts (runtime/tests/host) always take the copy path */
    if (!(addr & (PAGE_16K - 1)) && !(size & (PAGE_16K - 1))) {
        mach_vm_address_t a = addr;
        vm_prot_t cur, max;
        kern_return_t kr = mach_vm_remap(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                         (mach_vm_address_t)(uintptr_t)m->host, FALSE, &cur, &max, VM_INHERIT_NONE);
        if (kr == KERN_SUCCESS && a == addr) {
            map->remapped = 1;
            mprotect((void *)addr, size, PROT_READ | ((r[3] & 2) ? PROT_WRITE : 0));
            r[0] = 0;
            return;
        }
        LOG("ipc", "shared memory remap %#llx+%#llx failed (%d): falling back to a copy", (unsigned long long)addr, (unsigned long long)size, kr);
    }
#endif
    uint64_t lo = addr & ~(PAGE_16K - 1), hi = (addr + size + PAGE_16K - 1) & ~(PAGE_16K - 1);
    if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) {
        m->nmaps--;
        r[0] = R_INVALID_CURRENT_MEMORY;
        return;
    }
    memcpy((void *)addr, m->host, size);
    LOGV("ipc", "shared memory %#llx+%#llx mapped as a copy", (unsigned long long)addr, (unsigned long long)size);
    r[0] = 0;
}
static void svc_unmap_shared_memory(uint64_t *r) {
    IpcSharedMemory *m = handle_get((uint32_t)r[0], OBJ_SHMEM);
    if (!m) {
        r[0] = R_INVALID_HANDLE;
        return;
    }
    for (unsigned i = 0; i < m->nmaps; i++)
        if (m->maps[i].addr == r[1]) {
            if (m->maps[i].remapped) mmap((void *)m->maps[i].addr, m->maps[i].size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
            m->maps[i] = m->maps[--m->nmaps];
            r[0] = 0;
            return;
        }
    r[0] = R_INVALID_CURRENT_MEMORY;
}
void ipc_shared_memory_flush(IpcSharedMemory *m) {
    for (unsigned i = 0; i < m->nmaps; i++)
        if (!m->maps[i].remapped) memcpy((void *)m->maps[i].addr, m->host, m->maps[i].size);
}
static void svc_map_transfer_memory(uint64_t *r) {
    r[0] = handle_get((uint32_t)r[0], OBJ_TMEM) ? 0 : R_INVALID_HANDLE;
} /* services read guest memory directly */

/* ---- HIPC framing ------------------------------------------------------------------------------------------------ */
static uint64_t static_addr(const uint32_t *d) { return d[1] | ((uint64_t)((d[0] >> 12) & 0xf) << 32) | ((uint64_t)((d[0] >> 6) & 0x3f) << 36); }
uint64_t guest_addr_to_host(uint64_t a);
uint64_t host_addr_to_guest(uint64_t a);
static uint64_t buffer_addr(const uint32_t *d) { return d[1] | ((uint64_t)((d[2] >> 28) & 0xf) << 32) | ((uint64_t)((d[2] >> 2) & 0x3fffff) << 36); }
static uint64_t buffer_size(const uint32_t *d) { return d[0] | ((uint64_t)((d[2] >> 24) & 0xf) << 32); }

typedef struct {
    const uint32_t *data_words;
    unsigned ndata;
} ParseTail;

static void parse(const uint32_t *m, IpcCall *c, ParseTail *tail) {
    memset(c, 0, sizeof *c);
    c->type = m[0] & 0xffff;
    c->nx = (m[0] >> 16) & 0xf;
    c->na = (m[0] >> 20) & 0xf;
    c->nb = (m[0] >> 24) & 0xf;
    c->nw = (m[0] >> 28) & 0xf;
    tail->ndata = m[1] & 0x3ff;
    unsigned recv_mode = (m[1] >> 10) & 0xf, special = m[1] >> 31;
    if (c->nx > IPC_MAX_BUF || c->na > IPC_MAX_BUF || c->nb > IPC_MAX_BUF || c->nw > IPC_MAX_BUF) runtime_fail("IPC message with too many buffers");
    const uint32_t *p = m + 2;
    if (special) {
        uint32_t h = *p++;
        c->has_pid = h & 1;
        c->ncopy = (h >> 1) & 0xf;
        c->nmove = (h >> 5) & 0xf;
        if (c->ncopy > IPC_MAX_HANDLES || c->nmove > IPC_MAX_HANDLES) runtime_fail("IPC message with too many handles");
        if (c->has_pid) {
            memcpy(&c->pid, p, 8);
            p += 2;
        }
        for (unsigned i = 0; i < c->ncopy; i++) c->copy[i] = *p++;
        for (unsigned i = 0; i < c->nmove; i++) c->move[i] = *p++;
    }
    for (unsigned i = 0; i < c->nx; i++, p += 2) c->x[i] = (IpcBuf){guest_addr_to_host(static_addr(p)), p[0] >> 16, p[0] & 0x3f};
    IpcBuf *lists[3] = {c->a, c->b, c->w};
    unsigned counts[3] = {c->na, c->nb, c->nw};
    for (unsigned k = 0; k < 3; k++)
        for (unsigned i = 0; i < counts[k]; i++, p += 3) lists[k][i] = (IpcBuf){guest_addr_to_host(buffer_addr(p)), buffer_size(p), i};
    tail->data_words = p;
    const uint32_t *recv = p + tail->ndata;
    c->nc = recv_mode == 2 ? 1 : recv_mode > 2 ? recv_mode - 2 : 0;
    if (c->nc > IPC_MAX_BUF) runtime_fail("IPC message with too many receive buffers");
    for (unsigned i = 0; i < c->nc; i++, recv += 2) c->c[i] = (IpcBuf){guest_addr_to_host(recv[0] | ((uint64_t)(recv[1] & 0xffff) << 32)), recv[1] >> 16, i};
    if ((const uint8_t *)recv - (const uint8_t *)m > MSG_BYTES) runtime_fail("IPC request exceeds the message buffer");
}

static void respond(uint32_t *m, int domain, const IpcCall *c, const uint32_t *object_ids) {
    uint32_t out[MSG_BYTES / 4];
    memset(out, 0, sizeof out);
    unsigned special = c->nout_copy || c->nout_move;
    uint32_t *p = out + 2;
    if (special) {
        *p++ = (c->nout_copy << 1) | (c->nout_move << 5);
        for (unsigned i = 0; i < c->nout_copy; i++) *p++ = c->out_copy[i];
        for (unsigned i = 0; i < c->nout_move; i++) *p++ = c->out_move[i];
    }
    for (unsigned i = 0; i < c->nxout; i++, p += 2) {
        uint64_t a = host_addr_to_guest(c->xout[i].addr);
        p[0] = (c->xout[i].index & 0x3f) | (uint32_t)(((a >> 36) & 0x3f) << 6) | (uint32_t)(((a >> 32) & 0xf) << 12) | (uint32_t)(c->xout[i].size << 16);
        p[1] = (uint32_t)a;
    }
    uint8_t *words = (uint8_t *)p, *payload = (uint8_t *)out + (((words - (uint8_t *)out) + 15) & ~15);
    uint8_t *w = payload;
    if (domain) {
        uint32_t dh[4] = {c->nout_objects, 0, 0, 0};
        memcpy(w, dh, 16);
        w += 16;
    }
    uint32_t h[4] = {CMIF_OUT_MAGIC, 0, c->result, 0};
    memcpy(w, h, 16);
    w += 16;
    memcpy(w, c->out, c->out_size);
    w += c->out_size;
    if (domain) {
        w = (uint8_t *)(((uintptr_t)w + 3) & ~(uintptr_t)3);
        memcpy(w, object_ids, c->nout_objects * 4);
        w += c->nout_objects * 4;
    }
    unsigned nwords = (unsigned)((w - words) + 16 + 3) / 4; /* alignment slack at the end, as real servers leave */
    if ((size_t)(words - (uint8_t *)out) + nwords * 4 > MSG_BYTES) runtime_fail("IPC response exceeds the message buffer");
    out[0] = (c->nxout & 0xf) << 16;
    out[1] = (nwords & 0x3ff) | (special ? 1u << 31 : 0);
    memcpy(m, out, MSG_BYTES);
}

/* ---- dispatch ---------------------------------------------------------------------------------------------------- */
static void note_command(IpcCall *c, const IpcCmd *cmd, unsigned bytes) {
    summary_ipc(c->object->iface, c->cmd, cmd ? cmd->status : 2, bytes);
    static int trace_all = -1;
    if (trace_all < 0) {
        const char *e = getenv("NAPPLE_TRACE_IPC");
        trace_all = e && !strcmp(e, "all");
    }
    if (log_verbose || trace_all)
        LOG("ipc", "%s::%s (cmd %u) in=%u bytes x%u a%u b%u w%u c%u", c->object->iface, cmd ? cmd->name : "?", c->cmd, bytes, c->nx, c->na, c->nb, c->nw,
            c->nc);
    trace_event("ipc", "\"service\":\"%s\",\"cmd\":%u,\"name\":\"%s\",\"bytes\":%u,\"status\":\"%s\"", c->object->iface, c->cmd, cmd ? cmd->name : "?", bytes,
                cmd ? (cmd->status ? "stub" : "impl") : "missing");
}
static _Noreturn void unimplemented(IpcCall *c) {
    LOG("ipc", "%s: command %u not implemented (type %u, %u argument bytes, buffers x%u a%u b%u w%u c%u, %u copy + %u move handles%s)", c->object->iface,
        c->cmd, c->type, c->in_size, c->nx, c->na, c->nb, c->nw, c->nc, c->ncopy, c->nmove, c->has_pid ? ", pid" : "");
    runtime_trace_guest(trace_fp, trace_lr);
    summary_note("missing ipc command %s:%u", c->object->iface, c->cmd);
    runtime_fail("IPC command %s:%u not implemented", c->object->iface, c->cmd);
}
static void run_object(IpcObject *o, IpcCall *c) {
    c->object = o;
    const IpcCmd *cmd = NULL;
    for (size_t i = 0; i < o->ncmds; i++)
        if (o->cmds[i].cmd == c->cmd) {
            cmd = &o->cmds[i];
            break;
        }
    note_command(c, cmd, c->in_size);
    if (!cmd) {
        if (!getenv("NAPPLE_IPC_LENIENT")) unimplemented(c);
        LOG("ipc", "%s: command %u not implemented (lenient: error result)", o->iface, c->cmd);
        c->result = IPC_R_UNKNOWN_COMMAND;
        return;
    }
    cmd->fn(o, c);
}

/* Control requests on a session: domain conversion, cloning, pointer buffer size. */
static void control(IpcSession *s, IpcCall *c, uint32_t *domain_object_out) {
    (void)domain_object_out;
    switch (c->cmd) {
        case 0: { /* ConvertCurrentObjectToDomain -> u32 object id */
            if (!s->is_domain) {
                IpcDomain *d = calloc(1, sizeof *d);
                pthread_mutex_init(&d->lock, NULL);
                d->refs = 1;
                d->objects[1] = s->object;
                d->next_id = 2;
                s->dom = d;
                s->is_domain = 1;
                s->object = NULL;
            }
            uint32_t id = 1;
            IPC_OUT(c, id);
            return;
        }
        case 2:
        case 4: { /* CloneCurrentObject(Ex) -> move handle to the same object */
            if (s->is_domain) {
                IpcSession *n = session_new(NULL);
                n->is_domain = 1;
                n->dom = s->dom;
                __atomic_add_fetch(&s->dom->refs, 1, __ATOMIC_RELAXED);
                uint32_t h = handle_new(OBJ_SESSION, n);
                if (!h) runtime_fail("handle table exhausted cloning a session");
                ipc_out_move_handle(c, h);
            } else ipc_out_move_handle(c, session_handle(object_ref(s->object)));
            return;
        }
        case 3: {
            uint16_t n = POINTER_BUFFER_SIZE;
            IPC_OUT(c, n);
            return;
        }
        default: LOG("ipc", "control command %u not implemented", c->cmd); runtime_fail("IPC control command %u not implemented", c->cmd);
    }
}

/* TIPC (Horizon 12+): the command id is the message type minus 16, the arguments follow the descriptors with no CMIF header or
 * alignment, and the reply carries the result code as its first raw word, then the output data. No domains, no control requests. */
static void respond_tipc(uint32_t *m, const IpcCall *c) {
    uint32_t out[MSG_BYTES / 4];
    memset(out, 0, sizeof out);
    unsigned special = c->nout_copy || c->nout_move;
    uint32_t *p = out + 2;
    if (special) {
        *p++ = (c->nout_copy << 1) | (c->nout_move << 5);
        for (unsigned i = 0; i < c->nout_copy; i++) *p++ = c->out_copy[i];
        for (unsigned i = 0; i < c->nout_move; i++) *p++ = c->out_move[i];
    }
    for (unsigned i = 0; i < c->nxout; i++, p += 2) {
        uint64_t a = c->xout[i].addr;
        p[0] = (c->xout[i].index & 0x3f) | (uint32_t)(((a >> 36) & 0x3f) << 6) | (uint32_t)(((a >> 32) & 0xf) << 12) | (uint32_t)(c->xout[i].size << 16);
        p[1] = (uint32_t)a;
    }
    unsigned nwords = 1 + (c->out_size + 3) / 4;
    if ((size_t)((uint8_t *)p - (uint8_t *)out) + nwords * 4 > MSG_BYTES) runtime_fail("TIPC response exceeds the message buffer");
    *p++ = c->result;
    memcpy(p, c->out, c->out_size);
    out[0] = (c->nxout & 0xf) << 16;
    out[1] = (nwords & 0x3ff) | (special ? 1u << 31 : 0);
    memcpy(m, out, MSG_BYTES);
}
static void dispatch_tipc(IpcSession *s, uint32_t *m, IpcCall *c, const ParseTail *tail) {
    if (s->is_domain || !s->object) runtime_fail("TIPC request on a session without a plain object");
    c->cmd = c->type - T_TIPC_BASE;
    c->in = (const uint8_t *)tail->data_words;
    c->in_size = tail->ndata * 4;
    for (unsigned i = 0; i < c->ncopy; i++) { /* objects passed as arguments arrive as copy handles */
        IpcSession *as = handle_get(c->copy[i], OBJ_SESSION);
        if (as && as->object) c->in_objects[c->nin_objects++] = as->object;
    }
    run_object(s->object, c);
    for (unsigned i = 0; i < c->nout_objects; i++) ipc_out_move_handle(c, session_handle(c->out_objects[i]));
    c->nout_objects = 0;
    respond_tipc(m, c);
}

static void dispatch(IpcSession *s, uint32_t *m) {
    IpcCall c;
    ParseTail tail;
    parse(m, &c, &tail);
    c.session = s;
    if (c.type == T_CLOSE || c.type == T_TIPC_CLOSE) return;
    if (c.type >= T_TIPC_BASE) {
        dispatch_tipc(s, m, &c, &tail);
        return;
    }
    int control_msg = c.type == T_CONTROL || c.type == T_CONTROL_CTX;
    if (!control_msg && c.type != T_REQUEST && c.type != T_REQUEST_CTX) {
        LOG("ipc", "unsupported HIPC message type %u", c.type);
        runtime_fail("IPC message type %u not implemented", c.type);
    }
    const uint8_t *bytes = (const uint8_t *)tail.data_words, *end = bytes + tail.ndata * 4;
    const uint8_t *payload = (const uint8_t *)m + (((bytes - (const uint8_t *)m) + 15) & ~15);
    IpcObject *target = NULL;
    uint32_t object_ids[IPC_MAX_HANDLES];
    int domain_message = 0;
    if (s->is_domain && !control_msg) {
        /* DomainInHeader: u8 type (1 send, 2 close), u8 input objects, u16 data size, u32 object id, u32 pad, u32 token */
        uint8_t type = payload[0];
        uint16_t dsize;
        uint32_t oid;
        memcpy(&dsize, payload + 2, 2);
        memcpy(&oid, payload + 4, 4);
        pthread_mutex_lock(&s->dom->lock);
        target = oid && oid <= DOMAIN_MAX ? s->dom->objects[oid] : NULL;
        if (target && type == 2) {
            object_unref(target);
            s->dom->objects[oid] = NULL;
            target = NULL;
            pthread_mutex_unlock(&s->dom->lock);
            return;
        }
        pthread_mutex_unlock(&s->dom->lock);
        if (type != 1 || !target) {
            LOG("ipc", "domain message type %u object %u: no such object", type, oid);
            runtime_fail("IPC domain object %u not found", oid);
        }
        {
            unsigned nin = payload[1];
            const uint8_t *ids = payload + 16 + dsize; /* input objects follow the data */
            pthread_mutex_lock(&s->dom->lock);
            for (unsigned i = 0; i < nin && i < IPC_MAX_HANDLES && ids + 4 * (i + 1) <= end; i++) {
                uint32_t id;
                memcpy(&id, ids + 4 * i, 4);
                c.in_objects[c.nin_objects++] = id && id <= DOMAIN_MAX ? s->dom->objects[id] : NULL;
            }
            pthread_mutex_unlock(&s->dom->lock);
        }
        payload += 16;
        domain_message = 1;
    } else if (!control_msg)
        for (unsigned i = 0; i < c.ncopy; i++) { /* plain session: objects arrive as copy handles */
            IpcSession *as = handle_get(c.copy[i], OBJ_SESSION);
            if (as && as->object) c.in_objects[c.nin_objects++] = as->object;
        }
    uint32_t h[4];
    memcpy(h, payload, 16);
    if (h[0] != CMIF_IN_MAGIC) {
        LOG("ipc", "%s: bad CMIF magic %#x (type %u, domain %d); message words:", s->object ? s->object->iface : "(domain)", h[0], c.type, s->is_domain);
        for (unsigned i = 0; i < 16; i++) LOG("ipc", "  [%u] %08x", i, m[i]);
        runtime_fail("IPC bad CMIF header");
    }
    c.cmd = h[2];
    c.in = payload + 16;
    c.in_size = payload + 16 <= end ? (uint32_t)(end - payload - 16) : 0;
    if (!control_msg && !target) target = s->object;
    if (control_msg) {
        IpcObject ctl = {.iface = "(control)"};
        c.object = &ctl;
        summary_ipc("(control)", c.cmd, 0, c.in_size);
        control(s, &c, NULL);
    } else {
        run_object(target, &c);
    }
    /* New objects: domain object ids, or session handles. */
    for (unsigned i = 0; i < c.nout_objects; i++) {
        if (s->is_domain) {
            pthread_mutex_lock(&s->dom->lock);
            if (s->dom->next_id > DOMAIN_MAX) runtime_fail("IPC domain full");
            s->dom->objects[s->dom->next_id] = c.out_objects[i];
            object_ids[i] = s->dom->next_id++;
            pthread_mutex_unlock(&s->dom->lock);
        } else ipc_out_move_handle(&c, session_handle(c.out_objects[i]));
    }
    if (!s->is_domain) c.nout_objects = 0;
    (void)domain_message;
    respond(m, s->is_domain && !control_msg, &c, object_ids);
}

/* ---- sm: ------------------------------------------------------------------------------------------------------- */
static void sm_initialize(IpcObject *self, IpcCall *c) {
    (void)self;
    (void)c;
} /* RegisterClient(pid) */
/* The SDK's TIPC sm client (nn::sm GetServiceHandle) reads the reply at fixed positions: move handle at word 3, result at word 4, as in a successful
 * reply, without checking the special header first. An error reply laid out as header + result alone leaves word 4 holding whatever the request wrote
 * there (the high half of the service name), so the refusal read as "success, handle 0" (AC 3.0.x: vi:m/vi:s probes "succeeded", then a request on handle 0
 * killed boot; pctl:* likewise). Keep the success layout and put a 0 handle in it. */
static void sm_tipc_error_layout(IpcCall *c) {
    if (c->type >= T_TIPC_BASE) ipc_out_move_handle(c, 0);
}
static void sm_get_service(IpcObject *self, IpcCall *c) {
    (void)self;
    char name[9] = {0};
    memcpy(name, ipc_in(c, 0, 8), 8);
    pthread_mutex_lock(&registry_lock);
    const Service *found = NULL;
    for (unsigned i = 0; i < nservices; i++)
        if (!strncmp(services[i].name, name, 8)) found = &services[i];
    pthread_mutex_unlock(&registry_lock);
    int allowed = npdm_service_allowed(name);
    if (!allowed) { /* like the real service manager: a service outside the process's access list is refused, the SDK falls back */
        LOG("ipc", "sm: \"%s\" refused: not in the NPDM service access list", name);
        trace_event("service_refused", "\"service\":\"%s\"", name);
        c->result = IPC_R_NOT_FOUND(21, 8);
        sm_tipc_error_layout(c);
        return;
    }
    if (!found) {
        LOG("ipc", "sm: service \"%s\" is not implemented%s", name, allowed ? "" : " (and not in the NPDM access list)");
        summary_ipc(name, 0xffff, 2, 0);
        summary_note("missing service %s", name);
        runtime_trace_guest(trace_fp, trace_lr);
        if (!getenv("NAPPLE_IPC_LENIENT")) runtime_fail("IPC service \"%s\" not implemented", name);
        c->result = IPC_R_NOT_FOUND(21, 5); /* sm: ServiceNotRegistered-style error */
        sm_tipc_error_layout(c);
        return;
    }
    if (!allowed) LOG("ipc", "sm: note: \"%s\" is not in the NPDM service access list (a real console would refuse it)", name);
    ipc_out_move_handle(c, session_handle(found->open()));
    LOG("ipc", "opened service %s", found->name);
    trace_event("service_open", "\"service\":\"%s\"", found->name);
}
static const IpcCmd sm_cmds[] = {
    {0, "Initialize", sm_initialize, IPC_IMPL},
    {1, "GetServiceHandle", sm_get_service, IPC_IMPL},
};
static IpcObject *sm_open(void) { return IPC_OBJECT("sm:", sm_cmds, NULL, NULL); }

/* ---- svcs --------------------------------------------------------------------------------------------------------- */
static void svc_connect_to_named_port(uint64_t *r) {
    char name[13] = {0};
    const void *p = guest_ptr(r[1], 12);
    if (!p) {
        r[0] = R_INVALID_POINTER;
        return;
    }
    memcpy(name, p, 12);
    if (strcmp(name, "sm:")) {
        if (ksession_connect_named(name, r)) return; /* a port the game registered itself (ManageNamedPort) */
        LOG("ipc", "named port \"%s\" is not implemented", name);
        summary_ipc(name, 0xffff, 2, 0);
        runtime_fail("IPC named port \"%s\" not implemented", name);
    }
    r[0] = 0;
    r[1] = session_handle(sm_open());
}
static void svc_send_sync_request(uint64_t *r, int user_buffer) {
    uint32_t h = (uint32_t)(user_buffer ? r[2] : r[0]);
    if (ksession_is_client(h)) { /* a session to a server thread of the game (ksession.c) */
        uint32_t size = user_buffer ? (uint32_t)(r[1] < 0x10000 ? r[1] : 0x10000) : MSG_BYTES;
        uint32_t *m = user_buffer ? guest_ptr(r[0], size) : pthread_getspecific(runtime_keys[2]);
        r[0] = m ? ksession_send_request(h, m, size) : R_INVALID_POINTER;
        return;
    }
    IpcSession *s = handle_get(h, OBJ_SESSION);
    if (!s) {
        r[0] = R_INVALID_HANDLE;
        return;
    }
    uint32_t *m = user_buffer ? guest_ptr(r[0], MSG_BYTES) : pthread_getspecific(runtime_keys[2]);
    if (!m || (user_buffer && r[1] < MSG_BYTES)) runtime_fail("IPC message buffer missing");
    trace_fp = r[29];
    trace_lr = r[30];
    dispatch(s, m);
    r[0] = 0;
}
static void svc_send_sync(uint64_t *r) { svc_send_sync_request(r, 0); }
static void svc_send_sync_user(uint64_t *r) { svc_send_sync_request(r, 1); }
/* CreateSharedMemory(x1 size, w2 local permission, w3 remote permission) -> w1 handle. Zeroed host pages, mapped by MapSharedMemory. */
static void svc_create_shared_memory(uint64_t *r) {
    uint64_t size = r[1];
    uint32_t local = (uint32_t)r[2], remote = (uint32_t)r[3];
    if (!size || (size & 4095) || size > (4ull << 30)) {
        r[0] = R_INVALID_SIZE;
        return;
    }
    if ((local != 1 && local != 3) || (remote != 1 && remote != 3 && remote != 0x10000000u)) {
        r[0] = R_INVALID_MEMORY_PERMISSIONS;
        return;
    }
    IpcSharedMemory *m = ipc_shared_memory_new(size);
    uint32_t h = handle_new(OBJ_SHMEM, m); /* the handle owns the reference ipc_shared_memory_new returned */
    if (!h) {
        munmap(m->host, m->size);
        free(m);
        r[0] = R_OUT_OF_RESOURCE;
        return;
    }
    r[0] = 0;
    r[1] = h;
}
static void svc_create_transfer_memory(uint64_t *r) {
    IpcTransferMemory *t = malloc(sizeof *t);
    if (!t) {
        r[0] = R_OUT_OF_RESOURCE;
        return;
    }
    *t = (IpcTransferMemory){r[1], r[2], (uint32_t)r[3]};
    uint32_t h = handle_new(OBJ_TMEM, t);
    if (!h) {
        free(t);
        r[0] = R_OUT_OF_RESOURCE;
        return;
    }
    r[0] = 0;
    r[1] = h;
}

void svc_ipc_register(void) {
    svc_register(0x1f, "ConnectToNamedPort", svc_connect_to_named_port);
    svc_register(0x21, "SendSyncRequest", svc_send_sync);
    svc_register(0x22, "SendSyncRequestWithUserBuffer", svc_send_sync_user);
    svc_register(0x15, "CreateTransferMemory", svc_create_transfer_memory);
    svc_register(0x13, "MapSharedMemory", svc_map_shared_memory);
    svc_register(0x14, "UnmapSharedMemory", svc_unmap_shared_memory);
    svc_register(0x51, "MapTransferMemory", svc_map_transfer_memory);
    svc_register(0x52, "UnmapTransferMemory", svc_map_transfer_memory);
    svc_register(0x50, "CreateSharedMemory", svc_create_shared_memory);
    svc_ksession_register();
}
