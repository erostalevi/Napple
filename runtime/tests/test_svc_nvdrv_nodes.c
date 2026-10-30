/* Unit tests: the GPU device nodes of nvdrv (nvdrv_nodes_core behind nvdrv.c): /dev/nvhost-gpu channel state, /dev/nvhost-as-gpu address space,
 * /dev/nvhost-ctrl-gpu queries, through the real ioctl handlers. Layouts are the PROPOSED ones of nvdrv_nodes_core.h (unverified against real traces). */
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

static uint32_t nvmap_handle(uint32_t nm, uint32_t size) {
    uint32_t c[2] = {size, 0};
    ioctl_(nm, cmd_id(3, 8, 1, 0x01), c, 8);
    return c[1];
}
static uint32_t as_init(uint32_t as) { uint8_t b[0x28] = {0}; return gio(as, A, 9, b, 0x28); }

int main(void) {
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
    put64(b, 0, 0x1122334455667788ull);
    CHECK(gio(gpu, H, 0x14, b, 8) == 0, "SET_USER_DATA");
    memset(b, 0, 8);
    CHECK(gio(gpu, H, 0x15, b, 8) == 0 && get64(b, 0) == 0x1122334455667788ull, "GET_USER_DATA returns what was set");
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
    memset(b, 0, sizeof b); put32(b, 0, 0x4321);
    CHECK(gio(as, A, 9, b, 0x28) == NV_BAD_PARAMETER && !fds[as].gas.initialized, "INITIALIZE_EX with a bad big page size is refused");
    memset(b, 0, sizeof b); put64(b, 16, 0x2000); put64(b, 24, 0x1000000); put64(b, 32, 0x100000); /* start not below split */
    put64(b, 16, 0x2000000);
    CHECK(gio(as, A, 9, b, 0x28) == NV_BAD_PARAMETER, "INITIALIZE_EX with start >= split is refused");
    CHECK(as_init(as) == 0 && fds[as].gas.initialized && fds[as].gas.big_page_size == 0x10000, "INITIALIZE_EX with defaults: 64K big pages");
    CHECK(as_init(as) == NV_INVALID_STATE, "INITIALIZE_EX twice is InvalidState");
    memset(b, 0, sizeof b); put32(b, 8, 0x40);
    CHECK(gio(as, A, 8, b, 0x40) == 0 && get32(b, 8) == 0x30, "GET_VA_REGIONS reports 0x30 bytes of regions");
    CHECK(get64(b, 16) == 0x04000000ull && get32(b, 24) == 0x1000 && get64(b, 32) == (0x100000000ull - 0x04000000ull) / 0x1000, "small-page region: start, 4K pages, count");
    CHECK(get64(b, 40) == 0x100000000ull && get32(b, 48) == 0x10000 && get64(b, 56) == (0x2000000000ull - 0x100000000ull) / 0x10000, "big-page region: split, 64K pages, count");
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
    CHECK(gio(as, A, 4, b, 0x28) == NV_BAD_PARAMETER, "MAP_BUFFER_EX on a missing nvmap handle is refused");
    memset(b, 0, sizeof b); put32(b, 4, 0xfe); put32(b, 8, hnd); put32(b, 12, 0x10000);
    CHECK(gio(as, A, 4, b, 0x28) == 0, "MAP_BUFFER_EX (not fixed)");
    uint64_t m1 = get64(b, 32);
    CHECK(m1 && !(m1 & 0xffff) && 1, "MAP_BUFFER_EX returns an offset (%#llx)", (unsigned long long)m1);
    CHECK(!(m1 < r1 + 16 * 0x10000 && r1 < m1 + 0x20000) && !(m1 < r2 + 16 * 0x10000 && r2 < m1 + 0x20000), "a non-fixed map avoids the reservations");
    unsigned live = 0; for (unsigned i = 0; i < NVG_MAX_MAPS; i++) live += fds[as].gas.maps[i].used;
    CHECK(live == 1 && fds[as].gas.maps[0].handle == hnd && fds[as].gas.maps[0].kind == 0xfe && fds[as].gas.maps[0].size == 0x20000, "the mapping is tracked (handle, kind, size)");
    memset(b, 0, sizeof b); put32(b, 8, small); put32(b, 12, 0); /* page size 0: 4K for a small buffer */
    CHECK(gio(as, A, 4, b, 0x28) == 0 && fds[as].gas.maps[1].page_size == 0x1000 && fds[as].gas.maps[1].size == 0x3000 && get64(b, 32) != m1, "page size 0 picks 4K for a small buffer, size from the handle");
    uint64_t m2 = get64(b, 32);
    memset(b, 0, sizeof b); put32(b, 0, 1); put32(b, 8, hnd); put32(b, 12, 0x10000); put64(b, 32, m1);
    CHECK(gio(as, A, 4, b, 0x28) == NV_BAD_PARAMETER, "fixed map outside a reservation is refused");
    memset(b, 0, sizeof b); put32(b, 0, 1); put32(b, 8, hnd); put32(b, 12, 0x10000); put64(b, 32, r1);
    CHECK(gio(as, A, 4, b, 0x28) == 0 && get64(b, 32) == r1, "fixed map inside a reservation keeps its offset");
    memset(b, 0, sizeof b); put32(b, 0, 1); put32(b, 8, hnd); put32(b, 12, 0x10000); put64(b, 32, r1 + 0x10000);
    CHECK(gio(as, A, 4, b, 0x28) == NV_BAD_PARAMETER, "fixed map overlapping an existing map is refused");
    memset(b, 0, sizeof b); put32(b, 8, hnd); put32(b, 12, 0x10000); put64(b, 16, 0x30000);
    CHECK(gio(as, A, 4, b, 0x28) == NV_BAD_PARAMETER, "map with a buffer offset past the handle is refused");
    memset(b, 0, sizeof b); put32(b, 8, hnd); put32(b, 12, 0x10000); put64(b, 24, 0x30000);
    CHECK(gio(as, A, 4, b, 0x28) == NV_BAD_PARAMETER, "map longer than the handle is refused");
    memset(b, 0, sizeof b); put32(b, 8, hnd); put32(b, 12, 0x3000);
    CHECK(gio(as, A, 4, b, 0x28) == NV_BAD_PARAMETER, "map with an unsupported page size is refused");
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
    uint32_t c1[4] = {fid, 0, 0, 0};
    CHECK(ioctl_(open_("/dev/nvhost-ctrl"), cmd_id(3, 8, 0, 0x14), c1, 8) == 0 && c1[1] == 2, "the fence is visible through nvhost-ctrl SYNCPT_READ");

    /* ---- closing the address space unbinds its channels; closing the channel frees its syncpoint ---- */
    close_(as);
    CHECK(fds[gpu].gchan.as == NULL, "closing the address space unbinds the channel");
    uint32_t gpu2 = open_("/dev/nvhost-gpu");
    memset(b, 0, sizeof b); put32(b, 0, 16);
    CHECK(gio(gpu2, H, 0x1a, b, 0x20) == NV_INVALID_STATE, "a fresh channel starts unbound with no state");
    CHECK(fds[gpu2].gchan.priority == 100 && fds[gpu2].gchan.classes[0] == 0 && fds[gpu2].gchan.fence_id != fid, "fresh channel state and its own fence syncpoint");
    close_(gpu);
    CHECK(!syncpt_owned[fid], "closing the channel releases its fence syncpoint");

    /* ---- nvhost-ctrl-gpu ---- */
    memset(b, 0, sizeof b);
    CHECK(gio(ctrl, G, 1, b, 4) == 0 && get32(b, 0) == 0x1000, "ZCULL_GET_CTX_SIZE");
    memset(b, 0, sizeof b);
    CHECK(gio(ctrl, G, 2, b, 0x28) == 0 && get32(b, 0) == 16 && get32(b, 4) == 32 && get32(b, 36) == 0x10, "ZCULL_GET_INFO");
    CHECK(gio(ctrl, G, 2, b, 0x20) == NV_BAD_PARAMETER, "ZCULL_GET_INFO short buffer");
    memset(b, 0, sizeof b);
    CHECK(gio(ctrl, G, 5, b, 0xb0) == 0 && get64(b, 0) == 0xa0 && get32(b, 0x10) == 0x120 && get32(b, 0x14) == 0xb && get32(b, 0x50) == 0x902d && get32(b, 0x54) == 0xb197, "GET_CHARACTERISTICS keeps the retail values");
    CHECK(get64(b, 0x10 + 0x88) == 0x6230326d67 && get32(b, 0x10 + 0x40 + 24) == 1, "GET_CHARACTERISTICS chip name and GPC mask");
    CHECK(gio(ctrl, G, 5, b, 0x20) == NV_NOT_IMPLEMENTED, "GET_CHARACTERISTICS with a short buffer is unchanged (NotImplemented)");
    memset(b, 0, sizeof b);
    CHECK(gio(ctrl, G, 6, b, 0x18) == 0 && get64(b, 16) == 3, "GET_TPC_MASKS reports two TPCs");
    memset(b, 0, sizeof b); put32(b, 0, 1); put32(b, 4, 1);
    CHECK(gio(ctrl, G, 7, b, 8) == 0 && gio(ctrl, G, 7, b, 8) == 0 && fds[ctrl].gctl.l2_flushes == 2, "FLUSH_L2 is counted");
    uint64_t t[2] = {0, 0}, t2[2] = {0, 0};
    CHECK(gio(ctrl, G, 0x14, t, 16) == 0 && gio(ctrl, G, 0x14, t2, 16) == 0 && t2[0] >= t[0], "GET_GPU_TIME is monotonic");
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
