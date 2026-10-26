/* btm:u (nn::btm): Bluetooth management. The host has no Joy-Con radio of its own: BLE scans start and stop, report no results, and
 * connecting fails with the "not connected" result. Command ids and result description are from the author's memory of the public
 * documentation (unverified). */
#include "svc_common.h"
#include "hz_result.h"

static void core_acquire_scan_event(IpcObject *s, IpcCall *c) {
    uint8_t ok = 1;
    IPC_OUT(c, ok);
    svc_ret_event(s, c);
}
static void core_no_results(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t n = 0;
    IPC_OUT(c, n);
}
static void core_connect_failed(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = HZ_RESULT(HZ_MOD_BTM, 4);
} /* [unverified] */
static const IpcCmd core_cmds[] = {
    {0, "AcquireBleScanEvent", core_acquire_scan_event, IPC_STUB},
    {3, "StartBleScanForGeneral", svc_noop, IPC_STUB},
    {4, "StopBleScanForGeneral", svc_noop, IPC_STUB},
    {5, "GetBleScanResultsForGeneral", core_no_results, IPC_STUB},
    {6, "StartBleScanForPaired", svc_noop, IPC_STUB},
    {7, "StopBleScanForPaired", svc_noop, IPC_STUB},
    {8, "StartBleScanForSmartDevice", svc_noop, IPC_STUB},
    {9, "StopBleScanForSmartDevice", svc_noop, IPC_STUB},
    {10, "GetBleScanResultsForSmartDevice", core_no_results, IPC_STUB},
    {17, "AcquireBleConnectionEvent", svc_ret_event, IPC_STUB},
    {18, "BleConnect", core_connect_failed, IPC_STUB},
    {19, "BleOverrideConnection", core_connect_failed, IPC_STUB},
    {20, "BleDisconnect", svc_noop, IPC_STUB},
    {21, "BleGetConnectionState", core_no_results, IPC_STUB},
};
static void btm_get_core(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IBtmUserCore", core_cmds, NULL, NULL));
}
static const IpcCmd btm_cmds[] = {{0, "GetCore", btm_get_core, IPC_IMPL}};
static IpcObject *btm_open(void) { return IPC_OBJECT("btm:u", btm_cmds, NULL, NULL); }
void service_register_btm(void) { ipc_register_service("btm:u", btm_open); }
