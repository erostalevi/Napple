/* Napple rooms, LDN: local wireless play between the consoles of a room (net_room.h). Used by net_host_ldn.c's entry points whenever a room is
 * configured.
 *
 * A host advertises its NetworkInfo to the room server (re-advertised on every change, withdrawn when it ends); a scan asks the server for the networks
 * the other members advertise. The session is the LAN transport's (net_host_ldn.c), carried as room packets between virtual addresses: a station sends
 * JOIN, the host answers ACCEPT (node index, network, node list) or REJECT, sends NODES to every station on each change and BYE with a reason when it
 * ends the network or rejects a station. A member that leaves the room leaves the network (the host's leaving ends it for its stations). Node
 * addresses are virtual addresses, so the title's own sockets reach the other nodes over the virtual LAN. Every frame is bounds-checked. */
#include "net_room.h"
#include "net_host.h"
#include "hz_result.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

enum { P_JOIN = 3, P_ACCEPT = 4, P_REJECT = 5, P_NODES = 6, P_BYE = 7 };
enum { R_NONE = 0, R_USER = 1, R_DESTROYED_BY_USER = 3, R_REJECTED = 5, R_SIGNAL_LOST = 6 };
#define R_NODE_NOT_FOUND HZ_RESULT(HZ_MOD_LDN, 24) /* [unverified] as ldn_core.c */
#define JOIN_MS 3000
#define FRAME_MAX 2048

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int role; /* 0 none, 1 host, 2 station */
    OnlineLdnNetwork net;
    OnlineLdnNode nodes[ONLINE_LDN_MAX_NODES];
    uint32_t self, host_vip;
    uint16_t reason;
    /* station: the join in progress */
    int joining, join_answer; /* answer 1 accepted, 2 rejected */
    uint32_t joining_vip;
    /* scan */
    uint32_t scan_req, scan_got_req;
    uint8_t *scan_body;
    size_t scan_len;
    struct {
        uint8_t bssid[6];
        uint32_t vip;
    } seen[ONLINE_LDN_MAX_NETWORKS];
    unsigned nseen;
    pthread_mutex_t cb_lock;
    void (*cb)(void *);
    void *cb_arg;
} L = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .cb_lock = PTHREAD_MUTEX_INITIALIZER};

int net_room_ldn_active(void) { return net_room_configured(); }

void nr_ldn_set_notify(void (*cb)(void *), void *arg) {
    pthread_mutex_lock(&L.cb_lock);
    L.cb = cb;
    L.cb_arg = arg;
    pthread_mutex_unlock(&L.cb_lock);
}
static void notify(void) {
    pthread_mutex_lock(&L.cb_lock);
    if (L.cb) L.cb(L.cb_arg);
    pthread_mutex_unlock(&L.cb_lock);
}

static unsigned connected_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < ONLINE_LDN_MAX_NODES; i++) n += L.nodes[i].connected;
    return n;
}
static void fake_mac(uint8_t mac[6], uint32_t ip, uint8_t node) {
    mac[0] = 0x02, mac[1] = node, mac[2] = (uint8_t)(ip >> 24), mac[3] = (uint8_t)(ip >> 16), mac[4] = (uint8_t)(ip >> 8), mac[5] = (uint8_t)ip;
}
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static uint16_t get16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static void send_frame(uint32_t dst, uint8_t kind, const uint8_t *p, size_t n) { net_room_send_packet(dst, NR_BODY_LDN, kind, 0, 0, p, n); }
static void send_reason(uint32_t dst, uint8_t kind, uint16_t reason) {
    uint8_t b[2];
    put16(b, reason);
    send_frame(dst, kind, b, 2);
}

/* ---- host (lock held) --------------------------------------------------------------------------------------------------------------------------------- */
static void host_advertise(void) {
    uint8_t b[FRAME_MAX];
    OnlineLdnNetwork n = L.net;
    n.node_count = (uint8_t)connected_count();
    n.ipv4 = L.nodes[0].ipv4;
    size_t k = nh_ldn_encode_net(&n, b, sizeof b);
    uint8_t f[8 + FRAME_MAX + 2];
    if (!k) return;
    memcpy(f, &n.local_communication_id, 8);
    put16(f + 8, (uint16_t)k);
    memcpy(f + 10, b, k);
    net_room_send_frame(NR_T_LDN_ADVERTISE, f, 10 + k);
}
static void host_withdraw(void) {
    uint8_t f[10] = {0};
    net_room_send_frame(NR_T_LDN_ADVERTISE, f, 10);
}
/* ACCEPT / NODES for the station at node index i: {u8 its index, u16 len, network, node list} */
static void host_tell(unsigned i, uint8_t kind) {
    uint8_t f[FRAME_MAX * 2];
    OnlineLdnNetwork n = L.net;
    n.node_count = (uint8_t)connected_count();
    n.ipv4 = L.nodes[0].ipv4;
    f[0] = (uint8_t)i;
    size_t k = nh_ldn_encode_net(&n, f + 3, FRAME_MAX);
    if (!k) return;
    put16(f + 1, (uint16_t)k);
    size_t m = nh_ldn_encode_nodes(L.nodes, f + 3 + k, sizeof f - 3 - k);
    if (!m) return;
    send_frame(L.nodes[i].ipv4, kind, f, 3 + k + m);
}
/* NODES to every station except `skip` (the one that just got the same list in its ACCEPT). */
static void host_broadcast_nodes_except(unsigned skip) {
    for (unsigned i = 1; i < ONLINE_LDN_MAX_NODES; i++)
        if (L.nodes[i].connected && i != skip) host_tell(i, P_NODES);
    host_advertise();
}
static void host_broadcast_nodes(void) { host_broadcast_nodes_except(0); }
static int node_of(uint32_t vip) {
    for (unsigned i = 1; i < ONLINE_LDN_MAX_NODES; i++)
        if (L.nodes[i].connected && L.nodes[i].ipv4 == vip) return (int)i;
    return -1;
}
static int host_join(uint32_t src, const uint8_t *p, size_t n) {
    if (n < 33 + 2 + 8) return 0;
    char name[33];
    memcpy(name, p, 33);
    name[32] = 0;
    uint16_t version = get16(p + 33);
    uint64_t lcid;
    memcpy(&lcid, p + 35, 8);
    if (node_of(src) >= 0) {
        host_tell((unsigned)node_of(src), P_ACCEPT); /* a repeated JOIN: answer again */
        return 0;
    }
    int slot = -1;
    for (int i = 1; i < ONLINE_LDN_MAX_NODES && slot < 0; i++)
        if (!L.nodes[i].connected) slot = i;
    unsigned max = L.net.node_count_max ? L.net.node_count_max : ONLINE_LDN_MAX_NODES;
    if (lcid != L.net.local_communication_id || version != L.net.local_communication_version || slot < 0 || connected_count() >= max) {
        send_reason(src, P_REJECT, R_REJECTED);
        return 0;
    }
    OnlineLdnNode *nd = &L.nodes[slot];
    memset(nd, 0, sizeof *nd);
    nd->ipv4 = src;
    fake_mac(nd->mac, src, (uint8_t)slot);
    nd->node_id = (uint8_t)slot;
    nd->connected = 1;
    memcpy(nd->user_name, name, sizeof nd->user_name);
    host_tell((unsigned)slot, P_ACCEPT);
    host_broadcast_nodes_except((unsigned)slot);
    log_line("net", "room ldn: station '%s' joined as node %d", name, slot);
    return 1;
}
static int host_drop(uint32_t vip, uint16_t bye) {
    int i = node_of(vip);
    if (i < 0) return 0;
    if (bye) send_reason(vip, P_BYE, bye);
    memset(&L.nodes[i], 0, sizeof L.nodes[i]);
    host_broadcast_nodes();
    log_line("net", "room ldn: node %d left", i);
    return 1;
}

/* ---- station (lock held) ------------------------------------------------------------------------------------------------------------------------------ */
static void teardown(uint16_t reason) {
    memset(L.nodes, 0, sizeof L.nodes);
    memset(&L.net, 0, sizeof L.net);
    L.role = 0;
    L.self = 0;
    L.host_vip = 0;
    L.reason = reason;
}
/* ACCEPT / NODES from the host. 1 = applied. */
static int station_update(const uint8_t *p, size_t n) {
    if (n < 3) return 0;
    unsigned self = p[0];
    size_t k = get16(p + 1);
    if (self >= ONLINE_LDN_MAX_NODES || 3 + k > n) return 0;
    OnlineLdnNetwork net;
    OnlineLdnNode nodes[ONLINE_LDN_MAX_NODES];
    if (!nh_ldn_decode_net(p + 3, k, &net) || !nh_ldn_decode_nodes(p + 3 + k, n - 3 - k, nodes)) return 0;
    L.net = net;
    memcpy(L.nodes, nodes, sizeof nodes);
    L.self = self;
    return 1;
}

/* ---- inbound ------------------------------------------------------------------------------------------------------------------------------------------ */
void nr_ldn_packet_in(uint32_t src, uint8_t kind, const uint8_t *p, size_t n) {
    int changed = 0;
    pthread_mutex_lock(&L.lock);
    if (L.role == 1) {
        if (kind == P_JOIN) changed = host_join(src, p, n);
        else if (kind == P_BYE) changed = host_drop(src, 0);
    } else if (L.joining && src == L.joining_vip && (kind == P_ACCEPT || kind == P_REJECT)) {
        if (kind == P_ACCEPT && station_update(p, n)) L.join_answer = 1;
        else if (kind == P_REJECT) {
            L.join_answer = 2;
            L.reason = n >= 2 ? get16(p) : R_REJECTED;
        }
        pthread_cond_broadcast(&L.cond);
    } else if (L.role == 2 && src == L.host_vip) {
        if (kind == P_NODES || kind == P_ACCEPT) changed = station_update(p, n);
        else if (kind == P_BYE || kind == P_REJECT) {
            uint16_t r = n >= 2 ? get16(p) : R_SIGNAL_LOST;
            log_line("net", "room ldn: the host ended our membership (reason %u)", r);
            teardown(r);
            changed = 1;
        }
    }
    pthread_mutex_unlock(&L.lock);
    if (changed) notify();
}
void nr_ldn_member_left(uint32_t vip) {
    int changed = 0;
    pthread_mutex_lock(&L.lock);
    if (L.role == 1) changed = host_drop(vip, 0);
    else if (L.role == 2 && vip == L.host_vip) {
        log_line("net", "room ldn: the host left the room");
        teardown(R_SIGNAL_LOST);
        changed = 1;
    }
    pthread_mutex_unlock(&L.lock);
    if (changed) notify();
}
void nr_ldn_room_down(void) {
    pthread_mutex_lock(&L.lock);
    int changed = L.role != 0;
    if (changed) teardown(R_SIGNAL_LOST);
    pthread_cond_broadcast(&L.cond);
    pthread_mutex_unlock(&L.lock);
    if (changed) notify();
}
void nr_ldn_scan_result(const uint8_t *body, size_t n) {
    if (n < 4) return;
    pthread_mutex_lock(&L.lock);
    uint32_t req;
    memcpy(&req, body, 4);
    if (req == L.scan_req) {
        free(L.scan_body);
        L.scan_body = malloc(n);
        if (L.scan_body) memcpy(L.scan_body, body, n);
        L.scan_len = L.scan_body ? n : 0;
        L.scan_got_req = req;
        pthread_cond_broadcast(&L.cond);
    }
    pthread_mutex_unlock(&L.lock);
}

/* Waits on L.cond until pred or the deadline (lock held), giving the virtual core away. */
#define WAIT_UNTIL(pred, ms)                                                                                                                                   \
    do {                                                                                                                                                       \
        int tok_;                                                                                                                                              \
        int64_t end_ = nh_now_ms() + (ms);                                                                                                                     \
        nh_core_away(&tok_);                                                                                                                                   \
        while (!(pred) && nh_now_ms() < end_) {                                                                                                                \
            struct timespec ts_;                                                                                                                               \
            clock_gettime(CLOCK_REALTIME, &ts_);                                                                                                               \
            ts_.tv_nsec += 20 * 1000 * 1000;                                                                                                                   \
            if (ts_.tv_nsec >= 1000000000) ts_.tv_sec++, ts_.tv_nsec -= 1000000000;                                                                           \
            pthread_cond_timedwait(&L.cond, &L.lock, &ts_);                                                                                                    \
        }                                                                                                                                                      \
        nh_core_back(&tok_);                                                                                                                                   \
    } while (0)

/* ---- entry points ------------------------------------------------------------------------------------------------------------------------------------- */
static int scan_wait_ms(void) {
    const char *e = getenv("NAPPLE_LDN_SCAN_MS");
    long v = e ? strtol(e, NULL, 10) : 0;
    return v > 0 && v <= 5000 ? (int)(v < 500 ? 500 : v) : 1000; /* a round trip to the server, not a LAN broadcast */
}
int nr_ldn_scan(uint64_t lcid, OnlineLdnNetwork *out, uint32_t max) {
    if (!net_room_up()) return 0;
    pthread_mutex_lock(&L.lock);
    uint32_t req = ++L.scan_req;
    uint8_t b[12];
    memcpy(b, &req, 4), memcpy(b + 4, &lcid, 8);
    if (net_room_send_frame(NR_T_LDN_SCAN, b, sizeof b) < 0) {
        pthread_mutex_unlock(&L.lock);
        return 0;
    }
    WAIT_UNTIL(L.scan_got_req == req || !net_room_up(), scan_wait_ms());
    int got = 0;
    if (L.scan_got_req == req && L.scan_len >= 6) {
        const uint8_t *p = L.scan_body;
        size_t n = L.scan_len, off = 6;
        unsigned count = get16(p + 4);
        for (unsigned i = 0; i < count && off + 18 <= n; i++) {
            uint32_t vip;
            uint64_t elcid;
            memcpy(&vip, p + off + 4, 4);
            memcpy(&elcid, p + off + 8, 8);
            size_t bl = get16(p + off + 16);
            if (off + 18 + bl > n) break;
            OnlineLdnNetwork net;
            int ok = nh_ldn_decode_net(p + off + 18, bl, &net) != 0;
            off += 18 + bl;
            if (!ok || (lcid && net.local_communication_id != lcid) || (uint32_t)got >= max) continue;
            net.ipv4 = vip;
            out[got++] = net;
            unsigned j = 0;
            while (j < L.nseen && memcmp(L.seen[j].bssid, net.bssid, 6)) j++;
            if (j == L.nseen) j = L.nseen < ONLINE_LDN_MAX_NETWORKS ? L.nseen++ : ONLINE_LDN_MAX_NETWORKS - 1;
            memcpy(L.seen[j].bssid, net.bssid, 6);
            L.seen[j].vip = vip;
        }
    }
    static int last = -1;
    if (got != last) log_line("net", "room ldn: scan (title %#llx) finds %d network(s)%s", (unsigned long long)lcid, got, L.scan_got_req == req ? "" : " (no answer)");
    last = got;
    pthread_mutex_unlock(&L.lock);
    return got;
}

uint32_t nr_ldn_create(const OnlineLdnNetwork *cfg) {
    nr_ldn_destroy();
    nr_ldn_disconnect();
    uint32_t vip = net_room_vip();
    if (!net_room_vlan_on() || !vip) {
        log_line("net", "room ldn: cannot host: %s", net_room_up() ? "the virtual LAN is off (NAPPLE_VLAN=off)" : "not in the room yet");
        return HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    }
    pthread_mutex_lock(&L.lock);
    L.net = *cfg;
    arc4random_buf(L.net.bssid, 6);
    L.net.bssid[0] = (uint8_t)((L.net.bssid[0] & 0xfe) | 0x02);
    arc4random_buf(L.net.security_param, 16);
    if (!L.net.ssid[0]) {
        uint8_t r[8];
        arc4random_buf(r, sizeof r);
        snprintf(L.net.ssid, sizeof L.net.ssid, "napple-%02x%02x%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    }
    if (!L.net.channel) L.net.channel = 6;
    if (!L.net.node_count_max || L.net.node_count_max > ONLINE_LDN_MAX_NODES) L.net.node_count_max = ONLINE_LDN_MAX_NODES;
    L.net.ipv4 = vip;
    memset(L.nodes, 0, sizeof L.nodes);
    L.nodes[0].ipv4 = vip;
    fake_mac(L.nodes[0].mac, vip, 0);
    L.nodes[0].connected = 1;
    memcpy(L.nodes[0].user_name, L.net.host_name[0] ? L.net.host_name : net_room_nick(), sizeof L.nodes[0].user_name - 1);
    L.self = 0;
    L.role = 1;
    L.reason = R_NONE;
    host_advertise();
    log_line("net", "room ldn: network '%s' hosted in the room", L.net.ssid);
    pthread_mutex_unlock(&L.lock);
    return 0;
}
void nr_ldn_destroy(void) {
    pthread_mutex_lock(&L.lock);
    if (L.role == 1) {
        for (unsigned i = 1; i < ONLINE_LDN_MAX_NODES; i++)
            if (L.nodes[i].connected) send_reason(L.nodes[i].ipv4, P_BYE, R_DESTROYED_BY_USER);
        host_withdraw();
        teardown(R_DESTROYED_BY_USER);
    }
    pthread_mutex_unlock(&L.lock);
}
uint32_t nr_ldn_set_advertise(const void *data, uint32_t n) {
    if (n > ONLINE_LDN_ADVERTISE_MAX) n = ONLINE_LDN_ADVERTISE_MAX;
    pthread_mutex_lock(&L.lock);
    uint32_t r = L.role == 1 ? 0 : HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    if (!r) {
        if (n) memcpy(L.net.advertise, data, n);
        L.net.advertise_len = (uint16_t)n;
        host_broadcast_nodes();
    }
    pthread_mutex_unlock(&L.lock);
    return r;
}
uint32_t nr_ldn_reject(uint32_t ipv4) {
    pthread_mutex_lock(&L.lock);
    uint32_t r = R_NODE_NOT_FOUND;
    if (L.role == 1) {
        uint32_t vip = ipv4 < ONLINE_LDN_MAX_NODES && ipv4 ? L.nodes[ipv4].ipv4 : ipv4; /* an address, or a node id */
        if (vip && host_drop(vip, R_REJECTED)) r = 0;
    }
    pthread_mutex_unlock(&L.lock);
    if (!r) notify();
    return r;
}

uint32_t nr_ldn_connect(const OnlineLdnNetwork *net) {
    nr_ldn_destroy();
    nr_ldn_disconnect();
    if (!net_room_vlan_on()) return HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    pthread_mutex_lock(&L.lock);
    uint32_t vip = 0;
    for (unsigned i = 0; i < L.nseen && !vip; i++)
        if (!memcmp(L.seen[i].bssid, net->bssid, 6)) vip = L.seen[i].vip;
    pthread_mutex_unlock(&L.lock);
    if (!vip) { /* a NetworkInfo the guest kept from an earlier run: look again */
        OnlineLdnNetwork tmp[ONLINE_LDN_MAX_NETWORKS];
        nr_ldn_scan(net->local_communication_id, tmp, ONLINE_LDN_MAX_NETWORKS);
        pthread_mutex_lock(&L.lock);
        for (unsigned i = 0; i < L.nseen && !vip; i++)
            if (!memcmp(L.seen[i].bssid, net->bssid, 6)) vip = L.seen[i].vip;
        pthread_mutex_unlock(&L.lock);
        if (!vip) return HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    }
    uint8_t f[33 + 2 + 8 + 6] = {0};
    snprintf((char *)f, 33, "%s", net_room_nick());
    put16(f + 33, net->local_communication_version);
    memcpy(f + 35, &net->local_communication_id, 8);
    memcpy(f + 43, net->bssid, 6);
    pthread_mutex_lock(&L.lock);
    L.joining = 1, L.join_answer = 0, L.joining_vip = vip;
    send_frame(vip, P_JOIN, f, sizeof f);
    WAIT_UNTIL(L.join_answer || !net_room_up(), JOIN_MS);
    uint32_t r = HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    if (L.join_answer == 1) {
        L.role = 2;
        L.host_vip = vip;
        L.reason = R_NONE;
        r = 0;
        log_line("net", "room ldn: joined '%s' as node %u", L.net.ssid, L.self);
    } else if (!L.join_answer) {
        send_reason(vip, P_BYE, R_USER); /* no answer in time: make sure the host forgets us */
    }
    L.joining = 0;
    pthread_mutex_unlock(&L.lock);
    return r;
}
void nr_ldn_disconnect(void) {
    pthread_mutex_lock(&L.lock);
    if (L.role == 2) {
        send_reason(L.host_vip, P_BYE, R_USER);
        teardown(R_USER);
    }
    pthread_mutex_unlock(&L.lock);
}

uint32_t nr_ldn_get_network(OnlineLdnNetwork *out, uint16_t *reason) {
    pthread_mutex_lock(&L.lock);
    uint32_t r = L.role ? 0 : HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    memset(out, 0, sizeof *out);
    if (!r) {
        *out = L.net;
        out->node_count = (uint8_t)connected_count();
        out->ipv4 = L.nodes[L.self].ipv4;
    }
    if (reason) *reason = L.reason;
    pthread_mutex_unlock(&L.lock);
    return r;
}
int nr_ldn_get_nodes(OnlineLdnNode *out, uint32_t max, uint32_t *self) {
    pthread_mutex_lock(&L.lock);
    int n = -1;
    if (L.role) {
        n = (int)(max < ONLINE_LDN_MAX_NODES ? max : ONLINE_LDN_MAX_NODES);
        memcpy(out, L.nodes, sizeof *out * (size_t)n);
        if (self) *self = L.self;
    }
    pthread_mutex_unlock(&L.lock);
    return n;
}
