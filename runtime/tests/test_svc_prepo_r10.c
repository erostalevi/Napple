/* Unit tests: prepo ReadAllApplicationReportFiles / ReadAllReportFiles read back the reports SaveReport* / SaveSystemReport* kept (ipc-depth-95). */
#include "../src/services/offline.c"
#include "svc_test.h"

static IpcCall k;
static uint32_t save(IpcObject *o, unsigned cmd, const void *p, uint64_t n) {
    static uint8_t ev[0x20];
    memset(&k, 0, sizeof k);
    k.in = ev, k.in_size = 8;
    svc_set_in_buf(&k, ev, sizeof ev); /* buffer 0: the event id; buffer 1: the report */
    if (cmd >= 20100) k.na = 0;        /* system reports carry the report in buffer 0 */
    svc_set_in_buf(&k, p, n);
    svc_call_c(o, cmd, &k);
    return k.result;
}
static uint32_t read_all(IpcObject *o, unsigned cmd, uint8_t *out, uint64_t n) {
    memset(&k, 0, sizeof k);
    svc_set_out_buf(&k, out, n);
    svc_call_c(o, cmd, &k);
    return k.result ? 0xffffffffu : SVC_OUT(&k, uint32_t, 0);
}

int main(void) {
    IpcCall c;
    IpcObject *pr = prepo_open();
    svc_call(pr, 30100, NULL, 0, &c);
    uint8_t out[256];
    CHECK(read_all(pr, 90100, out, sizeof out) == 0 && read_all(pr, 50100, out, sizeof out) == 0, "nothing kept yet");
    g_program_id = 0x0100000000010000ull;
    CHECK(save(pr, 10106, "app-report", 10) == 0 && save(pr, 20102, "sys", 3) == 0 && save(pr, 10107, "second", 6) == 0, "three reports saved");
    memset(out, 0xcc, sizeof out);
    CHECK(read_all(pr, 90100, out, sizeof out) == 3, "ReadAllReportFiles: every report");
    CHECK(*(uint32_t *)out == 32 && *(uint32_t *)(out + 4) == 10 && *(uint64_t *)(out + 8) == 0x0100000000010000ull && !memcmp(out + 16, "app-report", 10) &&
              out[26] == 0, "record 1: size, payload size, application id, payload, padding");
    CHECK(*(uint32_t *)(out + 32) == 24 && *(uint32_t *)(out + 36) == 3 && !memcmp(out + 48, "sys", 3), "record 2: the system report");
    CHECK(read_all(pr, 50100, out, sizeof out) == 2 && !memcmp(out + 16, "app-report", 10) && !memcmp(out + 32 + 16, "second", 6),
          "ReadAllApplicationReportFiles leaves the system report out");
    CHECK(read_all(pr, 90100, out, 40) == 1, "a report that does not fit ends the list");
    svc_call(pr, 30300, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint64_t, 0) == 19, "storage usage counts the kept bytes");
    svc_call(pr, 30100, NULL, 0, &c);
    CHECK(read_all(pr, 90100, out, sizeof out) == 0, "ClearStorage drops the kept reports");
    printf("{\"test\":\"svc_prepo_r10\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
