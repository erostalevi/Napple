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
    BtmCore *st = core->state;
    CHECK(st->scanning[SCAN_GENERAL] && !st->scanning[SCAN_PAIRED] && !st->scanning[SCAN_SMART], "the earlier Start left only the general scan running");
    svc_call(core, 8, NULL, 0, &c);
    CHECK(st->scanning[SCAN_GENERAL] && !st->scanning[SCAN_PAIRED] && st->scanning[SCAN_SMART], "Start flags the right scan kinds");
    svc_call(core, 4, NULL, 0, &c); svc_call(core, 7, NULL, 0, &c);
    CHECK(!st->scanning[SCAN_GENERAL] && st->scanning[SCAN_SMART], "Stop clears only its own kind; stopping an idle scan is harmless");
    svc_call(core, 17, NULL, 0, &c); uint32_t ev = c.out_copy[0]; CHECK(c.nout_copy == 1 && !event_is_signaled(ev), "connection event starts clear");
    svc_call(core, 20, NULL, 0, &c); CHECK(event_is_signaled(ev), "BleDisconnect wakes the connection event");
    svc_call(core, 21, NULL, 0, &c); CHECK(c.out[0] == 0, "no connections");
    svc_call(core, 19, NULL, 0, &c); CHECK(c.result != 0, "BleOverrideConnection fails");
    svc_call(core, 10, NULL, 0, &c); CHECK(c.out[0] == 0, "no smart-device results");
    printf("{\"test\":\"svc_btm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
