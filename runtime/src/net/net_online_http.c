/* Napple online: the account and token requests to the Napple server. A minimal HTTP/1.1 client over the host provider's own sockets (and its TLS for an
 * https:// server, verified against the Mac's trust store), so it works from any thread exactly like the title's traffic. The server's API is
 * deliberately simple (docs/guides/napple-online.md): form-encoded requests, `key=value` line answers.
 *
 *   POST /napple/v1/account  uid=<32 hex>                          -> pid=<u64> secret=<hex>
 *   POST /napple/v1/token    pid=<u64>&secret=<hex>&title=<16 hex>  -> token=<token> expires=<unix seconds>
 *   POST /napple/v1/friends  pid=<u64>&secret=<hex>&friends=<u64>,...  -> friends=<count> (the friends napple-social knows on this server)
 *
 * net_online_request is the general form (any method, extra headers, JSON bodies, chunked answers): the napple-social client uses it.
 *
 * Credentials are kept per server and per local user in <state dir>/<host_port>/<uid hex>.txt (the same two lines). */
#include "net_online.h"
#include "hz_result.h"
#include "offline_core.h"
#include "../sha256.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

enum { G_SOCK_STREAM = 1, G_SOL_SOCKET = 0xffff, G_SO_SNDTIMEO = 0x1005, G_SO_RCVTIMEO = 0x1006 };

int net_online_kv(const char *text, const char *key, char *out, size_t n) {
    size_t k = strlen(key);
    for (const char *p = text; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > k && !strncmp(p, key, k) && p[k] == '=') {
            size_t v = len - k - 1;
            if (v && p[k + 1 + v - 1] == '\r') v--;
            if (v >= n) return -1;
            memcpy(out, p + k + 1, v);
            out[v] = 0;
            return 0;
        }
        p = eol ? eol + 1 : NULL;
    }
    return -1;
}

typedef struct {
    const OnlineProvider *p;
    int h;
    void *tls;
} Conn;
static int64_t conn_write(Conn *c, const void *buf, size_t n) {
    int err = 0;
    size_t done = 0;
    while (done < n) {
        int64_t w = c->tls ? c->p->tls_write(c->p->ctx, c->tls, (const char *)buf + done, n - done, &err)
                           : c->p->sock_send(c->p->ctx, c->h, (const char *)buf + done, n - done, 0, NULL, &err);
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return (int64_t)done;
}
static int64_t conn_read(Conn *c, void *buf, size_t n) {
    int err = 0;
    return c->tls ? c->p->tls_read(c->p->ctx, c->tls, buf, n, &err) : c->p->sock_recv(c->p->ctx, c->h, buf, n, 0, NULL, &err);
}

/* De-chunks a "Transfer-Encoding: chunked" body in place. Returns the new length. */
static size_t dechunk(char *b, size_t n) {
    size_t r = 0, w = 0;
    while (r < n) {
        char *end;
        unsigned long len = strtoul(b + r, &end, 16);
        char *eol = strstr(b + r, "\r\n");
        if (end == b + r || !eol) break;
        r = (size_t)(eol - b) + 2;
        if (!len || r + len > n) break;
        memmove(b + w, b + r, len);
        w += len, r += len + 2;
    }
    b[w] = 0;
    return w;
}

int net_online_request(const OnlineProvider *p, const NetOnlineServer *srv, const char *method, const char *path, const char *headers, const char *ctype,
                       const char *body, char **out) {
    *out = NULL;
    OnlineResolved r;
    char port[8];
    snprintf(port, sizeof port, "%u", srv->port);
    if (p->dns_resolve(p->ctx, srv->host, port, BSD_AF_INET, &r) || !r.count) return -1;
    OnlineAddr a = r.addrs[0];
    a.port = srv->port;
    int err = 0;
    Conn c = {p, p->sock_open(p->ctx, BSD_AF_INET, G_SOCK_STREAM, 0, &err), NULL};
    if (c.h < 0) return -1;
    struct {
        int64_t sec, usec;
    } tv = {10, 0}; /* the guest's (FreeBSD) timeval: a dead server does not stall the title for long */
    p->sock_setsockopt(p->ctx, c.h, G_SOL_SOCKET, G_SO_RCVTIMEO, &tv, sizeof tv, &err);
    p->sock_setsockopt(p->ctx, c.h, G_SOL_SOCKET, G_SO_SNDTIMEO, &tv, sizeof tv, &err);
    int status = -1;
    char *resp = NULL, *head = NULL;
    if (p->sock_connect(p->ctx, c.h, &a, &err)) goto done;
    if (srv->tls) {
        uint32_t result = 0;
        c.tls = p->tls_new ? p->tls_new(p->ctx, c.h) : NULL;
        if (!c.tls || p->tls_set_host(p->ctx, c.tls, srv->host) || p->tls_handshake(p->ctx, c.tls, &result)) {
            log_line("online", "napple: TLS to %s:%u failed (result 0x%x)", srv->host, srv->port, result);
            goto done;
        }
    }
    size_t blen = body ? strlen(body) : 0, hcap = 1024 + strlen(path) + (headers ? strlen(headers) : 0);
    head = malloc(hcap);
    if (!head) goto done;
    int hl = snprintf(head, hcap, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: napple-runtime\r\nConnection: close\r\n%s", method, path, srv->host,
                      headers ? headers : "");
    if (body && hl > 0 && (size_t)hl < hcap) hl += snprintf(head + hl, hcap - (size_t)hl, "Content-Type: %s\r\nContent-Length: %zu\r\n", ctype, blen);
    else if (hl > 0 && (size_t)hl < hcap && (!strcmp(method, "POST") || !strcmp(method, "PUT"))) hl += snprintf(head + hl, hcap - (size_t)hl, "Content-Length: 0\r\n");
    if (hl > 0 && (size_t)hl < hcap) hl += snprintf(head + hl, hcap - (size_t)hl, "\r\n");
    if (hl <= 0 || (size_t)hl >= hcap) goto done;
    if (conn_write(&c, head, (size_t)hl) < 0 || (blen && conn_write(&c, body, blen) < 0)) goto done;
    size_t cap = 16384, len = 0, max = 4u << 20;
    resp = malloc(cap + 1);
    if (!resp) goto done;
    for (;;) {
        if (len == cap) {
            if (cap >= max) break;
            char *g = realloc(resp, cap * 2 + 1);
            if (!g) break;
            resp = g, cap *= 2;
        }
        int64_t got = conn_read(&c, resp + len, cap - len);
        if (got <= 0) break;
        len += (size_t)got;
    }
    resp[len] = 0;
    int code = 0;
    if (sscanf(resp, "HTTP/1.%*d %d", &code) != 1) goto done;
    status = code;
    char *b = strstr(resp, "\r\n\r\n");
    if (b) {
        *b = 0;
        int chunked = strcasestr(resp, "\r\ntransfer-encoding: chunked") != NULL;
        b += 4;
        size_t blen2 = len - (size_t)(b - resp);
        memmove(resp, b, blen2 + 1);
        if (chunked) dechunk(resp, blen2);
        *out = resp;
        resp = NULL;
    }
done:
    free(resp);
    free(head);
    if (c.tls) p->tls_free(p->ctx, c.tls);
    p->sock_close(p->ctx, c.h, &err);
    return status;
}

int net_online_http(const OnlineProvider *p, const NetOnlineServer *srv, const char *path, const char *body, char *out, size_t n) {
    if (n) out[0] = 0;
    char *resp = NULL;
    int st = net_online_request(p, srv, body ? "POST" : "GET", path, NULL, "application/x-www-form-urlencoded", body, &resp);
    if (resp && n) snprintf(out, n, "%s", resp);
    free(resp);
    return st;
}

static void hex(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}
static void mkdirs(const char *path) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *s = tmp + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(tmp, 0700);
            *s = '/';
        }
    mkdir(tmp, 0700);
}
static int parse_account(const char *text, NetOnlineAccount *out) {
    char pid[32];
    if (net_online_kv(text, "pid", pid, sizeof pid) || net_online_kv(text, "secret", out->secret, sizeof out->secret)) return -1;
    out->pid = strtoull(pid, NULL, 10);
    return out->pid && out->secret[0] ? 0 : -1;
}

/* Napple Account identity (a signed-in Mac, src/online/): the server takes the account access token as the game's token and the principal id derives from
 * the profile (crates/napple-online/src/identity.rs principal_id): no /napple/v1/account, no secret. */
uint64_t net_online_principal_id(const char *profile_id) {
    char in[128];
    uint8_t h[32];
    int n = snprintf(in, sizeof in, "napple-online/pid/v1:");
    for (const char *c = profile_id; *c && n < (int)sizeof in - 1; c++) in[n++] = (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
    sha256(in, (size_t)n, h);
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v = v << 8 | h[i];
    return 0x4e41800000000000ull | (v & 0x7fffffffffffull);
}
int net_online_account_is_identity(const NetOnlineAccount *a) { return !strcmp(a->secret, NET_ONLINE_IDENTITY_SECRET); }

int net_online_account(const OnlineProvider *p, const NetOnlineServer *srv, const char *state_dir, const uint8_t uid[16], NetOnlineAccount *out) {
    memset(out, 0, sizeof *out);
    char tmp[4096], prof[64];
    if (napple_account_access_token(tmp, sizeof tmp - 1) && napple_account_profile_id(prof, sizeof prof)) {
        out->pid = net_online_principal_id(prof);
        snprintf(out->secret, sizeof out->secret, "%s", NET_ONLINE_IDENTITY_SECRET);
        return 0;
    }
    char uidhex[33], dir[1100], file[1200];
    hex(uid, 16, uidhex);
    if (state_dir) {
        snprintf(dir, sizeof dir, "%s/%s", state_dir, srv->key);
        snprintf(file, sizeof file, "%s/%s.txt", dir, uidhex);
        FILE *f = fopen(file, "r");
        if (f) {
            char text[256];
            size_t got = fread(text, 1, sizeof text - 1, f);
            fclose(f);
            text[got] = 0;
            if (parse_account(text, out) == 0) return 0;
        }
    }
    char body[64], resp[1024];
    snprintf(body, sizeof body, "uid=%s", uidhex);
    int st = net_online_http(p, srv, "/napple/v1/account", body, resp, sizeof resp);
    if (st != 200 || parse_account(resp, out)) return -1;
    if (state_dir) {
        mkdirs(dir);
        FILE *f = fopen(file, "w");
        if (f) {
            fprintf(f, "pid=%llu\nsecret=%s\n", (unsigned long long)out->pid, out->secret);
            fclose(f);
            chmod(file, 0600);
        }
    }
    return 0;
}

size_t net_online_token(const OnlineProvider *p, const NetOnlineServer *srv, const NetOnlineAccount *a, uint64_t title_id, char *out, size_t n) {
    char body[256], resp[8192];
    if (net_online_account_is_identity(a)) { /* the account's access token is the game's token (src/online/ refreshes it) */
        size_t t = napple_account_access_token(out, n > 1 ? n - 1 : 0);
        if (t) out[t] = 0;
        return t;
    }
    snprintf(body, sizeof body, "pid=%llu&secret=%s&title=%016llx", (unsigned long long)a->pid, a->secret, (unsigned long long)title_id);
    int st = net_online_http(p, srv, "/napple/v1/token", body, resp, sizeof resp);
    if (st != 200) return 0;
    if (net_online_kv(resp, "token", out, n)) return 0;
    return strlen(out);
}

int net_online_report_friends(const OnlineProvider *p, const NetOnlineServer *srv, const NetOnlineAccount *a, const uint64_t *pids, uint32_t n) {
    if (net_online_account_is_identity(a)) return 0; /* napple-online learns the friends from napple-social */
    size_t cap = 160 + (size_t)n * 21;
    char *body = malloc(cap), resp[256];
    if (!body) return -1;
    int w = snprintf(body, cap, "pid=%llu&secret=%s&friends=", (unsigned long long)a->pid, a->secret);
    for (uint32_t i = 0; i < n && w > 0 && (size_t)w < cap; i++) w += snprintf(body + w, cap - (size_t)w, "%s%llu", i ? "," : "", (unsigned long long)pids[i]);
    int st = net_online_http(p, srv, "/napple/v1/friends", body, resp, sizeof resp);
    free(body);
    return st == 200 ? 0 : -1;
}
