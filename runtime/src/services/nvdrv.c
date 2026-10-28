/* nvdrv / nvdrv:a / nvdrv:s / nvdrv:t: the Tegra driver front end the SDK's NvRm layer talks to (nv::InitializeGraphics, android:: gralloc
 * buffers of the vi native window, media). Napple's NVN is implemented by the graphics back end, so the GPU channels, address spaces and
 * syncpoints of the real driver are not needed: this service answers the *memory* side (nvmap handle bookkeeping on guest-provided memory) and
 * the control side (syncpoint counters, event objects) at the level the SDK initializes and polls, the host1x channels of the multimedia
 * engines (nvhost-nvdec/vic/nvjpg/msenc: the channel ioctls; a submit's command buffers are parsed as host1x streams and run on the register-level
 * engines of nvdrv_media_core, decode work going to the installed DecoderBackend; submits complete at once), and refuses everything else by number
 * (NotImplemented) so a game that really needs a device reports it in the log and the compat summary. See docs/architecture/runtime-services.md. */
#include "../ipc.h"
#include "nvdrv_media_core.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
typedef enum { DEV_NONE, DEV_NVMAP, DEV_CTRL, DEV_CHANNEL, DEV_DISP, DEV_OTHER } DevKind;
/* host1x client channel state (nvhost-nvdec / vic / nvjpg / msenc) */
#define CHANNEL_SYNCPTS 16
typedef struct {
    uint32_t class_id;
    int32_t nvmap_fd;
    uint32_t timeout_ms, submit_timeout_ms;
    uint32_t syncpt[CHANNEL_SYNCPTS];
    uint64_t submits;
    MediaEngine *eng; /* register-level engine behind the channel (nvdrv_media_core) */
} Channel;
typedef struct {
    DevKind kind;
    char path[48];
    int open;
    Channel ch;
    NvDisp disp; /* DEV_DISP: /dev/nvdisp-disp0 / disp1 */
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

static const char *const known_other[] = {"/dev/nvhost-gpu",  "/dev/nvhost-as-gpu", "/dev/nvhost-ctrl-gpu", "/dev/nvhost-dbg-gpu", "/dev/nvhost-prof-gpu",
                                          "/dev/nvhost-tsec", "/dev/nvdisp-ctrl",   "/dev/nvsched-ctrl",    "/dev/nvsched"};
/* Display controller devices: the vblank syncpoint ids are those of the older Tegra host1x sources (NVSYNCPT_VBLANK0/1 = 26/27; unverified for Tegra X1). */
static const struct {
    const char *path;
    uint32_t vblank_syncpt;
} displays[] = {{"/dev/nvdisp-disp0", 26}, {"/dev/nvdisp-disp1", 27}};
static uint64_t disp_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
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
            if (media_trace()) fprintf(stderr, "[nvmap] alloc id %u size %#x addr %#llx heap %#x flags %#x align %#x kind %u\n", id, h->size, (unsigned long long)addr, heap,
                                       flags, align, buf[16]);
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
 * A submit is validated (command buffers must lie inside live nvmap handles), the command buffers that have a mapped address are read
 * (relocations applied to a copy), run as host1x streams on the channel's engine (nvdrv_media_core: NVDEC to the DecoderBackend, VIC colour
 * conversion, NVJPG decode through jpeg_core, MSENC accepted) and the submit completes at once: its syncpoint increments land immediately and
 * the fence thresholds are what the guest then waits for. A malformed stream or an unmapped gather fails the submit like the kernel's
 * firewall; a failed decode is reported in the engine's status structure only. movie.c decodes only the movie formats it replaces at the
 * media-library level (the ones it hooks); any other decoder the title's own library drives (MK8D's 512x608 AVC preview video, for one) comes here
 * as nvhost-nvdec + nvhost-vic submits. Everything runs under the nvdrv lock. */
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
    if (media_trace()) fprintf(stderr, "[nvmap] pin id %u addr %#llx size %#x -> iova %#x\n", h->id, (unsigned long long)h->addr, h->size, h->iova);
    return h->iova;
}
/* Device address -> host pointer for the media engines: the nvmap handle whose pinned range covers it (guest addresses are host addresses here). */
static uint8_t *nv_resolve(void *ctx, uint64_t iova, size_t *avail) {
    (void)ctx;
    for (unsigned i = 0; i < MAX_NVMAP; i++) {
        NvMapHandle *h = &nvmap[i];
        if (!h->used || !h->iova || !h->addr || iova < h->iova || iova >= (uint64_t)h->iova + h->size) continue;
        *avail = (size_t)((uint64_t)h->iova + h->size - iova);
        uint8_t *p = guest_ptr(h->addr + (iova - h->iova), *avail); /* NULL when the guest gave an address that is not mapped */
        if (!p && media_trace()) {
            static int said;
            if (said++ < 20) fprintf(stderr, "[nvmap] iova %#llx -> handle %u addr %#llx: not guest memory\n", (unsigned long long)iova, h->id, (unsigned long long)h->addr);
        }
        return p;
    }
    if (media_trace()) {
        static int said;
        if (said++ < 40) fprintf(stderr, "[nvmap] resolve miss iova %#llx\n", (unsigned long long)iova);
    }
    return NULL;
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
    for (uint32_t i = 0; ch->eng && i < nc; i++) { /* run the command buffers on the engine */
        const uint8_t *e = buf + cmd_off + (uint64_t)i * 12;
        NvMapHandle *h = nvmap_find(rd32(e));
        uint32_t offset = rd32(e + 4), words = rd32(e + 8);
        const uint8_t *src = h->addr && words && !(offset & 3) ? guest_ptr(h->addr + offset, (uint64_t)words * 4) : NULL;
        if (!src) continue; /* no readable memory behind the handle (or nothing in it): accepted as before */
        uint32_t *w = malloc((size_t)words * 4);
        if (!w) return NV_INSUFFICIENT_MEMORY;
        memcpy(w, src, (size_t)words * 4);
        for (uint32_t r = 0; r < nrel; r++) { /* relocations: the word at (cmdbuf, offset) becomes (target iova + target offset) >> shift */
            const uint8_t *re = buf + rel_off + (uint64_t)r * 16;
            uint32_t at = rd32(re + 4), toff = rd32(re + 12), shift = rd32(buf + shift_off + (uint64_t)r * 4);
            if (rd32(re) != h->id || at < offset || at >= offset + words * 4 || at & 3) continue;
            NvMapHandle *t = nvmap_find(rd32(re + 8));
            uint32_t iova = t ? nvmap_pin(t) : 0;
            if (!t || !iova || shift > 31) {
                free(w);
                return t && !iova ? NV_INSUFFICIENT_MEMORY : NV_BAD_PARAMETER;
            }
            w[(at - offset) / 4] = (uint32_t)(((uint64_t)iova + toff) >> shift);
            if (media_trace()) {
                static int said;
                if (said++ < 64)
                    fprintf(stderr, "[nvdrv] class %#x reloc word %#x -> handle %u (+%#x) >> %u = %#x\n", ch->class_id, (at - offset) / 4, t->id, toff, shift,
                            w[(at - offset) / 4]);
            }
        }
        int rc = media_engine_run(ch->eng, w, words);
        free(w);
        if (rc) {
            LOG("nvdrv", "%s: command buffer %u rejected (%s)", "channel", i, rc == MEDIA_BAD_ADDRESS ? "unmapped gather" : "malformed host1x stream");
            return NV_BAD_PARAMETER;
        }
    }
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
    if (ch->eng) {
        const MediaStats *st = media_engine_stats(ch->eng);
        LOGV("nvdrv", "engine class %#x closed: %llu executes, %llu frames, %llu errors, %llu unknown register writes", media_engine_class(ch->eng),
             (unsigned long long)st->executes, (unsigned long long)st->frames, (unsigned long long)st->errors, (unsigned long long)st->unknown_regs);
        if (st->unknown_regs)
            summary_note("nvdrv: media engine %#x: %llu writes to registers outside the known map (last %#x)", media_engine_class(ch->eng),
                         (unsigned long long)st->unknown_regs, st->last_unknown_reg);
        if (st->errors) /* 1 codec refused by the backend (VP9 on VideoToolbox), 2 unmapped surface, 3 decode failed, 4 bad parameters / surface too small */
            summary_note("nvdrv: media engine %#x: %llu of %llu executes failed (last error %u)", media_engine_class(ch->eng), (unsigned long long)st->errors,
                         (unsigned long long)st->executes, st->last_error);
        media_engine_free(ch->eng);
        ch->eng = NULL;
    }
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
    uint32_t class_id = 0, disp_syncpt = 0;
    if (!strcmp(p, "/dev/nvmap")) kind = DEV_NVMAP;
    else if (!strcmp(p, "/dev/nvhost-ctrl")) kind = DEV_CTRL;
    else {
        for (unsigned i = 0; i < sizeof displays / sizeof *displays; i++)
            if (!strcmp(p, displays[i].path)) {
                kind = DEV_DISP;
                disp_syncpt = displays[i].vblank_syncpt;
            }
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
                static const NvMediaMem mem = {nv_resolve, NULL};
                fds[i] = (Fd){.kind = kind, .open = 1, .ch = {.class_id = class_id, .nvmap_fd = -1}};
                if (kind == DEV_CHANNEL) fds[i].ch.eng = media_engine_new(class_id, &mem);
                if (kind == DEV_DISP) {
                    nvdisp_init(&fds[i].disp, disp_now_ns(), 0);
                    fds[i].disp.syncpt = disp_syncpt;
                }
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
/* /dev/nvhost-ctrl-gpu: what the Switch's GM20B (Maxwell, Tegra X1) reports. Games' NVN is the host's, so none asks; the newer SDK that firmware applets are built with
 * queries it itself while it sets up its graphics. GET_CHARACTERISTICS (type 'G', nr 5) is {u64 buffer size, u64 buffer address, 0xa0 bytes of characteristics}
 * on the Switch; the field order is the nvgpu uapi's, values those of the retail console. */
static uint32_t ctrl_gpu_ioctl(unsigned nr, uint8_t *data, unsigned size) {
    if (nr == 5 && size >= 0xb0) {
        uint8_t *c = data + 0x10;
        memset(c, 0, 0xa0);
        uint32_t w[] = {0x120, 0xb, 0xa1, 1}; /* arch GM200, impl GM20B, revision A1, one GPC */
        memcpy(c, w, sizeof w);
        uint64_t l2 = 0x40000, flags = 0x55;
        memcpy(c + 0x10, &l2, 8);
        memcpy(c + 0x38, &flags, 8);
        uint32_t t[] = {2, 0x20, 0x20000, 0x20000, 0x1b, 0x30000}; /* TPC per GPC, AXI bus, big page 128K, compression page 128K, PDE bits, big page sizes */
        memcpy(c + 0x20, t, sizeof t);
        uint32_t cls[] = {0x902d, 0xb197, 0xb1c0, 0xb06f, 0xa140,
                          0xb0b5, 1,      0x503,  0x503,  0x20}; /* 2D, 3D, compute, gpfifo, i2m, copy; gpc mask, SM arch, warps */
        memcpy(c + 0x40, cls, sizeof cls);
        uint64_t chip = 0x6230326d67; /* "gm20b" */
        memcpy(c + 0x88, &chip, 8);
        uint64_t bs = 0xa0;
        memcpy(data, &bs, 8);
        return NV_OK;
    }
    LOG("nvdrv", "ctrl-gpu: ioctl nr %#x (size %#x) is not implemented", nr, size);
    return NV_NOT_IMPLEMENTED;
}
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
        case DEV_DISP:
            err = type == NVDISP_IOCTL_TYPE ? nvdisp_ioctl(&fds[fd].disp, nr, tmp, size, disp_now_ns()) : NV_NOT_IMPLEMENTED;
            if (err == NVDISP_BAD_PARAMETER) err = NV_BAD_PARAMETER;
            break;
        default:
            if (!strcmp(fds[fd].path, "/dev/nvhost-ctrl-gpu") && type == 0x47) err = ctrl_gpu_ioctl(nr, tmp, size);
            break;
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
/* DumpGraphicsMemoryInfo: the console writes its graphics memory table to the debug log; here the nvmap bookkeeping goes to the verbose log. No reply data. */
static void nv_dump_memory(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    uint64_t bytes = 0;
    unsigned live = 0;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < MAX_NVMAP; i++)
        if (nvmap[i].used) {
            live++;
            bytes += nvmap[i].size;
        }
    pthread_mutex_unlock(&lock);
    LOGV("nvdrv", "DumpGraphicsMemoryInfo: %u nvmap handles, %llu bytes", live, (unsigned long long)bytes);
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
    {4, "QueryEvent", nv_query_event, IPC_IMPL},
    {5, "MapSharedMemory", nv_set_aruid, IPC_IMPL}, /* nvmap works on the guest's own memory: there is no transfer memory to map, success is the whole answer */
    {6, "GetStatus", nv_status, IPC_IMPL}, /* the host has no graphics memory budget: an all-zero status ("nothing in use / no pressure") is the true answer */
    {7, "SetAruid", nv_set_aruid, IPC_IMPL}, /* one process, one applet resource user id: nothing to remember */
    {8, "DumpGraphicsMemoryInfo", nv_dump_memory, IPC_IMPL},
    {9, "InitializeDevtools", nv_set_aruid, IPC_IMPL}, /* the devtools backend does not exist on the host: accepted, there is nothing to initialise */
    {10, "Ioctl2", nv_ioctl2, IPC_IMPL},
    {11, "Ioctl3", nv_ioctl3, IPC_IMPL},
    {12, "Ioctl3", nv_ioctl3, IPC_IMPL},
    {13, "Unknown13", nv_cmd13, IPC_STUB}, /* no public description of the command (reply-less on the console builds seen so far); stays a stub on purpose */
};
static IpcObject *nv_new(void) { return IPC_OBJECT("nvdrv", nv_cmds, NULL, NULL); }
void service_register_nvdrv(void) {
    ipc_register_service("nvdrv", nv_new);
    ipc_register_service("nvdrv:a", nv_new);
    ipc_register_service("nvdrv:s", nv_new);
    ipc_register_service("nvdrv:t", nv_new);
}
