/* Unit tests: spsm is always awake. */
#include "../src/services/spsm.c"
#include "svc_test.h"

int main(void) {
    IpcObject *o = spsm_open(); IpcCall c;
    svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == SPSM_STATE_AWAKE, "GetState = Awake");
    svc_call(o, 1, NULL, 0, &c); CHECK(c.result == 0 && sleep_requests == 1, "SleepSystemAndWaitAwake returns at once");
    svc_call(o, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == SPSM_STATE_AWAKE, "still awake after a sleep request");
    svc_call(o, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "no notification message");
    printf("{\"test\":\"svc_spsm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
