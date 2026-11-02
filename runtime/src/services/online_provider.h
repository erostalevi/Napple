/* OnlineProvider: the one extension point for every network-facing service (bsd, nifm, sfdnsres, nsd, ssl, friend, prepo).
 *
 * The services never touch a socket or a server themselves: they decode the guest's request, call the active provider through this table and
 * encode the answer. The default provider (online_null_provider) is a console with no network. An external module (a private build, another
 * user's library) supplies a different table through the C ABI documented in docs/guides/network-providers.md.
 *
 * This repository contains no Nintendo online service implementation and never contacts Nintendo: no Nintendo endpoints, tokens, certificates or
 * keys. The only online service it knows is Napple's own (runtime/src/net/net_online*.c over NAPPLE_NET=host, server crates/napple-online/): with
 * NAPPLE_ONLINE_SERVER set, a title's game-server name goes to a community-run Napple server and its account token is issued by that server
 * (docs/guides/napple-online.md). This header stays portable, no macOS dependency.
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
    ONLINE_CAP_LDN = 1u << 7,     /* ldn: a local-wireless transport (scan, host, join) */
    ONLINE_CAP_ACCOUNT = 1u << 8, /* acc: the local user can be linked to a network account kept by the provider */
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

/* ldn: one visible or joined local-wireless network. The provider owns the transport (radio, LAN, relay); the service only keeps the state machine
 * (ldn_core.c) and encodes what it gets. ids and addresses are provider-defined; everything is host byte order. */
#define ONLINE_LDN_ADVERTISE_MAX 0x180
#define ONLINE_LDN_MAX_NETWORKS 24
typedef struct {
    uint64_t local_communication_id; /* the title's id; a scan only returns networks of the same game */
    uint16_t scene_id;
    uint16_t advertise_len;
    uint8_t bssid[6];
    uint8_t channel;
    uint8_t node_count, node_count_max;
    uint8_t security_mode;
    uint16_t local_communication_version;
    uint8_t security_param[16]; /* what the host handed out for the join handshake; opaque */
    char ssid[33];              /* UTF-8, NUL-terminated */
    char host_name[33];         /* user name of the host */
    uint32_t ipv4;              /* this node's address once joined or hosting, 0 before */
    uint8_t advertise[ONLINE_LDN_ADVERTISE_MAX];
} OnlineLdnNetwork;

/* acc: what the provider knows about a local user's network account. `id` and `resource_cache` are opaque to the service (it hands them to the guest). */
#define ONLINE_ACCOUNT_CACHE_SIZE 0x68u
typedef struct {
    uint8_t linked, pad[7];
    uint64_t id;
    uint8_t resource_cache[ONLINE_ACCOUNT_CACHE_SIZE];
} OnlineAccountInfo;

/* ldn: one node of a hosted or joined network (net-host-provider addition). ipv4 in host byte order; user_name UTF-8, NUL-terminated. */
#define ONLINE_LDN_MAX_NODES 8
typedef struct {
    uint32_t ipv4;
    uint8_t mac[6];
    uint8_t node_id, connected;
    char user_name[33];
    uint8_t pad[3];
} OnlineLdnNode;
/* ssl SetVerifyOption bits (nn::ssl::Connection::VerifyOption) as the provider receives them. */
enum { ONLINE_TLS_VERIFY_PEER_CA = 1u << 0, ONLINE_TLS_VERIFY_HOST_NAME = 1u << 1, ONLINE_TLS_VERIFY_DATE = 1u << 2 };
/* ssl IoMode values (tls_set_io_mode, napple-online addition). */
enum { ONLINE_TLS_IO_BLOCKING = 1, ONLINE_TLS_IO_NONBLOCKING = 2 };

struct OnlineFriendInfo; /* friend details (online-friends), defined after the table */
struct OnlineCloudBackup; /* olsc backup state (ipc-provider-hooks), defined after the table */
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

    /* ---- ldn (ONLINE_CAP_LDN; added after ABI 1 shipped: modules built with a smaller struct_size simply lack these, ONLINE_FN returns NULL) ----
     * Calls return 0 or a Horizon result code for the guest. */
    /* Networks visible to a station: the count written to out (<= max), or a negative value when the radio is unavailable. */
    int (*ldn_scan)(void *ctx, uint64_t local_communication_id, OnlineLdnNetwork *out, uint32_t max);
    uint32_t (*ldn_create)(void *ctx, const OnlineLdnNetwork *cfg);  /* start hosting `cfg` (ssid/channel/security chosen by the provider where 0) */
    void (*ldn_destroy)(void *ctx);                                  /* stop hosting */
    uint32_t (*ldn_connect)(void *ctx, const OnlineLdnNetwork *net); /* join `net` as a station */
    void (*ldn_disconnect)(void *ctx);
    /* The network currently hosted or joined: 0 and *out filled, else a Horizon result (no network). *disconnect_reason: why the last connection ended (0 = none). */
    uint32_t (*ldn_get_network)(void *ctx, OnlineLdnNetwork *out, uint16_t *disconnect_reason);
    uint32_t (*ldn_set_advertise)(void *ctx, const void *data, uint32_t n); /* host: replace the advertised data */
    uint32_t (*ldn_reject)(void *ctx, uint32_t ipv4);                       /* host: kick the station at `ipv4` */

    /* ---- acc (ONLINE_CAP_ACCOUNT) ---- */
    /* Network-account state of the local user `user_id` (16 bytes): 0 and *out filled, nonzero = not available (treated as not linked). */
    int (*account_info)(void *ctx, const uint8_t user_id[16], OnlineAccountInfo *out);

    /* ---- additions of net-host-provider (2026-11-02): struct_size-extensible, ABI version unchanged ---- */
    /* ssl: also trust the guest-imported CA certificate `cert` (DER, or PEM text) when this connection verifies its server. 0 = ok. */
    int (*tls_add_ca)(void *ctx, void *conn, const void *cert, size_t n);
    /* ssl: the guest's SetVerifyOption bits (ONLINE_TLS_VERIFY_*); before the call every check is on. 0 = ok. */
    int (*tls_set_verify)(void *ctx, void *conn, uint32_t option);
    /* ldn: the nodes of the current network, node 0 = the host: the count written (<= max), *self = the caller's own index; negative = no network. */
    int (*ldn_get_nodes)(void *ctx, OnlineLdnNode *out, uint32_t max, uint32_t *self);
    /* ldn: `cb(arg)` is called (from any thread) whenever the hosted or joined network changes: a station joined or left, the host went away. NULL clears. */
    void (*ldn_set_notify)(void *ctx, void (*cb)(void *arg), void *arg);

    /* ---- additions of napple-online (2026-11-02-r7): struct_size-extensible, ABI version unchanged ---- */
    /* acc (ONLINE_CAP_ACCOUNT): the ID token of the linked account of `user_id`, as EnsureIdTokenCacheAsync / LoadIdTokenCache hand it to the title (an
     * opaque printable string; the Napple provider returns a token issued by the configured Napple server). Returns the length written to out (no NUL
     * needed), 0 = no token (not linked, server unreachable). */
    size_t (*account_id_token)(void *ctx, const uint8_t user_id[16], char *out, size_t n);
    /* ssl: the connection's I/O mode as the guest set it (nn::ssl IoMode: 1 = blocking, the default; 2 = non-blocking). On a non-blocking connection
     * tls_read (and tls_write) return -1 with *err = HZ_EAGAIN instead of waiting, which the ssl service answers with ResultWouldBlock. 0 = ok. */
    int (*tls_set_io_mode)(void *ctx, void *conn, uint32_t mode);

    /* ---- additions of online-friends (2026-11-02-r10): struct_size-extensible, ABI version unchanged (ONLINE_CAP_FRIENDS) ---- */
    /* The friend list with presence (online first): the count written to out (<= max), starting at `offset`. */
    uint32_t (*friend_list_info)(void *ctx, uint32_t offset, struct OnlineFriendInfo *out, uint32_t max);
    /* One friend by id: 0 and *out filled, nonzero = not a friend. */
    int (*friend_find)(void *ctx, uint64_t id, struct OnlineFriendInfo *out);
    /* Friend requests received and still pending. */
    uint32_t (*friend_request_count)(void *ctx);
    /* The next queued friend notification (ONLINE_FRIEND_NOTE_*): 0 and *type / *id filled, nonzero = queue empty. */
    int (*friend_pop_note)(void *ctx, uint32_t *type, uint64_t *id);
    /* `cb(arg)` is called (from any thread) when a friend notification is queued. NULL clears. */
    void (*friend_set_notify)(void *ctx, void (*cb)(void *arg), void *arg);
    /* The local user declared (1) or closed (0) an online-play session (DeclareOpenOnlinePlaySession / DeclareCloseOnlinePlaySession). */
    void (*friend_online_play)(void *ctx, int open);

    /* ---- additions of ipc-provider-hooks (2026-11-03-r11): struct_size-extensible, ABI version unchanged (ONLINE_CAP_FRIENDS) ---- */
    /* The friend's profile picture (friend 10110 / 10111): up to `max` bytes of image data (JPEG) of the requested edge size (0 = the provider's default)
     * written to out. Returns the bytes written, 0 = the friend has no picture or is not a friend. */
    uint32_t (*friend_profile_image)(void *ctx, uint64_t id, uint32_t size, void *out, uint32_t max);
    /* Friend 11000 / 11001 GetProfileImageUrl: the picture URL `in` (as the friend list carries it) rewritten for edge size `size`, NUL-terminated into out[n].
     * 0 = ok, nonzero = no URL. */
    int (*friend_image_url)(void *ctx, const char *in, uint32_t size, char *out, size_t n);
    /* The local user sends a friend request to account `id` from within the running application (friend 10200). 0 = sent, else a Horizon result. */
    uint32_t (*friend_send_request)(void *ctx, uint64_t id);

    /* ---- cloud saves (ONLINE_CAP_ACCOUNT; olsc 18 / 27) ---- */
    /* The backup state of user `user_id`'s save data for the running application. 0 and *out filled, nonzero = no backup information available. */
    int (*cloud_backup_info)(void *ctx, const uint8_t user_id[16], struct OnlineCloudBackup *out);
    /* Starts uploading the user's save data for the running application. Returns 0 (accepted, `*done` set when it already finished) or a Horizon result. */
    uint32_t (*cloud_backup_upload)(void *ctx, const uint8_t user_id[16], int *done);
} OnlineProvider;

/* olsc (ipc-provider-hooks): what cloud_backup_info reports. last_s = unix seconds of the newest backup (0 = none); size in bytes; pending = an upload is queued. */
typedef struct OnlineCloudBackup {
    uint8_t exists, pending, pad[6];
    uint64_t size;
    uint64_t last_s;
    uint64_t title_id;
} OnlineCloudBackup;

/* friend (online-friends): what friend_list_info / friend_find return. The app field is the friend's own UpdateUserPresence app data, visible only
 * to a title of the same presence group (same_app); status follows nn::friends PresenceStatus (0 offline, 1 online, 2 online play). */
#define ONLINE_FRIEND_APP_FIELD 0xC0u
typedef struct OnlineFriendInfo {
    uint64_t id;          /* as OnlineFriend.id */
    char name[33];        /* UTF-8 nickname */
    uint8_t status;       /* 0 offline, 1 online, 2 online play */
    uint8_t same_app;     /* playing the running title */
    uint8_t is_new;       /* became a friend during this run */
    uint8_t has_app_field;
    uint8_t pad[3];
    uint64_t last_online_s; /* unix seconds, 0 = unknown */
    uint64_t title_id;      /* the title the friend plays, 0 = none */
    char icon_id[32];       /* provider-defined picture id, empty = none */
    uint8_t app_field[ONLINE_FRIEND_APP_FIELD];
} OnlineFriendInfo;
/* friend_pop_note types (nn::friends NotificationType). */
enum { ONLINE_FRIEND_NOTE_LIST_UPDATED = 0x1, ONLINE_FRIEND_NOTE_NEW_REQUEST = 0x65 };

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
/* Built-in providers selected with NAPPLE_NET (net-host-provider): NAPPLE_NET=host picks the provider a linked module registered here (runtime/src/net/
 * registers it from a constructor, so a build without that module has none and stays offline). NAPPLE_ONLINE_PROVIDER, when set, wins. */
#define ONLINE_ENV_NET "NAPPLE_NET"
extern const OnlineProvider *(*online_builtin_host)(void);
