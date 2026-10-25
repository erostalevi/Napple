/* Unit tests: audio recorder queue core and the audrec:u tables. */
#include "../src/services/audrec.c"
#include "svc_test.h"

int main(void) {
    AudrecQueue q; audrec_init(&q); uint64_t t[4], end = 99;
    CHECK(audrec_append(&q, 1, 400) == 0 && audrec_append(&q, 2, 800) == 0 && audrec_count(&q) == 2 && audrec_contains(&q, 2), "buffers queue while stopped");
    CHECK(audrec_release(&q, t, 4, &end) == 0 && end == 0, "nothing completes while stopped");
    audrec_start(&q);
    CHECK(audrec_end_time(&q, 1, &end) && end == 100 && audrec_end_time(&q, 2, &end) && end == 300, "end times are running frame counts (4 bytes per frame)");
    CHECK(audrec_release(&q, t, 1, &end) == 1 && t[0] == 1 && end == 100 && audrec_count(&q) == 1 && !audrec_contains(&q, 1), "release oldest first");
    CHECK(audrec_append(&q, 3, 40) == 0 && audrec_release(&q, t, 4, &end) == 2 && t[0] == 2 && t[1] == 3 && end == 310, "while started buffers complete at once");
    CHECK(!audrec_end_time(&q, 77, &end), "unknown tag");
    for (int i = 0; i < AUDREC_QUEUE_MAX; i++) CHECK(audrec_append(&q, 100 + i, 4) == 0, "fill %d", i);
    CHECK(audrec_append(&q, 999, 4) == -1, "queue full");

    IpcObject *mgr = audrec_open(); IpcCall c; uint8_t in[16] = {0};
    memset(&c, 0, sizeof c); c.in = in; c.in_size = sizeof in; svc_call_c(mgr, 0, &c);
    CHECK(c.nout_objects == 1 && c.out_size == 16 && SVC_OUT(&c, uint32_t, 0) == 48000 && SVC_OUT(&c, uint32_t, 4) == 2 && SVC_OUT(&c, uint32_t, 8) == 2 && SVC_OUT(&c, uint32_t, 12) == 1, "OpenFinalOutputRecorder: 48 kHz stereo PCM16, stopped");
    IpcObject *r = c.out_objects[0];
    svc_call(r, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1, "state Stopped");
    svc_call(r, 4, NULL, 0, &c); uint32_t ev = c.out_copy[0]; CHECK(!event_is_signaled(ev), "buffer event clear");
    static uint8_t data[64]; memset(data, 0xAA, sizeof data);
    uint64_t rb[5] = {0, (uint64_t)(uintptr_t)data, 64, 48, 8}; uint64_t tag = 0xBEEF;
    memset(&c, 0, sizeof c); c.in = (const uint8_t *)&tag; c.in_size = 8; svc_set_in_buf(&c, rb, sizeof rb); svc_call_c(r, 3, &c);
    CHECK(c.result == 0 && !event_is_signaled(ev), "append while stopped: no event");
    CHECK(data[7] == 0xAA && data[8] == 0 && data[55] == 0 && data[56] == 0xAA, "silence over [offset, offset+size) only");
    svc_call(r, 6, &tag, 8, &c); CHECK(c.out[0] == 1, "ContainsFinalOutputRecorderBuffer");
    svc_call(r, 1, NULL, 0, &c); CHECK(event_is_signaled(ev), "Start completes the buffer and signals the event");
    svc_call(r, 7, &tag, 8, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 12, "end time = 48 bytes / 4");
    uint64_t rel[4] = {0}; memset(&c, 0, sizeof c); svc_set_out_buf(&c, rel, sizeof rel); svc_call_c(r, 5, &c);
    CHECK(SVC_OUT(&c, uint64_t, 0) == 1 && SVC_OUT(&c, uint64_t, 8) == 12 && rel[0] == 0xBEEF && !event_is_signaled(ev), "released: tag, end time, event cleared");
    uint64_t bad = 5; svc_call(r, 7, &bad, 8, &c); CHECK(c.result != 0, "end time of an unknown tag fails");
    svc_call(r, 2, NULL, 0, &c); svc_call(r, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1, "Stop");
    printf("{\"test\":\"svc_audrec\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
