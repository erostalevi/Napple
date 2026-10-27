/* btm:u (nn::btm): Bluetooth management. The host has no Joy-Con radio of its own: BLE scans start and stop, report no results, and
 * connecting fails with the "not connected" result. Command ids and result description are from the author's memory of the public
 * documentation (unverified). */
#include "svc_common.h"
#include "hz_result.h"
#include <stdlib.h>

/* Per IBtmUserCore state: which BLE scans are running and the events handed out. The host has no radio, so a running scan never produces results and a
 * connection attempt never finds its device; Start/Stop, the events and the (empty) connection table are complete answers for a console with no BLE device
 * in range (class c), and the state is observable through GetBleConnectionState / the scan flags. */
enum { SCAN_GENERAL, SCAN_PAIRED, SCAN_SMART, SCAN_KINDS };
typedef struct {
    uint8_t scanning[SCAN_KINDS];
    uint32_t scan_w, conn_w; /* writable ends of the scan and connection events */
} BtmCore;
static void core_acquire_scan_event(IpcObject *s, IpcCall *c) {
    BtmCore *b = s->state;
    uint32_t r = 0, w = event_create(&r);
    if (!b->scan_w) b->scan_w = w;
    uint8_t ok = 1;
    IPC_OUT(c, ok);
    ipc_out_copy_handle(c, r);
}
static void core_acquire_conn_event(IpcObject *s, IpcCall *c) {
    BtmCore *b = s->state;
    uint32_t r = 0, w = event_create(&r);
    if (!b->conn_w) b->conn_w = w;
    ipc_out_copy_handle(c, r);
}
static void scan_set(IpcObject *s, int kind, int on) {
    BtmCore *b = s->state;
    b->scanning[kind] = (uint8_t)on;
}
static void core_start_general(IpcObject *s, IpcCall *c) {
    (void)c;
    scan_set(s, SCAN_GENERAL, 1);
}
static void core_stop_general(IpcObject *s, IpcCall *c) {
    (void)c;
    scan_set(s, SCAN_GENERAL, 0);
}
static void core_start_paired(IpcObject *s, IpcCall *c) {
    (void)c;
    scan_set(s, SCAN_PAIRED, 1);
}
static void core_stop_paired(IpcObject *s, IpcCall *c) {
    (void)c;
    scan_set(s, SCAN_PAIRED, 0);
}
static void core_start_smart(IpcObject *s, IpcCall *c) {
    (void)c;
    scan_set(s, SCAN_SMART, 1);
}
static void core_stop_smart(IpcObject *s, IpcCall *c) {
    (void)c;
    scan_set(s, SCAN_SMART, 0);
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
static void core_disconnect(IpcObject *s, IpcCall *c) { /* nothing is connected, so there is nothing to drop; wake a waiter so it re-reads the (empty) table */
    BtmCore *b = s->state;
    (void)c;
    if (b->conn_w) event_signal_handle(b->conn_w);
}
static const IpcCmd core_cmds[] = {
    {0, "AcquireBleScanEvent", core_acquire_scan_event, IPC_IMPL},
    {3, "StartBleScanForGeneral", core_start_general, IPC_IMPL},
    {4, "StopBleScanForGeneral", core_stop_general, IPC_IMPL},
    {5, "GetBleScanResultsForGeneral", core_no_results, IPC_IMPL},
    {6, "StartBleScanForPaired", core_start_paired, IPC_IMPL},
    {7, "StopBleScanForPaired", core_stop_paired, IPC_IMPL},
    {8, "StartBleScanForSmartDevice", core_start_smart, IPC_IMPL},
    {9, "StopBleScanForSmartDevice", core_stop_smart, IPC_IMPL},
    {10, "GetBleScanResultsForSmartDevice", core_no_results, IPC_IMPL},
    {17, "AcquireBleConnectionEvent", core_acquire_conn_event, IPC_IMPL},
    {18, "BleConnect", core_connect_failed, IPC_IMPL},
    {19, "BleOverrideConnection", core_connect_failed, IPC_IMPL},
    {20, "BleDisconnect", core_disconnect, IPC_IMPL},
    {21, "BleGetConnectionState", core_no_results, IPC_IMPL},
};
static void free_state(IpcObject *o) { free(o->state); }
static void btm_get_core(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IBtmUserCore", core_cmds, calloc(1, sizeof(BtmCore)), free_state));
}
static const IpcCmd btm_cmds[] = {{0, "GetCore", btm_get_core, IPC_IMPL}};
static IpcObject *btm_open(void) { return IPC_OBJECT("btm:u", btm_cmds, NULL, NULL); }
void service_register_btm(void) { ipc_register_service("btm:u", btm_open); }
