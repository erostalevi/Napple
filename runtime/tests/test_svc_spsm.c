/* Unit tests: spsm is always awake. */
#include "../src/services/spsm.c"
#include "svc_test.h"

int main(void) {
    IpcObject *o = spsm_open(); IpcCall c;
    svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == SPSM_STATE_AWAKE, "GetState = Awake");
    svc_call(o, 1, NULL, 0, &c); CHECK(c.result == 0 && sleep_requests == 1, "SleepSystemAndWaitAwake returns at once");
    svc_call(o, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == SPSM_STATE_AWAKE, "still awake after a sleep request");
    svc_call(o, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "no wake reason (2 GetLastWakeReason)");
    svc_call(o, 4, NULL, 0, &c); CHECK(c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "notification event never fires");
    svc_call(o, 5, NULL, 0, &c); CHECK(c.out_size == 4 && SVC_OUT(&c, uint32_t, 0) == 0, "ReceiveNotificationMessage: none");
    printf("{\"test\":\"svc_spsm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
