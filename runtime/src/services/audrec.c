/* audrec:u (nn::audio final output recorder): records the console's mixed output. The host has no tap on the mix, so an opened
 * recorder records silence and completes appended buffers at once (see audrec_core.h). OpenFinalOutputRecorder takes {u32 sample rate,
 * u32 unknown}, a u64 applet resource user id and a process handle, and returns {rate, channels, format, state} plus the recorder object.
 * Command ids and field layouts are from the author's memory of the public documentation (unverified). */
#include "svc_common.h"
#include "audrec_core.h"
#include "hz_result.h"
#include <stdlib.h>

typedef struct { AudrecQueue q; uint32_t event_w; } Rec;
static void signal_if_ready(Rec *r) { if (r->event_w && r->q.nreleased) event_signal_handle(r->event_w); }

static void ar_state(IpcObject *s, IpcCall *c) { Rec *r = s->state; uint32_t v = r->q.started ? 0 : 1; IPC_OUT(c, v); }
static void ar_start(IpcObject *s, IpcCall *c) { (void)c; Rec *r = s->state; audrec_start(&r->q); signal_if_ready(r); }
static void ar_stop(IpcObject *s, IpcCall *c) { (void)c; Rec *r = s->state; audrec_stop(&r->q); }
/* in: {u64 tag}; buffer 0: FinalOutputRecorderBuffer {next, data pointer, capacity, size, offset} (0x28). The data is zero-filled. */
static void ar_append(IpcObject *s, IpcCall *c) {
    Rec *r = s->state; uint64_t tag = IPC_IN(c, uint64_t, 0);
    const IpcBuf *b = ipc_in_buf(c, 0); const uint8_t *p = b ? ipc_buf_ptr(b) : NULL;
    uint64_t ptr = 0, cap = 0, size = 0, off = 0;
    if (p && b->size >= 0x28) { memcpy(&ptr, p + 8, 8); memcpy(&cap, p + 0x10, 8); memcpy(&size, p + 0x18, 8); memcpy(&off, p + 0x20, 8); }
    if (audrec_append(&r->q, tag, size)) { c->result = HZ_R_AUDREC_BUFFER_COUNT_LIMIT; return; }
    if (cap && off <= cap) { uint64_t n = size < cap - off ? size : cap - off; void *dst = guest_ptr(ptr + off, n); if (dst) memset(dst, 0, n); }
    signal_if_ready(r);
}
static void ar_event(IpcObject *s, IpcCall *c) { Rec *r = s->state; uint32_t rd = 0, w = event_create(&rd); r->event_w = w; signal_if_ready(r); ipc_out_copy_handle(c, rd); }
/* out: {u32 count, pad, u64 end time of the last returned buffer}; buffer 0 receives the u64 tags */
static void ar_released(IpcObject *s, IpcCall *c) {
    Rec *r = s->state; uint64_t tags[AUDREC_QUEUE_MAX]; const IpcBuf *b = ipc_out_buf(c, 0);
    unsigned max = b ? (unsigned)(b->size / 8) : 0; if (max > AUDREC_QUEUE_MAX) max = AUDREC_QUEUE_MAX;
    uint64_t end = 0; unsigned n = audrec_release(&r->q, tags, max, &end);
    if (b && n) ipc_write_buf(c, b, tags, (uint64_t)n * 8);
    if (!r->q.nreleased && r->event_w) event_clear_handle(r->event_w);
    uint64_t out[2] = {n, end}; ipc_out(c, out, sizeof out);
}
static void ar_contains(IpcObject *s, IpcCall *c) { Rec *r = s->state; uint8_t v = (uint8_t)audrec_contains(&r->q, IPC_IN(c, uint64_t, 0)); IPC_OUT(c, v); }
static void ar_end_time(IpcObject *s, IpcCall *c) {
    Rec *r = s->state; uint64_t end = 0;
    if (!audrec_end_time(&r->q, IPC_IN(c, uint64_t, 0), &end)) { c->result = HZ_R_AUDREC_BUFFER_NOT_FOUND; return; }
    IPC_OUT(c, end);
}
static const IpcCmd recorder_cmds[] = {
    {0, "GetFinalOutputRecorderState", ar_state, IPC_IMPL}, {1, "StartFinalOutputRecorder", ar_start, IPC_IMPL}, {2, "StopFinalOutputRecorder", ar_stop, IPC_IMPL},
    {3, "AppendFinalOutputRecorderBuffer", ar_append, IPC_IMPL}, {4, "RegisterBufferEvent", ar_event, IPC_IMPL},
    {5, "GetReleasedFinalOutputRecorderBuffers", ar_released, IPC_IMPL}, {6, "ContainsFinalOutputRecorderBuffer", ar_contains, IPC_IMPL},
    {7, "GetFinalOutputRecorderBufferEndTime", ar_end_time, IPC_IMPL}, {8, "AppendFinalOutputRecorderBufferAuto", ar_append, IPC_IMPL},
    {9, "GetReleasedFinalOutputRecorderBuffersAuto", ar_released, IPC_IMPL},
};
static void ar_destroy(IpcObject *o) { free(o->state); }

static void mgr_open(IpcObject *s, IpcCall *c) {
    (void)s; Rec *r = calloc(1, sizeof *r); audrec_init(&r->q);
    uint32_t out[4] = {AUDREC_RATE, AUDREC_CHANNELS, 2 /* PCM int16 */, 1 /* Stopped */};
    ipc_out(c, out, sizeof out);
    ipc_out_object(c, IPC_OBJECT("IFinalOutputRecorder", recorder_cmds, r, ar_destroy));
}
static const IpcCmd mgr_cmds[] = { {0, "OpenFinalOutputRecorder", mgr_open, IPC_IMPL} };
static IpcObject *audrec_open(void) { return IPC_OBJECT("audrec:u", mgr_cmds, NULL, NULL); }
void service_register_audrec(void) { ipc_register_service("audrec:u", audrec_open); }
