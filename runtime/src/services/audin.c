/* audin:u (nn::audio in): microphone capture. The device list holds the built-in headset input; an opened input records silence, and
 * appended buffers are zero-filled and complete at once (see audin_core.h). Buffer layout AudioInBuffer (0x28): {next, data pointer,
 * capacity, size, offset}. Command ids and field layouts are from the author's memory of the public documentation (unverified). */
#include "svc_common.h"
#include "audin_core.h"
#include "hz_result.h"
#include <stdlib.h>

typedef struct {
    AudinQueue q;
    uint32_t event_w;
    uint32_t rate, channels;
} AudIn;
static void signal_if_ready(AudIn *a) {
    if (a->event_w && a->q.nreleased) event_signal_handle(a->event_w);
}

static void ai_state(IpcObject *s, IpcCall *c) {
    AudIn *a = s->state;
    uint32_t v = a->q.started ? 0 : 1;
    IPC_OUT(c, v);
}
static void ai_start(IpcObject *s, IpcCall *c) {
    (void)c;
    AudIn *a = s->state;
    audin_start(&a->q);
    signal_if_ready(a);
}
static void ai_stop(IpcObject *s, IpcCall *c) {
    (void)c;
    AudIn *a = s->state;
    audin_stop(&a->q);
}
static void ai_append(IpcObject *s, IpcCall *c) {
    AudIn *a = s->state;
    uint64_t tag = IPC_IN(c, uint64_t, 0);
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b ? ipc_buf_ptr(b) : NULL;
    if (audin_append(&a->q, tag)) {
        c->result = HZ_RESULT(HZ_MOD_AUDIO_IN, 8);
        return;
    }                           /* [unverified] buffer count limit */
    if (p && b->size >= 0x28) { /* silence into the guest's data buffer */
        uint64_t ptr, cap, size, off;
        memcpy(&ptr, p + 8, 8);
        memcpy(&cap, p + 0x10, 8);
        memcpy(&size, p + 0x18, 8);
        memcpy(&off, p + 0x20, 8);
        void *dst = (cap && off <= cap) ? guest_ptr(ptr + off, size < cap - off ? size : cap - off) : NULL;
        if (dst) memset(dst, 0, size < cap - off ? size : cap - off);
    }
    signal_if_ready(a);
}
static void ai_event(IpcObject *s, IpcCall *c) {
    AudIn *a = s->state;
    uint32_t r = 0, w = event_create(&r);
    a->event_w = w;
    signal_if_ready(a);
    ipc_out_copy_handle(c, r);
}
static void ai_released(IpcObject *s, IpcCall *c) {
    AudIn *a = s->state;
    uint64_t tags[AUDIN_QUEUE_MAX];
    const IpcBuf *b = ipc_out_buf(c, 0);
    unsigned max = b ? (unsigned)(b->size / 8) : 0;
    if (max > AUDIN_QUEUE_MAX) max = AUDIN_QUEUE_MAX;
    unsigned n = audin_release(&a->q, tags, max);
    if (b && n) ipc_write_buf(c, b, tags, (uint64_t)n * 8);
    if (!a->q.nreleased && a->event_w) event_clear_handle(a->event_w);
    uint32_t cnt = n;
    IPC_OUT(c, cnt);
}
static void ai_contains(IpcObject *s, IpcCall *c) {
    AudIn *a = s->state;
    uint8_t v = (uint8_t)audin_contains(&a->q, IPC_IN(c, uint64_t, 0));
    IPC_OUT(c, v);
}
static void ai_count(IpcObject *s, IpcCall *c) {
    AudIn *a = s->state;
    uint32_t v = audin_count(&a->q);
    IPC_OUT(c, v);
}
static void ai_flush(IpcObject *s, IpcCall *c) {
    AudIn *a = s->state;
    uint8_t v = (uint8_t)audin_flush(&a->q);
    IPC_OUT(c, v);
    signal_if_ready(a);
}
static void ai_gain(IpcObject *s, IpcCall *c) {
    (void)s;
    float v = 1.0f;
    IPC_OUT(c, v);
}
static const IpcCmd audioin_cmds[] = {
    {0, "GetAudioInState", ai_state, IPC_IMPL},
    {1, "Start", ai_start, IPC_IMPL},
    {2, "Stop", ai_stop, IPC_IMPL},
    {3, "AppendAudioInBuffer", ai_append, IPC_IMPL},
    {4, "RegisterBufferEvent", ai_event, IPC_IMPL},
    {5, "GetReleasedAudioInBuffer", ai_released, IPC_IMPL},
    {6, "ContainsAudioInBuffer", ai_contains, IPC_IMPL},
    {7, "AppendAudioInBufferAuto", ai_append, IPC_IMPL},
    {8, "GetReleasedAudioInBufferAuto", ai_released, IPC_IMPL},
    {9, "GetAudioInBufferCount", ai_count, IPC_IMPL},
    {10, "SetDeviceGain", svc_noop, IPC_STUB},
    {11, "GetDeviceGain", ai_gain, IPC_STUB},
    {12, "FlushAudioInBuffers", ai_flush, IPC_IMPL},
};
static void ai_destroy(IpcObject *o) { free(o->state); }

static void mgr_list(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    int32_t n = 0;
    if (b && b->size >= 0x20) {
        char name[0x20] = "BuiltInHeadset";
        ipc_write_buf(c, b, name, sizeof name);
        n = 1;
    }
    IPC_OUT(c, n);
}
static void mgr_open(IpcObject *s, IpcCall *c) { /* in: {u32 sample rate, u16 channels, u16 pad, ...}; out: {rate, channels, format, state} */
    (void)s;
    AudIn *a = calloc(1, sizeof *a);
    audin_init(&a->q);
    uint32_t rate = IPC_IN(c, uint32_t, 0);
    uint32_t ch = IPC_IN(c, uint16_t, 4);
    audin_negotiate(rate, ch, &a->rate, &a->channels);
    const IpcBuf *name_in = ipc_in_buf(c, 0), *name_out = ipc_out_buf(c, 0);
    if (name_in && name_out) {
        const void *p = ipc_buf_ptr(name_in);
        if (p) ipc_write_buf(c, name_out, p, name_in->size);
    }
    uint32_t out[4] = {a->rate, a->channels, 2 /* PCM int16 */, 1 /* Stopped */};
    ipc_out(c, out, sizeof out);
    ipc_out_object(c, IPC_OBJECT("IAudioIn", audioin_cmds, a, ai_destroy));
}
static const IpcCmd mgr_cmds[] = {
    {0, "ListAudioIns", mgr_list, IPC_IMPL},
    {1, "OpenAudioIn", mgr_open, IPC_IMPL},
    {2, "ListAudioInsAuto", mgr_list, IPC_IMPL},
    {3, "OpenAudioInAuto", mgr_open, IPC_IMPL},
};
static IpcObject *audin_open(void) { return IPC_OBJECT("audin:u", mgr_cmds, NULL, NULL); }
void service_register_audin(void) { ipc_register_service("audin:u", audin_open); }
