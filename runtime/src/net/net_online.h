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
/* The NAT check's secondary server (nncs2) gets its own virtual address; its ports map to the Napple server's NAT check ports + 1000. */
#define NET_ONLINE_VIRTUAL_IP2 0x644f4f64u /* 100.79.79.100 */
#define NET_ONLINE_NATCHECK_PORT 10025     /* the NAT check's primary port; the secondary is + 100, the mapping tests use 33334 / 33335 */
#define NET_ONLINE_NATCHECK_SECONDARY 1000 /* the secondary server's ports on the Napple server: the primary's + this */

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
/* What happens to a DNS name: a NEX game server name and the NAT check servers (nncs1 / nncs2) are redirected, any other Nintendo name (or a name with
 * an unfilled '%' template) is blocked, everything else resolves through the host. */
int net_online_name_policy(const char *name);
/* nsd: fills the environment template ('%' -> "lp1"). 0 = ok. */
int net_online_nsd(const char *in, char *out, size_t n);
/* The virtual address a redirected name resolves to: NET_ONLINE_VIRTUAL_IP2 for the NAT check's secondary server, else NET_ONLINE_VIRTUAL_IP. */
uint32_t net_online_redirect_ip(const char *name);

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
/* The general request: any method, extra header lines ("Name: value\r\n" each, or NULL), a body of type ctype (or NULL). Returns the HTTP status (or
 * -1) and the answer body in *out (malloc'd, NUL-terminated, chunked bodies decoded; NULL when there is none). */
int net_online_request(const OnlineProvider *host, const NetOnlineServer *srv, const char *method, const char *path, const char *headers, const char *ctype,
                       const char *body, char **out);

/* ---- friends and presence (net_online_social.c, online-friends 2026-11-02-r10) ----
 * With a Napple Account access token available, the wrapper gets ONLINE_CAP_FRIENDS: friend:u's list, presence and notifications come from
 * napple-social (crates/napple-social, docs/guides/napple-social.md) for one Napple player profile, and the running title is published as that
 * profile's presence ("playing <title>", the title's own presence app data as the join token, mode "online" while it declares an online-play session).
 * A background thread polls the service; IPC calls only read its cache. Without a token nothing changes. */
#define NET_ONLINE_ENV_SOCIAL "NAPPLE_SOCIAL_URL"          /* napple-social base URL (paths /social/v1/...); default: NAPPLE_ONLINE_SERVER */
#define NET_ONLINE_ENV_ACCOUNT "NAPPLE_ACCOUNT_URL"        /* napple-account base URL; a trailing /account/v1 is accepted; default: NAPPLE_ONLINE_SERVER */
#define NET_ONLINE_ENV_TOKEN "NAPPLE_ACCOUNT_TOKEN"        /* a napple-account access token (tests, short runs: tokens last 15 minutes) */
#define NET_ONLINE_ENV_TOKEN_FILE "NAPPLE_ACCOUNT_TOKEN_FILE" /* a file holding the current access token, re-read whenever the service refuses it */
#define NET_ONLINE_ENV_PROFILE "NAPPLE_SOCIAL_PROFILE"     /* the Napple profile (UUID) acting; default: the account's first profile */
#define NET_ONLINE_ENV_SOCIAL_POLL "NAPPLE_SOCIAL_POLL_MS" /* friend list refresh interval, default 5000 */
/* A host application that keeps the Napple Account sign-in (Studio, napple-cloud) can define this to hand the current access token over: the length
 * written to out (no NUL needed), 0 = signed out. It wins over the environment. net_online_social.c has a weak default that returns 0; friends are
 * switched on when it hands out a token at start-up. */
size_t napple_account_access_token(char *out, size_t n);
/* The Napple profile (UUID text) the local user plays as, from the shared credentials' profile mapping (src/online/); weak default 0 = none. NAPPLE_SOCIAL_PROFILE wins. */
size_t napple_account_profile_id(char *out, size_t n);

typedef struct {
    NetOnlineServer social, account;
    char token[4096];      /* NAPPLE_ACCOUNT_TOKEN */
    char token_file[1024]; /* NAPPLE_ACCOUNT_TOKEN_FILE */
    char profile[64];      /* NAPPLE_SOCIAL_PROFILE */
    char online_key[300];  /* the Napple online server's key (NetOnlineServer.key): friends on the same server are listed under their game-server id */
    uint64_t title_id;
    char title_name[96];
    int poll_ms;
} NetSocialConfig;
/* Reads the environment; `online` is the NAPPLE_ONLINE_SERVER default for both URLs. 0 = a token source exists (friends on). */
int net_social_config(const NetOnlineServer *online, NetSocialConfig *out);
typedef struct NetSocial NetSocial;
/* Starts the client (thread = 0: no thread, the caller drives it with net_social_sync; tests). NULL on failure. */
NetSocial *net_social_start(const OnlineProvider *host, const NetSocialConfig *cfg, int thread);
void net_social_quit(NetSocial *s); /* stops the thread and signs the presence out; the cache stays readable */
void net_social_stop(NetSocial *s); /* quit, then free */
int net_social_sync(NetSocial *s);  /* one round (sign-in on first use, presence, friend list, requests): 0 = the service answered */
uint32_t net_social_list(NetSocial *s, uint32_t offset, OnlineFriendInfo *out, uint32_t max);
int net_social_find(NetSocial *s, uint64_t id, OnlineFriendInfo *out);
uint32_t net_social_count(NetSocial *s);
uint32_t net_social_request_count(NetSocial *s);
int net_social_pop_note(NetSocial *s, uint32_t *type, uint64_t *id);
void net_social_set_notify(NetSocial *s, void (*cb)(void *), void *arg);
void net_social_presence(NetSocial *s, const void *record, size_t n); /* the guest's UserPresence (0xE0: app field at 0x20) */
void net_social_online_play(NetSocial *s, int open);
const char *net_social_friend_code(NetSocial *s); /* "NP-1234-5678-9012" once signed in, else "" */
void net_social_set_self_id(NetSocial *s, uint64_t pid); /* this user's game-server account id, published in the join token */
/* cb(arg, ids, n) gets the game-server ids of the friends seen on the same online server whenever that set changes (from the client thread); a
 * nonzero return makes it try again next round. */
void net_social_set_pid_listener(NetSocial *s, int (*cb)(void *arg, const uint64_t *pids, uint32_t n), void *arg);
/* Tells the Napple online server which accounts are this account's friends (POST /napple/v1/friends), so the game server can answer "my friends'
 * sessions" (an empty GetPlayingSession list). 0 = accepted. */
int net_online_report_friends(const OnlineProvider *host, const NetOnlineServer *srv, const NetOnlineAccount *a, const uint64_t *pids, uint32_t n);
/* ---- pure helpers (tests) ---- */
uint64_t net_social_friend_id(const char *profile_id); /* the game-visible id of a Napple profile: 63-bit FNV-1a of the UUID */
/* Parses a GET /friends answer. Returns the entries written (<= max). key: this client's online server key. pids (may be NULL): each friend's
 * game-server id from its join token (0 = unknown); with pids NULL a known game-server id replaces the hashed id in out. */
uint32_t net_social_parse_friends(const char *json, uint64_t running_title, const char *key, int64_t since_ms, OnlineFriendInfo *out, uint64_t *pids,
                                  uint32_t max);
/* The PUT /presence body for this state: join token "np1:<key>:<pid>:<base64url app data>" when pid or app data is set. */
int net_social_presence_body(uint64_t title_id, const char *title_name, int online_play, const char *key, uint64_t pid, const uint8_t *app, size_t app_n,
                             char *out, size_t n);
