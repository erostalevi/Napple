/* offline core: the decisions the offline network services make (portable): which errno a BSD call reports on a console with no network,
 * the nifm request lifecycle, and the empty friend notification queue. Numbering is FreeBSD's (hz_result.h), which is what Horizon uses. */
#pragma once
#include <stdint.h>

/* ---- bsd ------------------------------------------------------------------------------------------------------------------ */
enum { BSD_AF_UNIX = 1, BSD_AF_INET = 2, BSD_AF_INET6 = 28 };
enum { BSD_SOCK_STREAM = 1, BSD_SOCK_DGRAM = 2, BSD_SOCK_RAW = 3 };
enum { BSD_IPPROTO_TCP = 6, BSD_IPPROTO_UDP = 17 };
#define BSD_MAX_FDS 128
typedef struct { int used, domain, type, protocol, bound, listening, connected; } BsdSocket;
typedef struct { BsdSocket fds[BSD_MAX_FDS]; } BsdTable;
typedef enum {
    BSD_OP_BIND, BSD_OP_LISTEN, BSD_OP_CONNECT, BSD_OP_ACCEPT, BSD_OP_RECV, BSD_OP_RECVFROM, BSD_OP_SEND, BSD_OP_SENDTO, BSD_OP_GETPEERNAME,
    BSD_OP_GETSOCKNAME, BSD_OP_GETSOCKOPT, BSD_OP_SETSOCKOPT, BSD_OP_SHUTDOWN, BSD_OP_IOCTL, BSD_OP_FCNTL, BSD_OP_DUP
} BsdOp;

void bsd_init(BsdTable *t);
/* Each returns the file descriptor / 0 on success or -1 with *err set to a BSD errno. */
int bsd_socket(BsdTable *t, int domain, int type, int protocol, int *err);
int bsd_close(BsdTable *t, int fd, int *err);
int bsd_op(BsdTable *t, BsdOp op, int fd, int *err);
const BsdSocket *bsd_lookup(const BsdTable *t, int fd);

/* ---- nifm ----------------------------------------------------------------------------------------------------------------- */
enum { NIFM_STATE_FREE = 1, NIFM_STATE_ONHOLD = 2, NIFM_STATE_ACCEPTED = 3 };
typedef struct { int state; int submitted; uint32_t result; } NifmRequest;
void nifm_request_init(NifmRequest *r);
void nifm_request_submit(NifmRequest *r);        /* no network: the request finishes at once, rejected */
void nifm_request_cancel(NifmRequest *r);        /* back to the unsubmitted state with a clean result */
uint32_t nifm_request_result(const NifmRequest *r);

/* ---- friends -------------------------------------------------------------------------------------------------------------- */
uint32_t friend_pop_notification_result(void);   /* result of INotificationService::Pop with an empty queue */
