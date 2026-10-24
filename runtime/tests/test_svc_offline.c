/* Unit tests: offline network decisions (bsd errno per call, nifm request lifecycle, friend queue, ssl handshake) and their IPC tables. */
#include "../src/services/offline.c"
#include "svc_test.h"

static int32_t bsd_ret(IpcCall *c, int i) { return SVC_OUT(c, int32_t, i * 4); }
static void bcall(IpcObject *o, unsigned cmd, int32_t a, int32_t b, int32_t d, IpcCall *c) { int32_t in[3] = {a, b, d}; svc_call(o, cmd, in, sizeof in, c); }

int main(void) {
    BsdTable t; bsd_init(&t); int err;
    CHECK(bsd_socket(&t, BSD_AF_INET, BSD_SOCK_STREAM, 0, &err) == 0 && err == 0, "first socket gets fd 0");
    CHECK(bsd_socket(&t, BSD_AF_INET, BSD_SOCK_DGRAM, BSD_IPPROTO_UDP, &err) == 1, "UDP datagram socket");
    CHECK(bsd_socket(&t, BSD_AF_INET6, BSD_SOCK_STREAM, 0, &err) == -1 && err == HZ_EAFNOSUPPORT, "IPv6 unsupported");
    CHECK(bsd_socket(&t, BSD_AF_INET, 9, 0, &err) == -1 && err == HZ_EPROTOTYPE, "unknown type");
    CHECK(bsd_socket(&t, BSD_AF_INET, BSD_SOCK_STREAM, BSD_IPPROTO_UDP, &err) == -1 && err == HZ_EPROTONOSUPPORT, "TCP stream with the UDP protocol");
    CHECK(bsd_close(&t, 0, &err) == 0 && bsd_close(&t, 0, &err) == -1 && err == HZ_EBADF, "close once, then EBADF");
    CHECK(bsd_socket(&t, BSD_AF_INET, BSD_SOCK_STREAM, 0, &err) == 0, "lowest free fd is reused");
    CHECK(bsd_op(&t, BSD_OP_CONNECT, 0, &err) == -1 && err == HZ_ENETDOWN, "connect: network is down");
    CHECK(bsd_op(&t, BSD_OP_CONNECT, 77, &err) == -1 && err == HZ_EBADF, "unknown fd: EBADF");
    CHECK(bsd_op(&t, BSD_OP_RECV, 0, &err) == -1 && err == HZ_ENOTCONN && bsd_op(&t, BSD_OP_RECV, 1, &err) == -1 && err == HZ_EAGAIN, "recv: ENOTCONN on stream, EAGAIN on datagram");
    CHECK(bsd_op(&t, BSD_OP_SEND, 1, &err) == -1 && err == HZ_EDESTADDRREQ && bsd_op(&t, BSD_OP_SENDTO, 1, &err) == -1 && err == HZ_ENETDOWN, "datagram send needs a destination; sendto: network down");
    CHECK(bsd_op(&t, BSD_OP_LISTEN, 0, &err) == -1 && err == HZ_EINVAL, "listen before bind");
    CHECK(bsd_op(&t, BSD_OP_BIND, 0, &err) == 0 && bsd_op(&t, BSD_OP_BIND, 0, &err) == -1 && err == HZ_EINVAL, "bind works once");
    CHECK(bsd_op(&t, BSD_OP_LISTEN, 0, &err) == 0 && bsd_op(&t, BSD_OP_ACCEPT, 0, &err) == -1 && err == HZ_EAGAIN, "listening stream: accept would block, nobody connects");
    CHECK(bsd_op(&t, BSD_OP_ACCEPT, 1, &err) == -1 && err == HZ_EOPNOTSUPP, "accept on a datagram socket");
    CHECK(bsd_op(&t, BSD_OP_GETPEERNAME, 0, &err) == -1 && err == HZ_ENOTCONN && bsd_op(&t, BSD_OP_SETSOCKOPT, 0, &err) == 0 && bsd_op(&t, BSD_OP_GETSOCKNAME, 0, &err) == 0, "peer name, sockopt, sockname");
    BsdTable full; bsd_init(&full); for (int i = 0; i < BSD_MAX_FDS; i++) bsd_socket(&full, BSD_AF_INET, BSD_SOCK_DGRAM, 0, &err);
    CHECK(bsd_socket(&full, BSD_AF_INET, BSD_SOCK_DGRAM, 0, &err) == -1 && err == HZ_EMFILE, "descriptor table full: EMFILE");

    NifmRequest r; nifm_request_init(&r);
    CHECK(r.state == NIFM_STATE_FREE && nifm_request_result(&r) == 0, "new request: free, no result yet");
    nifm_request_submit(&r); CHECK(r.state == NIFM_STATE_FREE && nifm_request_result(&r) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED && HZ_MODULE(nifm_request_result(&r)) == HZ_MOD_NIFM, "submit is rejected at once");
    nifm_request_cancel(&r); CHECK(nifm_request_result(&r) == 0 && !r.submitted, "cancel clears the result");
    CHECK(HZ_MODULE(friend_pop_notification_result()) == HZ_MOD_FRIENDS && friend_pop_notification_result() != 0, "friend pop result");

    /* bsd:u IPC */
    IpcObject *bs = bsd_open(); IpcCall c; bsd_ready = 0;
    bcall(bs, 2, BSD_AF_INET, BSD_SOCK_STREAM, 0, &c); CHECK(c.out_size == 8 && bsd_ret(&c, 0) == 0 && bsd_ret(&c, 1) == 0, "Socket -> {0, 0}");
    int32_t fd = 0; svc_call(bs, 14, &fd, 4, &c); CHECK(bsd_ret(&c, 0) == -1 && bsd_ret(&c, 1) == HZ_ENETDOWN, "Connect -> {-1, ENETDOWN}");
    svc_call(bs, 8, &fd, 4, &c); CHECK(bsd_ret(&c, 1) == HZ_ENOTCONN, "Recv on an unconnected stream -> ENOTCONN");
    svc_call(bs, 26, &fd, 4, &c); CHECK(bsd_ret(&c, 0) == 0, "Close");
    svc_call(bs, 26, &fd, 4, &c); CHECK(bsd_ret(&c, 0) == -1 && bsd_ret(&c, 1) == HZ_EBADF, "Close again -> EBADF");
    bcall(bs, 2, BSD_AF_INET6, BSD_SOCK_STREAM, 0, &c); CHECK(bsd_ret(&c, 1) == HZ_EAFNOSUPPORT, "Socket(AF_INET6) -> EAFNOSUPPORT");
    svc_call(bs, 2, NULL, 0, &c); CHECK(bsd_ret(&c, 0) == -1 && bsd_ret(&c, 1) == HZ_EINVAL, "short Socket request -> EINVAL, not fatal");
    svc_call(bs, 14, NULL, 0, &c); CHECK(bsd_ret(&c, 1) == HZ_EBADF, "short Connect request -> EBADF");
    bcall(bs, 2, BSD_AF_INET, BSD_SOCK_DGRAM, 0, &c); svc_call(bs, 23, NULL, 0, &c); fd = 0; svc_call(bs, 26, &fd, 4, &c); CHECK(bsd_ret(&c, 1) == HZ_EBADF, "ShutdownAllSockets closes everything");
    svc_call(bs, 7, NULL, 0, &c); CHECK(bsd_ret(&c, 1) == HZ_ENOENT, "Sysctl -> ENOENT");

    /* nifm IPC */
    IpcObject *nf = nifm_open(); svc_call(nf, 5, NULL, 0, &c); IpcObject *gen = c.out_objects[0];
    svc_call(gen, 4, NULL, 0, &c); IpcObject *rq = c.out_objects[0];
    svc_call(rq, 1, NULL, 0, &c); CHECK(c.result == 0, "GetResult before Submit: success");
    svc_call(rq, 2, NULL, 0, &c); CHECK(c.nout_copy == 2 && !event_is_signaled(c.out_copy[0]), "two events, clear before Submit");
    uint32_t ev0 = c.out_copy[0];
    svc_call(rq, 4, NULL, 0, &c); CHECK(c.result == 0 && event_is_signaled(ev0), "Submit signals the request event");
    svc_call(rq, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == NIFM_STATE_FREE, "GetRequestState after rejection: Free");
    svc_call(rq, 1, NULL, 0, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "GetResult after Submit: communication disabled");
    svc_call(rq, 3, NULL, 0, &c); svc_call(rq, 1, NULL, 0, &c); CHECK(c.result == 0, "Cancel clears the result");
    svc_call(gen, 15, NULL, 0, &c); CHECK(c.out_size == 4 && c.out[3] == 0, "internet connection status: disconnected");
    svc_call(gen, 4, NULL, 0, &c); IpcObject *rq2 = c.out_objects[0]; svc_call(rq2, 1, NULL, 0, &c); CHECK(c.result == 0 && rq2->state != rq->state, "requests are independent");

    /* friend + ssl */
    IpcObject *fr = friend_open(); svc_call(fr, 1, NULL, 0, &c); IpcObject *notif = c.out_objects[0];
    svc_call(notif, 2, NULL, 0, &c); CHECK(c.result == friend_pop_notification_result() && c.out_size == 0, "Pop on an empty queue fails and returns no record");
    IpcObject *ss = ssl_open(); svc_call(ss, 0, NULL, 0, &c); IpcObject *ctx = c.out_objects[0];
    svc_call(ctx, 2, NULL, 0, &c); IpcObject *conn = c.out_objects[0];
    svc_call(conn, 8, NULL, 0, &c); CHECK(c.result == HZ_R_SSL_NOT_CONNECTED, "DoHandshake cannot succeed");
    printf("{\"test\":\"svc_offline\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
