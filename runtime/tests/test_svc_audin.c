/* Unit tests: audio-in queue core and the audin:u tables. */
#include "../src/services/audin.c"
#include "svc_test.h"

int main(void) {
    uint32_t r, ch;
    audin_negotiate(0, 0, &r, &ch); CHECK(r == 48000 && ch == 2, "defaults");
    audin_negotiate(44100, 1, &r, &ch); CHECK(r == 48000 && ch == 1, "44.1 kHz snaps to 48 kHz");
    audin_negotiate(16000, 2, &r, &ch); CHECK(r == 16000, "16 kHz kept");
    audin_negotiate(20000, 9, &r, &ch); CHECK(r == 16000 && ch == 2, "nearest rate, channels clamped");

    AudinQueue q; audin_init(&q);
    CHECK(audin_append(&q, 1) == 0 && audin_append(&q, 2) == 0 && audin_count(&q) == 2 && audin_contains(&q, 2), "buffers queue while stopped");
    uint64_t t[4]; CHECK(audin_release(&q, t, 4) == 0, "nothing completes while stopped");
    audin_start(&q); CHECK(audin_release(&q, t, 1) == 1 && t[0] == 1, "start completes queued buffers in order");
    CHECK(audin_count(&q) == 1 && audin_contains(&q, 2) && !audin_contains(&q, 1), "one left to fetch");
    CHECK(audin_append(&q, 3) == 0 && audin_release(&q, t, 4) == 2 && t[0] == 2 && t[1] == 3, "while started, appended buffers complete at once");
    for (int i = 0; i < AUDIN_QUEUE_MAX; i++) CHECK(audin_append(&q, 100 + i) == 0, "fill %d", i);
    CHECK(audin_append(&q, 999) == -1, "queue full");
    audin_init(&q); audin_append(&q, 7); CHECK(audin_flush(&q) == 1 && audin_flush(&q) == 0 && audin_release(&q, t, 4) == 1, "flush releases queued buffers");

    /* IPC */
    IpcObject *mgr = audin_open(); IpcCall c; char names[0x40] = {0};
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, names, sizeof names); svc_call_c(mgr, 0, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 1 && !strcmp(names, "BuiltInHeadset"), "ListAudioIns: one device");
    uint8_t in[16] = {0}; uint32_t rate = 16000; uint16_t chn = 1; memcpy(in, &rate, 4); memcpy(in + 4, &chn, 2);
    char nin[0x20] = "BuiltInHeadset", nout[0x20] = {0};
    memset(&c, 0, sizeof c); c.in = in; c.in_size = sizeof in; svc_set_in_buf(&c, nin, sizeof nin); svc_set_out_buf(&c, nout, sizeof nout); svc_call_c(mgr, 1, &c);
    CHECK(c.nout_objects == 1 && c.out_size == 16 && SVC_OUT(&c, uint32_t, 0) == 16000 && SVC_OUT(&c, uint32_t, 4) == 1 && SVC_OUT(&c, uint32_t, 8) == 2 && SVC_OUT(&c, uint32_t, 12) == 1, "OpenAudioIn: negotiated parameters, format PCM16, stopped");
    CHECK(!strcmp(nout, "BuiltInHeadset"), "device name echoed");
    IpcObject *ai = c.out_objects[0];
    svc_call(ai, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1, "state Stopped");
    svc_call(ai, 4, NULL, 0, &c); uint32_t ev = c.out_copy[0]; CHECK(!event_is_signaled(ev), "buffer event clear");
    static uint8_t data[64]; memset(data, 0xAA, sizeof data);
    uint64_t ab[5] = {0, (uint64_t)(uintptr_t)data, 64, 48, 8};          /* next, ptr, capacity, size, offset */
    uint8_t in2[8]; uint64_t tag = 0xBEEF; memcpy(in2, &tag, 8);
    memset(&c, 0, sizeof c); c.in = in2; c.in_size = 8; svc_set_in_buf(&c, ab, sizeof ab); svc_call_c(ai, 3, &c);
    CHECK(c.result == 0 && !event_is_signaled(ev), "append while stopped: no event");
    CHECK(data[7] == 0xAA && data[8] == 0 && data[55] == 0 && data[56] == 0xAA, "silence is written over [offset, offset+size) only");
    svc_call(ai, 6, in2, 8, &c); CHECK(c.out[0] == 1, "ContainsAudioInBuffer");
    svc_call(ai, 1, NULL, 0, &c); CHECK(event_is_signaled(ev), "Start completes the buffer and signals the event");
    uint64_t rel[4] = {0}; memset(&c, 0, sizeof c); svc_set_out_buf(&c, rel, sizeof rel); svc_call_c(ai, 5, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 1 && rel[0] == 0xBEEF && !event_is_signaled(ev), "GetReleasedAudioInBuffer returns the tag and clears the event");
    svc_call(ai, 9, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "buffer count back to 0");
    printf("{\"test\":\"svc_audin\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
