/* fgm (nn::fgm): frequency governor requests. The host schedules CPU/GPU itself: a request is accepted, its range remembered, and its
 * event never fires (no governor ever raises or lowers the clock). Command ids are from the author's memory of the public
 * documentation (unverified). */
#include "svc_common.h"
#include <stdlib.h>

typedef struct { uint32_t module, priority, min, max; } FgmRequest;
static void rq_initialize(IpcObject *s, IpcCall *c) {      /* 0(u32 module, u32 priority) -> event */
    FgmRequest *r = s->state; r->module = IPC_IN(c, uint32_t, 0); r->priority = IPC_IN(c, uint32_t, 4); svc_ret_event(s, c);
}
static void rq_set(IpcObject *s, IpcCall *c) {              /* 1(u32 min, u32 max) */
    FgmRequest *r = s->state; uint32_t lo = IPC_IN(c, uint32_t, 0), hi = IPC_IN(c, uint32_t, 4);
    r->min = lo; r->max = hi < lo ? lo : hi;                /* a reversed range collapses to its lower bound */
}
static void rq_get(IpcObject *s, IpcCall *c) { FgmRequest *r = s->state; uint32_t v = r->max; IPC_OUT(c, v); }
static void rq_cancel(IpcObject *s, IpcCall *c) { (void)c; FgmRequest *r = s->state; r->min = r->max = 0; }
static void rq_destroy(IpcObject *o) { free(o->state); }
static const IpcCmd request_cmds[] = {{0, "Initialize", rq_initialize, IPC_STUB}, {1, "Set", rq_set, IPC_STUB}, {2, "Get", rq_get, IPC_STUB}, {3, "Cancel", rq_cancel, IPC_STUB}};
static void fgm_initialize(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IRequest", request_cmds, calloc(1, sizeof(FgmRequest)), rq_destroy)); }
static const IpcCmd fgm_cmds[] = {{0, "Initialize", fgm_initialize, IPC_STUB}};
static IpcObject *fgm_open(void) { return IPC_OBJECT("fgm", fgm_cmds, NULL, NULL); }
void service_register_fgm(void) { ipc_register_service("fgm", fgm_open); ipc_register_service("fgm:0", fgm_open); ipc_register_service("fgm:9", fgm_open); }
