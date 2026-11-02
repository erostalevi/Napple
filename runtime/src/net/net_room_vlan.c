/* Napple rooms, the virtual LAN: guest UDP and TCP between consoles of a room (net_room.h).
 *
 * A guest socket joins the virtual LAN when, while the room is up, it is bound to INADDR_ANY or to the console's virtual address, or sends to / connects
 * to a virtual address or a broadcast. Its host descriptor is then bound to an ephemeral port (several consoles on one Mac all "bind" 49152), and the
 * virtual port is kept here:
 *   - UDP out: datagrams to virtual addresses and broadcasts become room packets (a broadcast also reaches this console's own sockets, as a BSD stack
 *     loops broadcasts back); UDP in: a room packet for one of our virtual ports is injected into that socket's host port from a loopback socket, with
 *     a 16-byte header {magic, per-process cookie, source address, source port} that nr_sock_recvfrom strips (it reports the virtual source).
 *   - TCP: a connect to a virtual address asks the peer (SYN), and once its listener exists (SYNACK) the guest's descriptor is connected over loopback
 *     to this module's splice listener; an incoming SYN for one of our listening virtual ports is answered by connecting a splice to that socket's
 *     host port. Bytes read from a splice are sent as DATA packets and DATA from the peer is written into it; FIN, RST and a member leaving map to
 *     shutdown / close.
 * Datagrams from anywhere else still reach such a socket unchanged (a socket may also talk to the internet). Not modelled: TCP to one's own virtual
 * address, IP options, multicast. Peers are not trusted: every length is checked. */
#include "net_room.h"
#include "net_host.h"
#include "hz_result.h"
#include "offline_core.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define VS_MAX 512 /* = net_host_sock.c's handle table */
#define ST_MAX 128
#define INJ_HDR 16u
#define INJ_MAGIC 0x494d524eu /* "NRMI" */
#define UDP_MAX (65507u - INJ_HDR)
#define PEND_MAX (1u << 20)
#define SYN_MS 5000
#define EPH_LO 50000
#define EPH_HI 60000

typedef struct {
    int used, fd, type, listening, connected;
    uint32_t bind_ip, peer_ip;
    uint16_t vport, real_port, peer_port;
    uint64_t seq; /* bind order: a unicast datagram for a port bound twice goes to the latest */
} VSock;

typedef struct {
    int used, relay, state; /* state 1: SYN sent, waiting; 2: open */
    int result;             /* connector: 0 waiting, 1 accepted, else an HZ errno */
    int fin_in, fin_out, shut;
    uint16_t lport, rport, splice_port;
    uint32_t rip;
    uint8_t *pend;
    size_t plen;
} Stream;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static VSock vs[VS_MAX];
static Stream st[ST_MAX];
static uint64_t seq;
static int inj_fd = -1, splice_fd = -1;
static uint16_t inj_port, splice_port;
static uint32_t cookie;
static uint16_t next_eph = EPH_LO;

static int nonblock(int fd) {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}
static uint16_t local_port(int fd) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    return getsockname(fd, (struct sockaddr *)&a, &l) == 0 && a.sin_family == AF_INET ? ntohs(a.sin_port) : 0;
}
static uint32_t addr_ip(const OnlineAddr *a) { return (uint32_t)a->addr[0] << 24 | (uint32_t)a->addr[1] << 16 | (uint32_t)a->addr[2] << 8 | a->addr[3]; }
static void set_addr(OnlineAddr *a, uint32_t ip, uint16_t port) {
    memset(a, 0, sizeof *a);
    a->family = BSD_AF_INET;
    a->port = port;
    a->addr[0] = (uint8_t)(ip >> 24), a->addr[1] = (uint8_t)(ip >> 16), a->addr[2] = (uint8_t)(ip >> 8), a->addr[3] = (uint8_t)ip;
}

/* The loopback endpoints, made once (lock held). */
static int ensure_endpoints(void) {
    if (inj_fd >= 0 && splice_fd >= 0) return 0;
    struct sockaddr_in lo = {.sin_len = sizeof lo, .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (inj_fd < 0) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0 || nonblock(fd) < 0 || bind(fd, (struct sockaddr *)&lo, sizeof lo) != 0) {
            if (fd >= 0) close(fd);
            return -1;
        }
        int big = 1 << 20;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
        inj_fd = fd;
        inj_port = local_port(fd);
        arc4random_buf(&cookie, sizeof cookie);
    }
    if (splice_fd < 0) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0 || nonblock(fd) < 0 || bind(fd, (struct sockaddr *)&lo, sizeof lo) != 0 || listen(fd, 64) != 0) {
            if (fd >= 0) close(fd);
            return -1;
        }
        splice_fd = fd;
        splice_port = local_port(fd);
    }
    return 0;
}

static VSock *vget(int h) { return h >= 0 && h < VS_MAX && vs[h].used ? &vs[h] : NULL; }
static int vport_taken(int type, uint16_t p, int except) {
    for (int i = 0; i < VS_MAX; i++)
        if (i != except && vs[i].used && vs[i].type == type && vs[i].vport == p) return 1;
    return 0;
}
static uint16_t alloc_eph(int type) {
    for (int n = 0; n < EPH_HI - EPH_LO; n++) {
        uint16_t p = next_eph++;
        if (next_eph >= EPH_HI) next_eph = EPH_LO;
        if (!vport_taken(type, p, -1)) return p;
    }
    return 0;
}
/* Puts h on the virtual LAN: its host descriptor is bound to an ephemeral port (unless already bound) and it gets virtual port `want` (0 = any). Lock
 * held. 0, or an HZ errno. */
static int attach(int h, int fd, int type, uint32_t bind_ip, uint16_t want) {
    if (h < 0 || h >= VS_MAX) return HZ_EBADF;
    if (ensure_endpoints() < 0) return HZ_ENOBUFS;
    VSock *v = &vs[h];
    if (v->used && v->vport) return HZ_EINVAL; /* bound already */
    if (want && type == SOCK_STREAM && vport_taken(type, want, h)) return HZ_EADDRINUSE;
    uint16_t rp = local_port(fd);
    if (!rp) {
        struct sockaddr_in any = {.sin_len = sizeof any, .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
        if (bind(fd, (struct sockaddr *)&any, sizeof any) != 0) return nh_errno(errno);
        rp = local_port(fd);
    }
    uint16_t p = want ? want : alloc_eph(type);
    if (!p || !rp) return HZ_EADDRNOTAVAIL;
    *v = (VSock){.used = 1, .fd = fd, .type = type, .bind_ip = bind_ip, .vport = p, .real_port = rp, .seq = ++seq};
    return 0;
}

/* ---- UDP ---------------------------------------------------------------------------------------------------------------------------------------------- */
/* Delivers a datagram to our sockets bound to (dst, dport) (lock held). A unicast goes to the latest bind, a broadcast to every one. */
static void udp_deliver(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *p, size_t n) {
    if (n > UDP_MAX || inj_fd < 0) return;
    uint32_t me = net_room_vip();
    int bcast = net_room_is_broadcast(dst);
    if (!bcast && dst != me) return;
    static uint8_t buf[INJ_HDR + 65536];
    uint32_t h[3] = {INJ_MAGIC, cookie, src};
    memcpy(buf, h, 12);
    memcpy(buf + 12, &sport, 2);
    memset(buf + 14, 0, 2);
    memcpy(buf + INJ_HDR, p, n);
    VSock *best = NULL;
    for (int i = 0; i < VS_MAX; i++) {
        VSock *v = &vs[i];
        if (!v->used || v->type != SOCK_DGRAM || v->vport != dport || (v->bind_ip && v->bind_ip != me)) continue;
        if (v->connected && (v->peer_ip != src || v->peer_port != sport)) continue;
        if (!bcast) {
            if (!best || v->seq > best->seq) best = v;
            continue;
        }
        struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(v->real_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        sendto(inj_fd, buf, INJ_HDR + n, 0, (struct sockaddr *)&to, sizeof to);
    }
    if (best) {
        struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(best->real_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        sendto(inj_fd, buf, INJ_HDR + n, 0, (struct sockaddr *)&to, sizeof to);
    }
}

ssize_t nr_sock_recvfrom(int h, int fd, void *out, size_t n, int flags, struct sockaddr *from, socklen_t *fromlen) {
    (void)h;
    static __thread uint8_t buf[INJ_HDR + 65536];
    for (;;) {
        struct sockaddr_in ss;
        socklen_t sl = sizeof ss;
        size_t want = n + INJ_HDR < sizeof buf ? n + INJ_HDR : sizeof buf;
        ssize_t k = recvfrom(fd, buf, want, flags, (struct sockaddr *)&ss, &sl);
        if (k < 0) return k;
        uint32_t magic = 0, ck = 0;
        if (k >= (ssize_t)INJ_HDR) memcpy(&magic, buf, 4), memcpy(&ck, buf + 4, 4);
        int injected = sl >= sizeof(struct sockaddr_in) && ss.sin_family == AF_INET && ss.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
                       ntohs(ss.sin_port) == inj_port && k >= (ssize_t)INJ_HDR && magic == INJ_MAGIC && ck == cookie;
        if (!injected) {
            size_t c = (size_t)k < n ? (size_t)k : n;
            memcpy(out, buf, c);
            if (from && fromlen) {
                socklen_t cl = *fromlen < sl ? *fromlen : sl;
                memcpy(from, &ss, cl);
                *fromlen = sl;
            }
            return (ssize_t)c;
        }
        uint32_t src;
        uint16_t sport;
        memcpy(&src, buf + 8, 4);
        memcpy(&sport, buf + 12, 2);
        size_t pl = (size_t)k - INJ_HDR, c = pl < n ? pl : n;
        memcpy(out, buf + INJ_HDR, c);
        if (from && fromlen) {
            struct sockaddr_in v = {.sin_len = sizeof v, .sin_family = AF_INET, .sin_port = htons(sport), .sin_addr.s_addr = htonl(src)};
            socklen_t cl = *fromlen < sizeof v ? *fromlen : (socklen_t)sizeof v;
            memcpy(from, &v, cl);
            *fromlen = sizeof v;
        }
        return (ssize_t)c;
    }
}

/* ---- TCP streams (lock held) ------------------------------------------------------------------------------------------------------------------------- */
static Stream *stream_find(uint16_t lport, uint32_t rip, uint16_t rport) {
    for (int i = 0; i < ST_MAX; i++)
        if (st[i].used && st[i].lport == lport && st[i].rip == rip && st[i].rport == rport) return &st[i];
    return NULL;
}
static Stream *stream_new(void) {
    for (int i = 0; i < ST_MAX; i++)
        if (!st[i].used) {
            st[i] = (Stream){.used = 1, .relay = -1};
            return &st[i];
        }
    return NULL;
}
static void stream_free(Stream *s) {
    if (s->relay >= 0) close(s->relay);
    free(s->pend);
    *s = (Stream){0};
    s->relay = -1;
}
static void tcp_ctl(Stream *s, uint8_t kind) { net_room_send_packet(s->rip, NR_BODY_TCP, kind, s->lport, s->rport, NULL, 0); }
static void stream_reset(Stream *s, int send_rst) {
    if (send_rst) tcp_ctl(s, NR_TCP_RST);
    if (s->state == 1) {
        s->result = HZ_ECONNREFUSED;
        pthread_cond_broadcast(&cond);
        s->state = 0; /* the waiting connector frees it */
        return;
    }
    stream_free(s);
}
/* Writes pending peer data into the splice; after the peer's FIN and an empty buffer, half-closes it. Closes the stream when both sides are done. */
static void stream_drain(Stream *s) {
    while (s->relay >= 0 && s->plen) {
        ssize_t k = send(s->relay, s->pend, s->plen, 0);
        if (k > 0) {
            memmove(s->pend, s->pend + k, s->plen - (size_t)k);
            s->plen -= (size_t)k;
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EINTR || errno == ENOTCONN)) return;
        stream_reset(s, 1);
        return;
    }
    if (s->relay >= 0 && s->fin_in && !s->plen && !s->shut) {
        shutdown(s->relay, SHUT_WR);
        s->shut = 1;
    }
    if (s->fin_in && s->fin_out && !s->plen) stream_free(s);
}
static void tcp_in(uint32_t src, uint8_t kind, uint16_t sport, uint16_t dport, const uint8_t *p, size_t n) {
    Stream *s = stream_find(dport, src, sport);
    if (kind == NR_TCP_SYN) {
        if (s) return; /* a retransmitted SYN */
        uint32_t me = net_room_vip();
        VSock *l = NULL;
        for (int i = 0; i < VS_MAX && !l; i++)
            if (vs[i].used && vs[i].type == SOCK_STREAM && vs[i].listening && vs[i].vport == dport && (!vs[i].bind_ip || vs[i].bind_ip == me)) l = &vs[i];
        Stream *ns = l ? stream_new() : NULL;
        int fd = ns ? socket(AF_INET, SOCK_STREAM, 0) : -1;
        struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(l ? l->real_port : 0), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        if (fd < 0 || nonblock(fd) < 0 || (connect(fd, (struct sockaddr *)&to, sizeof to) != 0 && errno != EINPROGRESS)) {
            if (fd >= 0) close(fd);
            if (ns) *ns = (Stream){0}, ns->relay = -1;
            net_room_send_packet(src, NR_BODY_TCP, NR_TCP_RST, dport, sport, NULL, 0);
            return;
        }
        *ns = (Stream){.used = 1, .relay = fd, .state = 2, .lport = dport, .rip = src, .rport = sport, .splice_port = local_port(fd)};
        tcp_ctl(ns, NR_TCP_SYNACK);
        return;
    }
    if (!s) {
        if (kind == NR_TCP_DATA) net_room_send_packet(src, NR_BODY_TCP, NR_TCP_RST, dport, sport, NULL, 0);
        return;
    }
    switch (kind) {
        case NR_TCP_SYNACK:
            if (s->state == 1) {
                s->state = 2;
                s->result = 1;
                pthread_cond_broadcast(&cond);
            }
            break;
        case NR_TCP_RST: stream_reset(s, 0); break;
        case NR_TCP_DATA:
            if (s->state != 2 || s->fin_in) break;
            if (s->plen + n > PEND_MAX) {
                stream_reset(s, 1);
                break;
            }
            if (n) {
                uint8_t *np = realloc(s->pend, s->plen + n);
                if (!np) {
                    stream_reset(s, 1);
                    break;
                }
                s->pend = np;
                memcpy(s->pend + s->plen, p, n);
                s->plen += n;
            }
            stream_drain(s);
            break;
        case NR_TCP_FIN:
            s->fin_in = 1;
            stream_drain(s);
            break;
    }
}

/* ---- inbound from the room ---------------------------------------------------------------------------------------------------------------------------- */
void nr_vlan_packet_in(uint32_t src, uint32_t dst, uint8_t proto, uint8_t kind, uint16_t sport, uint16_t dport, const uint8_t *p, size_t n) {
    pthread_mutex_lock(&lock);
    if (proto == NR_BODY_UDP) udp_deliver(src, sport, dst, dport, p, n);
    else if (proto == NR_BODY_TCP && dst == net_room_vip()) tcp_in(src, kind, sport, dport, p, n);
    pthread_mutex_unlock(&lock);
}
void nr_vlan_member_left(uint32_t vip) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < ST_MAX; i++)
        if (st[i].used && st[i].rip == vip) stream_reset(&st[i], 0);
    pthread_mutex_unlock(&lock);
}
void nr_vlan_room_down(void) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < ST_MAX; i++)
        if (st[i].used) stream_reset(&st[i], 0);
    pthread_mutex_unlock(&lock);
}

/* ---- worker side: the splice listener and the splices ---------------------------------------------------------------------------------------------- */
static int poll_map[256]; /* -1 the splice listener, else a stream index */
int nr_vlan_pollfds(struct pollfd *pf, int max) {
    int n = 0;
    pthread_mutex_lock(&lock);
    if (splice_fd >= 0 && n < max) pf[n] = (struct pollfd){splice_fd, POLLIN, 0}, poll_map[n++] = -1;
    for (int i = 0; i < ST_MAX && n < max; i++) {
        Stream *s = &st[i];
        if (!s->used || s->relay < 0 || s->state != 2) continue;
        short ev = (short)((s->fin_out ? 0 : POLLIN) | (s->plen ? POLLOUT : 0));
        if (!ev) continue;
        pf[n] = (struct pollfd){s->relay, ev, 0}, poll_map[n++] = i;
    }
    pthread_mutex_unlock(&lock);
    return n;
}
void nr_vlan_poll_done(struct pollfd *pf, int n) {
    pthread_mutex_lock(&lock);
    for (int k = 0; k < n; k++) {
        if (!pf[k].revents) continue;
        if (poll_map[k] < 0) { /* a connector's guest descriptor arriving: match it by its local port */
            for (;;) {
                struct sockaddr_in peer;
                socklen_t pl = sizeof peer;
                int fd = accept(splice_fd, (struct sockaddr *)&peer, &pl);
                if (fd < 0) break;
                uint16_t port = ntohs(peer.sin_port);
                Stream *s = NULL;
                for (int i = 0; i < ST_MAX && !s; i++)
                    if (st[i].used && st[i].relay < 0 && st[i].splice_port == port && st[i].state == 2) s = &st[i];
                if (!s || nonblock(fd) < 0) {
                    close(fd);
                    continue;
                }
                s->relay = fd;
                stream_drain(s);
            }
            continue;
        }
        Stream *s = &st[poll_map[k]];
        if (!s->used || s->relay != pf[k].fd) continue;
        if (pf[k].revents & POLLOUT) stream_drain(s);
        if (!s->used || !(pf[k].revents & (POLLIN | POLLHUP | POLLERR)) || s->fin_out) continue;
        static uint8_t buf[16384];
        ssize_t r = recv(s->relay, buf, sizeof buf, 0);
        if (r > 0) {
            if (net_room_send_packet(s->rip, NR_BODY_TCP, NR_TCP_DATA, s->lport, s->rport, buf, (size_t)r) < 0) stream_reset(s, 1); /* bytes lost: reset */
        } else if (r == 0) {
            s->fin_out = 1;
            tcp_ctl(s, NR_TCP_FIN);
            stream_drain(s);
        } else if (errno != EAGAIN && errno != EINTR) {
            stream_reset(s, 1);
        }
    }
    pthread_mutex_unlock(&lock);
}

/* ---- socket hooks ------------------------------------------------------------------------------------------------------------------------------------- */
int nr_sock_is_vlan(int h) {
    pthread_mutex_lock(&lock);
    int r = vget(h) != NULL;
    pthread_mutex_unlock(&lock);
    return r;
}

int nr_sock_bind(int h, int fd, int type, const OnlineAddr *a, int *r, int *err) {
    if (!net_room_vlan_on() || (a->family != BSD_AF_INET && a->family != 0) || (type != SOCK_DGRAM && type != SOCK_STREAM)) return 0;
    uint32_t ip = a->family ? addr_ip(a) : 0, me = net_room_vip();
    if (ip && ip != me) return 0;
    pthread_mutex_lock(&lock);
    int e = attach(h, fd, type, ip, a->port);
    pthread_mutex_unlock(&lock);
    *r = e ? -1 : 0;
    if (e) *err = e;
    return 1;
}
int nr_sock_listen(int h) {
    pthread_mutex_lock(&lock);
    VSock *v = vget(h);
    if (v && v->type == SOCK_STREAM) v->listening = 1;
    pthread_mutex_unlock(&lock);
    return v != NULL;
}

int nr_sock_connect(int h, int fd, int type, const OnlineAddr *a, int *r, int *err) {
    if (!net_room_vlan_on() || a->family != BSD_AF_INET) return 0;
    uint32_t dst = addr_ip(a);
    if (!net_room_is_vlan_dst(dst)) return 0;
    pthread_mutex_lock(&lock);
    VSock *v = vget(h);
    int e = v ? 0 : attach(h, fd, type, 0, 0);
    v = vget(h);
    if (!e && type == SOCK_DGRAM) {
        v->connected = 1, v->peer_ip = dst, v->peer_port = a->port;
        pthread_mutex_unlock(&lock);
        *r = 0;
        return 1;
    }
    if (!e && (v->connected || v->listening)) e = HZ_EISCONN;
    if (!e && (dst == net_room_vip() || net_room_is_broadcast(dst))) e = HZ_ECONNREFUSED; /* not modelled: TCP to oneself or a broadcast */
    Stream *s = e ? NULL : stream_new();
    if (!e && !s) e = HZ_ENOBUFS;
    if (e) {
        pthread_mutex_unlock(&lock);
        *r = -1, *err = e;
        return 1;
    }
    *s = (Stream){.used = 1, .relay = -1, .state = 1, .lport = v->vport, .rip = dst, .rport = a->port, .splice_port = v->real_port};
    tcp_ctl(s, NR_TCP_SYN);
    int tok;
    nh_core_away(&tok);
    int64_t end = nh_now_ms() + SYN_MS;
    while (!s->result && nh_now_ms() < end) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&cond, &lock, &ts);
    }
    nh_core_back(&tok);
    int res = s->result;
    if (res != 1) {
        if (s->used && s->state == 1) tcp_ctl(s, NR_TCP_RST); /* timed out: tell the peer to forget it */
        stream_free(s);
        pthread_mutex_unlock(&lock);
        *r = -1, *err = res ? res : HZ_ETIMEDOUT;
        return 1;
    }
    /* the peer accepted: connect the guest descriptor to the splice listener (loopback, immediate) */
    struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(splice_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int ok = connect(fd, (struct sockaddr *)&to, sizeof to) == 0 || errno == EINPROGRESS || errno == EISCONN;
    if (ok) {
        v->connected = 1, v->peer_ip = dst, v->peer_port = a->port;
    } else {
        stream_reset(s, 1);
    }
    pthread_mutex_unlock(&lock);
    if (ok) {
        struct pollfd pf = {fd, POLLOUT, 0};
        poll(&pf, 1, 1000);
    }
    *r = ok ? 0 : -1;
    if (!ok) *err = HZ_ECONNREFUSED;
    return 1;
}

int nr_sock_send(int h, int fd, int type, const void *buf, size_t n, const OnlineAddr *to, int64_t *r, int *err) {
    if (type != SOCK_DGRAM || !net_room_vlan_on()) return 0;
    pthread_mutex_lock(&lock);
    VSock *v = vget(h);
    uint32_t dst;
    uint16_t dport;
    if (to && to->family) {
        if (to->family != BSD_AF_INET) {
            pthread_mutex_unlock(&lock);
            return 0;
        }
        dst = addr_ip(to), dport = to->port;
    } else if (v && v->connected) {
        dst = v->peer_ip, dport = v->peer_port;
    } else {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    if (!net_room_is_vlan_dst(dst)) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    int e = v ? 0 : attach(h, fd, type, 0, 0);
    v = vget(h);
    if (!e && n > UDP_MAX) e = HZ_EMSGSIZE;
    if (e) {
        pthread_mutex_unlock(&lock);
        *r = -1, *err = e;
        return 1;
    }
    uint32_t me = net_room_vip();
    uint16_t sport = v->vport;
    if (dst == me || net_room_is_broadcast(dst)) udp_deliver(me, sport, dst, dport, buf, n); /* our own sockets hear it too */
    pthread_mutex_unlock(&lock);
    if (dst != me) net_room_send_packet(dst, NR_BODY_UDP, 0, sport, dport, buf, n); /* a full queue drops it, like a busy LAN */
    *r = (int64_t)n;
    return 1;
}

void nr_sock_accepted(int listen_h, int h, int fd, OnlineAddr *peer) {
    (void)fd;
    pthread_mutex_lock(&lock);
    VSock *l = vget(listen_h);
    if (l && peer && h >= 0 && h < VS_MAX) {
        uint16_t port = peer->port;
        for (int i = 0; i < ST_MAX; i++) {
            Stream *s = &st[i];
            if (!s->used || s->splice_port != port || s->lport != l->vport) continue;
            vs[h] = (VSock){.used = 1, .fd = fd, .type = SOCK_STREAM, .bind_ip = l->bind_ip, .vport = l->vport, .real_port = local_port(fd), .connected = 1,
                            .peer_ip = s->rip, .peer_port = s->rport, .seq = ++seq};
            set_addr(peer, s->rip, s->rport);
            break;
        }
    }
    pthread_mutex_unlock(&lock);
}

int nr_sock_name(int h, OnlineAddr *out, int peer, int *r, int *err) {
    pthread_mutex_lock(&lock);
    VSock *v = vget(h);
    if (!v) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    if (peer && !v->connected) {
        *r = -1, *err = HZ_ENOTCONN;
    } else {
        if (peer) set_addr(out, v->peer_ip, v->peer_port);
        else set_addr(out, v->bind_ip ? v->bind_ip : (v->connected ? net_room_vip() : 0), v->vport);
        *r = 0;
    }
    pthread_mutex_unlock(&lock);
    return 1;
}

void nr_sock_closed(int h) {
    pthread_mutex_lock(&lock);
    if (h >= 0 && h < VS_MAX) vs[h] = (VSock){0};
    pthread_mutex_unlock(&lock);
}
