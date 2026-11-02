/* Host provider LDN over the LAN (NAPPLE_NET=host): two (three) provider instances in one process stand for consoles on one network. A host creates a
 * network, a station discovers it, joins, both exchange a UDP packet through the provider's sockets at the node addresses the network reports, the
 * advertise data reaches the station, Reject and DestroyNetwork end the station's connection with their reasons. Then the same through ldn:u. */
#include "../src/services/ldn.c"
#include "svc_test.h"
#include "../src/net/net_host.h"
#include <unistd.h>

#define LCID 0x0100abcd00010000ull
static int notes_a, notes_b;
static void note(void *arg) { __atomic_add_fetch((int *)arg, 1, __ATOMIC_SEQ_CST); }
static int wait_for(int *ctr, int at_least) {
    for (int i = 0; i < 200; i++) {
        if (__atomic_load_n(ctr, __ATOMIC_SEQ_CST) >= at_least) return 1;
        usleep(10000);
    }
    return 0;
}
static OnlineLdnNetwork cfg_of(uint8_t max, uint16_t version) {
    OnlineLdnNetwork c;
    memset(&c, 0, sizeof c);
    c.local_communication_id = LCID;
    c.scene_id = 7;
    c.node_count_max = max;
    c.local_communication_version = version;
    strcpy(c.host_name, "napple-host");
    return c;
}
static int is_gone(const OnlineProvider *p, uint16_t *reason) {
    OnlineLdnNetwork n;
    for (int i = 0; i < 200; i++) {
        if (p->ldn_get_network(p->ctx, &n, reason) != 0) return 1;
        usleep(10000);
    }
    return 0;
}

int main(void) {
    char port[16];
    snprintf(port, sizeof port, "%d", 30000 + (int)(getpid() % 20000)); /* away from the default port and from other runs */
    setenv("NAPPLE_LDN_PORT", port, 1);
    OnlineProvider *A = net_host_instance_new(), *B = net_host_instance_new(), *C = net_host_instance_new();
    CHECK(A && B && C, "three provider instances");
    A->ldn_set_notify(A->ctx, note, &notes_a);
    B->ldn_set_notify(B->ctx, note, &notes_b);

    OnlineLdnNetwork nets[ONLINE_LDN_MAX_NETWORKS], cur;
    uint16_t reason = 99;
    CHECK(B->ldn_scan(B->ctx, LCID, nets, ONLINE_LDN_MAX_NETWORKS) == 0, "nothing hosted: a scan finds no network");
    CHECK(A->ldn_get_network(A->ctx, &cur, &reason) != 0, "no network before CreateNetwork");
    OnlineLdnNetwork cfg = cfg_of(4, 3);
    CHECK(A->ldn_create(A->ctx, &cfg) == 0, "A hosts a network");
    CHECK(A->ldn_get_network(A->ctx, &cur, &reason) == 0 && cur.node_count == 1 && cur.ssid[0] && cur.ipv4 && cur.channel && reason == 0, "A: one node, an ssid, its own address");
    int n = B->ldn_scan(B->ctx, LCID, nets, ONLINE_LDN_MAX_NETWORKS);
    CHECK(n == 1 && nets[0].local_communication_id == LCID && !strcmp(nets[0].ssid, cur.ssid) && !memcmp(nets[0].bssid, cur.bssid, 6) &&
              !strcmp(nets[0].host_name, "napple-host") && nets[0].node_count == 1 && nets[0].node_count_max == 4 && nets[0].scene_id == 7,
          "B discovers it: one network with the host's ssid, bssid, name and limits");
    CHECK(B->ldn_scan(B->ctx, LCID + 1, nets + 1, 4) == 0, "a scan for another title does not see it");

    /* join */
    OnlineLdnNetwork wrongv = nets[0];
    wrongv.local_communication_version = 4;
    CHECK(C->ldn_connect(C->ctx, &wrongv) == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "a station of another version is rejected");
    CHECK(B->ldn_connect(B->ctx, &nets[0]) == 0, "B joins");
    CHECK(wait_for(&notes_a, 1), "A is told that a station joined");
    OnlineLdnNode an[8], bn[8];
    uint32_t aself = 9, bself = 9;
    CHECK(A->ldn_get_nodes(A->ctx, an, 8, &aself) == 8 && aself == 0 && an[0].connected && an[1].connected && !an[2].connected, "A: nodes 0 and 1");
    CHECK(B->ldn_get_nodes(B->ctx, bn, 8, &bself) == 8 && bself == 1 && bn[0].connected && bn[1].connected && bn[0].ipv4 && bn[1].ipv4 == an[1].ipv4,
          "B: the same nodes, itself node 1 at the address A sees");
    CHECK(B->ldn_get_network(B->ctx, &cur, &reason) == 0 && cur.node_count == 2 && cur.ipv4 == bn[1].ipv4, "B: two nodes, its own address");
    CHECK(A->ldn_get_network(A->ctx, &cur, &reason) == 0 && cur.node_count == 2, "A: two nodes");

    /* the title's own traffic: UDP between the node addresses, through the provider's bsd sockets */
    int err, ua = A->sock_open(A->ctx, 2, 2, 0, &err), ub = B->sock_open(B->ctx, 2, 2, 0, &err);
    OnlineAddr any = {.family = 2}, abound;
    A->sock_bind(A->ctx, ua, &any, &err);
    A->sock_getsockname(A->ctx, ua, &abound, &err);
    OnlineAddr to = {.family = 2, .port = abound.port};
    to.addr[0] = (uint8_t)(bn[0].ipv4 >> 24), to.addr[1] = (uint8_t)(bn[0].ipv4 >> 16), to.addr[2] = (uint8_t)(bn[0].ipv4 >> 8), to.addr[3] = (uint8_t)bn[0].ipv4;
    CHECK(B->sock_send(B->ctx, ub, "hello ldn", 9, 0, &to, &err) == 9, "B sends a packet to node 0's address (%u.%u.%u.%u:%u)", to.addr[0], to.addr[1], to.addr[2], to.addr[3], to.port);
    int64_t tv[2] = {2, 0};
    A->sock_setsockopt(A->ctx, ua, 0xffff, 0x1006, tv, 16, &err);
    B->sock_setsockopt(B->ctx, ub, 0xffff, 0x1006, tv, 16, &err);
    char buf[32] = {0};
    OnlineAddr from;
    CHECK(A->sock_recv(A->ctx, ua, buf, sizeof buf, 0, &from, &err) == 9 && !memcmp(buf, "hello ldn", 9), "A receives it");
    CHECK(A->sock_send(A->ctx, ua, "ack", 3, 0, &from, &err) == 3 && B->sock_recv(B->ctx, ub, buf, sizeof buf, 0, NULL, &err) == 3 && !memcmp(buf, "ack", 3),
          "A answers, B receives");
    A->sock_close(A->ctx, ua, &err), B->sock_close(B->ctx, ub, &err);

    /* advertise data reaches the station */
    int before = notes_b;
    CHECK(A->ldn_set_advertise(A->ctx, "ADVERTISED", 10) == 0, "A replaces its advertise data");
    CHECK(wait_for(&notes_b, before + 1), "B is told");
    CHECK(B->ldn_get_network(B->ctx, &cur, &reason) == 0 && cur.advertise_len == 10 && !memcmp(cur.advertise, "ADVERTISED", 10), "B sees the new advertise data");

    /* the node limit, then Reject */
    OnlineLdnNetwork full = cfg_of(2, 3);
    A->ldn_destroy(A->ctx);
    CHECK(is_gone(B, &reason) && reason == 3, "DestroyNetwork: B's connection ends with reason DestroyedByUser (%u)", reason);
    CHECK(A->ldn_get_network(A->ctx, &cur, &reason) != 0, "A has no network after DestroyNetwork");
    CHECK(A->ldn_create(A->ctx, &full) == 0, "A hosts again, at most 2 nodes");
    CHECK((n = B->ldn_scan(B->ctx, LCID, nets, 8)) == 1 && B->ldn_connect(B->ctx, &nets[0]) == 0, "B joins the new network");
    CHECK(C->ldn_scan(C->ctx, LCID, nets + 1, 4) == 1 && C->ldn_connect(C->ctx, &nets[1]) == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "a third node is refused: the network is full");
    B->ldn_get_nodes(B->ctx, bn, 8, &bself);
    CHECK(A->ldn_reject(A->ctx, 0x01020304) != 0, "Reject of an address that is no node fails");
    CHECK(A->ldn_reject(A->ctx, bn[1].ipv4) == 0, "A rejects B");
    CHECK(is_gone(B, &reason) && reason == 5, "B's connection ends with reason Rejected (%u)", reason);
    CHECK(A->ldn_get_network(A->ctx, &cur, &reason) == 0 && cur.node_count == 1, "A is alone again");
    CHECK(B->ldn_scan(B->ctx, LCID, nets, 8) == 1 && B->ldn_connect(B->ctx, &nets[0]) == 0, "B joins once more");
    B->ldn_disconnect(B->ctx);
    for (int i = 0; i < 200 && (A->ldn_get_network(A->ctx, &cur, &reason), cur.node_count != 1); i++) usleep(10000);
    CHECK(cur.node_count == 1, "Disconnect: A sees the station leave");
    CHECK(B->ldn_get_network(B->ctx, &cur, &reason) != 0 && reason == 1, "B: no network, reason DisconnectedByUser");
    A->ldn_destroy(A->ctx);

    /* ---- the same through ldn:u, one IUserLocalCommunicationService per console (the active provider is swapped between them) ---- */
    IpcCall c;
    online_provider_install(A);
    IpcObject *ra = ldn_open();
    svc_call(ra, 0, NULL, 0, &c);
    IpcObject *ha = c.out_objects[0];
    svc_call(ha, 100, NULL, 0, &c);
    uint32_t ev_a = c.out_copy[0];
    svc_call(ha, 400, NULL, 0, &c), svc_call(ha, 200, NULL, 0, &c);
    uint8_t create[0x98] = {0};
    memcpy(create + 0x44, "guest-host", 10);
    uint64_t lcid = LCID;
    memcpy(create + 0x78, &lcid, 8);
    create[0x78 + 0x12] = 8;
    uint16_t ver = 3;
    memcpy(create + 0x78 + 0x14, &ver, 2);
    memset(&c, 0, sizeof c); /* MK8D's order: SetAdvertiseData while the access point is only opened, then CreateNetwork */
    svc_set_in_buf(&c, "PRE-CREATE", 10);
    svc_call_c(ha, 206, &c);
    CHECK(c.result == 0, "ldn SetAdvertiseData before CreateNetwork is accepted");
    svc_call(ha, 202, create, sizeof create, &c);
    CHECK(c.result == 0, "ldn CreateNetwork through the provider");
    event_clear_handle(ev_a);

    online_provider_install(B);
    IpcObject *rb = ldn_open();
    svc_call(rb, 0, NULL, 0, &c);
    IpcObject *hb = c.out_objects[0];
    svc_call(hb, 400, NULL, 0, &c), svc_call(hb, 300, NULL, 0, &c);
    uint8_t scan_in[0x68] = {0}, infos[4][LDN_NETINFO_SIZE];
    memcpy(scan_in + 8, &lcid, 8);
    memset(&c, 0, sizeof c);
    c.in = scan_in, c.in_size = sizeof scan_in;
    svc_set_out_buf(&c, infos, sizeof infos);
    svc_call_c(hb, 102, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, int16_t, 0) == 1 && !memcmp(infos[0] + 0x27, "napple-", 7), "ldn Scan: one NetworkInfo with the host's ssid");
    CHECK(infos[0][0x268] == 10 && !memcmp(infos[0] + 0x26a, "PRE-CREATE", 10), "the advertise data set before CreateNetwork is what the station sees");
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, infos[0], LDN_NETINFO_SIZE);
    svc_call_c(hb, 302, &c);
    CHECK(c.result == 0, "ldn Connect with that NetworkInfo");
    svc_call(hb, 0, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == LDN_STATION_CONNECTED, "station state: StationConnected");
    uint8_t info[LDN_NETINFO_SIZE];
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, info, sizeof info);
    svc_call_c(hb, 1, &c);
    uint32_t n0, n1;
    memcpy(&n0, info + 0x68, 4), memcpy(&n1, info + 0x68 + LDN_NODE_SIZE, 4);
    CHECK(c.result == 0 && info[0x67] == 2 && info[0x68 + 0xb] == 1 && info[0x68 + LDN_NODE_SIZE + 0xb] == 1 && info[0x68 + LDN_NODE_SIZE + 0xa] == 1 &&
              !memcmp(info + 0x68 + 0xc, "guest-host", 10) && n0 && n1,
          "station GetNetworkInfo: two connected nodes, node 0 is the host 'guest-host'");
    svc_call(hb, 2, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == n1, "station GetIpv4Address = its own node's address");

    online_provider_install(A);
    CHECK(event_is_signaled(ev_a), "host: the state change event fired when the station joined");
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, info, sizeof info);
    svc_call_c(ha, 1, &c);
    CHECK(c.result == 0 && info[0x67] == 2, "host GetNetworkInfo: two nodes");
    svc_call(ha, 204, NULL, 0, &c);
    CHECK(c.result == 0, "host DestroyNetwork");

    online_provider_install(B);
    uint32_t st_b = 0;
    for (int i = 0; i < 200; i++) {
        svc_call(hb, 0, NULL, 0, &c);
        st_b = SVC_OUT(&c, uint32_t, 0);
        if (st_b != LDN_STATION_CONNECTED) break;
        usleep(10000);
    }
    CHECK(st_b == LDN_STATION_OPENED, "station: back to StationOpened once the host is gone");
    svc_call(hb, 3, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint16_t, 0) == 3, "station GetDisconnectReason = DestroyedByUser");

    online_provider_install(A); /* Finalize while hosting ends the network at the provider, and closing the object stops its notifications */
    svc_call(ha, 200, NULL, 0, &c), svc_call(ha, 202, create, sizeof create, &c);
    CHECK(c.result == 0 && A->ldn_get_network(A->ctx, &cur, &reason) == 0, "host creates again");
    svc_call(ha, 401, NULL, 0, &c);
    CHECK(A->ldn_get_network(A->ctx, &cur, &reason) != 0 && !((Ldn *)ha->state)->watching, "Finalize while hosting: the provider's network is gone, no notification registered");
    online_provider_install(NULL);

    net_host_instance_free(A), net_host_instance_free(B), net_host_instance_free(C);
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
