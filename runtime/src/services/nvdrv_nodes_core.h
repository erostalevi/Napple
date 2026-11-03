/* nvdrv_nodes_core: state-tracked ioctl surface of the GPU device nodes /dev/nvhost-gpu (channel), /dev/nvhost-as-gpu (address space) and
 * /dev/nvhost-ctrl-gpu. NVN is implemented by the graphics back end, so no channel executes: the nodes keep the *state* a driver client can observe
 * (what was allocated, bound, mapped, in which order) and answer with consistent, sensible values. Portable C; nvdrv.c supplies the nvmap and syncpoint
 * services through NvGpuEnv and owns the fd table. Error codes are the NvError values of nvdrv.c (passed as NVG_*, equal by construction).
 *
 * Layouts (2026-11-02, r10): every ioctl number, struct size and field offset below was checked against public sources, used as layout facts only:
 *   [libnx]  switchbrew/libnx nx/source/nvidia/ioctl/{nvchannel,nvhost-as-gpu,nvhost-ctrl-gpu}.c + nx/include/switch/nvidia/ioctl.h (ISC; the Switch
 *            numbering and struct sizes, exercised on hardware by homebrew)
 *   [nvgpu]  NVIDIA L4T r32.6.1 nvgpu include/uapi/linux/nvgpu.h (field meaning, flag bits, constants)
 * The full table with per-row sources and status is docs/reference/nvdrv-layouts.md; tests pin each struct (runtime/tests/test_svc_nvdrv_nodes.c).
 *
 * nvhost-gpu type 'H' (0x48):
 *   0x01 SET_NVMAP_FD{s32}(4)  0x03 SET_TIMEOUT{u32 ms}(4)                                                                         [libnx]
 *   0x08 SUBMIT_GPFIFO{u64 gpfifo,u32 num_entries,u32 flags,fence{u32 id,u32 value}}(0x18 + num_entries x u64 inline)              [libnx][nvgpu]
 *   0x09 ALLOC_OBJ_CTX{u32 class,u32 flags,u64 obj_id out}(0x10)  0x0B ZCULL_BIND{u64 gpu_va,u32 mode,u32 pad}(0x10)              [libnx]
 *   0x0C SET_ERROR_NOTIFIER{u64 offset,u64 size,u32 mem/enable,u32 pad}(0x18)  0x0D SET_PRIORITY{u32 50|100|150}(4)                [libnx]
 *   0x0E ENABLE  0x0F DISABLE  0x10 PREEMPT  0x11 FORCE_RESET (no data)  0x12 EVENT_ID_CONTROL{u32 cmd,u32 event_id}(8)              [nvgpu, nr-99 rule]
 *   0x16 GET_ERROR_INFO{u32 type,u32 info[31]}(0x80 out)  0x17 GET_ERROR_NOTIFICATION{u64 timestamp,u32 info32,u16 info16,u16 status}(0x10)  [libnx]
 *   0x18 ALLOC_GPFIFO_EX{u32 num_entries,u32 num_inflight_jobs,u32 flags,u32 reserved[5]}(0x20)                                      [nvgpu, nr-99 rule]
 *   0x1A ALLOC_GPFIFO_EX2{u32 num_entries,u32 flags,u32 unk0,fence{u32 id,u32 value} out,u32 unk1..3}(0x20)                          [libnx]
 *   0x1B KICKOFF_PB{u64 gpfifo,u32 num_entries,u32 flags,fence}(0x18; entries in the second Ioctl2 buffer)                          [libnx]
 *   The "nr-99 rule": Switch nr = L4T nvgpu nr - 99 for every channel ioctl libnx confirms in that range (107..111 -> 0x08..0x0C); it is applied to
 *   113..117 (ENABLE..EVENT_ID_CTRL) and 123 (ALLOC_GPFIFO_EX). These numbers are derived, not confirmed by a Switch source (see the layout table).
 * nvhost-gpu type 'G' (0x47) on the channel fd: 0x14 SET_USER_DATA{u64}(8)  0x15 GET_USER_DATA{u64}(8)                              [libnx: _NV_IOW(0x47,0x14)]
 *   (nvgpu_chan_ioctl_typed; nvdrv.c must route type 'G' on a gpu channel fd to it, see docs/reference/nvdrv-layouts.md)
 * nvhost-as-gpu type 'A' (0x41):
 *   0x01 BIND_CHANNEL{s32 fd}(4)  0x02 ALLOC_SPACE{u32 pages,u32 page_size,u32 flags,u32 pad,u64 offset|align}(0x18)               [libnx]
 *   0x03 FREE_SPACE{u64 offset,u32 pages,u32 page_size}(0x10)  0x05 UNMAP_BUFFER{u64 offset}(8)                                     [libnx]
 *   0x06 MAP_BUFFER_EX{u32 flags,s32 kind,u32 nvmap_handle,u32 page_size inout,u64 buffer_offset,u64 mapping_size,u64 offset inout}(0x28)  [libnx]
 *   0x08 GET_VA_REGIONS{u64 unused,u32 buf_size inout,u32 pad,region{u64 offset,u32 page_size,u32 pad,u64 pages}[2]}(0x40)          [libnx][nvgpu]
 *   0x09 INITIALIZE_EX{u32 flags,s32 as_fd,u32 big_page_size,u32 reserved,u64 va_start,u64 va_end,u64 va_split}(0x28)              [libnx order; va words per nvgpu]
 * nvhost-ctrl-gpu type 'G' (0x47):
 *   0x01 ZCULL_GET_CTX_SIZE{u32}(4)  0x02 ZCULL_GET_INFO{10 x u32}(0x28)  0x03 ZBC_SET_TABLE{u32 ds[4],l2[4],depth,format,type}(0x2C) [libnx]
 *   0x04 ZBC_QUERY_TABLE{u32 ds[4],l2[4],depth,ref_cnt,format,type,index_size}(0x34)                                                 [libnx][nvgpu]
 *   0x05 GET_CHARACTERISTICS{u64 buf_size,u64 buf_addr,characteristics(0xA0)}(0xB0)  0x06 GET_TPC_MASKS{u32 size,u32,u64 addr,u64 mask}(0x18) [libnx]
 *   0x07 FLUSH_L2{u32 flush,u32 invalidate}(8)  0x14 ZBC_GET_ACTIVE_SLOT_MASK{u32 slot,u32 mask}(8)  0x1C GET_GPU_TIME{u64 ns,u64 reserved}(0x10) [libnx]
 *   branch r11 additions [unverified, proposed from memory by the cloud session]: nvhost-gpu 0x1D EVENT_ID_CTRL{u32 cmd 0 disable|1 enable|2 clear,u32 event_id 0..2,s32 fd,u32}(0x10)
 *   (shares the enable mask of 0x12; clear keeps the mask)  nvhost-ctrl-gpu 0x0C GET_TPC_EXCEPTION_EN_STATUS{u64 mask out}(8)  0x0D NUM_VSMS{u32 num_vsms out,u32}(8)
 * Anything else: NVG_NOT_IMPLEMENTED (nvdrv.c logs it in the compat summary). A short buffer is NVG_BAD_PARAMETER. Calls arrive under the nvdrv lock. */
#ifndef NVDRV_NODES_CORE_H
#define NVDRV_NODES_CORE_H
#include <stdint.h>

enum { NVG_OK = 0, NVG_NOT_IMPLEMENTED = 1, NVG_BAD_PARAMETER = 4, NVG_INVALID_STATE = 8, NVG_INSUFFICIENT_MEMORY = 6, NVG_ALREADY_ALLOCATED = 13 };
enum { NVG_TYPE_CHANNEL = 0x48, NVG_TYPE_AS = 0x41, NVG_TYPE_CTRL = 0x47, NVG_TYPE_GPU = 0x47 /* 'G': user data on the channel fd */ };

/* Struct sizes (the size field of the ioctl command word). Tests pin them; nvdrv.c passes the size the guest encoded. */
enum {
    NVG_SZ_SET_NVMAP_FD = 4, NVG_SZ_SET_TIMEOUT = 4, NVG_SZ_SUBMIT_GPFIFO = 0x18, NVG_SZ_ALLOC_OBJ_CTX = 0x10, NVG_SZ_ZCULL_BIND = 0x10,
    NVG_SZ_SET_ERROR_NOTIFIER = 0x18, NVG_SZ_SET_PRIORITY = 4, NVG_SZ_EVENT_ID_CONTROL = 8, NVG_SZ_GET_ERROR_INFO = 0x80, NVG_SZ_GET_ERROR_NOTIFICATION = 0x10,
    NVG_SZ_ALLOC_GPFIFO_EX = 0x20, NVG_SZ_ALLOC_GPFIFO_EX2 = 0x20, NVG_SZ_KICKOFF_PB = 0x18, NVG_SZ_USER_DATA = 8,
    NVG_SZ_BIND_CHANNEL = 4, NVG_SZ_ALLOC_SPACE = 0x18, NVG_SZ_FREE_SPACE = 0x10, NVG_SZ_UNMAP_BUFFER = 8, NVG_SZ_MAP_BUFFER_EX = 0x28,
    NVG_SZ_GET_VA_REGIONS = 0x40, NVG_SZ_INITIALIZE_EX = 0x28,
    NVG_SZ_ZCULL_GET_CTX_SIZE = 4, NVG_SZ_ZCULL_GET_INFO = 0x28, NVG_SZ_ZBC_SET_TABLE = 0x2c, NVG_SZ_ZBC_QUERY_TABLE = 0x34, NVG_SZ_GET_CHARACTERISTICS = 0xb0,
    NVG_SZ_CHARACTERISTICS = 0xa0, NVG_SZ_GET_TPC_MASKS = 0x18, NVG_SZ_FLUSH_L2 = 8, NVG_SZ_ZBC_GET_ACTIVE_SLOT_MASK = 8, NVG_SZ_GET_GPU_TIME = 0x10,
};
/* Channel ioctl numbers (type 'H') */
enum {
    NVG_CH_SET_NVMAP_FD = 0x01, NVG_CH_SET_TIMEOUT = 0x03, NVG_CH_SUBMIT_GPFIFO = 0x08, NVG_CH_ALLOC_OBJ_CTX = 0x09, NVG_CH_ZCULL_BIND = 0x0b,
    NVG_CH_SET_ERROR_NOTIFIER = 0x0c, NVG_CH_SET_PRIORITY = 0x0d, NVG_CH_ENABLE = 0x0e, NVG_CH_DISABLE = 0x0f, NVG_CH_PREEMPT = 0x10, NVG_CH_FORCE_RESET = 0x11,
    NVG_CH_EVENT_ID_CONTROL = 0x12, NVG_CH_GET_ERROR_INFO = 0x16, NVG_CH_GET_ERROR_NOTIFICATION = 0x17, NVG_CH_ALLOC_GPFIFO_EX = 0x18,
    NVG_CH_ALLOC_GPFIFO_EX2 = 0x1a, NVG_CH_KICKOFF_PB = 0x1b,
    NVG_GPU_SET_USER_DATA = 0x14, NVG_GPU_GET_USER_DATA = 0x15 /* type 'G' */
};
/* Address-space ioctl numbers (type 'A') */
enum { NVG_AS_BIND_CHANNEL = 0x01, NVG_AS_ALLOC_SPACE = 0x02, NVG_AS_FREE_SPACE = 0x03, NVG_AS_UNMAP_BUFFER = 0x05, NVG_AS_MAP_BUFFER_EX = 0x06,
       NVG_AS_GET_VA_REGIONS = 0x08, NVG_AS_INITIALIZE_EX = 0x09 };
/* ctrl-gpu ioctl numbers (type 'G') */
enum { NVG_CTRL_ZCULL_GET_CTX_SIZE = 0x01, NVG_CTRL_ZCULL_GET_INFO = 0x02, NVG_CTRL_ZBC_SET_TABLE = 0x03, NVG_CTRL_ZBC_QUERY_TABLE = 0x04,
       NVG_CTRL_GET_CHARACTERISTICS = 0x05, NVG_CTRL_GET_TPC_MASKS = 0x06, NVG_CTRL_FLUSH_L2 = 0x07, NVG_CTRL_GET_TPC_EXCEPTION_EN_STATUS = 0x0c, NVG_CTRL_NUM_VSMS = 0x0d, NVG_CTRL_ZBC_GET_ACTIVE_SLOT_MASK = 0x14,
       NVG_CTRL_GET_GPU_TIME = 0x1c };
/* Flag bits [nvgpu]/[libnx] */
enum { NVG_SUBMIT_FENCE_WAIT = 1, NVG_SUBMIT_FENCE_GET = 2, NVG_SUBMIT_HW_FORMAT = 4 };
enum { NVG_AS_ALLOC_FIXED = 1, NVG_AS_ALLOC_SPARSE = 2 };
enum { NVG_AS_MAP_FIXED = 1, NVG_AS_MAP_CACHEABLE = 4, NVG_AS_MAP_MODIFY = 0x100 };
enum { NVG_EVENT_ID_CMD_ENABLE = 1, NVG_EVENT_ID_MAX = 6 };

/* Services of nvdrv.c. nvmap_size returns the size of a live nvmap handle, 0 when there is none. syncpt_incr bumps a syncpoint's max and value by one and
 * returns the new value. Either may be NULL in a test that does not need it (then handles are accepted at size 0x10000000 / the value is counted locally). */
typedef struct {
    void *ctx;
    uint32_t (*nvmap_size)(void *ctx, uint32_t handle);
    uint32_t (*syncpt_incr)(void *ctx, uint32_t id);
} NvGpuEnv;

#define NVG_MAX_CLASSES 8
typedef struct NvGpuAs NvGpuAs;
typedef struct {
    int32_t nvmap_fd;
    uint32_t timeout_ms, priority;
    uint64_t user_data, err_off, err_size, zcull_va;
    uint32_t err_mem, zcull_mode;
    NvGpuAs *as; /* the address space it is bound to */
    uint32_t gpfifo_entries, gpfifo_flags, gpfifo_inflight;
    uint32_t fence_id, fence_value; /* the channel's syncpoint (assigned by nvdrv.c at open) */
    uint32_t classes[NVG_MAX_CLASSES], nclasses;
    uint64_t next_obj_id, submits, entries_submitted, kickoffs, fence_waits;
    int enabled;                    /* ENABLE / DISABLE; a disabled channel refuses submits (InvalidState) */
    uint32_t preempts, resets, events; /* PREEMPT / FORCE_RESET counts, EVENT_ID_CONTROL enabled-event mask */
} NvGpuChan;

#define NVG_MAX_ALLOCS 32
#define NVG_MAX_MAPS 128
typedef struct {
    uint64_t offset;
    uint32_t pages, page_size, flags;
    int used;
} NvAsAlloc;
typedef struct {
    uint64_t offset, size, buf_off;
    uint32_t handle, page_size, kind, flags;
    int used;
} NvAsMap;
struct NvGpuAs {
    int initialized;
    uint32_t big_page_size, flags;
    uint64_t va_start, va_split, va_end, cursor;
    NvAsAlloc allocs[NVG_MAX_ALLOCS];
    NvAsMap maps[NVG_MAX_MAPS];
    unsigned bound; /* channels bound */
};

#define NVG_ZBC_MAX 16
typedef struct {
    uint32_t color_ds[4], color_l2[4], depth, format;
    int used;
} NvZbcEntry;
typedef struct {
    NvZbcEntry color[NVG_ZBC_MAX], depth[NVG_ZBC_MAX];
    uint64_t t0_ns, last_time_ns;
    uint32_t l2_flushes;
} NvGpuCtrl;

extern const uint32_t nvgpu_classes[6]; /* 2D, 3D, compute, gpfifo, inline-to-memory, copy: what GET_CHARACTERISTICS lists and ALLOC_OBJ_CTX accepts */

void nvgpu_chan_init(NvGpuChan *c, uint32_t fence_id);
void nvgpu_as_init(NvGpuAs *a);
void nvgpu_ctrl_init(NvGpuCtrl *c, uint64_t now_ns);
/* AS side of BIND_CHANNEL once nvdrv.c resolved the channel fd (BadParameter when already bound). Also used by nvdrv.c to unbind on close. */
int nvgpu_as_bind(NvGpuAs *a, NvGpuChan *c);
void nvgpu_as_unbind(NvGpuChan *c);

/* Channel ioctls of type 'H'. */
int nvgpu_chan_ioctl(NvGpuChan *c, const NvGpuEnv *env, unsigned nr, uint8_t *data, unsigned size);
/* Every channel ioctl by type: 'H' as nvgpu_chan_ioctl, 'G' the user-data pair; anything else NVG_NOT_IMPLEMENTED. */
int nvgpu_chan_ioctl_typed(NvGpuChan *c, const NvGpuEnv *env, unsigned type, unsigned nr, uint8_t *data, unsigned size);
/* as-gpu: BIND_CHANNEL needs `chan`, the channel behind the fd in the request (nvdrv.c resolves it; NULL when the fd is not a gpu channel). */
int nvgpu_as_ioctl(NvGpuAs *a, const NvGpuEnv *env, NvGpuChan *chan, unsigned nr, uint8_t *data, unsigned size);
int nvgpu_ctrl_ioctl(NvGpuCtrl *c, unsigned nr, uint8_t *data, unsigned size, uint64_t now_ns);
#endif
