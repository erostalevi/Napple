/* Unit tests: ovln:snd counts notifications and keeps the id. */
#include "../src/services/ovln.c"
#include "svc_test.h"

int main(void) {
    IpcObject *o = ovln_open(); IpcCall c;
    svc_call(o, 0, NULL, 0, &c); CHECK(c.nout_objects == 1, "OpenSender");
    IpcObject *snd = c.out_objects[0]; uint8_t msg[0x80]; memset(msg, 0, sizeof msg); memcpy(msg, "NOTIFYID01234567", 16);
    svc_call(snd, 0, msg, sizeof msg, &c); CHECK(c.result == 0 && sent == 1 && !memcmp(last_id, "NOTIFYID01234567", 16), "Send counted, id kept");
    svc_call(snd, 0, msg, 8, &c); CHECK(c.result == 0 && sent == 2 && !memcmp(last_id, "NOTIFYID01234567", 16), "short message: counted, id unchanged");
    printf("{\"test\":\"svc_ovln\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
