/* Host provider TLS (NAPPLE_NET=host): handshakes over a provider socket to a local `openssl s_server` whose certificate comes from a throw-away CA made
 * for this run. Without the CA the server is refused (the host trust store does not know it); with the CA imported as a guest CA it is accepted; a wrong
 * host name is refused; the guest's verify option can turn checks off. Through the ssl service handlers too, and a public HTTPS server against the host
 * trust store when the Mac is online. */
#include "../src/services/offline.c"
#include "svc_test.h"
#include "../src/net/net_host.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static char dir[256];
static int sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int sh(const char *fmt, ...) {
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    return system(cmd);
}
static void write_file(const char *name, const char *text) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f) fputs(text, f), fclose(f);
}
static size_t read_file(const char *name, uint8_t *buf, size_t cap) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static const OnlineProvider *P;
static uint16_t srv_port;
static int tcp_to(uint16_t port, int *err) {
    int h = P->sock_open(P->ctx, 2, 1, 0, err);
    OnlineAddr a = {.family = 2, .port = port};
    a.addr[0] = 127, a.addr[3] = 1;
    if (h >= 0 && P->sock_connect(P->ctx, h, &a, err) != 0) {
        int e;
        P->sock_close(P->ctx, h, &e);
        return -1;
    }
    return h;
}
/* One TLS client run: 0 when the handshake succeeded and an HTTP answer came back over it, else -1. */
static int tls_get(const char *host, const void *ca, size_t ca_n, int verify) {
    int err, h = tcp_to(srv_port, &err);
    if (h < 0) return -2;
    void *t = P->tls_new(P->ctx, h);
    int rc = -1;
    if (host) P->tls_set_host(P->ctx, t, host);
    if (ca) P->tls_add_ca(P->ctx, t, ca, ca_n);
    if (verify >= 0) P->tls_set_verify(P->ctx, t, (uint32_t)verify);
    uint32_t res = 0;
    if (P->tls_handshake(P->ctx, t, &res) == 0) {
        const char *req = "GET / HTTP/1.0\r\n\r\n";
        char buf[256] = {0};
        int64_t w = P->tls_write(P->ctx, t, req, strlen(req), &err);
        int64_t r = w > 0 ? P->tls_read(P->ctx, t, buf, sizeof buf - 1, &err) : -1;
        rc = r > 8 && !memcmp(buf, "HTTP/1.", 7) ? 0 : -3;
    } else {
        CHECK(res == HZ_R_SSL_NOT_CONNECTED, "a failed handshake reports the ssl result");
    }
    P->tls_free(P->ctx, t);
    P->sock_close(P->ctx, h, &err);
    return rc;
}

int main(void) {
    (void)svc_call;
    snprintf(dir, sizeof dir, "%s/napple-tls-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) return 1;
    write_file("ca.cnf", "[req]\ndistinguished_name=dn\n[dn]\n[v3_ca]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\n");
    write_file("leaf.cnf", "[v3_leaf]\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n"
                           "subjectAltName=DNS:localhost,IP:127.0.0.1\nauthorityKeyIdentifier=keyid\n");
    int ok = sh("cd '%s' && /usr/bin/openssl req -x509 -new -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 2 -subj /CN=napple-test-ca "
                "-config ca.cnf -extensions v3_ca >/dev/null 2>&1 && /usr/bin/openssl req -new -newkey rsa:2048 -nodes -keyout srv.key -out srv.csr "
                "-subj /CN=localhost -config ca.cnf >/dev/null 2>&1 && /usr/bin/openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial "
                "-out srv.pem -days 2 -extfile leaf.cnf -extensions v3_leaf >/dev/null 2>&1 && /usr/bin/openssl x509 -in ca.pem -outform DER -out ca.der",
                dir) == 0;
    CHECK(ok, "made a throw-away CA and a localhost server certificate in %s", dir);
    if (!ok) return 1;

    setenv("NAPPLE_NET", "host", 1);
    unsetenv("NAPPLE_ONLINE_PROVIDER");
    online_provider_reset();
    P = online_provider();
    CHECK(P == net_host_provider(), "host provider selected");
    int err, probe = P->sock_open(P->ctx, 2, 1, 0, &err);
    OnlineAddr any = {.family = 2};
    P->sock_bind(P->ctx, probe, &any, &err);
    OnlineAddr me;
    P->sock_getsockname(P->ctx, probe, &me, &err);
    P->sock_close(P->ctx, probe, &err);
    srv_port = me.port;

    pid_t pid = fork();
    if (pid == 0) {
        char port[16];
        snprintf(port, sizeof port, "%u", srv_port);
        if (chdir(dir) != 0) _exit(127);
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 1), dup2(null, 2);
        execl("/usr/bin/openssl", "openssl", "s_server", "-accept", port, "-cert", "srv.pem", "-key", "srv.key", "-www", "-quiet", (char *)NULL);
        _exit(127);
    }
    int up = 0;
    for (int i = 0; i < 100 && !up; i++) {
        int h = tcp_to(srv_port, &err);
        if (h >= 0) up = 1, P->sock_close(P->ctx, h, &err);
        else usleep(50000);
    }
    CHECK(up, "openssl s_server listening on 127.0.0.1:%u", srv_port);

    uint8_t pem[8192], der[8192];
    size_t pem_n = read_file("ca.pem", pem, sizeof pem), der_n = read_file("ca.der", der, sizeof der);
    CHECK(pem_n > 100 && der_n > 100, "CA certificate read (PEM %zu, DER %zu bytes)", pem_n, der_n);
    CHECK(tls_get("localhost", NULL, 0, -1) == -1, "no guest CA: the host trust store refuses the server");
    CHECK(tls_get("localhost", der, der_n, -1) == 0, "the CA imported as a guest CA (DER): handshake, request and answer over TLS");
    CHECK(tls_get("localhost", pem, pem_n, -1) == 0, "the same CA as PEM text");
    CHECK(tls_get("wrong.example", der, der_n, -1) == -1, "a host name the certificate does not carry is refused");
    CHECK(tls_get("wrong.example", der, der_n, ONLINE_TLS_VERIFY_PEER_CA) == 0, "VerifyOption without HostName: the name is not checked");
    CHECK(tls_get("localhost", NULL, 0, 0) == 0, "VerifyOption 0: nothing is checked");
    CHECK(tls_get("localhost", NULL, 0, ONLINE_TLS_VERIFY_HOST_NAME) == 0, "HostName only: an unknown CA passes, the right name is enough");
    CHECK(tls_get("wrong.example", NULL, 0, ONLINE_TLS_VERIFY_HOST_NAME) == -1, "HostName only: a wrong name is still refused");
    CHECK(P->tls_new(P->ctx, 300) == NULL, "tls_new on an unknown socket handle: NULL");
    CHECK(P->tls_add_ca(P->ctx, P->tls_new(P->ctx, -1), der, der_n) == -1, "tls_add_ca without a connection fails");

    { /* through the ssl and bsd service handlers: the connection is routed to the provider; the server's CA is unknown to the host, so it is refused */
        IpcObject *b0 = bsd_open(), *svc = ssl_open();
        bsdg_reset(bsd_g());
        IpcCall c;
        int32_t sa_args[3] = {2, 1, 0};
        svc_call(b0, 2, sa_args, 12, &c);
        int32_t fd = SVC_OUT(&c, int32_t, 0);
        uint8_t sa[16] = {16, 2, (uint8_t)(srv_port >> 8), (uint8_t)srv_port, 127, 0, 0, 1};
        memset(&c, 0, sizeof c);
        c.in = (const uint8_t *)&fd, c.in_size = 4;
        svc_set_in_buf(&c, sa, 16);
        svc_call_c(b0, 14, &c);
        CHECK(SVC_OUT(&c, int32_t, 0) == 0, "bsd Connect to the TLS server");
        svc_call(svc, 0, NULL, 0, &c);
        IpcObject *ctx = c.out_objects[0];
        svc_call(ctx, 2, NULL, 0, &c);
        IpcObject *conn = c.out_objects[0];
        svc_call(conn, 0, &fd, 4, &c);
        memset(&c, 0, sizeof c);
        svc_set_in_buf(&c, "localhost", 9);
        svc_call_c(conn, 1, &c);
        svc_call(conn, 8, NULL, 0, &c);
        CHECK(c.result == HZ_R_SSL_NOT_CONNECTED, "ssl DoHandshake reaches the server and refuses its unknown CA");
        conn->destroy(conn), ctx->destroy(ctx); /* frees the provider connection (tls_free) */
        bsdg_reset(bsd_g());
    }

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    sh("rm -rf '%s'", dir);

    OnlineNetInfo ni;
    P->net_info(P->ctx, &ni);
    OnlineResolved res;
    if (ni.connected && P->dns_resolve(P->ctx, "example.com", "443", 2, &res) == 0) { /* the host trust store on a real server */
        int h = P->sock_open(P->ctx, 2, 1, 0, &err);
        int c = P->sock_connect(P->ctx, h, &res.addrs[0], &err);
        void *t = c == 0 ? P->tls_new(P->ctx, h) : NULL;
        uint32_t r = 0;
        if (t) P->tls_set_host(P->ctx, t, "example.com");
        int hs = t ? P->tls_handshake(P->ctx, t, &r) : -1;
        if (c == 0) CHECK(hs == 0, "TLS to example.com:443, verified against the host trust store");
        else printf("note: example.com:443 not reachable (errno %d): public TLS check skipped\n", err);
        if (t) P->tls_free(P->ctx, t);
        P->sock_close(P->ctx, h, &err);
    } else {
        printf("note: offline: public TLS check skipped\n");
    }
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
