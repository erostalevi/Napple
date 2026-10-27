/* Test-only OnlineProvider: an in-memory loopback network. Stream sockets can listen, connect (to a listener on the same address), accept and
 * exchange bytes; connecting to an address nobody listens on fails with ECONNREFUSED. DNS knows "echo.test" and "v6.test"; TLS is a pass-through
 * (the handshake succeeds, bytes are copied unchanged); the friend list has two entries. Proves that a provider can serve a bsd echo connection and
 * that the services route through the table. It is not shipped and contains no real service code. */
#pragma once
#include "../src/services/online_provider.h"
#include "../src/services/hz_result.h"
#include <string.h>
#include <stdlib.h>

#define LB_MAX 32
#define LB_BUF 4096
typedef struct {
    int used, type, listening, bound, peer, nonblock, closed_peer;
    OnlineAddr addr;
    uint8_t buf[LB_BUF];
    size_t len;
    int backlog[8], nback;
} LbSock;
typedef struct {
    LbSock s[LB_MAX];
    int connected, reports, presence_updates;
    uint32_t net_connects, net_releases;
    char last_host[64];
    /* ldn: one hosted network that a station in the same process can scan and join; acc: one linked account */
    int ldn_hosting, ldn_joined, ldn_adv_updates;
    OnlineLdnNetwork ldn_net;
    uint16_t ldn_reason;
    uint32_t ldn_rejected_ip;
    int account_linked;
} Lb;
static Lb lb;

static LbSock *lbs(int h) { return h >= 0 && h < LB_MAX && lb.s[h].used ? &lb.s[h] : NULL; }
static int lb_open(void *c, int d, int t, int p, int *e) {
    (void)c; (void)d; (void)p;
    for (int i = 0; i < LB_MAX; i++)
        if (!lb.s[i].used) { memset(&lb.s[i], 0, sizeof lb.s[i]); lb.s[i].used = 1; lb.s[i].type = t; lb.s[i].peer = -1; *e = 0; return i; }
    *e = HZ_EMFILE; return -1;
}
static int lb_close(void *c, int h, int *e) {
    (void)c; LbSock *s = lbs(h);
    if (!s) { *e = HZ_EBADF; return -1; }
    if (s->peer >= 0 && lbs(s->peer)) lbs(s->peer)->closed_peer = 1;
    memset(s, 0, sizeof *s); return 0;
}
static int lb_bind(void *c, int h, const OnlineAddr *a, int *e) { (void)c; LbSock *s = lbs(h); if (!s) { *e = HZ_EBADF; return -1; } s->addr = *a; s->bound = 1; return 0; }
static int lb_listen(void *c, int h, int b, int *e) { (void)c; (void)b; LbSock *s = lbs(h); if (!s || !s->bound) { *e = s ? HZ_EINVAL : HZ_EBADF; return -1; } s->listening = 1; return 0; }
static int lb_connect(void *c, int h, const OnlineAddr *a, int *e) {
    (void)c; LbSock *s = lbs(h);
    if (!s) { *e = HZ_EBADF; return -1; }
    for (int i = 0; i < LB_MAX; i++) {
        LbSock *l = &lb.s[i];
        if (l->used && l->listening && l->addr.port == a->port && l->nback < 8) {
            int srv = lb_open(NULL, 2, 1, 0, e);
            if (srv < 0) return -1;
            lb.s[srv].peer = h; s->peer = srv; lb.s[srv].addr = l->addr;
            l->backlog[l->nback++] = srv;
            return 0;
        }
    }
    *e = HZ_ECONNREFUSED; return -1;
}
static int lb_accept(void *c, int h, OnlineAddr *peer, int *e) {
    (void)c; LbSock *l = lbs(h);
    if (!l || !l->listening) { *e = l ? HZ_EINVAL : HZ_EBADF; return -1; }
    if (!l->nback) { *e = HZ_EAGAIN; return -1; }
    int s = l->backlog[0]; memmove(l->backlog, l->backlog + 1, sizeof(int) * (size_t)--l->nback);
    if (peer) { memset(peer, 0, sizeof *peer); peer->family = 2; peer->port = 49152; }
    return s;
}
static int64_t lb_send(void *c, int h, const void *b, size_t n, int fl, const OnlineAddr *to, int *e) {
    (void)c; (void)fl; (void)to; LbSock *s = lbs(h);
    if (!s) { *e = HZ_EBADF; return -1; }
    LbSock *p = lbs(s->peer);
    if (!p) { *e = HZ_ENOTCONN; return -1; }
    if (n > LB_BUF - p->len) n = LB_BUF - p->len;
    memcpy(p->buf + p->len, b, n); p->len += n; return (int64_t)n;
}
static int64_t lb_recv(void *c, int h, void *b, size_t n, int fl, OnlineAddr *from, int *e) {
    (void)c; (void)fl; (void)from; LbSock *s = lbs(h);
    if (!s) { *e = HZ_EBADF; return -1; }
    if (!s->len) { if (s->closed_peer) return 0; *e = s->peer >= 0 ? HZ_EAGAIN : HZ_ENOTCONN; return -1; }
    if (n > s->len) n = s->len;
    memcpy(b, s->buf, n); memmove(s->buf, s->buf + n, s->len - n); s->len -= n; return (int64_t)n;
}
static int lb_shutdown(void *c, int h, int how, int *e) { (void)c; (void)how; if (!lbs(h)) { *e = HZ_EBADF; return -1; } return 0; }
static int lb_getsockname(void *c, int h, OnlineAddr *o, int *e) { (void)c; LbSock *s = lbs(h); if (!s) { *e = HZ_EBADF; return -1; } *o = s->addr; if (!o->family) o->family = 2; return 0; }
static int lb_getpeername(void *c, int h, OnlineAddr *o, int *e) {
    (void)c; LbSock *s = lbs(h);
    if (!s) { *e = HZ_EBADF; return -1; }
    if (!lbs(s->peer)) { *e = HZ_ENOTCONN; return -1; }
    *o = lbs(s->peer)->addr; if (!o->family) o->family = 2; return 0;
}
static int lb_setsockopt(void *c, int h, int lv, int op, const void *v, size_t n, int *e) { (void)c; (void)lv; (void)op; (void)v; (void)n; if (!lbs(h)) { *e = HZ_EBADF; return -1; } return 0; }
static int lb_getsockopt(void *c, int h, int lv, int op, void *v, size_t *n, int *e) {
    (void)c; (void)lv; (void)op; if (!lbs(h)) { *e = HZ_EBADF; return -1; }
    if (*n >= 4) { memset(v, 0, 4); *n = 4; if (op == 0x1007 /* SO_ERROR */) *(int32_t *)v = 0; } return 0;
}
static int lb_nonblock(void *c, int h, int on, int *e) { (void)c; LbSock *s = lbs(h); if (!s) { *e = HZ_EBADF; return -1; } s->nonblock = on; return 0; }
static int lb_poll(void *c, OnlinePollFd *f, unsigned n, int to, int *e) {
    (void)c; (void)to; (void)e; int ready = 0;
    for (unsigned i = 0; i < n; i++) {
        LbSock *s = lbs(f[i].fd); f[i].revents = 0;
        if (!s) { f[i].revents = ONLINE_POLLNVAL; ready++; continue; }
        if ((f[i].events & ONLINE_POLLIN) && (s->len || s->nback || s->closed_peer)) f[i].revents |= ONLINE_POLLIN;
        if ((f[i].events & ONLINE_POLLOUT) && (s->peer >= 0)) f[i].revents |= ONLINE_POLLOUT;
        if (f[i].revents) ready++;
    }
    return ready;
}
static int lb_dns(void *c, const char *host, const char *svc, int fam, OnlineResolved *o) {
    (void)c; (void)svc; memset(o, 0, sizeof *o);
    snprintf(lb.last_host, sizeof lb.last_host, "%s", host);
    if (!strcmp(host, "echo.test") && fam != 28) { o->count = 1; o->addrs[0].family = 2; o->addrs[0].addr[0] = 10; o->addrs[0].addr[3] = 1; strcpy(o->canonname, "echo.test"); return 0; }
    if (!strcmp(host, "v6.test") && fam != 2) { o->count = 1; o->addrs[0].family = 28; o->addrs[0].addr[15] = 1; return 0; }
    return HZ_EAI_NONAME;
}
static int lb_reverse(void *c, const OnlineAddr *a, char *h, size_t n) {
    (void)c; if (a->family == 2 && a->addr[0] == 10 && a->addr[3] == 1) { snprintf(h, n, "echo.test"); return 0; } return HZ_EAI_NONAME;
}
typedef struct { int sock; int established; } LbTls;
static void *lb_tls_new(void *c, int s) { (void)c; LbTls *t = calloc(1, sizeof *t); t->sock = s; return t; }
static void lb_tls_free(void *c, void *t) { (void)c; free(t); }
static int lb_tls_host(void *c, void *t, const char *h) { (void)c; (void)t; snprintf(lb.last_host, sizeof lb.last_host, "%s", h); return 0; }
static int lb_tls_hs(void *c, void *t, uint32_t *res) { (void)c; if (!lbs(((LbTls *)t)->sock) || lbs(((LbTls *)t)->sock)->peer < 0) { *res = HZ_RESULT(HZ_MOD_SSL, 205); return -1; } ((LbTls *)t)->established = 1; return 0; }
static int64_t lb_tls_read(void *c, void *t, void *b, size_t n, int *e) { return lb_recv(c, ((LbTls *)t)->sock, b, n, 0, NULL, e); }
static int64_t lb_tls_write(void *c, void *t, const void *b, size_t n, int *e) { return lb_send(c, ((LbTls *)t)->sock, b, n, 0, NULL, e); }
static int64_t lb_tls_pending(void *c, void *t) { (void)c; LbSock *s = lbs(((LbTls *)t)->sock); return s ? (int64_t)s->len : 0; }
static uint32_t lb_fcount(void *c) { (void)c; return 2; }
static uint32_t lb_flist(void *c, uint32_t off, OnlineFriend *o, uint32_t max) {
    (void)c; uint32_t n = 0;
    for (uint32_t i = off; i < 2 && n < max; i++, n++) { memset(&o[n], 0, sizeof o[n]); o[n].id = 0x1000 + i; snprintf(o[n].name, sizeof o[n].name, "friend%u", i); o[n].presence = 1; }
    return n;
}
static void lb_presence(void *c, const void *r, size_t n) { (void)c; (void)r; (void)n; lb.presence_updates++; }
static void lb_report(void *c, const void *r, size_t n) { (void)c; (void)r; (void)n; lb.reports++; }
static int lb_net_info(void *c, OnlineNetInfo *o) { (void)c; memset(o, 0, sizeof *o); o->connected = 1; o->kind = 1; o->strength = 3; o->ip = 0x0a000002; o->mask = 0xffffff00; o->gateway = 0x0a000001; return 0; }
static uint32_t lb_net_connect(void *c) { (void)c; lb.net_connects++; return 0; }
static void lb_net_release(void *c) { (void)c; lb.net_releases++; }
static int lb_nsd(void *c, const char *in, char *out, size_t n) { (void)c; snprintf(out, n, "mapped.%s", in); return 0; }

static int lb_ldn_scan(void *c, uint64_t lcid, OnlineLdnNetwork *o, uint32_t max) {
    (void)c;
    if (!max) return 0;
    if (!lb.ldn_hosting || (lcid && lb.ldn_net.local_communication_id != lcid)) return 0;
    o[0] = lb.ldn_net; return 1;
}
static uint32_t lb_ldn_create(void *c, const OnlineLdnNetwork *cfg) {
    (void)c;
    if (lb.ldn_hosting) return HZ_RESULT(HZ_MOD_LDN, 32);
    lb.ldn_net = *cfg; lb.ldn_hosting = 1; lb.ldn_net.node_count = 1; lb.ldn_net.ipv4 = 0xc0a80001;
    snprintf(lb.ldn_net.ssid, sizeof lb.ldn_net.ssid, "loopback-ldn"); memset(lb.ldn_net.bssid, 0xaa, 6);
    lb.ldn_net.security_param[0] = 0x5a; lb.ldn_net.channel = lb.ldn_net.channel ? lb.ldn_net.channel : 6; return 0;
}
static void lb_ldn_destroy(void *c) { (void)c; lb.ldn_hosting = 0; lb.ldn_reason = 3; }
static uint32_t lb_ldn_connect(void *c, const OnlineLdnNetwork *n) {
    (void)c;
    if (!lb.ldn_hosting || strcmp(n->ssid, lb.ldn_net.ssid) || n->local_communication_id != lb.ldn_net.local_communication_id) return HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    lb.ldn_joined = 1; lb.ldn_net.node_count = 2; return 0;
}
static void lb_ldn_disconnect(void *c) { (void)c; lb.ldn_joined = 0; lb.ldn_reason = 7; }
static uint32_t lb_ldn_get(void *c, OnlineLdnNetwork *o, uint16_t *reason) {
    (void)c; *reason = lb.ldn_reason;
    if (!lb.ldn_hosting) return HZ_R_LDN_NETWORK_NOT_AVAILABLE;
    *o = lb.ldn_net; return 0;
}
static uint32_t lb_ldn_adv(void *c, const void *d, uint32_t n) {
    (void)c; if (n > ONLINE_LDN_ADVERTISE_MAX) return HZ_RESULT(HZ_MOD_LDN, 2);
    memcpy(lb.ldn_net.advertise, d, n); lb.ldn_net.advertise_len = (uint16_t)n; lb.ldn_adv_updates++; return 0;
}
static uint32_t lb_ldn_reject(void *c, uint32_t ip) { (void)c; if (ip != 0xc0a80002) return HZ_RESULT(HZ_MOD_LDN, 24); lb.ldn_rejected_ip = ip; lb.ldn_joined = 0; return 0; }
static int lb_account(void *c, const uint8_t u[16], OnlineAccountInfo *o) {
    (void)c; (void)u; if (!lb.account_linked) return 1;
    memset(o, 0, sizeof *o); o->linked = 1; o->id = 0x1234abcd5678ef90ull; memset(o->resource_cache, 0x77, sizeof o->resource_cache); return 0;
}

static const OnlineProvider lb_provider = {
    .abi_version = ONLINE_ABI_VERSION, .struct_size = sizeof(OnlineProvider), .name = "loopback (test)",
    .caps = ONLINE_CAP_NETWORK | ONLINE_CAP_SOCKETS | ONLINE_CAP_DNS | ONLINE_CAP_TLS | ONLINE_CAP_FRIENDS | ONLINE_CAP_REPORTS | ONLINE_CAP_NSD | ONLINE_CAP_LDN | ONLINE_CAP_ACCOUNT,
    .net_info = lb_net_info, .net_connect = lb_net_connect, .net_release = lb_net_release,
    .sock_open = lb_open, .sock_close = lb_close, .sock_bind = lb_bind, .sock_listen = lb_listen, .sock_accept = lb_accept, .sock_connect = lb_connect,
    .sock_send = lb_send, .sock_recv = lb_recv, .sock_shutdown = lb_shutdown, .sock_getsockname = lb_getsockname, .sock_getpeername = lb_getpeername,
    .sock_setsockopt = lb_setsockopt, .sock_getsockopt = lb_getsockopt, .sock_set_nonblock = lb_nonblock, .sock_poll = lb_poll,
    .dns_resolve = lb_dns, .dns_reverse = lb_reverse,
    .tls_new = lb_tls_new, .tls_free = lb_tls_free, .tls_set_host = lb_tls_host, .tls_handshake = lb_tls_hs, .tls_read = lb_tls_read, .tls_write = lb_tls_write, .tls_pending = lb_tls_pending,
    .friend_count = lb_fcount, .friend_list = lb_flist, .presence_update = lb_presence, .report = lb_report, .nsd_resolve = lb_nsd,
    .ldn_scan = lb_ldn_scan, .ldn_create = lb_ldn_create, .ldn_destroy = lb_ldn_destroy, .ldn_connect = lb_ldn_connect, .ldn_disconnect = lb_ldn_disconnect,
    .ldn_get_network = lb_ldn_get, .ldn_set_advertise = lb_ldn_adv, .ldn_reject = lb_ldn_reject, .account_info = lb_account,
};
