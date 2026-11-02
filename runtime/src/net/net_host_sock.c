/* Host provider, bsd sockets: guest sockets are real host sockets.
 *
 * Every host descriptor is non-blocking; the guest's blocking mode (Fcntl / Ioctl FIONBIO, SOCK_NONBLOCK, MSG_DONTWAIT) and its SO_RCVTIMEO / SO_SNDTIMEO
 * are applied here by waiting in short poll slices. So a Close or ShutdownAllSockets from another guest thread ends a wait (the waiter gets EBADF), and
 * a waiting thread gives its virtual core up. Option levels, option names and flags are FreeBSD's, which Darwin shares except where mapped below. */
#include "net_host.h"
#include "net_room.h"
#include "hz_result.h"
#include "offline_core.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define NH_MAX_SOCKS 512
#define NH_SLICE_MS 50
#define NH_CONNECT_MS 75000 /* a blocking connect gives up like the host's own TCP stack would */

struct NhSock {
    int fd, type, nb;
    int refs;    /* table reference + one per call in flight (under tab_lock) */
    int closing; /* closed by the guest: waits end, the host descriptor closes with the last reference */
    int rcvtimeo_ms, sndtimeo_ms; /* 0 = none */
    OnlineAddr trace_to, trace_from; /* NAPPLE_NET_TRACE: the last destination / source logged */
};
static NhSock *tab[NH_MAX_SOCKS];
static pthread_mutex_t tab_lock = PTHREAD_MUTEX_INITIALIZER;

NhSock *nh_sock_get(int h) {
    NhSock *s = NULL;
    pthread_mutex_lock(&tab_lock);
    if (h >= 0 && h < NH_MAX_SOCKS && tab[h] && !tab[h]->closing) {
        s = tab[h];
        s->refs++;
    }
    pthread_mutex_unlock(&tab_lock);
    return s;
}
void nh_sock_put(NhSock *s) {
    if (!s) return;
    pthread_mutex_lock(&tab_lock);
    int last = --s->refs == 0;
    pthread_mutex_unlock(&tab_lock);
    if (last) {
        close(s->fd);
        free(s);
    }
}
int nh_sock_fd(const NhSock *s) { return s->fd; }
int nh_sock_nonblocking(const NhSock *s) { return s->nb; }

/* NAPPLE_NET_TRACE=1: one log line per bind / connect / sendto destination / option (addresses and ports), for finding out what a title's network mode does. */
static int trace_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("NAPPLE_NET_TRACE");
        v = e && *e && *e != '0';
    }
    return v;
}
static const char *addr_str(const OnlineAddr *a, char *b, size_t n) {
    if (!a) snprintf(b, n, "-");
    else if (a->family == BSD_AF_INET6) snprintf(b, n, "[v6]:%u", a->port);
    else snprintf(b, n, "%u.%u.%u.%u:%u", a->addr[0], a->addr[1], a->addr[2], a->addr[3], a->port);
    return b;
}
#define TRACE(...)                                                                                                                                             \
    do {                                                                                                                                                       \
        if (trace_on()) log_line("net", __VA_ARGS__);                                                                                                          \
    } while (0)

static int closing(NhSock *s) { return __atomic_load_n(&s->closing, __ATOMIC_ACQUIRE); }

int nh_sock_wait(NhSock *s, int events, int timeout_ms) {
    int64_t end = timeout_ms < 0 ? -1 : nh_now_ms() + timeout_ms;
    int tok, r = 0;
    nh_core_away(&tok);
    for (;;) {
        if (closing(s)) {
            r = -1;
            break;
        }
        int slice = NH_SLICE_MS;
        if (end >= 0) {
            int64_t left = end - nh_now_ms();
            if (left <= 0) break;
            if (left < slice) slice = (int)left;
        }
        struct pollfd p = {s->fd, (short)events, 0};
        int n = poll(&p, 1, slice);
        if (n > 0) {
            r = 1;
            break;
        }
        if (n < 0 && errno != EINTR) {
            r = 1; /* let the call itself report the error */
            break;
        }
    }
    nh_core_back(&tok);
    return r;
}

static int fail(int *err, int e) {
    *err = e;
    return -1;
}
static int fail_host(int *err) { return fail(err, nh_errno(errno)); }

static int host_setup(int fd) {
    int one = 1;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); /* a write to a closed peer is EPIPE for the guest, never a signal for the runtime */
    return 0;
}
static int insert(int fd, int type, int nb, int *err) {
    NhSock *s = calloc(1, sizeof *s);
    if (!s) {
        close(fd);
        return fail(err, HZ_ENOMEM);
    }
    *s = (NhSock){.fd = fd, .type = type, .nb = nb, .refs = 1};
    pthread_mutex_lock(&tab_lock);
    int h = -1;
    for (int i = 0; i < NH_MAX_SOCKS; i++)
        if (!tab[i]) {
            tab[i] = s;
            h = i;
            break;
        }
    pthread_mutex_unlock(&tab_lock);
    if (h < 0) {
        close(fd);
        free(s);
        return fail(err, HZ_EMFILE);
    }
    *err = 0;
    return h;
}

#define BSD_SOCK_NONBLOCK 0x20000000
#define BSD_SOCK_CLOEXEC 0x10000000
int nh_sock_open(void *ctx, int domain, int type, int protocol, int *err) {
    (void)ctx;
    int nb = (type & BSD_SOCK_NONBLOCK) != 0;
    type &= ~(BSD_SOCK_NONBLOCK | BSD_SOCK_CLOEXEC);
    int hd = domain == BSD_AF_INET ? AF_INET : domain == BSD_AF_INET6 ? AF_INET6 : -1;
    if (hd < 0) return fail(err, HZ_EAFNOSUPPORT);
    int ht = type == BSD_SOCK_STREAM ? SOCK_STREAM : type == BSD_SOCK_DGRAM ? SOCK_DGRAM : type == BSD_SOCK_RAW ? SOCK_RAW : -1;
    if (ht < 0) return fail(err, HZ_EPROTONOSUPPORT);
    int fd = socket(hd, ht, protocol);
    if (fd < 0) return fail_host(err);
    if (host_setup(fd) < 0) {
        int e = nh_errno(errno);
        close(fd);
        return fail(err, e);
    }
    return insert(fd, ht, nb, err);
}

int nh_sock_close(void *ctx, int h, int *err) {
    (void)ctx;
    NhSock *s = NULL;
    pthread_mutex_lock(&tab_lock);
    if (h >= 0 && h < NH_MAX_SOCKS && tab[h]) {
        s = tab[h];
        tab[h] = NULL;
        __atomic_store_n(&s->closing, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&tab_lock);
    if (!s) return fail(err, HZ_EBADF);
    nr_sock_closed(h); /* rooms: forget its virtual-LAN port */
    nh_sock_put(s); /* the table's reference: calls still waiting see `closing` and drop theirs */
    *err = 0;
    return 0;
}
void nh_sock_close_all(void) {
    for (int i = 0; i < NH_MAX_SOCKS; i++) {
        int e;
        nh_sock_close(NULL, i, &e);
    }
}

#define WITH(s, h, err)                                                                                                                                        \
    NhSock *s = nh_sock_get(h);                                                                                                                                \
    if (!s) return fail(err, HZ_EBADF);

int nh_sock_bind(void *ctx, int h, const OnlineAddr *a, int *err) {
    (void)ctx;
    WITH(s, h, err)
    struct sockaddr_storage ss;
    socklen_t len;
    int e = 0, r = -1;
    if (nr_sock_bind(h, s->fd, s->type, a, &r, err)) { /* rooms: a virtual-LAN address */
    } else if ((e = nh_addr_to_host(a, &ss, &len))) *err = e;
    else if (bind(s->fd, (struct sockaddr *)&ss, len) == 0) r = 0;
    else *err = nh_errno(errno);
    char tb[64];
    TRACE("sock %d (%s) bind %s -> %s", h, s->type == SOCK_STREAM ? "tcp" : "udp", addr_str(a, tb, sizeof tb), r ? "error" : "ok");
    nh_sock_put(s);
    return r;
}
int nh_sock_listen(void *ctx, int h, int backlog, int *err) {
    (void)ctx;
    WITH(s, h, err)
    int r = listen(s->fd, backlog) == 0 ? 0 : fail_host(err);
    if (r == 0) nr_sock_listen(h);
    nh_sock_put(s);
    return r;
}
int nh_sock_accept(void *ctx, int h, OnlineAddr *peer, int *err) {
    (void)ctx;
    WITH(s, h, err)
    int r = -1;
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t len = sizeof ss;
        int fd = accept(s->fd, (struct sockaddr *)&ss, &len);
        if (fd >= 0) {
            if (host_setup(fd) < 0) {
                *err = nh_errno(errno);
                close(fd);
                break;
            }
            OnlineAddr tmp;
            if (!peer) peer = &tmp;
            nh_addr_from_host((struct sockaddr *)&ss, peer);
            r = insert(fd, s->type, 0, err);
            if (r >= 0) nr_sock_accepted(h, r, fd, peer); /* rooms: a spliced virtual-LAN connection reports its virtual peer */
            break;
        }
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno != EAGAIN || s->nb) {
            *err = nh_errno(errno);
            break;
        }
        int w = nh_sock_wait(s, POLLIN, s->rcvtimeo_ms ? s->rcvtimeo_ms : -1);
        if (w <= 0) {
            *err = w < 0 ? HZ_EBADF : HZ_EAGAIN;
            break;
        }
    }
    nh_sock_put(s);
    return r;
}
int nh_sock_connect(void *ctx, int h, const OnlineAddr *a, int *err) {
    (void)ctx;
    WITH(s, h, err)
    struct sockaddr_storage ss;
    socklen_t len;
    int r = -1, e = 0;
    if (nr_sock_connect(h, s->fd, s->type, a, &r, err)) { /* rooms: a virtual-LAN peer */
    } else if ((e = nh_addr_to_host(a, &ss, &len))) {
        *err = e;
    } else if (connect(s->fd, (struct sockaddr *)&ss, len) == 0) {
        r = 0;
    } else if (errno != EINPROGRESS || s->nb) {
        *err = nh_errno(errno);
    } else { /* blocking connect: wait for the handshake, then read its outcome */
        int w = nh_sock_wait(s, POLLOUT, s->sndtimeo_ms ? s->sndtimeo_ms : NH_CONNECT_MS);
        int so = 0;
        socklen_t sl = sizeof so;
        if (w < 0) *err = HZ_EBADF;
        else if (w == 0) *err = HZ_ETIMEDOUT;
        else if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &so, &sl) < 0) *err = nh_errno(errno);
        else if (so) *err = nh_errno(so);
        else r = 0;
    }
    char tb[64];
    TRACE("sock %d connect %s -> %s (err %d)", h, addr_str(a, tb, sizeof tb), r ? "error" : "ok", r ? *err : 0);
    nh_sock_put(s);
    return r;
}

/* send / recv flags: FreeBSD's MSG_OOB 0x1, PEEK 0x2, DONTROUTE 0x4, EOR 0x8, WAITALL 0x40, DONTWAIT 0x80 (Darwin's are the same); MSG_NOSIGNAL (0x20000)
 * is implied (SO_NOSIGPIPE). */
#define BSD_MSG_WAITALL 0x40
#define BSD_MSG_DONTWAIT 0x80
int64_t nh_sock_send(void *ctx, int h, const void *buf, size_t n, int flags, const OnlineAddr *to, int *err) {
    (void)ctx;
    NhSock *s = nh_sock_get(h);
    if (!s) return fail(err, HZ_EBADF);
    int64_t vr;
    if (nr_sock_send(h, s->fd, s->type, buf, n, to, &vr, err)) { /* rooms: a datagram for the virtual LAN */
        nh_sock_put(s);
        return vr;
    }
    struct sockaddr_storage ss;
    socklen_t len = 0;
    if (to && to->family) {
        int e = nh_addr_to_host(to, &ss, &len);
        if (e) {
            nh_sock_put(s);
            return fail(err, e);
        }
    }
    if (trace_on() && to && to->family && memcmp(&s->trace_to, to, sizeof *to)) {
        char tb[64];
        s->trace_to = *to;
        TRACE("sock %d sendto %s (%zu bytes)", h, addr_str(to, tb, sizeof tb), n);
    }
    int dontwait = s->nb || (flags & BSD_MSG_DONTWAIT);
    int hflags = flags & (MSG_OOB | MSG_DONTROUTE | MSG_EOR);
    size_t done = 0;
    int64_t r = -1;
    for (;;) {
        ssize_t k = len ? sendto(s->fd, (const uint8_t *)buf + done, n - done, hflags, (struct sockaddr *)&ss, len)
                        : send(s->fd, (const uint8_t *)buf + done, n - done, hflags);
        if (k >= 0) {
            done += (size_t)k;
            if (done >= n || s->type != SOCK_STREAM || dontwait) { /* a blocking stream send returns once everything is queued, as on the console */
                r = (int64_t)done;
                break;
            }
            continue;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN || dontwait) {
            if (done) r = (int64_t)done;
            else *err = nh_errno(errno);
            TRACE("sock %d send of %zu bytes failed: host errno %d", h, n, errno);
            break;
        }
        int w = nh_sock_wait(s, POLLOUT, s->sndtimeo_ms ? s->sndtimeo_ms : -1);
        if (w <= 0) {
            if (done) r = (int64_t)done;
            else *err = w < 0 ? HZ_EBADF : HZ_EAGAIN;
            break;
        }
    }
    nh_sock_put(s);
    return r;
}
int64_t nh_sock_recv(void *ctx, int h, void *buf, size_t n, int flags, OnlineAddr *from, int *err) {
    (void)ctx;
    NhSock *s = nh_sock_get(h);
    if (!s) return fail(err, HZ_EBADF);
    int dontwait = s->nb || (flags & BSD_MSG_DONTWAIT);
    int waitall = (flags & BSD_MSG_WAITALL) && s->type == SOCK_STREAM && !dontwait && !(flags & MSG_PEEK);
    int hflags = flags & (MSG_OOB | MSG_PEEK);
    int vlan = s->type == SOCK_DGRAM && nr_sock_is_vlan(h); /* rooms: datagrams injected from the virtual LAN carry a header */
    size_t done = 0;
    int64_t r = -1;
    if (from) memset(from, 0, sizeof *from);
    for (;;) {
        struct sockaddr_storage ss;
        ss.ss_family = AF_UNSPEC; /* a connected stream may leave the address untouched */
        socklen_t len = sizeof ss;
        ssize_t k = vlan ? nr_sock_recvfrom(h, s->fd, (uint8_t *)buf + done, n - done, hflags, from ? (struct sockaddr *)&ss : NULL, from ? &len : NULL)
                         : recvfrom(s->fd, (uint8_t *)buf + done, n - done, hflags, from ? (struct sockaddr *)&ss : NULL, from ? &len : NULL);
        if (k >= 0) {
            if (from && len) {
                nh_addr_from_host((struct sockaddr *)&ss, from);
                if (trace_on() && from->family && memcmp(&s->trace_from, from, sizeof *from)) {
                    char tb[64];
                    s->trace_from = *from;
                    TRACE("sock %d recvfrom %s (%zd bytes)", h, addr_str(from, tb, sizeof tb), k);
                }
            }
            done += (size_t)k;
            if (!waitall || k == 0 || done >= n) {
                r = (int64_t)done;
                break;
            }
            continue;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN || dontwait) {
            if (done) r = (int64_t)done;
            else *err = nh_errno(errno);
            if (errno != EAGAIN) TRACE("sock %d receive failed: host errno %d", h, errno);
            break;
        }
        int w = nh_sock_wait(s, POLLIN, s->rcvtimeo_ms ? s->rcvtimeo_ms : -1);
        if (w <= 0) {
            if (done) r = (int64_t)done;
            else *err = w < 0 ? HZ_EBADF : HZ_EAGAIN;
            break;
        }
    }
    nh_sock_put(s);
    return r;
}
int nh_sock_shutdown(void *ctx, int h, int how, int *err) {
    (void)ctx;
    WITH(s, h, err)
    int r = shutdown(s->fd, how) == 0 ? 0 : fail_host(err);
    nh_sock_put(s);
    return r;
}
static int name_common(int h, OnlineAddr *out, int peer, int *err) {
    WITH(s, h, err)
    int vr;
    if (nr_sock_name(h, out, peer, &vr, err)) { /* rooms: the virtual address and port */
        nh_sock_put(s);
        return vr;
    }
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    int r = (peer ? getpeername(s->fd, (struct sockaddr *)&ss, &len) : getsockname(s->fd, (struct sockaddr *)&ss, &len)) == 0 ? 0 : fail_host(err);
    if (r == 0) nh_addr_from_host((struct sockaddr *)&ss, out);
    nh_sock_put(s);
    return r;
}
int nh_sock_getsockname(void *ctx, int h, OnlineAddr *out, int *err) {
    (void)ctx;
    return name_common(h, out, 0, err);
}
int nh_sock_getpeername(void *ctx, int h, OnlineAddr *out, int *err) {
    (void)ctx;
    return name_common(h, out, 1, err);
}

/* Option translation. FreeBSD and Darwin share SOL_SOCKET (0xffff), IPPROTO_IP / TCP / IPV6 and most names; the differences are mapped, the guest's
 * receive / send timeouts are kept here (the host descriptor is non-blocking), and options this host does not have are accepted and ignored once logged. */
#define BSD_SOL_SOCKET 0xffff
#define BSD_SO_NOSIGPIPE 0x0800
#define BSD_SO_SNDTIMEO 0x1005
#define BSD_SO_RCVTIMEO 0x1006
#define BSD_TCP_KEEPIDLE 0x100
#define BSD_TCP_KEEPINTVL 0x200
#define BSD_TCP_KEEPCNT 0x400
typedef enum { OPT_PASS, OPT_LOCAL, OPT_IGNORE } OptKind;
static OptKind map_opt(int level, int opt, int *hl, int *ho) {
    *hl = level, *ho = opt;
    if (level == BSD_SOL_SOCKET) {
        *hl = SOL_SOCKET;
        switch (opt) {
            case BSD_SO_SNDTIMEO:
            case BSD_SO_RCVTIMEO: return OPT_LOCAL;
            case BSD_SO_NOSIGPIPE: return OPT_IGNORE; /* always on */
            case SO_DEBUG: case SO_ACCEPTCONN: case SO_REUSEADDR: case SO_KEEPALIVE: case SO_DONTROUTE: case SO_BROADCAST: case SO_LINGER:
            case SO_OOBINLINE: case SO_REUSEPORT: case SO_SNDBUF: case SO_RCVBUF: case SO_SNDLOWAT: case SO_RCVLOWAT: case SO_ERROR: case SO_TYPE:
                return OPT_PASS;
            default: return OPT_IGNORE;
        }
    }
    if (level == IPPROTO_TCP) {
        switch (opt) {
            case TCP_NODELAY: case TCP_MAXSEG: case TCP_NOPUSH: case TCP_NOOPT: return OPT_PASS;
            case BSD_TCP_KEEPIDLE: *ho = TCP_KEEPALIVE; return OPT_PASS;
            case BSD_TCP_KEEPINTVL: *ho = TCP_KEEPINTVL; return OPT_PASS;
            case BSD_TCP_KEEPCNT: *ho = TCP_KEEPCNT; return OPT_PASS;
            default: return OPT_IGNORE;
        }
    }
    if (level == IPPROTO_IP) {
        switch (opt) {
            case IP_HDRINCL: case IP_TOS: case IP_TTL: case IP_MULTICAST_IF: case IP_MULTICAST_TTL: case IP_MULTICAST_LOOP: case IP_ADD_MEMBERSHIP:
            case IP_DROP_MEMBERSHIP: return OPT_PASS;
            default: return OPT_IGNORE;
        }
    }
    if (level == IPPROTO_IPV6) {
        switch (opt) {
            case IPV6_V6ONLY: case IPV6_JOIN_GROUP: case IPV6_LEAVE_GROUP: case IPV6_MULTICAST_IF: case IPV6_MULTICAST_HOPS: case IPV6_MULTICAST_LOOP:
            case IPV6_UNICAST_HOPS: return OPT_PASS;
            default: return OPT_IGNORE;
        }
    }
    return OPT_IGNORE;
}
static void log_ignored(int level, int opt, const char *what) {
    static uint32_t seen[32];
    static unsigned nseen;
    uint32_t key = (uint32_t)level << 16 ^ (uint32_t)opt;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == key) return;
    if (nseen < 32) seen[nseen++] = key;
    log_line("net", "host provider: %s of option level 0x%x name 0x%x has no host equivalent: accepted, ignored", what, level, opt);
}
/* struct timeval of the guest (LP64: s64 seconds, s64 microseconds) <-> milliseconds */
static int tv_to_ms(const void *v, size_t n) {
    int64_t sec = 0, usec = 0;
    if (n >= 8) memcpy(&sec, v, 8);
    if (n >= 16) memcpy(&usec, (const uint8_t *)v + 8, 8);
    else if (n >= 12) {
        int32_t u;
        memcpy(&u, (const uint8_t *)v + 8, 4);
        usec = u;
    }
    if (sec < 0 || usec < 0) return 0;
    int64_t ms = sec * 1000 + (usec + 999) / 1000;
    return ms > 0x7fffffff ? 0x7fffffff : (int)ms;
}
int nh_sock_setsockopt(void *ctx, int h, int level, int opt, const void *val, size_t n, int *err) {
    (void)ctx;
    WITH(s, h, err)
    int hl, ho, r = 0;
    if (trace_on()) {
        int v = 0;
        if (val && n >= 4) memcpy(&v, val, 4);
        TRACE("sock %d setsockopt level 0x%x name 0x%x value %d (%zu bytes)", h, level, opt, v, n);
    }
    switch (map_opt(level, opt, &hl, &ho)) {
        case OPT_LOCAL:
            if (!val || n < 8) r = fail(err, HZ_EINVAL);
            else if (opt == BSD_SO_RCVTIMEO) s->rcvtimeo_ms = tv_to_ms(val, n);
            else s->sndtimeo_ms = tv_to_ms(val, n);
            break;
        case OPT_IGNORE:
            if (!(level == BSD_SOL_SOCKET && opt == BSD_SO_NOSIGPIPE)) log_ignored(level, opt, "SetSockOpt");
            break;
        case OPT_PASS:
            if (setsockopt(s->fd, hl, ho, val, (socklen_t)n) != 0) r = fail_host(err);
            break;
    }
    nh_sock_put(s);
    return r;
}
int nh_sock_getsockopt(void *ctx, int h, int level, int opt, void *val, size_t *n, int *err) {
    (void)ctx;
    WITH(s, h, err)
    int hl, ho, r = 0;
    switch (map_opt(level, opt, &hl, &ho)) {
        case OPT_LOCAL: {
            int ms = opt == BSD_SO_RCVTIMEO ? s->rcvtimeo_ms : s->sndtimeo_ms;
            int64_t tv[2] = {ms / 1000, (int64_t)(ms % 1000) * 1000};
            size_t k = *n < sizeof tv ? *n : sizeof tv;
            memcpy(val, tv, k);
            *n = k;
            break;
        }
        case OPT_IGNORE:
            log_ignored(level, opt, "GetSockOpt");
            r = fail(err, HZ_ENOPROTOOPT);
            break;
        case OPT_PASS: {
            socklen_t len = (socklen_t)*n;
            if (getsockopt(s->fd, hl, ho, val, &len) != 0) r = fail_host(err);
            else {
                *n = len;
                if (hl == SOL_SOCKET && ho == SO_ERROR && len >= sizeof(int)) { /* the pending error, in the guest's numbering */
                    int e;
                    memcpy(&e, val, sizeof e);
                    e = nh_errno(e);
                    memcpy(val, &e, sizeof e);
                }
            }
            break;
        }
    }
    nh_sock_put(s);
    return r;
}
int nh_sock_set_nonblock(void *ctx, int h, int on, int *err) {
    (void)ctx;
    WITH(s, h, err)
    s->nb = on != 0;
    nh_sock_put(s);
    *err = 0;
    return 0;
}

/* poll over provider handles: events and revents use the FreeBSD bits, which Darwin shares (POLLIN 1, PRI 2, OUT 4, ERR 8, HUP 0x10, NVAL 0x20, RDNORM 0x40,
 * RDBAND 0x80, WRBAND 0x100). Waits in slices so sockets closed meanwhile report POLLNVAL instead of hanging the caller. */
#define NH_POLL_MAX 256
int nh_sock_poll(void *ctx, OnlinePollFd *fds, unsigned n, int timeout_ms, int *err) {
    (void)ctx;
    if (n > NH_POLL_MAX) return fail(err, HZ_EINVAL);
    NhSock *ss[NH_POLL_MAX];
    struct pollfd pf[NH_POLL_MAX];
    int ready = 0;
    for (unsigned i = 0; i < n; i++) {
        fds[i].revents = 0;
        ss[i] = fds[i].fd >= 0 ? nh_sock_get(fds[i].fd) : NULL;
        pf[i] = (struct pollfd){ss[i] ? ss[i]->fd : -1, (short)(fds[i].events & 0x1ff), 0};
        if (fds[i].fd >= 0 && !ss[i]) fds[i].revents = ONLINE_POLLNVAL, ready++;
    }
    int64_t end = timeout_ms < 0 ? -1 : nh_now_ms() + timeout_ms;
    int tok = 0, away = 0;
    while (!ready) {
        int slice = NH_SLICE_MS;
        if (end >= 0) {
            int64_t left = end - nh_now_ms();
            if (left < slice) slice = left > 0 ? (int)left : 0;
        }
        if (slice && !away) nh_core_away(&tok), away = 1;
        int k = poll(pf, n, slice);
        if (k < 0 && errno != EINTR) {
            *err = nh_errno(errno);
            ready = -1;
            break;
        }
        for (unsigned i = 0; i < n; i++) {
            if (ss[i] && closing(ss[i])) pf[i].revents = POLLNVAL;
            if (ss[i] && pf[i].revents) fds[i].revents = (int16_t)(pf[i].revents & 0x1ff), ready++;
        }
        if (ready || (end >= 0 && nh_now_ms() >= end)) break;
    }
    if (away) nh_core_back(&tok);
    for (unsigned i = 0; i < n; i++) nh_sock_put(ss[i]);
    if (ready >= 0) *err = 0;
    return ready;
}
