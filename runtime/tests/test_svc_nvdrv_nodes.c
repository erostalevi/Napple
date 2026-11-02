/* Unit tests: the GPU device nodes of nvdrv (nvdrv_nodes_core behind nvdrv.c): /dev/nvhost-gpu channel state, /dev/nvhost-as-gpu address space,
 * /dev/nvhost-ctrl-gpu queries, through the real ioctl handlers. Layouts: the public sources named in nvdrv_nodes_core.h, mirrored below as C structs
 * (one comment per struct names its source); the pins check size + offsets, the round trips go through nv_ioctl. Table: docs/reference/nvdrv-layouts.md. */
#include "../src/services/nvdrv.c"
#include "check.h"

#ifdef NAPPLE_HOST_TEST /* make test-host: kernel and thread stand-ins of tests/host */
#include "host/host_stubs.h"
#elif defined(__linux__) /* tools/linux-check: ipc_mock.c has everything but the summary hook */
void summary_ipc(const char *service, unsigned cmd, int status, unsigned request_bytes) { (void)service; (void)cmd; (void)status; (void)request_bytes; }
#endif

static IpcObject *nv;
static IpcCall call;
static uint8_t io[0x4000];
static uint32_t args[2];
static uint32_t cmd_id(unsigned dir, unsigned size, unsigned type, unsigned nr) { return (dir << 30) | (size << 16) | (type << 8) | nr; }
static uint32_t ioctl_(uint32_t fd, uint32_t cmd, void *data, uint32_t size) {
    memset(&call, 0, sizeof call);
    args[0] = fd; args[1] = cmd;
    call.object = nv; call.in = (const uint8_t *)args; call.in_size = 8;
    memcpy(io, data, size);
    call.na = 1; call.a[0] = (IpcBuf){(uintptr_t)io, size, 0};
    call.nb = 1; call.b[0] = (IpcBuf){(uintptr_t)io, size, 0};
    nv_ioctl(nv, &call);
    memcpy(data, io, size);
    uint32_t err; memcpy(&err, call.out + call.out_size - 4, 4);
    return err;
}
static uint32_t open_(const char *path) {
    memset(&call, 0, sizeof call);
    static char p[64]; snprintf(p, sizeof p, "%s", path);
    call.object = nv; call.na = 1; call.a[0] = (IpcBuf){(uintptr_t)p, strlen(p), 0};
    nv_open(nv, &call);
    uint32_t fd; memcpy(&fd, call.out, 4); return fd;
}
static void close_(uint32_t fd) { memset(&call, 0, sizeof call); args[0] = fd; call.object = nv; call.in = (const uint8_t *)args; call.in_size = 4; nv_close(nv, &call); }

typedef struct { uint32_t handle, iova; uint8_t *mem; uint32_t size; } Buf;

static void put32(void *b, unsigned off, uint32_t v) { memcpy((uint8_t *)b + off, &v, 4); }
static void put64(void *b, unsigned off, uint64_t v) { memcpy((uint8_t *)b + off, &v, 8); }
static uint32_t get32(const void *b, unsigned off) { uint32_t v; memcpy(&v, (const uint8_t *)b + off, 4); return v; }
static uint64_t get64(const void *b, unsigned off) { uint64_t v; memcpy(&v, (const uint8_t *)b + off, 8); return v; }
static uint32_t gio(uint32_t fd, unsigned type, unsigned nr, void *b, unsigned size) { return ioctl_(fd, cmd_id(3, size, type, nr), b, size); }
#define H 0x48
#define A 0x41
#define G 0x47

/* ---- layout mirrors: written from the public sources, used to pin sizes/offsets and to build requests ---------------------------------------------- */
#include <stddef.h>
typedef struct { uint32_t id, value; } MFence;                                                         /* libnx nvioctl_fence */
typedef struct { uint64_t gpfifo; uint32_t num_entries, flags; MFence fence; } MSubmitGpfifo;           /* libnx nvioctlChannel_SubmitGpfifo / KickoffPb */
typedef struct { uint32_t class_num, flags; uint64_t obj_id; } MAllocObjCtx;                          /* libnx nvioctlChannel_AllocObjCtx */
typedef struct { uint64_t gpu_va; uint32_t mode, padding; } MZcullBind;                               /* libnx nvioctlChannel_ZCullBind */
typedef struct { uint64_t offset, size; uint32_t enable, padding; } MSetErrorNotifier;               /* libnx nvioctlChannel_SetErrorNotifier */
typedef struct { uint32_t num_entries, flags, unk0; MFence fence_out; uint32_t unk1, unk2, unk3; } MAllocGpfifoEx2; /* libnx nvioctlChannel_AllocGpfifoEx2 */
typedef struct { uint32_t num_entries, num_inflight_jobs, flags, reserved[5]; } MAllocGpfifoEx;       /* L4T nvgpu.h nvgpu_alloc_gpfifo_ex_args */
typedef struct { uint32_t cmd, event_id; } MEventIdCtrl;                                              /* L4T nvgpu.h nvgpu_event_id_ctrl_args, Switch prefix */
typedef struct { uint32_t type, info[31]; } MErrorInfo;                                               /* libnx NvError */
typedef struct { uint64_t timestamp; uint32_t info32; uint16_t info16, status; } MNotification;       /* libnx NvNotification */
typedef struct { uint32_t pages, page_size, flags, pad; uint64_t offset_or_align; } MAllocSpace;      /* libnx nvioctlNvhostAsGpu_AllocSpace */
typedef struct { uint64_t offset; uint32_t pages, page_size; } MFreeSpace;                           /* libnx nvioctlNvhostAsGpu_FreeSpace */
typedef struct { uint32_t flags; int32_t kind; uint32_t nvmap_handle, page_size; uint64_t buffer_offset, mapping_size, offset; } MMapBufferEx; /* libnx */
typedef struct { uint64_t offset; uint32_t page_size, pad; uint64_t pages; } MVaRegion;              /* libnx nvioctl_va_region / nvgpu nvgpu_as_va_region */
typedef struct { uint64_t not_used; uint32_t bufsize, pad; MVaRegion regions[2]; } MGetVaRegions;    /* libnx nvioctlNvhostAsGpu_GetVARegions */
typedef struct { uint32_t flags; int32_t as_fd; uint32_t big_page_size, reserved; uint64_t unk0, unk1, unk2; } MInitializeEx; /* libnx InitializeEx */
typedef struct { uint32_t v[10]; } MZcullInfo;                                                        /* libnx nvioctl_zcull_info */
typedef struct { uint32_t color_ds[4], color_l2[4], depth, format, type; } MZbcSet;                  /* libnx nvioctlNvhostCtrlGpu_ZbcSetTable */
typedef struct { uint32_t color_ds[4], color_l2[4], depth, ref_cnt, format, type, size; } MZbcEntry; /* libnx nvioctl_zbc_entry */
typedef struct {                                                                                     /* libnx nvioctl_gpu_characteristics */
    uint32_t arch, impl, rev, num_gpc; uint64_t L2_cache_size, on_board_video_memory_size;
    uint32_t num_tpc_per_gpc, bus_type, big_page_size, compression_page_size, pde_coverage_bit_count, available_big_page_sizes, gpc_mask;
    uint32_t sm_arch_sm_version, sm_arch_spa_version, sm_arch_warp_count, gpu_va_bit_count, reserved; uint64_t flags;
    uint32_t twod_class, threed_class, compute_class, gpfifo_class, inline_to_memory_class, dma_copy_class;
    uint32_t max_fbps_count, fbp_en_mask, max_ltc_per_fbp, max_lts_per_ltc, max_tex_per_tpc, max_gpc_count, rop_l2_en_mask_0, rop_l2_en_mask_1;
    uint64_t chipname, gr_compbit_store_base_hw;
} MCharacteristics;
typedef struct { uint64_t gc_buf_size, gc_buf_addr; MCharacteristics gc; } MGetCharacteristics;      /* libnx nvioctlNvhostCtrlGpu_GetCharacteristics */
typedef struct { uint32_t bufsize, padding; uint64_t bufaddr; uint8_t out[8]; } MGetTpcMasks;       /* libnx nvioctlNvhostCtrlGpu_GetTpcMasks */
typedef struct { uint32_t slot, mask; } MZbcSlotMask;                                                /* libnx nvioctl_zbc_slot_mask */
typedef struct { uint64_t timestamp, reserved; } MGpuTime;                                           /* libnx nvioctl_gpu_time */
#define PIN(t, sz) CHECK(sizeof(t) == (sz), "layout: sizeof(" #t ") == %#x (got %#zx)", (unsigned)(sz), sizeof(t))
#define OFF(t, f, o) CHECK(offsetof(t, f) == (o), "layout: " #t "." #f " at %#x (got %#zx)", (unsigned)(o), offsetof(t, f))
static void layout_pins(void) {
    PIN(MSubmitGpfifo, NVG_SZ_SUBMIT_GPFIFO); OFF(MSubmitGpfifo, num_entries, 8); OFF(MSubmitGpfifo, flags, 12); OFF(MSubmitGpfifo, fence, 16);
    PIN(MSubmitGpfifo, NVG_SZ_KICKOFF_PB);
    PIN(MAllocObjCtx, NVG_SZ_ALLOC_OBJ_CTX); OFF(MAllocObjCtx, obj_id, 8);
    PIN(MZcullBind, NVG_SZ_ZCULL_BIND); OFF(MZcullBind, mode, 8);
    PIN(MSetErrorNotifier, NVG_SZ_SET_ERROR_NOTIFIER); OFF(MSetErrorNotifier, size, 8); OFF(MSetErrorNotifier, enable, 16);
    PIN(MAllocGpfifoEx2, NVG_SZ_ALLOC_GPFIFO_EX2); OFF(MAllocGpfifoEx2, flags, 4); OFF(MAllocGpfifoEx2, fence_out, 12); OFF(MAllocGpfifoEx2, unk3, 28);
    PIN(MAllocGpfifoEx, NVG_SZ_ALLOC_GPFIFO_EX); OFF(MAllocGpfifoEx, flags, 8);
    PIN(MEventIdCtrl, NVG_SZ_EVENT_ID_CONTROL);
    PIN(MErrorInfo, NVG_SZ_GET_ERROR_INFO);
    PIN(MNotification, NVG_SZ_GET_ERROR_NOTIFICATION); OFF(MNotification, info32, 8); OFF(MNotification, status, 14);
    PIN(uint64_t, NVG_SZ_USER_DATA);
    PIN(int32_t, NVG_SZ_BIND_CHANNEL);
    PIN(MAllocSpace, NVG_SZ_ALLOC_SPACE); OFF(MAllocSpace, flags, 8); OFF(MAllocSpace, offset_or_align, 16);
    PIN(MFreeSpace, NVG_SZ_FREE_SPACE); OFF(MFreeSpace, page_size, 12);
    PIN(uint64_t, NVG_SZ_UNMAP_BUFFER);
    PIN(MMapBufferEx, NVG_SZ_MAP_BUFFER_EX); OFF(MMapBufferEx, kind, 4); OFF(MMapBufferEx, nvmap_handle, 8); OFF(MMapBufferEx, page_size, 12);
    OFF(MMapBufferEx, buffer_offset, 16); OFF(MMapBufferEx, mapping_size, 24); OFF(MMapBufferEx, offset, 32);
    PIN(MVaRegion, 0x18); PIN(MGetVaRegions, NVG_SZ_GET_VA_REGIONS); OFF(MGetVaRegions, bufsize, 8); OFF(MGetVaRegions, regions, 16);
    PIN(MInitializeEx, NVG_SZ_INITIALIZE_EX); OFF(MInitializeEx, big_page_size, 8); OFF(MInitializeEx, unk0, 16); OFF(MInitializeEx, unk2, 32);
    PIN(uint32_t, NVG_SZ_ZCULL_GET_CTX_SIZE); PIN(MZcullInfo, NVG_SZ_ZCULL_GET_INFO);
    PIN(MZbcSet, NVG_SZ_ZBC_SET_TABLE); OFF(MZbcSet, depth, 32); OFF(MZbcSet, type, 40);
    PIN(MZbcEntry, NVG_SZ_ZBC_QUERY_TABLE); OFF(MZbcEntry, ref_cnt, 36); OFF(MZbcEntry, type, 44); OFF(MZbcEntry, size, 48);
    PIN(MCharacteristics, NVG_SZ_CHARACTERISTICS); PIN(MGetCharacteristics, NVG_SZ_GET_CHARACTERISTICS);
    OFF(MCharacteristics, num_tpc_per_gpc, 0x20); OFF(MCharacteristics, gpc_mask, 0x38); OFF(MCharacteristics, flags, 0x50); OFF(MCharacteristics, twod_class, 0x58);
    OFF(MCharacteristics, dma_copy_class, 0x6c); OFF(MCharacteristics, rop_l2_en_mask_0, 0x88); OFF(MCharacteristics, chipname, 0x90);
    PIN(MGetTpcMasks, NVG_SZ_GET_TPC_MASKS); OFF(MGetTpcMasks, out, 16);
    PIN(MZbcSlotMask, NVG_SZ_ZBC_GET_ACTIVE_SLOT_MASK); PIN(MGpuTime, NVG_SZ_GET_GPU_TIME);
    /* the ioctl numbers libnx encodes (type, nr, size) */
    CHECK(cmd_id(1, 4, H, NVG_CH_SET_NVMAP_FD) == 0x40044801u && cmd_id(3, 0x20, H, NVG_CH_ALLOC_GPFIFO_EX2) == 0xc020481au && cmd_id(3, 0x10, H, NVG_CH_ALLOC_OBJ_CTX) == 0xc0104809u,
          "layout: channel command words (SET_NVMAP_FD, ALLOC_GPFIFO_EX2, ALLOC_OBJ_CTX)");
    CHECK(cmd_id(3, 0x28, A, NVG_AS_MAP_BUFFER_EX) == 0xc0284106u && cmd_id(1, 0x28, A, NVG_AS_INITIALIZE_EX) == 0x40284109u && cmd_id(3, 0x40, A, NVG_AS_GET_VA_REGIONS) == 0xc0404108u,
          "layout: as-gpu command words (MAP_BUFFER_EX, INITIALIZE_EX, GET_VA_REGIONS)");
    CHECK(cmd_id(3, 0xb0, G, NVG_CTRL_GET_CHARACTERISTICS) == 0xc0b04705u && cmd_id(3, 0x10, G, NVG_CTRL_GET_GPU_TIME) == 0xc010471cu && cmd_id(2, 8, G, NVG_CTRL_ZBC_GET_ACTIVE_SLOT_MASK) == 0x80084714u,
          "layout: ctrl-gpu command words (GET_CHARACTERISTICS, GET_GPU_TIME, ZBC_GET_ACTIVE_SLOT_MASK)");
}

static uint32_t nvmap_handle(uint32_t nm, uint32_t size) {
    uint32_t c[2] = {size, 0};
    ioctl_(nm, cmd_id(3, 8, 1, 0x01), c, 8);
    return c[1];
}
static uint32_t as_init(uint32_t as) { MInitializeEx r = {.flags = 1, .big_page_size = 0x10000}; return gio(as, A, NVG_AS_INITIALIZE_EX, &r, sizeof r); } /* what libnx passes */

int main(void) {
    layout_pins();
    nv = nv_new();
    uint32_t nm = open_("/dev/nvmap");
    /* ---- open: the three nodes are state-tracked devices, the other known nodes stay open-only ---- */
    uint32_t gpu = open_("/dev/nvhost-gpu"), as = open_("/dev/nvhost-as-gpu"), ctrl = open_("/dev/nvhost-ctrl-gpu"), dbg = open_("/dev/nvhost-dbg-gpu");
    CHECK(gpu && as && ctrl && dbg, "all four nodes open");
    CHECK(fds[gpu].kind == DEV_GPU_CHAN && fds[as].kind == DEV_GPU_AS && fds[ctrl].kind == DEV_GPU_CTRL && fds[dbg].kind == DEV_OTHER, "node kinds");
    CHECK(fds[gpu].gchan.fence_id >= SYNCPT_FIRST_DYNAMIC && syncpt_owned[fds[gpu].gchan.fence_id], "the channel owns a fence syncpoint");
    uint8_t b[0x100];

    /* ---- nvhost-gpu: simple setters ---- */
    memset(b, 0, sizeof b); put32(b, 0, 7);
    CHECK(gio(gpu, H, 1, b, 4) == 0 && fds[gpu].gchan.nvmap_fd == 7, "SET_NVMAP_FD stores the fd");
    CHECK(gio(gpu, H, 1, b, 2) == NV_BAD_PARAMETER, "SET_NVMAP_FD short buffer");
    put32(b, 0, 3000);
    CHECK(gio(gpu, H, 3, b, 4) == 0 && fds[gpu].gchan.timeout_ms == 3000, "SET_TIMEOUT stores the value");
    CHECK(fds[gpu].gchan.priority == 100, "default priority is medium");
    put32(b, 0, 150);
    CHECK(gio(gpu, H, 0xd, b, 4) == 0 && fds[gpu].gchan.priority == 150, "SET_PRIORITY high");
    put32(b, 0, 77);
    CHECK(gio(gpu, H, 0xd, b, 4) == NV_BAD_PARAMETER && fds[gpu].gchan.priority == 150, "SET_PRIORITY refuses other values and keeps the old one");
    /* SET/GET_USER_DATA are type 'G' on the channel fd [libnx]: the core answers them (nvgpu_chan_ioctl_typed); 'H' 0x14 / 0x15 are not channel ioctls */
    put64(b, 0, 0x1122334455667788ull);
    CHECK(nvgpu_chan_ioctl_typed(&fds[gpu].gchan, NULL, NVG_TYPE_GPU, NVG_GPU_SET_USER_DATA, b, 8) == 0, "SET_USER_DATA ('G' 0x14)");
    memset(b, 0, 8);
    CHECK(nvgpu_chan_ioctl_typed(&fds[gpu].gchan, NULL, NVG_TYPE_GPU, NVG_GPU_GET_USER_DATA, b, 8) == 0 && get64(b, 0) == 0x1122334455667788ull, "GET_USER_DATA ('G' 0x15) returns what was set");
    CHECK(nvgpu_chan_ioctl_typed(&fds[gpu].gchan, NULL, NVG_TYPE_GPU, NVG_GPU_GET_USER_DATA, b, 4) == NV_BAD_PARAMETER, "GET_USER_DATA short buffer");
    CHECK(nvgpu_chan_ioctl_typed(&fds[gpu].gchan, NULL, NVG_TYPE_CHANNEL, NVG_CH_SET_TIMEOUT, b, 4) == 0, "the typed entry passes type 'H' through");
    CHECK(nvgpu_chan_ioctl_typed(&fds[gpu].gchan, NULL, 'X', 1, b, 8) == NV_NOT_IMPLEMENTED, "the typed entry refuses another type");
    CHECK(gio(gpu, H, 0x14, b, 8) == NV_NOT_IMPLEMENTED && gio(gpu, H, 0x15, b, 8) == NV_NOT_IMPLEMENTED, "'H' 0x14 / 0x15 are not implemented");
    put32(b, 0, 3000);
    gio(gpu, H, 3, b, 4);
    memset(b, 0, sizeof b); put64(b, 0, 0x1000); put64(b, 8, 0x200); put32(b, 16, 5);
    CHECK(gio(gpu, H, 0xc, b, 0x18) == 0 && fds[gpu].gchan.err_off == 0x1000 && fds[gpu].gchan.err_size == 0x200 && fds[gpu].gchan.err_mem == 5, "SET_ERROR_NOTIFIER stores the notifier");
    CHECK(gio(gpu, H, 0xc, b, 8) == NV_BAD_PARAMETER, "SET_ERROR_NOTIFIER short buffer");

    /* ---- ordering: gpfifo needs a bound address space; objects and submits need the gpfifo ---- */
    memset(b, 0, sizeof b); put32(b, 0, 0x400);
    CHECK(gio(gpu, H, 0x1a, b, 0x20) == NV_INVALID_STATE && !fds[gpu].gchan.gpfifo_entries, "ALLOC_GPFIFO_EX2 before BIND_CHANNEL is InvalidState");
    memset(b, 0, sizeof b); put32(b, 0, 0x9001);
    CHECK(gio(gpu, H, 9, b, 0x10) == NV_BAD_PARAMETER, "ALLOC_OBJ_CTX with an unknown class is BadParameter");
    put32(b, 0, 0xb197);
    CHECK(gio(gpu, H, 9, b, 0x10) == NV_INVALID_STATE, "ALLOC_OBJ_CTX before the gpfifo is InvalidState");
    memset(b, 0, sizeof b);
    CHECK(gio(gpu, H, 8, b, 0x18) == NV_INVALID_STATE, "SUBMIT_GPFIFO before the gpfifo is InvalidState");
    CHECK(gio(gpu, H, 0xb, b, 0x10) == NV_INVALID_STATE, "ZCULL_BIND before the gpfifo is InvalidState");

    /* ---- nvhost-as-gpu: needs INITIALIZE_EX first ---- */
    memset(b, 0, sizeof b); put32(b, 0, 1); put32(b, 4, 0x1000);
    CHECK(gio(as, A, 2, b, 0x18) == NV_INVALID_STATE, "ALLOC_SPACE before INITIALIZE_EX is InvalidState");
    CHECK(gio(as, A, 8, b, 0x40) == NV_INVALID_STATE, "GET_VA_REGIONS before INITIALIZE_EX is InvalidState");
    { MInitializeEx r = {.flags = 1, .big_page_size = 0x4321};
      CHECK(gio(as, A, 9, &r, sizeof r) == NV_BAD_PARAMETER && !fds[as].gas.initialized, "INITIALIZE_EX with a bad big page size (offset 8) is refused"); }
    { MInitializeEx r = {.flags = 0x10000, .big_page_size = 0x4321}; /* the old (big page first) reading would have accepted this */
      CHECK(gio(as, A, 9, &r, sizeof r) == NV_BAD_PARAMETER, "INITIALIZE_EX reads big_page_size at +8, not +0 (libnx order)"); }
    { MInitializeEx r = {.big_page_size = 0x10000, .unk0 = 0x2000000, .unk1 = 0x1000000, .unk2 = 0x100000}; /* start not below split */
      CHECK(gio(as, A, 9, &r, sizeof r) == NV_BAD_PARAMETER, "INITIALIZE_EX with start >= split is refused"); }
    CHECK(as_init(as) == 0 && fds[as].gas.initialized && fds[as].gas.big_page_size == 0x10000 && fds[as].gas.flags == 1, "INITIALIZE_EX as libnx calls it: flags 1, 64K big pages");
    CHECK(as_init(as) == NV_INVALID_STATE, "INITIALIZE_EX twice is InvalidState");
    memset(b, 0, sizeof b); put32(b, 8, 0x40);
    CHECK(gio(as, A, 8, b, 0x40) == 0 && get32(b, 8) == 0x30, "GET_VA_REGIONS reports 0x30 bytes of regions");
    CHECK(get64(b, 16) == 0x04000000ull && get32(b, 24) == 0x1000 && get64(b, 32) == (0x100000000ull - 0x04000000ull) / 0x1000, "small-page region: start, 4K pages, count");
    CHECK(get64(b, 40) == 0x100000000ull && get32(b, 48) == 0x10000 && get64(b, 56) == (0x2000000000ull - 0x100000000ull) / 0x10000, "big-page region: split, 64K pages, count");
    { MGetVaRegions r = {.bufsize = 0x30}; /* round trip through the mirror */
      CHECK(gio(as, A, NVG_AS_GET_VA_REGIONS, &r, sizeof r) == 0 && r.bufsize == 0x30 && r.regions[0].offset == 0x04000000ull && r.regions[0].page_size == 0x1000 &&
            r.regions[1].offset == 0x100000000ull && r.regions[1].page_size == 0x10000 && r.regions[1].pages == (0x2000000000ull - 0x100000000ull) / 0x10000,
            "GET_VA_REGIONS round trip through the libnx struct"); }
    { uint32_t as3 = open_("/dev/nvhost-as-gpu"); MInitializeEx r = {0};
      CHECK(gio(as3, A, 9, &r, sizeof r) == 0 && fds[as3].gas.big_page_size == 0x20000, "INITIALIZE_EX big_page_size 0 = the 128K default GET_CHARACTERISTICS reports");
      close_(as3); }
    memset(b, 0, sizeof b); put32(b, 8, 0x18);
    CHECK(gio(as, A, 8, b, 0x40) == 0 && get32(b, 8) == 0x30 && get64(b, 40) == 0, "GET_VA_REGIONS with room for one region fills only that one");
    CHECK(gio(as, A, 8, b, 0x20) == NV_BAD_PARAMETER, "GET_VA_REGIONS short buffer");

    /* ---- ALLOC_SPACE / FREE_SPACE ---- */
    memset(b, 0, sizeof b); put32(b, 0, 16); put32(b, 4, 0x10000); put64(b, 16, 0x10000);
    CHECK(gio(as, A, 2, b, 0x18) == 0, "ALLOC_SPACE (not fixed)");
    uint64_t r1 = get64(b, 16);
    CHECK(r1 >= 0x04000000ull && !(r1 & 0xffff), "ALLOC_SPACE returns an aligned offset inside the space (%#llx)", (unsigned long long)r1);
    memset(b, 0, sizeof b); put32(b, 0, 16); put32(b, 4, 0x10000); put64(b, 16, 0x10000);
    CHECK(gio(as, A, 2, b, 0x18) == 0 && get64(b, 16) == r1 + 16 * 0x10000, "a second reservation follows the first without overlap");
    uint64_t r2 = get64(b, 16);
    memset(b, 0, sizeof b); put32(b, 0, 16); put32(b, 4, 0x10000); put32(b, 8, 1); put64(b, 16, r1);
    CHECK(gio(as, A, 2, b, 0x18) == NV_BAD_PARAMETER, "ALLOC_SPACE fixed on top of a reservation is refused");
    memset(b, 0, sizeof b); put32(b, 0, 4); put32(b, 4, 0x1000); put32(b, 8, 1); put64(b, 16, 0x08000000ull);
    CHECK(gio(as, A, 2, b, 0x18) == 0 && get64(b, 16) == 0x08000000ull, "ALLOC_SPACE fixed keeps the requested offset");
    memset(b, 0, sizeof b); put32(b, 0, 4); put32(b, 4, 0x3000);
    CHECK(gio(as, A, 2, b, 0x18) == NV_BAD_PARAMETER, "ALLOC_SPACE with an unsupported page size is refused");
    memset(b, 0, sizeof b); put32(b, 4, 0x1000);
    CHECK(gio(as, A, 2, b, 0x18) == NV_BAD_PARAMETER, "ALLOC_SPACE with zero pages is refused");
    memset(b, 0, sizeof b); put64(b, 0, 0x08000000ull); put32(b, 8, 4); put32(b, 12, 0x2000);
    CHECK(gio(as, A, 3, b, 0x10) == NV_BAD_PARAMETER, "FREE_SPACE with a wrong page size is refused");
    put32(b, 12, 0x1000);
    CHECK(gio(as, A, 3, b, 0x10) == 0, "FREE_SPACE of the fixed reservation");
    CHECK(gio(as, A, 3, b, 0x10) == NV_BAD_PARAMETER, "FREE_SPACE twice is refused");

    /* ---- MAP_BUFFER_EX / UNMAP_BUFFER ---- */
    uint32_t hnd = nvmap_handle(nm, 0x20000), small = nvmap_handle(nm, 0x3000);
    memset(b, 0, sizeof b); put32(b, 8, 0x9999); put32(b, 12, 0x10000);
    CHECK(gio(as, A, 6, b, 0x28) == NV_BAD_PARAMETER, "MAP_BUFFER_EX on a missing nvmap handle is refused");
    memset(b, 0, sizeof b); put32(b, 4, 0xfe); put32(b, 8, hnd); put32(b, 12, 0x10000);
    CHECK(gio(as, A, 4, b, 0x28) == NV_NOT_IMPLEMENTED, "as-gpu nr 4 (old MAP_BUFFER, no Switch layout) is NotImplemented");
    CHECK(gio(as, A, 6, b, 0x28) == 0 && get32(b, 12) == 0x10000, "MAP_BUFFER_EX (nr 6, not fixed) reports the page size back");
    uint64_t m1 = get64(b, 32);
    CHECK(m1 && !(m1 & 0xffff) && 1, "MAP_BUFFER_EX returns an offset (%#llx)", (unsigned long long)m1);
    CHECK(!(m1 < r1 + 16 * 0x10000 && r1 < m1 + 0x20000) && !(m1 < r2 + 16 * 0x10000 && r2 < m1 + 0x20000), "a non-fixed map avoids the reservations");
    unsigned live = 0; for (unsigned i = 0; i < NVG_MAX_MAPS; i++) live += fds[as].gas.maps[i].used;
    CHECK(live == 1 && fds[as].gas.maps[0].handle == hnd && fds[as].gas.maps[0].kind == 0xfe && fds[as].gas.maps[0].size == 0x20000, "the mapping is tracked (handle, kind, size)");
    memset(b, 0, sizeof b); put32(b, 8, small); put32(b, 12, 0); /* page size 0: 4K for a small buffer */
    CHECK(gio(as, A, 6, b, 0x28) == 0 && get32(b, 12) == 0x1000 && fds[as].gas.maps[1].page_size == 0x1000 && fds[as].gas.maps[1].size == 0x3000 && get64(b, 32) != m1, "page size 0 picks 4K for a small buffer, size from the handle");
    uint64_t m2 = get64(b, 32);
    memset(b, 0, sizeof b); put32(b, 0, 1); put32(b, 8, hnd); put32(b, 12, 0x10000); put64(b, 32, m1);
    CHECK(gio(as, A, 6, b, 0x28) == NV_BAD_PARAMETER, "fixed map outside a reservation is refused");
    memset(b, 0, sizeof b); put32(b, 0, 1); put32(b, 8, hnd); put32(b, 12, 0x10000); put64(b, 32, r1);
    CHECK(gio(as, A, 6, b, 0x28) == 0 && get64(b, 32) == r1, "fixed map inside a reservation keeps its offset");
    memset(b, 0, sizeof b); put32(b, 0, 1); put32(b, 8, hnd); put32(b, 12, 0x10000); put64(b, 32, r1 + 0x10000);
    CHECK(gio(as, A, 6, b, 0x28) == NV_BAD_PARAMETER, "fixed map overlapping an existing map is refused");
    memset(b, 0, sizeof b); put32(b, 8, hnd); put32(b, 12, 0x10000); put64(b, 16, 0x30000);
    CHECK(gio(as, A, 6, b, 0x28) == NV_BAD_PARAMETER, "map with a buffer offset past the handle is refused");
    memset(b, 0, sizeof b); put32(b, 8, hnd); put32(b, 12, 0x10000); put64(b, 24, 0x30000);
    CHECK(gio(as, A, 6, b, 0x28) == NV_BAD_PARAMETER, "map longer than the handle is refused");
    memset(b, 0, sizeof b); put32(b, 8, hnd); put32(b, 12, 0x3000);
    CHECK(gio(as, A, 6, b, 0x28) == NV_BAD_PARAMETER, "map with an unsupported page size is refused");
    { MMapBufferEx r = {.flags = NVG_AS_MAP_MODIFY | NVG_AS_MAP_CACHEABLE, .kind = 0xdb, .offset = m2}; /* re-kind the 4K mapping in place */
      CHECK(gio(as, A, NVG_AS_MAP_BUFFER_EX, &r, sizeof r) == 0 && r.offset == m2 && r.page_size == 0x1000 && fds[as].gas.maps[1].kind == 0xdb &&
            fds[as].gas.maps[1].handle == small && (fds[as].gas.maps[1].flags & NVG_AS_MAP_CACHEABLE), "MAP_BUFFER_EX MODIFY changes the kind of the mapping at offset");
      r.offset = m2 + 0x1000;
      CHECK(gio(as, A, NVG_AS_MAP_BUFFER_EX, &r, sizeof r) == NV_BAD_PARAMETER, "MAP_BUFFER_EX MODIFY at a non-mapping is refused"); }
    { MMapBufferEx r = {.kind = -1, .nvmap_handle = small, .mapping_size = 0x2000};
      CHECK(gio(as, A, NVG_AS_MAP_BUFFER_EX, &r, sizeof r) == 0 && r.offset && r.page_size == 0x1000, "MAP_BUFFER_EX round trip through the libnx struct (kind -1, partial size)");
      MFreeSpace u = {.offset = r.offset}; /* UNMAP_BUFFER takes the u64 offset only */
      CHECK(gio(as, A, NVG_AS_UNMAP_BUFFER, &u, 8) == 0, "UNMAP_BUFFER of that mapping"); }
    memset(b, 0, sizeof b); put64(b, 0, m2 + 0x1000);
    CHECK(gio(as, A, 5, b, 8) == NV_BAD_PARAMETER, "UNMAP_BUFFER at a non-mapping is refused");
    put64(b, 0, m2);
    CHECK(gio(as, A, 5, b, 8) == 0 && gio(as, A, 5, b, 8) == NV_BAD_PARAMETER, "UNMAP_BUFFER removes the mapping, a second one fails");
    memset(b, 0, sizeof b); put64(b, 0, r1); put32(b, 8, 16); put32(b, 12, 0x10000);
    CHECK(gio(as, A, 3, b, 0x10) == 0, "FREE_SPACE");
    live = 0; for (unsigned i = 0; i < NVG_MAX_MAPS; i++) live += fds[as].gas.maps[i].used && fds[as].gas.maps[i].offset >= r1 && fds[as].gas.maps[i].offset < r1 + 16 * 0x10000;
    CHECK(live == 0, "FREE_SPACE drops the mappings inside the reservation");

    /* ---- BIND_CHANNEL, then the channel proceeds ---- */
    memset(b, 0, sizeof b); put32(b, 0, 999);
    CHECK(gio(as, A, 1, b, 4) == NV_BAD_PARAMETER, "BIND_CHANNEL with a bad fd is refused");
    put32(b, 0, nm);
    CHECK(gio(as, A, 1, b, 4) == NV_BAD_PARAMETER, "BIND_CHANNEL with a non-channel fd is refused");
    put32(b, 0, gpu);
    CHECK(gio(as, A, 1, b, 4) == 0 && fds[gpu].gchan.as == &fds[as].gas && fds[as].gas.bound == 1, "BIND_CHANNEL binds the channel");
    CHECK(gio(as, A, 1, b, 4) == NV_BAD_PARAMETER && fds[as].gas.bound == 1, "binding the same channel twice is refused");

    memset(b, 0, sizeof b); put32(b, 0, 0);
    CHECK(gio(gpu, H, 0x1a, b, 0x20) == NV_BAD_PARAMETER, "ALLOC_GPFIFO_EX2 with zero entries is refused");
    put32(b, 0, 0x400); put32(b, 4, 0x5);
    CHECK(gio(gpu, H, 0x1a, b, 0x20) == 0 && fds[gpu].gchan.gpfifo_entries == 0x400 && fds[gpu].gchan.gpfifo_flags == 5, "ALLOC_GPFIFO_EX2 allocates the ring");
    CHECK(get32(b, 12) == fds[gpu].gchan.fence_id && get32(b, 16) == 0, "ALLOC_GPFIFO_EX2 reports the channel fence (id, value 0)");
    CHECK(gio(gpu, H, 0x1a, b, 0x20) == 13, "ALLOC_GPFIFO_EX2 twice is AlreadyAllocated");

    /* ---- ALLOC_OBJ_CTX ---- */
    memset(b, 0, sizeof b); put32(b, 0, 0xb197);
    CHECK(gio(gpu, H, 9, b, 0x10) == 0 && get64(b, 8) == 1, "ALLOC_OBJ_CTX 3D class returns object id 1");
    put32(b, 0, 0xb1c0); put64(b, 8, 0);
    CHECK(gio(gpu, H, 9, b, 0x10) == 0 && get64(b, 8) == 2 && fds[gpu].gchan.nclasses == 2, "ALLOC_OBJ_CTX compute class returns object id 2");
    put32(b, 0, 0xb197);
    CHECK(gio(gpu, H, 9, b, 0x10) == 13 && fds[gpu].gchan.nclasses == 2, "ALLOC_OBJ_CTX of the same class twice is AlreadyAllocated");
    for (unsigned i = 0; i < 6; i++) {
        put32(b, 0, nvgpu_classes[i]);
        gio(gpu, H, 9, b, 0x10);
    }
    CHECK(fds[gpu].gchan.nclasses == 6, "every advertised class can be allocated once");

    /* ---- ZCULL_BIND ---- */
    memset(b, 0, sizeof b); put64(b, 0, 0x5000000); put32(b, 8, 4);
    CHECK(gio(gpu, H, 0xb, b, 0x10) == NV_BAD_PARAMETER, "ZCULL_BIND mode > 3 is refused");
    put32(b, 8, 2);
    CHECK(gio(gpu, H, 0xb, b, 0x10) == 0 && fds[gpu].gchan.zcull_va == 0x5000000 && fds[gpu].gchan.zcull_mode == 2, "ZCULL_BIND stores va and mode");

    /* ---- SUBMIT_GPFIFO ---- */
    uint32_t fid = fds[gpu].gchan.fence_id;
    memset(b, 0, sizeof b); put32(b, 8, 0x401);
    CHECK(gio(gpu, H, 8, b, 0x18) == NV_BAD_PARAMETER && fds[gpu].gchan.submits == 0, "SUBMIT_GPFIFO with more entries than the ring is refused");
    memset(b, 0, sizeof b); put32(b, 8, 3);
    CHECK(gio(gpu, H, 8, b, 0x18) == 0 && fds[gpu].gchan.submits == 1 && fds[gpu].gchan.entries_submitted == 3 && syncpt[fid] == 0, "SUBMIT_GPFIFO without FENCE_GET leaves the syncpoint alone");
    memset(b, 0, sizeof b); put32(b, 8, 2); put32(b, 12, 2);
    CHECK(gio(gpu, H, 8, b, 0x18) == 0 && get32(b, 16) == fid && get32(b, 20) == 1 && syncpt[fid] == 1 && syncpt_max[fid] == 1, "SUBMIT_GPFIFO with FENCE_GET returns fence value 1 and advances the syncpoint");
    memset(b, 0, sizeof b); put32(b, 8, 1); put32(b, 12, 2);
    CHECK(gio(gpu, H, 8, b, 0x18) == 0 && get32(b, 20) == 2 && fds[gpu].gchan.submits == 3, "the next fence is 2");
    CHECK(gio(gpu, H, 8, b, 0x10) == NV_BAD_PARAMETER, "SUBMIT_GPFIFO short buffer");
    { MSubmitGpfifo r = {.num_entries = 1, .flags = NVG_SUBMIT_FENCE_GET | NVG_SUBMIT_FENCE_WAIT, .fence = {fid, 2}}; /* round trip through the mirror */
      CHECK(gio(gpu, H, NVG_CH_SUBMIT_GPFIFO, &r, sizeof r) == 0 && r.fence.id == fid && r.fence.value == 3 && fds[gpu].gchan.fence_waits == 1,
            "SUBMIT_GPFIFO round trip: FENCE_WAIT recorded, FENCE_GET returns fence 3"); }
    { uint8_t big[0x18 + 4 * 8] = {0}; MSubmitGpfifo r = {.num_entries = 4}; memcpy(big, &r, sizeof r); /* entries inline after the struct */
      CHECK(gio(gpu, H, NVG_CH_SUBMIT_GPFIFO, big, sizeof big) == 0 && fds[gpu].gchan.entries_submitted == 3 + 2 + 1 + 1 + 4, "SUBMIT_GPFIFO with inline entries"); }
    { MSubmitGpfifo r = {.num_entries = 2, .flags = NVG_SUBMIT_FENCE_GET};
      CHECK(gio(gpu, H, NVG_CH_KICKOFF_PB, &r, sizeof r) == 0 && r.fence.value == 4 && fds[gpu].gchan.kickoffs == 1, "KICKOFF_PB (0x1B): a submit with the entries elsewhere, fence 4");
      CHECK(gio(gpu, H, NVG_CH_KICKOFF_PB, &r, 0x10) == NV_BAD_PARAMETER, "KICKOFF_PB short buffer"); }
    /* ENABLE / DISABLE / PREEMPT / FORCE_RESET (no data) */
    CHECK(gio(gpu, H, NVG_CH_DISABLE, b, 0) == 0 && !fds[gpu].gchan.enabled, "DISABLE");
    { MSubmitGpfifo r = {.num_entries = 1};
      CHECK(gio(gpu, H, NVG_CH_SUBMIT_GPFIFO, &r, sizeof r) == NV_INVALID_STATE, "a disabled channel refuses submits"); }
    CHECK(gio(gpu, H, NVG_CH_ENABLE, b, 0) == 0 && fds[gpu].gchan.enabled, "ENABLE");
    CHECK(gio(gpu, H, NVG_CH_PREEMPT, b, 0) == 0 && gio(gpu, H, NVG_CH_FORCE_RESET, b, 0) == 0 && fds[gpu].gchan.preempts == 1 && fds[gpu].gchan.resets == 1 &&
          fds[gpu].gchan.gpfifo_entries == 0x400, "PREEMPT and FORCE_RESET are counted; the ring stays");
    { MEventIdCtrl r = {NVG_EVENT_ID_CMD_ENABLE, 2};
      CHECK(gio(gpu, H, NVG_CH_EVENT_ID_CONTROL, &r, sizeof r) == 0 && fds[gpu].gchan.events == 4, "EVENT_ID_CONTROL enables event 2 (BLOCKING_SYNC)");
      r.cmd = 0;
      CHECK(gio(gpu, H, NVG_CH_EVENT_ID_CONTROL, &r, sizeof r) == 0 && fds[gpu].gchan.events == 0, "EVENT_ID_CONTROL disables it");
      r.event_id = NVG_EVENT_ID_MAX;
      CHECK(gio(gpu, H, NVG_CH_EVENT_ID_CONTROL, &r, sizeof r) == NV_BAD_PARAMETER && gio(gpu, H, NVG_CH_EVENT_ID_CONTROL, &r, 4) == NV_BAD_PARAMETER, "EVENT_ID_CONTROL bad id / short buffer"); }
    { MErrorInfo r; memset(&r, 0xee, sizeof r);
      CHECK(gio(gpu, H, NVG_CH_GET_ERROR_INFO, &r, sizeof r) == 0 && r.type == 0 && r.info[30] == 0, "GET_ERROR_INFO (0x80 bytes): no error");
      MNotification n; memset(&n, 0xee, sizeof n);
      CHECK(gio(gpu, H, NVG_CH_GET_ERROR_NOTIFICATION, &n, sizeof n) == 0 && n.info32 == 0 && n.status == 0, "GET_ERROR_NOTIFICATION (0x10 bytes): nothing pending");
      CHECK(gio(gpu, H, NVG_CH_GET_ERROR_INFO, &r, 0x40) == NV_BAD_PARAMETER && gio(gpu, H, NVG_CH_GET_ERROR_NOTIFICATION, &n, 8) == NV_BAD_PARAMETER, "error queries: short buffers"); }
    uint32_t c1[4] = {fid, 0, 0, 0};
    CHECK(ioctl_(open_("/dev/nvhost-ctrl"), cmd_id(3, 8, 0, 0x14), c1, 8) == 0 && c1[1] == 4, "the fence (4) is visible through nvhost-ctrl SYNCPT_READ");

    /* ---- closing the address space unbinds its channels; closing the channel frees its syncpoint ---- */
    close_(as);
    CHECK(fds[gpu].gchan.as == NULL, "closing the address space unbinds the channel");
    uint32_t gpu2 = open_("/dev/nvhost-gpu");
    memset(b, 0, sizeof b); put32(b, 0, 16);
    CHECK(gio(gpu2, H, 0x1a, b, 0x20) == NV_INVALID_STATE, "a fresh channel starts unbound with no state");
    { uint32_t as4 = open_("/dev/nvhost-as-gpu"); as_init(as4); put32(b, 0, gpu2); gio(as4, A, 1, b, 4);
      MAllocGpfifoEx r = {.num_entries = 0x80, .num_inflight_jobs = 3, .flags = 2};
      CHECK(gio(gpu2, H, NVG_CH_ALLOC_GPFIFO_EX, &r, sizeof r) == 0 && fds[gpu2].gchan.gpfifo_entries == 0x80 && fds[gpu2].gchan.gpfifo_flags == 2 &&
            fds[gpu2].gchan.gpfifo_inflight == 3, "ALLOC_GPFIFO_EX (0x18, nvgpu layout: flags at +8)");
      CHECK(gio(gpu2, H, NVG_CH_ALLOC_GPFIFO_EX, &r, sizeof r) == 13, "ALLOC_GPFIFO_EX after a ring exists is AlreadyAllocated");
      CHECK(gio(gpu2, H, NVG_CH_ALLOC_GPFIFO_EX, &r, 0x10) == NV_BAD_PARAMETER, "ALLOC_GPFIFO_EX short buffer");
      close_(as4); }
    CHECK(fds[gpu2].gchan.priority == 100 && fds[gpu2].gchan.classes[0] == 0 && fds[gpu2].gchan.fence_id != fid, "fresh channel state and its own fence syncpoint");
    close_(gpu);
    CHECK(!syncpt_owned[fid], "closing the channel releases its fence syncpoint");

    /* ---- nvhost-ctrl-gpu ---- */
    memset(b, 0, sizeof b);
    CHECK(gio(ctrl, G, 1, b, 4) == 0 && get32(b, 0) == 0x1000, "ZCULL_GET_CTX_SIZE");
    memset(b, 0, sizeof b);
    { MZcullInfo z = {{0}}; static const uint32_t want[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xc0, 0x20, 0x40, 0x10};
      CHECK(gio(ctrl, G, NVG_CTRL_ZCULL_GET_INFO, &z, sizeof z) == 0 && !memcmp(z.v, want, sizeof want), "ZCULL_GET_INFO: the ten values libnx reads on hardware"); }
    CHECK(gio(ctrl, G, 2, b, 0x20) == NV_BAD_PARAMETER, "ZCULL_GET_INFO short buffer");
    memset(b, 0, sizeof b);
    { MGetCharacteristics r = {.gc_buf_size = sizeof(MCharacteristics), .gc_buf_addr = 1}; /* as libnx asks */
      CHECK(gio(ctrl, G, NVG_CTRL_GET_CHARACTERISTICS, &r, sizeof r) == 0 && r.gc_buf_size == 0xa0, "GET_CHARACTERISTICS: buf_size comes back as 0xA0");
      MCharacteristics *c = &r.gc;
      CHECK(c->arch == 0x120 && c->impl == 0xb && c->rev == 0xa1 && c->num_gpc == 1 && c->L2_cache_size == 0x40000 && c->on_board_video_memory_size == 0, "characteristics: GM20B A1, 1 GPC, 256K L2");
      CHECK(c->num_tpc_per_gpc == 2 && c->bus_type == 0x20 && c->big_page_size == 0x20000 && c->compression_page_size == 0x20000 && c->pde_coverage_bit_count == 0x1b &&
            c->available_big_page_sizes == 0x30000 && c->gpc_mask == 1, "characteristics: TPCs, AXI, page sizes, GPC mask");
      CHECK(c->sm_arch_sm_version == 0x503 && c->sm_arch_spa_version == 0x503 && c->sm_arch_warp_count == 0x80 && c->gpu_va_bit_count == 0x28 && c->flags == 0x55, "characteristics: SM 5.3, 128 warps, 40-bit VA, flags 0x55");
      CHECK(c->twod_class == 0x902d && c->threed_class == 0xb197 && c->compute_class == 0xb1c0 && c->gpfifo_class == 0xb06f && c->inline_to_memory_class == 0xa140 &&
            c->dma_copy_class == 0xb0b5, "characteristics: the six engine classes at +0x58");
      CHECK(c->max_fbps_count == 1 && c->fbp_en_mask == 0 && c->max_ltc_per_fbp == 2 && c->max_lts_per_ltc == 1 && c->max_tex_per_tpc == 0 && c->max_gpc_count == 1 &&
            c->rop_l2_en_mask_0 == 0x21d70 && c->rop_l2_en_mask_1 == 0 && c->chipname == 0x6230326d67ull && c->gr_compbit_store_base_hw == 0, "characteristics: FBP/LTC, ROP mask, chip name \"gm20b\" at +0x90"); }
    { MGetCharacteristics r = {.gc_buf_size = 0x20, .gc_buf_addr = 1};
      CHECK(gio(ctrl, G, NVG_CTRL_GET_CHARACTERISTICS, &r, sizeof r) == 0 && r.gc_buf_size == 0xa0 && r.gc.arch == 0x120 && r.gc.flags == 0, "GET_CHARACTERISTICS with a 0x20-byte buffer fills only that prefix"); }
    CHECK(gio(ctrl, G, 5, b, 0x20) == NV_BAD_PARAMETER, "GET_CHARACTERISTICS with a short ioctl buffer is BadParameter");
    memset(b, 0, sizeof b);
    CHECK(gio(ctrl, G, 6, b, 0x18) == 0 && get64(b, 16) == 3, "GET_TPC_MASKS reports two TPCs");
    memset(b, 0, sizeof b); put32(b, 0, 1); put32(b, 4, 1);
    CHECK(gio(ctrl, G, 7, b, 8) == 0 && gio(ctrl, G, 7, b, 8) == 0 && fds[ctrl].gctl.l2_flushes == 2, "FLUSH_L2 is counted");
    uint64_t t[2] = {0, 0}, t2[2] = {0, 0};
    CHECK(gio(ctrl, G, NVG_CTRL_GET_GPU_TIME, t, 16) == 0 && gio(ctrl, G, NVG_CTRL_GET_GPU_TIME, t2, 16) == 0 && t2[0] >= t[0] && t2[1] == 0, "GET_GPU_TIME (0x1C) is monotonic, reserved 0");
    CHECK(gio(ctrl, G, NVG_CTRL_GET_GPU_TIME, t, 8) == NV_BAD_PARAMETER, "GET_GPU_TIME short buffer");
    { MGpuTime a = {0}, b2 = {0}; NvGpuCtrl cc; nvgpu_ctrl_init(&cc, 1000);
      CHECK(nvgpu_ctrl_ioctl(&cc, NVG_CTRL_GET_GPU_TIME, (uint8_t *)&a, sizeof a, 5000) == 0 && a.timestamp == 4000 &&
            nvgpu_ctrl_ioctl(&cc, NVG_CTRL_GET_GPU_TIME, (uint8_t *)&b2, sizeof b2, 3000) == 0 && b2.timestamp == 4000, "GET_GPU_TIME: ns since open, never backwards (core)"); }
    uint8_t z[0x34];
    memset(z, 0, sizeof z); put32(z, 0, 0x3f800000); put32(z, 4, 0x3f800000); put32(z, 16, 0x3f800000); put32(z, 36, 7); put32(z, 40, 1);
    CHECK(gio(ctrl, G, 3, z, 0x2c) == 0, "ZBC_SET_TABLE colour entry");
    CHECK(gio(ctrl, G, 3, z, 0x2c) == 0, "ZBC_SET_TABLE identical entry shares the slot");
    unsigned used = 0; for (int i = 0; i < NVG_ZBC_MAX; i++) used += fds[ctrl].gctl.color[i].used;
    CHECK(used == 1, "one colour slot used after two identical sets (%u)", used);
    memset(z, 0, sizeof z); put32(z, 32, 0x3f000000); put32(z, 36, 3); put32(z, 40, 2);
    CHECK(gio(ctrl, G, 3, z, 0x2c) == 0 && fds[ctrl].gctl.depth[0].used && fds[ctrl].gctl.depth[0].depth == 0x3f000000, "ZBC_SET_TABLE depth entry");
    memset(z, 0, sizeof z); put32(z, 40, 9);
    CHECK(gio(ctrl, G, 3, z, 0x2c) == NV_BAD_PARAMETER, "ZBC_SET_TABLE with an unknown type is refused");
    CHECK(gio(ctrl, G, 3, z, 0x20) == NV_BAD_PARAMETER, "ZBC_SET_TABLE short buffer");
    memset(z, 0, sizeof z); put32(z, 44, 1); put32(z, 48, 0);
    CHECK(gio(ctrl, G, 4, z, 0x34) == 0 && get32(z, 0) == 0x3f800000 && get32(z, 16) == 0x3f800000 && get32(z, 36) == 1 && get32(z, 40) == 7 && get32(z, 48) == NVG_ZBC_MAX, "ZBC_QUERY_TABLE colour index 0: values, ref count, format, table size");
    memset(z, 0, sizeof z); put32(z, 44, 2);
    CHECK(gio(ctrl, G, 4, z, 0x34) == 0 && get32(z, 32) == 0x3f000000, "ZBC_QUERY_TABLE depth index 0");
    memset(z, 0, sizeof z); put32(z, 44, 1); put32(z, 48, 5);
    CHECK(gio(ctrl, G, 4, z, 0x34) == NV_BAD_PARAMETER, "ZBC_QUERY_TABLE past the used entries is refused");
    for (int i = 0; i < NVG_ZBC_MAX + 1; i++) {
        memset(z, 0, sizeof z); put32(z, 0, 100 + i); put32(z, 40, 1);
        uint32_t e = gio(ctrl, G, 3, z, 0x2c);
        if (i == NVG_ZBC_MAX) CHECK(e == NV_INSUFFICIENT_MEMORY, "ZBC_SET_TABLE on a full table is InsufficientMemory");
    }

    { MZbcSlotMask m = {0, 0};
      CHECK(gio(ctrl, G, NVG_CTRL_ZBC_GET_ACTIVE_SLOT_MASK, &m, sizeof m) == 0 && m.slot == 7 && m.mask == 0xffff, "ZBC_GET_ACTIVE_SLOT_MASK (0x14): slot 7, every colour slot in use");
      CHECK(gio(ctrl, G, NVG_CTRL_ZBC_GET_ACTIVE_SLOT_MASK, &m, 4) == NV_BAD_PARAMETER, "ZBC_GET_ACTIVE_SLOT_MASK short buffer"); }
    { MZbcEntry q = {.type = 1, .size = 0}; /* round trip through the mirror */
      CHECK(gio(ctrl, G, NVG_CTRL_ZBC_QUERY_TABLE, &q, sizeof q) == 0 && q.color_ds[0] == 0x3f800000 && q.ref_cnt == 1 && q.format == 7 && q.size == NVG_ZBC_MAX, "ZBC_QUERY_TABLE round trip through the libnx struct"); }
    { MGetTpcMasks m = {.bufsize = 8};
      CHECK(gio(ctrl, G, NVG_CTRL_GET_TPC_MASKS, &m, sizeof m) == 0 && m.out[0] == 3, "GET_TPC_MASKS round trip: mask 3 at +16"); }
    /* ---- round 11: GET_ERROR_NOTIFICATION, EVENT_ID_CTRL, NUM_VSMS, TPC_EXCEPTION_EN_STATUS ---- */
    uint32_t gpu3 = open_("/dev/nvhost-gpu"), ctrl3 = open_("/dev/nvhost-ctrl-gpu");
    uint8_t n[0x40];
    memset(n, 0xaa, sizeof n);
    CHECK(gio(gpu3, H, 0x17, n, 8) == NV_BAD_PARAMETER, "GET_ERROR_NOTIFICATION short buffer");
    CHECK(gio(gpu3, H, 0x17, n, 16) == 0 && get64(n, 0) == 0 && get32(n, 8) == 0 && get32(n, 12) == 0, "GET_ERROR_NOTIFICATION on a healthy channel: zero timestamp, info and status");
    memset(n, 0, sizeof n); put32(n, 0, 1); put32(n, 4, 0); put32(n, 8, 99);
    CHECK(gio(gpu3, H, 0x1d, n, 16) == 0 && fds[gpu3].gchan.events == 1, "EVENT_ID_CTRL enable id 0");
    memset(n, 0, sizeof n); put32(n, 0, 1); put32(n, 4, 2); put32(n, 8, 99);
    CHECK(gio(gpu3, H, 0x1d, n, 16) == 0 && fds[gpu3].gchan.events == 5, "EVENT_ID_CTRL enable id 2 keeps id 0");
    memset(n, 0, sizeof n); put32(n, 0, 0); put32(n, 4, 0);
    CHECK(gio(gpu3, H, 0x1d, n, 16) == 0 && fds[gpu3].gchan.events == 4, "EVENT_ID_CTRL disable id 0");
    memset(n, 0, sizeof n); put32(n, 0, 2); put32(n, 4, 2);
    CHECK(gio(gpu3, H, 0x1d, n, 16) == 0 && fds[gpu3].gchan.events == 4, "EVENT_ID_CTRL clear leaves the mask alone");
    memset(n, 0, sizeof n); put32(n, 0, 1); put32(n, 4, 3);
    CHECK(gio(gpu3, H, 0x1d, n, 16) == NV_BAD_PARAMETER && fds[gpu3].gchan.events == 4, "EVENT_ID_CTRL with an unknown event id is refused");
    memset(n, 0, sizeof n); put32(n, 0, 7);
    CHECK(gio(gpu3, H, 0x1d, n, 16) == NV_BAD_PARAMETER, "EVENT_ID_CTRL with an unknown command is refused");
    CHECK(gio(gpu3, H, 0x1d, n, 8) == NV_BAD_PARAMETER, "EVENT_ID_CTRL short buffer");
    CHECK(gio(ctrl3, H, 0x17, n, 16) == NV_NOT_IMPLEMENTED && gio(ctrl3, G, 0x1d, n, 16) == NV_NOT_IMPLEMENTED, "the new channel ioctls are not answered by ctrl-gpu");
    memset(n, 0xaa, sizeof n);
    CHECK(gio(ctrl3, G, 0xd, n, 4) == NV_BAD_PARAMETER, "NUM_VSMS short buffer");
    CHECK(gio(ctrl3, G, 0xd, n, 8) == 0 && get32(n, 0) == 2 && get32(n, 4) == 0, "NUM_VSMS: two SMs (one per TPC), reserved word cleared");
    memset(n, 0xaa, sizeof n);
    CHECK(gio(ctrl3, G, 0xc, n, 4) == NV_BAD_PARAMETER, "GET_TPC_EXCEPTION_EN_STATUS short buffer");
    CHECK(gio(ctrl3, G, 0xc, n, 8) == 0 && get64(n, 0) == 0, "GET_TPC_EXCEPTION_EN_STATUS: no exception enabled");
    CHECK(gio(gpu3, G, 0xd, n, 8) == NV_NOT_IMPLEMENTED, "NUM_VSMS is not answered by the channel node");

    /* ---- unknown ioctls and other nodes keep the existing error path ---- */
    uint32_t junk[64] = {0}; /* 0xb0 bytes are passed below */
    CHECK(gio(gpu2, H, 0x7f, junk, 32) == NV_NOT_IMPLEMENTED && gio(gpu2, 'X', 1, junk, 32) == NV_NOT_IMPLEMENTED, "nvhost-gpu: unknown nr / type are NotImplemented");
    uint32_t as2 = open_("/dev/nvhost-as-gpu");
    CHECK(gio(as2, A, 0x7f, junk, 32) == NV_NOT_IMPLEMENTED && gio(as2, G, 5, junk, 32) == NV_NOT_IMPLEMENTED, "as-gpu: unknown nr / a ctrl-gpu type are NotImplemented");
    CHECK(gio(ctrl, G, 0x7f, junk, 32) == NV_NOT_IMPLEMENTED && gio(ctrl, H, 1, junk, 32) == NV_NOT_IMPLEMENTED, "ctrl-gpu: unknown nr / a channel type are NotImplemented");
    CHECK(gio(dbg, G, 5, junk, 32) == NV_NOT_IMPLEMENTED, "nvhost-dbg-gpu stays open-only");
    CHECK(ioctl_(60, cmd_id(3, 4, H, 1), junk, 4) == NV_BAD_PARAMETER, "ioctl on a closed fd is BadParameter");
    uint32_t dec = open_("/dev/nvhost-nvdec");
    uint32_t sp[2] = {0, 0};
    CHECK(ioctl_(dec, cmd_id(3, 8, 0, 2), sp, 8) == 0 && sp[1] >= SYNCPT_FIRST_DYNAMIC && sp[1] != fds[gpu2].gchan.fence_id, "media channels still allocate their own syncpoints");
    CHECK(gio(dec, G, 5, junk, 0xb0) == NV_NOT_IMPLEMENTED, "nvhost-nvdec does not answer GPU ioctls");
    printf("{\"test\":\"svc_nvdrv_nodes\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
