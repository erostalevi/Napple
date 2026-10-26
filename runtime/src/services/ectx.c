/* ectx:aw (nn::err::context writer for applications): error context reports are accepted and remembered (the last result is logged);
 * nothing is stored or uploaded. Command ids are guesses from memory of the public documentation (unverified). */
#include "svc_common.h"

static uint32_t last_result;
static unsigned reports;
static void ectx_set_result(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size >= 4) last_result = IPC_IN(c, uint32_t, 0);
    reports++;
    LOG("ectx", "error context: result %#x (report %u)", last_result, reports);
}
static void ectx_accept(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    reports++;
}
static const IpcCmd writer_cmds[] = {{0, "SetResult", ectx_set_result, IPC_IMPL}, {1, "SetResultWithContext", ectx_accept, IPC_IMPL}};
static IpcObject *ectx_open(void) { return IPC_OBJECT("ectx:aw", writer_cmds, NULL, NULL); }
void service_register_ectx(void) { ipc_register_service("ectx:aw", ectx_open); }
