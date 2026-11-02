/* Unit tests: ectx:aw hands out context registrars, remembers the last reported result and never fails (ids per switchbrew). */
#include "../src/services/ectx.c"
#include "svc_test.h"

int main(void) {
    IpcObject *o = ectx_open(); IpcCall c; uint32_t r = 0x1234;
    svc_call(o, 0, &r, 4, &c); CHECK(c.result == 0 && c.nout_objects == 1 && last_result == 0x1234 && reports == 1 && registrars == 1, "CreateContextRegistrar returns a registrar and records the result");
    IpcObject *reg = c.out_objects[0];
    svc_call(reg, 0, NULL, 0, &c); CHECK(c.result == 0 && completed == 1, "Complete");
    svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0 && c.nout_objects == 1 && last_result == 0x1234, "no argument bytes is tolerated");
    svc_call(o, 1, NULL, 0, &c); CHECK(c.result == 0 && reports == 3, "CommitContext accepted");
    CHECK(svc_find(o, 0)->status == IPC_IMPL && svc_find(o, 1)->status == IPC_IMPL, "both rows are real");
    printf("{\"test\":\"svc_ectx\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
