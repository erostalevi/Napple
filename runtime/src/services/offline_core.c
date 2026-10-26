#include "offline_core.h"
#include "hz_result.h"
#include <string.h>

void bsd_init(BsdTable *t) { memset(t, 0, sizeof *t); }
const BsdSocket *bsd_lookup(const BsdTable *t, int fd) { return fd >= 0 && fd < BSD_MAX_FDS && t->fds[fd].used ? &t->fds[fd] : NULL; }
static BsdSocket *slot(BsdTable *t, int fd) { return fd >= 0 && fd < BSD_MAX_FDS && t->fds[fd].used ? &t->fds[fd] : NULL; }

int bsd_socket(BsdTable *t, int domain, int type, int protocol, int *err) {
    if (domain != BSD_AF_INET) {
        *err = HZ_EAFNOSUPPORT;
        return -1;
    }
    if (type != BSD_SOCK_STREAM && type != BSD_SOCK_DGRAM && type != BSD_SOCK_RAW) {
        *err = HZ_EPROTOTYPE;
        return -1;
    }
    if (protocol &&
        !((type == BSD_SOCK_STREAM && protocol == BSD_IPPROTO_TCP) || (type == BSD_SOCK_DGRAM && protocol == BSD_IPPROTO_UDP) || type == BSD_SOCK_RAW)) {
        *err = HZ_EPROTONOSUPPORT;
        return -1;
    }
    for (int fd = 0; fd < BSD_MAX_FDS; fd++)
        if (!t->fds[fd].used) {
            t->fds[fd] = (BsdSocket){1, domain, type, protocol, 0, 0, 0};
            *err = 0;
            return fd;
        }
    *err = HZ_EMFILE;
    return -1;
}
int bsd_close(BsdTable *t, int fd, int *err) {
    BsdSocket *s = slot(t, fd);
    if (!s) {
        *err = HZ_EBADF;
        return -1;
    }
    memset(s, 0, sizeof *s);
    *err = 0;
    return 0;
}

/* Without a network interface: sockets can be created, configured and bound (all local), but nothing reaches a peer. */
int bsd_op(BsdTable *t, BsdOp op, int fd, int *err) {
    BsdSocket *s = slot(t, fd);
    *err = 0;
    if (!s) {
        *err = HZ_EBADF;
        return -1;
    }
    int stream = s->type == BSD_SOCK_STREAM;
    switch (op) {
        case BSD_OP_BIND:
            if (s->bound) {
                *err = HZ_EINVAL;
                return -1;
            }
            s->bound = 1;
            return 0;
        case BSD_OP_LISTEN:
            if (!stream) {
                *err = HZ_EOPNOTSUPP;
                return -1;
            }
            if (!s->bound) {
                *err = HZ_EINVAL;
                return -1;
            }
            s->listening = 1;
            return 0;
        case BSD_OP_CONNECT: *err = HZ_ENETDOWN; return -1;
        case BSD_OP_ACCEPT:
            if (!stream) {
                *err = HZ_EOPNOTSUPP;
                return -1;
            }
            if (!s->listening) {
                *err = HZ_EINVAL;
                return -1;
            }
            *err = HZ_EAGAIN;
            return -1; /* nobody ever connects */
        case BSD_OP_RECV:
        case BSD_OP_RECVFROM: *err = stream ? HZ_ENOTCONN : HZ_EAGAIN; return -1;
        case BSD_OP_SEND: *err = stream ? HZ_ENOTCONN : HZ_EDESTADDRREQ; return -1;
        case BSD_OP_SENDTO: *err = stream ? HZ_EISCONN : HZ_ENETDOWN; return -1;
        case BSD_OP_GETPEERNAME: *err = HZ_ENOTCONN; return -1;
        case BSD_OP_SHUTDOWN:
            if (stream) {
                *err = HZ_ENOTCONN;
                return -1;
            }
            return 0;
        case BSD_OP_GETSOCKNAME:
        case BSD_OP_GETSOCKOPT:
        case BSD_OP_SETSOCKOPT:
        case BSD_OP_IOCTL:
        case BSD_OP_FCNTL: return 0;
        case BSD_OP_DUP: *err = HZ_EOPNOTSUPP; return -1;
    }
    *err = HZ_EINVAL;
    return -1;
}

void nifm_request_init(NifmRequest *r) {
    r->state = NIFM_STATE_FREE;
    r->submitted = 0;
    r->result = 0;
}
void nifm_request_submit(NifmRequest *r) {
    r->submitted = 1;
    r->state = NIFM_STATE_FREE;
    r->result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
}
void nifm_request_cancel(NifmRequest *r) { nifm_request_init(r); }
uint32_t nifm_request_result(const NifmRequest *r) { return r->result; }

uint32_t friend_pop_notification_result(void) { return HZ_RESULT(HZ_MOD_FRIENDS, 15); } /* [unverified] "notification queue empty" */
