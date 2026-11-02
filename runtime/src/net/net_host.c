/* The built-in host provider: the table, its instances, and the helpers every part shares (errno and address conversion, giving the virtual
 * core away while waiting). Registered with online_core.c from a constructor, so NAPPLE_NET=host can select it. */
#include "net_host.h"
#include "net_room.h"
#include "hz_result.h"
#include "offline_core.h"
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* threads.c: a blocking host service gives its virtual core up. Weak: unit tests that link only the services have no scheduler. */
void thread_core_away(int *token) __attribute__((weak));
void thread_core_back(int *token) __attribute__((weak));

void nh_core_away(int *token) {
    *token = 0;
    if (thread_core_away) thread_core_away(token);
}
void nh_core_back(int *token) {
    if (thread_core_back) thread_core_back(token);
}
int64_t nh_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Darwin keeps FreeBSD's numbers for everything up to EHOSTUNREACH (65); past it the two differ, so the ones a socket call can return are mapped by name. */
int nh_errno(int e) {
    if (e > 0 && e <= 65 && e != 45) return e;
    switch (e) {
        case 0: return 0;
        case ENOTSUP:
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
        case EOPNOTSUPP:
#endif
            return HZ_EOPNOTSUPP;
#ifdef ECANCELED
        case ECANCELED: return 85; /* FreeBSD ECANCELED */
#endif
#ifdef EOVERFLOW
        case EOVERFLOW: return 84;
#endif
#ifdef EPROTO
        case EPROTO: return 92;
#endif
        default: return HZ_EIO;
    }
}

int nh_addr_to_host(const OnlineAddr *a, struct sockaddr_storage *ss, socklen_t *len) {
    memset(ss, 0, sizeof *ss);
    if (a->family == BSD_AF_INET || a->family == 0) { /* 0: a zeroed address (the service's default) is INADDR_ANY */
        struct sockaddr_in *s = (struct sockaddr_in *)ss;
        s->sin_len = sizeof *s;
        s->sin_family = AF_INET;
        s->sin_port = htons(a->port);
        memcpy(&s->sin_addr, a->addr, 4);
        *len = sizeof *s;
        return 0;
    }
    if (a->family == BSD_AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)ss;
        s->sin6_len = sizeof *s;
        s->sin6_family = AF_INET6;
        s->sin6_port = htons(a->port);
        memcpy(&s->sin6_addr, a->addr, 16);
        *len = sizeof *s;
        return 0;
    }
    return HZ_EAFNOSUPPORT;
}
void nh_addr_from_host(const struct sockaddr *sa, OnlineAddr *out) {
    memset(out, 0, sizeof *out);
    if (!sa) return;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)sa;
        out->family = BSD_AF_INET;
        out->port = ntohs(s->sin_port);
        memcpy(out->addr, &s->sin_addr, 4);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)sa;
        out->family = BSD_AF_INET6;
        out->port = ntohs(s->sin6_port);
        memcpy(out->addr, &s->sin6_addr, 16);
    }
}

/* Rooms (NAPPLE_ROOM, net_room.h): nifm reports the room's virtual interface instead of the Mac's. */
static int nh_net_info_any(void *ctx, OnlineNetInfo *out) { return net_room_net_info(out) ? 0 : nh_net_info(ctx, out); }
static uint32_t nh_net_connect_any(void *ctx) {
    OnlineNetInfo ni;
    if (net_room_net_info(&ni)) return ni.connected ? 0 : HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
    return nh_net_connect(ctx);
}

static int nh_init(void *ctx) {
    NetHost *h = ctx;
    net_room_start(); /* NAPPLE_ROOM: join the room (once per process) */
    if (!h->ldn) h->ldn = nh_ldn_new();
    return h->ldn ? 0 : -1;
}
static void nh_shutdown(void *ctx) {
    NetHost *h = ctx;
    nh_ldn_free(h->ldn);
    h->ldn = NULL;
}

static const OnlineProvider nh_template = {
    .abi_version = ONLINE_ABI_VERSION,
    .struct_size = sizeof(OnlineProvider),
    .name = NET_HOST_NAME,
    .caps = ONLINE_CAP_NETWORK | ONLINE_CAP_SOCKETS | ONLINE_CAP_DNS | ONLINE_CAP_TLS | ONLINE_CAP_LDN,
    .init = nh_init,
    .shutdown = nh_shutdown,
    .net_info = nh_net_info_any,
    .net_connect = nh_net_connect_any,
    .net_release = nh_net_release,
    .sock_open = nh_sock_open,
    .sock_close = nh_sock_close,
    .sock_bind = nh_sock_bind,
    .sock_listen = nh_sock_listen,
    .sock_accept = nh_sock_accept,
    .sock_connect = nh_sock_connect,
    .sock_send = nh_sock_send,
    .sock_recv = nh_sock_recv,
    .sock_shutdown = nh_sock_shutdown,
    .sock_getsockname = nh_sock_getsockname,
    .sock_getpeername = nh_sock_getpeername,
    .sock_setsockopt = nh_sock_setsockopt,
    .sock_getsockopt = nh_sock_getsockopt,
    .sock_set_nonblock = nh_sock_set_nonblock,
    .sock_poll = nh_sock_poll,
    .dns_resolve = nh_dns_resolve,
    .dns_reverse = nh_dns_reverse,
    .tls_new = nh_tls_new,
    .tls_free = nh_tls_free,
    .tls_set_host = nh_tls_set_host,
    .tls_handshake = nh_tls_handshake,
    .tls_read = nh_tls_read,
    .tls_write = nh_tls_write,
    .tls_pending = nh_tls_pending,
    /* friend, prepo, nsd and acc stay NULL: those services keep their offline answers (no online service is implemented here) */
    .ldn_scan = nh_ldn_scan,
    .ldn_create = nh_ldn_create,
    .ldn_destroy = nh_ldn_destroy,
    .ldn_connect = nh_ldn_connect,
    .ldn_disconnect = nh_ldn_disconnect,
    .ldn_get_network = nh_ldn_get_network,
    .ldn_set_advertise = nh_ldn_set_advertise,
    .ldn_reject = nh_ldn_reject,
    .tls_add_ca = nh_tls_add_ca,
    .tls_set_verify = nh_tls_set_verify,
    .ldn_get_nodes = nh_ldn_get_nodes,
    .ldn_set_notify = nh_ldn_set_notify,
};

static NetHost main_ctx;
static OnlineProvider main_table;
static pthread_once_t main_once = PTHREAD_ONCE_INIT;
static void main_build(void) {
    main_table = nh_template;
    main_table.ctx = &main_ctx;
}
const OnlineProvider *net_host_provider(void) {
    pthread_once(&main_once, main_build);
    return &main_table;
}

OnlineProvider *net_host_instance_new(void) {
    OnlineProvider *p = calloc(1, sizeof *p);
    NetHost *h = calloc(1, sizeof *h);
    if (!p || !h || nh_init(h) != 0) {
        free(p);
        free(h);
        return NULL;
    }
    *p = nh_template;
    p->ctx = h;
    p->init = NULL; /* already started */
    return p;
}
void net_host_instance_free(OnlineProvider *p) {
    if (!p) return;
    nh_shutdown(p->ctx);
    free(p->ctx);
    free(p);
}

/* Only when nothing registered first: a wrapper around this provider (net_online.c) may register from its own constructor, and Darwin runs constructors
 * in link order, not by priority. */
__attribute__((constructor)) static void nh_register(void) {
    if (!online_builtin_host) online_builtin_host = net_host_provider;
}
