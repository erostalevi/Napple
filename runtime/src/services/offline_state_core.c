#include "offline_state_core.h"
#include <string.h>

void mm_init(MmTable *t) { memset(t, 0, sizeof *t); }
uint32_t mm_request_open(MmTable *t, uint32_t module) {
    for (unsigned i = 0; i < MM_MAX_REQUESTS; i++)
        if (!t->req[i].used) {
            memset(&t->req[i], 0, sizeof t->req[i]);
            t->req[i].used = 1;
            t->req[i].module = module;
            return i + 1;
        }
    return 0;
}
void mm_request_close(MmTable *t, uint32_t id) {
    if (id >= 1 && id <= MM_MAX_REQUESTS) t->req[id - 1].used = 0;
}
void mm_request_set(MmTable *t, uint32_t id, uint32_t min, int32_t max) {
    if (id < 1 || id > MM_MAX_REQUESTS || !t->req[id - 1].used) return;
    t->req[id - 1].min = min;
    t->req[id - 1].max = max;
}
uint32_t mm_request_get(const MmTable *t, uint32_t id) { return id >= 1 && id <= MM_MAX_REQUESTS && t->req[id - 1].used ? t->req[id - 1].min : 0; }
void mm_module_open(MmTable *t, uint32_t m) {
    if (m < MM_MAX_MODULES) memset(&t->mod[m], 0, sizeof t->mod[m]), t->mod[m].used = 1;
}
void mm_module_close(MmTable *t, uint32_t m) {
    if (m < MM_MAX_MODULES) t->mod[m].used = 0;
}
void mm_module_set(MmTable *t, uint32_t m, uint32_t min, int32_t max) {
    if (m >= MM_MAX_MODULES || !t->mod[m].used) return;
    t->mod[m].min = min;
    t->mod[m].max = max;
}
uint32_t mm_module_get(const MmTable *t, uint32_t m) { return m < MM_MAX_MODULES && t->mod[m].used ? t->mod[m].min : 0; }

void prepo_init(PrepoStore *p) {
    memset(p, 0, sizeof *p);
    p->agreement_check = 1;
}
void prepo_account(PrepoStore *p, uint64_t n) {
    if (p->bytes + n > PREPO_STORAGE_LIMIT) return;
    p->bytes += n;
    p->count++;
}
void prepo_clear(PrepoStore *p) {
    p->bytes = 0;
    p->count = 0;
}
