/* Host provider, ldn: local wireless play emulated over the LAN between Napple instances (on one Mac or on several machines of one network).
 *
 * Addressing follows the ldn_mitm idea: a node's address in the network is its real address on the LAN, so the title's own bsd sockets reach the other
 * consoles with no translation. Two instances on one Mac share 127.0.0.1 (or the Mac's address), so titles that bind one fixed port on every node
 * can only run one node per Mac.
 *
 * Discovery: a host listens on the first free UDP port of [NAPPLE_LDN_PORT, +8) (default 11452). A scan sends one SCAN datagram to each of those ports
 * on 127.0.0.1, on the IPv4 broadcast address and on every address of NAPPLE_LDN_PEERS (comma-separated, for networks that drop broadcasts), and collects
 * the answers for NAPPLE_LDN_SCAN_MS (default 150 ms). Session: a station opens a TCP connection to the host's control port (given in the answer),
 * sends JOIN and gets ACCEPT (its node index, the network, the node list) or REJECT; the host sends the new node list to every station on each change
 * (NODES) and BYE with a disconnect reason when it destroys the network or rejects a station. Datagrams and frames carry a magic, a type and a length,
 * and every field is bounds-checked: peers are not trusted.
 *
 * Not modelled: the passphrase (the service does not hand it over) and the accept policy / MAC filters (the service keeps them). */
#include "net_host.h"
#include "hz_result.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LDN_MAGIC 0x4e444c4eu /* "NLDN" */
#define LDN_HDR 8u
#define LDN_PORT_SPAN 8
#define LDN_FRAME_MAX 2048u
#define LDN_STATIONS (ONLINE_LDN_MAX_NODES - 1)
#define LDN_JOIN_MS 3000
enum { P_SCAN = 1, P_SCAN_REPLY = 2, P_JOIN = 3, P_ACCEPT = 4, P_REJECT = 5, P_NODES = 6, P_BYE = 7 };
/* nn::ldn DisconnectReason */
enum { R_NONE = 0, R_USER = 1, R_DESTROYED_BY_USER = 3, R_REJECTED = 5, R_SIGNAL_LOST = 6 };
#define R_NODE_NOT_FOUND HZ_RESULT(HZ_MOD_LDN, 24) /* [unverified] as ldn_core.c */

typedef struct {
    int fd;
    int node; /* host side: node index once joined, 0 while the JOIN is pending */
    uint8_t buf[LDN_FRAME_MAX];
    size_t len;
} Conn;

struct NhLdn {
    pthread_mutex_t lock;
    int role; /* 0 none, 1 host, 2 station */
    OnlineLdnNetwork net;
    OnlineLdnNode nodes[ONLINE_LDN_MAX_NODES];
    uint32_t self;
    uint16_t reason;
    /* host */
    int udp_fd, listen_fd;
    uint16_t ctl_port, disc_port;
    Conn sta[LDN_STATIONS];
    /* station */
    Conn link;
    /* scan results: where each network seen lives */
    struct {
        uint8_t bssid[6];
        uint32_t ip;
        uint16_t port;
    } seen[ONLINE_LDN_MAX_NETWORKS];
    unsigned nseen;
    /* worker */
    pthread_t th;
    int running; /* the worker should keep going (cleared by teardown, also from the worker itself) */
    int th_live; /* a worker thread exists and has not been joined */
    int wake[2];
    pthread_mutex_t cb_lock;
    void (*cb)(void *);
    void *cb_arg;
};

/* ---- wire helpers ---------------------------------------------------------------------------------------------------- */
typedef struct {
    uint8_t *p;
    size_t n, cap;
    int bad;
} W;
static void wput(W *w, const void *d, size_t n) {
    if (w->n + n > w->cap) {
        w->bad = 1;
        return;
    }
    memcpy(w->p + w->n, d, n);
    w->n += n;
}
static void w8(W *w, uint8_t v) { wput(w, &v, 1); }
static void w16(W *w, uint16_t v) { wput(w, &v, 2); }
static void w32(W *w, uint32_t v) { wput(w, &v, 4); }
static void w64(W *w, uint64_t v) { wput(w, &v, 8); }
typedef struct {
    const uint8_t *p;
    size_t n, off;
    int bad;
} Rd;
static void rget(Rd *r, void *d, size_t n) {
    if (r->off + n > r->n) {
        r->bad = 1;
        memset(d, 0, n);
        return;
    }
    memcpy(d, r->p + r->off, n);
    r->off += n;
}
static uint8_t r8(Rd *r) {
    uint8_t v;
    rget(r, &v, 1);
    return v;
}
static uint16_t r16(Rd *r) {
    uint16_t v;
    rget(r, &v, 2);
    return v;
}
static uint32_t r32(Rd *r) {
    uint32_t v;
    rget(r, &v, 4);
    return v;
}
static uint64_t r64(Rd *r) {
    uint64_t v;
    rget(r, &v, 8);
    return v;
}
static void wstr(W *w, const char *s) { /* fixed 33-byte field */
    char b[33] = {0};
    strncpy(b, s, 32);
    wput(w, b, 33);
}
static void rstr(Rd *r, char out[33]) {
    rget(r, out, 33);
    out[32] = 0;
}
static void put_net(W *w, const OnlineLdnNetwork *n) {
    uint16_t al = n->advertise_len > ONLINE_LDN_ADVERTISE_MAX ? ONLINE_LDN_ADVERTISE_MAX : n->advertise_len;
    w64(w, n->local_communication_id);
    w16(w, n->scene_id);
    w16(w, al);
    wput(w, n->bssid, 6);
    w8(w, n->channel), w8(w, n->node_count), w8(w, n->node_count_max), w8(w, n->security_mode);
    w16(w, n->local_communication_version);
    wput(w, n->security_param, 16);
    wstr(w, n->ssid);
    wstr(w, n->host_name);
    w32(w, n->ipv4);
    wput(w, n->advertise, al);
}
static void get_net(Rd *r, OnlineLdnNetwork *n) {
    memset(n, 0, sizeof *n);
    n->local_communication_id = r64(r);
    n->scene_id = r16(r);
    uint16_t al = r16(r);
    rget(r, n->bssid, 6);
    n->channel = r8(r), n->node_count = r8(r), n->node_count_max = r8(r), n->security_mode = r8(r);
    n->local_communication_version = r16(r);
    rget(r, n->security_param, 16);
    rstr(r, n->ssid);
    rstr(r, n->host_name);
    n->ipv4 = r32(r);
    if (al > ONLINE_LDN_ADVERTISE_MAX) {
        r->bad = 1;
        return;
    }
    rget(r, n->advertise, al);
    n->advertise_len = al;
}
static void put_nodes(W *w, const OnlineLdnNode *nodes, uint32_t node0_ip) {
    w8(w, ONLINE_LDN_MAX_NODES);
    for (unsigned i = 0; i < ONLINE_LDN_MAX_NODES; i++) {
        w32(w, i == 0 ? node0_ip : nodes[i].ipv4);
        wput(w, nodes[i].mac, 6);
        w8(w, nodes[i].node_id), w8(w, nodes[i].connected);
        wstr(w, nodes[i].user_name);
    }
}
static void get_nodes(Rd *r, OnlineLdnNode *nodes) {
    unsigned n = r8(r);
    memset(nodes, 0, sizeof(OnlineLdnNode) * ONLINE_LDN_MAX_NODES);
    if (n > ONLINE_LDN_MAX_NODES) {
        r->bad = 1;
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        nodes[i].ipv4 = r32(r);
        rget(r, nodes[i].mac, 6);
        nodes[i].node_id = r8(r), nodes[i].connected = r8(r);
        rstr(r, nodes[i].user_name);
    }
}
static void hdr(W *w, uint16_t type) {
    w32(w, LDN_MAGIC);
    w16(w, type);
    w16(w, 0); /* patched by fin */
}
static size_t fin(W *w) {
    uint16_t len = (uint16_t)(w->n - LDN_HDR);
    memcpy(w->p + 6, &len, 2);
    return w->bad ? 0 : w->n;
}
static int send_all(int fd, const void *p, size_t n) {
    size_t done = 0;
    int64_t end = nh_now_ms() + LDN_JOIN_MS;
    while (done < n) {
        ssize_t k = send(fd, (const uint8_t *)p + done, n - done, 0);
        if (k > 0) {
            done += (size_t)k;
            continue;
        }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && errno == EAGAIN && nh_now_ms() < end) {
            struct pollfd pf = {fd, POLLOUT, 0};
            poll(&pf, 1, 50);
            continue;
        }
        return -1;
    }
    return 0;
}
/* Reads what is available into c->buf. Returns -1 on EOF or error. Frames are taken out with next_frame. */
static int conn_fill(Conn *c) {
    if (c->len >= sizeof c->buf) return -1; /* a peer that never finishes a frame */
    ssize_t k = recv(c->fd, c->buf + c->len, sizeof c->buf - c->len, 0);
    if (k > 0) {
        c->len += (size_t)k;
        return 0;
    }
    if (k < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    return -1;
}
/* The next complete frame: its type, payload and length (consumed on the next call). 0 = none yet, -1 = malformed. */
static int next_frame(Conn *c, uint16_t *type, uint8_t *payload, size_t *plen) {
    if (c->len < LDN_HDR) return 0;
    uint32_t magic;
    uint16_t len;
    memcpy(&magic, c->buf, 4);
    memcpy(type, c->buf + 4, 2);
    memcpy(&len, c->buf + 6, 2);
    if (magic != LDN_MAGIC || LDN_HDR + len > sizeof c->buf) return -1;
    if (c->len < LDN_HDR + len) return 0;
    memcpy(payload, c->buf + LDN_HDR, len);
    *plen = len;
    memmove(c->buf, c->buf + LDN_HDR + len, c->len - LDN_HDR - len);
    c->len -= LDN_HDR + len;
    return 1;
}
static void conn_close(Conn *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->len = 0;
    c->node = 0;
}
static int nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static uint16_t base_port(void) {
    const char *e = getenv("NAPPLE_LDN_PORT");
    long v = e ? strtol(e, NULL, 10) : 0;
    return v > 0 && v < 65536 - LDN_PORT_SPAN ? (uint16_t)v : 11452;
}
static uint32_t self_ip(void) {
    OnlineNetInfo ni;
    nh_net_info(NULL, &ni);
    return ni.connected && ni.ip ? ni.ip : 0x7f000001u;
}
static void fake_mac(uint8_t mac[6], uint32_t ip, uint8_t node) { /* locally administered, stable per address and node */
    mac[0] = 0x02, mac[1] = node, mac[2] = (uint8_t)(ip >> 24), mac[3] = (uint8_t)(ip >> 16), mac[4] = (uint8_t)(ip >> 8), mac[5] = (uint8_t)ip;
}

/* ---- life cycle ------------------------------------------------------------------------------------------------------ */
NhLdn *nh_ldn_new(void) {
    NhLdn *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    pthread_mutex_init(&l->lock, NULL);
    pthread_mutex_init(&l->cb_lock, NULL);
    l->udp_fd = l->listen_fd = l->link.fd = -1;
    l->wake[0] = l->wake[1] = -1;
    for (int i = 0; i < LDN_STATIONS; i++) l->sta[i].fd = -1;
    return l;
}
static void teardown(NhLdn *l, uint16_t reason);
void nh_ldn_free(NhLdn *l) {
    if (!l) return;
    nh_ldn_destroy(&(NetHost){l});
    nh_ldn_disconnect(&(NetHost){l});
    free(l);
}
void nh_ldn_set_notify(void *ctx, void (*cb)(void *), void *arg) {
    NhLdn *l = nh_ldn_of(ctx);
    pthread_mutex_lock(&l->cb_lock); /* after this returns with NULL, no callback runs any more */
    l->cb = cb;
    l->cb_arg = arg;
    pthread_mutex_unlock(&l->cb_lock);
}
static void notify(NhLdn *l) {
    pthread_mutex_lock(&l->cb_lock);
    if (l->cb) l->cb(l->cb_arg);
    pthread_mutex_unlock(&l->cb_lock);
}

static unsigned connected_count(const NhLdn *l) {
    unsigned n = 0;
    for (unsigned i = 0; i < ONLINE_LDN_MAX_NODES; i++) n += l->nodes[i].connected;
    return n;
}
/* host: the ACCEPT / NODES frame for the station on `c` (lock held). */
static size_t station_frame(NhLdn *l, Conn *c, uint16_t type, uint8_t *buf, size_t cap) {
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    uint32_t node0 = l->nodes[0].ipv4;
    if (getsockname(c->fd, (struct sockaddr *)&me, &ml) == 0 && me.sin_family == AF_INET) node0 = ntohl(me.sin_addr.s_addr); /* the host as this station reaches it */
    OnlineLdnNetwork n = l->net;
    n.node_count = (uint8_t)connected_count(l);
    n.ipv4 = node0;
    W w = {buf, 0, cap, 0};
    hdr(&w, type);
    w8(&w, (uint8_t)c->node);
    w32(&w, l->nodes[c->node].ipv4);
    put_net(&w, &n);
    put_nodes(&w, l->nodes, node0);
    return fin(&w);
}
static void host_broadcast_nodes(NhLdn *l) {
    uint8_t buf[LDN_FRAME_MAX];
    for (int i = 0; i < LDN_STATIONS; i++) {
        Conn *c = &l->sta[i];
        if (c->fd < 0 || !c->node) continue;
        size_t n = station_frame(l, c, P_NODES, buf, sizeof buf);
        if (n && send_all(c->fd, buf, n) < 0) {
            l->nodes[c->node].connected = 0;
            conn_close(c);
        }
    }
}
static void send_reason(int fd, uint16_t type, uint16_t reason) {
    uint8_t buf[16];
    W w = {buf, 0, sizeof buf, 0};
    hdr(&w, type);
    w16(&w, reason);
    size_t n = fin(&w);
    if (n) send_all(fd, buf, n);
}
static void host_drop_station(NhLdn *l, Conn *c, uint16_t bye_reason) {
    if (bye_reason) send_reason(c->fd, P_BYE, bye_reason);
    if (c->node) memset(&l->nodes[c->node], 0, sizeof l->nodes[c->node]);
    conn_close(c);
    host_broadcast_nodes(l);
}

/* host: a JOIN on station connection c (lock held). Returns 1 when the node list changed. */
static int host_join(NhLdn *l, Conn *c, const uint8_t *p, size_t n) {
    Rd r = {p, n, 0, 0};
    char name[33];
    rstr(&r, name);
    uint16_t version = r16(&r);
    uint64_t lcid = r64(&r);
    int slot = -1;
    for (int i = 1; i < ONLINE_LDN_MAX_NODES && slot < 0; i++)
        if (!l->nodes[i].connected) slot = i;
    if (r.bad || c->node || lcid != l->net.local_communication_id || version != l->net.local_communication_version || slot < 0 ||
        connected_count(l) >= (l->net.node_count_max ? l->net.node_count_max : ONLINE_LDN_MAX_NODES)) {
        send_reason(c->fd, P_REJECT, R_REJECTED);
        conn_close(c);
        return 0;
    }
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    uint32_t ip = getpeername(c->fd, (struct sockaddr *)&peer, &pl) == 0 && peer.sin_family == AF_INET ? ntohl(peer.sin_addr.s_addr) : 0;
    OnlineLdnNode *nd = &l->nodes[slot];
    memset(nd, 0, sizeof *nd);
    nd->ipv4 = ip;
    fake_mac(nd->mac, ip, (uint8_t)slot);
    nd->node_id = (uint8_t)slot;
    nd->connected = 1;
    memcpy(nd->user_name, name, sizeof nd->user_name);
    c->node = slot;
    uint8_t buf[LDN_FRAME_MAX];
    size_t k = station_frame(l, c, P_ACCEPT, buf, sizeof buf);
    if (!k || send_all(c->fd, buf, k) < 0) {
        memset(nd, 0, sizeof *nd);
        conn_close(c);
        return 0;
    }
    log_line("net", "host provider: ldn station '%s' joined as node %d", name, slot);
    host_broadcast_nodes(l);
    return 1;
}
/* host: a SCAN datagram (lock held). */
static void host_scan(NhLdn *l) {
    uint8_t in[64], out[LDN_FRAME_MAX];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t k = recvfrom(l->udp_fd, in, sizeof in, 0, (struct sockaddr *)&from, &fl);
    if (k < (ssize_t)(LDN_HDR + 8)) return;
    uint32_t magic;
    uint16_t type;
    uint64_t lcid;
    memcpy(&magic, in, 4), memcpy(&type, in + 4, 2), memcpy(&lcid, in + LDN_HDR, 8);
    if (magic != LDN_MAGIC || type != P_SCAN || (lcid && lcid != l->net.local_communication_id)) return;
    OnlineLdnNetwork n = l->net;
    n.node_count = (uint8_t)connected_count(l);
    W w = {out, 0, sizeof out, 0};
    hdr(&w, P_SCAN_REPLY);
    w16(&w, l->ctl_port);
    put_net(&w, &n);
    size_t len = fin(&w);
    if (len) sendto(l->udp_fd, out, len, 0, (struct sockaddr *)&from, fl);
}
/* station: a frame from the host (lock held). Returns 1 when something the guest sees changed; *gone when the network ended. */
static int station_frame_in(NhLdn *l, uint16_t type, const uint8_t *p, size_t n, int *gone) {
    Rd r = {p, n, 0, 0};
    if (type == P_NODES || type == P_ACCEPT) {
        uint8_t self = r8(&r);
        uint32_t ip = r32(&r);
        OnlineLdnNetwork net;
        OnlineLdnNode nodes[ONLINE_LDN_MAX_NODES];
        get_net(&r, &net);
        get_nodes(&r, nodes);
        if (r.bad || self >= ONLINE_LDN_MAX_NODES) return 0;
        net.ipv4 = ip;
        l->net = net;
        memcpy(l->nodes, nodes, sizeof nodes);
        l->self = self;
        return 1;
    }
    if (type == P_BYE || type == P_REJECT) {
        uint16_t reason = r16(&r);
        l->reason = r.bad ? R_SIGNAL_LOST : reason;
        *gone = 1;
        return 1;
    }
    return 0;
}

static void *worker(void *arg) {
    NhLdn *l = arg;
    for (;;) {
        struct pollfd pf[3 + LDN_STATIONS];
        int idx[3 + LDN_STATIONS]; /* -1 wake, -2 udp, -3 listen, -4 link, else station slot */
        unsigned n = 0;
        pthread_mutex_lock(&l->lock);
        if (!l->running) {
            pthread_mutex_unlock(&l->lock);
            break;
        }
        pf[n] = (struct pollfd){l->wake[0], POLLIN, 0}, idx[n++] = -1;
        if (l->udp_fd >= 0) pf[n] = (struct pollfd){l->udp_fd, POLLIN, 0}, idx[n++] = -2;
        if (l->listen_fd >= 0) pf[n] = (struct pollfd){l->listen_fd, POLLIN, 0}, idx[n++] = -3;
        if (l->link.fd >= 0) pf[n] = (struct pollfd){l->link.fd, POLLIN, 0}, idx[n++] = -4;
        for (int i = 0; i < LDN_STATIONS; i++)
            if (l->sta[i].fd >= 0) pf[n] = (struct pollfd){l->sta[i].fd, POLLIN, 0}, idx[n++] = i;
        pthread_mutex_unlock(&l->lock);
        if (poll(pf, n, 250) <= 0) continue;
        int changed = 0;
        pthread_mutex_lock(&l->lock);
        for (unsigned k = 0; k < n && l->running; k++) {
            if (!pf[k].revents) continue;
            if (idx[k] == -1) {
                char b[16];
                while (read(l->wake[0], b, sizeof b) > 0) {}
            } else if (idx[k] == -2 && l->udp_fd >= 0) {
                host_scan(l);
            } else if (idx[k] == -3 && l->listen_fd >= 0) {
                int fd = accept(l->listen_fd, NULL, NULL);
                if (fd < 0) continue;
                int slot = -1;
                for (int i = 0; i < LDN_STATIONS && slot < 0; i++)
                    if (l->sta[i].fd < 0) slot = i;
                if (slot < 0 || nonblock(fd) < 0) {
                    close(fd);
                    continue;
                }
                l->sta[slot].fd = fd, l->sta[slot].len = 0, l->sta[slot].node = 0;
            } else if (idx[k] == -4 && l->link.fd >= 0) {
                int gone = conn_fill(&l->link) < 0;
                uint16_t type;
                uint8_t payload[LDN_FRAME_MAX];
                size_t plen;
                int f;
                while (!gone && (f = next_frame(&l->link, &type, payload, &plen)) != 0) {
                    if (f < 0) {
                        gone = 1;
                        break;
                    }
                    changed |= station_frame_in(l, type, payload, plen, &gone);
                }
                if (gone) {
                    if (!l->reason) l->reason = R_SIGNAL_LOST;
                    log_line("net", "host provider: ldn host gone (reason %u)", l->reason);
                    teardown(l, l->reason);
                    changed = 1;
                }
            } else if (idx[k] >= 0) {
                Conn *c = &l->sta[idx[k]];
                if (c->fd < 0) continue;
                int bad = conn_fill(c) < 0;
                uint16_t type;
                uint8_t payload[LDN_FRAME_MAX];
                size_t plen;
                int f;
                while (!bad && c->fd >= 0 && (f = next_frame(c, &type, payload, &plen)) != 0) {
                    if (f < 0 || (type != P_JOIN && type != P_BYE)) {
                        bad = 1;
                        break;
                    }
                    if (type == P_JOIN) changed |= host_join(l, c, payload, plen);
                    else bad = 1; /* the station leaves */
                }
                if (bad && c->fd >= 0) {
                    int was = c->node;
                    host_drop_station(l, c, 0);
                    if (was) {
                        log_line("net", "host provider: ldn node %d left", was);
                        changed = 1;
                    }
                }
            }
        }
        pthread_mutex_unlock(&l->lock);
        if (changed) notify(l);
    }
    return NULL;
}
static int start_worker(NhLdn *l) {
    if (pipe(l->wake) != 0) return -1;
    fcntl(l->wake[0], F_SETFL, O_NONBLOCK);
    fcntl(l->wake[1], F_SETFL, O_NONBLOCK);
    l->running = 1;
    if (pthread_create(&l->th, NULL, worker, l) != 0) {
        l->running = 0;
        close(l->wake[0]), close(l->wake[1]);
        l->wake[0] = l->wake[1] = -1;
        return -1;
    }
    l->th_live = 1;
    return 0;
}
/* Ends and joins the worker, also one that already stopped by itself (the host went away). Lock NOT held: the worker may be waiting for it. Never
 * called from the worker. */
static void stop_worker(NhLdn *l) {
    pthread_mutex_lock(&l->lock);
    int live = l->th_live;
    l->running = 0;
    l->th_live = 0;
    pthread_mutex_unlock(&l->lock);
    if (!live) return;
    if (write(l->wake[1], "x", 1) < 0) {}
    pthread_join(l->th, NULL);
    close(l->wake[0]), close(l->wake[1]);
    l->wake[0] = l->wake[1] = -1;
}
/* Drops every socket and the network (lock held). The worker stops by itself when running is cleared. */
static void teardown(NhLdn *l, uint16_t reason) {
    if (l->udp_fd >= 0) close(l->udp_fd);
    if (l->listen_fd >= 0) close(l->listen_fd);
    l->udp_fd = l->listen_fd = -1;
    for (int i = 0; i < LDN_STATIONS; i++) conn_close(&l->sta[i]);
    conn_close(&l->link);
    memset(l->nodes, 0, sizeof l->nodes);
    memset(&l->net, 0, sizeof l->net);
    l->role = 0;
    l->reason = reason;
    l->running = 0;
}

/* ---- host ------------------------------------------------------------------------------------------------------------ */
uint32_t nh_ldn_create(void *ctx, const OnlineLdnNetwork *cfg) {
    NhLdn *l = nh_ldn_of(ctx);
    nh_ldn_destroy(ctx);
    nh_ldn_disconnect(ctx);
    int lfd = socket(AF_INET, SOCK_STREAM, 0), ufd = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
    struct sockaddr_in a = {.sin_len = sizeof a, .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    socklen_t al = sizeof a;
    uint16_t base = base_port(), dport = 0;
    if (lfd < 0 || ufd < 0) goto fail;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(lfd, 8) != 0 || getsockname(lfd, (struct sockaddr *)&a, &al) != 0) goto fail;
    uint16_t cport = ntohs(a.sin_port);
    for (uint16_t p = base; p < base + LDN_PORT_SPAN && !dport; p++) {
        struct sockaddr_in u = {.sin_len = sizeof u, .sin_family = AF_INET, .sin_port = htons(p), .sin_addr.s_addr = htonl(INADDR_ANY)};
        if (bind(ufd, (struct sockaddr *)&u, sizeof u) == 0) dport = p;
    }
    if (!dport) {
        log_line("net", "host provider: ldn: no free discovery port in %u..%u", base, base + LDN_PORT_SPAN - 1);
        goto fail;
    }
    nonblock(lfd), nonblock(ufd);
    pthread_mutex_lock(&l->lock);
    l->net = *cfg;
    arc4random_buf(l->net.bssid, 6);
    l->net.bssid[0] = (uint8_t)((l->net.bssid[0] & 0xfe) | 0x02);
    arc4random_buf(l->net.security_param, 16);
    if (!l->net.ssid[0]) {
        uint8_t r[8];
        arc4random_buf(r, sizeof r);
        snprintf(l->net.ssid, sizeof l->net.ssid, "napple-%02x%02x%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    }
    if (!l->net.channel) l->net.channel = 6;
    if (!l->net.node_count_max || l->net.node_count_max > ONLINE_LDN_MAX_NODES) l->net.node_count_max = ONLINE_LDN_MAX_NODES;
    uint32_t ip = self_ip();
    l->net.ipv4 = ip;
    l->net.node_count = 1;
    memset(l->nodes, 0, sizeof l->nodes);
    l->nodes[0].ipv4 = ip;
    fake_mac(l->nodes[0].mac, ip, 0);
    l->nodes[0].connected = 1;
    memcpy(l->nodes[0].user_name, l->net.host_name, sizeof l->nodes[0].user_name);
    l->self = 0;
    l->listen_fd = lfd, l->udp_fd = ufd;
    l->ctl_port = cport, l->disc_port = dport;
    l->role = 1;
    l->reason = R_NONE;
    pthread_mutex_unlock(&l->lock);
    if (start_worker(l) != 0) {
        pthread_mutex_lock(&l->lock);
        teardown(l, R_NONE);
        pthread_mutex_unlock(&l->lock);
        return HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    }
    log_line("net", "host provider: ldn network '%s' hosted (discovery udp %u, control tcp %u)", l->net.ssid, dport, cport);
    return 0;
fail:
    if (lfd >= 0) close(lfd);
    if (ufd >= 0) close(ufd);
    return HZ_R_LDN_NETWORK_NOT_AVAILABLE;
}
void nh_ldn_destroy(void *ctx) {
    NhLdn *l = nh_ldn_of(ctx);
    pthread_mutex_lock(&l->lock);
    int host = l->role == 1;
    if (host)
        for (int i = 0; i < LDN_STATIONS; i++)
            if (l->sta[i].fd >= 0) send_reason(l->sta[i].fd, P_BYE, R_DESTROYED_BY_USER);
    pthread_mutex_unlock(&l->lock);
    stop_worker(l);
    if (!host) return;
    pthread_mutex_lock(&l->lock);
    teardown(l, R_DESTROYED_BY_USER);
    pthread_mutex_unlock(&l->lock);
}
uint32_t nh_ldn_set_advertise(void *ctx, const void *data, uint32_t n) {
    NhLdn *l = nh_ldn_of(ctx);
    if (n > ONLINE_LDN_ADVERTISE_MAX) n = ONLINE_LDN_ADVERTISE_MAX;
    pthread_mutex_lock(&l->lock);
    uint32_t r = l->role == 1 ? 0 : HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    if (!r) {
        if (n) memcpy(l->net.advertise, data, n);
        l->net.advertise_len = (uint16_t)n;
        host_broadcast_nodes(l);
    }
    pthread_mutex_unlock(&l->lock);
    return r;
}
uint32_t nh_ldn_reject(void *ctx, uint32_t ipv4) {
    NhLdn *l = nh_ldn_of(ctx);
    uint32_t r = R_NODE_NOT_FOUND;
    pthread_mutex_lock(&l->lock);
    for (int i = 0; l->role == 1 && i < LDN_STATIONS; i++) {
        Conn *c = &l->sta[i];
        if (c->fd < 0 || !c->node) continue;
        if (l->nodes[c->node].ipv4 == ipv4 || (ipv4 < ONLINE_LDN_MAX_NODES && (uint32_t)c->node == ipv4)) { /* an address, or a node id */
            host_drop_station(l, c, R_REJECTED);
            r = 0;
            break;
        }
    }
    pthread_mutex_unlock(&l->lock);
    if (!r) notify(l);
    return r;
}

/* ---- station --------------------------------------------------------------------------------------------------------- */
static void remember(NhLdn *l, const uint8_t bssid[6], uint32_t ip, uint16_t port) {
    for (unsigned i = 0; i < l->nseen; i++)
        if (!memcmp(l->seen[i].bssid, bssid, 6)) {
            l->seen[i].ip = ip, l->seen[i].port = port;
            return;
        }
    unsigned i = l->nseen < ONLINE_LDN_MAX_NETWORKS ? l->nseen++ : ONLINE_LDN_MAX_NETWORKS - 1;
    memcpy(l->seen[i].bssid, bssid, 6);
    l->seen[i].ip = ip, l->seen[i].port = port;
}
static int scan_wait_ms(void) {
    const char *e = getenv("NAPPLE_LDN_SCAN_MS");
    long v = e ? strtol(e, NULL, 10) : 0;
    return v > 0 && v <= 5000 ? (int)v : 150;
}
static void probe(int fd, uint32_t ip, uint16_t base, const uint8_t *pkt, size_t n) {
    for (uint16_t p = base; p < base + LDN_PORT_SPAN; p++) {
        struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(p), .sin_addr.s_addr = htonl(ip)};
        sendto(fd, pkt, n, 0, (struct sockaddr *)&to, sizeof to); /* failures (no route for broadcast when offline) are expected */
    }
}
int nh_ldn_scan(void *ctx, uint64_t lcid, OnlineLdnNetwork *out, uint32_t max) {
    NhLdn *l = nh_ldn_of(ctx);
    int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    nonblock(fd);
    uint8_t pkt[32];
    W w = {pkt, 0, sizeof pkt, 0};
    hdr(&w, P_SCAN);
    w64(&w, lcid);
    size_t pn = fin(&w);
    uint16_t base = base_port();
    probe(fd, 0x7f000001u, base, pkt, pn);
    probe(fd, 0xffffffffu, base, pkt, pn);
    const char *peers = getenv("NAPPLE_LDN_PEERS");
    if (peers) {
        char buf[512];
        strncpy(buf, peers, sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
        for (char *save = NULL, *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
            struct in_addr a;
            if (inet_pton(AF_INET, t, &a) == 1) probe(fd, ntohl(a.s_addr), base, pkt, pn);
        }
    }
    int got = 0, tok;
    int64_t end = nh_now_ms() + scan_wait_ms();
    nh_core_away(&tok);
    for (int64_t left; (left = end - nh_now_ms()) > 0;) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, (int)left) <= 0) continue;
        uint8_t in[LDN_FRAME_MAX];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t k;
        while ((k = recvfrom(fd, in, sizeof in, 0, (struct sockaddr *)&from, &fl)) > 0) {
            uint32_t magic;
            uint16_t type, len;
            if (k < (ssize_t)LDN_HDR) continue;
            memcpy(&magic, in, 4), memcpy(&type, in + 4, 2), memcpy(&len, in + 6, 2);
            if (magic != LDN_MAGIC || type != P_SCAN_REPLY || LDN_HDR + len > (size_t)k) continue;
            Rd r = {in + LDN_HDR, len, 0, 0};
            uint16_t port = r16(&r);
            OnlineLdnNetwork n;
            get_net(&r, &n);
            if (r.bad || (lcid && n.local_communication_id != lcid)) continue;
            int dup = 0;
            for (int i = 0; i < got && !dup; i++) dup = !memcmp(out[i].bssid, n.bssid, 6);
            if (dup || (uint32_t)got >= max) continue;
            n.ipv4 = ntohl(from.sin_addr.s_addr); /* where the host answered from: the address a station reaches it at */
            out[got++] = n;
            pthread_mutex_lock(&l->lock);
            remember(l, n.bssid, n.ipv4, port);
            pthread_mutex_unlock(&l->lock);
            fl = sizeof from;
        }
    }
    nh_core_back(&tok);
    close(fd);
    return got;
}
static int find_seen(NhLdn *l, const uint8_t bssid[6], uint32_t *ip, uint16_t *port) {
    pthread_mutex_lock(&l->lock);
    int found = 0;
    for (unsigned i = 0; i < l->nseen && !found; i++)
        if (!memcmp(l->seen[i].bssid, bssid, 6)) *ip = l->seen[i].ip, *port = l->seen[i].port, found = 1;
    pthread_mutex_unlock(&l->lock);
    return found;
}
uint32_t nh_ldn_connect(void *ctx, const OnlineLdnNetwork *net) {
    NhLdn *l = nh_ldn_of(ctx);
    nh_ldn_destroy(ctx);
    nh_ldn_disconnect(ctx);
    uint32_t ip;
    uint16_t port;
    if (!find_seen(l, net->bssid, &ip, &port)) { /* a NetworkInfo the guest kept from an earlier run: look again */
        OnlineLdnNetwork tmp[ONLINE_LDN_MAX_NETWORKS];
        nh_ldn_scan(ctx, net->local_communication_id, tmp, ONLINE_LDN_MAX_NETWORKS);
        if (!find_seen(l, net->bssid, &ip, &port)) return HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0), tok;
    if (fd < 0 || nonblock(fd) < 0) {
        if (fd >= 0) close(fd);
        return HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    }
    struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip)};
    uint32_t result = HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    Conn c = {.fd = fd};
    nh_core_away(&tok);
    int ok = connect(fd, (struct sockaddr *)&to, sizeof to) == 0 || errno == EINPROGRESS;
    if (ok) {
        struct pollfd pf = {fd, POLLOUT, 0};
        int so = 0;
        socklen_t sl = sizeof so;
        ok = poll(&pf, 1, LDN_JOIN_MS) > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) == 0 && !so;
    }
    if (ok) {
        uint8_t buf[64];
        W w = {buf, 0, sizeof buf, 0};
        hdr(&w, P_JOIN);
        wstr(&w, ""); /* the station's own user name is not handed to the provider: its node shows an empty name */
        w16(&w, net->local_communication_version);
        w64(&w, net->local_communication_id);
        size_t n = fin(&w);
        ok = n && send_all(fd, buf, n) == 0;
    }
    int accepted = 0;
    int64_t end = nh_now_ms() + LDN_JOIN_MS;
    while (ok && !accepted) {
        int64_t left = end - nh_now_ms();
        struct pollfd pf = {fd, POLLIN, 0};
        if (left <= 0 || poll(&pf, 1, (int)left) <= 0 || conn_fill(&c) < 0) break;
        uint16_t type;
        uint8_t payload[LDN_FRAME_MAX];
        size_t plen;
        int f = next_frame(&c, &type, payload, &plen);
        if (f < 0) break;
        if (f == 0) continue;
        pthread_mutex_lock(&l->lock);
        int gone = 0;
        if (type == P_ACCEPT && station_frame_in(l, type, payload, plen, &gone) && !gone) {
            accepted = 1;
            l->role = 2;
            l->reason = R_NONE;
            l->link = c;
        } else if (type == P_REJECT) {
            station_frame_in(l, type, payload, plen, &gone);
            ok = 0;
        }
        pthread_mutex_unlock(&l->lock);
    }
    nh_core_back(&tok);
    if (!accepted) {
        close(fd);
        return result;
    }
    if (start_worker(l) != 0) {
        pthread_mutex_lock(&l->lock);
        teardown(l, R_SIGNAL_LOST);
        pthread_mutex_unlock(&l->lock);
        return result;
    }
    log_line("net", "host provider: ldn joined '%s' as node %u", l->net.ssid, l->self);
    return 0;
}
void nh_ldn_disconnect(void *ctx) {
    NhLdn *l = nh_ldn_of(ctx);
    pthread_mutex_lock(&l->lock);
    int station = l->role == 2;
    if (station && l->link.fd >= 0) send_reason(l->link.fd, P_BYE, R_USER);
    pthread_mutex_unlock(&l->lock);
    stop_worker(l);
    if (!station) return;
    pthread_mutex_lock(&l->lock);
    teardown(l, R_USER);
    pthread_mutex_unlock(&l->lock);
}

/* ---- queries --------------------------------------------------------------------------------------------------------- */
uint32_t nh_ldn_get_network(void *ctx, OnlineLdnNetwork *out, uint16_t *reason) {
    NhLdn *l = nh_ldn_of(ctx);
    pthread_mutex_lock(&l->lock);
    uint32_t r = l->role ? 0 : HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    memset(out, 0, sizeof *out);
    if (!r) {
        *out = l->net;
        out->node_count = (uint8_t)connected_count(l);
        out->ipv4 = l->nodes[l->self].ipv4; /* this node's own address */
    }
    if (reason) *reason = l->reason;
    pthread_mutex_unlock(&l->lock);
    return r;
}
int nh_ldn_get_nodes(void *ctx, OnlineLdnNode *out, uint32_t max, uint32_t *self) {
    NhLdn *l = nh_ldn_of(ctx);
    pthread_mutex_lock(&l->lock);
    int n = -1;
    if (l->role) {
        n = (int)(max < ONLINE_LDN_MAX_NODES ? max : ONLINE_LDN_MAX_NODES);
        memcpy(out, l->nodes, sizeof *out * (size_t)n);
        if (self) *self = l->self;
    }
    pthread_mutex_unlock(&l->lock);
    return n;
}
