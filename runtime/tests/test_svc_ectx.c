/* Unit tests: ectx:aw remembers the last reported result and never fails. */
#include "../src/services/ectx.c"
#include "svc_test.h"

int main(void) {
    IpcObject *o = ectx_open(); IpcCall c; uint32_t r = 0x1234;
    svc_call(o, 0, &r, 4, &c); CHECK(c.result == 0 && last_result == 0x1234 && reports == 1, "SetResult recorded");
    svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0 && last_result == 0x1234, "SetResult with no argument bytes is tolerated");
    svc_call(o, 1, NULL, 0, &c); CHECK(c.result == 0 && reports == 3, "SetResultWithContext accepted");
    printf("{\"test\":\"svc_ectx\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
