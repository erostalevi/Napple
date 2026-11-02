/* ovln:snd (nn::ovln sender): overlay notifications (battery low, friend online, ...). Layouts from switchbrew 'PSC services' (ovln section):
 *   ovln:snd 0 OpenSender(SourceName {char name[0x10]}, QueueAttribute {u32 QueueLength; u32 reserved}) -> ISender
 *   ISender  0 Send(RawMessage {u32 Tag; u32 DataSize; u8 Data[0x78]}, SendOption {u8 EnqueuePosition (0 front, 1 back); u8 OverflowOption (0 Error,
 *              1 RemoveFront, 2 RemoveBack, 3 Block); u8 reserved[6]})
 *            1 GetUnreceivedMessageCount() -> u32
 * Each sender owns a queue of QueueLength messages that the overlay process takes from (ovln:rcv IReceiver, AddSource by source name). The host runs no
 * overlay, so no receiver ever takes a message: they stay queued, the count is what GetUnreceivedMessageCount reports, and a full queue behaves as its
 * OverflowOption says. The first 16 bytes of the last message are kept for the log (as before). */
#include "svc_common.h"
#include "sys_default.h"
#include <stdlib.h>
#include <string.h>

#define OVLN_DEFAULT_QUEUE 16 /* [unverified] a QueueLength of 0, or an OpenSender without its attribute */
#define OVLN_MAX_QUEUE 64
enum { OVLN_POS_FRONT = 0, OVLN_POS_BACK = 1 };
enum { OVLN_OVERFLOW_ERROR = 0, OVLN_OVERFLOW_REMOVE_FRONT = 1, OVLN_OVERFLOW_REMOVE_BACK = 2, OVLN_OVERFLOW_BLOCK = 3 };
typedef struct {
    char source[16];
    uint32_t queue_length, n;
    uint32_t tags[OVLN_MAX_QUEUE]; /* the queued messages' tags, front first */
} OvlnSender;

static unsigned sent;
static uint8_t last_id[16];
static void sender_send(IpcObject *s, IpcCall *c) {
    OvlnSender *q = s->state;
    sent++;
    if (c->in_size >= 16) memcpy(last_id, c->in, 16);
    uint32_t tag = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0;
    /* a request without its SendOption (shorter than 0x80 + 8): to the back, Error on overflow [napple's choice] */
    uint8_t pos = c->in_size >= 0x82 ? IPC_IN(c, uint8_t, 0x80) : OVLN_POS_BACK, overflow = c->in_size >= 0x82 ? IPC_IN(c, uint8_t, 0x81) : OVLN_OVERFLOW_ERROR;
    if (q && q->n >= q->queue_length) {
        if (overflow == OVLN_OVERFLOW_REMOVE_FRONT) memmove(q->tags, q->tags + 1, --q->n * sizeof q->tags[0]);
        else if (overflow == OVLN_OVERFLOW_REMOVE_BACK) q->n--;
        else { /* Error; Block would wait for a receiver that never comes on this host, so it fails the same way */
            LOGV("ovln", "notification %u (tag %#x): the queue of '%.16s' is full (%u)", sent, tag, q->source, q->n);
            c->result = HZ_R_OVLN_QUEUE_FULL;
            return;
        }
    }
    if (q) {
        if (pos == OVLN_POS_FRONT) {
            memmove(q->tags + 1, q->tags, q->n * sizeof q->tags[0]);
            q->tags[0] = tag;
        } else q->tags[q->n] = tag;
        q->n++;
    }
    LOGV("ovln", "notification %u (tag %#x, id %02x%02x%02x%02x..), %u unreceived", sent, tag, last_id[0], last_id[1], last_id[2], last_id[3], q ? q->n : 0);
}
static void sender_unreceived(IpcObject *s, IpcCall *c) {
    OvlnSender *q = s->state;
    uint32_t v = q ? q->n : 0;
    IPC_OUT(c, v);
}
static const IpcCmd sender_cmds[] = {{0, "Send", sender_send, IPC_IMPL}, {1, "GetUnreceivedMessageCount", sender_unreceived, IPC_IMPL}};
static void sender_destroy(IpcObject *o) { free(o->state); }
static void ovln_open_sender(IpcObject *s, IpcCall *c) {
    (void)s;
    OvlnSender *q = calloc(1, sizeof *q);
    if (!q) {
        c->result = IPC_R_UNKNOWN_COMMAND;
        return;
    }
    if (c->in_size >= 16) memcpy(q->source, c->in, 16);
    uint32_t len = c->in_size >= 0x14 ? IPC_IN(c, uint32_t, 0x10) : 0;
    q->queue_length = !len ? OVLN_DEFAULT_QUEUE : len > OVLN_MAX_QUEUE ? OVLN_MAX_QUEUE : len;
    ipc_out_object(c, IPC_OBJECT("ISender", sender_cmds, q, sender_destroy));
}
static const IpcCmd ovln_cmds[] = {{0, "OpenSender", ovln_open_sender, IPC_IMPL}};
static IpcObject *ovln_open(void) { return IPC_OBJECT("ovln:snd", ovln_cmds, NULL, NULL); }
void service_register_ovln(void) { ipc_register_service("ovln:snd", ovln_open); }
