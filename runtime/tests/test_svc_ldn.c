/* Unit tests: ldn state machine and the IUserLocalCommunicationService table. */
#include "../src/services/ldn.c"
#include "svc_test.h"
#include "online_loopback.h"

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
    { /* ---- routed through the OnlineProvider: ldn codec and a loopback network ---- */
        OnlineLdnNetwork n; memset(&n, 0, sizeof n);
        n.local_communication_id = 0x0100abcd12345678ull; n.scene_id = 3; n.channel = 6; n.node_count = 1; n.node_count_max = 8; n.security_mode = 1; n.advertise_len = 5; memcpy(n.advertise, "hello", 5);
        snprintf(n.ssid, sizeof n.ssid, "napple-ssid"); snprintf(n.host_name, sizeof n.host_name, "Player"); memset(n.bssid, 0x11, 6); n.ipv4 = 0xc0a80001; n.security_param[0] = 0x42;
        uint8_t info[LDN_NETINFO_SIZE]; ldn_net_encode(&n, info);
        CHECK(*(uint64_t *)info == n.local_communication_id && info[0x26] == 11 && !memcmp(info + 0x27, "napple-ssid", 11) && info[0x50] == 0x42 && info[0x66] == 8 && info[0x67] == 1 && !memcmp(info + 0x26a, "hello", 5) && *(uint16_t *)(info + 0x268) == 5, "NetworkInfo encoding");
        CHECK(!memcmp(info + 0x68 + 0xc, "Player", 6) && *(uint32_t *)(info + 0x68) == 0xc0a80001 && info[0x68 + 0xb] == 1, "node 0 is the host");
        OnlineLdnNetwork d; CHECK(ldn_net_decode(info, sizeof info, &d) == 0 && d.local_communication_id == n.local_communication_id && !strcmp(d.ssid, "napple-ssid") && d.advertise_len == 5 && d.node_count_max == 8 && d.channel == 6 && !memcmp(d.bssid, n.bssid, 6), "decode returns the identifying fields");
        CHECK(ldn_net_decode(info, 0x100, &d) == -1, "short NetworkInfo rejected");
        uint8_t cr[0x98] = {0}; cr[0] = 2; memcpy(cr + 0x44, "Host", 4); uint64_t lc = 0x0100abcd12345678ull; memcpy(cr + 0x78, &lc, 8); cr[0x78 + 0x10] = 11; cr[0x78 + 0x12] = 4;
        OnlineLdnNetwork cd; ldn_create_decode(cr, sizeof cr, &cd); CHECK(cd.security_mode == 2 && !strcmp(cd.host_name, "Host") && cd.local_communication_id == lc && cd.channel == 11 && cd.node_count_max == 4, "CreateNetwork arguments decode");
        ldn_create_decode(cr, 4, &cd); CHECK(cd.security_mode == 2 && cd.local_communication_id == 0, "short CreateNetwork arguments leave zeros");
        { LdnState ls = LDN_STATION_OPENED; CHECK(ldn_connect_done(&ls, 0) == 0 && ls == LDN_STATION_CONNECTED, "connect success moves to Connected");
          ls = LDN_STATION_OPENED; CHECK(ldn_connect_done(&ls, 77) == 77 && ls == LDN_STATION_OPENED, "connect failure keeps the state");
          ls = LDN_INITIALIZED; CHECK(ldn_connect_done(&ls, 0) != 0, "connect outside a station is refused"); }

        memset(&lb, 0, sizeof lb); online_provider_install(&lb_provider);
        IpcObject *root = ldn_open(); IpcCall c;
        svc_call(root, 0, NULL, 0, &c); IpcObject *host = c.out_objects[0]; svc_call(root, 0, NULL, 0, &c); IpcObject *sta = c.out_objects[0];
        svc_call(host, 400, NULL, 0, &c); svc_call(sta, 400, NULL, 0, &c);
        svc_call(host, 200, NULL, 0, &c);
        svc_call(host, 1, NULL, 0, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "no network info before the network exists");
        svc_call(host, 202, cr, sizeof cr, &c); CHECK(c.result == 0 && lb.ldn_hosting && *st(host) == LDN_AP_CREATED, "CreateNetwork starts hosting at the provider");
        { IpcObject *h2 = NULL; svc_call(root, 0, NULL, 0, &c); h2 = c.out_objects[0]; svc_call(h2, 400, NULL, 0, &c); svc_call(h2, 200, NULL, 0, &c);
          svc_call(h2, 202, cr, sizeof cr, &c); CHECK(c.result != 0 && *st(h2) == LDN_AP_OPENED, "provider refusal leaves the state"); }
        uint8_t ni[LDN_NETINFO_SIZE]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, ni, sizeof ni); svc_call_c(host, 1, &c);
        CHECK(c.result == 0 && ni[0x26] == 12 && !memcmp(ni + 0x27, "loopback-ldn", 12) && ni[0x67] == 1, "GetNetworkInfo while hosting");
        svc_call(host, 2, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0xc0a80001, "GetIpv4Address = the provider's address");
        svc_call(host, 4, NULL, 0, &c); CHECK(c.result == 0 && c.out[0] == 0x5a && c.out_size == 16, "GetSecurityParameter");
        svc_call(host, 5, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 0x20 && SVC_OUT(&c, uint64_t, 0) == lc && c.out[0x12] == 4, "GetNetworkConfig carries the creation config");
        uint8_t adv[10] = "advertise"; memset(&c, 0, sizeof c); svc_set_in_buf(&c, adv, 10); svc_call_c(host, 206, &c);
        CHECK(c.result == 0 && lb.ldn_adv_updates == 1 && lb.ldn_net.advertise_len == 10 && !memcmp(lb.ldn_net.advertise, "advertise", 9), "SetAdvertiseData reaches the provider");

        svc_call(sta, 300, NULL, 0, &c);
        { uint8_t list[LDN_NETINFO_SIZE * 2]; uint8_t sf[0x68] = {0}; memcpy(sf + 8, &lc, 8); memset(list, 0, sizeof list); memset(&c, 0, sizeof c); c.in = sf; c.in_size = sizeof sf; svc_set_out_buf(&c, list, sizeof list); svc_call_c(sta, 102, &c);
          CHECK(c.result == 0 && SVC_OUT(&c, int16_t, 0) == 1 && !memcmp(list + 0x27, "loopback-ldn", 12) && !memcmp(list + 0x26a, "advertise", 9), "Scan returns the hosted network with its advertise data");
          uint64_t other = lc + 1; memcpy(sf + 8, &other, 8); memset(&c, 0, sizeof c); c.in = sf; c.in_size = sizeof sf; svc_set_out_buf(&c, list, sizeof list); svc_call_c(sta, 102, &c);
          CHECK(c.result == 0 && SVC_OUT(&c, int16_t, 0) == 0, "Scan for another game's id finds nothing");
          memset(&c, 0, sizeof c); svc_call_c(sta, 102, &c); CHECK(c.result == 0 && c.out_size == 2 && SVC_OUT(&c, int16_t, 0) == 0, "Scan without an output buffer is a count of 0");
          memcpy(sf + 8, &lc, 8); memset(&c, 0, sizeof c); c.in = sf; c.in_size = sizeof sf; svc_set_out_buf(&c, list, sizeof list); svc_call_c(sta, 102, &c);
          uint8_t bad[LDN_NETINFO_SIZE]; memcpy(bad, list, sizeof bad); bad[0x27] = 'X';
          memset(&c, 0, sizeof c); svc_set_in_buf(&c, bad, sizeof bad); svc_call_c(sta, 302, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE && *st(sta) == LDN_STATION_OPENED && !lb.ldn_joined, "Connect to a network the provider does not know fails, state stays");
          memset(&c, 0, sizeof c); svc_call_c(sta, 302, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE && *st(sta) == LDN_STATION_OPENED, "Connect without a NetworkInfo buffer fails");
          memset(&c, 0, sizeof c); svc_set_in_buf(&c, list, LDN_NETINFO_SIZE); svc_call_c(sta, 302, &c); CHECK(c.result == 0 && lb.ldn_joined && *st(sta) == LDN_STATION_CONNECTED, "Connect joins through the provider"); }
        svc_call(sta, 2, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0xc0a80001, "the station reads the address through the provider");
        svc_call(host, 205, (uint32_t[]){0xc0a80002}, 4, &c); CHECK(c.result == 0 && lb.ldn_rejected_ip == 0xc0a80002 && !lb.ldn_joined, "Reject reaches the provider");
        svc_call(host, 205, (uint32_t[]){0x01020304}, 4, &c); CHECK(c.result == HZ_RESULT(HZ_MOD_LDN, 24), "Reject of an unknown station: the provider's result");
        lb.ldn_joined = 1; svc_call(sta, 304, NULL, 0, &c); CHECK(c.result == 0 && !lb.ldn_joined && *st(sta) == LDN_STATION_OPENED, "Disconnect leaves the provider network");
        svc_call(sta, 3, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint16_t, 0) == 7, "GetDisconnectReason = the provider's reason");
        svc_call(host, 204, NULL, 0, &c); CHECK(c.result == 0 && !lb.ldn_hosting && *st(host) == LDN_AP_OPENED, "DestroyNetwork stops hosting");
        svc_call(host, 1, NULL, 0, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "no network after DestroyNetwork");
        svc_call(host, 202, cr, sizeof cr, &c); CHECK(lb.ldn_hosting, "host again"); svc_call(host, 201, NULL, 0, &c); CHECK(c.result == 0 && !lb.ldn_hosting && *st(host) == LDN_INITIALIZED, "CloseAccessPoint also stops hosting");
        online_provider_install(NULL);
        svc_call(host, 200, NULL, 0, &c); svc_call(host, 202, cr, sizeof cr, &c); CHECK(c.result == 0 && *st(host) == LDN_AP_CREATED, "NullProvider: CreateNetwork keeps its offline answer");
        svc_call(host, 1, NULL, 0, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "NullProvider: no network info");
    }
    printf("{\"test\":\"svc_ldn\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
