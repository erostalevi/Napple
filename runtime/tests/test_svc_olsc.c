/* Unit tests: olsc is reachable but offline; ids follow switchbrew (ipc-reference-audit). */
#include "../src/services/olsc.c"
#include "svc_test.h"

int main(void) {
    IpcObject *o = olsc_open(); IpcCall c;
    svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0, "Initialize");
    svc_call(o, 10, NULL, 0, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "10 VerifySaveDataBackupLicenseAsync needs the network");
    for (unsigned id = 13; id <= 16; id++) { svc_call(o, id, NULL, 0, &c); CHECK(c.result == 0, "%u: the device-side backup setting call succeeds (Animal Crossing ends the process on an error)", id); }
    CHECK(svc_find(o, 1) == NULL && svc_find(o, 11) == NULL && svc_find(o, 12) == NULL, "the old guessed rows (1-9, 11, 12) are gone");
    /* the system-side controllers keep the switchbrew ids */
    IpcCall d; memset(&d, 0, sizeof d); make_tasks(NULL, &d); IpcObject *t = d.out_objects[0];
    svc_call(t, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "no transfer tasks (GetTransferTaskCountForOcean)");
    svc_call(t, 16, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "no transfer tasks (GetTransferTaskCount)");
    svc_call(t, 5, NULL, 0, &c); CHECK(c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "the end event never fires");
    memset(&d, 0, sizeof d); make_remote(NULL, &d); IpcObject *r = d.out_objects[0];
    svc_call(r, 1, NULL, 0, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "RegisterDownloadSaveDataTransferTask needs the network");
    svc_call(r, 3, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "GetCount: 0");
    printf("{\"test\":\"svc_olsc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
