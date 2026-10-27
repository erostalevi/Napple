#include "bsd_core.h"
#include "hz_result.h"
#include <string.h>

static BsdGuest global_guest;
BsdGuest *bsdg_global(void) { return &global_guest; }
static void ensure(BsdGuest *g) { /* caller holds nothing; the one-time init is racy only before the first socket call, which is single-threaded in a guest */
    if (!g->init) {
        pthread_mutex_init(&g->lock, NULL);
        for (int i = 0; i < BSD_MAX_FDS; i++) g->h[i] = -1;
        g->init = 1;
    }
}
int bsdg_handle(BsdGuest *g, int fd) {
    ensure(g);
    pthread_mutex_lock(&g->lock);
    int h = fd >= 0 && fd < BSD_MAX_FDS ? g->h[fd] : -1;
    pthread_mutex_unlock(&g->lock);
    return h;
}
void bsdg_reset(BsdGuest *g) {
    ensure(g);
    const OnlineProvider *p = online_provider();
    int (*cl)(void *, int, int *) = ONLINE_FN(p, sock_close);
    pthread_mutex_lock(&g->lock);
    for (int i = 0; i < BSD_MAX_FDS; i++)
        if (g->h[i] >= 0) {
            int e, h = g->h[i];
            if (cl) cl(p->ctx, h, &e);
            for (int j = i; j < BSD_MAX_FDS; j++) /* aliases made by DuplicateSocket share the one provider socket: closed once */
                if (g->h[j] == h) g->h[j] = -1, g->nb[j] = 0;
        }
    pthread_mutex_unlock(&g->lock);
}

#define PROV(var, field, errp, fail)                                                                                                                           \
    const OnlineProvider *P_ = online_provider();                                                                                                              \
    __typeof__(P_->field) var = ONLINE_FN(P_, field);                                                                                                          \
    if (!var) {                                                                                                                                                \
        *(errp) = HZ_EOPNOTSUPP;                                                                                                                               \
        return (fail);                                                                                                                                         \
    }

int bsdg_socket(BsdGuest *g, int domain, int type, int protocol, int *err) {
    ensure(g);
    PROV(open, sock_open, err, -1)
    int fd = -1;
    pthread_mutex_lock(&g->lock);
    for (int i = 0; i < BSD_MAX_FDS; i++)
        if (g->h[i] == -1) {
            g->h[i] = -2; /* reserved while the provider call runs */
            fd = i;
            break;
        }
    pthread_mutex_unlock(&g->lock);
    if (fd < 0) {
        *err = HZ_EMFILE;
        return -1;
    }
    int h = open(P_->ctx, domain, type, protocol, err);
    pthread_mutex_lock(&g->lock);
    g->h[fd] = h >= 0 ? h : -1;
    g->nb[fd] = 0;
    pthread_mutex_unlock(&g->lock);
    if (h < 0) return -1;
    *err = 0;
    return fd;
}
int bsdg_close(BsdGuest *g, int fd, int *err) {
    ensure(g);
    PROV(cl, sock_close, err, -1)
    pthread_mutex_lock(&g->lock);
    int h = fd >= 0 && fd < BSD_MAX_FDS ? g->h[fd] : -1;
    int alias = 0;
    if (h >= 0) {
        g->h[fd] = -1, g->nb[fd] = 0;
        for (int i = 0; i < BSD_MAX_FDS && !alias; i++) alias = g->h[i] == h;
    }
    pthread_mutex_unlock(&g->lock);
    if (h < 0) {
        *err = HZ_EBADF;
        return -1;
    }
    if (alias) { /* another descriptor still names the provider socket (DuplicateSocket): only the last close reaches the provider */
        *err = 0;
        return 0;
    }
    int r = cl(P_->ctx, h, err);
    if (r == 0) *err = 0;
    return r;
}

int bsdg_dup(BsdGuest *g, int fd, int *err) {
    ensure(g);
    int nfd = -1;
    pthread_mutex_lock(&g->lock);
    int h = fd >= 0 && fd < BSD_MAX_FDS ? g->h[fd] : -1;
    if (h >= 0)
        for (int i = 0; i < BSD_MAX_FDS; i++)
            if (g->h[i] == -1) {
                g->h[i] = h;
                g->nb[i] = g->nb[fd]; /* the file status flags belong to the shared open description */
                nfd = i;
                break;
            }
    pthread_mutex_unlock(&g->lock);
    *err = h < 0 ? HZ_EBADF : nfd < 0 ? HZ_EMFILE : 0;
    return nfd;
}

#define WITH_HANDLE(g, fd, err, fail)                                                                                                                          \
    ensure(g);                                                                                                                                                 \
    int h = bsdg_handle(g, fd);                                                                                                                                \
    if (h < 0) {                                                                                                                                               \
        *(err) = HZ_EBADF;                                                                                                                                     \
        return (fail);                                                                                                                                         \
    }

int bsdg_bind(BsdGuest *g, int fd, const OnlineAddr *a, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_bind, err, -1)
    int r = f(P_->ctx, h, a, err);
    if (r == 0) *err = 0;
    return r;
}
int bsdg_listen(BsdGuest *g, int fd, int backlog, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_listen, err, -1)
    int r = f(P_->ctx, h, backlog, err);
    if (r == 0) *err = 0;
    return r;
}
int bsdg_accept(BsdGuest *g, int fd, OnlineAddr *peer, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_accept, err, -1)
    int nh = f(P_->ctx, h, peer, err);
    if (nh < 0) return -1;
    int nfd = -1;
    pthread_mutex_lock(&g->lock);
    for (int i = 0; i < BSD_MAX_FDS; i++)
        if (g->h[i] == -1) {
            g->h[i] = nh;
            g->nb[i] = 0;
            nfd = i;
            break;
        }
    pthread_mutex_unlock(&g->lock);
    if (nfd < 0) {
        int e;
        int (*cl)(void *, int, int *) = ONLINE_FN(P_, sock_close);
        if (cl) cl(P_->ctx, nh, &e);
        *err = HZ_EMFILE;
        return -1;
    }
    *err = 0;
    return nfd;
}
int bsdg_connect(BsdGuest *g, int fd, const OnlineAddr *a, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_connect, err, -1)
    int r = f(P_->ctx, h, a, err);
    if (r == 0) *err = 0;
    return r;
}
int64_t bsdg_send(BsdGuest *g, int fd, const void *buf, size_t n, int flags, const OnlineAddr *to, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_send, err, -1)
    int64_t r = f(P_->ctx, h, buf, n, flags, to, err);
    if (r >= 0) *err = 0;
    return r;
}
int64_t bsdg_recv(BsdGuest *g, int fd, void *buf, size_t n, int flags, OnlineAddr *from, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_recv, err, -1)
    int64_t r = f(P_->ctx, h, buf, n, flags, from, err);
    if (r >= 0) *err = 0;
    return r;
}
int bsdg_shutdown(BsdGuest *g, int fd, int how, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_shutdown, err, -1)
    int r = f(P_->ctx, h, how, err);
    if (r == 0) *err = 0;
    return r;
}
int bsdg_getsockname(BsdGuest *g, int fd, OnlineAddr *out, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_getsockname, err, -1)
    int r = f(P_->ctx, h, out, err);
    if (r == 0) *err = 0;
    return r;
}
int bsdg_getpeername(BsdGuest *g, int fd, OnlineAddr *out, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_getpeername, err, -1)
    int r = f(P_->ctx, h, out, err);
    if (r == 0) *err = 0;
    return r;
}
int bsdg_setsockopt(BsdGuest *g, int fd, int level, int opt, const void *v, size_t n, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_setsockopt, err, -1)
    int r = f(P_->ctx, h, level, opt, v, n, err);
    if (r == 0) *err = 0;
    return r;
}
int bsdg_getsockopt(BsdGuest *g, int fd, int level, int opt, void *v, size_t *n, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_getsockopt, err, -1)
    int r = f(P_->ctx, h, level, opt, v, n, err);
    if (r == 0) *err = 0;
    return r;
}
int bsdg_nonblock(BsdGuest *g, int fd, int on, int *err) {
    WITH_HANDLE(g, fd, err, -1)
    PROV(f, sock_set_nonblock, err, -1)
    int r = f(P_->ctx, h, on, err);
    if (r == 0) *err = 0;
    return r;
}

int bsdg_poll(BsdGuest *g, OnlinePollFd *fds, unsigned n, int timeout_ms, int *err) {
    ensure(g);
    PROV(poll, sock_poll, err, -1)
    OnlinePollFd tmp[BSD_MAX_FDS];
    int src[BSD_MAX_FDS];
    unsigned m = 0;
    int ready = 0;
    for (unsigned i = 0; i < n; i++) {
        int h = fds[i].fd >= 0 ? bsdg_handle(g, fds[i].fd) : -1;
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue; /* a negative descriptor is ignored */
        if (h < 0 || m == BSD_MAX_FDS) {
            fds[i].revents = ONLINE_POLLNVAL;
            ready++;
            continue;
        }
        tmp[m] = (OnlinePollFd){h, fds[i].events, 0};
        src[m++] = (int)i;
    }
    if (m) {
        int r = poll(P_->ctx, tmp, m, timeout_ms, err);
        if (r < 0) return -1;
        for (unsigned k = 0; k < m; k++) {
            fds[src[k]].revents = tmp[k].revents;
            if (tmp[k].revents) {
                /* the provider's count and ours must agree; count entries rather than trust r */
                ready++;
            }
        }
    }
    *err = 0;
    return ready;
}
int bsdg_select(BsdGuest *g, int nfds, uint8_t *rd, uint8_t *wr, uint8_t *ex, int timeout_ms, int *err) {
    if (nfds < 0 || nfds > BSD_MAX_FDS) {
        *err = HZ_EINVAL;
        return -1;
    }
    OnlinePollFd fds[BSD_MAX_FDS];
    unsigned n = 0;
    for (int i = 0; i < nfds; i++) {
        int in_r = rd && (rd[i / 8] >> (i % 8) & 1), in_w = wr && (wr[i / 8] >> (i % 8) & 1), in_e = ex && (ex[i / 8] >> (i % 8) & 1);
        if (!in_r && !in_w && !in_e) continue;
        fds[n++] = (OnlinePollFd){i, (int16_t)((in_r ? ONLINE_POLLIN : 0) | (in_w ? ONLINE_POLLOUT : 0) | (in_e ? ONLINE_POLLPRI : 0)), 0};
    }
    int r = bsdg_poll(g, fds, n, timeout_ms, err);
    if (r < 0) return -1;
    size_t bytes = ((size_t)nfds + 7) / 8;
    if (rd) memset(rd, 0, bytes);
    if (wr) memset(wr, 0, bytes);
    if (ex) memset(ex, 0, bytes);
    int count = 0;
    for (unsigned k = 0; k < n; k++) {
        int fd = fds[k].fd, any = 0;
        if (fds[k].revents & ONLINE_POLLNVAL) {
            *err = HZ_EBADF;
            return -1;
        }
        if (rd && (fds[k].revents & (ONLINE_POLLIN | ONLINE_POLLHUP | ONLINE_POLLERR))) rd[fd / 8] |= (uint8_t)(1 << (fd % 8)), count++, any = 1;
        if (wr && (fds[k].revents & ONLINE_POLLOUT)) wr[fd / 8] |= (uint8_t)(1 << (fd % 8)), count++, any = 1;
        if (ex && (fds[k].revents & ONLINE_POLLPRI)) ex[fd / 8] |= (uint8_t)(1 << (fd % 8)), count++, any = 1;
        (void)any;
    }
    *err = 0;
    return count;
}

int bsd_sockaddr_decode(const uint8_t *p, size_t n, OnlineAddr *out) {
    memset(out, 0, sizeof *out);
    if (!p || n < 2) return -1;
    uint8_t fam = p[1];
    if (fam == BSD_AF_INET && n >= 8) {
        out->family = BSD_AF_INET;
        out->port = (uint16_t)(p[2] << 8 | p[3]);
        memcpy(out->addr, p + 4, 4);
        return 0;
    }
    if (fam == BSD_AF_INET6 && n >= 24) {
        out->family = BSD_AF_INET6;
        out->port = (uint16_t)(p[2] << 8 | p[3]);
        memcpy(out->addr, p + 8, 16);
        return 0;
    }
    return -1;
}
size_t bsd_sockaddr_encode(const OnlineAddr *a, uint8_t *out, size_t cap) {
    uint8_t tmp[28] = {0};
    size_t len = a->family == BSD_AF_INET6 ? 28 : 16;
    tmp[0] = (uint8_t)len;
    tmp[1] = a->family ? a->family : BSD_AF_INET;
    tmp[2] = (uint8_t)(a->port >> 8);
    tmp[3] = (uint8_t)a->port;
    if (a->family == BSD_AF_INET6) memcpy(tmp + 8, a->addr, 16);
    else memcpy(tmp + 4, a->addr, 4);
    memcpy(out, tmp, cap < len ? cap : len);
    return len;
}
