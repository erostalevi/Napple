/* Host provider (NAPPLE_NET=host), against this Mac's own stack on localhost: provider selection, bsd sockets directly and through the bsd:u handlers
 * (TCP echo, UDP, non-blocking connect + poll, timeouts, a Close that ends a blocked Recv), DNS through the host resolver (a public name is skipped
 * when the Mac is offline) and nifm connectivity. */
#include "../src/services/offline.c"
#include "svc_test.h"
#include "../src/net/net_host.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>

static int32_t ri(const IpcCall *c, int i) { return SVC_OUT(c, int32_t, i * 4); }
typedef struct { IpcCall c; int32_t in[16]; } Call;
static IpcCall *bs(IpcObject *o, unsigned cmd, int32_t fd, int32_t a, int32_t b, const void *in0, uint64_t n0, void *out0, uint64_t m0, void *out1, uint64_t m1) {
    static Call k; memset(&k, 0, sizeof k);
    k.in[0] = fd; k.in[1] = a; k.in[2] = b; k.c.in = (const uint8_t *)k.in; k.c.in_size = 12;
    if (in0) svc_set_in_buf(&k.c, in0, n0);
    if (out0) svc_set_out_buf(&k.c, out0, m0);
    if (out1) svc_set_out_buf(&k.c, out1, m1);
    svc_call_c(o, cmd, &k.c); return &k.c;
}
static void guest_sin(uint8_t out[16], uint16_t port, uint32_t ip) { /* the guest's FreeBSD sockaddr_in; ip in host order */
    memset(out, 0, 16); out[0] = 16; out[1] = 2; out[2] = (uint8_t)(port >> 8); out[3] = (uint8_t)port;
    out[4] = (uint8_t)(ip >> 24); out[5] = (uint8_t)(ip >> 16); out[6] = (uint8_t)(ip >> 8); out[7] = (uint8_t)ip;
}
static OnlineAddr lo(uint16_t port) { OnlineAddr a = {.family = 2, .port = port}; a.addr[0] = 127; a.addr[3] = 1; return a; }

typedef struct { const OnlineProvider *p; int h; int64_t r; int err; } Blocked;
static void *blocked_recv(void *arg) {
    Blocked *b = arg; char buf[16];
    b->r = b->p->sock_recv(b->p->ctx, b->h, buf, sizeof buf, 0, NULL, &b->err);
    return NULL;
}

int main(void) {
    (void)svc_call;
    /* ---- selection: unset / offline / unknown keep the NullProvider; host picks the built-in one ---- */
    unsetenv("NAPPLE_ONLINE_PROVIDER");
    unsetenv("NAPPLE_NET"); online_provider_reset(); CHECK(online_provider() == online_null_provider(), "NAPPLE_NET unset: offline (NullProvider)");
    setenv("NAPPLE_NET", "offline", 1); online_provider_reset(); CHECK(online_provider() == online_null_provider(), "NAPPLE_NET=offline: NullProvider");
    setenv("NAPPLE_NET", "bogus", 1); online_provider_reset(); CHECK(online_provider() == online_null_provider(), "NAPPLE_NET=bogus: NullProvider");
    setenv("NAPPLE_NET", "host", 1); setenv("NAPPLE_ONLINE_PROVIDER", "/nonexistent/provider.dylib", 1); online_provider_reset();
    CHECK(online_provider() == online_null_provider(), "an explicit NAPPLE_ONLINE_PROVIDER wins over NAPPLE_NET, even when it fails to load");
    unsetenv("NAPPLE_ONLINE_PROVIDER"); online_provider_reset();
    const OnlineProvider *p = online_provider();
    CHECK(p == net_host_provider() && !strcmp(p->name, NET_HOST_NAME), "NAPPLE_NET=host: the built-in host provider");
    CHECK(online_has(p, ONLINE_CAP_SOCKETS) && online_has(p, ONLINE_CAP_DNS) && online_has(p, ONLINE_CAP_TLS) && online_has(p, ONLINE_CAP_NETWORK) &&
          online_has(p, ONLINE_CAP_LDN) && !online_has(p, ONLINE_CAP_FRIENDS) && !online_has(p, ONLINE_CAP_ACCOUNT) && !online_has(p, ONLINE_CAP_REPORTS),
          "caps: network, sockets, dns, tls, ldn; no friends, accounts or reports");
    CHECK(!ONLINE_FN(p, friend_list) && !ONLINE_FN(p, nsd_resolve) && !ONLINE_FN(p, account_info) && !ONLINE_FN(p, report), "online-service entry points stay empty");
    void *c0 = p->ctx; int err = 0;

    /* ---- TCP echo, provider level ---- */
    int ls = p->sock_open(c0, 2, 1, 0, &err); CHECK(ls >= 0, "socket(AF_INET, SOCK_STREAM)");
    OnlineAddr any = lo(0); CHECK(p->sock_bind(c0, ls, &any, &err) == 0, "bind 127.0.0.1:0");
    OnlineAddr me; CHECK(p->sock_getsockname(c0, ls, &me, &err) == 0 && me.family == 2 && me.port != 0 && me.addr[0] == 127, "getsockname: an ephemeral port");
    CHECK(p->sock_listen(c0, ls, 4, &err) == 0, "listen");
    int cs = p->sock_open(c0, 2, 1, 0, &err);
    CHECK(p->sock_connect(c0, cs, &me, &err) == 0, "blocking connect to the listener");
    OnlineAddr peer; int as = p->sock_accept(c0, ls, &peer, &err); CHECK(as >= 0 && peer.family == 2 && peer.addr[0] == 127, "accept returns the peer");
    CHECK(p->sock_send(c0, cs, "ping", 4, 0, NULL, &err) == 4, "send 4 bytes");
    char buf[64] = {0}; CHECK(p->sock_recv(c0, as, buf, sizeof buf, 0, NULL, &err) == 4 && !memcmp(buf, "ping", 4), "recv them on the accepted socket");
    CHECK(p->sock_send(c0, as, "pong!", 5, 0, NULL, &err) == 5 && p->sock_recv(c0, cs, buf, 5, 0x40, NULL, &err) == 5 && !memcmp(buf, "pong!", 5), "echo back (MSG_WAITALL)");
    OnlineAddr pn; CHECK(p->sock_getpeername(c0, cs, &pn, &err) == 0 && pn.port == me.port, "getpeername");
    int one = 1; CHECK(p->sock_setsockopt(c0, cs, 6, 1, &one, 4, &err) == 0, "TCP_NODELAY passes through");
    int v = 0; size_t vn = 4; CHECK(p->sock_getsockopt(c0, cs, 6, 1, &v, &vn, &err) == 0 && v != 0, "TCP_NODELAY reads back");
    v = 0; vn = 4; CHECK(p->sock_getsockopt(c0, cs, 0xffff, 0x1008, &v, &vn, &err) == 0 && v == 1, "SO_TYPE = SOCK_STREAM");
    CHECK(p->sock_setsockopt(c0, cs, 0xffff, 0x0800, &one, 4, &err) == 0, "FreeBSD SO_NOSIGPIPE accepted (always on here)");
    CHECK(p->sock_setsockopt(c0, cs, 0xffff, 0x7777, &one, 4, &err) == 0, "an option the host lacks is accepted and ignored");
    /* non-blocking recv with nothing queued, then a receive timeout */
    CHECK(p->sock_set_nonblock(c0, cs, 1, &err) == 0 && p->sock_recv(c0, cs, buf, 8, 0, NULL, &err) == -1 && err == HZ_EAGAIN, "non-blocking recv: EAGAIN");
    p->sock_set_nonblock(c0, cs, 0, &err);
    CHECK(p->sock_recv(c0, cs, buf, 8, 0x80, NULL, &err) == -1 && err == HZ_EAGAIN, "MSG_DONTWAIT: EAGAIN");
    int64_t tv[2] = {0, 150000}; CHECK(p->sock_setsockopt(c0, cs, 0xffff, 0x1006, tv, 16, &err) == 0, "SO_RCVTIMEO 150 ms");
    int64_t tv2[2] = {9, 9}; size_t tn = 16; CHECK(p->sock_getsockopt(c0, cs, 0xffff, 0x1006, tv2, &tn, &err) == 0 && tv2[0] == 0 && tv2[1] == 150000, "SO_RCVTIMEO reads back");
    int64_t t0 = nh_now_ms(); CHECK(p->sock_recv(c0, cs, buf, 8, 0, NULL, &err) == -1 && err == HZ_EAGAIN, "blocking recv times out with EAGAIN");
    int64_t dt = nh_now_ms() - t0; CHECK(dt >= 140 && dt < 1000, "after about the timeout (%lld ms)", (long long)dt);
    /* a Close from another thread ends a blocked recv */
    Blocked bl = {p, as, 0, 0}; pthread_t th; pthread_create(&th, NULL, blocked_recv, &bl);
    usleep(100000); CHECK(p->sock_close(c0, as, &err) == 0, "close the socket another thread waits on");
    pthread_join(th, NULL); CHECK(bl.r == -1 && bl.err == HZ_EBADF, "the blocked recv returns EBADF");
    CHECK(p->sock_recv(c0, cs, buf, 8, 0, NULL, &err) == 0, "the peer sees the close: recv 0 (EOF)");
    CHECK(p->sock_close(c0, as, &err) == -1 && err == HZ_EBADF, "closing twice: EBADF");
    /* connection refused, then a non-blocking connect completed through poll */
    int rs = p->sock_open(c0, 2, 1, 0, &err); OnlineAddr dead = lo(me.port); p->sock_close(c0, ls, &err);
    CHECK(p->sock_connect(c0, rs, &dead, &err) == -1 && err == HZ_ECONNREFUSED, "connect to a closed port: ECONNREFUSED");
    p->sock_close(c0, rs, &err); p->sock_close(c0, cs, &err);
    ls = p->sock_open(c0, 2, 1, 0, &err); p->sock_bind(c0, ls, &any, &err); p->sock_listen(c0, ls, 4, &err); p->sock_getsockname(c0, ls, &me, &err);
    int ns = p->sock_open(c0, 2, 0x20000000 | 1, 0, &err); CHECK(ns >= 0, "SOCK_STREAM | SOCK_NONBLOCK");
    int cr = p->sock_connect(c0, ns, &me, &err); CHECK(cr == 0 || (cr == -1 && err == HZ_EINPROGRESS), "non-blocking connect: 0 or EINPROGRESS");
    OnlinePollFd pf = {ns, ONLINE_POLLOUT, 0}; CHECK(p->sock_poll(c0, &pf, 1, 2000, &err) == 1 && (pf.revents & ONLINE_POLLOUT), "poll: writable once connected");
    v = -1; vn = 4; CHECK(p->sock_getsockopt(c0, ns, 0xffff, 0x1007, &v, &vn, &err) == 0 && v == 0, "SO_ERROR = 0");
    OnlinePollFd lp = {ls, ONLINE_POLLIN, 0}; CHECK(p->sock_poll(c0, &lp, 1, 2000, &err) == 1 && (lp.revents & ONLINE_POLLIN), "poll: the listener is readable");
    as = p->sock_accept(c0, ls, NULL, &err); CHECK(as >= 0, "accept");
    OnlinePollFd rp[2] = {{as, ONLINE_POLLIN, 0}, {-1, ONLINE_POLLIN, 0}}; t0 = nh_now_ms();
    CHECK(p->sock_poll(c0, rp, 2, 120, &err) == 0 && nh_now_ms() - t0 >= 100, "poll with nothing to read times out");
    OnlinePollFd bad = {400, ONLINE_POLLIN, 0}; CHECK(p->sock_poll(c0, &bad, 1, 0, &err) == 1 && bad.revents == ONLINE_POLLNVAL, "poll on an unknown handle: POLLNVAL");
    p->sock_close(c0, as, &err); p->sock_close(c0, ns, &err); p->sock_close(c0, ls, &err);

    /* ---- UDP ---- */
    int u1 = p->sock_open(c0, 2, 2, 0, &err), u2 = p->sock_open(c0, 2, 2, 0, &err);
    p->sock_bind(c0, u1, &any, &err); OnlineAddr ua; p->sock_getsockname(c0, u1, &ua, &err);
    CHECK(p->sock_send(c0, u2, "dgram", 5, 0, &ua, &err) == 5, "sendto a UDP socket");
    OnlineAddr from; CHECK(p->sock_recv(c0, u1, buf, sizeof buf, 0, &from, &err) == 5 && !memcmp(buf, "dgram", 5) && from.family == 2 && from.port != 0, "recvfrom gets it with the sender's address");
    CHECK(p->sock_send(c0, u1, "back", 4, 0, &from, &err) == 4 && p->sock_recv(c0, u2, buf, sizeof buf, 0, NULL, &err) == 4, "reply to the sender");
    CHECK(p->sock_send(c0, u2, "x", 1, 0, NULL, &err) == -1 && err == HZ_EDESTADDRREQ, "send on an unconnected UDP socket: EDESTADDRREQ");
    p->sock_close(c0, u1, &err); p->sock_close(c0, u2, &err);
    CHECK(p->sock_open(c0, 77, 1, 0, &err) == -1 && err == HZ_EAFNOSUPPORT, "unknown family: EAFNOSUPPORT");

    /* ---- the same through the bsd:u handlers (guest descriptors, guest sockaddr) ---- */
    IpcObject *b0 = bsd_open(); bsdg_reset(bsd_g());
    IpcCall *r = bs(b0, 2, 2, 1, 0, NULL, 0, NULL, 0, NULL, 0); int lfd = ri(r, 0); CHECK(lfd == 0 && ri(r, 1) == 0, "bsd Socket -> fd 0");
    uint8_t sa[16]; guest_sin(sa, 0, 0x7f000001); r = bs(b0, 13, lfd, 0, 0, sa, 16, NULL, 0, NULL, 0); CHECK(ri(r, 0) == 0, "bsd Bind 127.0.0.1:0");
    uint8_t got[16] = {0}; r = bs(b0, 16, lfd, 0, 0, NULL, 0, got, 16, NULL, 0); uint16_t port = (uint16_t)(got[2] << 8 | got[3]);
    CHECK(ri(r, 0) == 0 && ri(r, 2) == 16 && got[1] == 2 && port != 0 && got[4] == 127, "bsd GetSockName: guest sockaddr_in with the port");
    r = bs(b0, 18, lfd, 4, 0, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r, 0) == 0, "bsd Listen");
    r = bs(b0, 2, 2, 1, 0, NULL, 0, NULL, 0, NULL, 0); int cfd = ri(r, 0);
    guest_sin(sa, port, 0x7f000001); r = bs(b0, 14, cfd, 0, 0, sa, 16, NULL, 0, NULL, 0); CHECK(ri(r, 0) == 0 && ri(r, 1) == 0, "bsd Connect to it (was ENETDOWN offline)");
    uint8_t pa[16]; r = bs(b0, 12, lfd, 0, 0, NULL, 0, pa, 16, NULL, 0); int afd = ri(r, 0); CHECK(afd >= 0 && ri(r, 2) == 16 && pa[4] == 127, "bsd Accept with the peer address");
    r = bs(b0, 10, cfd, 0, 0, "hello guest", 11, NULL, 0, NULL, 0); CHECK(ri(r, 0) == 11, "bsd Send");
    char rb[32] = {0}; r = bs(b0, 8, afd, 0, 0, NULL, 0, rb, sizeof rb, NULL, 0); CHECK(ri(r, 0) == 11 && !memcmp(rb, "hello guest", 11), "bsd Recv");
    uint8_t pfd[8]; int32_t f32 = cfd; int16_t ev = ONLINE_POLLOUT; memcpy(pfd, &f32, 4); memcpy(pfd + 4, &ev, 2); memset(pfd + 6, 0, 2);
    uint8_t pres[8] = {0}; { static Call k; memset(&k, 0, sizeof k); k.in[0] = 1; k.in[1] = 500; k.c.in = (const uint8_t *)k.in; k.c.in_size = 8;
        svc_set_in_buf(&k.c, pfd, 8); svc_set_out_buf(&k.c, pres, 8); svc_call_c(b0, 6, &k.c); int16_t re; memcpy(&re, pres + 6, 2);
        CHECK(ri(&k.c, 0) == 1 && (re & ONLINE_POLLOUT), "bsd Poll: the connected socket is writable"); }
    r = bs(b0, 20, afd, 4, 4, NULL, 0, NULL, 0, NULL, 0); r = bs(b0, 8, afd, 0, 0, NULL, 0, rb, sizeof rb, NULL, 0); CHECK(ri(r, 0) == -1 && ri(r, 1) == HZ_EAGAIN, "bsd Fcntl(O_NONBLOCK) then Recv: EAGAIN");
    r = bs(b0, 26, cfd, 0, 0, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r, 0) == 0, "bsd Close");
    r = bs(b0, 23, 0, 0, 0, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r, 0) == 0, "bsd ShutdownAllSockets");
    r = bs(b0, 26, lfd, 0, 0, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r, 1) == HZ_EBADF, "every descriptor is gone afterwards");

    /* ---- DNS ---- */
    OnlineResolved res; CHECK(p->dns_resolve(c0, "localhost", NULL, 2, &res) == 0 && res.count >= 1 && res.addrs[0].family == 2 && res.addrs[0].addr[0] == 127, "resolve localhost (IPv4)");
    CHECK(p->dns_resolve(c0, "localhost", "80", 0, &res) == 0 && res.addrs[0].port == 80, "resolve localhost with a service: the port");
    CHECK(p->dns_resolve(c0, "napple-test.invalid", NULL, 0, &res) == HZ_EAI_NONAME, "an .invalid name: EAI_NONAME");
    OnlineAddr l1 = lo(0); char hn[256] = {0}; CHECK(p->dns_reverse(c0, &l1, hn, sizeof hn) == 0 && hn[0], "reverse 127.0.0.1 -> '%s'", hn);
    OnlineNetInfo ni; CHECK(p->net_info(c0, &ni) == 0, "nifm net_info");
    int rc = p->dns_resolve(c0, "example.com", NULL, 2, &res);
    if (ni.connected && rc == 0) CHECK(res.count >= 1 && res.addrs[0].family == 2, "resolve a public name (example.com) through the host resolver");
    else printf("note: public DNS check skipped (Mac connected %d, result %d)\n", ni.connected, rc);

    /* ---- nifm ---- */
    if (ni.connected) {
        CHECK(ni.ip != 0 && ni.kind == 2 && ni.mask != 0, "nifm: connected, wired-style, with an address and mask (%u.%u.%u.%u)", ni.ip >> 24, ni.ip >> 16 & 255, ni.ip >> 8 & 255, ni.ip & 255);
        CHECK(p->net_connect(c0) == 0, "net_connect accepted");
        IpcCall nc; memset(&nc, 0, sizeof nc); nifm_ip(NULL, &nc); CHECK(SVC_OUT(&nc, uint32_t, 0) == ni.ip, "nifm GetCurrentIpAddress = the host's address");
        memset(&nc, 0, sizeof nc); ret_conn_status(NULL, &nc); CHECK(nc.out[0] == 2 && nc.out[2] == 4, "nifm connection status: wired, connected");
        NifmRequest rq; nifm_request_init(&rq); nifm_request_submit(&rq); CHECK(rq.state == NIFM_STATE_ACCEPTED && nifm_request_result(&rq) == 0, "a nifm request is accepted");
        nifm_request_cancel(&rq);
    } else {
        printf("note: this Mac has no primary IPv4 interface: nifm connectivity checks skipped\n");
        CHECK(p->net_connect(c0) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "offline Mac: net_connect refused");
    }

    /* ---- back to offline: the previous answers ---- */
    unsetenv("NAPPLE_NET"); online_provider_reset(); bsdg_reset(bsd_g()); online_null_reset();
    r = bs(b0, 2, 2, 1, 0, NULL, 0, NULL, 0, NULL, 0); r = bs(b0, 14, 0, 0, 0, sa, 16, NULL, 0, NULL, 0); CHECK(ri(r, 0) == -1 && ri(r, 1) == HZ_ENETDOWN, "variable unset again: Connect is ENETDOWN as before");
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
