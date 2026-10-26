/* OnlineProvider: the one extension point for every network-facing service (bsd, nifm, sfdnsres, nsd, ssl, friend, prepo).
 *
 * The services never touch a socket or a server themselves: they decode the guest's request, call the active provider through this table and
 * encode the answer. The default provider (online_null_provider) is a console with no network. An external module (a private build, another
 * user's library) supplies a different table through the C ABI documented in docs/guides/network-providers.md.
 *
 * This repository contains NO online service implementation: no endpoints, tokens, certificates or protocol code. Portable, no macOS dependency.
 * Numbering of errors is FreeBSD's (hz_result.h): a provider returns -1 and sets *err to an HZ_E* value, exactly what a guest sees from bsd:u. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define ONLINE_ABI_VERSION 1u
#define ONLINE_PLUGIN_SYMBOL "napple_online_provider_v1"
#define ONLINE_ENV_PROVIDER "NAPPLE_ONLINE_PROVIDER" /* path of a shared library exporting ONLINE_PLUGIN_SYMBOL; absent or failing = NullProvider */

/* Capability flags (OnlineProvider.caps): what the provider can really do. A service whose capability is missing answers like the NullProvider. */
enum {
    ONLINE_CAP_NETWORK = 1u << 0, /* nifm: a connection can be established, net_info is meaningful */
    ONLINE_CAP_SOCKETS = 1u << 1, /* bsd */
    ONLINE_CAP_DNS = 1u << 2,     /* sfdnsres */
    ONLINE_CAP_TLS = 1u << 3,     /* ssl */
    ONLINE_CAP_FRIENDS = 1u << 4, /* friend */
    ONLINE_CAP_REPORTS = 1u << 5, /* prepo */
    ONLINE_CAP_NSD = 1u << 6,     /* nsd name resolution is provider-defined */
};

/* A socket address in host byte order (the services convert from and to the guest's sockaddr layout). */
typedef struct {
    uint8_t family; /* 2 = AF_INET, 28 = AF_INET6 (FreeBSD numbering, as the guest uses) */
    uint8_t pad;
    uint16_t port;
    uint8_t addr[16]; /* IPv4 uses addr[0..3] in network order */
} OnlineAddr;

typedef struct {
    int32_t fd; /* provider socket handle */
    int16_t events, revents;
} OnlinePollFd;
enum { ONLINE_POLLIN = 0x1, ONLINE_POLLPRI = 0x2, ONLINE_POLLOUT = 0x4, ONLINE_POLLERR = 0x8, ONLINE_POLLHUP = 0x10, ONLINE_POLLNVAL = 0x20 };

/* nifm: the state of the (single) network interface. kind 0 = none, 1 = wireless, 2 = wired. strength 0..3. IPv4 values in host byte order. */
typedef struct {
    uint8_t connected, kind, strength, pad;
    uint32_t ip, mask, gateway, dns1, dns2;
} OnlineNetInfo;

/* DNS results. */
#define ONLINE_MAX_ADDRS 8
typedef struct {
    uint32_t count;
    OnlineAddr addrs[ONLINE_MAX_ADDRS];
    char canonname[256]; /* empty when none */
} OnlineResolved;

/* friend: an entry of the friend list. id is an opaque 64-bit account id owned by the provider. presence 0 = offline, 1 = online, 2 = playing. */
typedef struct {
    uint64_t id;
    char name[33]; /* UTF-8, NUL-terminated */
    uint8_t presence;
    uint8_t pad[6];
} OnlineFriend;

/* TLS: the provider owns the whole connection state. sock is a provider socket handle (see sockets) or -1. */
typedef struct OnlineProvider {
    uint32_t abi_version;        /* must be ONLINE_ABI_VERSION */
    uint32_t struct_size;        /* sizeof(OnlineProvider) the module was built with; fields past it are treated as absent */
    const char *name;            /* for logs */
    uint32_t caps;               /* ONLINE_CAP_* */
    void *ctx;                   /* passed as the first argument of every call */
    int (*init)(void *ctx);      /* optional; 0 = ok. A failure falls back to the NullProvider */
    void (*shutdown)(void *ctx); /* optional */

    /* ---- nifm ---- */
    int (*net_info)(void *ctx, OnlineNetInfo *out); /* 0 = ok */
    /* Establish the connection a nifm request asked for. Returns 0 (accepted) or a Horizon result code to report from GetResult. */
    uint32_t (*net_connect)(void *ctx);
    void (*net_release)(void *ctx);

    /* ---- bsd sockets: a handle >= 0 or -1 with *err set. Calls return 0/>=0 on success or -1 with *err. ---- */
    int (*sock_open)(void *ctx, int domain, int type, int protocol, int *err);
    int (*sock_close)(void *ctx, int h, int *err);
    int (*sock_bind)(void *ctx, int h, const OnlineAddr *a, int *err);
    int (*sock_listen)(void *ctx, int h, int backlog, int *err);
    int (*sock_accept)(void *ctx, int h, OnlineAddr *peer, int *err); /* returns the new handle */
    int (*sock_connect)(void *ctx, int h, const OnlineAddr *a, int *err);
    int64_t (*sock_send)(void *ctx, int h, const void *buf, size_t n, int flags, const OnlineAddr *to, int *err);
    int64_t (*sock_recv)(void *ctx, int h, void *buf, size_t n, int flags, OnlineAddr *from, int *err);
    int (*sock_shutdown)(void *ctx, int h, int how, int *err);
    int (*sock_getsockname)(void *ctx, int h, OnlineAddr *out, int *err);
    int (*sock_getpeername)(void *ctx, int h, OnlineAddr *out, int *err);
    int (*sock_setsockopt)(void *ctx, int h, int level, int opt, const void *val, size_t n, int *err);
    int (*sock_getsockopt)(void *ctx, int h, int level, int opt, void *val, size_t *n, int *err);
    int (*sock_set_nonblock)(void *ctx, int h, int on, int *err);
    int (*sock_poll)(void *ctx, OnlinePollFd *fds, unsigned n, int timeout_ms, int *err); /* returns the number of ready entries */

    /* ---- sfdnsres: 0 = ok with out filled, else an HZ_EAI_* error (the hostent calls map it to a netdb error) ---- */
    int (*dns_resolve)(void *ctx, const char *host, const char *service, int family, OnlineResolved *out);
    int (*dns_reverse)(void *ctx, const OnlineAddr *a, char *host, size_t n);

    /* ---- ssl ---- */
    void *(*tls_new)(void *ctx, int sock_handle); /* a connection object or NULL */
    void (*tls_free)(void *ctx, void *conn);
    int (*tls_set_host)(void *ctx, void *conn, const char *host);
    int (*tls_handshake)(void *ctx, void *conn, uint32_t *result); /* 0 = established; otherwise *result is the Horizon result to report */
    int64_t (*tls_read)(void *ctx, void *conn, void *buf, size_t n, int *err);
    int64_t (*tls_write)(void *ctx, void *conn, const void *buf, size_t n, int *err);
    int64_t (*tls_pending)(void *ctx, void *conn);

    /* ---- friend ---- */
    uint32_t (*friend_count)(void *ctx);
    uint32_t (*friend_list)(void *ctx, uint32_t offset, OnlineFriend *out, uint32_t max); /* returns the entries written */
    void (*presence_update)(void *ctx, const void *record, size_t n);                     /* the guest's UpdateUserPresence payload, opaque */

    /* ---- prepo / nsd ---- */
    void (*report)(void *ctx, const void *record, size_t n);                   /* a play report; the NullProvider drops it */
    int (*nsd_resolve)(void *ctx, const char *in, char *out, size_t out_size); /* 0 = out filled; the NullProvider returns the name unchanged */
} OnlineProvider;

/* The active provider; never NULL. The first call loads ONLINE_ENV_PROVIDER once (a failure is logged and falls back to the NullProvider). */
const OnlineProvider *online_provider(void);
const OnlineProvider *online_null_provider(void);
/* Installs a provider (tests, an embedding host). NULL restores the NullProvider. Returns the previous one. The table must outlive its use. */
const OnlineProvider *online_provider_install(const OnlineProvider *p);
/* Forgets the active provider and the environment decision: the next online_provider() reads NAPPLE_ONLINE_PROVIDER again. Tests. */
void online_provider_reset(void);
/* Validates abi_version and struct_size, then init. Returns 0 on success. Used by the loader and by tests. */
int online_provider_check(const OnlineProvider *p);
/* Drops everything the NullProvider remembers (sockets). Used by ShutdownAllSockets and tests. */
void online_null_reset(void);
/* A provider function that this table has (the module may have been built with an older, smaller struct) or NULL. */
#define ONLINE_FN(p, field) ((offsetof(OnlineProvider, field) + sizeof((p)->field) <= (p)->struct_size) ? (p)->field : NULL)
/* Capability test against the active provider. */
static inline int online_has(const OnlineProvider *p, uint32_t cap) { return (p->caps & cap) != 0; }
