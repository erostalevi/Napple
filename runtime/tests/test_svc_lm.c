/* Unit tests: lm / ILogger rows 2 (TransmitHashedLog) and 3 (DevNotify) now go to the runtime log like Log does. lm.c is #included. */
#include "../src/services/lm.c"
#include "svc_test.h"

int main(void) {
    IpcCall c;
    IpcObject *l = lm_open();
    svc_call(l, 0, NULL, 0, &c);
    CHECK(c.result == 0 && c.nout_objects == 1, "OpenLogger");
    IpcObject *lg = c.out_objects[0];
    uint8_t packet[64] = {0};
    packet[18] = 3; /* severity */
    for (unsigned cmd = 2; cmd <= 3; cmd++) {
        memset(&c, 0, sizeof c);
        svc_set_in_buf(&c, packet, sizeof packet);
        svc_call_c(lg, cmd, &c);
        CHECK(c.result == 0, "ILogger %u accepts a packet", cmd);
        memset(&c, 0, sizeof c);
        svc_set_in_buf(&c, packet, 8);
        svc_call_c(lg, cmd, &c);
        CHECK(c.result == 0, "ILogger %u ignores a truncated packet", cmd);
        svc_call(lg, cmd, NULL, 0, &c);
        CHECK(c.result == 0, "ILogger %u without a buffer is harmless", cmd);
        CHECK(svc_find(lg, cmd)->status == IPC_IMPL, "ILogger %u is real now", cmd);
    }
    CHECK(svc_find(lg, 11)->status == IPC_STUB, "11 OpenSharedMemoryAccessorForHashedLog stays a documented default");
    printf("{\"test\":\"svc_lm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
