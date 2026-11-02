/* The built-in host network provider (NAPPLE_NET=host): an OnlineProvider over the Mac's own network stack.
 *
 * Generic transport only: BSD sockets (net_host_sock.c), the host resolver (net_host_dns.c), TLS clients verified against the host trust store
 * (net_host_tls.m, Security / SecureTransport), the host's connectivity for nifm (net_host_nifm.m, SystemConfiguration) and local wireless emulated
 * over the LAN between Napple instances (net_host_ldn.c). Nothing here knows any online service: no hostnames, endpoints, tokens, certificates or
 * account/device authentication. Friends, presence, play reports, nsd and network accounts stay with the offline answers (their entry points are NULL).
 *
 * Errors are FreeBSD's numbering (hz_result.h), as the guest sees them; every call may come from any guest thread, concurrently. A call that may wait
 * gives its virtual core up while it waits (thread_core_away), so other guest threads keep running. */
#pragma once
#include "online_provider.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define NET_HOST_NAME "host (macOS network stack)"

/* The provider table (one shared instance; the first call builds it). */
const OnlineProvider *net_host_provider(void);
/* A second, independent instance (own LDN state; sockets are process-wide), for tests that run two consoles in one process. Free with net_host_instance_free. */
OnlineProvider *net_host_instance_new(void);
void net_host_instance_free(OnlineProvider *p);

/* ---- shared helpers (net_host.c) ---- */
int nh_errno(int host_errno);                                           /* a host errno as the guest's FreeBSD errno */
int nh_addr_to_host(const OnlineAddr *a, struct sockaddr_storage *ss, socklen_t *len); /* 0, or an HZ errno */
void nh_addr_from_host(const struct sockaddr *sa, OnlineAddr *out);     /* unknown families leave a zero address */
/* Waiting on the host: gives the calling guest thread's virtual core away (no-op on a host thread or in tests) and takes it back. */
void nh_core_away(int *token);
void nh_core_back(int *token);
int64_t nh_now_ms(void);

/* ---- sockets (net_host_sock.c) ---- */
typedef struct NhSock NhSock;
NhSock *nh_sock_get(int h); /* takes a reference; NULL for an unknown or closing handle */
void nh_sock_put(NhSock *s);
int nh_sock_fd(const NhSock *s);
int nh_sock_nonblocking(const NhSock *s);
/* Waits until the socket is ready for `events` (POLLIN / POLLOUT) or timeout_ms passes (< 0 = forever), in short slices so that a close from another thread
 * ends the wait. 1 ready, 0 timed out, -1 closed meanwhile. */
int nh_sock_wait(NhSock *s, int events, int timeout_ms);
void nh_sock_close_all(void);

int nh_sock_open(void *ctx, int domain, int type, int protocol, int *err);
int nh_sock_close(void *ctx, int h, int *err);
int nh_sock_bind(void *ctx, int h, const OnlineAddr *a, int *err);
int nh_sock_listen(void *ctx, int h, int backlog, int *err);
int nh_sock_accept(void *ctx, int h, OnlineAddr *peer, int *err);
int nh_sock_connect(void *ctx, int h, const OnlineAddr *a, int *err);
int64_t nh_sock_send(void *ctx, int h, const void *buf, size_t n, int flags, const OnlineAddr *to, int *err);
int64_t nh_sock_recv(void *ctx, int h, void *buf, size_t n, int flags, OnlineAddr *from, int *err);
int nh_sock_shutdown(void *ctx, int h, int how, int *err);
int nh_sock_getsockname(void *ctx, int h, OnlineAddr *out, int *err);
int nh_sock_getpeername(void *ctx, int h, OnlineAddr *out, int *err);
int nh_sock_setsockopt(void *ctx, int h, int level, int opt, const void *val, size_t n, int *err);
int nh_sock_getsockopt(void *ctx, int h, int level, int opt, void *val, size_t *n, int *err);
int nh_sock_set_nonblock(void *ctx, int h, int on, int *err);
int nh_sock_poll(void *ctx, OnlinePollFd *fds, unsigned n, int timeout_ms, int *err);

/* ---- DNS (net_host_dns.c) ---- */
int nh_dns_resolve(void *ctx, const char *host, const char *service, int family, OnlineResolved *out);
int nh_dns_reverse(void *ctx, const OnlineAddr *a, char *host, size_t n);
int nh_eai(int host_eai); /* a host EAI_* as the guest's */

/* ---- TLS (net_host_tls.m) ---- */
void *nh_tls_new(void *ctx, int sock_handle);
void nh_tls_free(void *ctx, void *conn);
int nh_tls_set_host(void *ctx, void *conn, const char *host);
int nh_tls_handshake(void *ctx, void *conn, uint32_t *result);
int64_t nh_tls_read(void *ctx, void *conn, void *buf, size_t n, int *err);
int64_t nh_tls_write(void *ctx, void *conn, const void *buf, size_t n, int *err);
int64_t nh_tls_pending(void *ctx, void *conn);
int nh_tls_add_ca(void *ctx, void *conn, const void *cert, size_t n);
int nh_tls_set_verify(void *ctx, void *conn, uint32_t option);

/* ---- nifm (net_host_nifm.m) ---- */
int nh_net_info(void *ctx, OnlineNetInfo *out);
uint32_t nh_net_connect(void *ctx);
void nh_net_release(void *ctx);

/* ---- ldn over the LAN (net_host_ldn.c); state per provider instance ---- */
typedef struct NhLdn NhLdn;
NhLdn *nh_ldn_new(void);
void nh_ldn_free(NhLdn *l);
int nh_ldn_scan(void *ctx, uint64_t local_communication_id, OnlineLdnNetwork *out, uint32_t max);
uint32_t nh_ldn_create(void *ctx, const OnlineLdnNetwork *cfg);
void nh_ldn_destroy(void *ctx);
uint32_t nh_ldn_connect(void *ctx, const OnlineLdnNetwork *net);
void nh_ldn_disconnect(void *ctx);
uint32_t nh_ldn_get_network(void *ctx, OnlineLdnNetwork *out, uint16_t *disconnect_reason);
uint32_t nh_ldn_set_advertise(void *ctx, const void *data, uint32_t n);
uint32_t nh_ldn_reject(void *ctx, uint32_t ipv4);
int nh_ldn_get_nodes(void *ctx, OnlineLdnNode *out, uint32_t max, uint32_t *self);
void nh_ldn_set_notify(void *ctx, void (*cb)(void *arg), void *arg);

/* The provider context: what one instance owns. */
typedef struct {
    NhLdn *ldn;
} NetHost;
static inline NhLdn *nh_ldn_of(void *ctx) { return ((NetHost *)ctx)->ldn; }
