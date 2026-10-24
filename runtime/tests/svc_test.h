/* Helpers for the service unit tests (shared by the macOS and Linux builds): look commands up in a service object's table and call them
 * with a fabricated IpcCall, the way the dispatcher would. Tests #include the service source to reach its static handlers. */
#ifndef RT_SVC_TEST_H
#define RT_SVC_TEST_H
#include "../src/ipc.h"
#include "check.h"

static const IpcCmd *svc_find(const IpcObject *o, unsigned cmd) {
    for (size_t i = 0; i < o->ncmds; i++) if (o->cmds[i].cmd == cmd) return &o->cmds[i];
    return NULL;
}
/* Calls `cmd` on `o`. Returns the handler's call state in *c (c->result, c->out, c->out_objects, ...). A missing command is reported as a failure. */
static int svc_call(IpcObject *o, unsigned cmd, const void *in, uint32_t n, IpcCall *c) {
    const IpcCmd *e = svc_find(o, cmd);
    memset(c, 0, sizeof *c); c->object = o; c->cmd = cmd; c->in = in; c->in_size = n;
    CHECK(e != NULL, "%s: command %u is not in the table", o->iface, cmd);
    if (!e) return 0;
    e->fn(o, c);
    return 1;
}
/* Same, but for a call the test prepared itself (buffers, handles): only the object, command and result are set. */
static int svc_call_c(IpcObject *o, unsigned cmd, IpcCall *c) {
    const IpcCmd *e = svc_find(o, cmd);
    c->object = o; c->cmd = cmd; c->result = 0;
    CHECK(e != NULL, "%s: command %u is not in the table", o->iface, cmd);
    if (!e) return 0;
    e->fn(o, c);
    return 1;
}
#define SVC_OUT(c, T, off) (*(const T *)((c)->out + (off)))
static void svc_set_out_buf(IpcCall *c, void *p, uint64_t n) { c->b[c->nb++] = (IpcBuf){(uint64_t)(uintptr_t)p, n, 0}; }
static void svc_set_in_buf(IpcCall *c, const void *p, uint64_t n) { c->a[c->na++] = (IpcBuf){(uint64_t)(uintptr_t)p, n, 0}; }
#endif
