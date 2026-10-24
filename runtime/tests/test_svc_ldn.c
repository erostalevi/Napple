/* Unit tests: ldn state machine and the IUserLocalCommunicationService table. */
#include "../src/services/ldn.c"
#include "svc_test.h"

int main(void) {
    LdnState s = LDN_NONE;
    CHECK(ldn_apply(&s, LDN_OP_OPEN_AP) != 0 && s == LDN_NONE, "opening an access point before Initialize is refused, state unchanged");
    CHECK(ldn_apply(&s, LDN_OP_INITIALIZE) == 0 && s == LDN_INITIALIZED, "initialize");
    CHECK(ldn_apply(&s, LDN_OP_INITIALIZE) == 0 && s == LDN_INITIALIZED, "initialize twice is harmless");
    CHECK(ldn_apply(&s, LDN_OP_CREATE_NETWORK) != 0, "create before open is refused");
    CHECK(ldn_apply(&s, LDN_OP_OPEN_AP) == 0 && s == LDN_AP_OPENED && ldn_apply(&s, LDN_OP_CREATE_NETWORK) == 0 && s == LDN_AP_CREATED, "open then create");
    CHECK(ldn_apply(&s, LDN_OP_OPEN_STATION) != 0, "no station while hosting");
    CHECK(ldn_apply(&s, LDN_OP_DESTROY_NETWORK) == 0 && s == LDN_AP_OPENED && ldn_apply(&s, LDN_OP_CLOSE_AP) == 0 && s == LDN_INITIALIZED, "destroy and close");
    CHECK(ldn_apply(&s, LDN_OP_OPEN_STATION) == 0 && s == LDN_STATION_OPENED, "open station");
    CHECK(ldn_apply(&s, LDN_OP_SCAN) == 0, "scan is allowed from a station");
    CHECK(ldn_apply(&s, LDN_OP_CONNECT) == HZ_R_LDN_NETWORK_NOT_AVAILABLE && s == LDN_STATION_OPENED, "connect fails: no network, state stays");
    CHECK(ldn_apply(&s, LDN_OP_DISCONNECT) != 0, "disconnect without a connection is refused");
    CHECK(ldn_apply(&s, LDN_OP_CLOSE_STATION) == 0 && s == LDN_INITIALIZED, "close station");
    CHECK(ldn_apply(&s, LDN_OP_FINALIZE) == 0 && s == LDN_NONE, "finalize from anywhere");

    IpcObject *root = ldn_open(); IpcCall c;
    svc_call(root, 0, NULL, 0, &c); CHECK(c.nout_objects == 1, "CreateUserLocalCommunicationService");
    IpcObject *a = c.out_objects[0];
    svc_call(root, 0, NULL, 0, &c); IpcObject *b = c.out_objects[0];
    svc_call(a, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == LDN_NONE, "fresh object: state None");
    svc_call(a, 100, NULL, 0, &c); uint32_t ev = c.out_copy[0]; CHECK(!event_is_signaled(ev), "state-change event starts clear");
    svc_call(a, 400, NULL, 0, &c); CHECK(c.result == 0 && event_is_signaled(ev), "Initialize signals the state-change event");
    svc_call(a, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == LDN_INITIALIZED, "GetState = Initialized");
    svc_call(b, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == LDN_NONE, "state is per object");
    svc_call(a, 102, NULL, 0, &c); CHECK(c.result != 0, "Scan from Initialized is refused");
    svc_call(a, 300, NULL, 0, &c); svc_call(a, 102, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 2 && SVC_OUT(&c, int16_t, 0) == 0, "Scan finds no networks");
    svc_call(a, 302, NULL, 0, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "Connect fails");
    svc_call(a, 1, NULL, 0, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "GetNetworkInfo: no network");
    svc_call(a, 401, NULL, 0, &c); svc_call(a, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == LDN_NONE, "Finalize");
    printf("{\"test\":\"svc_ldn\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
