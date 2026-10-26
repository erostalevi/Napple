/* Unit tests: olsc is reachable but offline. */
#include "../src/services/olsc.c"
#include "svc_test.h"

int main(void) {
    IpcObject *o = olsc_open(); IpcCall c;
    svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0, "Initialize");
    svc_call(o, 10, NULL, 0, &c); CHECK(c.nout_objects == 1, "transfer task list controller");
    IpcObject *t = c.out_objects[0];
    svc_call(t, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "no transfer tasks");
    svc_call(o, 11, NULL, 0, &c); IpcObject *r = c.out_objects[0];
    svc_call(r, 1, NULL, 0, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "synchronising needs the network");
    svc_call(r, 2, NULL, 0, &c); CHECK(c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "completion event never fires");
    svc_call(o, 12, NULL, 0, &c); CHECK(c.result == 0, "policy info answers success with zeroed output (backup off)");
    svc_call(o, 13, NULL, 0, &c); CHECK(c.result == 0, "DeleteUserSaveDataInSyncArea succeeds: Animal Crossing ends the process on an error");
    printf("{\"test\":\"svc_olsc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
