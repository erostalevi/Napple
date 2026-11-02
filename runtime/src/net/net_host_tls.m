/* Host provider, ssl: TLS client connections over the guest's own bsd socket, with the host's TLS stack (Security.framework SecureTransport: the one
 * Apple TLS API that runs over a descriptor the caller already connected; Network.framework only opens its own connections).
 *
 * The server is verified against the HOST trust store (the system roots plus whatever the user trusts in Keychain Access) and, in addition, the CA
 * certificates the guest imported for this connection (tls_add_ca). The guest's SetVerifyOption is honoured: without PeerCa the chain is not
 * checked, without HostName the name is not. TLS 1.2 is the minimum. No certificate, key or host name is built in. */
#include "net_host.h"
#include "hz_result.h"
#include <Foundation/Foundation.h>
#include <Security/Security.h>
#include <Security/SecureTransport.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations" /* SecureTransport is deprecated but has no descriptor-based replacement */

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define NH_TLS_HANDSHAKE_MS 30000

typedef struct {
    int h;              /* provider socket handle */
    SSLContextRef ssl;  /* created at the handshake */
    char host[256];     /* SNI and the name to verify; empty = none */
    uint32_t verify;    /* ONLINE_TLS_VERIFY_* */
    CFMutableArrayRef cas; /* guest-imported anchors (SecCertificateRef) */
    int nb_io;          /* 1: the I/O callbacks return errSSLWouldBlock instead of waiting */
    int closed;
} NhTls;

/* I/O over the provider socket. With nb_io the callbacks never wait (tls_read / tls_write wait outside SecureTransport, honouring the guest's blocking
 * mode); otherwise (the handshake) they wait until the bytes moved or the socket closed. */
static OSStatus io_read(SSLConnectionRef c, void *data, size_t *len) {
    NhTls *t = (NhTls *)c;
    size_t want = *len, got = 0;
    NhSock *s = nh_sock_get(t->h);
    *len = 0;
    if (!s) return errSSLClosedAbort;
    OSStatus st = noErr;
    while (got < want) {
        ssize_t r = recv(nh_sock_fd(s), (uint8_t *)data + got, want - got, 0);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (r == 0) {
            st = errSSLClosedGraceful;
            break;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN) {
            st = errSSLClosedAbort;
            break;
        }
        if (t->nb_io) {
            st = errSSLWouldBlock;
            break;
        }
        if (nh_sock_wait(s, POLLIN, NH_TLS_HANDSHAKE_MS) <= 0) {
            st = errSSLClosedAbort;
            break;
        }
    }
    nh_sock_put(s);
    *len = got;
    return st;
}
static OSStatus io_write(SSLConnectionRef c, const void *data, size_t *len) {
    NhTls *t = (NhTls *)c;
    size_t want = *len, put = 0;
    NhSock *s = nh_sock_get(t->h);
    *len = 0;
    if (!s) return errSSLClosedAbort;
    OSStatus st = noErr;
    while (put < want) {
        ssize_t r = send(nh_sock_fd(s), (const uint8_t *)data + put, want - put, 0);
        if (r >= 0) {
            put += (size_t)r;
            continue;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN) {
            st = errSSLClosedAbort;
            break;
        }
        if (t->nb_io) {
            st = errSSLWouldBlock;
            break;
        }
        if (nh_sock_wait(s, POLLOUT, NH_TLS_HANDSHAKE_MS) <= 0) {
            st = errSSLClosedAbort;
            break;
        }
    }
    nh_sock_put(s);
    *len = put;
    return st;
}

void *nh_tls_new(void *ctx, int sock_handle) {
    (void)ctx;
    NhSock *s = nh_sock_get(sock_handle);
    if (!s) return NULL;
    nh_sock_put(s);
    NhTls *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->h = sock_handle;
    t->verify = ONLINE_TLS_VERIFY_PEER_CA | ONLINE_TLS_VERIFY_HOST_NAME | ONLINE_TLS_VERIFY_DATE;
    return t;
}
void nh_tls_free(void *ctx, void *conn) {
    (void)ctx;
    NhTls *t = conn;
    if (!t) return;
    if (t->ssl) {
        if (!t->closed) {
            t->nb_io = 1; /* best-effort close_notify, never waiting */
            SSLClose(t->ssl);
        }
        CFRelease(t->ssl);
    }
    if (t->cas) CFRelease(t->cas);
    free(t);
}
int nh_tls_set_host(void *ctx, void *conn, const char *host) {
    (void)ctx;
    NhTls *t = conn;
    if (!t || !host) return -1;
    strncpy(t->host, host, sizeof t->host - 1);
    t->host[sizeof t->host - 1] = 0;
    return 0;
}
int nh_tls_set_verify(void *ctx, void *conn, uint32_t option) {
    (void)ctx;
    NhTls *t = conn;
    if (!t) return -1;
    t->verify = option;
    return 0;
}

/* A PEM block's base64 body as DER, or NULL. */
static CFDataRef pem_to_der(const uint8_t *p, size_t n) {
    NSString *text = [[NSString alloc] initWithBytes:p length:n encoding:NSASCIIStringEncoding];
    if (!text) return NULL;
    NSRange b = [text rangeOfString:@"-----BEGIN CERTIFICATE-----"], e = [text rangeOfString:@"-----END CERTIFICATE-----"];
    if (b.location == NSNotFound || e.location == NSNotFound || e.location < NSMaxRange(b)) return NULL;
    NSString *body = [text substringWithRange:NSMakeRange(NSMaxRange(b), e.location - NSMaxRange(b))];
    NSData *der = [[NSData alloc] initWithBase64EncodedString:body options:NSDataBase64DecodingIgnoreUnknownCharacters];
    return der ? CFBridgingRetain(der) : NULL;
}
int nh_tls_add_ca(void *ctx, void *conn, const void *cert, size_t n) {
    (void)ctx;
    NhTls *t = conn;
    if (!t || !cert || !n) return -1;
    CFDataRef der = n > 10 && !memcmp(cert, "-----BEGIN", 10) ? pem_to_der(cert, n) : CFDataCreate(NULL, cert, (CFIndex)n);
    if (!der) return -1;
    SecCertificateRef c = SecCertificateCreateWithData(NULL, der);
    CFRelease(der);
    if (!c) return -1;
    if (!t->cas) t->cas = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFArrayAppendValue(t->cas, c);
    CFRelease(c);
    return 0;
}

/* The server's chain against the host store plus the guest's anchors, as the verify option asks. */
static int verify_peer(NhTls *t) {
    if (!(t->verify & (ONLINE_TLS_VERIFY_PEER_CA | ONLINE_TLS_VERIFY_HOST_NAME))) return 0;
    SecTrustRef trust = NULL;
    if (SSLCopyPeerTrust(t->ssl, &trust) != noErr || !trust) return -1;
    CFStringRef name = (t->verify & ONLINE_TLS_VERIFY_HOST_NAME) && t->host[0] ? CFStringCreateWithCString(NULL, t->host, kCFStringEncodingUTF8) : NULL;
    SecPolicyRef pol = SecPolicyCreateSSL(true, name);
    SecTrustSetPolicies(trust, pol);
    CFMutableArrayRef anchors = t->cas ? CFArrayCreateMutableCopy(NULL, 0, t->cas) : CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (!(t->verify & ONLINE_TLS_VERIFY_PEER_CA)) { /* only the name is asked for: the server's own certificate is the anchor, so the chain cannot fail */
        CFArrayRef chain = SecTrustCopyCertificateChain(trust);
        if (chain && CFArrayGetCount(chain)) CFArrayAppendValue(anchors, CFArrayGetValueAtIndex(chain, 0));
        if (chain) CFRelease(chain);
    }
    if (CFArrayGetCount(anchors)) {
        SecTrustSetAnchorCertificates(trust, anchors);
        SecTrustSetAnchorCertificatesOnly(trust, false); /* the host's roots stay trusted too */
    }
    CFRelease(anchors);
    CFErrorRef cerr = NULL;
    bool ok = SecTrustEvaluateWithError(trust, &cerr);
    if (!ok) {
        CFStringRef d = cerr ? CFErrorCopyDescription(cerr) : NULL;
        char msg[256] = "?";
        if (d) CFStringGetCString(d, msg, sizeof msg, kCFStringEncodingUTF8), CFRelease(d);
        log_line("net", "host provider: TLS server for '%s' not trusted: %s", t->host[0] ? t->host : "(no name)", msg);
    }
    if (cerr) CFRelease(cerr);
    CFRelease(pol);
    if (name) CFRelease(name);
    CFRelease(trust);
    return ok ? 0 : -1;
}

int nh_tls_handshake(void *ctx, void *conn, uint32_t *result) {
    (void)ctx;
    NhTls *t = conn;
    *result = HZ_R_SSL_NOT_CONNECTED;
    if (!t) return -1;
    if (!t->ssl) {
        t->ssl = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
        if (!t->ssl) return -1;
        SSLSetIOFuncs(t->ssl, io_read, io_write);
        SSLSetConnection(t->ssl, t);
        SSLSetProtocolVersionMin(t->ssl, kTLSProtocol12);
        SSLSetSessionOption(t->ssl, kSSLSessionOptionBreakOnServerAuth, true); /* verification is ours: host store + guest anchors */
        if (t->host[0]) SSLSetPeerDomainName(t->ssl, t->host, strlen(t->host)); /* SNI */
    }
    t->nb_io = 0;
    int tok;
    nh_core_away(&tok);
    OSStatus st;
    for (;;) {
        st = SSLHandshake(t->ssl);
        if (st == errSSLServerAuthCompleted || st == errSSLPeerAuthCompleted) {
            if (verify_peer(t) != 0) {
                st = errSSLXCertChainInvalid;
                break;
            }
            continue;
        }
        if (st != errSSLWouldBlock) break;
    }
    nh_core_back(&tok);
    if (st != noErr) {
        if (st != errSSLXCertChainInvalid) log_line("net", "host provider: TLS handshake with '%s' failed (OSStatus %d)", t->host[0] ? t->host : "(no name)", (int)st);
        t->closed = 1;
        return -1;
    }
    *result = 0;
    return 0;
}

static int64_t tls_io(NhTls *t, void *rbuf, const void *wbuf, size_t n, int *err) {
    if (!t || !t->ssl || t->closed) {
        *err = HZ_ENOTCONN;
        return -1;
    }
    NhSock *s = nh_sock_get(t->h);
    if (!s) {
        *err = HZ_EBADF;
        return -1;
    }
    int nonblocking = nh_sock_nonblocking(s);
    int64_t r = -1;
    t->nb_io = 1;
    for (;;) {
        size_t done = 0;
        OSStatus st = rbuf ? SSLRead(t->ssl, rbuf, n, &done) : SSLWrite(t->ssl, wbuf, n, &done);
        if (done > 0 || (st == noErr)) {
            r = (int64_t)done;
            break;
        }
        if (st == errSSLClosedGraceful || st == errSSLClosedNoNotify) {
            t->closed = 1;
            r = 0;
            if (!rbuf) r = -1, *err = HZ_EPIPE;
            break;
        }
        if (st != errSSLWouldBlock) {
            t->closed = 1;
            *err = HZ_ECONNRESET;
            break;
        }
        if (nonblocking) {
            *err = HZ_EAGAIN;
            break;
        }
        int w = nh_sock_wait(s, rbuf ? POLLIN : POLLOUT, -1);
        if (w < 0) {
            *err = HZ_EBADF;
            break;
        }
    }
    nh_sock_put(s);
    return r;
}
int64_t nh_tls_read(void *ctx, void *conn, void *buf, size_t n, int *err) {
    (void)ctx;
    return tls_io(conn, buf, NULL, n, err);
}
int64_t nh_tls_write(void *ctx, void *conn, const void *buf, size_t n, int *err) {
    (void)ctx;
    return tls_io(conn, NULL, buf, n, err);
}
int64_t nh_tls_pending(void *ctx, void *conn) {
    (void)ctx;
    NhTls *t = conn;
    size_t n = 0;
    if (t && t->ssl && SSLGetBufferedReadSize(t->ssl, &n) != noErr) n = 0;
    return (int64_t)n;
}
