/* btm:u (nn::btm): Bluetooth management. The host has no Joy-Con radio of its own: BLE scans start and stop, report no results, and
 * connecting fails with the "not connected" result. Command ids follow switchbrew's IBtmUserCore since 2026-11-01 (19-21 were one off before:
 * 19 BleDisconnect, 20 BleGetConnectionState, 21 AcquireBlePairingEvent); the result description is unverified. */
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
    uint32_t pair_w, discovery_w, mtu_w;
    unsigned data_paths; /* RegisterBleGattDataPath - Unregister */
} BtmCore;
static void core_acquire_scan_event(IpcObject *s, IpcCall *c) {
    BtmCore *b = s->state;
    uint32_t r = 0, w = event_create(&r);
    if (!b->scan_w) b->scan_w = w;
    uint8_t ok = 1;
    IPC_OUT(c, ok);
    ipc_out_copy_handle(c, r);
}
/* The Acquire*Event commands answer {bool success} and the event (libnx reads the flag for every one of them). */
static void acquire_event(IpcCall *c, uint32_t *slot) {
    uint32_t r = 0, w = event_create(&r);
    if (!*slot) *slot = w;
    uint8_t ok = 1;
    IPC_OUT(c, ok);
    ipc_out_copy_handle(c, r);
}
static void core_acquire_conn_event(IpcObject *s, IpcCall *c) { acquire_event(c, &((BtmCore *)s->state)->conn_w); }
static void core_acquire_pair_event(IpcObject *s, IpcCall *c) { acquire_event(c, &((BtmCore *)s->state)->pair_w); }
static void core_acquire_discovery_event(IpcObject *s, IpcCall *c) { acquire_event(c, &((BtmCore *)s->state)->discovery_w); }
static void core_acquire_mtu_event(IpcObject *s, IpcCall *c) { acquire_event(c, &((BtmCore *)s->state)->mtu_w); }
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
/* Scan filter parameters (1: BleAdvertisePacketParameter, 2: GattAttributeUuid) are constants the system keeps per parameter id; their values are not
 * known here, so an all-zero filter is answered (documented stub: a scan with it would match nothing, which is what the host's scans find anyway). */
static void core_filter_param(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_zero(c, c->cmd == 1 ? 0x10 : 0x14); /* [unverified sizes] */
}
static void core_unpair(IpcObject *s, IpcCall *c) { /* 23 / 24: nothing is paired, so there is nothing to remove */
    (void)s;
    (void)c;
}
static void core_register_path(IpcObject *s, IpcCall *c) { /* 36: a data path registration is kept (no data ever flows on it) */
    (void)c;
    ((BtmCore *)s->state)->data_paths++;
}
static void core_unregister_path(IpcObject *s, IpcCall *c) {
    (void)c;
    BtmCore *b = s->state;
    if (b->data_paths) b->data_paths--;
}
/* 22 pair, 27-32 GATT queries and 34/35 MTU take a connection handle; none can be valid without a connection, so they fail like BleConnect. */
static const IpcCmd core_cmds[] = {
    {0, "AcquireBleScanEvent", core_acquire_scan_event, IPC_IMPL},
    {1, "GetBleScanFilterParameter", core_filter_param, IPC_STUB},
    {2, "GetBleScanFilterParameter2", core_filter_param, IPC_STUB},
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
    {19, "BleDisconnect", core_disconnect, IPC_IMPL},
    {20, "BleGetConnectionState", core_no_results, IPC_IMPL},
    {21, "AcquireBlePairingEvent", core_acquire_pair_event, IPC_IMPL},
    {22, "BlePairDevice", core_connect_failed, IPC_IMPL},
    {23, "BleUnPairDevice", core_unpair, IPC_IMPL},
    {24, "BleUnPairDevice2", core_unpair, IPC_IMPL},
    {25, "BleGetPairedDevices", core_no_results, IPC_IMPL},
    {26, "AcquireBleServiceDiscoveryEvent", core_acquire_discovery_event, IPC_IMPL},
    {27, "GetGattServices", core_connect_failed, IPC_IMPL},
    {28, "GetGattService", core_connect_failed, IPC_IMPL},
    {29, "GetGattIncludedServices", core_connect_failed, IPC_IMPL},
    {30, "GetBelongingGattService", core_connect_failed, IPC_IMPL},
    {31, "GetGattCharacteristics", core_connect_failed, IPC_IMPL},
    {32, "GetGattDescriptors", core_connect_failed, IPC_IMPL},
    {33, "AcquireBleMtuConfigEvent", core_acquire_mtu_event, IPC_IMPL},
    {34, "ConfigureBleMtu", core_connect_failed, IPC_IMPL},
    {35, "GetBleMtu", core_connect_failed, IPC_IMPL},
    {36, "RegisterBleGattDataPath", core_register_path, IPC_IMPL},
    {37, "UnregisterBleGattDataPath", core_unregister_path, IPC_IMPL},
};
static void free_state(IpcObject *o) { free(o->state); }
static void btm_get_core(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IBtmUserCore", core_cmds, calloc(1, sizeof(BtmCore)), free_state));
}
static const IpcCmd btm_cmds[] = {{0, "GetCore", btm_get_core, IPC_IMPL}};
static IpcObject *btm_open(void) { return IPC_OBJECT("btm:u", btm_cmds, NULL, NULL); }
void service_register_btm(void) { ipc_register_service("btm:u", btm_open); }
