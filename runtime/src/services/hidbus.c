/* hidbus (nn::hidbus): devices on the Joy-Con rail (Ring-Con, ...). None are attached: GetBusHandle reports "not found", device queries
 * say disconnected, and Initialize / commands report the HID "not connected" result. The shared-memory block is zeroed. Command ids
 * and the result description are from the author's memory of the public documentation (unverified). */
#include "svc_common.h"
#include "hidbus_core.h"
#include "hz_result.h"
#include <stdlib.h>

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
/* Commands that take a bus handle first: a handle that does not name an attached device (all of them, on this host) answers "not connected", the same
 * result the console gives for a Joy-Con rail with nothing plugged in. A handle too short to decode is also answered, never fatal. */
static void hb_not_connected(IpcObject *s, IpcCall *c) {
    (void)s;
    HidbusHandle h;
    if (hidbus_handle_decode(c->in, c->in_size, 0, &h) == 0 && hidbus_handle_attached(&h)) return; /* unreachable: no device can attach */
    c->result = HZ_R_HID_NOT_CONNECTED;
}
/* Per-session state the console keeps for the status manager: the type last set (nothing else observes it without a device) and whether
 * joy polling is on (it can only be turned on for an attached device, so it stays off here). */
typedef struct {
    uint32_t status_manager_type;
} HbState;
static void hb_set_status_manager_type(IpcObject *s, IpcCall *c) {
    HbState *st = s->state;
    if (c->in_size >= 4) st->status_manager_type = IPC_IN(c, uint32_t, 0);
}
static void hb_get_shmem(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!shm) shm = ipc_shared_memory_new(0x1000);
    ipc_out_copy_handle(c, ipc_shared_memory_handle(shm));
}
/* Finalize / DisableJoyPollingReceiveMode on a handle that is not attached: nothing to tear down, success. */
static void hb_finalize(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static const IpcCmd hb_cmds[] = {
    {1, "GetBusHandle", hb_get_handle, IPC_IMPL},
    {2, "IsExternalDeviceConnected", svc_ret_u8_0, IPC_IMPL},
    {3, "Initialize", hb_not_connected, IPC_IMPL},
    {4, "Finalize", hb_finalize, IPC_IMPL},
    {5, "EnableExternalDevice", hb_not_connected, IPC_IMPL},
    {6, "GetExternalDeviceId", hb_not_connected, IPC_IMPL},
    {7, "SendCommandAsync", hb_not_connected, IPC_IMPL},
    {8, "GetSendCommandAsynceResult", hb_not_connected, IPC_IMPL},
    {9, "SetEventForSendCommandAsycResult", svc_ret_event, IPC_IMPL},
    {10, "GetSharedMemoryHandle", hb_get_shmem, IPC_IMPL},
    {11, "EnableJoyPollingReceiveMode", hb_not_connected, IPC_IMPL},
    {12, "DisableJoyPollingReceiveMode", hb_finalize, IPC_IMPL},
    {13, "GetPollingData", hb_not_connected, IPC_IMPL},
    {14, "SetStatusManagerType", hb_set_status_manager_type, IPC_IMPL},
};
static void hb_destroy(IpcObject *o) { free(o->state); }
static IpcObject *hb_open(void) { return IPC_OBJECT("hidbus", hb_cmds, calloc(1, sizeof(HbState)), hb_destroy); }
void service_register_hidbus(void) { ipc_register_service("hidbus", hb_open); }
