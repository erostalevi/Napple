/* Napple online: the host provider wrapped for a Napple server (see net_online.h). The wrapper shares the host provider's ctx, so every entry point it
 * does not override is the host's own; the overrides find their state through a small registry keyed by that ctx. */
#include "net_online.h"
#include "net_host.h"
#include "net_room.h"
#include "hz_result.h"
#include "offline_core.h"
#include "napple.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ipc_trace_guest(unsigned n) __attribute__((weak)); /* ipc.c: the guest call stack of the request being served (debug aid) */

/* ---- configuration and name policy ---- */

int net_online_parse_server(const char *url, NetOnlineServer *out) {
    memset(out, 0, sizeof *out);
    if (!url || !*url) return -1;
    const char *p = url;
    if (!strncasecmp(p, "https://", 8)) out->tls = 1, p += 8;
    else if (!strncasecmp(p, "http://", 7)) p += 7;
    else if (strstr(p, "://")) return -1; /* another scheme */
    size_t n = strcspn(p, ":/");
    if (!n || n >= sizeof out->host) return -1;
    memcpy(out->host, p, n);
    p += n;
    out->port = out->tls ? 443 : 80;
    if (*p == ':') {
        char *end;
        long v = strtol(p + 1, &end, 10);
        if (v <= 0 || v > 65535 || (*end && *end != '/')) return -1;
        out->port = (uint16_t)v;
    }
    snprintf(out->key, sizeof out->key, "%s_%u", out->host, out->port);
    for (char *k = out->key; *k; k++)
        if (!isalnum((unsigned char)*k) && *k != '.' && *k != '-' && *k != '_') *k = '_';
    return 0;
}

static int ends_with(const char *s, size_t n, const char *suffix) {
    size_t m = strlen(suffix);
    return n >= m && !strncasecmp(s + n - m, suffix, m) && (n == m || s[n - m - 1] == '.');
}
/* Nintendo's own domains: whatever a title or SDK builds from them is never resolved (no Nintendo server is contacted). */
static const char *const nintendo_domains[] = {
    "nintendo.net", "nintendo.com", "nintendo.co.jp", "nintendo.jp", "nintendowifi.net", "nintendo.eu", "nintendo.co.uk", "nintendo.de", "nintendo.fr",
    "nintendo.es", "nintendo.it", "nintendo.nl", "nintendo.be", "nintendo.at", "nintendo.ch", "nintendo.pt", "nintendo.ru", "nintendo.com.au",
    "nintendo.co.nz", "nintendo.co.kr", "nintendo.com.hk", "nintendo.tw", "nintendo-europe.com", "nintendo.cn", "nintendo.ca", "nintendo.com.mx",
    "nintendo.com.br", "nintendo-net.com", "nintendo.co.za",
};
/* A NEX game server: g<8 hex>-<env>.<kind>.srv.nintendo.net (kind "s.n" = the game server proper, "p" = its sibling); the environment must be filled. */
static int is_game_server(const char *s, size_t n) {
    if (n < 10 || (s[0] != 'g' && s[0] != 'G')) return 0;
    for (int i = 1; i <= 8; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    if (s[9] != '-') return 0;
    return ends_with(s, n, "srv.nintendo.net");
}
/* Pia's NAT check servers: nncs1-<env>.n.n.srv.nintendo.net (primary) and nncs2-... (secondary); also the nncsN.<platform>.<env> form. */
static int nat_check_server(const char *s, size_t n) {
    if (n < 6 || strncasecmp(s, "nncs", 4) || (s[4] != '1' && s[4] != '2') || (s[5] != '-' && s[5] != '.')) return 0;
    return ends_with(s, n, "n.n.srv.nintendo.net") ? s[4] - '0' : 0;
}
uint32_t net_online_redirect_ip(const char *name) {
    size_t n = name ? strlen(name) : 0;
    while (n && name[n - 1] == '.') n--;
    return n && nat_check_server(name, n) == 2 ? NET_ONLINE_VIRTUAL_IP2 : NET_ONLINE_VIRTUAL_IP;
}
int net_online_name_policy(const char *name) {
    if (!name || !*name) return NET_ONLINE_NAME_PASS;
    size_t n = strlen(name);
    while (n && name[n - 1] == '.') n--; /* a fully qualified name */
    int nintendo = 0;
    for (size_t i = 0; i < sizeof nintendo_domains / sizeof nintendo_domains[0] && !nintendo; i++) nintendo = ends_with(name, n, nintendo_domains[i]);
    if (!nintendo) return memchr(name, '%', n) ? NET_ONLINE_NAME_BLOCK : NET_ONLINE_NAME_PASS;
    if (memchr(name, '%', n)) return NET_ONLINE_NAME_BLOCK;
    return is_game_server(name, n) || nat_check_server(name, n) ? NET_ONLINE_NAME_REDIRECT : NET_ONLINE_NAME_BLOCK;
}
int net_online_nsd(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        const char *rep = *p == '%' ? "lp1" : NULL;
        size_t m = rep ? 3 : 1;
        if (o + m >= n) return -1;
        if (rep) memcpy(out + o, rep, 3);
        else out[o] = *p;
        o += m;
    }
    if (o >= n) return -1;
    out[o] = 0;
    return 0;
}

/* ---- the wrapper's state ---- */

#define NO_MAX_REDIRECTED 64
typedef struct {
    OnlineProvider table; /* what online_core.c calls; ctx = the host's */
    const OnlineProvider *host;
    NetOnlineServer srv;
    char state_dir[1024];
    pthread_mutex_t mu;
    OnlineAddr server_addr; /* resolved on first connect */
    int server_resolved;
    struct {
        int h;
        uint16_t port; /* the port the title asked for (getpeername reports it back) */
    } redirected[NO_MAX_REDIRECTED];
    unsigned nredirected;
    int have_account, account_failed;
    uint8_t account_uid[16];
    NetOnlineAccount account;
    char token[4096];
    size_t token_len;
    int64_t token_ms;
    char logged[16][128]; /* names already logged once */
    unsigned nlogged;
    NetSocial *social; /* friends and presence (net_online_social.c), NULL without a Napple Account token */
} NetOnline;

/* NAPPLE_ONLINE_DEBUG=1: every lookup, connection and TLS step of the wrapper is logged (diagnosing a title's online path). */
static int no_debug = -1;
#define NO_DBG(...)                                                                                                                            \
    do {                                                                                                                                       \
        if (no_debug < 0) no_debug = getenv("NAPPLE_ONLINE_DEBUG") && *getenv("NAPPLE_ONLINE_DEBUG") == '1';                                  \
        if (no_debug) log_line("online", "napple debug: " __VA_ARGS__);                                                                        \
    } while (0)
static const char *addr_str(const OnlineAddr *a, char *buf, size_t n) {
    if (a->family == BSD_AF_INET) snprintf(buf, n, "%u.%u.%u.%u:%u", a->addr[0], a->addr[1], a->addr[2], a->addr[3], a->port);
    else snprintf(buf, n, "family %u port %u", a->family, a->port);
    return buf;
}

static pthread_mutex_t reg_mu = PTHREAD_MUTEX_INITIALIZER;
static NetOnline *reg[8];
static NetOnline *no_of(void *ctx) {
    pthread_mutex_lock(&reg_mu);
    NetOnline *o = NULL;
    for (unsigned i = 0; i < 8 && !o; i++)
        if (reg[i] && reg[i]->table.ctx == ctx) o = reg[i];
    pthread_mutex_unlock(&reg_mu);
    return o;
}

static void log_once(NetOnline *o, const char *what, const char *name) {
    pthread_mutex_lock(&o->mu);
    char key[128];
    snprintf(key, sizeof key, "%s %s", what, name);
    int seen = 0;
    for (unsigned i = 0; i < o->nlogged && !seen; i++) seen = !strcmp(o->logged[i], key);
    if (!seen && o->nlogged < 16) snprintf(o->logged[o->nlogged++], sizeof o->logged[0], "%s", key);
    pthread_mutex_unlock(&o->mu);
    if (!seen) log_line("online", "napple: %s %s", what, name);
}

static int redirected_index(NetOnline *o, int h) {
    for (unsigned i = 0; i < o->nredirected; i++)
        if (o->redirected[i].h == h) return (int)i;
    return -1;
}
static int is_redirected(NetOnline *o, int h, uint16_t *port) {
    pthread_mutex_lock(&o->mu);
    int i = redirected_index(o, h);
    if (i >= 0 && port) *port = o->redirected[i].port;
    pthread_mutex_unlock(&o->mu);
    return i >= 0;
}
/* 1 = the primary virtual address (game servers, NAT check primary), 2 = the NAT check's secondary server, 0 = not ours. */
static int is_virtual(const OnlineAddr *a) {
    uint32_t ip = (uint32_t)a->addr[0] << 24 | (uint32_t)a->addr[1] << 16 | (uint32_t)a->addr[2] << 8 | a->addr[3];
    if (a->family != BSD_AF_INET) return 0;
    return ip == NET_ONLINE_VIRTUAL_IP ? 1 : ip == NET_ONLINE_VIRTUAL_IP2 ? 2 : 0;
}
static void set_ip(OnlineAddr *a, uint32_t ip, uint16_t port) {
    memset(a, 0, sizeof *a);
    a->family = BSD_AF_INET;
    a->port = port;
    a->addr[0] = ip >> 24, a->addr[1] = ip >> 16 & 255, a->addr[2] = ip >> 8 & 255, a->addr[3] = ip & 255;
}
static int nat_check_port(uint16_t p) { return p == NET_ONLINE_NATCHECK_PORT || p == NET_ONLINE_NATCHECK_PORT + 100 || p == 33334 || p == 33335; }
/* Where a virtual address and port really are: the NAT check ports go to the server's NAT check (the secondary server's to its ports + 1000);
 * anything else to the server's HTTP / WebSocket port. */
static uint16_t real_port(NetOnline *o, int v, uint16_t port);
/* The Napple server's real address (resolved once through the host). 0 = ok. */
static int server_address(NetOnline *o, OnlineAddr *out) {
    pthread_mutex_lock(&o->mu);
    int ok = o->server_resolved;
    if (ok) *out = o->server_addr;
    pthread_mutex_unlock(&o->mu);
    if (ok) return 0;
    OnlineResolved r;
    char port[8];
    snprintf(port, sizeof port, "%u", o->srv.port);
    if (o->host->dns_resolve(o->host->ctx, o->srv.host, port, BSD_AF_INET, &r) || !r.count) {
        log_line("online", "napple: server '%s' does not resolve", o->srv.host);
        return -1;
    }
    r.addrs[0].port = o->srv.port;
    pthread_mutex_lock(&o->mu);
    o->server_addr = r.addrs[0];
    o->server_resolved = 1;
    *out = o->server_addr;
    pthread_mutex_unlock(&o->mu);
    return 0;
}

/* NAPPLE_ONLINE_NATCHECK_BASE: the server's NAT check runs on other ports (all shifted by base - 10025), e.g. beside another server on one Mac. */
static int natcheck_shift(void) {
    static int shift = -100000;
    if (shift == -100000) {
        const char *e = getenv("NAPPLE_ONLINE_NATCHECK_BASE");
        int b = e && *e ? atoi(e) : NET_ONLINE_NATCHECK_PORT;
        shift = b > 0 && b < 65535 ? b - NET_ONLINE_NATCHECK_PORT : 0;
    }
    return shift;
}
static uint16_t real_port(NetOnline *o, int v, uint16_t port) {
    if (nat_check_port(port)) return (uint16_t)(port + natcheck_shift() + (v == 2 ? NET_ONLINE_NATCHECK_SECONDARY : 0));
    return o->srv.port;
}

/* ---- sfdnsres / nsd ---- */

static int no_dns_resolve(void *ctx, const char *host, const char *service, int family, OnlineResolved *out) {
    NetOnline *o = no_of(ctx);
    char filled[256];
    if (host && strchr(host, '%') && net_online_nsd(host, filled, sizeof filled) == 0) host = filled; /* the console's resolver fills nsd templates itself */
    NO_DBG("resolve '%s' service '%s' family %d -> policy %d", host ? host : "", service ? service : "", family, net_online_name_policy(host));
    switch (net_online_name_policy(host)) {
        case NET_ONLINE_NAME_BLOCK:
            log_once(o, "blocked name", host);
            memset(out, 0, sizeof *out);
            return HZ_EAI_NONAME;
        case NET_ONLINE_NAME_REDIRECT: {
            log_once(o, "redirected name", host);
            memset(out, 0, sizeof *out);
            if (family == BSD_AF_INET6) return HZ_EAI_NONAME; /* the virtual address is IPv4 */
            out->count = 1;
            set_ip(&out->addrs[0], net_online_redirect_ip(host), service ? (uint16_t)atoi(service) : 0);
            snprintf(out->canonname, sizeof out->canonname, "%s", host);
            return 0;
        }
        default: return o->host->dns_resolve(o->host->ctx, host, service, family, out);
    }
}
static int no_dns_reverse(void *ctx, const OnlineAddr *a, char *host, size_t n) {
    NetOnline *o = no_of(ctx);
    if (is_virtual(a)) {
        snprintf(host, n, "%s", o->srv.host);
        return 0;
    }
    return o->host->dns_reverse(o->host->ctx, a, host, n);
}
static int no_nsd_resolve(void *ctx, const char *in, char *out, size_t n) {
    NetOnline *o = no_of(ctx);
    if (net_online_nsd(in, out, n)) return -1;
    log_once(o, "nsd", out);
    return 0;
}

/* ---- sockets: the virtual address is the Napple server ---- */

static int no_sock_connect(void *ctx, int h, const OnlineAddr *a, int *err) {
    NetOnline *o = no_of(ctx);
    char ab[64];
    NO_DBG("connect socket %d to %s%s", h, addr_str(a, ab, sizeof ab), is_virtual(a) ? " (the Napple server)" : "");
    int v = is_virtual(a);
    if (!v) return o->host->sock_connect(o->host->ctx, h, a, err);
    OnlineAddr real;
    if (server_address(o, &real)) {
        *err = HZ_ECONNREFUSED;
        return -1;
    }
    real.port = real_port(o, v, a->port);
    if (nat_check_port(a->port)) { /* a connected NAT check socket: no TLS, no port bookkeeping */
        log_once(o, "NAT check ->", o->srv.host);
        return o->host->sock_connect(o->host->ctx, h, &real, err);
    }
    pthread_mutex_lock(&o->mu);
    if (redirected_index(o, h) < 0 && o->nredirected < NO_MAX_REDIRECTED) {
        o->redirected[o->nredirected].h = h;
        o->redirected[o->nredirected].port = a->port;
        o->nredirected++;
    }
    pthread_mutex_unlock(&o->mu);
    char what[64];
    snprintf(what, sizeof what, "port %u ->", a->port);
    char where[300];
    snprintf(where, sizeof where, "%s:%u (%s)", o->srv.host, o->srv.port, o->srv.tls ? "https" : "http");
    log_once(o, what, where);
    int rc = o->host->sock_connect(o->host->ctx, h, &real, err);
    NO_DBG("connect socket %d to the server: rc %d errno %d", h, rc, rc ? *err : 0);
    return rc;
}
/* NAPPLE_ONLINE_NATCHECK_LOCALIP (1, 0, or unset = on while a room's virtual LAN is up): a NAT check message (type 1..5 / 101..103, 16 bytes) whose "local IPv4" field is still zero gets the console's own
 * address (nifm) there. The server reports a client it sees on loopback at that address instead of the Mac's LAN address, so two instances on one Mac (each
 * with its own virtual address, e.g. from a room) get two distinct public addresses: Pia identifies a station by address, and one shared address makes a host
 * refuse its first joiner. Real clients are never seen on loopback, so the server ignores the field for them. */
static int natcheck_local_ip_on(void) {
    const char *e = getenv("NAPPLE_ONLINE_NATCHECK_LOCALIP"); /* 1 on, 0 off; unset: on while a room's virtual LAN is up */
    if (e && *e) return *e == '1';
    return net_room_vlan_on();
}
static void natcheck_local_ip(NetOnline *o, const void *buf, size_t n, uint8_t out[16], const void **use) {
    *use = buf;
    if (!natcheck_local_ip_on() || n != 16) return;
    const uint8_t *b = buf;
    uint32_t kind = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    if (!((kind >= 1 && kind <= 5) || (kind >= 101 && kind <= 103)) || b[12] | b[13] | b[14] | b[15]) return;
    OnlineNetInfo ni;
    if (o->host->net_info(o->host->ctx, &ni) != 0 || !ni.connected || !ni.ip) return;
    memcpy(out, buf, 16);
    out[12] = (uint8_t)(ni.ip >> 24), out[13] = (uint8_t)(ni.ip >> 16), out[14] = (uint8_t)(ni.ip >> 8), out[15] = (uint8_t)ni.ip;
    *use = out;
}
static int64_t no_sock_send(void *ctx, int h, const void *buf, size_t n, int flags, const OnlineAddr *to, int *err) {
    NetOnline *o = no_of(ctx);
    OnlineAddr real;
    int v = to ? is_virtual(to) : 0;
    uint8_t patched[16];
    if (!to || v) natcheck_local_ip(o, buf, n, patched, &buf);
    if (v) {
        if (server_address(o, &real)) {
            *err = HZ_ENETUNREACH;
            return -1;
        }
        real.port = real_port(o, v, to->port);
        char ab[64];
        NO_DBG("datagram socket %d to %s -> server port %u", h, addr_str(to, ab, sizeof ab), real.port);
        log_once(o, nat_check_port(to->port) ? "NAT check ->" : "datagram ->", o->srv.host);
        to = &real;
    }
    return o->host->sock_send(o->host->ctx, h, buf, n, flags, to, err);
}
/* Datagrams from the server's NAT check ports come back from the virtual address the title sent to. Only those ports: a peer on the same address
 * (two instances on one Mac) keeps its own address. */
static int64_t no_sock_recv(void *ctx, int h, void *buf, size_t n, int flags, OnlineAddr *from, int *err) {
    NetOnline *o = no_of(ctx);
    int64_t r = o->host->sock_recv(o->host->ctx, h, buf, n, flags, from, err);
    OnlineAddr srv;
    if (r >= 0 && from && from->family == BSD_AF_INET && o->server_resolved && server_address(o, &srv) == 0 && !memcmp(from->addr, srv.addr, 4)) {
        uint16_t p = (uint16_t)(from->port - natcheck_shift()), q = from->port;
        if (nat_check_port(p)) set_ip(from, NET_ONLINE_VIRTUAL_IP, p);
        else if (nat_check_port((uint16_t)(p - NET_ONLINE_NATCHECK_SECONDARY))) set_ip(from, NET_ONLINE_VIRTUAL_IP2, (uint16_t)(p - NET_ONLINE_NATCHECK_SECONDARY));
        else return r;
        if (r >= 16) memcpy((uint8_t *)buf + 12, from->addr, 4); /* the reply's "server address" field: the server the title knows */
        if (r >= 16 && natcheck_local_ip_on()) {
            /* the port the server saw is the host socket's; on a virtual LAN (rooms) the title's socket has its own virtual port, the one a peer must send to */
            OnlineAddr me;
            int e2 = 0;
            if (o->host->sock_getsockname && o->host->sock_getsockname(o->host->ctx, h, &me, &e2) == 0 && me.port) {
                uint8_t *b = buf;
                b[4] = (uint8_t)(me.port >> 24), b[5] = (uint8_t)(me.port >> 16), b[6] = (uint8_t)(me.port >> 8), b[7] = (uint8_t)me.port;
            }
        }
        NO_DBG("datagram socket %d from the NAT check (server port %u)", h, q);
    }
    return r;
}
static int no_sock_getpeername(void *ctx, int h, OnlineAddr *out, int *err) {
    NetOnline *o = no_of(ctx);
    int rc = o->host->sock_getpeername(o->host->ctx, h, out, err);
    uint16_t port;
    if (rc == 0 && is_redirected(o, h, &port)) set_ip(out, NET_ONLINE_VIRTUAL_IP, port);
    return rc;
}
static int no_sock_bind(void *ctx, int h, const OnlineAddr *a, int *err) {
    NetOnline *o = no_of(ctx);
    int rc = o->host->sock_bind(o->host->ctx, h, a, err);
    char ab[64];
    NO_DBG("bind socket %d to %s: rc %d errno %d", h, addr_str(a, ab, sizeof ab), rc, rc ? *err : 0);
    return rc;
}
static int no_sock_set_nonblock(void *ctx, int h, int on, int *err) {
    NetOnline *o = no_of(ctx);
    NO_DBG("socket %d non-blocking %d", h, on);
    return o->host->sock_set_nonblock(o->host->ctx, h, on, err);
}
static int no_sock_close(void *ctx, int h, int *err) {
    NetOnline *o = no_of(ctx);
    pthread_mutex_lock(&o->mu);
    int i = redirected_index(o, h);
    if (i >= 0) o->redirected[i] = o->redirected[--o->nredirected];
    pthread_mutex_unlock(&o->mu);
    return o->host->sock_close(o->host->ctx, h, err);
}

/* ---- ssl: TLS to the virtual address ends here ---- */

#define NO_TLS_MAGIC 0x4e4f544cu
typedef struct {
    uint32_t magic;
    int ours;   /* over a redirected socket */
    int plain;  /* ours, http:// server: no TLS at all */
    int sock;
    int nonblocking; /* ours: the guest's IoMode is non-blocking */
    void *inner; /* the host provider's connection (not ours, or ours over https) */
} NoTls;

/* Ours, non-blocking: is there anything to read without waiting (TLS bytes already decrypted, or data on the socket)? */
static int readable(NetOnline *o, NoTls *t) {
    if (!t->plain && o->host->tls_pending(o->host->ctx, t->inner) > 0) return 1;
    OnlinePollFd pf = {t->sock, ONLINE_POLLIN, 0};
    int err = 0;
    return o->host->sock_poll(o->host->ctx, &pf, 1, 0, &err) > 0;
}

static void *no_tls_new(void *ctx, int sock) {
    NetOnline *o = no_of(ctx);
    NoTls *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->magic = NO_TLS_MAGIC;
    t->sock = sock;
    t->ours = sock >= 0 && is_redirected(o, sock, NULL);
    t->plain = t->ours && !o->srv.tls;
    NO_DBG("tls_new over socket %d: %s", sock, t->plain ? "ours, plain" : t->ours ? "ours, TLS to the server" : "not ours");
    if (!t->plain) {
        t->inner = o->host->tls_new(o->host->ctx, sock);
        if (!t->inner) {
            free(t);
            return NULL;
        }
    }
    return t;
}
static void no_tls_free(void *ctx, void *conn) {
    NetOnline *o = no_of(ctx);
    NoTls *t = conn;
    if (!t) return;
    if (t->inner) o->host->tls_free(o->host->ctx, t->inner);
    t->magic = 0;
    free(t);
}
static int no_tls_set_host(void *ctx, void *conn, const char *host) {
    NetOnline *o = no_of(ctx);
    NoTls *t = conn;
    if (!t->ours) return o->host->tls_set_host(o->host->ctx, t->inner, host);
    log_once(o, "TLS for", host);
    return t->plain ? 0 : o->host->tls_set_host(o->host->ctx, t->inner, o->srv.host); /* the Napple server's own name is verified */
}
static int no_tls_handshake(void *ctx, void *conn, uint32_t *result) {
    NetOnline *o = no_of(ctx);
    NoTls *t = conn;
    NO_DBG("tls handshake over socket %d", t->sock);
    if (t->plain) return 0;
    if (t->ours) o->host->tls_set_host(o->host->ctx, t->inner, o->srv.host); /* also when the title never named the server */
    return o->host->tls_handshake(o->host->ctx, t->inner, result);
}
static int64_t no_tls_read(void *ctx, void *conn, void *buf, size_t n, int *err) {
    NetOnline *o = no_of(ctx);
    NoTls *t = conn;
    if (t->nonblocking && !readable(o, t)) {
        *err = HZ_EAGAIN;
        return -1;
    }
    int64_t r = t->plain ? o->host->sock_recv(o->host->ctx, t->sock, buf, n, 0, NULL, err) : o->host->tls_read(o->host->ctx, t->inner, buf, n, err);
    if (t->ours) NO_DBG("tls read socket %d: %lld of %zu (errno %d)", t->sock, (long long)r, n, r < 0 ? *err : 0);
    return r;
}
static int64_t no_tls_write(void *ctx, void *conn, const void *buf, size_t n, int *err) {
    NetOnline *o = no_of(ctx);
    NoTls *t = conn;
    int64_t r = t->plain ? o->host->sock_send(o->host->ctx, t->sock, buf, n, 0, NULL, err) : o->host->tls_write(o->host->ctx, t->inner, buf, n, err);
    if (t->ours) NO_DBG("tls write socket %d: %lld of %zu (errno %d)", t->sock, (long long)r, n, r < 0 ? *err : 0);
    return r;
}
static int64_t no_tls_pending(void *ctx, void *conn) {
    NetOnline *o = no_of(ctx);
    NoTls *t = conn;
    return t->plain ? 0 : o->host->tls_pending(o->host->ctx, t->inner);
}
static int no_tls_set_io_mode(void *ctx, void *conn, uint32_t mode) {
    NetOnline *o = no_of(ctx);
    NoTls *t = conn;
    if (!t->ours) {
        int (*f)(void *, void *, uint32_t) = ONLINE_FN(o->host, tls_set_io_mode);
        return f ? f(o->host->ctx, t->inner, mode) : 0;
    }
    t->nonblocking = mode == ONLINE_TLS_IO_NONBLOCKING;
    NO_DBG("tls io mode of socket %d: %s", t->sock, t->nonblocking ? "non-blocking" : "blocking");
    return 0;
}
static int no_tls_add_ca(void *ctx, void *conn, const void *cert, size_t n) {
    NetOnline *o = no_of(ctx);
    NoTls *t = conn;
    if (t->ours) return 0; /* the title's CAs are Nintendo's: the Napple server is verified against the Mac's trust store only */
    int (*f)(void *, void *, const void *, size_t) = ONLINE_FN(o->host, tls_add_ca);
    return f ? f(o->host->ctx, t->inner, cert, n) : 0;
}
static int no_tls_set_verify(void *ctx, void *conn, uint32_t option) {
    NetOnline *o = no_of(ctx);
    NoTls *t = conn;
    if (t->ours) return 0; /* every check stays on for the Napple server */
    int (*f)(void *, void *, uint32_t) = ONLINE_FN(o->host, tls_set_verify);
    return f ? f(o->host->ctx, t->inner, option) : 0;
}

/* ---- acc: the Napple account and its token ---- */

static int ensure_account(NetOnline *o, const uint8_t uid[16]) {
    pthread_mutex_lock(&o->mu);
    int have = o->have_account && !memcmp(o->account_uid, uid, 16);
    int failed = o->account_failed;
    pthread_mutex_unlock(&o->mu);
    if (have) return 0;
    if (failed) return -1; /* one attempt per run: a title asks often, and an unreachable server must not stall every call */
    NetOnlineAccount a;
    int rc = net_online_account(o->host, &o->srv, o->state_dir[0] ? o->state_dir : NULL, uid, &a);
    pthread_mutex_lock(&o->mu);
    if (rc == 0) {
        o->account = a;
        memcpy(o->account_uid, uid, 16);
        o->have_account = 1;
        o->token_len = 0;
    } else o->account_failed = 1;
    pthread_mutex_unlock(&o->mu);
    if (rc == 0) log_line("online", "napple: local user linked to Napple account %llu on %s:%u", (unsigned long long)a.pid, o->srv.host, o->srv.port);
    if (rc == 0 && o->social) net_social_set_self_id(o->social, a.pid); /* friends find this user's game sessions by it */
    else log_line("online", "napple: no Napple account (server %s:%u unreachable or refused): the user stays unlinked", o->srv.host, o->srv.port);
    return rc;
}
static int no_account_info(void *ctx, const uint8_t uid[16], OnlineAccountInfo *out) {
    NetOnline *o = no_of(ctx);
    memset(out, 0, sizeof *out);
    if (ensure_account(o, uid)) return -1;
    out->linked = 1;
    out->id = o->account.pid;
    memcpy(out->resource_cache, &out->id, 8); /* the cache starts with the account id; the rest is left empty */
    return 0;
}
static uint64_t running_title(void) {
    return strtoull(g_cfg.title_id, NULL, 16);
}
static size_t no_account_id_token(void *ctx, const uint8_t uid[16], char *out, size_t n) {
    NetOnline *o = no_of(ctx);
    if (ensure_account(o, uid)) return 0;
    int64_t now = nh_now_ms();
    pthread_mutex_lock(&o->mu);
    size_t len = o->token_len && now - o->token_ms < 5 * 60 * 1000 ? o->token_len : 0; /* the server issues tokens for 15 minutes */
    if (len && len <= n) memcpy(out, o->token, len);
    NetOnlineAccount a = o->account;
    pthread_mutex_unlock(&o->mu);
    if (len) return len <= n ? len : 0;
    char tok[sizeof o->token];
    len = net_online_token(o->host, &o->srv, &a, running_title(), tok, sizeof tok);
    if (!len) {
        log_line("online", "napple: the server issued no token");
        return 0;
    }
    pthread_mutex_lock(&o->mu);
    memcpy(o->token, tok, len);
    o->token_len = len;
    o->token_ms = now;
    pthread_mutex_unlock(&o->mu);
    log_line("online", "napple: ID token issued (%zu bytes) for title %016llx", len, (unsigned long long)running_title());
    if (len > n) return 0;
    memcpy(out, tok, len);
    return len;
}

/* ---- prepo: play reports are dropped (never sent anywhere); NAPPLE_ONLINE_DEBUG shows them, they often carry a title's error details ---- */

static void no_report(void *ctx, const void *record, size_t n) {
    (void)ctx;
    if (no_debug < 0) no_debug = getenv("NAPPLE_ONLINE_DEBUG") && *getenv("NAPPLE_ONLINE_DEBUG") == '1';
    if (!no_debug) return;
    char hex[1025];
    size_t m = n < 512 ? n : 512;
    for (size_t i = 0; i < m; i++) snprintf(hex + 2 * i, 3, "%02x", ((const uint8_t *)record)[i]);
    hex[2 * m] = 0;
    log_line("online", "napple debug: play report (%zu bytes, dropped): %s", n, hex);
    if (ipc_trace_guest) ipc_trace_guest(32); /* who reported it: an error report names the title's code path */
}

/* ---- friend: napple-social (net_online_social.c) ---- */

static uint32_t no_friend_count(void *ctx) { return net_social_count(no_of(ctx)->social); }
static uint32_t no_friend_list(void *ctx, uint32_t off, OnlineFriend *out, uint32_t max) {
    OnlineFriendInfo f[16];
    uint32_t n = 0;
    while (n < max) {
        uint32_t want = max - n < 16 ? max - n : 16, got = net_social_list(no_of(ctx)->social, off + n, f, want);
        for (uint32_t i = 0; i < got; i++) {
            memset(&out[n + i], 0, sizeof out[0]);
            out[n + i].id = f[i].id;
            memcpy(out[n + i].name, f[i].name, sizeof out[0].name);
            out[n + i].presence = f[i].status;
        }
        n += got;
        if (got < want) break;
    }
    return n;
}
/* The social client learned which friends play on this server: the server hears it (only once this user has its account here). */
static int no_report_friend_pids(void *arg, const uint64_t *pids, uint32_t n) {
    NetOnline *o = arg;
    pthread_mutex_lock(&o->mu);
    int have = o->have_account;
    NetOnlineAccount a = o->account;
    pthread_mutex_unlock(&o->mu);
    if (!have) return -1;
    int rc = net_online_report_friends(o->host, &o->srv, &a, pids, n);
    log_line("online", "napple: %u friends on %s:%u reported to the game server%s", n, o->srv.host, o->srv.port, rc ? " (refused)" : "");
    return rc;
}
static void no_presence_update(void *ctx, const void *record, size_t n) { net_social_presence(no_of(ctx)->social, record, n); }
static uint32_t no_friend_list_info(void *ctx, uint32_t off, OnlineFriendInfo *out, uint32_t max) { return net_social_list(no_of(ctx)->social, off, out, max); }
static int no_friend_find(void *ctx, uint64_t id, OnlineFriendInfo *out) { return net_social_find(no_of(ctx)->social, id, out); }
static uint32_t no_friend_request_count(void *ctx) { return net_social_request_count(no_of(ctx)->social); }
static int no_friend_pop_note(void *ctx, uint32_t *type, uint64_t *id) { return net_social_pop_note(no_of(ctx)->social, type, id); }
static void no_friend_set_notify(void *ctx, void (*cb)(void *), void *arg) { net_social_set_notify(no_of(ctx)->social, cb, arg); }
static void no_friend_online_play(void *ctx, int open) { net_social_online_play(no_of(ctx)->social, open); }

/* ---- construction ---- */

OnlineProvider *net_online_new(const OnlineProvider *host, const NetOnlineServer *server, const char *state_dir) {
    if (!host || !server) return NULL;
    NetOnline *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    pthread_mutex_init(&o->mu, NULL);
    o->host = host;
    o->srv = *server;
    if (state_dir) snprintf(o->state_dir, sizeof o->state_dir, "%s", state_dir);
    OnlineProvider *t = &o->table;
    memcpy(t, host, host->struct_size < sizeof *t ? host->struct_size : sizeof *t);
    t->struct_size = sizeof *t;
    t->name = NET_ONLINE_NAME;
    t->caps = host->caps | ONLINE_CAP_NSD | ONLINE_CAP_ACCOUNT;
    t->dns_resolve = no_dns_resolve;
    t->dns_reverse = no_dns_reverse;
    t->nsd_resolve = no_nsd_resolve;
    t->sock_connect = no_sock_connect;
    t->sock_send = no_sock_send;
    t->sock_recv = no_sock_recv;
    t->sock_getpeername = no_sock_getpeername;
    t->sock_close = no_sock_close;
    t->sock_set_nonblock = no_sock_set_nonblock;
    t->sock_bind = no_sock_bind;
    t->tls_new = no_tls_new;
    t->tls_free = no_tls_free;
    t->tls_set_host = no_tls_set_host;
    t->tls_handshake = no_tls_handshake;
    t->tls_read = no_tls_read;
    t->tls_write = no_tls_write;
    t->tls_pending = no_tls_pending;
    t->tls_add_ca = no_tls_add_ca;
    t->tls_set_verify = no_tls_set_verify;
    t->account_info = no_account_info;
    t->account_id_token = no_account_id_token;
    t->tls_set_io_mode = no_tls_set_io_mode;
    t->report = no_report;
    NetSocialConfig sc;
    if (net_social_config(server, &sc) == 0 && (o->social = net_social_start(host, &sc, 1))) {
        t->caps |= ONLINE_CAP_FRIENDS;
        t->friend_count = no_friend_count;
        t->friend_list = no_friend_list;
        t->presence_update = no_presence_update;
        t->friend_list_info = no_friend_list_info;
        t->friend_find = no_friend_find;
        t->friend_request_count = no_friend_request_count;
        t->friend_pop_note = no_friend_pop_note;
        t->friend_set_notify = no_friend_set_notify;
        t->friend_online_play = no_friend_online_play;
        net_social_set_pid_listener(o->social, no_report_friend_pids, o);
        log_line("online", "napple social: friends and presence from %s:%u (account %s:%u)", sc.social.host, sc.social.port, sc.account.host, sc.account.port);
    }
    pthread_mutex_lock(&reg_mu);
    unsigned i = 0;
    while (i < 8 && reg[i]) i++;
    if (i < 8) reg[i] = o;
    pthread_mutex_unlock(&reg_mu);
    if (i == 8) {
        net_social_stop(o->social);
        free(o);
        return NULL;
    }
    return t;
}
void net_online_free(OnlineProvider *p) {
    if (!p) return;
    NetOnline *o = (NetOnline *)p; /* table is the first member */
    pthread_mutex_lock(&reg_mu);
    for (unsigned i = 0; i < 8; i++)
        if (reg[i] == o) reg[i] = NULL;
    pthread_mutex_unlock(&reg_mu);
    net_social_stop(o->social);
    pthread_mutex_destroy(&o->mu);
    free(o);
}

static const OnlineProvider *builtin;
static pthread_once_t builtin_once = PTHREAD_ONCE_INIT;
/* A normal exit signs the presence out (friends see "offline" at once instead of after three missed heartbeats). */
static void builtin_exit(void) {
    NetOnline *o = builtin && builtin != net_host_provider() ? (NetOnline *)builtin : NULL;
    if (o && o->social) net_social_quit(o->social); /* the cache stays readable for threads still running */
}
static void builtin_build(void) {
    builtin = net_host_provider();
    const char *url = getenv(NET_ONLINE_ENV_SERVER);
    if (!url || !*url) return;
    NetOnlineServer srv;
    if (!strcmp(url, "default")) url = NET_ONLINE_DEFAULT_SERVER; /* the community server; still opt-in */
    if (net_online_parse_server(url, &srv)) {
        log_line("online", "napple: %s='%s' is not an http:// or https:// URL: Napple online stays off", NET_ONLINE_ENV_SERVER, url);
        return;
    }
    char dir[1024] = "";
    const char *env = getenv(NET_ONLINE_ENV_STATE);
    const char *support = napple_support_dir();
    if (env && *env) snprintf(dir, sizeof dir, "%s", env);
    else if (support && *support) snprintf(dir, sizeof dir, "%s/online", support);
    OnlineProvider *p = net_online_new(builtin, &srv, dir[0] ? dir : NULL);
    if (!p) return;
    builtin = p;
    atexit(builtin_exit);
    log_line("online", "napple: online service %s://%s:%u (game-server names redirected there, other Nintendo names blocked)", srv.tls ? "https" : "http",
             srv.host, srv.port);
}
const OnlineProvider *net_online_builtin(void) {
    pthread_once(&builtin_once, builtin_build);
    return builtin;
}

/* NAPPLE_NET=host picks this table. net_host.c registers the bare host provider from its own constructor; Darwin runs constructors in link order
 * (net_host*.o before net_online*.o), so this one comes later and wins; net_online_builtin falls back to the host table when no server is set. */
__attribute__((constructor)) static void no_register(void) { online_builtin_host = net_online_builtin; }
