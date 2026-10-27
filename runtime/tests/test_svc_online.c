/* Unit tests: the OnlineProvider boundary. Every network service routes through the active provider: the NullProvider answers are the offline ones
 * (known calls of the titles are pinned below), and a test-only loopback provider serves a bsd echo connection, DNS, nifm, TLS, friends, nsd and prepo. */
#include "../src/services/offline.c"
#include "../src/services/sfdnsres.c"
#include "svc_test.h"
#include "online_loopback.h"
#include <unistd.h>

static int32_t ri(const IpcCall *c, int i) { return SVC_OUT(c, int32_t, i * 4); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void sin_addr(uint8_t out[16], uint16_t port, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    memset(out, 0, 16); out[0] = 16; out[1] = 2; out[2] = (uint8_t)(port >> 8); out[3] = (uint8_t)port; out[4] = a; out[5] = b; out[6] = c; out[7] = d;
}
/* one bsd call: args = {fd, a, b}, optional input buffers and output buffers */
typedef struct { IpcCall c; int32_t in[16]; } Call;
static IpcCall *bs(IpcObject *o, unsigned cmd, int32_t fd, int32_t a, int32_t b, const void *in0, uint64_t n0, const void *in1, uint64_t n1, void *out0, uint64_t m0, void *out1, uint64_t m1) {
    static Call k; memset(&k, 0, sizeof k);
    k.in[0] = fd; k.in[1] = a; k.in[2] = b; k.c.in = (const uint8_t *)k.in; k.c.in_size = 12;
    if (in0) svc_set_in_buf(&k.c, in0, n0);
    if (in1) svc_set_in_buf(&k.c, in1, n1);
    if (out0) svc_set_out_buf(&k.c, out0, m0);
    if (out1) svc_set_out_buf(&k.c, out1, m1);
    svc_call_c(o, cmd, &k.c); return &k.c;
}
#define BS(o, cmd, fd, a, b) bs(o, cmd, fd, a, b, NULL, 0, NULL, 0, NULL, 0, NULL, 0)

int main(void) {
    /* ---- address codec ---- */
    OnlineAddr a; uint8_t sa[28];
    sin_addr(sa, 8080, 10, 1, 2, 3);
    CHECK(bsd_sockaddr_decode(sa, 16, &a) == 0 && a.family == 2 && a.port == 8080 && a.addr[0] == 10 && a.addr[3] == 3, "decode sockaddr_in");
    uint8_t enc[28]; CHECK(bsd_sockaddr_encode(&a, enc, sizeof enc) == 16 && !memcmp(enc, sa, 16), "encode sockaddr_in round-trips");
    OnlineAddr v6 = {.family = 28, .port = 443}; v6.addr[15] = 1; CHECK(bsd_sockaddr_encode(&v6, enc, sizeof enc) == 28 && enc[0] == 28 && enc[1] == 28 && enc[2] == 0x01 && enc[3] == 0xbb && enc[23] == 1, "encode sockaddr_in6");
    OnlineAddr back; CHECK(bsd_sockaddr_decode(enc, 28, &back) == 0 && back.family == 28 && back.port == 443 && back.addr[15] == 1, "decode sockaddr_in6");
    CHECK(bsd_sockaddr_decode(sa, 4, &a) == -1 && bsd_sockaddr_decode(NULL, 16, &a) == -1, "short or missing sockaddr rejected");
    sa[1] = 9; CHECK(bsd_sockaddr_decode(sa, 16, &a) == -1, "unsupported family rejected");

    /* ---- provider table validation ---- */
    OnlineProvider bad = lb_provider; bad.abi_version = 2; CHECK(online_provider_check(&bad) == -1, "wrong ABI version rejected");
    bad = lb_provider; bad.struct_size = 8; CHECK(online_provider_check(&bad) == -2, "truncated table rejected");
    CHECK(online_provider_check(NULL) == -1, "NULL table rejected");
    OnlineProvider small = lb_provider; small.struct_size = offsetof(OnlineProvider, sock_open); CHECK(ONLINE_FN(&small, sock_open) == NULL && ONLINE_FN(&small, net_info) != NULL, "ONLINE_FN hides fields past struct_size");
    CHECK(online_provider() == online_null_provider(), "no provider installed: NullProvider");
    CHECK(online_has(online_null_provider(), ONLINE_CAP_SOCKETS) == 0 && online_null_provider()->caps == 0, "NullProvider has no capabilities");

    /* ---- the offline answers (NullProvider) of the calls the titles make, pinned ---- */
    IpcCall c; IpcObject *b0 = bsd_open(); bsdg_reset(bsd_g()); online_null_reset();
    CHECK(BS(b0, 2, 2, 1, 0)->out_size == 8, "Socket -> 8 bytes"); IpcCall *r = BS(b0, 2, 2, 1, 0); CHECK(ri(r, 0) == 1 && ri(r, 1) == 0, "second socket is fd 1");
    r = BS(b0, 14, 0, 0, 0); CHECK(ri(r, 0) == -1 && ri(r, 1) == HZ_ENETDOWN, "Connect: ENETDOWN");
    r = BS(b0, 6, 0, 0, 0); CHECK(ri(r, 0) == 0 && ri(r, 1) == 0, "Poll without buffers: timeout");
    r = BS(b0, 5, 0, 0, 0); CHECK(ri(r, 0) == 0 && ri(r, 1) == 0, "Select without sets: timeout");
    r = BS(b0, 17, 0, 1, 4); CHECK(ri(r, 0) == 0, "GetSockOpt: ok"); r = BS(b0, 21, 0, 1, 4); CHECK(ri(r, 0) == 0, "SetSockOpt: ok");
    r = BS(b0, 20, 0, 4, 4); CHECK(ri(r, 0) == 0, "Fcntl(F_SETFL): ok"); r = BS(b0, 20, 0, 3, 0); CHECK(ri(r, 0) == 4, "Fcntl(F_GETFL) returns O_NONBLOCK once set");
    r = BS(b0, 27, 0, 0, 0); CHECK(ri(r, 0) == 2 && ri(r, 1) == 0, "DuplicateSocket: the lowest free descriptor (0 and 1 are open)");
    r = BS(b0, 27, 77, 0, 0); CHECK(ri(r, 0) == -1 && ri(r, 1) == HZ_EBADF, "DuplicateSocket of an unknown descriptor: EBADF");
    r = BS(b0, 20, 2, 3, 0); CHECK(ri(r, 0) == 4, "the duplicate shares the non-blocking flag");
    r = BS(b0, 26, 0, 0, 0); CHECK(ri(r, 0) == 0, "Close the original"); r = BS(b0, 20, 2, 3, 0); CHECK(ri(r, 0) == 4, "the duplicate outlives the original");
    r = BS(b0, 26, 2, 0, 0); CHECK(ri(r, 0) == 0, "Close the duplicate"); r = BS(b0, 26, 0, 0, 0); CHECK(ri(r, 1) == HZ_EBADF, "Close again: EBADF");
    bsdg_reset(bsd_g());
    { /* nifm */
        IpcObject *nf = nifm_open(); svc_call(nf, 5, NULL, 0, &c); IpcObject *gen = c.out_objects[0];
        svc_call(gen, 4, NULL, 0, &c); IpcObject *rq = c.out_objects[0];
        svc_call(rq, 4, NULL, 0, &c); svc_call(rq, 1, NULL, 0, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "NullProvider: Submit rejected, communication disabled");
        svc_call(rq, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == NIFM_STATE_FREE, "state stays Free");
        svc_call(gen, 12, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "no IP address");
        svc_call(gen, 21, NULL, 0, &c); CHECK(c.out[0] == 0, "no accepted request");
        svc_call(gen, 16, NULL, 0, &c); CHECK(c.out[0] == 0, "wireless not enabled");
    }
    { /* dns */
        IpcObject *rs = res_open(); char host[] = "example.invalid", out[256];
        memset(&c, 0, sizeof c); svc_set_in_buf(&c, host, sizeof host); svc_set_out_buf(&c, out, sizeof out); svc_call_c(rs, 2, &c);
        CHECK(SVC_OUT(&c, uint32_t, 0) == HZ_NETDB_TRY_AGAIN && SVC_OUT(&c, uint32_t, 8) == 0, "NullProvider: GetHostByName TRY_AGAIN");
        memset(&c, 0, sizeof c); svc_set_in_buf(&c, host, sizeof host); svc_set_in_buf(&c, "443", 4); svc_set_out_buf(&c, out, sizeof out); svc_call_c(rs, 6, &c);
        CHECK(SVC_OUT(&c, int32_t, 0) == HZ_EAI_AGAIN && SVC_OUT(&c, uint32_t, 8) == 0, "NullProvider: GetAddrInfo EAI_AGAIN");
    }
    { /* ssl, friend, nsd, prepo under the NullProvider */
        IpcObject *ss = ssl_open(); svc_call(ss, 0, NULL, 0, &c); IpcObject *ctx = c.out_objects[0]; svc_call(ctx, 2, NULL, 0, &c); IpcObject *cn = c.out_objects[0];
        svc_call(cn, 8, NULL, 0, &c); CHECK(c.result == HZ_R_SSL_NOT_CONNECTED, "NullProvider: DoHandshake not connected");
        IpcObject *fr = friend_open(); svc_call(fr, 0, NULL, 0, &c); IpcObject *fs = c.out_objects[0];
        svc_call(fs, 20100, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "no friends"); svc_call(fs, 10101, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "empty friend list");
        IpcObject *nd = nsd_open(); char in[] = "name.example", out[64] = {0};
        memset(&c, 0, sizeof c); svc_set_in_buf(&c, in, sizeof in); svc_set_out_buf(&c, out, sizeof out); svc_call_c(nd, 20, &c);
        CHECK(!strcmp(out, "name.example") && SVC_OUT(&c, uint32_t, 0) == 0, "NullProvider: nsd resolves a name to itself");
        IpcObject *pp = prepo_open(); char rep[] = "report"; memset(&c, 0, sizeof c); svc_set_in_buf(&c, rep, sizeof rep); svc_call_c(pp, 10100, &c); CHECK(c.result == 0, "reports are accepted and dropped");
    }

    /* ---- loopback provider: bsd echo ---- */
    memset(&lb, 0, sizeof lb); online_provider_install(&lb_provider); bsdg_reset(bsd_g());
    CHECK(online_provider() == &lb_provider && online_has(online_provider(), ONLINE_CAP_SOCKETS), "provider installed");
    IpcObject *bsd = bsd_open();
    r = BS(bsd, 2, 2, 1, 6); int srv = ri(r, 0); CHECK(srv == 0 && ri(r, 1) == 0, "Socket (server) through the provider");
    r = BS(bsd, 2, 2, 1, 6); int cli = ri(r, 0); CHECK(cli == 1, "Socket (client)");
    uint8_t laddr[16]; sin_addr(laddr, 7000, 127, 0, 0, 1);
    r = bs(bsd, 13, srv, 0, 0, laddr, 16, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r, 0) == 0, "Bind");
    r = BS(bsd, 18, srv, 4, 0); CHECK(ri(r, 0) == 0, "Listen");
    r = bs(bsd, 12, srv, 0, 0, NULL, 0, NULL, 0, enc, sizeof enc, NULL, 0); CHECK(ri(r, 0) == -1 && ri(r, 1) == HZ_EAGAIN, "Accept with nobody connecting: EAGAIN");
    uint8_t bad_addr[16]; sin_addr(bad_addr, 9999, 127, 0, 0, 1);
    r = bs(bsd, 14, cli, 0, 0, bad_addr, 16, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r, 0) == -1 && ri(r, 1) == HZ_ECONNREFUSED, "Connect to a port nobody listens on: ECONNREFUSED");
    r = bs(bsd, 14, cli, 0, 0, laddr, 16, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r, 0) == 0 && ri(r, 1) == 0, "Connect to the listener");
    { uint32_t pf[2] = {(uint32_t)srv, 0}; ((int16_t *)&pf[1])[0] = ONLINE_POLLIN; uint8_t out[8];
      uint32_t nfds = 1; int32_t args[2]; args[0] = (int32_t)nfds; args[1] = 0; Call k; memset(&k, 0, sizeof k); memcpy(k.in, args, 8); k.c.in = (const uint8_t *)k.in; k.c.in_size = 8;
      svc_set_in_buf(&k.c, pf, 8); svc_set_out_buf(&k.c, out, 8); svc_call_c(bsd, 6, &k.c);
      CHECK(ri(&k.c, 0) == 1 && ri(&k.c, 1) == 0 && (out[6] & ONLINE_POLLIN), "Poll: the listener is readable after a connect"); }
    uint8_t peer[28]; r = bs(bsd, 12, srv, 0, 0, NULL, 0, NULL, 0, peer, sizeof peer, NULL, 0); int conn = ri(r, 0);
    CHECK(conn == 2 && ri(r, 2) == 16 && peer[1] == 2, "Accept returns a new descriptor and the peer address (length 16)");
    char msg[] = "hello over the loopback";
    r = bs(bsd, 10, cli, 0, 0, msg, sizeof msg, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r, 0) == (int)sizeof msg, "Send");
    char rx[64] = {0}; r = bs(bsd, 8, conn, 0, 0, NULL, 0, NULL, 0, rx, sizeof rx, NULL, 0); CHECK(ri(r, 0) == (int)sizeof msg && !strcmp(rx, msg), "Recv returns the bytes sent");
    r = bs(bsd, 10, conn, 0, 0, rx, (uint64_t)strlen(rx) + 1, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r, 0) == (int)sizeof msg, "Echo back");
    char rx2[64] = {0}; r = bs(bsd, 9, cli, 0, 0, NULL, 0, NULL, 0, rx2, sizeof rx2, peer, sizeof peer); CHECK(ri(r, 0) == (int)sizeof msg && !strcmp(rx2, msg), "RecvFrom gets the echo");
    r = bs(bsd, 8, cli, 0, 0, NULL, 0, NULL, 0, rx2, sizeof rx2, NULL, 0); CHECK(ri(r, 0) == -1 && ri(r, 1) == HZ_EAGAIN, "Recv with no data: EAGAIN");
    r = bs(bsd, 16, srv, 0, 0, NULL, 0, NULL, 0, enc, sizeof enc, NULL, 0); CHECK(ri(r, 0) == 0 && ri(r, 2) == 16 && enc[2] == 0x1b && enc[3] == 0x58, "GetSockName returns the bound port 7000");
    r = bs(bsd, 15, cli, 0, 0, NULL, 0, NULL, 0, enc, sizeof enc, NULL, 0); CHECK(ri(r, 0) == 0 && enc[2] == 0x1b, "GetPeerName on the client");
    r = bs(bsd, 17, cli, 0xffff, 0x1007, NULL, 0, NULL, 0, rx2, 4, NULL, 0); CHECK(ri(r, 0) == 0 && ri(r, 2) == 4, "GetSockOpt(SO_ERROR) returns 4 bytes");
    r = BS(bsd, 20, cli, 4, 4); CHECK(ri(r, 0) == 0 && lb.s[1].nonblock == 1, "Fcntl(F_SETFL, O_NONBLOCK) reaches the provider");
    { int32_t on = 0; int32_t args[3] = {cli, (int32_t)0x8004667e, 1}; Call k; memset(&k, 0, sizeof k); memcpy(k.in, args, 12); k.c.in = (const uint8_t *)k.in; k.c.in_size = 12; svc_set_in_buf(&k.c, &on, 4); svc_call_c(bsd, 19, &k.c);
      CHECK(ri(&k.c, 0) == 0 && lb.s[1].nonblock == 0, "Ioctl(FIONBIO, 0) clears it"); }
    { uint8_t rd[16] = {0}, wr[16] = {0}; rd[0] = (uint8_t)(1 << conn); wr[0] = (uint8_t)(1 << cli); uint8_t ord[16], owr[16];
      int32_t args[2] = {conn + 1, 0}; int64_t tv[2] = {0, 0}; Call k; memset(&k, 0, sizeof k); memcpy(k.in, args, 8); memcpy((uint8_t *)k.in + 8, tv, 16); k.c.in = (const uint8_t *)k.in; k.c.in_size = 24;
      uint8_t ex[16] = {0}; svc_set_in_buf(&k.c, rd, 16); svc_set_in_buf(&k.c, wr, 16); svc_set_in_buf(&k.c, ex, 16); svc_set_out_buf(&k.c, ord, 16); svc_set_out_buf(&k.c, owr, 16);
      svc_call_c(bsd, 5, &k.c); CHECK(ri(&k.c, 0) == 1 && ri(&k.c, 1) == 0 && (owr[0] & (1 << cli)) && !(ord[0] & (1 << conn)), "Select: client writable, accepted socket has nothing to read"); }
    r = BS(bsd, 26, cli, 0, 0); CHECK(ri(r, 0) == 0, "Close the client");
    r = bs(bsd, 8, conn, 0, 0, NULL, 0, NULL, 0, rx2, sizeof rx2, NULL, 0); CHECK(ri(r, 0) == 0 && ri(r, 1) == 0, "Recv after the peer closed: end of stream (0)");
    r = BS(bsd, 26, cli, 0, 0); CHECK(ri(r, 0) == -1 && ri(r, 1) == HZ_EBADF, "Close of a closed descriptor: EBADF");
    r = BS(bsd, 14, 99, 0, 0); CHECK(ri(r, 1) == HZ_EBADF, "unknown descriptor: EBADF");
    svc_call(bsd, 23, NULL, 0, &c); CHECK(c.result == 0, "ShutdownAllSockets"); r = BS(bsd, 26, srv, 0, 0); CHECK(ri(r, 1) == HZ_EBADF && !lb.s[0].used, "everything closed through the provider");
    { int last = 0, err = 0; BsdGuest *g = bsd_g(); for (int i = 0; i < 40; i++) { int fd = bsdg_socket(g, 2, 1, 6, &err); if (fd < 0) break; last = i + 1; }
      CHECK(last == LB_MAX && err == HZ_EMFILE, "provider socket limit surfaces as EMFILE"); bsdg_reset(g); }

    /* ---- DNS through the provider ---- */
    IpcObject *rs = res_open(); char outb[512]; memset(&c, 0, sizeof c);
    { char host[] = "echo.test", svc[] = "80"; uint8_t hints[24]; memset(hints, 0, sizeof hints); hints[0] = 0xBE; hints[1] = 0xEF; hints[2] = 0xCA; hints[3] = 0xFE; hints[15] = 1; /* socktype stream */
      svc_set_in_buf(&c, host, sizeof host); svc_set_in_buf(&c, svc, sizeof svc); svc_set_in_buf(&c, hints, sizeof hints); svc_set_out_buf(&c, outb, sizeof outb); svc_call_c(rs, 6, &c);
      uint32_t n = SVC_OUT(&c, uint32_t, 8); CHECK(SVC_OUT(&c, int32_t, 0) == 0 && n > 24 + 16, "GetAddrInfo resolves through the provider");
      CHECK(be32((uint8_t *)outb) == SFDNS_ADDRINFO_MAGIC && be32((uint8_t *)outb + 8) == 2 && be32((uint8_t *)outb + 12) == 1 && be32((uint8_t *)outb + 20) == 16, "entry: magic, AF_INET, SOCK_STREAM, 16-byte sockaddr");
      CHECK(outb[25] == 2 && ((uint8_t)outb[26] << 8 | (uint8_t)outb[27]) == 80 && (uint8_t)outb[28] == 10 && (uint8_t)outb[31] == 1, "sockaddr: family, port from the service string, address 10.0.0.1");
      CHECK(!strcmp(outb + 24 + 16, "echo.test") && be32((uint8_t *)outb + n - 4) == 0, "canonical name and list terminator"); }
    { char host[] = "echo.test"; memset(&c, 0, sizeof c); svc_set_in_buf(&c, host, sizeof host); svc_set_in_buf(&c, "", 1); svc_set_out_buf(&c, outb, sizeof outb); svc_call_c(rs, 6, &c);
      uint32_t n = SVC_OUT(&c, uint32_t, 8); int entries = 0; for (uint32_t o = 0; o + 24 < n && be32((uint8_t *)outb + o) == SFDNS_ADDRINFO_MAGIC; ) { entries++; o += 24 + 16 + (uint32_t)strlen(outb + o + 24 + 16) + 1; }
      CHECK(entries == 2, "no hints: one stream and one datagram entry per address"); }
    { char host[] = "nonexistent.test"; memset(&c, 0, sizeof c); svc_set_in_buf(&c, host, sizeof host); svc_set_in_buf(&c, "", 1); svc_set_out_buf(&c, outb, sizeof outb); svc_call_c(rs, 6, &c);
      CHECK(SVC_OUT(&c, int32_t, 0) == HZ_EAI_NONAME && SVC_OUT(&c, uint32_t, 8) == 0, "unknown host: EAI_NONAME, no data"); }
    { char host[] = "echo.test"; memset(&c, 0, sizeof c); svc_set_in_buf(&c, host, sizeof host); svc_set_out_buf(&c, outb, sizeof outb); svc_call_c(rs, 2, &c);
      uint32_t n = SVC_OUT(&c, uint32_t, 8); CHECK(SVC_OUT(&c, uint32_t, 0) == 0 && n == 10 + 4 + 2 + 2 + 4 + 4, "GetHostByName hostent size");
      const uint8_t *h = (const uint8_t *)outb; CHECK(!strcmp(outb, "echo.test") && h[14 + 1] == 2 && h[14 + 3] == 4 && h[14 + 7] == 1 && h[22] == 10, "hostent: name, AF_INET, 4-byte address, count 1, 10.x"); }
    { char host[] = "nonexistent.test"; memset(&c, 0, sizeof c); svc_set_in_buf(&c, host, sizeof host); svc_set_out_buf(&c, outb, sizeof outb); svc_call_c(rs, 2, &c);
      CHECK(SVC_OUT(&c, uint32_t, 0) == HZ_NETDB_HOST_NOT_FOUND, "hostent: unknown host maps to HOST_NOT_FOUND"); }
    { uint8_t ad[16]; sin_addr(ad, 0, 10, 0, 0, 1); char hostn[64], svcn[16]; memset(&c, 0, sizeof c); sin_addr(ad, 8080, 10, 0, 0, 1);
      svc_set_in_buf(&c, ad, 16); svc_set_out_buf(&c, hostn, sizeof hostn); svc_set_out_buf(&c, svcn, sizeof svcn); svc_call_c(rs, 7, &c);
      CHECK(SVC_OUT(&c, int32_t, 0) == 0 && !strcmp(hostn, "echo.test") && !strcmp(svcn, "8080"), "GetNameInfo: reverse lookup and the port as a string"); }
    CHECK(sfdns_port_from_service("8080") == 8080 && sfdns_port_from_service("http") == 0 && sfdns_port_from_service("99999") == 0 && sfdns_port_from_service(NULL) == 0, "service string to port");
    { OnlineResolved rr; memset(&rr, 0, sizeof rr); rr.count = 1; rr.addrs[0].family = 2; uint8_t tiny[10]; CHECK(sfdns_serialize_addrinfo(&rr, 0, 1, 6, tiny, sizeof tiny) == 0 && sfdns_serialize_hostent("x", &rr, tiny, 4) == 0, "serialisers refuse an output buffer that is too small"); }
    { int f, fa, st, pr; uint8_t hb[20] = {0xBE, 0xEF, 0xCA, 0xFE, 0, 0, 0, 5, 0, 0, 0, 28, 0, 0, 0, 2, 0, 0, 0, 17}; CHECK(sfdns_parse_hints(hb, 20, &f, &fa, &st, &pr) == 0 && f == 5 && fa == 28 && st == 2 && pr == 17, "hints parsed"); CHECK(sfdns_parse_hints(hb, 8, &f, &fa, &st, &pr) == -1 && f == 0, "short hints default to zero"); }

    /* ---- nifm ---- */
    { IpcObject *nf = nifm_open(); svc_call(nf, 5, NULL, 0, &c); IpcObject *gen = c.out_objects[0]; svc_call(gen, 4, NULL, 0, &c); IpcObject *rq = c.out_objects[0];
      svc_call(rq, 2, NULL, 0, &c); uint32_t ev0 = c.out_copy[0]; CHECK(!event_is_signaled(ev0), "request event clear before Submit");
      svc_call(rq, 4, NULL, 0, &c); CHECK(c.result == 0 && lb.net_connects == 1 && event_is_signaled(ev0), "Submit asks the provider to connect and signals");
      svc_call(rq, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == NIFM_STATE_ACCEPTED, "request is Accepted"); svc_call(rq, 1, NULL, 0, &c); CHECK(c.result == 0, "GetResult: success");
      svc_call(gen, 21, NULL, 0, &c); CHECK(c.out[0] == 1, "IsAnyInternetRequestAccepted"); svc_call(gen, 12, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0x0a000002, "GetCurrentIpAddress from the provider");
      svc_call(gen, 15, NULL, 0, &c); CHECK(c.out_size == 4 && c.out[0] == 1 && c.out[1] == 3 && c.out[2] == 4, "connection status: wireless, strength 3, connected");
      svc_call(gen, 16, NULL, 0, &c); CHECK(c.out[0] == 1, "wireless communication enabled");
      svc_call(rq, 3, NULL, 0, &c); CHECK(lb.net_releases == 1, "Cancel releases the connection"); svc_call(gen, 21, NULL, 0, &c); CHECK(c.out[0] == 0, "no accepted request after Cancel");
      svc_call(rq, 4, NULL, 0, &c); svc_call(rq, 4, NULL, 0, &c); CHECK(lb.net_connects == 3 && lb.net_releases == 2, "a second Submit replaces the first connection");
      svc_call(gen, 4, NULL, 0, &c); IpcObject *rq2 = c.out_objects[0]; svc_call(rq2, 4, NULL, 0, &c); svc_call(gen, 21, NULL, 0, &c); CHECK(c.out[0] == 1, "two requests accepted"); nifm_req_destroy(rq2); free(rq2); /* IpcObject is heap-allocated by the mock */
    }

    /* ---- ssl over the provider ---- */
    { IpcObject *bsd2 = bsd_open(); bsdg_reset(bsd_g()); memset(&lb, 0, sizeof lb);
      r = BS(bsd2, 2, 2, 1, 6); int s1 = ri(r, 0); r = BS(bsd2, 2, 2, 1, 6); int s2 = ri(r, 0);
      r = bs(bsd2, 13, s2, 0, 0, laddr, 16, NULL, 0, NULL, 0, NULL, 0); r = BS(bsd2, 18, s2, 1, 0);
      IpcObject *ss = ssl_open(); svc_call(ss, 0, NULL, 0, &c); IpcObject *ctx = c.out_objects[0]; svc_call(ctx, 2, NULL, 0, &c); IpcObject *cn = c.out_objects[0];
      svc_call(cn, 0, &s1, 4, &c); char host[] = "tls.test"; memset(&c, 0, sizeof c); svc_set_in_buf(&c, host, sizeof host); svc_call_c(cn, 1, &c);
      svc_call(cn, 8, NULL, 0, &c); CHECK(c.result == HZ_R_SSL_NOT_CONNECTED || c.result == HZ_RESULT(HZ_MOD_SSL, 205), "handshake on an unconnected socket fails with the provider's result");
      r = bs(bsd2, 14, s1, 0, 0, laddr, 16, NULL, 0, NULL, 0, NULL, 0); int acc = ri(bs(bsd2, 12, s2, 0, 0, NULL, 0, NULL, 0, enc, sizeof enc, NULL, 0), 0);
      svc_call(cn, 8, NULL, 0, &c); CHECK(c.result == 0 && !strcmp(lb.last_host, "tls.test"), "DoHandshake succeeds over a connected socket; the host name reached the provider");
      char tx[] = "secret", rxb[16] = {0}; memset(&c, 0, sizeof c); svc_set_in_buf(&c, tx, sizeof tx); svc_call_c(cn, 11, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == sizeof tx, "Write");
      r = bs(bsd2, 8, acc, 0, 0, NULL, 0, NULL, 0, rxb, sizeof rxb, NULL, 0); CHECK(ri(r, 0) == (int)sizeof tx && !strcmp(rxb, tx), "the peer receives the bytes written through TLS");
      r = bs(bsd2, 10, acc, 0, 0, "abc", 4, NULL, 0, NULL, 0, NULL, 0); svc_call(cn, 12, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 4, "Pending");
      memset(rxb, 0, sizeof rxb); memset(&c, 0, sizeof c); svc_set_out_buf(&c, rxb, sizeof rxb); svc_call_c(cn, 10, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 4 && !strcmp(rxb, "abc"), "Read");
      svc_call(cn, 4, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == s1, "GetSocketDescriptor");
      (void)s2; bsdg_reset(bsd_g()); }

    /* ---- friend, nsd, prepo ---- */
    { memset(&lb, 0, sizeof lb); IpcObject *fr = friend_open(); svc_call(fr, 0, NULL, 0, &c); IpcObject *fs = c.out_objects[0];
      svc_call(fs, 20100, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 2, "friend count from the provider");
      uint64_t ids[4] = {0}; uint32_t off = 0; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&off; c.in_size = 4; svc_set_out_buf(&c, ids, sizeof ids); svc_call_c(fs, 10100, &c);
      CHECK(SVC_OUT(&c, uint32_t, 0) == 2 && ids[0] == 0x1000 && ids[1] == 0x1001, "GetFriendListIds");
      static uint8_t recs[2 * 0x300]; off = 1; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&off; c.in_size = 4; svc_set_out_buf(&c, recs, sizeof recs); svc_call_c(fs, 10101, &c);
      CHECK(SVC_OUT(&c, uint32_t, 0) == 1 && !strcmp((char *)recs + 0x40, "friend1"), "GetFriendList honours the offset");
      char pres[] = "presence"; memset(&c, 0, sizeof c); svc_set_in_buf(&c, pres, sizeof pres); svc_call_c(fs, 10610, &c); CHECK(lb.presence_updates == 1, "UpdateUserPresence reaches the provider");
      IpcObject *nd = nsd_open(); char in[] = "x.example", out[64] = {0}; memset(&c, 0, sizeof c); svc_set_in_buf(&c, in, sizeof in); svc_set_out_buf(&c, out, sizeof out); svc_call_c(nd, 21, &c);
      CHECK(!strcmp(out, "mapped.x.example"), "nsd Resolve goes through the provider");
      IpcObject *pp = prepo_open(); char rep[] = "rep"; memset(&c, 0, sizeof c); svc_set_in_buf(&c, rep, sizeof rep); svc_call_c(pp, 10105, &c); CHECK(lb.reports == 1, "a play report reaches the provider"); }

    /* ---- plugin boundary: build three tiny modules with the host compiler and load them through the environment ---- */
    { char dir[] = "/tmp/napple-online-plugin-XXXXXX"; char *d = mkdtemp(dir); char src[300], so[300], cmd[1800], inc[700], here[300];
      snprintf(here, sizeof here, "%s", __FILE__); char *sl = strrchr(here, '/'); if (sl) *sl = 0; else strcpy(here, ".");
      snprintf(inc, sizeof inc, "%s/../src/services", here);
      static const char *const body[3] = {
          "#include \"online_provider.h\"\nstatic int ni(void *c, OnlineNetInfo *o) { (void)c; o->connected = 1; o->kind = 2; o->ip = 0x01020304; return 0; }\n"
          "static const OnlineProvider p = {.abi_version = ONLINE_ABI_VERSION, .struct_size = sizeof(OnlineProvider), .name = \"plugin-test\", .caps = ONLINE_CAP_NETWORK, .net_info = ni};\n"
          "const OnlineProvider *napple_online_provider_v1(void) { return &p; }\n",
          "#include \"online_provider.h\"\nstatic const OnlineProvider p = {.abi_version = 99, .struct_size = sizeof(OnlineProvider), .name = \"bad-abi\"};\n"
          "const OnlineProvider *napple_online_provider_v1(void) { return &p; }\n",
          "int unrelated(void) { return 1; }\n"};
      const char *tag[3] = {"good", "badabi", "nosym"};
      int built = d != NULL;
      for (int i = 0; i < 3 && built; i++) {
          snprintf(src, sizeof src, "%s/%s.c", d, tag[i]); snprintf(so, sizeof so, "%s/%s.so", d, tag[i]);
          FILE *f = fopen(src, "w"); fputs(body[i], f); fclose(f);
          snprintf(cmd, sizeof cmd, "cc -shared -fPIC -I%s -o %s %s >/dev/null 2>&1", inc, so, src);
          built = system(cmd) == 0;
      }
      if (!built) printf("note: no host compiler for the plugin test, skipped\n");
      else {
          char path[300];
          snprintf(path, sizeof path, "%s/good.so", d); setenv("NAPPLE_ONLINE_PROVIDER", path, 1); online_provider_reset();
          const OnlineProvider *p = online_provider(); CHECK(p != online_null_provider() && !strcmp(p->name, "plugin-test") && online_has(p, ONLINE_CAP_NETWORK), "a provider module is loaded from NAPPLE_ONLINE_PROVIDER");
          OnlineNetInfo ni; CHECK(p->net_info(p->ctx, &ni) == 0 && ni.connected && ni.ip == 0x01020304, "and its entry points are called");
          IpcObject *nf = nifm_open(); svc_call(nf, 5, NULL, 0, &c); IpcObject *gen = c.out_objects[0]; svc_call(gen, 12, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0x01020304, "nifm reports the module's address");
          CHECK(ONLINE_FN(p, sock_open) == NULL, "entry points the module leaves NULL stay NULL");
          snprintf(path, sizeof path, "%s/badabi.so", d); setenv("NAPPLE_ONLINE_PROVIDER", path, 1); online_provider_reset(); CHECK(online_provider() == online_null_provider(), "a module with the wrong ABI version is rejected: NullProvider");
          snprintf(path, sizeof path, "%s/nosym.so", d); setenv("NAPPLE_ONLINE_PROVIDER", path, 1); online_provider_reset(); CHECK(online_provider() == online_null_provider(), "a module without the entry symbol is rejected");
          setenv("NAPPLE_ONLINE_PROVIDER", "/nonexistent/provider.so", 1); online_provider_reset(); CHECK(online_provider() == online_null_provider(), "a missing module falls back to the NullProvider");
      }
      unsetenv("NAPPLE_ONLINE_PROVIDER"); online_provider_reset();
      if (d) { snprintf(cmd, sizeof cmd, "rm -rf %s", d); (void)!system(cmd); } }

    /* ---- back to offline ---- */
    online_provider_install(&lb_provider); CHECK(online_provider_install(NULL) == &lb_provider && online_provider() == online_null_provider(), "NULL restores the NullProvider");
    bsdg_reset(bsd_g()); online_null_reset();
    { IpcObject *nf = nifm_open(); svc_call(nf, 5, NULL, 0, &c); IpcObject *gen = c.out_objects[0]; svc_call(gen, 4, NULL, 0, &c); IpcObject *rq = c.out_objects[0];
      svc_call(rq, 4, NULL, 0, &c); svc_call(rq, 1, NULL, 0, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "offline again after uninstalling"); }
    { /* DuplicateSocket and the mmsg calls over the loopback provider */
        memset(&lb, 0, sizeof lb); online_provider_install(&lb_provider); bsdg_reset(bsd_g());
        IpcObject *bs2 = bsd_open(); IpcCall *r2;
        r2 = BS(bs2, 2, 2, 1, 6); int ls = ri(r2, 0); r2 = BS(bs2, 2, 2, 1, 6); int cl = ri(r2, 0);
        uint8_t ad[16]; sin_addr(ad, 7100, 127, 0, 0, 1);
        bs(bs2, 13, ls, 0, 0, ad, 16, NULL, 0, NULL, 0, NULL, 0); BS(bs2, 18, ls, 4, 0); bs(bs2, 14, cl, 0, 0, ad, 16, NULL, 0, NULL, 0, NULL, 0);
        r2 = bs(bs2, 12, ls, 0, 0, NULL, 0, NULL, 0, enc, sizeof enc, NULL, 0); int sv = ri(r2, 0);
        r2 = BS(bs2, 27, cl, 0, 0); int cl2 = ri(r2, 0); CHECK(cl2 > cl && ri(r2, 1) == 0, "DuplicateSocket on a provider socket");
        unsigned used = 0; for (int i = 0; i < LB_MAX; i++) used += lb.s[i].used;
        BS(bs2, 26, cl, 0, 0); unsigned used2 = 0; for (int i = 0; i < LB_MAX; i++) used2 += lb.s[i].used;
        CHECK(used == used2, "closing one alias keeps the provider socket open");
        char m1[] = "dup-send"; r2 = bs(bs2, 10, cl2, 0, 0, m1, sizeof m1, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r2, 0) == (int)sizeof m1, "the duplicate can send");
        char rb[32] = {0}; r2 = bs(bs2, 8, sv, 0, 0, NULL, 0, NULL, 0, rb, sizeof rb, NULL, 0); CHECK(!strcmp(rb, m1), "the peer receives it");
        BS(bs2, 26, cl2, 0, 0); unsigned used3 = 0; for (int i = 0; i < LB_MAX; i++) used3 += lb.s[i].used;
        CHECK(used3 == used2 - 1, "the last close reaches the provider");
        /* SendMMsg: two messages, the first with two iovecs. FreeBSD LP64 mmsghdr, 0x38 bytes each. */
        BS(bs2, 14, 0, 0, 0); r2 = BS(bs2, 2, 2, 1, 6); int c3 = ri(r2, 0); bs(bs2, 14, c3, 0, 0, ad, 16, NULL, 0, NULL, 0, NULL, 0);
        r2 = bs(bs2, 12, ls, 0, 0, NULL, 0, NULL, 0, enc, sizeof enc, NULL, 0); int s3 = ri(r2, 0);
        char p1[] = "ab", p2[] = "cdef", p3[] = "XYZ"; uint64_t iov[6] = {(uint64_t)(uintptr_t)p1, 2, (uint64_t)(uintptr_t)p2, 4, (uint64_t)(uintptr_t)p3, 3};
        uint8_t mm[0x38 * 2] = {0}; uint64_t ip0 = (uint64_t)(uintptr_t)iov, ip1 = (uint64_t)(uintptr_t)(iov + 4); int32_t n2 = 2, n1 = 1;
        memcpy(mm + 0x10, &ip0, 8); memcpy(mm + 0x18, &n2, 4); memcpy(mm + 0x38 + 0x10, &ip1, 8); memcpy(mm + 0x38 + 0x18, &n1, 4);
        r2 = bs(bs2, 31, c3, 2, 0, mm, sizeof mm, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r2, 0) == 2 && ri(r2, 1) == 0, "SendMMsg sends both messages");
        uint32_t l0, l1; memcpy(&l0, mm + 0x30, 4); memcpy(&l1, mm + 0x38 + 0x30, 4); CHECK(l0 == 6 && l1 == 3, "msg_len holds the bytes of each message");
        char gather[16] = {0}; r2 = bs(bs2, 8, s3, 0, 0, NULL, 0, NULL, 0, gather, sizeof gather, NULL, 0); CHECK(ri(r2, 0) == 9 && !memcmp(gather, "abcdefXYZ", 9), "the iovecs were gathered in order");
        /* RecvMMsg into two buffers */
        r2 = bs(bs2, 10, c3, 0, 0, "12345678", 8, NULL, 0, NULL, 0, NULL, 0);
        char q1[3] = {0}, q2[16] = {0}; uint64_t riov[4] = {(uint64_t)(uintptr_t)q1, 3, (uint64_t)(uintptr_t)q2, 16}; uint8_t rm[0x38] = {0}; uint64_t rp = (uint64_t)(uintptr_t)riov; int32_t rn = 2;
        memcpy(rm + 0x10, &rp, 8); memcpy(rm + 0x18, &rn, 4);
        r2 = bs(bs2, 30, s3, 1, 0, NULL, 0, NULL, 0, rm, sizeof rm, NULL, 0); CHECK(ri(r2, 0) == 1, "RecvMMsg returns one message");
        uint32_t rl; memcpy(&rl, rm + 0x30, 4); CHECK(rl == 8 && !memcmp(q1, "123", 3) && !strcmp(q2, "45678"), "RecvMMsg scatters across the iovecs");
        r2 = bs(bs2, 30, s3, 1, 0, NULL, 0, NULL, 0, rm, sizeof rm, NULL, 0); CHECK(ri(r2, 0) == -1 && ri(r2, 1) == HZ_EAGAIN, "RecvMMsg with nothing queued: EAGAIN");
        r2 = bs(bs2, 31, 99, 1, 0, mm, sizeof mm, NULL, 0, NULL, 0, NULL, 0); CHECK(ri(r2, 0) == -1 && ri(r2, 1) == HZ_EBADF, "SendMMsg on an unknown descriptor: EBADF");
        bsdg_reset(bsd_g()); online_provider_install(NULL);
    }
    printf("{\"test\":\"svc_online\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
