/* nvdrv_nodes_core: see nvdrv_nodes_core.h. Pure state machines over the request buffers; no host calls. */
#include "nvdrv_nodes_core.h"
#include <string.h>

const uint32_t nvgpu_classes[6] = {0x902d, 0xb197, 0xb1c0, 0xb06f, 0xa140, 0xb0b5};

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

void nvgpu_chan_init(NvGpuChan *c, uint32_t fence_id) {
    memset(c, 0, sizeof *c);
    c->nvmap_fd = -1;
    c->priority = 100; /* medium */
    c->fence_id = fence_id;
    c->next_obj_id = 1;
}
void nvgpu_as_init(NvGpuAs *a) { memset(a, 0, sizeof *a); }
void nvgpu_ctrl_init(NvGpuCtrl *c, uint64_t now_ns) {
    memset(c, 0, sizeof *c);
    c->t0_ns = now_ns;
}
int nvgpu_as_bind(NvGpuAs *a, NvGpuChan *c) {
    if (!a || !c || c->as) return NVG_BAD_PARAMETER;
    c->as = a;
    a->bound++;
    return NVG_OK;
}
void nvgpu_as_unbind(NvGpuChan *c) {
    if (c->as && c->as->bound) c->as->bound--;
    c->as = NULL;
}

/* ---- nvhost-gpu ---------------------------------------------------------------------------------------------------------------------- */
enum { SUBMIT_FENCE_GET = 2 };
int nvgpu_chan_ioctl(NvGpuChan *c, const NvGpuEnv *env, unsigned nr, uint8_t *d, unsigned size) {
    switch (nr) {
        case 0x01: /* SET_NVMAP_FD */
            if (size < 4) return NVG_BAD_PARAMETER;
            c->nvmap_fd = (int32_t)rd32(d);
            return NVG_OK;
        case 0x03: /* SET_TIMEOUT */
            if (size < 4) return NVG_BAD_PARAMETER;
            c->timeout_ms = rd32(d);
            return NVG_OK;
        case 0x0d: { /* SET_PRIORITY: low 50, medium 100, high 150 */
            if (size < 4) return NVG_BAD_PARAMETER;
            uint32_t p = rd32(d);
            if (p != 50 && p != 100 && p != 150) return NVG_BAD_PARAMETER;
            c->priority = p;
            return NVG_OK;
        }
        case 0x14: /* SET_USER_DATA */
            if (size < 8) return NVG_BAD_PARAMETER;
            c->user_data = rd64(d);
            return NVG_OK;
        case 0x15: /* GET_USER_DATA */
            if (size < 8) return NVG_BAD_PARAMETER;
            wr64(d, c->user_data);
            return NVG_OK;
        case 0x0c: /* SET_ERROR_NOTIFIER */
            if (size < 0x18) return NVG_BAD_PARAMETER;
            c->err_off = rd64(d);
            c->err_size = rd64(d + 8);
            c->err_mem = rd32(d + 16);
            return NVG_OK;
        case 0x1a: { /* ALLOC_GPFIFO_EX2: needs a bound address space, once per channel */
            if (size < 0x20) return NVG_BAD_PARAMETER;
            uint32_t n = rd32(d);
            if (!c->as) return NVG_INVALID_STATE;
            if (c->gpfifo_entries) return NVG_ALREADY_ALLOCATED;
            if (!n || n > 0x40000) return NVG_BAD_PARAMETER;
            c->gpfifo_entries = n;
            c->gpfifo_flags = rd32(d + 4);
            wr32(d + 12, c->fence_id);
            wr32(d + 16, c->fence_value);
            return NVG_OK;
        }
        case 0x09: { /* ALLOC_OBJ_CTX: a supported engine class, once per class, after the gpfifo exists */
            if (size < 0x10) return NVG_BAD_PARAMETER;
            uint32_t cls = rd32(d), ok = 0;
            for (unsigned i = 0; i < sizeof nvgpu_classes / sizeof *nvgpu_classes; i++) ok |= nvgpu_classes[i] == cls;
            if (!ok) return NVG_BAD_PARAMETER;
            if (!c->gpfifo_entries) return NVG_INVALID_STATE;
            for (unsigned i = 0; i < c->nclasses; i++)
                if (c->classes[i] == cls) return NVG_ALREADY_ALLOCATED;
            if (c->nclasses >= NVG_MAX_CLASSES) return NVG_INSUFFICIENT_MEMORY;
            c->classes[c->nclasses++] = cls;
            wr64(d + 8, c->next_obj_id++);
            return NVG_OK;
        }
        case 0x0b: /* ZCULL_BIND: mode 0..3 */
            if (size < 0x10) return NVG_BAD_PARAMETER;
            if (!c->gpfifo_entries) return NVG_INVALID_STATE;
            if (rd32(d + 8) > 3) return NVG_BAD_PARAMETER;
            c->zcull_va = rd64(d);
            c->zcull_mode = rd32(d + 8);
            return NVG_OK;
        case 0x08: { /* SUBMIT_GPFIFO: nothing executes; FENCE_GET returns a fresh fence of the channel's syncpoint */
            if (size < 0x18) return NVG_BAD_PARAMETER;
            uint32_t n = rd32(d + 8), flags = rd32(d + 12);
            if (!c->gpfifo_entries) return NVG_INVALID_STATE;
            if (n > c->gpfifo_entries) return NVG_BAD_PARAMETER;
            c->submits++;
            c->entries_submitted += n;
            if (flags & SUBMIT_FENCE_GET) {
                c->fence_value = env && env->syncpt_incr ? env->syncpt_incr(env->ctx, c->fence_id) : c->fence_value + 1;
                wr32(d + 16, c->fence_id);
                wr32(d + 20, c->fence_value);
            }
            return NVG_OK;
        }
    }
    return NVG_NOT_IMPLEMENTED;
}

/* ---- nvhost-as-gpu ------------------------------------------------------------------------------------------------------------------- */
enum { AS_FIXED = 1 };
static uint64_t align_up(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }
/* Does [at, at+len) intersect a reservation or a mapping? Returns the end of the first one hit, else 0. */
static uint64_t as_overlap(const NvGpuAs *a, uint64_t at, uint64_t len, int check_allocs) {
    for (unsigned i = 0; check_allocs && i < NVG_MAX_ALLOCS; i++) {
        const NvAsAlloc *r = &a->allocs[i];
        uint64_t rl = (uint64_t)r->pages * r->page_size;
        if (r->used && at < r->offset + rl && r->offset < at + len) return r->offset + rl;
    }
    for (unsigned i = 0; i < NVG_MAX_MAPS; i++) {
        const NvAsMap *m = &a->maps[i];
        if (m->used && at < m->offset + m->size && m->offset < at + len) return m->offset + m->size;
    }
    return 0;
}
static int as_in_alloc(const NvGpuAs *a, uint64_t at, uint64_t len) {
    for (unsigned i = 0; i < NVG_MAX_ALLOCS; i++) {
        const NvAsAlloc *r = &a->allocs[i];
        if (r->used && at >= r->offset && at + len <= r->offset + (uint64_t)r->pages * r->page_size) return 1;
    }
    return 0;
}
/* First free range of `len` bytes at `align` from the cursor on; 0 when the space is exhausted. */
static uint64_t as_find(NvGpuAs *a, uint64_t len, uint64_t align) {
    uint64_t at = align_up(a->cursor > a->va_start ? a->cursor : a->va_start, align);
    for (unsigned guard = 0; guard < NVG_MAX_ALLOCS + NVG_MAX_MAPS + 2; guard++) {
        if (at + len > a->va_end || at + len < at) return 0;
        uint64_t hit = as_overlap(a, at, len, 1);
        if (!hit) return at;
        at = align_up(hit, align);
    }
    return 0;
}
static int page_ok(const NvGpuAs *a, uint32_t ps) { return ps == 0x1000 || (a->big_page_size && ps == a->big_page_size); }

int nvgpu_as_ioctl(NvGpuAs *a, const NvGpuEnv *env, NvGpuChan *chan, unsigned nr, uint8_t *d, unsigned size) {
    switch (nr) {
        case 0x09: { /* INITIALIZE_EX */
            if (size < 0x28) return NVG_BAD_PARAMETER;
            uint32_t big = rd32(d), flags = rd32(d + 8);
            uint64_t s = rd64(d + 16), e = rd64(d + 24), sp = rd64(d + 32);
            if (a->initialized) return NVG_INVALID_STATE;
            if (big != 0 && big != 0x10000 && big != 0x20000) return NVG_BAD_PARAMETER;
            if (!e) { /* defaults: 37-bit space, small pages up to 4 GiB, big pages above */
                s = 0x04000000ull;
                sp = 0x100000000ull;
                e = 0x2000000000ull;
            } else if (!(s < sp && sp <= e && !(s & 0xfff) && !(sp & 0xfff))) return NVG_BAD_PARAMETER;
            a->big_page_size = big ? big : 0x10000;
            a->flags = flags;
            a->va_start = s;
            a->va_split = sp;
            a->va_end = e;
            a->cursor = s;
            a->initialized = 1;
            return NVG_OK;
        }
        case 0x01: /* BIND_CHANNEL: the channel behind the fd */
            if (size < 4) return NVG_BAD_PARAMETER;
            return nvgpu_as_bind(a, chan);
        case 0x08: { /* GET_VA_REGIONS */
            if (size < 0x40) return NVG_BAD_PARAMETER;
            if (!a->initialized) return NVG_INVALID_STATE;
            uint32_t cap = rd32(d + 8);
            if (cap >= 0x18) {
                wr64(d + 16, a->va_start);
                wr32(d + 24, 0x1000);
                wr64(d + 32, (a->va_split - a->va_start) / 0x1000);
            }
            if (cap >= 0x30) {
                wr64(d + 40, a->va_split);
                wr32(d + 48, a->big_page_size);
                wr64(d + 56, (a->va_end - a->va_split) / a->big_page_size);
            }
            wr32(d + 8, 0x30);
            return NVG_OK;
        }
        case 0x02: { /* ALLOC_SPACE */
            if (size < 0x18) return NVG_BAD_PARAMETER;
            uint32_t pages = rd32(d), ps = rd32(d + 4), flags = rd32(d + 8);
            if (!a->initialized) return NVG_INVALID_STATE;
            if (!pages || !page_ok(a, ps)) return NVG_BAD_PARAMETER;
            uint64_t len = (uint64_t)pages * ps, at;
            unsigned slot = NVG_MAX_ALLOCS;
            for (unsigned i = 0; i < NVG_MAX_ALLOCS; i++)
                if (!a->allocs[i].used) {
                    slot = i;
                    break;
                }
            if (slot == NVG_MAX_ALLOCS) return NVG_INSUFFICIENT_MEMORY;
            if (flags & AS_FIXED) {
                at = rd64(d + 16);
                if (at & (ps - 1) || at < a->va_start || at + len > a->va_end || at + len < at || as_overlap(a, at, len, 1)) return NVG_BAD_PARAMETER;
            } else {
                uint64_t al = rd64(d + 16);
                at = as_find(a, len, al > ps ? al : ps);
                if (!at) return NVG_INSUFFICIENT_MEMORY;
                a->cursor = at + len;
                wr64(d + 16, at);
            }
            a->allocs[slot] = (NvAsAlloc){.offset = at, .pages = pages, .page_size = ps, .flags = flags, .used = 1};
            return NVG_OK;
        }
        case 0x03: { /* FREE_SPACE: exactly a reservation; mappings inside it go with it */
            if (size < 0x10) return NVG_BAD_PARAMETER;
            uint64_t at = rd64(d);
            uint32_t pages = rd32(d + 8), ps = rd32(d + 12);
            if (!a->initialized) return NVG_INVALID_STATE;
            for (unsigned i = 0; i < NVG_MAX_ALLOCS; i++) {
                NvAsAlloc *r = &a->allocs[i];
                if (!r->used || r->offset != at || r->pages != pages || r->page_size != ps) continue;
                uint64_t len = (uint64_t)pages * ps;
                for (unsigned m = 0; m < NVG_MAX_MAPS; m++)
                    if (a->maps[m].used && a->maps[m].offset >= at && a->maps[m].offset < at + len) a->maps[m].used = 0;
                r->used = 0;
                return NVG_OK;
            }
            return NVG_BAD_PARAMETER;
        }
        case 0x04: { /* MAP_BUFFER_EX: flags bit0 = fixed offset (inside a reservation), else the next free range */
            if (size < 0x28) return NVG_BAD_PARAMETER;
            uint32_t flags = rd32(d), handle = rd32(d + 8), ps = rd32(d + 12);
            int32_t kind = (int32_t)rd32(d + 4);
            uint64_t boff = rd64(d + 16), len = rd64(d + 24);
            if (!a->initialized) return NVG_INVALID_STATE;
            uint32_t hsize = env && env->nvmap_size ? env->nvmap_size(env->ctx, handle) : 0x10000000u;
            if (!hsize || boff >= hsize) return NVG_BAD_PARAMETER;
            if (!len) len = hsize - boff;
            if (boff + len > hsize) return NVG_BAD_PARAMETER;
            if (!ps) ps = len >= a->big_page_size ? a->big_page_size : 0x1000;
            if (!page_ok(a, ps)) return NVG_BAD_PARAMETER;
            uint64_t span = align_up(len, ps), at;
            unsigned slot = NVG_MAX_MAPS;
            for (unsigned i = 0; i < NVG_MAX_MAPS; i++)
                if (!a->maps[i].used) {
                    slot = i;
                    break;
                }
            if (slot == NVG_MAX_MAPS) return NVG_INSUFFICIENT_MEMORY;
            if (flags & AS_FIXED) {
                at = rd64(d + 32);
                if (at & (ps - 1) || !as_in_alloc(a, at, span) || as_overlap(a, at, span, 0)) return NVG_BAD_PARAMETER;
            } else {
                at = as_find(a, span, ps);
                if (!at) return NVG_INSUFFICIENT_MEMORY;
                a->cursor = at + span;
                wr64(d + 32, at);
            }
            a->maps[slot] = (NvAsMap){.offset = at, .size = span, .buf_off = boff, .handle = handle, .page_size = ps, .kind = (uint32_t)kind, .flags = flags, .used = 1};
            return NVG_OK;
        }
        case 0x05: { /* UNMAP_BUFFER */
            if (size < 8) return NVG_BAD_PARAMETER;
            uint64_t at = rd64(d);
            for (unsigned i = 0; i < NVG_MAX_MAPS; i++)
                if (a->maps[i].used && a->maps[i].offset == at) {
                    a->maps[i].used = 0;
                    return NVG_OK;
                }
            return NVG_BAD_PARAMETER;
        }
    }
    return NVG_NOT_IMPLEMENTED;
}

/* ---- nvhost-ctrl-gpu ----------------------------------------------------------------------------------------------------------------- */
static int zbc_find(NvZbcEntry *t, const NvZbcEntry *e) {
    for (int i = 0; i < NVG_ZBC_MAX; i++)
        if (t[i].used && !memcmp(t[i].color_ds, e->color_ds, 16) && !memcmp(t[i].color_l2, e->color_l2, 16) && t[i].depth == e->depth && t[i].format == e->format) return i;
    return -1;
}
int nvgpu_ctrl_ioctl(NvGpuCtrl *c, unsigned nr, uint8_t *d, unsigned size, uint64_t now_ns) {
    switch (nr) {
        case 0x01: /* ZCULL_GET_CTX_SIZE */
            if (size < 4) return NVG_BAD_PARAMETER;
            wr32(d, 0x1000);
            return NVG_OK;
        case 0x02: { /* ZCULL_GET_INFO: width/height align, pixel squares by aliquots, aliquot total, region byte multiplier, header sizes, subregion align/count */
            if (size < 0x28) return NVG_BAD_PARAMETER;
            static const uint32_t info[10] = {16, 32, 0x400, 0x800, 0x20, 0x20, 0xc0, 0x20, 0x40, 0x10};
            memcpy(d, info, sizeof info);
            return NVG_OK;
        }
        case 0x03: { /* ZBC_SET_TABLE {color_ds[4], color_l2[4], depth, format, type 1 colour / 2 depth}: identical entries share a slot */
            if (size < 0x2c) return NVG_BAD_PARAMETER;
            uint32_t type = rd32(d + 40);
            NvZbcEntry e = {.depth = rd32(d + 32), .format = rd32(d + 36), .used = 1};
            memcpy(e.color_ds, d, 16);
            memcpy(e.color_l2, d + 16, 16);
            if (type != 1 && type != 2) return NVG_BAD_PARAMETER;
            NvZbcEntry *t = type == 1 ? c->color : c->depth;
            if (zbc_find(t, &e) >= 0) return NVG_OK;
            for (int i = 0; i < NVG_ZBC_MAX; i++)
                if (!t[i].used) {
                    t[i] = e;
                    return NVG_OK;
                }
            return NVG_INSUFFICIENT_MEMORY;
        }
        case 0x04: { /* ZBC_QUERY_TABLE {color_ds[4], color_l2[4], depth, ref_cnt, format, type, index_size in:index out:table size} */
            if (size < 0x34) return NVG_BAD_PARAMETER;
            uint32_t type = rd32(d + 44), idx = rd32(d + 48);
            if (type != 1 && type != 2) return NVG_BAD_PARAMETER;
            const NvZbcEntry *t = type == 1 ? c->color : c->depth;
            if (idx >= NVG_ZBC_MAX || !t[idx].used) return NVG_BAD_PARAMETER;
            memcpy(d, t[idx].color_ds, 16);
            memcpy(d + 16, t[idx].color_l2, 16);
            wr32(d + 32, t[idx].depth);
            wr32(d + 36, 1);
            wr32(d + 40, t[idx].format);
            wr32(d + 48, NVG_ZBC_MAX);
            return NVG_OK;
        }
        case 0x05: { /* GET_CHARACTERISTICS {u64 buffer size, u64 buffer address, 0xa0 bytes}: the GM20B of the retail console */
            if (size < 0xb0) return NVG_NOT_IMPLEMENTED;
            uint8_t *ch = d + 0x10;
            memset(ch, 0, 0xa0);
            uint32_t w[] = {0x120, 0xb, 0xa1, 1}; /* arch GM200, impl GM20B, revision A1, one GPC */
            memcpy(ch, w, sizeof w);
            wr64(ch + 0x10, 0x40000);
            wr64(ch + 0x38, 0x55);
            uint32_t t[] = {2, 0x20, 0x20000, 0x20000, 0x1b, 0x30000}; /* TPC per GPC, AXI bus, big page 128K, compression page 128K, PDE bits, big page sizes */
            memcpy(ch + 0x20, t, sizeof t);
            uint32_t cls[10];
            memcpy(cls, nvgpu_classes, sizeof nvgpu_classes);
            cls[6] = 1; /* 2D, 3D, compute, gpfifo, i2m, copy; gpc mask, SM arch, warps */
            cls[7] = 0x503;
            cls[8] = 0x503;
            cls[9] = 0x20;
            memcpy(ch + 0x40, cls, sizeof cls);
            wr64(ch + 0x88, 0x6230326d67); /* "gm20b" */
            wr64(d, 0xa0);
            return NVG_OK;
        }
        case 0x06: /* GET_TPC_MASKS {u32 buf size, u32, u64 buf addr, u64 mask out}: two TPCs on the single GPC */
            if (size < 0x18) return NVG_BAD_PARAMETER;
            wr64(d + 16, 3);
            return NVG_OK;
        case 0x07: /* FLUSH_L2 {u32 flush, u32 invalidate} */
            if (size < 8) return NVG_BAD_PARAMETER;
            c->l2_flushes++;
            return NVG_OK;
        case 0x14: { /* GET_GPU_TIME {u64 ns, u64}: monotonic nanoseconds since the node opened */
            if (size < 8) return NVG_BAD_PARAMETER;
            uint64_t t = now_ns > c->t0_ns ? now_ns - c->t0_ns : 0;
            if (t < c->last_time_ns) t = c->last_time_ns;
            c->last_time_ns = t;
            wr64(d, t);
            return NVG_OK;
        }
    }
    return NVG_NOT_IMPLEMENTED;
}
