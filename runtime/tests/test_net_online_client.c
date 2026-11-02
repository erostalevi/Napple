/* Napple online client glue (runtime/src/net/net_online*.c) against a fake Napple server on localhost: URL parsing, the name policy (game servers
 * redirected, other Nintendo names blocked), nsd, the unchanged host provider without NAPPLE_ONLINE_SERVER, account creation and its saved
 * credentials, the ID token through acc's IManagerForApplication rows, a redirected connection with the title's TLS ended locally (blocking and
 * non-blocking), and an unreachable server leaving the user unlinked. */
#include "../src/services/account.c"
#include "check.h"
#include "svc_test.h"
#include "../src/net/net_host.h"
#include "../src/net/net_online.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- a fake Napple server: the two API calls, and "PING" -> "PONG" for a redirected game connection ---- */
static int fake_fd;
static uint16_t fake_port;
static int fake_accounts, fake_tokens;
static void *fake_server(void *arg) {
    (void)arg;
    for (;;) {
        int c = accept(fake_fd, NULL, NULL);
        if (c < 0) return NULL;
        char buf[4096];
        size_t n = 0;
        for (;;) {
            ssize_t r = recv(c, buf + n, sizeof buf - 1 - n, 0);
            if (r <= 0) break;
            n += (size_t)r;
            buf[n] = 0;
            if (n >= 4 && !memcmp(buf, "PING", 4)) {
                send(c, "PONG", 4, 0);
                n = 0;
                continue;
            }
            char *body = strstr(buf, "\r\n\r\n");
            if (!body) continue;
            const char *cl = strstr(buf, "Content-Length: ");
            size_t want = cl ? (size_t)atoi(cl + 16) : 0;
            if (strlen(body + 4) < want) continue;
            char resp[512], out[256];
            if (!strncmp(buf, "POST /napple/v1/account", 23) && strstr(body, "uid=4e6170706c6555736572")) {
                fake_accounts++;
                snprintf(out, sizeof out, "pid=777\nsecret=00112233445566778899aabbccddeeff\n");
            } else if (!strncmp(buf, "POST /napple/v1/token", 21) && strstr(body, "pid=777&secret=00112233445566778899aabbccddeeff&title=")) {
                fake_tokens++;
                snprintf(out, sizeof out, "token=eyJ.napple-token-%s\nexpires=0\n", strstr(body, "title=") + 6);
            } else {
                snprintf(resp, sizeof resp, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
                send(c, resp, strlen(resp), 0);
                break;
            }
            int len = snprintf(resp, sizeof resp, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(out), out);
            send(c, resp, (size_t)len, 0);
            break;
        }
        close(c);
    }
}
static void fake_start(void) {
    fake_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_len = sizeof a, .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(fake_fd, (struct sockaddr *)&a, sizeof a);
    listen(fake_fd, 8);
    socklen_t l = sizeof a;
    getsockname(fake_fd, (struct sockaddr *)&a, &l);
    fake_port = ntohs(a.sin_port);
    pthread_t t;
    pthread_create(&t, NULL, fake_server, NULL);
    pthread_detach(t);
}

static void set_ip(OnlineAddr *a, uint32_t ip, uint16_t port) {
    memset(a, 0, sizeof *a);
    a->family = 2, a->port = port;
    a->addr[0] = ip >> 24, a->addr[1] = ip >> 16 & 255, a->addr[2] = ip >> 8 & 255, a->addr[3] = ip & 255;
}
static int ip_is(const OnlineAddr *a, uint32_t ip) {
    return a->family == 2 && a->addr[0] == (ip >> 24) && a->addr[1] == (ip >> 16 & 255) && a->addr[2] == (ip >> 8 & 255) && a->addr[3] == (ip & 255);
}

int main(void) {
    (void)svc_call;
    snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "0100152000022000");

    /* ---- server URLs ---- */
    NetOnlineServer s;
    CHECK(net_online_parse_server("http://127.0.0.1:8737", &s) == 0 && !s.tls && !strcmp(s.host, "127.0.0.1") && s.port == 8737 && !strcmp(s.key, "127.0.0.1_8737"),
          "http://127.0.0.1:8737");
    CHECK(net_online_parse_server("https://online.example.org", &s) == 0 && s.tls && s.port == 443 && !strcmp(s.host, "online.example.org"), "https:// defaults to 443");
    CHECK(net_online_parse_server("HTTP://lan-box/napple", &s) == 0 && s.port == 80 && !strcmp(s.host, "lan-box"), "http:// defaults to 80, a path is ignored");
    CHECK(net_online_parse_server("ftp://x", &s) != 0 && net_online_parse_server("http://:80", &s) != 0 && net_online_parse_server("http://h:99999", &s) != 0 &&
          net_online_parse_server("", &s) != 0, "other schemes, no host, a bad port are refused");

    /* ---- name policy and nsd ---- */
    CHECK(net_online_name_policy("g2b309e01-lp1.s.n.srv.nintendo.net") == NET_ONLINE_NAME_REDIRECT, "a NEX game server is redirected");
    CHECK(net_online_name_policy("G2B309E01-lp1.p.srv.nintendo.net.") == NET_ONLINE_NAME_REDIRECT, "its sibling, case-insensitive, fully qualified");
    CHECK(net_online_name_policy("nncs1-lp1.n.n.srv.nintendo.net") == NET_ONLINE_NAME_REDIRECT && net_online_name_policy("nncs2-lp1.n.n.srv.nintendo.net") == NET_ONLINE_NAME_REDIRECT &&
          net_online_name_policy("nncs2.p01.lp1.n.n.srv.nintendo.net") == NET_ONLINE_NAME_REDIRECT, "the NAT check servers are redirected");
    CHECK(net_online_redirect_ip("nncs2-lp1.n.n.srv.nintendo.net") == NET_ONLINE_VIRTUAL_IP2 && net_online_redirect_ip("nncs1-lp1.n.n.srv.nintendo.net") == NET_ONLINE_VIRTUAL_IP &&
          net_online_redirect_ip("g2b309e01-lp1.s.n.srv.nintendo.net") == NET_ONLINE_VIRTUAL_IP, "the secondary NAT check server has its own virtual address");
    const char *blocked[] = {"nncs3-lp1.n.n.srv.nintendo.net", "accounts.nintendo.com", "dauth-lp1.ndas.srv.nintendo.net", "conntest.nintendowifi.net",
                             "g2b309e01-%.s.n.srv.nintendo.net", "nintendo.net", "api-lp1.znc.srv.nintendo.net", "gxyz-lp1.s.n.srv.nintendo.net", "x%y.example"};
    int all = 1;
    for (unsigned i = 0; i < sizeof blocked / sizeof blocked[0]; i++) all &= net_online_name_policy(blocked[i]) == NET_ONLINE_NAME_BLOCK;
    CHECK(all, "other Nintendo names, unfilled templates and malformed game-server names are blocked");
    CHECK(net_online_name_policy("example.com") == NET_ONLINE_NAME_PASS && net_online_name_policy("notnintendo.net") == NET_ONLINE_NAME_PASS &&
          net_online_name_policy("localhost") == NET_ONLINE_NAME_PASS, "everything else resolves through the host");
    char nsd[128];
    CHECK(net_online_nsd("g2b309e01-%.s.n.srv.nintendo.net", nsd, sizeof nsd) == 0 && !strcmp(nsd, "g2b309e01-lp1.s.n.srv.nintendo.net"), "nsd fills the environment");
    CHECK(net_online_nsd("a%b%c", nsd, 6) != 0, "nsd: too small a buffer is an error");

    /* ---- without NAPPLE_ONLINE_SERVER the host provider is unchanged (r6) ---- */
    unsetenv(NET_ONLINE_ENV_SERVER);
    CHECK(net_online_builtin() == net_host_provider(), "NAPPLE_ONLINE_SERVER unset: NAPPLE_NET=host is the bare host provider");
    CHECK(!ONLINE_FN(net_host_provider(), account_info) && !ONLINE_FN(net_host_provider(), nsd_resolve), "the host provider still has no accounts and no nsd");

    /* ---- against the fake server ---- */
    fake_start();
    char url[64], dir[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%u", fake_port);
    snprintf(dir, sizeof dir, "%s/napple-online-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    CHECK(net_online_parse_server(url, &s) == 0, "fake server URL");
    OnlineProvider *p = net_online_new(net_host_provider(), &s, dir);
    CHECK(p && !strcmp(p->name, NET_ONLINE_NAME) && online_has(p, ONLINE_CAP_ACCOUNT) && online_has(p, ONLINE_CAP_NSD) && online_has(p, ONLINE_CAP_SOCKETS) &&
          p->ctx == net_host_provider()->ctx, "the wrapper: accounts and nsd on top of the host's capabilities, sharing its ctx");
    CHECK(p->ldn_scan == net_host_provider()->ldn_scan && p->sock_bind == net_host_provider()->sock_bind, "entry points it does not override are the host's own");
    void *ctx = p->ctx;
    OnlineAccountInfo ai;
    CHECK(p->account_info(ctx, g_user_id, &ai) == 0 && ai.linked && ai.id == 777 && fake_accounts == 1, "first use creates the Napple account on the server");
    char file[512];
    snprintf(file, sizeof file, "%s/%s/4e6170706c6555736572000000000001.txt", dir, s.key);
    struct stat st;
    CHECK(stat(file, &st) == 0 && (st.st_mode & 077) == 0, "credentials are saved per server and user, private to the owner");
    char tok[256];
    size_t n = p->account_id_token(ctx, g_user_id, tok, sizeof tok);
    tok[n < sizeof tok ? n : sizeof tok - 1] = 0;
    CHECK(n && !strcmp(tok, "eyJ.napple-token-0100152000022000") && fake_tokens == 1, "the ID token comes from the server, for the running title");
    n = p->account_id_token(ctx, g_user_id, tok, sizeof tok);
    CHECK(n && fake_tokens == 1, "a fresh token is cached");
    net_online_free(p);
    p = net_online_new(net_host_provider(), &s, dir);
    ctx = p->ctx;
    CHECK(p->account_info(ctx, g_user_id, &ai) == 0 && ai.id == 777 && fake_accounts == 1, "a new run reuses the saved account");

    /* acc rows through the provider */
    online_provider_install(p);
    IpcCall c;
    memset(&c, 0, sizeof c);
    manager_ensure_id_token(NULL, &c);
    CHECK(c.result == 0 && c.nout_objects == 1, "EnsureIdTokenCacheAsync -> IAsyncContext");
    IpcObject *async = c.out_objects[0];
    svc_call(async, 2, NULL, 0, &c);
    CHECK(c.result == 0 && c.out[0] == 1, "IAsyncContext::HasDone");
    svc_call(async, 3, NULL, 0, &c);
    CHECK(c.result == 0, "IAsyncContext::GetResult: success");
    char cache[ACC_ID_TOKEN_MAX];
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, cache, sizeof cache);
    manager_load_id_token(NULL, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == strlen("eyJ.napple-token-0100152000022000") && !memcmp(cache, "eyJ.napple-token-", 17),
          "LoadIdTokenCache: size and token");
    memset(&c, 0, sizeof c);
    manager_account_id(NULL, &c);
    CHECK(SVC_OUT(&c, uint64_t, 0) == 777, "GetAccountId: the Napple account id");
    online_provider_install(NULL);
    memset(&c, 0, sizeof c);
    manager_ensure_id_token(NULL, &c);
    CHECK(c.result == IPC_R_UNKNOWN_COMMAND && c.nout_objects == 0, "offline: EnsureIdTokenCacheAsync keeps its old answer");

    /* DNS */
    OnlineResolved r;
    CHECK(p->dns_resolve(ctx, "g2b309e01-lp1.s.n.srv.nintendo.net", "443", 2, &r) == 0 && r.count == 1 && ip_is(&r.addrs[0], NET_ONLINE_VIRTUAL_IP) &&
          r.addrs[0].port == 443, "the game server resolves to the virtual address");
    CHECK(p->dns_resolve(ctx, "g2b309e01-lp1.s.n.srv.nintendo.net", NULL, 28, &r) == HZ_EAI_NONAME, "no IPv6 answer for it");
    CHECK(p->dns_resolve(ctx, "accounts.nintendo.com", NULL, 0, &r) == HZ_EAI_NONAME && r.count == 0, "another Nintendo name: EAI_NONAME");
    CHECK(p->dns_resolve(ctx, "nncs2-%.n.n.srv.nintendo.net", NULL, 2, &r) == 0 && ip_is(&r.addrs[0], NET_ONLINE_VIRTUAL_IP2), "a template is filled, like the console's resolver does");

    /* the NAT check: datagrams to the virtual servers reach the server's NAT check ports and come back from the virtual servers */
    {
        int nerr = 0, us = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a = {.sin_len = sizeof a, .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(NET_ONLINE_NATCHECK_PORT + NET_ONLINE_NATCHECK_SECONDARY)};
        int ok_bind = bind(us, (struct sockaddr *)&a, sizeof a) == 0;
        int g = p->sock_open(ctx, 2, 2, 0, &nerr);
        OnlineAddr to;
        set_ip(&to, NET_ONLINE_VIRTUAL_IP2, NET_ONLINE_NATCHECK_PORT);
        if (ok_bind && g >= 0) {
            CHECK(p->sock_send(ctx, g, "natcheck", 8, 0, &to, &nerr) == 8, "a datagram to the secondary NAT check server");
            char b[16];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t got = recvfrom(us, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
            CHECK(got == 8, "arrives on the server's NAT check port + 1000");
            sendto(us, "reply", 5, 0, (struct sockaddr *)&from, fl);
            OnlineAddr src;
            usleep(50000);
            CHECK(p->sock_recv(ctx, g, b, sizeof b, 0, &src, &nerr) == 5 && ip_is(&src, NET_ONLINE_VIRTUAL_IP2) && src.port == NET_ONLINE_NATCHECK_PORT,
                  "the reply comes from the secondary server's virtual address and port");
        } else CHECK(1, "NAT check port taken on this Mac: skipped");
        if (g >= 0) p->sock_close(ctx, g, &nerr);
        close(us);
    }
    CHECK(p->dns_resolve(ctx, "localhost", NULL, 2, &r) == 0 && r.count >= 1 && ip_is(&r.addrs[0], 0x7f000001), "other names: the host resolver");
    char outn[128];
    CHECK(p->nsd_resolve(ctx, "g2b309e01-%.s.n.srv.nintendo.net", outn, sizeof outn) == 0 && !strcmp(outn, "g2b309e01-lp1.s.n.srv.nintendo.net"), "nsd through the provider");

    /* a redirected connection and the title's TLS over it */
    int err = 0;
    int h = p->sock_open(ctx, 2, 1, 0, &err);
    OnlineAddr v = r.addrs[0];
    p->dns_resolve(ctx, "g2b309e01-lp1.s.n.srv.nintendo.net", "443", 2, &r);
    v = r.addrs[0];
    CHECK(h >= 0 && p->sock_connect(ctx, h, &v, &err) == 0, "connect to the virtual address reaches the Napple server");
    OnlineAddr peer;
    CHECK(p->sock_getpeername(ctx, h, &peer, &err) == 0 && ip_is(&peer, NET_ONLINE_VIRTUAL_IP) && peer.port == 443, "getpeername reports what the title connected to");
    void *t = p->tls_new(ctx, h);
    uint32_t res = 1;
    CHECK(t && p->tls_set_host(ctx, t, "g2b309e01-lp1.s.n.srv.nintendo.net") == 0 && p->tls_add_ca(ctx, t, "x", 1) == 0 && p->tls_set_verify(ctx, t, 7) == 0 &&
          p->tls_handshake(ctx, t, &res) == 0, "the title's TLS to it ends here (plain http:// server): handshake succeeds");
    CHECK(p->tls_write(ctx, t, "PING", 4, &err) == 4, "write goes to the server");
    char pong[8] = {0};
    CHECK(p->tls_read(ctx, t, pong, sizeof pong, &err) == 4 && !memcmp(pong, "PONG", 4), "read comes from the server");
    CHECK(p->tls_set_io_mode(ctx, t, ONLINE_TLS_IO_NONBLOCKING) == 0 && p->tls_read(ctx, t, pong, sizeof pong, &err) == -1 && err == HZ_EAGAIN,
          "non-blocking with nothing to read: EAGAIN (ResultWouldBlock in the ssl service)");
    p->tls_write(ctx, t, "PING", 4, &err);
    usleep(100000);
    CHECK(p->tls_read(ctx, t, pong, sizeof pong, &err) == 4, "non-blocking with data: read");
    p->tls_free(ctx, t);
    CHECK(p->sock_close(ctx, h, &err) == 0, "close");
    /* a connection elsewhere is not ours: its TLS is the host's */
    int h2 = p->sock_open(ctx, 2, 1, 0, &err);
    void *t2 = p->tls_new(ctx, h2);
    CHECK(t2 != NULL, "TLS over another socket is the host provider's");
    p->tls_free(ctx, t2);
    p->sock_close(ctx, h2, &err);
    net_online_free(p);

    /* ---- an unreachable server: the user stays unlinked, nothing else changes ---- */
    NetOnlineServer dead;
    net_online_parse_server("http://127.0.0.1:1", &dead);
    p = net_online_new(net_host_provider(), &dead, NULL);
    CHECK(p->account_info(p->ctx, g_user_id, &ai) != 0 && !ai.linked, "server unreachable: not linked");
    CHECK(p->account_id_token(p->ctx, g_user_id, tok, sizeof tok) == 0, "and no token");
    net_online_free(p);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    CHECK(system(cmd) == 0, "cleanup");
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
