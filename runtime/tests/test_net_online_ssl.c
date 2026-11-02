/* ssl service, non-blocking connections (napple-online): ISslConnection SetIoMode reaches the provider (tls_set_io_mode, before and after the
 * handshake), and a provider read or write that would block (-1, HZ_EAGAIN) is answered with ResultWouldBlock instead of "not connected", which
 * is what MK8D's NEX loop waits for. Other errors keep their old answer. A fake TLS layer over the host provider's sockets. */
#include "../src/services/offline.c"
#include "check.h"
#include "svc_test.h"
#include "../src/net/net_host.h"

static uint32_t fake_mode, fake_mode_calls;
static int fake_pending, fake_full, fake_broken;
static int fake_conn;
static void *f_new(void *ctx, int sock) { (void)ctx; (void)sock; return &fake_conn; }
static void f_free(void *ctx, void *conn) { (void)ctx; (void)conn; }
static int f_host(void *ctx, void *conn, const char *h) { (void)ctx; (void)conn; (void)h; return 0; }
static int f_handshake(void *ctx, void *conn, uint32_t *res) { (void)ctx; (void)conn; *res = 0; return 0; }
static int f_io_mode(void *ctx, void *conn, uint32_t mode) {
    (void)ctx;
    (void)conn;
    fake_mode = mode;
    fake_mode_calls++;
    return 0;
}
static int64_t f_read(void *ctx, void *conn, void *buf, size_t n, int *err) {
    (void)ctx;
    (void)conn;
    if (fake_broken) return *err = HZ_ECONNRESET, -1;
    if (!fake_pending) {
        if (fake_mode == ONLINE_TLS_IO_NONBLOCKING) return *err = HZ_EAGAIN, -1;
        return 0;
    }
    fake_pending = 0;
    memcpy(buf, "abc", n < 3 ? n : 3);
    return n < 3 ? (int64_t)n : 3;
}
static int64_t f_write(void *ctx, void *conn, const void *buf, size_t n, int *err) {
    (void)ctx;
    (void)conn;
    (void)buf;
    if (fake_full && fake_mode == ONLINE_TLS_IO_NONBLOCKING) return *err = HZ_EAGAIN, -1;
    return (int64_t)n;
}
static int64_t f_pending(void *ctx, void *conn) { (void)ctx; (void)conn; return fake_pending ? 3 : 0; }

static IpcObject *new_conn(IpcObject *svc, IpcObject *b0, int32_t *fd_out, uint32_t mode_before) {
    IpcCall c;
    int32_t sa_args[3] = {2, 1, 0};
    svc_call(b0, 2, sa_args, 12, &c);
    int32_t fd = SVC_OUT(&c, int32_t, 0);
    *fd_out = fd;
    svc_call(svc, 0, NULL, 0, &c);
    IpcObject *ctx = c.out_objects[0];
    svc_call(ctx, 2, NULL, 0, &c);
    IpcObject *conn = c.out_objects[0];
    svc_call(conn, 0, &fd, 4, &c);
    if (mode_before) svc_call(conn, 3, &mode_before, 4, &c);
    svc_call(conn, 8, NULL, 0, &c);
    CHECK(c.result == 0, "DoHandshake over the fake TLS layer");
    return conn;
}
static uint32_t do_read(IpcObject *conn, uint32_t *n) {
    IpcCall c;
    char buf[16];
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, buf, sizeof buf);
    svc_call_c(conn, 10, &c);
    *n = c.result ? 0 : SVC_OUT(&c, uint32_t, 0);
    return c.result;
}
static uint32_t do_write(IpcObject *conn) {
    IpcCall c;
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, "xyz", 3);
    svc_call_c(conn, 11, &c);
    return c.result;
}

int main(void) {
    (void)svc_call;
    static OnlineProvider fake;
    fake = *net_host_provider(); /* real sockets (the ssl service needs a bsd descriptor), fake TLS */
    fake.tls_new = f_new, fake.tls_free = f_free, fake.tls_set_host = f_host, fake.tls_handshake = f_handshake;
    fake.tls_read = f_read, fake.tls_write = f_write, fake.tls_pending = f_pending, fake.tls_set_io_mode = f_io_mode;
    fake.tls_add_ca = NULL, fake.tls_set_verify = NULL;
    online_provider_install(&fake);
    IpcObject *b0 = bsd_open(), *svc = ssl_open();
    bsdg_reset(bsd_g());
    IpcCall c;
    uint32_t n;
    int32_t fd;

    /* SetIoMode before the handshake: the provider gets it with the new connection */
    IpcObject *conn = new_conn(svc, b0, &fd, ONLINE_TLS_IO_NONBLOCKING);
    CHECK(fake_mode == ONLINE_TLS_IO_NONBLOCKING && fake_mode_calls == 1, "IoMode set before DoHandshake reaches the provider once the connection exists");
    CHECK(do_read(conn, &n) == HZ_R_SSL_WOULD_BLOCK, "non-blocking Read with nothing to read: ResultWouldBlock (2123-0204)");
    CHECK(HZ_R_SSL_WOULD_BLOCK == 0x1987bu, "ResultWouldBlock is module 123 description 204");
    fake_pending = 1;
    CHECK(do_read(conn, &n) == 0 && n == 3, "non-blocking Read with data: the bytes");
    fake_full = 1;
    CHECK(do_write(conn) == HZ_R_SSL_WOULD_BLOCK, "non-blocking Write that would block: ResultWouldBlock");
    fake_full = 0;
    CHECK(do_write(conn) == 0, "Write once there is room");
    /* SetIoMode on an established connection goes straight to the provider */
    uint32_t blocking = ONLINE_TLS_IO_BLOCKING;
    svc_call(conn, 3, &blocking, 4, &c);
    CHECK(fake_mode == ONLINE_TLS_IO_BLOCKING && fake_mode_calls == 2, "IoMode changed after the handshake reaches the provider");
    svc_call(conn, 7, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == ONLINE_TLS_IO_BLOCKING, "GetIoMode reports it");
    /* other errors keep the old answer */
    fake_broken = 1;
    CHECK(do_read(conn, &n) == HZ_R_SSL_NOT_CONNECTED, "a broken connection is still 'not connected'");
    fake_broken = 0;
    conn->destroy(conn);
    svc_call(b0, 26, &fd, 4, &c);

    /* no IoMode set: the provider is not told anything (the host provider keeps its default, blocking) */
    fake_mode = 0, fake_mode_calls = 0;
    conn = new_conn(svc, b0, &fd, 0);
    CHECK(fake_mode_calls == 0, "no SetIoMode: tls_set_io_mode is not called");
    conn->destroy(conn);
    svc_call(b0, 26, &fd, 4, &c);

    /* a provider without tls_set_io_mode (older modules, the host provider) still works */
    fake.tls_set_io_mode = NULL;
    fake_mode = ONLINE_TLS_IO_NONBLOCKING;
    conn = new_conn(svc, b0, &fd, ONLINE_TLS_IO_NONBLOCKING);
    CHECK(do_read(conn, &n) == HZ_R_SSL_WOULD_BLOCK, "without tls_set_io_mode a provider's EAGAIN is still ResultWouldBlock");
    conn->destroy(conn);
    svc_call(b0, 26, &fd, 4, &c);

    online_provider_install(NULL);
    bsdg_reset(bsd_g());
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
