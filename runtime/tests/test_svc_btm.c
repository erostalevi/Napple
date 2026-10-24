/* Unit tests: btm:u scans find nothing, connects fail. */
#include "../src/services/btm.c"
#include "svc_test.h"

int main(void) {
    IpcObject *o = btm_open(); IpcCall c;
    svc_call(o, 0, NULL, 0, &c); CHECK(c.nout_objects == 1, "GetCore");
    IpcObject *core = c.out_objects[0];
    svc_call(core, 0, NULL, 0, &c); CHECK(c.out_size == 1 && c.out[0] == 1 && c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "AcquireBleScanEvent: success flag + unsignalled event");
    svc_call(core, 3, NULL, 0, &c); CHECK(c.result == 0, "StartBleScanForGeneral");
    svc_call(core, 5, NULL, 0, &c); CHECK(c.out[0] == 0, "no scan results");
    svc_call(core, 18, NULL, 0, &c); CHECK(c.result != 0, "BleConnect fails");
    svc_call(core, 20, NULL, 0, &c); CHECK(c.result == 0, "BleDisconnect harmless");
    printf("{\"test\":\"svc_btm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
