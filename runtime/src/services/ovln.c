/* ovln:snd (nn::ovln sender): overlay notifications (battery low, friend online, ...). Messages are accepted and counted; the host shows
 * no overlay. The first 16 bytes of a message are its source id and are kept for the log. Command ids are from the author's memory of
 * the public documentation (unverified). */
#include "svc_common.h"

static unsigned sent;
static uint8_t last_id[16];
static void sender_send(IpcObject *s, IpcCall *c) {
    (void)s;
    sent++;
    if (c->in_size >= 16) memcpy(last_id, c->in, 16);
    LOGV("ovln", "notification %u (id %02x%02x%02x%02x..)", sent, last_id[0], last_id[1], last_id[2], last_id[3]);
}
static const IpcCmd sender_cmds[] = {{0, "Send", sender_send, IPC_STUB}};
static void ovln_open_sender(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("ISender", sender_cmds, NULL, NULL));
}
static const IpcCmd ovln_cmds[] = {{0, "OpenSender", ovln_open_sender, IPC_STUB}};
static IpcObject *ovln_open(void) { return IPC_OBJECT("ovln:snd", ovln_cmds, NULL, NULL); }
void service_register_ovln(void) { ipc_register_service("ovln:snd", ovln_open); }
