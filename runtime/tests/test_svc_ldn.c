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
    LdnConfig cfg; memset(&cfg, 0, sizeof cfg); uint8_t mac[6] = {1, 2, 3, 4, 5, 6}, adv[8] = {9};
    CHECK(ldn_set_advertise(LDN_INITIALIZED, &cfg, adv, 8) != 0, "SetAdvertiseData needs a created network");
    CHECK(ldn_set_advertise(LDN_AP_CREATED, &cfg, adv, 8) == 0 && cfg.advertise_len == 8 && cfg.advertise[0] == 9, "SetAdvertiseData stores the data");
    CHECK(ldn_set_advertise(LDN_AP_CREATED, &cfg, adv, LDN_ADVERTISE_MAX + 1) != 0 && cfg.advertise_len == 8, "oversized advertise data is refused, old data kept");
    CHECK(ldn_set_accept_policy(LDN_AP_CREATED, &cfg, 2) == 0 && cfg.accept_policy == 2 && ldn_set_accept_policy(LDN_AP_CREATED, &cfg, 9) != 0 && cfg.accept_policy == 2, "accept policy range");
    CHECK(ldn_filter_add(LDN_AP_CREATED, &cfg, mac) == 0 && ldn_filter_add(LDN_AP_CREATED, &cfg, mac) == 0 && cfg.nfilter == 1, "filter entries are unique");
    for (int i = 0; i < LDN_FILTER_MAX; i++) { mac[5] = (uint8_t)(10 + i); ldn_filter_add(LDN_AP_CREATED, &cfg, mac); }
    CHECK(cfg.nfilter == LDN_FILTER_MAX && ldn_filter_add(LDN_AP_CREATED, &cfg, (const uint8_t[6]){7, 7, 7, 7, 7, 7}) != 0, "filter table is bounded");
    CHECK(ldn_filter_clear(LDN_AP_CREATED, &cfg) == 0 && cfg.nfilter == 0 && ldn_filter_clear(LDN_NONE, &cfg) != 0, "ClearAcceptFilter");
    CHECK(ldn_reject(LDN_AP_CREATED) != 0 && ldn_reject(LDN_INITIALIZED) != 0 && ldn_reject(LDN_AP_CREATED) != ldn_reject(LDN_INITIALIZED), "Reject: no node vs wrong state");
    {
        IpcObject *r2 = ldn_open(); svc_call(r2, 0, NULL, 0, &c); IpcObject *d = c.out_objects[0];
        uint32_t pol = 1;
        svc_call(d, 207, &pol, 4, &c); CHECK(c.result != 0, "SetStationAcceptPolicy before CreateNetwork is refused");
        svc_call(d, 400, NULL, 0, &c); svc_call(d, 200, NULL, 0, &c); svc_call(d, 202, NULL, 0, &c);
        svc_call(d, 207, &pol, 4, &c); CHECK(c.result == 0 && ((Ldn *)d->state)->cfg.accept_policy == 1, "SetStationAcceptPolicy once created");
        svc_call(d, 208, mac, 6, &c); CHECK(c.result == 0 && ((Ldn *)d->state)->cfg.nfilter == 1, "AddAcceptFilterEntry");
        svc_call(d, 209, NULL, 0, &c); CHECK(((Ldn *)d->state)->cfg.nfilter == 0, "ClearAcceptFilter via IPC");
        uint32_t rs = 2; svc_call(d, 104, &rs, 4, &c); CHECK(((Ldn *)d->state)->cfg.controller_restriction == 2, "SetWirelessControllerRestriction stored");
        svc_call(d, 4, NULL, 0, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "GetSecurityParameter: not available");
        svc_call(d, 5, NULL, 0, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "GetNetworkConfig: not available");
        svc_call(d, 101, NULL, 0, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "GetNetworkInfoLatestUpdate: not available");
        svc_call(d, 3, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint16_t, 0) == 0, "GetDisconnectReason = none");
        svc_call(d, 205, &pol, 4, &c); CHECK(c.result != 0, "Reject: no stations to reject");
    }
    printf("{\"test\":\"svc_ldn\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
