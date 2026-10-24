/* HIPC message transfer between two guest message buffers of the same process (guest-to-guest sessions, ksession.c). Pure code: no kernel
 * state, guest memory is reached through a callback, so runtime/tests/host can test it on any host.
 *
 * What the kernel does for a message and what this does:
 *   header, raw data, A/B/W buffer descriptors, receive list  copied verbatim (one address space: no remapping needed)
 *   process id (special header flag)                         written with the sender's pid
 *   copy / move handles                                       passed through unchanged (one handle table: the receiver sees the sender's
 *                                                             handle values; a copy handle is shared, not duplicated)
 *   X (pointer) descriptors                                   data copied into the receiver's receive list (C) buffers and the descriptor
 *                                                             rewritten to point there; receive list mode 0/1 keeps the sender's address */
#pragma once
#include <stdint.h>

#define KMSG_MAX_RECV 13
typedef struct { unsigned mode, n; uint64_t addr[KMSG_MAX_RECV], size[KMSG_MAX_RECV]; } KRecvList;
typedef struct {
    unsigned type, nx, na, nb, nw, ndata, recv_mode, nrecv;
    int special, has_pid; unsigned ncopy, nmove;
    unsigned pid_word, handle_word, x_word, abw_word, data_word, recv_word, total_words;
} KMsgLayout;
/* Validated host pointer for guest [addr, addr+size), or NULL. */
typedef void *(*KMsgPtr)(uint64_t addr, uint64_t size);

/* Parses the header words; 0 or R_MESSAGE_TOO_LARGE when the message does not fit `cap_bytes`. */
uint32_t kmsg_layout(const uint32_t *msg, uint32_t cap_bytes, KMsgLayout *out);
/* The receive list a receiver prepared in its own buffer (read before the incoming message overwrites it). */
void kmsg_recv_list(const uint32_t *msg, uint32_t cap_bytes, KRecvList *out);
/* Copies the message in `src` into `dst` for a receiver whose receive list is `dst_recv`. Returns 0, R_MESSAGE_TOO_LARGE,
 * R_RECEIVE_LIST_BROKEN (X data does not fit the receiver's buffers) or R_INVALID_POINTER (a buffer is not guest memory). */
uint32_t kmsg_copy(uint32_t *dst, uint32_t dst_cap, const KRecvList *dst_recv, const uint32_t *src, uint32_t src_cap, uint64_t pid, KMsgPtr ptr);
