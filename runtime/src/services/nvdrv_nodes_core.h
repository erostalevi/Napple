/* nvdrv_nodes_core: state-tracked ioctl surface of the GPU device nodes /dev/nvhost-gpu (channel), /dev/nvhost-as-gpu (address space) and
 * /dev/nvhost-ctrl-gpu. NVN is implemented by the graphics back end, so no channel executes: the nodes keep the *state* a driver client can observe
 * (what was allocated, bound, mapped, in which order) and answer with consistent, sensible values. Portable C; nvdrv.c supplies the nvmap and syncpoint
 * services through NvGpuEnv and owns the fd table. Layouts follow the public nvgpu uapi / switchbrew as remembered by the author: PROPOSED, not verified
 * against real traces. Error codes are the NvError values of nvdrv.c (passed as NVG_*, equal by construction).
 *
 * nvhost-gpu   type 'H' (0x48): 1 SET_NVMAP_FD{s32}  3 SET_TIMEOUT{u32}  8 SUBMIT_GPFIFO{u64 gpfifo,u32 n,u32 flags,u32 fence_id,u32 fence_value}(0x18)
 *              9 ALLOC_OBJ_CTX{u32 class,u32 flags,u64 obj_id out}(0x10)  0xB ZCULL_BIND{u64 va,u32 mode,u32}(0x10)  0xC SET_ERROR_NOTIFIER{u64 off,u64 size,u32 mem,u32}(0x18)
 *              0xD SET_PRIORITY{u32 50|100|150}  0x14 SET_USER_DATA{u64}  0x15 GET_USER_DATA{u64}
 *              0x1A ALLOC_GPFIFO_EX2{u32 entries,u32 flags,u32 unk,u32 fence_id out,u32 fence_value out,u32 unk[3]}(0x20)
 * nvhost-as-gpu type 'A' (0x41): 1 BIND_CHANNEL{s32 fd}  2 ALLOC_SPACE{u32 pages,u32 page_size,u32 flags,u32,u64 offset|align}(0x18)
 *              3 FREE_SPACE{u64 offset,u32 pages,u32 page_size}(0x10)  4 MAP_BUFFER_EX{u32 flags,s32 kind,u32 handle,u32 page_size,u64 buf_off,u64 size,u64 offset}(0x28)
 *              5 UNMAP_BUFFER{u64 offset}  8 GET_VA_REGIONS{u64,u32 bufsize,u32,{u64 off,u32 page_size,u32,u64 pages}[2]}(0x40)
 *              9 INITIALIZE_EX{u32 big_page_size,s32 as_fd,u32 flags,u32,u64 va_start,u64 va_end,u64 va_split}(0x28)
 * nvhost-ctrl-gpu type 'G' (0x47): 1 ZCULL_GET_CTX_SIZE  2 ZCULL_GET_INFO  3 ZBC_SET_TABLE  4 ZBC_QUERY_TABLE  5 GET_CHARACTERISTICS  6 GET_TPC_MASKS
 *              7 FLUSH_L2  0x14 GET_GPU_TIME{u64 ns,u64}
 * Anything else: NVG_NOT_IMPLEMENTED (nvdrv.c logs it in the compat summary). A short buffer is NVG_BAD_PARAMETER. Calls arrive under the nvdrv lock. */
#ifndef NVDRV_NODES_CORE_H
#define NVDRV_NODES_CORE_H
#include <stdint.h>

enum { NVG_OK = 0, NVG_NOT_IMPLEMENTED = 1, NVG_BAD_PARAMETER = 4, NVG_INVALID_STATE = 8, NVG_INSUFFICIENT_MEMORY = 6, NVG_ALREADY_ALLOCATED = 13 };
enum { NVG_TYPE_CHANNEL = 0x48, NVG_TYPE_AS = 0x41, NVG_TYPE_CTRL = 0x47 };

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
    uint32_t gpfifo_entries, gpfifo_flags;
    uint32_t fence_id, fence_value; /* the channel's syncpoint (assigned by nvdrv.c at open) */
    uint32_t classes[NVG_MAX_CLASSES], nclasses;
    uint64_t next_obj_id, submits, entries_submitted;
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

int nvgpu_chan_ioctl(NvGpuChan *c, const NvGpuEnv *env, unsigned nr, uint8_t *data, unsigned size);
/* as-gpu: BIND_CHANNEL needs `chan`, the channel behind the fd in the request (nvdrv.c resolves it; NULL when the fd is not a gpu channel). */
int nvgpu_as_ioctl(NvGpuAs *a, const NvGpuEnv *env, NvGpuChan *chan, unsigned nr, uint8_t *data, unsigned size);
int nvgpu_ctrl_ioctl(NvGpuCtrl *c, unsigned nr, uint8_t *data, unsigned size, uint64_t now_ns);
#endif
