/* Unit tests: hidbus has no devices. */
#include "../src/services/hidbus.c"
#include "svc_test.h"

int main(void) {
    HidbusHandle h; memset(&h, 0xff, sizeof h);
    CHECK(hidbus_find_handle(0, HIDBUS_RIGHT_JOY_RAIL, &h) == 0 && h.is_valid == 0 && h.abstracted_pad_id == 0, "no handle is found");
    CHECK(hidbus_bus_type_valid(2) && !hidbus_bus_type_valid(3), "bus types");
    CHECK(sizeof(HidbusHandle) == 8, "handle is 8 bytes");
    IpcObject *o = hb_open(); IpcCall c; uint8_t in[16] = {0}; in[4] = HIDBUS_RIGHT_JOY_RAIL;
    svc_call(o, 1, in, sizeof in, &c); CHECK(c.result == 0 && c.out_size == 12 && c.out[0] == 0 && SVC_OUT(&c, uint32_t, 4) == 0 && SVC_OUT(&c, uint32_t, 8) == 0, "GetBusHandle: not found, zero handle (u8 at 0, handle at 4)");
    in[4] = 9; svc_call(o, 1, in, sizeof in, &c); CHECK(c.result == 0 && c.out[0] == 0, "invalid bus type: not found");
    svc_call(o, 2, in, 16, &c); CHECK(c.out[0] == 0, "IsExternalDeviceConnected = false");
    svc_call(o, 3, in, 16, &c); CHECK(c.result == HZ_R_HID_NOT_CONNECTED, "Initialize: not connected");
    svc_call(o, 4, in, 16, &c); CHECK(c.result == 0, "Finalize is harmless");
    svc_call(o, 10, NULL, 0, &c); CHECK(c.result == 0 && c.nout_copy == 1 && handle_get(c.out_copy[0], OBJ_SHMEM), "shared memory handle");
    svc_call(o, 9, NULL, 0, &c); CHECK(c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "command-result event never fires");
    uint8_t hin[8] = {0}; HidbusHandle dh;
    CHECK(hidbus_handle_decode(hin, 8, 0, &dh) == 0 && hidbus_handle_decode(hin, 7, 0, &dh) < 0, "handle decode needs 8 bytes");
    svc_call(o, 5, hin, 8, &c); CHECK(c.result == HZ_R_HID_NOT_CONNECTED, "EnableExternalDevice: not connected");
    svc_call(o, 6, hin, 8, &c); CHECK(c.result == HZ_R_HID_NOT_CONNECTED, "GetExternalDeviceId: not connected");
    svc_call(o, 7, hin, 8, &c); CHECK(c.result == HZ_R_HID_NOT_CONNECTED, "SendCommandAsync: not connected");
    svc_call(o, 11, hin, 8, &c); CHECK(c.result == HZ_R_HID_NOT_CONNECTED, "EnableJoyPollingReceiveMode: not connected");
    svc_call(o, 13, hin, 8, &c); CHECK(c.result == HZ_R_HID_NOT_CONNECTED, "GetPollingData: not connected");
    svc_call(o, 12, hin, 8, &c); CHECK(c.result == 0, "DisableJoyPollingReceiveMode is harmless");
    uint32_t t = 3; svc_call(o, 14, &t, 4, &c); CHECK(c.result == 0 && ((HbState *)o->state)->status_manager_type == 3, "SetStatusManagerType is remembered");
    printf("{\"test\":\"svc_hidbus\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
