#include "audin_core.h"
#include <string.h>

void audin_negotiate(uint32_t req_rate, uint32_t req_channels, uint32_t *rate, uint32_t *channels) {
    static const uint32_t rates[] = {16000, 32000, 48000};
    uint32_t best = 48000;
    if (req_rate) { uint32_t bd = ~0u; for (unsigned i = 0; i < 3; i++) { uint32_t d = rates[i] > req_rate ? rates[i] - req_rate : req_rate - rates[i]; if (d < bd) { bd = d; best = rates[i]; } } }
    *rate = best; *channels = (req_channels == 1) ? 1 : 2;
}
void audin_init(AudinQueue *q) { memset(q, 0, sizeof *q); q->started = 0; }
static void release_queued(AudinQueue *q) {
    for (unsigned i = 0; i < q->nqueued && q->nreleased < AUDIN_QUEUE_MAX; i++) q->released[q->nreleased++] = q->queued[i];
    q->nqueued = 0;
}
void audin_start(AudinQueue *q) { q->started = 1; release_queued(q); }
void audin_stop(AudinQueue *q) { q->started = 0; }
int audin_append(AudinQueue *q, uint64_t tag) {
    if (q->nqueued + q->nreleased >= AUDIN_QUEUE_MAX) return -1;
    if (q->started) q->released[q->nreleased++] = tag; else q->queued[q->nqueued++] = tag;
    return 0;
}
unsigned audin_release(AudinQueue *q, uint64_t *out, unsigned max) {
    unsigned n = q->nreleased < max ? q->nreleased : max;
    memcpy(out, q->released, n * sizeof *out);
    memmove(q->released, q->released + n, (q->nreleased - n) * sizeof *out);
    q->nreleased -= n; return n;
}
int audin_contains(const AudinQueue *q, uint64_t tag) {
    for (unsigned i = 0; i < q->nqueued; i++) if (q->queued[i] == tag) return 1;
    for (unsigned i = 0; i < q->nreleased; i++) if (q->released[i] == tag) return 1;
    return 0;
}
unsigned audin_count(const AudinQueue *q) { return q->nqueued + q->nreleased; }
int audin_flush(AudinQueue *q) { int had = q->nqueued > 0; release_queued(q); return had; }
