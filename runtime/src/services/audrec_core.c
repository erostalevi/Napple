#include "audrec_core.h"
#include <string.h>

void audrec_init(AudrecQueue *q) { memset(q, 0, sizeof *q); }
static void complete(AudrecQueue *q, AudrecBuf b) {
    q->frames += b.size / AUDREC_FRAME_BYTES; b.end_time = q->frames; q->released[q->nreleased++] = b;
}
void audrec_start(AudrecQueue *q) {
    q->started = 1;
    for (unsigned i = 0; i < q->nqueued && q->nreleased < AUDREC_QUEUE_MAX; i++) complete(q, q->queued[i]);
    q->nqueued = 0;
}
void audrec_stop(AudrecQueue *q) { q->started = 0; }
int audrec_append(AudrecQueue *q, uint64_t tag, uint64_t size_bytes) {
    if (q->nqueued + q->nreleased >= AUDREC_QUEUE_MAX) return -1;
    AudrecBuf b = {tag, 0, size_bytes};
    if (q->started) complete(q, b); else q->queued[q->nqueued++] = b;
    return 0;
}
unsigned audrec_release(AudrecQueue *q, uint64_t *tags, unsigned max, uint64_t *last_end_time) {
    unsigned n = q->nreleased < max ? q->nreleased : max;
    for (unsigned i = 0; i < n; i++) tags[i] = q->released[i].tag;
    if (last_end_time) *last_end_time = n ? q->released[n - 1].end_time : 0;
    memmove(q->released, q->released + n, (q->nreleased - n) * sizeof *q->released);
    q->nreleased -= n; return n;
}
int audrec_contains(const AudrecQueue *q, uint64_t tag) { uint64_t e; return audrec_end_time(q, tag, &e); }
int audrec_end_time(const AudrecQueue *q, uint64_t tag, uint64_t *end) {
    for (unsigned i = 0; i < q->nqueued; i++) if (q->queued[i].tag == tag) { *end = 0; return 1; }
    for (unsigned i = 0; i < q->nreleased; i++) if (q->released[i].tag == tag) { *end = q->released[i].end_time; return 1; }
    return 0;
}
unsigned audrec_count(const AudrecQueue *q) { return q->nqueued + q->nreleased; }
