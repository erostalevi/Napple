/* hidbus (nn::hidbus): devices on the Joy-Con rail (Ring-Con, ...). None are attached: GetBusHandle reports "not found", device queries
 * say disconnected, and Initialize / commands report the HID "not connected" result. The shared-memory block is zeroed. Command ids
 * and the result description are from the author's memory of the public documentation (unverified). */
#include "svc_common.h"
#include "hidbus_core.h"
#include "hz_result.h"

static IpcSharedMemory *shm;
static void hb_get_handle(IpcObject *s, IpcCall *c) { /* 1(u32 npad id, u32 bus type, u64 aruid) -> {bool found, BusHandle} */
    (void)s;
    HidbusHandle h;
    uint32_t type = IPC_IN(c, uint32_t, 4);
    uint8_t found = (uint8_t)(hidbus_bus_type_valid(type) ? hidbus_find_handle(IPC_IN(c, uint32_t, 0), type, &h) : 0);
    if (!found) memset(&h, 0, sizeof h);
    IPC_OUT(c, found);
    ipc_out(c, &h.abstracted_pad_id, 4);
    ipc_out(c, &h.player_number, 4); /* handle fields: 4-aligned, as a struct would be */
}
static void hb_not_connected(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = HZ_R_HID_NOT_CONNECTED;
}
static void hb_get_shmem(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!shm) shm = ipc_shared_memory_new(0x1000);
    ipc_out_copy_handle(c, ipc_shared_memory_handle(shm));
}
static const IpcCmd hb_cmds[] = {
    {1, "GetBusHandle", hb_get_handle, IPC_IMPL},
    {2, "IsExternalDeviceConnected", svc_ret_u8_0, IPC_IMPL},
    {3, "Initialize", hb_not_connected, IPC_STUB},
    {4, "Finalize", svc_noop, IPC_STUB},
    {5, "EnableExternalDevice", hb_not_connected, IPC_STUB},
    {6, "GetExternalDeviceId", hb_not_connected, IPC_STUB},
    {7, "SendCommandAsync", hb_not_connected, IPC_STUB},
    {8, "GetSendCommandAsynceResult", hb_not_connected, IPC_STUB},
    {9, "SetEventForSendCommandAsycResult", svc_ret_event, IPC_STUB},
    {10, "GetSharedMemoryHandle", hb_get_shmem, IPC_STUB},
    {11, "EnableJoyPollingReceiveMode", hb_not_connected, IPC_STUB},
    {12, "DisableJoyPollingReceiveMode", svc_noop, IPC_STUB},
    {13, "GetPollingData", hb_not_connected, IPC_STUB},
    {14, "SetStatusManagerType", svc_noop, IPC_STUB},
};
static IpcObject *hb_open(void) { return IPC_OBJECT("hidbus", hb_cmds, NULL, NULL); }
void service_register_hidbus(void) { ipc_register_service("hidbus", hb_open); }
