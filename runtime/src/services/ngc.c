/* ngc:u (nn::ngc): the profanity filter. Animal Crossing asks for it right after the timezone service during boot. The word lists are not shipped with the
 * runtime, so nothing is ever flagged: Check reports "clean" and Mask returns the text unchanged. Command ids and layouts are from the author's memory of
 * the public documentation (unverified). */
#include "svc_common.h"

static void ngc_content_version(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = 1; IPC_OUT(c, v); }
static void ngc_check(IpcObject *s, IpcCall *c) { (void)s; uint32_t flags = 0; IPC_OUT(c, flags); }
static void ngc_mask(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0);
    if (in && out) { uint64_t n = in->size < out->size ? in->size : out->size; if (n) ipc_write_buf(c, out, ipc_buf_ptr(in), n); }
    uint32_t masked = 0; IPC_OUT(c, masked);
}
static const IpcCmd ngc_cmds[] = {
    {0, "GetContentVersion", ngc_content_version, IPC_STUB}, {1, "Check", ngc_check, IPC_STUB},
    {2, "Mask", ngc_mask, IPC_STUB}, {3, "Reload", svc_noop, IPC_STUB},
};
static IpcObject *ngc_open(void) { return IPC_OBJECT("ngc:u", ngc_cmds, NULL, NULL); }
void service_register_ngc(void) { ipc_register_service("ngc:u", ngc_open); }
