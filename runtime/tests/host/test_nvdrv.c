/* Host unit tests: nvdrv host1x channels (nvhost-nvdec / vic / nvjpg / msenc) through the service's own IPC handlers: open, nvmap
 * handles, SET_NVMAP_FD, GET_SYNCPOINT (allocation, per channel, released on close), SUBMIT (validation, syncpoint increments, fence
 * thresholds, nvhost-ctrl reading the new value), MAP/UNMAP_CMD_BUFFER (stable device addresses) and the error returns. */
#include "../../src/services/nvdrv.c"
#include "host_stubs.h"
#include "../check.h"

static IpcObject *nv;
static IpcCall call;
static uint8_t io[0x400];
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
static uint32_t open_(const char *path, uint32_t *err) {
    memset(&call, 0, sizeof call);
    static char p[64]; snprintf(p, sizeof p, "%s", path);
    call.object = nv; call.na = 1; call.a[0] = (IpcBuf){(uintptr_t)p, strlen(p), 0};
    nv_open(nv, &call);
    uint32_t fd; memcpy(&fd, call.out, 4); memcpy(err, call.out + 4, 4);
    return fd;
}
static uint32_t close_(uint32_t fd) {
    memset(&call, 0, sizeof call); args[0] = fd; call.object = nv; call.in = (const uint8_t *)args; call.in_size = 4;
    nv_close(nv, &call); uint32_t e; memcpy(&e, call.out, 4); return e;
}

int main(void) {
    host_thread_attach(calloc(1, 0x200));
    nv = nv_new();
    uint32_t err;
    uint32_t map = open_("/dev/nvmap", &err), ctrl = open_("/dev/nvhost-ctrl", &err);
    uint32_t dec = open_("/dev/nvhost-nvdec", &err);
    CHECK(err == NV_OK && dec && fds[dec].kind == DEV_CHANNEL && fds[dec].ch.class_id == 0xF0, "nvhost-nvdec opens as a host1x channel");
    uint32_t vic = open_("/dev/nvhost-vic", &err), jpg = open_("/dev/nvhost-nvjpg", &err), enc = open_("/dev/nvhost-msenc", &err);
    CHECK(vic && jpg && enc && fds[vic].ch.class_id == 0x5D && fds[jpg].ch.class_id == 0xC0 && fds[enc].ch.class_id == 0x21, "vic, nvjpg, msenc channels");
    open_("/dev/nvhost-nope", &err);
    CHECK(err == NV_NOT_SUPPORTED, "unknown device refused");

    /* an nvmap handle for the command buffer */
    uint32_t create[2] = {0x2000, 0};
    CHECK(ioctl_(map, cmd_id(3, 8, 1, 0x01), create, 8) == NV_OK && create[1], "nvmap CREATE");
    uint32_t handle = create[1];

    int32_t mfd = (int32_t)map;
    CHECK(ioctl_(dec, cmd_id(1, 4, 0x48, 0x01), &mfd, 4) == NV_OK && fds[dec].ch.nvmap_fd == (int32_t)map, "SET_NVMAP_FD");
    uint32_t to = 1000;
    CHECK(ioctl_(dec, cmd_id(1, 4, 0x48, 0x03), &to, 4) == NV_OK && fds[dec].ch.timeout_ms == 1000, "SET_TIMEOUT");
    CHECK(ioctl_(dec, cmd_id(1, 4, 0x48, 0x08), &to, 4) == NV_NOT_IMPLEMENTED, "GPU channel ioctls are not host1x ones");
    CHECK(ioctl_(dec, cmd_id(1, 4, 0x00, 0x07), &to, 4) == NV_OK && fds[dec].ch.submit_timeout_ms == 1000, "SET_SUBMIT_TIMEOUT");

    uint32_t gs[2] = {0, 0};
    CHECK(ioctl_(dec, cmd_id(3, 8, 0, 0x02), gs, 8) == NV_OK && gs[1] >= SYNCPT_FIRST_DYNAMIC, "GET_SYNCPOINT allocates a syncpoint");
    uint32_t sp = gs[1]; gs[1] = 0;
    CHECK(ioctl_(dec, cmd_id(3, 8, 0, 0x02), gs, 8) == NV_OK && gs[1] == sp, "same index, same syncpoint");
    uint32_t gv[2] = {0, 0};
    CHECK(ioctl_(vic, cmd_id(3, 8, 0, 0x02), gv, 8) == NV_OK && gv[1] && gv[1] != sp, "another channel gets its own");
    gs[0] = 16;
    CHECK(ioctl_(dec, cmd_id(3, 8, 0, 0x02), gs, 8) == NV_BAD_PARAMETER, "syncpoint index out of range");
    uint32_t wb[2] = {0, 7};
    CHECK(ioctl_(dec, cmd_id(3, 8, 0, 0x03), wb, 8) == NV_OK && wb[1] == 0, "GET_WAITBASE");

    /* SUBMIT: one command buffer, no relocs, one syncpoint increment of 3, one fence */
    uint8_t sub[16 + 12 + 20 + 4] = {0};
    uint32_t hdr[4] = {1, 0, 1, 1}; memcpy(sub, hdr, 16);
    uint32_t cb[3] = {handle, 0x100, 16}; memcpy(sub + 16, cb, 12);
    uint32_t incr[5] = {sp, 3, 0, 0, 0}; memcpy(sub + 28, incr, 20);
    uint32_t before = syncpt[sp];
    CHECK(ioctl_(dec, cmd_id(3, 16, 0, 0x01), sub, sizeof sub) == NV_OK, "SUBMIT");
    uint32_t fence; memcpy(&fence, sub + 48, 4);
    CHECK(syncpt[sp] == before + 3 && fence == syncpt_max[sp] && fence == before + 3, "increments landed, fence threshold returned");
    uint32_t rd[2] = {sp, 0};
    CHECK(ioctl_(ctrl, cmd_id(3, 8, 0, 0x14), rd, 8) == NV_OK && rd[1] == fence, "nvhost-ctrl SYNCPT_READ sees the completed value");
    uint32_t wait[4] = {sp, fence, 100, 0};
    CHECK(ioctl_(ctrl, cmd_id(3, 16, 0, 0x1d), wait, 16) == NV_OK, "EVENT_WAIT on a reached threshold returns at once");

    uint8_t bad[sizeof sub]; memcpy(bad, sub, sizeof sub);
    uint32_t cb_bad[3] = {handle, 0x1ff0, 16}; memcpy(bad + 16, cb_bad, 12);
    CHECK(ioctl_(dec, cmd_id(3, 16, 0, 0x01), bad, sizeof bad) == NV_BAD_PARAMETER, "command buffer beyond its nvmap handle");
    memcpy(bad, sub, sizeof sub); cb_bad[0] = 0x7777; cb_bad[1] = 0; memcpy(bad + 16, cb_bad, 12);
    CHECK(ioctl_(dec, cmd_id(3, 16, 0, 0x01), bad, sizeof bad) == NV_BAD_PARAMETER, "command buffer in an unknown handle");
    memcpy(bad, sub, sizeof sub); uint32_t many[4] = {1, 0, 1, 9}; memcpy(bad, many, 16);
    CHECK(ioctl_(dec, cmd_id(3, 16, 0, 0x01), bad, sizeof bad) == NV_BAD_PARAMETER, "arrays larger than the ioctl buffer");
    memcpy(bad, sub, sizeof sub); uint32_t bad_incr[5] = {NSYNC, 1, 0, 0, 0}; memcpy(bad + 28, bad_incr, 20);
    CHECK(ioctl_(dec, cmd_id(3, 16, 0, 0x01), bad, sizeof bad) == NV_BAD_PARAMETER && syncpt[sp] == fence, "bad syncpoint id: nothing increments");

    /* MAP / UNMAP_CMD_BUFFER */
    uint32_t mb[3 + 2] = {1, 0, 0, handle, 0};
    CHECK(ioctl_(jpg, cmd_id(3, 12, 0, 0x09), mb, sizeof mb) == NV_OK && mb[4] >= 0x10000000u && !(mb[4] & 0xfff), "MAP_CMD_BUFFER gives a device address");
    uint32_t iova = mb[4]; mb[4] = 0;
    CHECK(ioctl_(enc, cmd_id(3, 12, 0, 0x09), mb, sizeof mb) == NV_OK && mb[4] == iova, "stable address per handle across channels");
    CHECK(ioctl_(enc, cmd_id(3, 12, 0, 0x0a), mb, sizeof mb) == NV_OK, "UNMAP_CMD_BUFFER");
    uint32_t mb_bad[5] = {1, 0, 0, 0x9999, 0};
    CHECK(ioctl_(jpg, cmd_id(3, 12, 0, 0x09), mb_bad, sizeof mb_bad) == NV_BAD_PARAMETER, "map of an unknown handle");
    uint32_t mb_short[5] = {3, 0, 0, handle, 0};
    CHECK(ioctl_(jpg, cmd_id(3, 12, 0, 0x09), mb_short, sizeof mb_short) == NV_BAD_PARAMETER, "entry count beyond the buffer");

    /* close releases the channel's syncpoints */
    CHECK(close_(dec) == NV_OK && !syncpt_owned[sp], "close returns the syncpoints");
    CHECK(ioctl_(dec, cmd_id(3, 8, 0, 0x02), gs, 8) == NV_BAD_PARAMETER, "ioctl on a closed fd");
    CHECK(close_(dec) == NV_BAD_PARAMETER, "close twice");
    printf("{\"test\":\"nvdrv\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
