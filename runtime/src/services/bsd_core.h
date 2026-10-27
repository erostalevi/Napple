/* bsd core: the guest's socket descriptor table over an OnlineProvider, and the guest <-> provider address conversions (portable).
 * Guest descriptors are small integers handed out lowest-first (as the console's bsd:u does); each maps to a provider socket handle.
 * Every call goes through online_provider(): with the NullProvider the answers are the offline ones (offline_core.c). Not thread-safe per
 * descriptor (a guest serialises its own use); the table itself is locked, and blocking provider calls run without the lock. */
#pragma once
#include "offline_core.h"
#include "online_provider.h"
#include <pthread.h>

typedef struct {
    pthread_mutex_t lock;
    int init;
    int h[BSD_MAX_FDS];      /* provider handle, or -1 when the descriptor is free */
    uint8_t nb[BSD_MAX_FDS]; /* O_NONBLOCK as set through Fcntl / Ioctl(FIONBIO) */
} BsdGuest;

void bsdg_reset(BsdGuest *g); /* closes every socket through the provider and empties the table */
BsdGuest *bsdg_global(void);
int bsdg_handle(BsdGuest *g, int fd); /* provider handle of a guest descriptor, or -1 */

/* Each returns the result of the call (descriptor, count or 0) or -1 with *err set to a BSD errno. */
int bsdg_socket(BsdGuest *g, int domain, int type, int protocol, int *err);
int bsdg_close(BsdGuest *g, int fd, int *err);
/* DuplicateSocket: a second descriptor for the same provider socket (lowest free number). The provider socket is closed with the last descriptor. */
int bsdg_dup(BsdGuest *g, int fd, int *err);
int bsdg_bind(BsdGuest *g, int fd, const OnlineAddr *a, int *err);
int bsdg_listen(BsdGuest *g, int fd, int backlog, int *err);
int bsdg_accept(BsdGuest *g, int fd, OnlineAddr *peer, int *err);
int bsdg_connect(BsdGuest *g, int fd, const OnlineAddr *a, int *err);
int64_t bsdg_send(BsdGuest *g, int fd, const void *buf, size_t n, int flags, const OnlineAddr *to, int *err);
int64_t bsdg_recv(BsdGuest *g, int fd, void *buf, size_t n, int flags, OnlineAddr *from, int *err);
int bsdg_shutdown(BsdGuest *g, int fd, int how, int *err);
int bsdg_getsockname(BsdGuest *g, int fd, OnlineAddr *out, int *err);
int bsdg_getpeername(BsdGuest *g, int fd, OnlineAddr *out, int *err);
int bsdg_setsockopt(BsdGuest *g, int fd, int level, int opt, const void *v, size_t n, int *err);
int bsdg_getsockopt(BsdGuest *g, int fd, int level, int opt, void *v, size_t *n, int *err);
int bsdg_nonblock(BsdGuest *g, int fd, int on, int *err);
/* poll over guest descriptors (revents are written back into fds[]); returns the ready count. */
int bsdg_poll(BsdGuest *g, OnlinePollFd *fds, unsigned n, int timeout_ms, int *err);
/* select over fd_set bitmaps (byte i/8, bit i%8; any set may be NULL). Sets are rewritten with the ready descriptors; returns the ready count. */
int bsdg_select(BsdGuest *g, int nfds, uint8_t *rd, uint8_t *wr, uint8_t *ex, int timeout_ms, int *err);

/* sockaddr in the guest's FreeBSD layout: u8 length, u8 family, u16 port (big endian), then the address (sin: 4 bytes + 8 zero, 16 bytes in all;
 * sin6: u32 flow, 16 bytes, u32 scope, 28 in all). decode returns 0, or -1 when the buffer is too short or the family is unsupported. */
int bsd_sockaddr_decode(const uint8_t *p, size_t n, OnlineAddr *out);
/* Writes the guest layout into out (capacity cap) and returns the address length the call reports (16 or 28); bytes past cap are cut. */
size_t bsd_sockaddr_encode(const OnlineAddr *a, uint8_t *out, size_t cap);
