/* nvdrv / nvdrv:a / nvdrv:s / nvdrv:t: the Tegra driver front end the SDK's NvRm layer talks to (nv::InitializeGraphics, android:: gralloc
 * buffers of the vi native window, media). Napple's NVN is implemented by the graphics back end, so the GPU channels, address spaces and
 * syncpoints of the real driver are not needed: this service answers the *memory* side (nvmap handle bookkeeping on guest-provided memory) and
 * the control side (syncpoint counters, event objects) at the level the SDK initializes and polls, the host1x channels of the multimedia
 * engines (nvhost-nvdec/vic/nvjpg/msenc: the channel ioctls, with submits that complete at once), and refuses everything else by number
 * (NotImplemented) so a game that really needs a device reports it in the log and the compat summary. See docs/architecture/runtime-services.md. */
#include "../ipc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    NV_OK = 0,
    NV_NOT_IMPLEMENTED = 1,
    NV_NOT_SUPPORTED = 2,
    NV_NOT_INITIALIZED = 3,
    NV_BAD_PARAMETER = 4,
    NV_TIMEOUT = 5,
    NV_INSUFFICIENT_MEMORY = 6,
    NV_ACCESS_DENIED = 0x10
};
typedef enum { DEV_NONE, DEV_NVMAP, DEV_CTRL, DEV_CHANNEL, DEV_OTHER } DevKind;
/* host1x client channel state (nvhost-nvdec / vic / nvjpg / msenc) */
#define CHANNEL_SYNCPTS 16
typedef struct {
    uint32_t class_id;
    int32_t nvmap_fd;
    uint32_t timeout_ms, submit_timeout_ms;
    uint32_t syncpt[CHANNEL_SYNCPTS];
    uint64_t submits;
} Channel;
typedef struct {
    DevKind kind;
    char path[48];
    int open;
    Channel ch;
} Fd;
#define MAX_FD 64
static Fd fds[MAX_FD];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int initialized;

typedef struct {
    uint32_t id;
    int used;
    uint32_t size, align, flags, heapmask;
    uint64_t addr;
    int allocated;
    uint32_t refs;
    uint32_t iova;
} NvMapHandle;
#define MAX_NVMAP 4096
static NvMapHandle nvmap[MAX_NVMAP];
static uint32_t next_nvmap_id = 1;
/* Syncpoints: `max` is what has been submitted, `val` what the (instant) engine has completed. Event waits register a pending slot that fires when
 * the counter reaches the threshold; guest event objects are created per event id on QueryEvent. */
#define NSYNC 192
static uint32_t syncpt[NSYNC], syncpt_max[NSYNC];
#define MAX_SLOTS 64
typedef struct {
    int used;
    uint32_t id, threshold;
} PendingWait;
static PendingWait pending[MAX_SLOTS];
typedef struct {
    uint32_t event_id, writable, readable;
    int used;
} NvEvent;
static NvEvent events[MAX_SLOTS];
static uint32_t slot_registered; /* bit per EVENT_REGISTER slot (low 32) */
static void events_signal_for(unsigned slot) {
    for (unsigned i = 0; i < MAX_SLOTS; i++)
        if (events[i].used && (events[i].event_id & 0xff) == slot) event_signal_handle(events[i].writable);
}
static void syncpt_advance(uint32_t id, uint32_t n) {
    if (id >= NSYNC) return;
    syncpt[id] += n;
    for (unsigned i = 0; i < MAX_SLOTS; i++)
        if (pending[i].used && pending[i].id == id && (int32_t)(syncpt[id] - pending[i].threshold) >= 0) {
            pending[i].used = 0;
            events_signal_for(i);
        }
}

static const char *const known_other[] = {"/dev/nvhost-gpu",      "/dev/nvhost-as-gpu", "/dev/nvhost-ctrl-gpu", "/dev/nvhost-dbg-gpu",
                                          "/dev/nvhost-prof-gpu", "/dev/nvhost-tsec",   "/dev/nvdisp-disp0",    "/dev/nvdisp-disp1",
                                          "/dev/nvdisp-ctrl",     "/dev/nvsched-ctrl",  "/dev/nvsched"};
/* host1x engines with their host1x class ids */
static const struct {
    const char *path;
    uint32_t class_id;
} channels[] = {
    {"/dev/nvhost-nvdec", 0xF0},
    {"/dev/nvhost-vic", 0x5D},
    {"/dev/nvhost-nvjpg", 0xC0},
    {"/dev/nvhost-msenc", 0x21},
};

static NvMapHandle *nvmap_find(uint32_t id) {
    for (unsigned i = 0; i < MAX_NVMAP; i++)
        if (nvmap[i].used && nvmap[i].id == id) return &nvmap[i];
    return NULL;
}

static uint32_t nvmap_ioctl(unsigned nr, uint8_t *buf, uint32_t size) {
    pthread_mutex_lock(&lock);
    uint32_t r = NV_NOT_IMPLEMENTED;
    switch (nr) {
        case 0x01: { /* CREATE {u32 size; u32 handle} */
            if (size < 8) {
                r = NV_BAD_PARAMETER;
                break;
            }
            uint32_t sz;
            memcpy(&sz, buf, 4);
            NvMapHandle *h = NULL;
            for (unsigned i = 0; i < MAX_NVMAP; i++)
                if (!nvmap[i].used) {
                    h = &nvmap[i];
                    break;
                }
            if (!h || !sz) {
                r = h ? NV_BAD_PARAMETER : NV_INSUFFICIENT_MEMORY;
                break;
            }
            *h = (NvMapHandle){.id = next_nvmap_id++, .used = 1, .size = sz, .refs = 1};
            memcpy(buf + 4, &h->id, 4);
            r = NV_OK;
            break;
        }
        case 0x03: { /* FROM_ID {u32 id; u32 handle} */
            if (size < 8) {
                r = NV_BAD_PARAMETER;
                break;
            }
            uint32_t id;
            memcpy(&id, buf, 4);
            NvMapHandle *h = nvmap_find(id);
            if (!h) {
                r = NV_BAD_PARAMETER;
                break;
            }
            h->refs++;
            memcpy(buf + 4, &h->id, 4);
            r = NV_OK;
            break;
        }
        case 0x04: { /* ALLOC {handle, heapmask, flags, align, u8 kind, pad[7], u64 address} */
            if (size < 0x20) {
                r = NV_BAD_PARAMETER;
                break;
            }
            uint32_t id, heap, flags, align;
            uint64_t addr;
            memcpy(&id, buf, 4);
            memcpy(&heap, buf + 4, 4);
            memcpy(&flags, buf + 8, 4);
            memcpy(&align, buf + 12, 4);
            memcpy(&addr, buf + 24, 8);
            NvMapHandle *h = nvmap_find(id);
            if (!h) {
                r = NV_BAD_PARAMETER;
                break;
            }
            if (h->allocated) {
                r = 13;
                break;
            } /* AlreadyAllocated */
            h->heapmask = heap;
            h->flags = flags;
            h->align = align;
            h->addr = addr;
            h->allocated = 1;
            r = NV_OK;
            break;
        }
        case 0x05: { /* FREE {handle, pad, u64 address(out), u32 size(out), u32 flags(out)} */
            if (size < 0x18) {
                r = NV_BAD_PARAMETER;
                break;
            }
            uint32_t id;
            memcpy(&id, buf, 4);
            NvMapHandle *h = nvmap_find(id);
            if (!h) {
                r = NV_BAD_PARAMETER;
                break;
            }
            uint64_t addr = h->addr;
            uint32_t sz = h->size, fl = 0;
            memcpy(buf + 8, &addr, 8);
            memcpy(buf + 16, &sz, 4);
            memcpy(buf + 20, &fl, 4);
            if (--h->refs == 0) h->used = 0;
            r = NV_OK;
            break;
        }
        case 0x09: { /* PARAM {handle, param, u32 result} */
            if (size < 12) {
                r = NV_BAD_PARAMETER;
                break;
            }
            uint32_t id, param;
            memcpy(&id, buf, 4);
            memcpy(&param, buf + 4, 4);
            NvMapHandle *h = nvmap_find(id);
            if (!h) {
                r = NV_BAD_PARAMETER;
                break;
            }
            uint32_t v = 0;
            switch (param) {
                case 1: v = h->size; break;
                case 2: v = h->align; break;
                case 3: v = h->heapmask; break;
                case 4: v = h->flags; break;
                case 5: v = (uint32_t)h->addr; break;
                case 6: v = h->id; break;
                default: v = 0;
            }
            memcpy(buf + 8, &v, 4);
            r = NV_OK;
            break;
        }
        case 0x0e: { /* GET_ID {u32 id(out); u32 handle} */
            if (size < 8) {
                r = NV_BAD_PARAMETER;
                break;
            }
            uint32_t id;
            memcpy(&id, buf + 4, 4);
            NvMapHandle *h = nvmap_find(id);
            if (!h) {
                r = NV_BAD_PARAMETER;
                break;
            }
            memcpy(buf, &h->id, 4);
            r = NV_OK;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static uint32_t ctrl_ioctl(unsigned nr, uint8_t *buf, uint32_t size) {
    uint32_t id = 0;
    if (size >= 4) memcpy(&id, buf, 4);
    switch (nr) {
        case 0x14:
            if (size >= 8) {
                uint32_t v = id < NSYNC ? syncpt[id] : 0;
                memcpy(buf + 4, &v, 4);
                return NV_OK;
            }
            return NV_BAD_PARAMETER; /* SYNCPT_READ */
        case 0x15:
            if (size >= 4) {
                syncpt_max[id % NSYNC]++;
                syncpt_advance(id, 1);
                return NV_OK;
            }
            return NV_BAD_PARAMETER; /* SYNCPT_INCR */
        case 0x16:
        case 0x19: return NV_OK; /* SYNCPT_WAIT(EX): the engine is instant */
        case 0x17: return NV_OK; /* MODULE_MUTEX */
        case 0x1a:
            if (size >= 8) {
                uint32_t v = id < NSYNC ? syncpt_max[id] : 0;
                memcpy(buf + 4, &v, 4);
                return NV_OK;
            }
            return NV_BAD_PARAMETER;        /* SYNCPT_READ_MAX */
        case 0x1b: return NV_NOT_SUPPORTED; /* GET_CONFIG */
        case 0x1c: return NV_OK;            /* EVENT_SIGNAL */
        case 0x1f:
            if (size >= 4) {
                slot_registered |= 1u << (id & 31);
                return NV_OK;
            }
            return NV_BAD_PARAMETER; /* EVENT_REGISTER {slot} */
        case 0x1d:
        case 0x1e: { /* EVENT_WAIT(_ASYNC) {id, threshold, timeout, value/slot} */
            if (size < 16) return NV_BAD_PARAMETER;
            uint32_t thr, slot;
            memcpy(&thr, buf + 4, 4);
            memcpy(&slot, buf + 12, 4);
            if (id >= NSYNC) return NV_BAD_PARAMETER;
            if ((int32_t)(syncpt[id] - thr) >= 0) return NV_OK; /* already reached: no event needed */
            pending[slot % MAX_SLOTS] = (PendingWait){1, id, thr};
            return NV_TIMEOUT; /* TryAgain: the event signals when reached */
        }
    }
    return NV_NOT_IMPLEMENTED;
}
/* Host1x channels (nvhost-nvdec / -vic / -nvjpg / -msenc). The ioctl surface is the nvhost channel ABI (the host1x part shared by the
 * multimedia engines; layouts as documented by the public driver sources and switchbrew):
 *   'H' 0x01 SET_NVMAP_FD {s32 fd}   'H' 0x03 SET_TIMEOUT {u32 ms}
 *   0x01 SUBMIT {u32 cmdbufs, relocs, syncpt_incrs, fences} + cmdbuf{s32 handle, u32 offset, s32 words}[cmdbufs] + reloc{4 x s32}[relocs]
 *        + u32 shift[relocs] + incr{u32 id, u32 count, 3 x u32 reserved}[syncpt_incrs] + u32 fence_threshold[fences] (out)
 *   0x02 GET_SYNCPOINT {u32 index, u32 id (out)}   0x03 GET_WAITBASE {u32 index, u32 base (out)}   0x07 SET_SUBMIT_TIMEOUT {u32 ms}
 *   0x09 MAP_CMD_BUFFER / 0x0A UNMAP_CMD_BUFFER {u32 count, u32 data_address, u8 attach_host_ch_das, pad[3]} + {u32 handle, u32 iova (out)}[count]
 * The engines themselves are not emulated: a submit is validated (command buffers must lie inside live nvmap handles) and completes
 * at once, its syncpoint increments land immediately and the fence thresholds are what the guest then waits for. Output surfaces keep
 * whatever the guest had there (video is decoded by movie.c at the media library level instead). */
enum { NV_BAD_VALUE = 0xB, NV_INVALID_STATE = 8 };
#define SYNCPT_FIRST_DYNAMIC 32 /* ids below are the fixed ones (display, GPU) the SDK may poll through nvhost-ctrl */
static uint8_t syncpt_owned[NSYNC];
static uint32_t syncpt_alloc(void) {
    for (uint32_t i = SYNCPT_FIRST_DYNAMIC; i < NSYNC; i++)
        if (!syncpt_owned[i]) {
            syncpt_owned[i] = 1;
            return i;
        }
    return 0;
}
/* Device addresses for MAP_CMD_BUFFER: a bump allocator over the 32-bit host1x space, one stable address per nvmap handle. */
static uint32_t next_iova = 0x10000000u;
static uint32_t nvmap_pin(NvMapHandle *h) {
    if (h->iova) return h->iova;
    uint64_t align = h->align > 0x1000 ? h->align : 0x1000, at = ((uint64_t)next_iova + align - 1) & ~(align - 1),
             end = at + (((uint64_t)h->size + 0xfff) & ~0xfffull);
    if (end > 0xfff00000u) return 0;
    next_iova = (uint32_t)end;
    h->iova = (uint32_t)at;
    return h->iova;
}
static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint32_t channel_submit(Channel *ch, uint8_t *buf, uint32_t size) {
    if (size < 16) return NV_BAD_PARAMETER;
    uint32_t nc = rd32(buf), nrel = rd32(buf + 4), ni = rd32(buf + 8), nf = rd32(buf + 12);
    if (nc > 0x400 || nrel > 0x400 || ni > 0x400 || nf > 0x400) return NV_BAD_PARAMETER;
    uint64_t cmd_off = 16, rel_off = cmd_off + (uint64_t)nc * 12, shift_off = rel_off + (uint64_t)nrel * 16, incr_off = shift_off + (uint64_t)nrel * 4,
             fence_off = incr_off + (uint64_t)ni * 20, end = fence_off + (uint64_t)nf * 4;
    if (end > size) return NV_BAD_PARAMETER;
    for (uint32_t i = 0; i < nc; i++) { /* every command buffer inside a live nvmap handle */
        const uint8_t *e = buf + cmd_off + (uint64_t)i * 12;
        uint32_t handle = rd32(e), offset = rd32(e + 4);
        int32_t words = (int32_t)rd32(e + 8);
        NvMapHandle *h = nvmap_find(handle);
        if (!h || words < 0 || (uint64_t)offset + (uint64_t)words * 4 > h->size) return NV_BAD_PARAMETER;
    }
    for (uint32_t i = 0; i < ni; i++)
        if (rd32(buf + incr_off + (uint64_t)i * 20) >= NSYNC) return NV_BAD_PARAMETER;
    for (uint32_t i = 0; i < ni; i++) {
        const uint8_t *e = buf + incr_off + (uint64_t)i * 20;
        uint32_t id = rd32(e), n = rd32(e + 4);
        syncpt_max[id] += n;
        uint32_t threshold = syncpt_max[id];
        syncpt_advance(id, n); /* the engine is instant: completed as soon as submitted */
        if (i < nf) wr32(buf + fence_off + (uint64_t)i * 4, threshold);
    }
    ch->submits++;
    return NV_OK;
}
static uint32_t channel_map(int unmap, uint8_t *buf, uint32_t size) {
    if (size < 12) return NV_BAD_PARAMETER;
    uint32_t cnt = rd32(buf);
    if (12 + (uint64_t)cnt * 8 > size) return NV_BAD_PARAMETER;
    for (uint32_t i = 0; i < cnt; i++)
        if (!nvmap_find(rd32(buf + 12 + (uint64_t)i * 8))) return NV_BAD_PARAMETER;
    for (uint32_t i = 0; i < cnt; i++) {
        uint8_t *e = buf + 12 + (uint64_t)i * 8;
        NvMapHandle *h = nvmap_find(rd32(e));
        if (unmap) continue; /* the mapping stays: other channels may still use the handle */
        uint32_t iova = nvmap_pin(h);
        if (!iova) return NV_INSUFFICIENT_MEMORY;
        wr32(e + 4, iova);
    }
    return NV_OK;
}
static uint32_t channel_ioctl(Channel *ch, unsigned type, unsigned nr, uint8_t *buf, uint32_t size) {
    pthread_mutex_lock(&lock);
    uint32_t r = NV_NOT_IMPLEMENTED;
    if (type == 0x48) {
        switch (nr) {
            case 0x01:
                if (size < 4) {
                    r = NV_BAD_PARAMETER;
                    break;
                }
                ch->nvmap_fd = (int32_t)rd32(buf);
                r = NV_OK;
                break; /* SET_NVMAP_FD */
            case 0x03:
                if (size < 4) {
                    r = NV_BAD_PARAMETER;
                    break;
                }
                ch->timeout_ms = rd32(buf);
                r = NV_OK;
                break; /* SET_TIMEOUT */
        }
    } else if (type == 0) {
        switch (nr) {
            case 0x01: r = channel_submit(ch, buf, size); break;
            case 0x02:
            case 0x03: { /* GET_SYNCPOINT / GET_WAITBASE */
                if (size < 8) {
                    r = NV_BAD_PARAMETER;
                    break;
                }
                uint32_t index = rd32(buf);
                if (index >= CHANNEL_SYNCPTS) {
                    r = NV_BAD_PARAMETER;
                    break;
                }
                uint32_t v = 0;
                if (nr == 0x02) {
                    if (!ch->syncpt[index]) ch->syncpt[index] = syncpt_alloc();
                    v = ch->syncpt[index];
                    if (!v) {
                        r = NV_INSUFFICIENT_MEMORY;
                        break;
                    }
                }
                wr32(buf + 4, v);
                r = NV_OK;
                break; /* wait bases are not used by the SDK: 0 */
            }
            case 0x07:
                if (size < 4) {
                    r = NV_BAD_PARAMETER;
                    break;
                }
                ch->submit_timeout_ms = rd32(buf);
                r = NV_OK;
                break; /* SET_SUBMIT_TIMEOUT */
            case 0x09:
            case 0x0a: r = channel_map(nr == 0x0a, buf, size); break;
        }
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static void channel_release(Channel *ch) { /* close of the fd: its syncpoints go back to the pool */
    for (unsigned i = 0; i < CHANNEL_SYNCPTS; i++)
        if (ch->syncpt[i]) {
            syncpt_owned[ch->syncpt[i]] = 0;
            ch->syncpt[i] = 0;
        }
}

/* ---- IPC ----------------------------------------------------------------------------------------------------------------------------- */
static void nv_open(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const char *path = b ? ipc_buf_ptr(b) : NULL;
    char p[48] = {0};
    if (path) {
        size_t n = b->size < sizeof p - 1 ? (size_t)b->size : sizeof p - 1;
        memcpy(p, path, n);
    }
    DevKind kind = DEV_NONE;
    uint32_t class_id = 0;
    if (!strcmp(p, "/dev/nvmap")) kind = DEV_NVMAP;
    else if (!strcmp(p, "/dev/nvhost-ctrl")) kind = DEV_CTRL;
    else {
        for (unsigned i = 0; i < sizeof channels / sizeof *channels; i++)
            if (!strcmp(p, channels[i].path)) {
                kind = DEV_CHANNEL;
                class_id = channels[i].class_id;
            }
        for (unsigned i = 0; i < sizeof known_other / sizeof *known_other; i++)
            if (!strcmp(p, known_other[i])) kind = DEV_OTHER;
    }
    uint32_t fd = 0, err = NV_NOT_SUPPORTED;
    if (kind != DEV_NONE) {
        pthread_mutex_lock(&lock);
        err = NV_INSUFFICIENT_MEMORY;
        for (unsigned i = 1; i < MAX_FD; i++)
            if (!fds[i].open) {
                fds[i] = (Fd){.kind = kind, .open = 1, .ch = {.class_id = class_id, .nvmap_fd = -1}};
                snprintf(fds[i].path, sizeof fds[i].path, "%s", p);
                fd = i;
                err = NV_OK;
                break;
            }
        pthread_mutex_unlock(&lock);
    }
    LOGV("nvdrv", "Open(\"%s\") -> fd %u error %u", p, fd, err);
    if (kind == DEV_OTHER) summary_note("nvdrv: opened %s (no GPU channel emulation, ioctls are refused)", p);
    IPC_OUT(c, fd);
    IPC_OUT(c, err);
}
/* Ioctl family: data goes in through the pointer/map input buffer, the same struct comes back through the output buffer. */
static void do_ioctl(IpcCall *c, unsigned in_idx, unsigned out_idx) {
    uint32_t fd = IPC_IN(c, uint32_t, 0), cmd = IPC_IN(c, uint32_t, 4);
    const IpcBuf *in = ipc_in_buf(c, in_idx), *out = ipc_out_buf(c, out_idx);
    unsigned nr = cmd & 0xff, type = (cmd >> 8) & 0xff, size = (cmd >> 16) & 0x3fff;
    uint8_t tmp[0x4000] = {0};
    uint32_t err = NV_NOT_IMPLEMENTED;
    if (in && in->size > size && in->size <= sizeof tmp) size = (unsigned)in->size; /* channel Submit carries arrays beyond the nominal struct */
    if (fd >= MAX_FD || !fds[fd].open) {
        LOGV("nvdrv", "ioctl %#x on closed fd %u", cmd, fd);
        err = NV_BAD_PARAMETER;
        goto done;
    }
    if (size > sizeof tmp) {
        err = NV_BAD_PARAMETER;
        goto done;
    }
    if (in && in->size) {
        const void *src = ipc_buf_ptr(in);
        if (src) memcpy(tmp, src, in->size < size ? (size_t)in->size : size);
    }
    switch (fds[fd].kind) {
        case DEV_NVMAP: err = type == 1 ? nvmap_ioctl(nr, tmp, size) : NV_NOT_IMPLEMENTED; break;
        case DEV_CTRL: err = type == 0 ? ctrl_ioctl(nr, tmp, size) : NV_NOT_IMPLEMENTED; break;
        case DEV_CHANNEL: err = channel_ioctl(&fds[fd].ch, type, nr, tmp, size); break;
        default: break;
    }
    if (out) ipc_write_buf(c, out, tmp, out->size < size ? out->size : size);
done:
    LOGV("nvdrv", "ioctl fd %u (%s) cmd %#x (type %#x nr %#x size %#x) -> %u", fd, fd < MAX_FD ? fds[fd].path : "?", cmd, type, nr, size, err);
    if (log_verbose && fd < MAX_FD && fds[fd].kind != DEV_NVMAP) {
        char hex[260];
        unsigned n = size < 64 ? size : 64;
        for (unsigned i = 0; i < n; i++) snprintf(hex + i * 3, 4, "%02x ", tmp[i]);
        LOG("nvdrv", "   data %s", hex);
    }
    if (err == NV_NOT_IMPLEMENTED) {
        char b[96];
        snprintf(b, sizeof b, "nvdrv:ioctl %s %#x", fd < MAX_FD ? fds[fd].path : "?", cmd);
        summary_ipc(b, 0, 2, 0);
        LOG("nvdrv", "unimplemented ioctl %#x on %s", cmd, fd < MAX_FD ? fds[fd].path : "?");
    }
    IPC_OUT(c, err);
}
static void nv_ioctl(IpcObject *s, IpcCall *c) {
    (void)s;
    do_ioctl(c, 0, 0);
}
static void nv_ioctl2(IpcObject *s, IpcCall *c) {
    (void)s;
    do_ioctl(c, 0, 0);
} /* extra input buffer is ignored (only ioctls on devices we refuse use it) */
static void nv_ioctl3(IpcObject *s, IpcCall *c) {
    (void)s;
    do_ioctl(c, 0, 0);
}
static void nv_close(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t fd = IPC_IN(c, uint32_t, 0);
    uint32_t err = NV_BAD_PARAMETER;
    pthread_mutex_lock(&lock);
    if (fd < MAX_FD && fds[fd].open) {
        if (fds[fd].kind == DEV_CHANNEL) channel_release(&fds[fd].ch);
        fds[fd].open = 0;
        err = NV_OK;
    }
    pthread_mutex_unlock(&lock);
    IPC_OUT(c, err);
}
static void nv_initialize(IpcObject *s, IpcCall *c) {
    (void)s;
    initialized = 1;
    uint32_t err = NV_OK;
    IPC_OUT(c, err);
}
static void nv_query_event(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t fd = IPC_IN(c, uint32_t, 0), eid = IPC_IN(c, uint32_t, 4);
    pthread_mutex_lock(&lock);
    NvEvent *e = NULL;
    for (unsigned i = 0; i < MAX_SLOTS; i++)
        if (events[i].used && events[i].event_id == eid) e = &events[i];
    if (!e)
        for (unsigned i = 0; i < MAX_SLOTS; i++)
            if (!events[i].used) {
                e = &events[i];
                e->used = 1;
                e->event_id = eid;
                e->writable = event_create(&e->readable);
                break;
            }
    pthread_mutex_unlock(&lock);
    LOGV("nvdrv", "QueryEvent(fd %u, event %#x) -> %s", fd, eid, e ? "ok" : "no slot");
    uint32_t err = e ? NV_OK : NV_INSUFFICIENT_MEMORY;
    if (e) ipc_out_copy_handle(c, e->readable);
    IPC_OUT(c, err);
}
static void nv_set_aruid(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t err = NV_OK;
    IPC_OUT(c, err);
}
static void nv_noop(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static void nv_cmd13(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static void nv_status(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_zero(c, 0x20);
}
static const IpcCmd nv_cmds[] = {
    {0, "Open", nv_open, IPC_IMPL},
    {1, "Ioctl", nv_ioctl, IPC_IMPL},
    {2, "Close", nv_close, IPC_IMPL},
    {3, "Initialize", nv_initialize, IPC_IMPL},
    {4, "QueryEvent", nv_query_event, IPC_STUB},
    {5, "MapSharedMemory", nv_set_aruid, IPC_STUB},
    {6, "GetStatus", nv_status, IPC_STUB},
    {7, "SetAruid", nv_set_aruid, IPC_STUB},
    {8, "DumpGraphicsMemoryInfo", nv_noop, IPC_STUB},
    {9, "InitializeDevtools", nv_set_aruid, IPC_STUB},
    {10, "Ioctl2", nv_ioctl2, IPC_IMPL},
    {11, "Ioctl3", nv_ioctl3, IPC_IMPL},
    {12, "SetGraphicsFirmwareMemoryMarginEnabled", nv_noop, IPC_STUB},
    {13, "Unknown13", nv_cmd13, IPC_STUB},
};
static IpcObject *nv_new(void) { return IPC_OBJECT("nvdrv", nv_cmds, NULL, NULL); }
void service_register_nvdrv(void) {
    ipc_register_service("nvdrv", nv_new);
    ipc_register_service("nvdrv:a", nv_new);
    ipc_register_service("nvdrv:s", nv_new);
    ipc_register_service("nvdrv:t", nv_new);
}
