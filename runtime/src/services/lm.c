/* lm: system log service (nn::diag logging). OpenLogger -> ILogger { Log, SetDestination }.
 * The log packet (public format) carries the text of the guest's own log lines; they are forwarded to the runtime log. */
#include "../ipc.h"
#include <stdio.h>
#include <string.h>

static void logger_log(IpcObject *self, IpcCall *c) {
    (void)self;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b ? ipc_buf_ptr(b) : NULL;
    if (!p || b->size < 24) return;
    /* header: u64 process id, u64 thread id, u8 flags, u8 pad, u8 severity, u8 verbosity, u32 payload size; then chunks {u8 type, u8 length, data} */
    uint32_t payload; memcpy(&payload, p + 20, 4);
    uint8_t severity = p[18];
    if (payload > b->size - 24) payload = (uint32_t)(b->size - 24);
    char text[1024]; size_t n = 0;
    for (uint32_t i = 0; i + 2 <= payload;) {
        uint8_t type = p[24 + i], len = p[24 + i + 1];
        if (i + 2 + len > payload) break;
        if (type == 2 && n + len < sizeof text) { memcpy(text + n, p + 24 + i + 2, len); n += len; }   /* LogMessage chunk */
        i += 2 + len;
    }
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
    text[n] = 0;
    if (n) LOG("guest-log", "[sev %u] %s", severity, text);
}
static void logger_set_destination(IpcObject *self, IpcCall *c) { (void)self; (void)c; }
static const IpcCmd logger_cmds[] = {{0, "Log", logger_log, IPC_IMPL}, {1, "SetDestination", logger_set_destination, IPC_IMPL}};

static void lm_open_logger(IpcObject *self, IpcCall *c) { (void)self; ipc_out_object(c, IPC_OBJECT("ILogger", logger_cmds, NULL, NULL)); }
static const IpcCmd lm_cmds[] = {{0, "OpenLogger", lm_open_logger, IPC_IMPL}};
static IpcObject *lm_open(void) { return IPC_OBJECT("lm", lm_cmds, NULL, NULL); }
void service_register_lm(void) { ipc_register_service("lm", lm_open); }
