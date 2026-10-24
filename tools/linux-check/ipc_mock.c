/* Linux stand-in for the runtime pieces the services call (ipc.c, kernel handles/events, log, memory). Used only by the unit tests.
 * Framing helpers are copies of ipc.c's; guest addresses are host addresses (as in the real runtime). */
#include "../../runtime/src/ipc.h"
#include "../../runtime/src/timescale.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

RuntimeConfig g_cfg; uint64_t g_program_id = 0x0100000000001000ULL; int log_verbose;
uint64_t mock_ticks; int64_t mock_skew_ns;

size_t strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { size_t k = l >= n ? n - 1 : l; memcpy(d, s, k); d[k] = 0; } return l; }
void arc4random_buf(void *b, size_t n) { for (size_t i = 0; i < n; i++) ((uint8_t *)b)[i] = (uint8_t)(i * 7 + 1); }
void log_line(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void summary_note(const char *fmt, ...) { (void)fmt; }
void summary_stage(const char *s) { (void)s; }
void runtime_fail(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fprintf(stderr, "runtime_fail: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap); exit(70); }
uint64_t runtime_guest_ticks(void) { return mock_ticks; }
int64_t timescale_wall_skew_ns(void) { return mock_skew_ns; }
void *guest_ptr(uint64_t a, uint64_t n) { (void)n; return a ? (void *)(uintptr_t)a : NULL; }
void guest_write_prepare(uint64_t a, size_t n) { (void)a; (void)n; }

/* handles and events */
#define MAXH 256
static struct { ObjType t; void *o; int signaled; uint32_t pair; } H[MAXH]; static uint32_t nh = 1;
uint32_t handle_new(ObjType t, void *o) { if (nh >= MAXH) runtime_fail("mock: out of handles"); H[nh].t = t; H[nh].o = o; return nh++; }
void *handle_get(uint32_t h, ObjType t) { return h && h < nh && H[h].t == t ? H[h].o : NULL; }
ObjType handle_type(uint32_t h) { return h && h < nh ? H[h].t : OBJ_FREE; }
uint32_t event_create(uint32_t *r) { uint32_t w = handle_new(OBJ_EVENT_W, NULL), rd = handle_new(OBJ_EVENT_R, NULL); H[w].pair = rd; H[rd].pair = w; *r = rd; return w; }
void event_signal_handle(uint32_t w) { if (w && w < nh) { H[w].signaled = 1; H[H[w].pair].signaled = 1; } }
void event_clear_handle(uint32_t h) { if (h && h < nh) { H[h].signaled = 0; H[H[h].pair].signaled = 0; } }
int event_is_signaled(uint32_t r) { return r && r < nh && H[r].signaled; }

/* services */
typedef struct { char name[16]; IpcOpen open; } Reg; static Reg regs[128]; static unsigned nregs;
void ipc_register_service(const char *name, IpcOpen open) { if (nregs < 128) { snprintf(regs[nregs].name, 16, "%s", name); regs[nregs++].open = open; } }
IpcObject *mock_open_service(const char *name) { for (unsigned i = 0; i < nregs; i++) if (!strcmp(regs[i].name, name)) return regs[i].open(); return NULL; }
IpcObject *ipc_object_new(const char *iface, const IpcCmd *cmds, size_t n, void *state, void (*destroy)(IpcObject *)) {
    IpcObject *o = calloc(1, sizeof *o); o->iface = iface; o->cmds = cmds; o->ncmds = n; o->state = state; o->destroy = destroy; o->refs = 1; return o;
}
const void *ipc_in(const IpcCall *c, uint32_t off, uint32_t n) { if (off + n > c->in_size) runtime_fail("IPC argument out of range (%s cmd %u)", c->object ? c->object->iface : "?", c->cmd); return c->in + off; }
static const IpcBuf *pick_buf(const IpcBuf *l1, unsigned n1, const IpcBuf *l2, unsigned n2, unsigned n) {
    unsigned k = 0;
    for (unsigned i = 0; i < n1 + n2; i++) { const IpcBuf *b = i < n1 ? &l1[i] : &l2[i - n1]; if (b->size && k++ == n) return b; }
    if (n < n1) return &l1[n];
    return n - n1 < n2 ? &l2[n - n1] : NULL;
}
const IpcBuf *ipc_in_buf(const IpcCall *c, unsigned n) { return pick_buf(c->a, c->na, c->x, c->nx, n); }
const IpcBuf *ipc_out_buf(const IpcCall *c, unsigned n) { return pick_buf(c->b, c->nb, c->c, c->nc, n); }
void *ipc_buf_ptr(const IpcBuf *b) { return b ? guest_ptr(b->addr, b->size) : NULL; }
uint64_t ipc_write_buf(IpcCall *c, const IpcBuf *b, const void *data, uint64_t n) {
    if (!b) return 0;
    uint64_t w = n < b->size ? n : b->size; void *p = guest_ptr(b->addr, w);
    if (w) memcpy(p, data, w);
    if (b >= c->c && b < c->c + IPC_MAX_BUF && c->nxout < IPC_MAX_BUF) c->xout[c->nxout++] = (IpcBuf){b->addr, w, b->index};
    return w;
}
void ipc_out(IpcCall *c, const void *p, uint32_t n) {
    uint32_t a = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
    c->out_size = (c->out_size + a - 1) & ~(a - 1);
    if (c->out_size + n > sizeof c->out) runtime_fail("IPC response data too large");
    memcpy(c->out + c->out_size, p, n); c->out_size += n;
}
void ipc_out_zero(IpcCall *c, uint32_t n) { uint8_t z[0x100] = {0}; ipc_out(c, z, n); }
void ipc_out_copy_handle(IpcCall *c, uint32_t h) { c->out_copy[c->nout_copy++] = h; }
void ipc_out_move_handle(IpcCall *c, uint32_t h) { c->out_move[c->nout_move++] = h; }
void ipc_out_object(IpcCall *c, IpcObject *o) { c->out_objects[c->nout_objects++] = o; }
void ipc_fail(IpcCall *c, uint32_t r) { c->result = r; }
IpcSharedMemory *ipc_shared_memory_new(uint64_t size) { IpcSharedMemory *m = calloc(1, sizeof *m); size = (size + 0x3fff) & ~0x3fffull; m->host = calloc(1, size); m->size = size; m->refs = 1; m->owned = 1; return m; }
uint32_t ipc_shared_memory_handle(IpcSharedMemory *m) { m->refs++; return handle_new(OBJ_SHMEM, m); }
void ipc_shared_memory_flush(IpcSharedMemory *m) { (void)m; }
