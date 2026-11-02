/* nvdrv_nodes_core: see nvdrv_nodes_core.h (layouts and sources) and docs/reference/nvdrv-layouts.md. Pure state machines over the request buffers; no host
 * calls. Written from the public layout facts only (numbers, sizes, offsets); no driver code was used. */
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
    c->enabled = 1; /* a channel is runnable once opened [nvgpu: ENABLE / DISABLE toggle it] */
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
/* SUBMIT_GPFIFO and KICKOFF_PB share the leading 0x18 bytes {u64 gpfifo, u32 num_entries, u32 flags, fence{u32 id, u32 value}} [libnx]. Nothing executes;
 * FENCE_WAIT is recorded, FENCE_GET returns a fresh fence of the channel's syncpoint. */
static int chan_submit(NvGpuChan *c, const NvGpuEnv *env, uint8_t *d) {
    uint32_t n = rd32(d + 8), flags = rd32(d + 12);
    if (!c->gpfifo_entries || !c->enabled) return NVG_INVALID_STATE;
    if (n > c->gpfifo_entries) return NVG_BAD_PARAMETER;
    c->submits++;
    c->entries_submitted += n;
    if (flags & NVG_SUBMIT_FENCE_WAIT) c->fence_waits++;
    if (flags & NVG_SUBMIT_FENCE_GET) {
        c->fence_value = env && env->syncpt_incr ? env->syncpt_incr(env->ctx, c->fence_id) : c->fence_value + 1;
        wr32(d + 16, c->fence_id);
        wr32(d + 20, c->fence_value);
    }
    return NVG_OK;
}
/* The ring itself: needs a bound address space, once per channel. */
static int chan_gpfifo(NvGpuChan *c, uint32_t n, uint32_t flags) {
    if (!c->as) return NVG_INVALID_STATE;
    if (c->gpfifo_entries) return NVG_ALREADY_ALLOCATED;
    if (!n || n > 0x40000) return NVG_BAD_PARAMETER;
    c->gpfifo_entries = n;
    c->gpfifo_flags = flags;
    return NVG_OK;
}
int nvgpu_chan_ioctl(NvGpuChan *c, const NvGpuEnv *env, unsigned nr, uint8_t *d, unsigned size) {
    switch (nr) {
        case NVG_CH_SET_NVMAP_FD:
            if (size < NVG_SZ_SET_NVMAP_FD) return NVG_BAD_PARAMETER;
            c->nvmap_fd = (int32_t)rd32(d);
            return NVG_OK;
        case NVG_CH_SET_TIMEOUT:
            if (size < NVG_SZ_SET_TIMEOUT) return NVG_BAD_PARAMETER;
            c->timeout_ms = rd32(d);
            return NVG_OK;
        case NVG_CH_SET_PRIORITY: { /* low 50, medium 100, high 150 [libnx] */
            if (size < NVG_SZ_SET_PRIORITY) return NVG_BAD_PARAMETER;
            uint32_t p = rd32(d);
            if (p != 50 && p != 100 && p != 150) return NVG_BAD_PARAMETER;
            c->priority = p;
            return NVG_OK;
        }
        case NVG_CH_SET_ERROR_NOTIFIER: /* offset, size (unused on the Switch per libnx), mem/enable */
            if (size < NVG_SZ_SET_ERROR_NOTIFIER) return NVG_BAD_PARAMETER;
            c->err_off = rd64(d);
            c->err_size = rd64(d + 8);
            c->err_mem = rd32(d + 16);
            return NVG_OK;
        case NVG_CH_ENABLE:
            c->enabled = 1;
            return NVG_OK;
        case NVG_CH_DISABLE:
            c->enabled = 0;
            return NVG_OK;
        case NVG_CH_PREEMPT: /* nothing runs, so nothing to take off the engine: counted */
            c->preempts++;
            return NVG_OK;
        case NVG_CH_FORCE_RESET: /* the ring and the objects stay; the error state (always clean here) is what a reset clears */
            c->resets++;
            return NVG_OK;
        case NVG_CH_EVENT_ID_CONTROL: { /* {u32 cmd, u32 event_id}: cmd 1 enables the event, any other command disables it [nvgpu] */
            if (size < NVG_SZ_EVENT_ID_CONTROL) return NVG_BAD_PARAMETER;
            uint32_t cmd = rd32(d), id = rd32(d + 4);
            if (id >= NVG_EVENT_ID_MAX || cmd > 2) return NVG_BAD_PARAMETER;
            if (cmd == NVG_EVENT_ID_CMD_ENABLE) c->events |= 1u << id;
            else c->events &= ~(1u << id);
            return NVG_OK;
        }
        case NVG_CH_GET_ERROR_INFO: /* {u32 type, u32 info[31]}: no channel ever faults here, type 0 */
            if (size < NVG_SZ_GET_ERROR_INFO) return NVG_BAD_PARAMETER;
            memset(d, 0, NVG_SZ_GET_ERROR_INFO);
            return NVG_OK;
        case NVG_CH_GET_ERROR_NOTIFICATION: /* {u64 timestamp, u32 info32, u16 info16, u16 status}: no notification, status 0 */
            if (size < NVG_SZ_GET_ERROR_NOTIFICATION) return NVG_BAD_PARAMETER;
            memset(d, 0, NVG_SZ_GET_ERROR_NOTIFICATION);
            return NVG_OK;
        case NVG_CH_ALLOC_GPFIFO_EX: /* [nvgpu] {num_entries, num_inflight_jobs, flags, reserved[5]}: no fence is returned by this variant */
            if (size < NVG_SZ_ALLOC_GPFIFO_EX) return NVG_BAD_PARAMETER;
            {
                int r = chan_gpfifo(c, rd32(d), rd32(d + 8));
                if (!r) c->gpfifo_inflight = rd32(d + 4);
                return r;
            }
        case NVG_CH_ALLOC_GPFIFO_EX2: { /* [libnx] {num_entries, flags, unk0, fence out, unk1..3} */
            if (size < NVG_SZ_ALLOC_GPFIFO_EX2) return NVG_BAD_PARAMETER;
            int r = chan_gpfifo(c, rd32(d), rd32(d + 4));
            if (r) return r;
            wr32(d + 12, c->fence_id);
            wr32(d + 16, c->fence_value);
            return NVG_OK;
        }
        case NVG_CH_ALLOC_OBJ_CTX: { /* a supported engine class, once per class, after the gpfifo exists */
            if (size < NVG_SZ_ALLOC_OBJ_CTX) return NVG_BAD_PARAMETER;
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
        case NVG_CH_ZCULL_BIND: /* mode 0..3: global, no ctxsw, separate buffer, part of regular buffer [libnx] */
            if (size < NVG_SZ_ZCULL_BIND) return NVG_BAD_PARAMETER;
            if (!c->gpfifo_entries) return NVG_INVALID_STATE;
            if (rd32(d + 8) > 3) return NVG_BAD_PARAMETER;
            c->zcull_va = rd64(d);
            c->zcull_mode = rd32(d + 8);
            return NVG_OK;
        case NVG_CH_SUBMIT_GPFIFO: /* the entries follow inline (0x18 + n * 8 bytes); they are not read */
            if (size < NVG_SZ_SUBMIT_GPFIFO) return NVG_BAD_PARAMETER;
            return chan_submit(c, env, d);
        case NVG_CH_KICKOFF_PB: { /* SUBMIT_GPFIFO with the entries in the Ioctl2 input buffer */
            if (size < NVG_SZ_KICKOFF_PB) return NVG_BAD_PARAMETER;
            int r = chan_submit(c, env, d);
            if (!r) c->kickoffs++;
            return r;
        }
    }
    return NVG_NOT_IMPLEMENTED;
}
int nvgpu_chan_ioctl_typed(NvGpuChan *c, const NvGpuEnv *env, unsigned type, unsigned nr, uint8_t *d, unsigned size) {
    if (type == NVG_TYPE_CHANNEL) return nvgpu_chan_ioctl(c, env, nr, d, size);
    if (type != NVG_TYPE_GPU) return NVG_NOT_IMPLEMENTED;
    switch (nr) { /* [libnx] nvioctlChannel_SetUserData: _NV_IOW(0x47, 0x14, u64) on the channel fd */
        case NVG_GPU_SET_USER_DATA:
            if (size < NVG_SZ_USER_DATA) return NVG_BAD_PARAMETER;
            c->user_data = rd64(d);
            return NVG_OK;
        case NVG_GPU_GET_USER_DATA:
            if (size < NVG_SZ_USER_DATA) return NVG_BAD_PARAMETER;
            wr64(d, c->user_data);
            return NVG_OK;
    }
    return NVG_NOT_IMPLEMENTED;
}

/* ---- nvhost-as-gpu ------------------------------------------------------------------------------------------------------------------- */
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
        case NVG_AS_INITIALIZE_EX: { /* [libnx] {u32 flags, s32 as_fd, u32 big_page_size, u32 reserved, u64 x3}; the three words read as nvgpu's va range */
            if (size < NVG_SZ_INITIALIZE_EX) return NVG_BAD_PARAMETER;
            uint32_t flags = rd32(d), big = rd32(d + 8);
            uint64_t s = rd64(d + 16), e = rd64(d + 24), sp = rd64(d + 32);
            if (a->initialized) return NVG_INVALID_STATE;
            if (big != 0 && big != 0x10000 && big != 0x20000) return NVG_BAD_PARAMETER;
            if (!e) { /* defaults: 37-bit space, small pages up to 4 GiB, big pages above */
                s = 0x04000000ull;
                sp = 0x100000000ull;
                e = 0x2000000000ull;
            } else if (!(s < sp && sp <= e && !(s & 0xfff) && !(sp & 0xfff))) return NVG_BAD_PARAMETER;
            a->big_page_size = big ? big : 0x20000; /* 0 = the default big page size GET_CHARACTERISTICS reports */
            a->flags = flags;
            a->va_start = s;
            a->va_split = sp;
            a->va_end = e;
            a->cursor = s;
            a->initialized = 1;
            return NVG_OK;
        }
        case NVG_AS_BIND_CHANNEL: /* the channel behind the fd */
            if (size < NVG_SZ_BIND_CHANNEL) return NVG_BAD_PARAMETER;
            return nvgpu_as_bind(a, chan);
        case NVG_AS_GET_VA_REGIONS: { /* buf_size in: room the caller has (libnx passes 0x30); out: 0x30, two regions of 0x18 */
            if (size < NVG_SZ_GET_VA_REGIONS) return NVG_BAD_PARAMETER;
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
        case NVG_AS_ALLOC_SPACE: { /* flags bit0 fixed offset, bit1 sparse [libnx][nvgpu] */
            if (size < NVG_SZ_ALLOC_SPACE) return NVG_BAD_PARAMETER;
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
            if (flags & NVG_AS_ALLOC_FIXED) {
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
        case NVG_AS_FREE_SPACE: { /* exactly a reservation; mappings inside it go with it */
            if (size < NVG_SZ_FREE_SPACE) return NVG_BAD_PARAMETER;
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
        case NVG_AS_MAP_BUFFER_EX: { /* [libnx] flags bit0 fixed offset (inside a reservation), bit2 cacheable, bit8 modify (re-kind an existing mapping);
                                       * page_size and offset are in/out */
            if (size < NVG_SZ_MAP_BUFFER_EX) return NVG_BAD_PARAMETER;
            uint32_t flags = rd32(d), handle = rd32(d + 8), ps = rd32(d + 12);
            int32_t kind = (int32_t)rd32(d + 4);
            uint64_t boff = rd64(d + 16), len = rd64(d + 24);
            if (!a->initialized) return NVG_INVALID_STATE;
            if (flags & NVG_AS_MAP_MODIFY) { /* the mapping that starts at `offset` keeps its place; its kind (and handle) change */
                uint64_t at = rd64(d + 32);
                for (unsigned i = 0; i < NVG_MAX_MAPS; i++) {
                    NvAsMap *m = &a->maps[i];
                    if (!m->used || m->offset != at || (len && len > m->size)) continue;
                    m->kind = (uint32_t)kind;
                    if (handle) m->handle = handle;
                    m->flags = (m->flags & ~(uint32_t)NVG_AS_MAP_CACHEABLE) | (flags & NVG_AS_MAP_CACHEABLE);
                    wr32(d + 12, m->page_size);
                    return NVG_OK;
                }
                return NVG_BAD_PARAMETER;
            }
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
            if (flags & NVG_AS_MAP_FIXED) {
                at = rd64(d + 32);
                if (at & (ps - 1) || !as_in_alloc(a, at, span) || as_overlap(a, at, span, 0)) return NVG_BAD_PARAMETER;
            } else {
                at = as_find(a, span, ps);
                if (!at) return NVG_INSUFFICIENT_MEMORY;
                a->cursor = at + span;
                wr64(d + 32, at);
            }
            wr32(d + 12, ps);
            a->maps[slot] = (NvAsMap){.offset = at, .size = span, .buf_off = boff, .handle = handle, .page_size = ps, .kind = (uint32_t)kind, .flags = flags, .used = 1};
            return NVG_OK;
        }
        case NVG_AS_UNMAP_BUFFER: {
            if (size < NVG_SZ_UNMAP_BUFFER) return NVG_BAD_PARAMETER;
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
/* GET_CHARACTERISTICS payload [libnx nvioctl_gpu_characteristics, 0xA0 bytes]: the GM20B of the retail console, the values libnx reads on hardware. */
static void characteristics(uint8_t *ch) {
    memset(ch, 0, NVG_SZ_CHARACTERISTICS);
    wr32(ch + 0x00, 0x120);  /* arch GM200 */
    wr32(ch + 0x04, 0xb);    /* impl GM20B */
    wr32(ch + 0x08, 0xa1);   /* revision A1 */
    wr32(ch + 0x0c, 1);      /* num_gpc */
    wr64(ch + 0x10, 0x40000); /* L2 cache bytes; +0x18 on-board video memory 0 */
    wr32(ch + 0x20, 2);      /* num_tpc_per_gpc */
    wr32(ch + 0x24, 0x20);   /* bus type AXI */
    wr32(ch + 0x28, 0x20000); /* big page size */
    wr32(ch + 0x2c, 0x20000); /* compression page size */
    wr32(ch + 0x30, 0x1b);   /* pde coverage bit count */
    wr32(ch + 0x34, 0x30000); /* available big page sizes (64K | 128K) */
    wr32(ch + 0x38, 1);      /* gpc mask */
    wr32(ch + 0x3c, 0x503);  /* sm arch sm version */
    wr32(ch + 0x40, 0x503);  /* sm arch spa version */
    wr32(ch + 0x44, 0x80);   /* sm arch warp count */
    wr32(ch + 0x48, 0x28);   /* gpu va bit count; +0x4c reserved */
    wr64(ch + 0x50, 0x55);   /* flags: HAS_SYNCPOINTS | SPARSE_ALLOCS | CYCLE_STATS | ... as read on hardware */
    for (unsigned i = 0; i < 6; i++) wr32(ch + 0x58 + 4 * i, nvgpu_classes[i]); /* 2D, 3D, compute, gpfifo, inline-to-memory, DMA copy */
    wr32(ch + 0x70, 1);       /* max_fbps_count; +0x74 fbp_en_mask 0 */
    wr32(ch + 0x78, 2);       /* max_ltc_per_fbp */
    wr32(ch + 0x7c, 1);       /* max_lts_per_ltc; +0x80 max_tex_per_tpc 0 */
    wr32(ch + 0x84, 1);       /* max_gpc_count */
    wr32(ch + 0x88, 0x21d70); /* rop_l2_en_mask[0]; +0x8c [1] 0 */
    wr64(ch + 0x90, 0x6230326d67ull); /* chip name "gm20b"; +0x98 gr_compbit_store_base_hw 0 */
}
int nvgpu_ctrl_ioctl(NvGpuCtrl *c, unsigned nr, uint8_t *d, unsigned size, uint64_t now_ns) {
    switch (nr) {
        case NVG_CTRL_ZCULL_GET_CTX_SIZE:
            if (size < NVG_SZ_ZCULL_GET_CTX_SIZE) return NVG_BAD_PARAMETER;
            wr32(d, 0x1000);
            return NVG_OK;
        case NVG_CTRL_ZCULL_GET_INFO: { /* [libnx nvioctl_zcull_info] width/height align, pixel squares by aliquots, aliquot total, region byte multiplier,
                                        * region / subregion header sizes, subregion width / height align, subregion count */
            if (size < NVG_SZ_ZCULL_GET_INFO) return NVG_BAD_PARAMETER;
            static const uint32_t info[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xc0, 0x20, 0x40, 0x10};
            memcpy(d, info, sizeof info);
            return NVG_OK;
        }
        case NVG_CTRL_ZBC_SET_TABLE: { /* {color_ds[4], color_l2[4], depth, format, type 1 colour / 2 depth}: identical entries share a slot */
            if (size < NVG_SZ_ZBC_SET_TABLE) return NVG_BAD_PARAMETER;
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
        case NVG_CTRL_ZBC_QUERY_TABLE: { /* {color_ds[4], color_l2[4], depth, ref_cnt, format, type, index_size in: index, out: table size} */
            if (size < NVG_SZ_ZBC_QUERY_TABLE) return NVG_BAD_PARAMETER;
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
        case NVG_CTRL_GET_CHARACTERISTICS: { /* {u64 buf_size in/out, u64 buf_addr, characteristics}: buf_size comes back as 0xA0 [libnx] */
            if (size < NVG_SZ_GET_CHARACTERISTICS) return NVG_BAD_PARAMETER;
            uint64_t want = rd64(d);
            uint8_t ch[NVG_SZ_CHARACTERISTICS];
            characteristics(ch);
            memset(d + 0x10, 0, NVG_SZ_CHARACTERISTICS);
            memcpy(d + 0x10, ch, want && want < NVG_SZ_CHARACTERISTICS ? (size_t)want : NVG_SZ_CHARACTERISTICS); /* a smaller caller gets its prefix */
            wr64(d, NVG_SZ_CHARACTERISTICS);
            return NVG_OK;
        }
        case NVG_CTRL_GET_TPC_MASKS: /* {u32 buf size, u32, u64 buf addr, u64 mask out}: two TPCs on the single GPC */
            if (size < NVG_SZ_GET_TPC_MASKS) return NVG_BAD_PARAMETER;
            wr64(d + 16, 3);
            return NVG_OK;
        case NVG_CTRL_FLUSH_L2: /* {u32 flush, u32 invalidate} */
            if (size < NVG_SZ_FLUSH_L2) return NVG_BAD_PARAMETER;
            c->l2_flushes++;
            return NVG_OK;
        case NVG_CTRL_ZBC_GET_ACTIVE_SLOT_MASK: { /* {u32 slot, u32 mask}: slot 7 as libnx reads on hardware; mask = the colour slots in use */
            if (size < NVG_SZ_ZBC_GET_ACTIVE_SLOT_MASK) return NVG_BAD_PARAMETER;
            uint32_t mask = 0;
            for (int i = 0; i < NVG_ZBC_MAX; i++) mask |= (uint32_t)(c->color[i].used != 0) << i;
            wr32(d, 7);
            wr32(d + 4, mask);
            return NVG_OK;
        }
        case NVG_CTRL_GET_GPU_TIME: { /* {u64 ns, u64 reserved}: monotonic nanoseconds since the node opened */
            if (size < NVG_SZ_GET_GPU_TIME) return NVG_BAD_PARAMETER;
            uint64_t t = now_ns > c->t0_ns ? now_ns - c->t0_ns : 0;
            if (t < c->last_time_ns) t = c->last_time_ns;
            c->last_time_ns = t;
            wr64(d, t);
            wr64(d + 8, 0);
            return NVG_OK;
        }
    }
    return NVG_NOT_IMPLEMENTED;
}
