/* audin core: audio-in buffer bookkeeping (portable). The host has no microphone: a started input "records" silence and completes
 * every appended buffer immediately (a timed completion would need a timer thread to signal the guest's event). */
#pragma once
#include <stdint.h>

#define AUDIN_QUEUE_MAX 32
typedef struct {
    int started;                                    /* AudioInState: 0 Started, 1 Stopped on the wire */
    uint64_t queued[AUDIN_QUEUE_MAX]; unsigned nqueued;     /* appended while stopped */
    uint64_t released[AUDIN_QUEUE_MAX]; unsigned nreleased; /* completed, not yet fetched by the guest */
} AudinQueue;

void audin_negotiate(uint32_t req_rate, uint32_t req_channels, uint32_t *rate, uint32_t *channels);   /* snap to what the device can do */
void audin_init(AudinQueue *q);
void audin_start(AudinQueue *q);                    /* completes anything queued while stopped */
void audin_stop(AudinQueue *q);
int audin_append(AudinQueue *q, uint64_t tag);      /* 0, or -1 when the queue is full; returns with the buffer released if started */
unsigned audin_release(AudinQueue *q, uint64_t *out, unsigned max);   /* hands released tags to the guest, oldest first */
int audin_contains(const AudinQueue *q, uint64_t tag);                 /* queued or released-but-unfetched */
unsigned audin_count(const AudinQueue *q);          /* buffers appended and not yet fetched */
int audin_flush(AudinQueue *q);                     /* releases everything queued; returns 1 if there was something */
