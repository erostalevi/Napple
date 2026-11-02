/* Unit tests: the ssl rows that became real in ipc-depth-95 (2026-11-02-r10). ISslConnection 9 DoHandshakeGetServerCert, 13 Peek, 14 Poll, 15/24 verify
 * errors, 16 GetNeededServerCertBufferSize, 22/23 options, 25 GetCipherInfo, 26/27 ALPN, 28 SetDtlsSocketDescriptor, 30 SetPrivateOption,
 * 31 SetSrtpCiphers; ISslContext 8-12 (internal PKI, policy OIDs, CRLs, client cert+key); ssl 2/3/10/11 (built-in certificate list), 6, 9.
 * The NullProvider side first (what an offline console answers), then on macOS the host provider (NAPPLE_NET=host) against a local `openssl s_server`
 * with a throw-away CA, as test_net_host_tls.c does. offline.c is #included. */
#include "../src/services/offline.c"
#include "svc_test.h"
#ifdef __APPLE__
#include "../src/net/net_host.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static IpcCall C;
/* One call with optional argument bytes, one input buffer and one output buffer. */
static IpcCall *call(IpcObject *o, unsigned cmd, const void *in, uint32_t n, const void *ib, uint64_t ibn, void *ob, uint64_t obn) {
    memset(&C, 0, sizeof C);
    C.in = in, C.in_size = n;
    if (ib) svc_set_in_buf(&C, ib, ibn);
    if (ob) svc_set_out_buf(&C, ob, obn);
    svc_call_c(o, cmd, &C);
    return &C;
}
static IpcObject *new_conn(IpcObject *ctx) {
    IpcCall c;
    svc_call(ctx, 2, NULL, 0, &c);
    return c.out_objects[0];
}
static int32_t tsock(IpcObject *b) {
    IpcCall c;
    int32_t a[3] = {2, 1, 0};
    svc_call(b, 2, a, 12, &c);
    return SVC_OUT(&c, int32_t, 0);
}

static void offline_side(void) {
    IpcCall c;
    unsetenv("NAPPLE_NET");
    unsetenv("NAPPLE_ONLINE_PROVIDER");
    online_provider_reset();
    CHECK(online_provider() == online_null_provider(), "NullProvider active");
    IpcObject *sv = ssl_open(), *bs = bsd_open();
    bsdg_reset(bsd_g());
    svc_call(sv, 0, NULL, 0, &c);
    IpcObject *cx = c.out_objects[0], *cn = new_conn(cx);
    SslConn *k = cn->state;

    /* ---- table: converted rows are real, the kept ones stay stubs */
    const unsigned conn_impl[] = {9, 13, 14, 15, 16, 22, 23, 24, 25, 26, 27, 28, 30, 31}, conn_stub[] = {29, 32, 33, 36, 37};
    for (unsigned i = 0; i < sizeof conn_impl / sizeof *conn_impl; i++) CHECK(svc_find(cn, conn_impl[i])->status == IPC_IMPL, "ISslConnection %u is real", conn_impl[i]);
    for (unsigned i = 0; i < sizeof conn_stub / sizeof *conn_stub; i++) CHECK(svc_find(cn, conn_stub[i])->status == IPC_STUB, "ISslConnection %u stays a stub", conn_stub[i]);
    for (unsigned i = 8; i <= 12; i++) CHECK(svc_find(cx, i)->status == IPC_IMPL, "ISslContext %u is real", i);
    CHECK(svc_find(cx, 13)->status == IPC_STUB, "ISslContext 13 GeneratePrivateKeyAndCert stays a stub");
    const unsigned svc_impl[] = {2, 3, 6, 9, 10, 11};
    for (unsigned i = 0; i < 6; i++) CHECK(svc_find(sv, svc_impl[i])->status == IPC_IMPL, "ssl %u is real", svc_impl[i]);

    /* ---- before SetSocketDescriptor */
    uint32_t pin[2] = {SSL_POLL_READ | SSL_POLL_WRITE, 0}, one = 1, two = 2;
    uint8_t buf[0x48];
    CHECK(call(cn, 14, pin, 8, NULL, 0, NULL, 0)->result == HZ_R_SSL_NOT_CONNECTED, "Poll without a socket: not connected");
    CHECK(call(cn, 13, NULL, 0, NULL, 0, buf, 16)->result == HZ_R_SSL_NOT_CONNECTED, "Peek before a handshake: not connected");
    CHECK(call(cn, 25, &one, 4, NULL, 0, buf, 0x48)->result == HZ_R_SSL_NOT_CONNECTED, "GetCipherInfo without a socket: not connected");
    CHECK(call(cn, 25, &two, 4, NULL, 0, buf, 0x48)->result == HZ_R_SSL_INVALID_ARGUMENT, "GetCipherInfo: the version word must be 1");
    CHECK(call(cn, 25, &one, 4, NULL, 0, buf, 0x40)->result == HZ_R_SSL_INVALID_ARGUMENT, "GetCipherInfo: the buffer must be 0x48 bytes");
    const uint8_t alpn[] = "\x02h2\x08http/1.1";
    CHECK(call(cn, 26, NULL, 0, alpn, sizeof alpn - 1, NULL, 0)->result == HZ_R_SSL_NOT_CONNECTED, "SetNextAlpnProto needs a socket");
    CHECK(call(cn, 27, NULL, 0, NULL, 0, buf, 16)->result == HZ_R_SSL_NOT_CONNECTED, "GetNextAlpnProto needs a socket");

    /* ---- 22 / 23 options */
    struct { uint8_t flag, pad[3]; uint32_t opt; } so = {1, {0}, SSL_OPT_DO_NOT_CLOSE_SOCKET};
    CHECK(call(cn, 22, &so, 8, NULL, 0, NULL, 0)->result == 0, "SetOption DoNotCloseSocket before the socket");
    uint32_t o = SSL_OPT_DO_NOT_CLOSE_SOCKET;
    CHECK(call(cn, 23, &o, 4, NULL, 0, NULL, 0)->result == 0 && C.out_size == 1 && C.out[0] == 1, "GetOption reads it back (u8)");
    so.opt = SSL_OPT_GET_SERVER_CERT_CHAIN;
    call(cn, 22, &so, 8, NULL, 0, NULL, 0);
    o = SSL_OPT_GET_SERVER_CERT_CHAIN;
    CHECK(call(cn, 23, &o, 4, NULL, 0, NULL, 0)->out[0] == 1, "GetServerCertChain set");
    so.flag = 0;
    call(cn, 22, &so, 8, NULL, 0, NULL, 0);
    CHECK(call(cn, 23, &o, 4, NULL, 0, NULL, 0)->out[0] == 0, "...and cleared");
    o = SSL_OPT_ENABLE_ALPN;
    CHECK(call(cn, 23, &o, 4, NULL, 0, NULL, 0)->out[0] == 0, "EnableAlpn off by default");
    so.opt = 4;
    CHECK(call(cn, 22, &so, 8, NULL, 0, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "SetOption: an unknown option is refused");
    o = 4;
    CHECK(call(cn, 23, &o, 4, NULL, 0, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "GetOption: an unknown option is refused");
    IpcObject *cn2 = new_conn(cx);
    o = SSL_OPT_DO_NOT_CLOSE_SOCKET;
    CHECK(call(cn2, 23, &o, 4, NULL, 0, NULL, 0)->out[0] == 0, "options are per connection");

    /* ---- with an (offline) socket */
    int32_t fd = tsock(bs);
    CHECK(fd >= 0, "bsd Socket under the NullProvider");
    svc_call(cn, 0, &fd, 4, &c);
    so = (typeof(so)){1, {0}, SSL_OPT_DO_NOT_CLOSE_SOCKET};
    CHECK(call(cn, 22, &so, 8, NULL, 0, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "DoNotCloseSocket after SetSocketDescriptor is refused");
    so.opt = SSL_OPT_ENABLE_ALPN;
    CHECK(call(cn, 22, &so, 8, NULL, 0, NULL, 0)->result == 0, "...other options still apply");
    pin[1] = 50;
    CHECK(call(cn, 14, pin, 8, NULL, 0, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0, "Poll offline: nothing ever becomes ready (timeout, no events)");
    int32_t bogus = 77;
    svc_call(cn2, 0, &bogus, 4, &c);
    CHECK(call(cn2, 14, pin, 8, NULL, 0, NULL, 0)->result == HZ_R_SSL_NOT_CONNECTED, "Poll on a descriptor bsd does not know: not connected");
    CHECK(call(cn, 26, NULL, 0, alpn, sizeof alpn - 1, NULL, 0)->result == 0 && k->alpn_n == sizeof alpn - 1 && !memcmp(k->alpn, alpn, k->alpn_n),
          "SetNextAlpnProto keeps the list");
    CHECK(call(cn, 26, NULL, 0, "\x05h2", 3, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...an entry past the buffer is refused");
    CHECK(call(cn, 26, NULL, 0, "\x02", 1, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...a buffer under 2 bytes is refused");
    CHECK(call(cn, 26, NULL, 0, "\x02h2\x00", 4, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...an empty entry is refused");
    CHECK(k->alpn_n == sizeof alpn - 1, "...and a refused list leaves the kept one");
    memset(buf, 0xee, sizeof buf);
    CHECK(call(cn, 27, NULL, 0, NULL, 0, buf, 16)->result == 0 && C.out_size == 8 && SVC_OUT(&C, uint32_t, 0) == 0 && SVC_OUT(&C, uint32_t, 4) == 0,
          "GetNextAlpnProto before a handshake: NoSupport, empty");

    /* ---- handshake under the NullProvider, and the state it leaves */
    CHECK(call(cn, 8, NULL, 0, NULL, 0, NULL, 0)->result == HZ_R_SSL_NOT_CONNECTED, "DoHandshake offline: not connected");
    CHECK(call(cn, 9, NULL, 0, NULL, 0, buf, sizeof buf)->result == HZ_R_SSL_NOT_CONNECTED, "DoHandshakeGetServerCert offline: not connected");
    CHECK(call(cn, 15, NULL, 0, NULL, 0, NULL, 0)->result == 0, "GetVerifyCertError: no handshake ran, nothing to report");
    uint32_t errs[4] = {9, 9, 9, 9};
    CHECK(call(cn, 24, NULL, 0, NULL, 0, errs, sizeof errs)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0 && SVC_OUT(&C, uint32_t, 4) == 0 && errs[0] == 9,
          "GetVerifyCertErrors: none");
    CHECK(call(cn, 16, NULL, 0, NULL, 0, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0, "GetNeededServerCertBufferSize: 0");
    CHECK(call(cn, 25, &one, 4, NULL, 0, buf, 0x48)->result == HZ_R_SSL_NOT_CONNECTED, "GetCipherInfo before a passed handshake: not connected");

    /* ---- 28 SetDtlsSocketDescriptor */
    IpcObject *dn = new_conn(cx);
    uint8_t sa[16] = {16, 2, 0x12, 0x34, 127, 0, 0, 1};
    int32_t ufd = 5;
    CHECK(call(dn, 28, &ufd, 4, sa, 8, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "SetDtlsSocketDescriptor: a SockAddr under 0x10 bytes is refused");
    sa[0] = 15;
    CHECK(call(dn, 28, &ufd, 4, sa, 16, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...byte 0 must be the buffer size");
    sa[0] = 16;
    CHECK(call(dn, 28, &ufd, 4, sa, 16, NULL, 0)->result == 0 && SVC_OUT(&C, int32_t, 0) == 5, "SetDtlsSocketDescriptor -> the descriptor");
    SslConn *dk = dn->state;
    CHECK(dk->dtls && dk->fd_set && dk->fd == 5 && dk->dtls_addr_n == 16 && dk->dtls_addr[2] == 0x12, "...kept with the peer address");
    svc_call(dn, 4, NULL, 0, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 5, "GetSocketDescriptor reports it");
    CHECK(call(dn, 28, &ufd, 4, sa, 16, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...only once per connection");
    CHECK(call(dn, 8, NULL, 0, NULL, 0, NULL, 0)->result == HZ_R_SSL_NOT_CONNECTED, "a DTLS connection cannot be handshaken (no provider runs DTLS)");

    /* ---- 30 SetPrivateOption, 31 SetSrtpCiphers */
    uint32_t po[2] = {1, SSL_PRIV_DTLS_SESSION}, mode = 1;
    CHECK(call(dn, 30, po, 8, NULL, 0, NULL, 0)->result == 0 && dk->priv_dtls_session == 1, "SetPrivateOption DtlsSession");
    CHECK(call(dn, 17, &mode, 4, NULL, 0, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...then a non-zero SetSessionCacheMode is refused");
    svc_call(dn, 18, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "...and the mode is unchanged");
    mode = 0;
    CHECK(call(dn, 17, &mode, 4, NULL, 0, NULL, 0)->result == 0, "...mode 0 is still accepted");
    mode = 2;
    CHECK(call(cn, 17, &mode, 4, NULL, 0, NULL, 0)->result == 0, "a connection without DtlsSession takes any mode");
    po[0] = 0;
    call(dn, 30, po, 8, NULL, 0, NULL, 0);
    mode = 1;
    CHECK(call(dn, 17, &mode, 4, NULL, 0, NULL, 0)->result == 0, "DtlsSession cleared: the mode is accepted again");
    po[0] = 0xc02f, po[1] = SSL_PRIV_SET_CIPHER;
    CHECK(call(dn, 30, po, 8, NULL, 0, NULL, 0)->result == 0 && dk->priv_cipher == 0xc02f, "SetPrivateOption SetCipher keeps the suite");
    po[1] = 3;
    CHECK(call(dn, 30, po, 8, NULL, 0, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...an unknown private option is refused");
    uint16_t sr[5] = {1, 7, 2, 0, 1};
    CHECK(call(dn, 31, NULL, 0, sr, 6, NULL, 0)->result == 0 && dk->nsrtp == 2 && dk->srtp[0] == 1 && dk->srtp[1] == 2, "SetSrtpCiphers keeps 1 and 2, ignores 7");
    CHECK(call(dn, 31, NULL, 0, sr, 5, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...an odd size is refused");
    CHECK(call(dn, 31, NULL, 0, sr, 10, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT && dk->nsrtp == 2, "...more than 4 entries are refused");

    /* ---- ISslContext 8-12 */
    SslCtx *x = cx->state;
    uint32_t ip = 0;
    CHECK(call(cx, 8, &ip, 4, NULL, 0, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "RegisterInternalPki: only DeviceClientCertDefault (1)");
    ip = 1;
    CHECK(call(cx, 8, &ip, 4, NULL, 0, NULL, 0)->result == 0, "RegisterInternalPki(DeviceClientCertDefault)");
    uint64_t ipki = SVC_OUT(&C, uint64_t, 0);
    CHECK(ipki != 0 && x->client_id == ipki && x->npki == 1 && x->pki[0] == ipki, "...an id kept as the context's client certificate");
    CHECK(call(cx, 8, &ip, 4, NULL, 0, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...a second time is refused");
    call(cx, 7, &ipki, 8, NULL, 0, NULL, 0);
    CHECK(x->client_id == 0 && x->npki == 0, "RemoveClientPki drops it");
    CHECK(call(cx, 8, &ip, 4, NULL, 0, NULL, 0)->result == 0 && SVC_OUT(&C, uint64_t, 0) != ipki, "...then it can be registered again (a new id)");
    svc_call(sv, 0, NULL, 0, &c);
    IpcObject *cx2 = c.out_objects[0];
    svc_call(cx2, 5, NULL, 0, &c);
    CHECK(call(cx2, 8, &ip, 4, NULL, 0, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "after ImportClientPki RegisterInternalPki is refused");
    CHECK(call(cx, 9, NULL, 0, "1.2.3.4", 8, NULL, 0)->result == 0 && x->noid == 1 && !strcmp(x->oid[0], "1.2.3.4"), "AddPolicyOid keeps the string");
    CHECK(call(cx, 9, NULL, 0, "1.2.3.4", 7, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...the NUL must be inside the buffer");
    char longoid[0x110];
    memset(longoid, '1', 0xFF), longoid[0xFF] = 0;
    CHECK(call(cx, 9, NULL, 0, longoid, 0x100, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...at most 0xFE characters");
    longoid[0xFE] = 0;
    CHECK(call(cx, 9, NULL, 0, longoid, 0x100, NULL, 0)->result == 0 && x->noid == 2, "...0xFE is accepted");
    const uint8_t crl[] = {0x30, 0x03, 0x02, 0x01, 0x00}, bad[] = {0x30, 0x05, 0x02};
    CHECK(call(cx, 10, NULL, 0, crl, sizeof crl, NULL, 0)->result == 0, "ImportCrl (DER)");
    uint64_t crl1 = SVC_OUT(&C, uint64_t, 0);
    call(cx, 10, NULL, 0, crl, sizeof crl, NULL, 0);
    uint64_t crl2 = SVC_OUT(&C, uint64_t, 0);
    CHECK(crl1 && crl2 && crl1 != crl2 && x->ncrl == 2, "...two CRLs, two ids");
    CHECK(crl1 != x->client_id, "...in the PKI id space");
    CHECK(call(cx, 10, NULL, 0, bad, sizeof bad, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...a truncated DER element is refused");
    CHECK(call(cx, 10, NULL, 0, NULL, 0, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...an empty buffer is refused");
    CHECK(call(cx, 11, &crl1, 8, NULL, 0, NULL, 0)->result == 0 && x->ncrl == 1 && x->crl[0] == crl2, "RemoveCrl drops one id");
    CHECK(call(cx, 11, &crl1, 8, NULL, 0, NULL, 0)->result == HZ_R_SSL_NOT_FOUND, "...an id that is not there is refused");
    {
        IpcCall *q = &C;
        uint32_t fmt = 2;
        memset(q, 0, sizeof *q);
        q->in = (const uint8_t *)&fmt, q->in_size = 4;
        svc_set_in_buf(q, crl, sizeof crl), svc_set_in_buf(q, crl, sizeof crl);
        svc_call_c(cx2, 12, q);
        CHECK(q->result == 0 && SVC_OUT(q, uint64_t, 0) != 0, "ImportClientCertKeyPki (DER) -> an id");
        fmt = 3;
        memset(q, 0, sizeof *q);
        q->in = (const uint8_t *)&fmt, q->in_size = 4;
        svc_set_in_buf(q, crl, sizeof crl), svc_set_in_buf(q, crl, sizeof crl);
        svc_call_c(cx2, 12, q);
        CHECK(q->result == HZ_R_SSL_INVALID_ARGUMENT, "...an unknown format is refused");
        fmt = 1;
        memset(q, 0, sizeof *q);
        q->in = (const uint8_t *)&fmt, q->in_size = 4;
        svc_set_in_buf(q, crl, sizeof crl), svc_set_in_buf(q, crl, sizeof crl);
        svc_call_c(cx2, 12, q);
        CHECK(q->result == HZ_R_SSL_INVALID_ARGUMENT, "...PEM format with DER data is refused");
        const char pc[] = "-----BEGIN CERTIFICATE-----\nAA==\n-----END CERTIFICATE-----\n", pk[] = "-----BEGIN PRIVATE KEY-----\nAA==\n-----END PRIVATE KEY-----\n";
        memset(q, 0, sizeof *q);
        q->in = (const uint8_t *)&fmt, q->in_size = 4;
        svc_set_in_buf(q, pc, sizeof pc - 1), svc_set_in_buf(q, pk, sizeof pk - 1);
        svc_call_c(cx2, 12, q);
        CHECK(q->result == 0, "...PEM data with format 1 is taken");
        memset(q, 0, sizeof *q);
        q->in = (const uint8_t *)&fmt, q->in_size = 4;
        svc_set_in_buf(q, pc, sizeof pc - 1);
        svc_call_c(cx2, 12, q);
        CHECK(q->result == HZ_R_SSL_INVALID_ARGUMENT, "...the key buffer is required");
    }
    SslCtx *x2 = cx2->state;
    unsigned before = x2->npki;
    uint64_t cl = x2->client_id;
    CHECK(cl != 0 && before == 3, "the client imports of a context are listed as PKIs");

    /* ---- ssl service: the built-in certificate list (empty: no CertStore is shipped) */
    uint32_t all = SSL_CA_ALL, id1 = 1, idx = 0;
    uint8_t cb[256];
    CHECK(call(sv, 11, NULL, 0, NULL, 0, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0, "GetTrustedCertificateCount: 0");
    CHECK(call(sv, 3, NULL, 0, &all, 4, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0, "GetCertificateBufSize(All): 0");
    CHECK(call(sv, 2, NULL, 0, &all, 4, cb, sizeof cb)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0, "GetCertificates(All): no entries");
    CHECK(call(sv, 2, NULL, 0, &id1, 4, cb, sizeof cb)->result == HZ_R_SSL_NOT_FOUND, "GetCertificates(NintendoCAG3): not in the list");
    CHECK(call(sv, 3, NULL, 0, &id1, 4, NULL, 0)->result == HZ_R_SSL_NOT_FOUND, "GetCertificateBufSize(NintendoCAG3): not in the list");
    CHECK(call(sv, 10, &idx, 4, NULL, 0, cb, sizeof cb)->result == HZ_R_SSL_NOT_FOUND, "GetCertificateByIndex(0): past the count");
    { /* a list installed by the test: two trusted, one removed */
        static const uint8_t d1[] = {0x30, 1, 0xa1}, d2[] = {0x30, 2, 0xb1, 0xb2}, d3[] = {0x30, 0};
        static const SslBuiltinCert list[] = {{1, 1, d1, 3}, {5, 0, d3, 2}, {9, 1, d2, 4}};
        ssl_builtin = list, ssl_nbuiltin = 3;
        CHECK(call(sv, 11, NULL, 0, NULL, 0, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 2, "GetTrustedCertificateCount: the EnabledTrusted entries");
        CHECK(call(sv, 3, NULL, 0, &all, 4, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 3 * 0x18 + 9, "GetCertificateBufSize(All): infos + data");
        uint32_t need = SVC_OUT(&C, uint32_t, 0);
        CHECK(call(sv, 2, NULL, 0, &all, 4, cb, need - 1)->result == HZ_R_SSL_INVALID_ARGUMENT, "GetCertificates into a buffer one byte short: refused");
        memset(cb, 0, sizeof cb);
        CHECK(call(sv, 2, NULL, 0, &all, 4, cb, sizeof cb)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 3, "GetCertificates(All): 3 entries");
        uint32_t e0[2], e2[2];
        uint64_t sz2, off2;
        memcpy(e0, cb, 8), memcpy(e2, cb + 0x30, 8), memcpy(&sz2, cb + 0x38, 8), memcpy(&off2, cb + 0x40, 8);
        CHECK(e0[0] == 1 && e0[1] == 1 && e2[0] == 9 && sz2 == 4 && off2 == 0x48 + 3 + 2 && !memcmp(cb + off2, d2, 4), "...BuiltInCertificateInfo + DER at the offsets");
        uint32_t two_ids[2] = {9, 1};
        CHECK(call(sv, 3, NULL, 0, two_ids, 8, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 2 * 0x18 + 7, "GetCertificateBufSize of two ids");
        idx = 1;
        CHECK(call(sv, 10, &idx, 4, NULL, 0, cb, sizeof cb)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0x18 + 4 && cb[0] == 9 && !memcmp(cb + 0x18, d2, 4),
              "GetCertificateByIndex(1): the second trusted entry");
        idx = 2;
        CHECK(call(sv, 10, &idx, 4, NULL, 0, cb, sizeof cb)->result == HZ_R_SSL_NOT_FOUND, "GetCertificateByIndex(count): refused");
        id1 = 7;
        CHECK(call(sv, 3, NULL, 0, &id1, 4, NULL, 0)->result == HZ_R_SSL_NOT_FOUND, "an id not in the list is refused");
        ssl_builtin = NULL, ssl_nbuiltin = 0;
    }
    uint32_t ft = 1;
    CHECK(call(sv, 6, &ft, 4, NULL, 0, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0, "FlushSessionCache(AllHosts): nothing cached");
    ft = 0;
    CHECK(call(sv, 6, &ft, 4, "example.com", 12, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0, "FlushSessionCache(SingleHost)");
    CHECK(call(sv, 6, &ft, 4, NULL, 0, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...SingleHost without a host is refused");
    ft = 2;
    CHECK(call(sv, 6, &ft, 4, NULL, 0, NULL, 0)->result == HZ_R_SSL_INVALID_ARGUMENT, "...an unknown type is refused");
    CHECK(call(sv, 9, NULL, 0, NULL, 0, NULL, 0)->result == 0 && C.out_size == 0, "ClearTls12FallbackFlag: no output");

    cn->destroy(cn), cn2->destroy(cn2), dn->destroy(dn), cx->destroy(cx), cx2->destroy(cx2);
    bsdg_reset(bsd_g());
}

#ifdef __APPLE__
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
static size_t read_file(const char *name, uint8_t *b, size_t cap) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(b, 1, cap, f);
    fclose(f);
    return n;
}
static uint16_t srv_port;
/* A bsd socket connected to the local TLS server. */
static int32_t connected(IpcObject *bs) {
    int32_t fd = tsock(bs);
    uint8_t sa[16] = {16, 2, (uint8_t)(srv_port >> 8), (uint8_t)srv_port, 127, 0, 0, 1};
    call(bs, 14, &fd, 4, sa, 16, NULL, 0);
    return SVC_OUT(&C, int32_t, 0) == 0 ? fd : -1;
}
static void host_side(void) {
    snprintf(dir, sizeof dir, "%s/napple-ssl-r10-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) return;
    write_file("ca.cnf", "[req]\ndistinguished_name=dn\n[dn]\n[v3_ca]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\n");
    write_file("leaf.cnf", "[v3_leaf]\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n"
                           "subjectAltName=DNS:localhost,IP:127.0.0.1\nauthorityKeyIdentifier=keyid\n");
    int ok = sh("cd '%s' && /usr/bin/openssl req -x509 -new -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 2 -subj /CN=napple-test-ca "
                "-config ca.cnf -extensions v3_ca >/dev/null 2>&1 && /usr/bin/openssl req -new -newkey rsa:2048 -nodes -keyout srv.key -out srv.csr "
                "-subj /CN=localhost -config ca.cnf >/dev/null 2>&1 && /usr/bin/openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial "
                "-out srv.pem -days 2 -extfile leaf.cnf -extensions v3_leaf >/dev/null 2>&1 && /usr/bin/openssl x509 -in ca.pem -outform DER -out ca.der",
                dir) == 0;
    CHECK(ok, "made a throw-away CA and a localhost server certificate");
    if (!ok) return;
    setenv("NAPPLE_NET", "host", 1);
    online_provider_reset();
    const OnlineProvider *P = online_provider();
    CHECK(P == net_host_provider(), "host provider selected");
    int err, probe = P->sock_open(P->ctx, 2, 1, 0, &err);
    OnlineAddr any = {.family = 2}, me;
    P->sock_bind(P->ctx, probe, &any, &err);
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
    IpcObject *bs = bsd_open(), *sv = ssl_open();
    bsdg_reset(bsd_g());
    int32_t fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) {
        fd = connected(bs);
        if (fd < 0) usleep(50000);
    }
    CHECK(fd >= 0, "openssl s_server reachable on 127.0.0.1:%u", srv_port);
    uint8_t der[8192];
    size_t der_n = read_file("ca.der", der, sizeof der);
    IpcCall c;
    svc_call(sv, 0, NULL, 0, &c);
    IpcObject *cx = c.out_objects[0];

    /* a refused handshake: the CA is not known to the host */
    IpcObject *cn = new_conn(cx);
    svc_call(cn, 0, &fd, 4, &c);
    call(cn, 1, NULL, 0, "localhost", 9, NULL, 0);
    uint32_t pin[2] = {SSL_POLL_WRITE, 2000};
    CHECK(call(cn, 14, pin, 8, NULL, 0, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == SSL_POLL_WRITE, "Poll before the handshake: the connected socket is writable");
    CHECK(call(cn, 8, NULL, 0, NULL, 0, NULL, 0)->result == HZ_R_SSL_NOT_CONNECTED, "DoHandshake: the unknown CA is refused");
    uint32_t errs[4] = {0};
    CHECK(call(cn, 24, NULL, 0, NULL, 0, errs, sizeof errs)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 1 && SVC_OUT(&C, uint32_t, 4) == 1 &&
              errs[0] == HZ_R_SSL_NOT_CONNECTED,
          "GetVerifyCertErrors: the refused handshake's result");
    CHECK(call(cn, 24, NULL, 0, NULL, 0, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0 && SVC_OUT(&C, uint32_t, 4) == 1, "...no buffer: 0 written of 1");
    CHECK(call(cn, 15, NULL, 0, NULL, 0, NULL, 0)->result == HZ_R_SSL_NOT_CONNECTED, "GetVerifyCertError: the refused handshake's result");
    CHECK(call(cn, 15, NULL, 0, NULL, 0, NULL, 0)->result == 0, "...cleared once read");
    uint8_t ci[0x48];
    uint32_t one = 1;
    CHECK(call(cn, 25, &one, 4, NULL, 0, ci, sizeof ci)->result == HZ_R_SSL_NOT_CONNECTED, "GetCipherInfo after a refused handshake: not connected");
    cn->destroy(cn);
    svc_call(bs, 26, &fd, 4, &c);

    /* the CA imported: DoHandshakeGetServerCert passes, then Poll / Peek / Pending / Read over the real TLS stream */
    uint32_t fmt = 1;
    memset(&C, 0, sizeof C);
    C.in = (const uint8_t *)&fmt, C.in_size = 4;
    svc_set_in_buf(&C, der, der_n);
    svc_call_c(cx, 4, &C);
    fd = connected(bs);
    cn = new_conn(cx);
    SslConn *k = cn->state;
    svc_call(cn, 0, &fd, 4, &c);
    call(cn, 1, NULL, 0, "localhost", 9, NULL, 0);
    struct { uint8_t flag, pad[3]; uint32_t opt; } so = {1, {0}, SSL_OPT_GET_SERVER_CERT_CHAIN};
    call(cn, 22, &so, 8, NULL, 0, NULL, 0);
    uint8_t certs[4096];
    CHECK(call(cn, 9, NULL, 0, NULL, 0, certs, sizeof certs)->result == 0 && C.out_size == 8, "DoHandshakeGetServerCert with the imported CA: established");
    CHECK(SVC_OUT(&C, uint32_t, 0) == 0 && SVC_OUT(&C, uint32_t, 4) == 0, "...no certificate bytes (the provider exposes none)");
    CHECK(call(cn, 16, NULL, 0, NULL, 0, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0, "GetNeededServerCertBufferSize agrees: 0");
    CHECK(call(cn, 15, NULL, 0, NULL, 0, NULL, 0)->result == 0, "GetVerifyCertError: a passed handshake has none");
    CHECK(call(cn, 24, NULL, 0, NULL, 0, errs, sizeof errs)->result == 0 && SVC_OUT(&C, uint32_t, 4) == 0, "GetVerifyCertErrors: none");
    memset(ci, 0xee, sizeof ci);
    CHECK(call(cn, 25, &one, 4, NULL, 0, ci, sizeof ci)->result == 0 && ci[0] == 0 && ci[0x40] == 0, "GetCipherInfo on an established connection: written");
    uint8_t ab[16];
    CHECK(call(cn, 27, NULL, 0, NULL, 0, ab, sizeof ab)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0, "GetNextAlpnProto after the handshake: NoSupport (none offered)");
    pin[0] = SSL_POLL_READ, pin[1] = 0;
    CHECK(call(cn, 14, pin, 8, NULL, 0, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 0, "Poll(Read, 0) before the request: nothing to read");
    const char *req = "GET / HTTP/1.0\r\n\r\n";
    CHECK(call(cn, 11, NULL, 0, req, strlen(req), NULL, 0)->result == 0, "Write the request");
    pin[0] = SSL_POLL_READ, pin[1] = 5000;
    CHECK(call(cn, 14, pin, 8, NULL, 0, NULL, 0)->result == 0 && (SVC_OUT(&C, uint32_t, 0) & SSL_POLL_READ), "Poll(Read): the answer arrived");
    char pk[8] = {0}, pk2[8] = {0}, rd[8] = {0};
    CHECK(call(cn, 13, NULL, 0, NULL, 0, pk, 7)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 7 && !memcmp(pk, "HTTP/1.", 7), "Peek: the answer's first bytes");
    CHECK(call(cn, 13, NULL, 0, NULL, 0, pk2, 4)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 4 && !memcmp(pk2, "HTTP", 4), "Peek again: the same bytes");
    svc_call(cn, 12, NULL, 0, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) >= 7, "Pending counts the peeked bytes");
    pin[0] = SSL_POLL_READ, pin[1] = 0;
    CHECK(call(cn, 14, pin, 8, NULL, 0, NULL, 0)->result == 0 && SVC_OUT(&C, uint32_t, 0) == SSL_POLL_READ, "Poll(Read, 0): ready at once with peeked bytes");
    CHECK(call(cn, 10, NULL, 0, NULL, 0, rd, 4)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 4 && !memcmp(rd, "HTTP", 4), "Read returns the peeked bytes");
    CHECK(k->npeek == 3, "...and consumes them");
    memset(rd, 0, sizeof rd);
    CHECK(call(cn, 10, NULL, 0, NULL, 0, rd, 7)->result == 0 && SVC_OUT(&C, uint32_t, 0) == 3 && !memcmp(rd, "/1.", 3), "...the rest of the peek, then");
    char more[64] = {0};
    CHECK(call(cn, 10, NULL, 0, NULL, 0, more, sizeof more - 1)->result == 0 && SVC_OUT(&C, uint32_t, 0) > 0, "Read goes back to the provider");
    cn->destroy(cn), cx->destroy(cx);
    svc_call(bs, 26, &fd, 4, &c);
    bsdg_reset(bsd_g());
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    sh("rm -rf '%s'", dir);
    unsetenv("NAPPLE_NET");
    online_provider_reset();
}
#endif

int main(void) {
    offline_side();
#ifdef __APPLE__
    host_side();
#endif
    printf("{\"test\":\"svc_ssl_r10\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
