/* Unit tests: irs camera handles and the no-camera answers. */
#include "../src/services/irs.c"
#include "svc_test.h"

int main(void) {
    uint32_t h = 99;
    CHECK(irs_camera_handle(0, &h) == 0 && h == 0 && irs_camera_handle(7, &h) == 0 && h == 7, "players 1 and 8");
    CHECK(irs_camera_handle(0x20, &h) == 0 && h == 8 && irs_camera_handle(0x10, &h) == 0 && h == 9, "handheld and other");
    CHECK(irs_camera_handle(8, &h) == -1 && irs_camera_handle(0x21, &h) == -1, "not an npad id");
    IpcObject *o = irs_open(); IpcCall c;
    svc_call(o, 302, NULL, 0, &c); CHECK(c.result == 0, "ActivateIrsensor");
    svc_call(o, 304, NULL, 0, &c); CHECK(c.result == 0 && c.nout_copy == 1, "shared memory handle");
    IpcSharedMemory *m = handle_get(c.out_copy[0], OBJ_SHMEM); CHECK(m && m->size >= 0x8000, "shared memory is at least 0x8000 bytes");
    int zero = 1; for (uint64_t i = 0; m && i < 0x8000; i++) if (((uint8_t *)m->host)[i]) zero = 0; CHECK(zero, "shared memory is zeroed");
    uint32_t id = 0x20; svc_call(o, 311, &id, 4, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 8, "GetNpadIrCameraHandle(handheld)");
    id = 0x55; svc_call(o, 311, &id, 4, &c); CHECK(c.result != 0, "GetNpadIrCameraHandle with a bad id fails");
    svc_call(o, 306, NULL, 0, &c); CHECK(c.result == HZ_R_IRSENSOR_NOT_CONNECTED, "RunMomentProcessor: not connected");
    svc_call(o, 309, NULL, 0, &c); CHECK(c.result == HZ_R_IRSENSOR_NOT_CONNECTED, "GetImageTransferProcessorState: not connected");
    svc_call(o, 305, NULL, 0, &c); CHECK(c.result == 0, "StopImageProcessor succeeds");
    printf("{\"test\":\"svc_irs\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
