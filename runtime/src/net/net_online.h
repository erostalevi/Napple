/* Napple online (NAPPLE_ONLINE_SERVER): the client side of the community-run Napple online service, layered over the host provider (NAPPLE_NET=host).
 *
 * Games keep their own online code. What a console would get from Nintendo comes from the configured Napple server instead:
 * - acc: the local user is linked to a Napple account (created on the server on first use, credentials kept under the support directory) and the
 *   "ID token" a title hands to its game server is a Napple-issued token (net_online_http.c);
 * - nsd / sfdnsres: a title's NEX game-server name (g<id>-lp1.s.n.srv.nintendo.net) resolves to a reserved virtual address; a connection to that
 *   address goes to the Napple server instead, and the title's TLS over it is ended here (plain for an http:// server, real TLS to the server's own
 *   name, verified against the Mac's trust store, for https://);
 * - every other Nintendo name stays unresolvable: nothing here ever contacts Nintendo, and no console key, certificate or Nintendo credential is used.
 *
 * Without NAPPLE_ONLINE_SERVER the host provider is used unchanged (r6 behaviour). */
#pragma once
#include "online_provider.h"
#include <stddef.h>
#include <stdint.h>

#define NET_ONLINE_ENV_SERVER "NAPPLE_ONLINE_SERVER"   /* http://host[:port], https://host[:port], or "default" */
#define NET_ONLINE_DEFAULT_SERVER "https://pretend.napple.lat" /* what NAPPLE_ONLINE_SERVER=default means: the community-run Napple server */
#define NET_ONLINE_ENV_STATE "NAPPLE_ONLINE_STATE_DIR" /* where account credentials are kept; default <support dir>/online */
#define NET_ONLINE_NAME "napple-online over host"
/* The address redirected names resolve to (100.64.0.0/10, shared address space: never a real peer of a title). Host byte order. */
#define NET_ONLINE_VIRTUAL_IP 0x644e4e64u /* 100.78.78.100: a palindrome, so it reads the same in either byte order (sfdnsres hostents come out byte-swapped) */

typedef struct {
    int tls;         /* 1 = https */
    char host[256];  /* server host name or address */
    uint16_t port;   /* 80 / 443 when the URL has none */
    char key[300];   /* host_port, a file-name-safe identity of the server (credentials are per server) */
} NetOnlineServer;

/* Parses a server URL. 0 = ok. */
int net_online_parse_server(const char *url, NetOnlineServer *out);

/* ---- name policy (pure) ---- */
enum { NET_ONLINE_NAME_PASS = 0, NET_ONLINE_NAME_REDIRECT = 1, NET_ONLINE_NAME_BLOCK = 2 };
/* What happens to a DNS name: a NEX game server name is redirected, any other Nintendo name (or a name with an unfilled '%' template) is blocked,
 * everything else resolves through the host. */
int net_online_name_policy(const char *name);
/* nsd: fills the environment template ('%' -> "lp1"). 0 = ok. */
int net_online_nsd(const char *in, char *out, size_t n);

/* ---- the provider ---- */
/* The table NAPPLE_NET=host selects: the host provider wrapped for the server in NAPPLE_ONLINE_SERVER, or the host provider itself when unset. */
const OnlineProvider *net_online_builtin(void);
/* A wrapper over `host` for `server` (tests). state_dir: where credentials are kept (NULL = memory only). Free with net_online_free. */
OnlineProvider *net_online_new(const OnlineProvider *host, const NetOnlineServer *server, const char *state_dir);
void net_online_free(OnlineProvider *p);

/* ---- accounts and tokens (net_online_http.c) ---- */
typedef struct {
    uint64_t pid;     /* the Napple account id (the NEX principal id) */
    char secret[65];  /* hex; proves the account to the server */
} NetOnlineAccount;
/* A blocking HTTP request to the server (POST when body != NULL, form-encoded). Returns the HTTP status (or -1) and the body in out (NUL-terminated). */
int net_online_http(const OnlineProvider *host, const NetOnlineServer *srv, const char *path, const char *body, char *out, size_t n);
/* `key=value` lines: copies the value of `key` into out. 0 = found. */
int net_online_kv(const char *text, const char *key, char *out, size_t n);
/* The account of user `uid` on the server: loaded from state_dir, else created on the server (and saved). 0 = ok. */
int net_online_account(const OnlineProvider *host, const NetOnlineServer *srv, const char *state_dir, const uint8_t uid[16], NetOnlineAccount *out);
/* A Napple-issued ID token for that account and the running title. Returns the token length (0 = failure). */
size_t net_online_token(const OnlineProvider *host, const NetOnlineServer *srv, const NetOnlineAccount *a, uint64_t title_id, char *out, size_t n);
