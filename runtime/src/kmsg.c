/* HIPC message transfer between guest message buffers (see kmsg.h). */
#include "kmsg.h"
#include "napple.h"
#include <string.h>

uint32_t kmsg_layout(const uint32_t *m, uint32_t cap, KMsgLayout *o) {
    memset(o, 0, sizeof *o);
    if (cap < 8) return R_MESSAGE_TOO_LARGE;
    o->type = m[0] & 0xffff;
    o->nx = (m[0] >> 16) & 0xf;
    o->na = (m[0] >> 20) & 0xf;
    o->nb = (m[0] >> 24) & 0xf;
    o->nw = (m[0] >> 28) & 0xf;
    o->ndata = m[1] & 0x3ff;
    o->recv_mode = (m[1] >> 10) & 0xf;
    o->special = (int)(m[1] >> 31);
    o->nrecv = o->recv_mode == 2 ? 1 : o->recv_mode > 2 ? o->recv_mode - 2 : 0;
    unsigned w = 2;
    if (o->special) {
        if (cap < 12) return R_MESSAGE_TOO_LARGE;
        uint32_t h = m[2];
        w = 3;
        o->has_pid = h & 1;
        o->ncopy = (h >> 1) & 0xf;
        o->nmove = (h >> 5) & 0xf;
        o->pid_word = w;
        if (o->has_pid) w += 2;
        o->handle_word = w;
        w += o->ncopy + o->nmove;
    }
    o->x_word = w;
    w += o->nx * 2;
    o->abw_word = w;
    w += (o->na + o->nb + o->nw) * 3;
    o->data_word = w;
    w += o->ndata;
    o->recv_word = w;
    w += o->nrecv * 2;
    o->total_words = w;
    return (uint64_t)w * 4 > cap ? R_MESSAGE_TOO_LARGE : 0;
}

void kmsg_recv_list(const uint32_t *m, uint32_t cap, KRecvList *out) {
    memset(out, 0, sizeof *out);
    KMsgLayout l;
    if (kmsg_layout(m, cap, &l)) return;
    out->mode = l.recv_mode;
    out->n = l.nrecv < KMSG_MAX_RECV ? l.nrecv : KMSG_MAX_RECV;
    for (unsigned i = 0; i < out->n; i++) {
        const uint32_t *d = m + l.recv_word + 2 * i;
        out->addr[i] = d[0] | ((uint64_t)(d[1] & 0xffff) << 32);
        out->size[i] = d[1] >> 16;
    }
}

static uint64_t x_addr(const uint32_t *d) { return d[1] | ((uint64_t)((d[0] >> 12) & 0xf) << 32) | ((uint64_t)((d[0] >> 6) & 0x3f) << 36); }
static void x_set_addr(uint32_t *d, uint64_t a) {
    d[0] = (d[0] & ~((0x3fu << 6) | (0xfu << 12))) | (uint32_t)(((a >> 36) & 0x3f) << 6) | (uint32_t)(((a >> 32) & 0xf) << 12);
    d[1] = (uint32_t)a;
}

uint32_t kmsg_copy(uint32_t *dst, uint32_t dst_cap, const KRecvList *recv, const uint32_t *src, uint32_t src_cap, uint64_t pid, KMsgPtr ptr) {
    KMsgLayout l;
    uint32_t r = kmsg_layout(src, src_cap, &l);
    if (r) return r;
    if ((uint64_t)l.total_words * 4 > dst_cap) return R_MESSAGE_TOO_LARGE;
    /* pointer data first, so a broken receive list leaves the receiver's message buffer untouched */
    uint32_t xd[16][2];
    uint64_t packed = 0;
    for (unsigned i = 0; i < l.nx; i++) {
        uint32_t *d = xd[i];
        memcpy(d, src + l.x_word + 2 * i, 8);
        uint64_t a = x_addr(d), size = d[0] >> 16;
        unsigned index = d[0] & 0x3f;
        if (recv->mode < 2 || !size) continue; /* no receive list (or inline): the receiver reads the sender's memory */
        uint64_t to, cap;
        if (recv->mode == 2) {
            packed = (packed + 15) & ~15ull;
            to = recv->addr[0] + packed;
            cap = recv->size[0] >= packed ? recv->size[0] - packed : 0;
            packed += size;
        } else {
            if (index >= recv->n) return R_RECEIVE_LIST_BROKEN;
            to = recv->addr[index];
            cap = recv->size[index];
        }
        if (size > cap) return R_RECEIVE_LIST_BROKEN;
        const void *from = ptr(a, size);
        void *into = ptr(to, size);
        if (!from || !into) return R_INVALID_POINTER;
        memmove(into, from, size);
        x_set_addr(d, to);
    }
    memmove(dst, src, (size_t)l.total_words * 4);
    for (unsigned i = 0; i < l.nx; i++) memcpy(dst + l.x_word + 2 * i, xd[i], 8);
    if (l.has_pid) {
        dst[l.pid_word] = (uint32_t)pid;
        dst[l.pid_word + 1] = (uint32_t)(pid >> 32);
    }
    return 0;
}
