/* ectx:aw (nn::err::context writer for applications): error context reports are accepted and remembered (the last result is logged);
 * nothing is stored or uploaded. */
#include "svc_common.h"

/* ectx:aw rows per switchbrew 'Glue services': 0 CreateContextRegistrar (returns an IContextRegistrar), 1 CommitContext; IContextRegistrar 0 Complete. The
 * guest describes an error context (the result code first); it is logged and counted, nothing is stored or uploaded. */
static uint32_t last_result;
static unsigned reports, registrars, completed;
static void ectx_note(const IpcCall *c) {
    if (c->in_size >= 4) last_result = IPC_IN(c, uint32_t, 0);
    reports++;
    LOG("ectx", "error context: result %#x (report %u)", last_result, reports);
}
static void ectx_complete(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    completed++;
}
static const IpcCmd registrar_cmds[] = {{0, "Complete", ectx_complete, IPC_IMPL}};
static void ectx_create_registrar(IpcObject *s, IpcCall *c) {
    (void)s;
    registrars++;
    ectx_note(c);
    ipc_out_object(c, IPC_OBJECT("IContextRegistrar", registrar_cmds, NULL, NULL));
}
static void ectx_commit(IpcObject *s, IpcCall *c) {
    (void)s;
    ectx_note(c);
}
static const IpcCmd writer_cmds[] = {{0, "CreateContextRegistrar", ectx_create_registrar, IPC_IMPL}, {1, "CommitContext", ectx_commit, IPC_IMPL}};
static IpcObject *ectx_open(void) { return IPC_OBJECT("ectx:aw", writer_cmds, NULL, NULL); }
void service_register_ectx(void) { ipc_register_service("ectx:aw", ectx_open); }
