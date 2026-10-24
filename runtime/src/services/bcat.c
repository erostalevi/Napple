/* bcat:u (nn::bcat): background content delivery. Offline there is never a delivery cache: sync requests complete at once with the
 * offline result, directory and file opens report "not found", enumerations are empty. Name checks and the progress record are real.
 * Command ids and result descriptions are from the author's memory of the public documentation (unverified). */
#include "svc_common.h"
#include "bcat_core.h"
#include "hz_result.h"

static void ret_s32_0(IpcObject *s, IpcCall *c) { (void)s; int32_t v = 0; IPC_OUT(c, v); }
static void not_found(IpcObject *s, IpcCall *c) { (void)s; c->result = HZ_R_BCAT_NOT_FOUND; }
static void bad_name(IpcCall *c) { c->result = HZ_RESULT(HZ_MOD_BCAT, 2); }   /* [unverified] invalid argument */

/* IDeliveryCacheProgressService: the sync "ran" and failed because there is no network. */
static void prog_get_event(IpcObject *s, IpcCall *c) { svc_ret_event_signaled(s, c); }
static void prog_get_impl(IpcObject *s, IpcCall *c) {
    (void)s; uint8_t rec[BCAT_PROGRESS_SIZE]; bcat_make_progress(BCAT_STATUS_DONE, HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, rec);
    const IpcBuf *b = ipc_out_buf(c, 0); if (b) ipc_write_buf(c, b, rec, sizeof rec);
}
static const IpcCmd progress_cmds[] = {{0, "GetEvent", prog_get_event, IPC_STUB}, {1, "GetImpl", prog_get_impl, IPC_IMPL}};
static void make_progress(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IDeliveryCacheProgressService", progress_cmds, NULL, NULL)); }

/* directory and file services */
static void dir_open(IpcObject *s, IpcCall *c) { (void)s; if (!bcat_valid_directory_name(ipc_in(c, 0, BCAT_NAME_SIZE))) { bad_name(c); return; } c->result = HZ_R_BCAT_NOT_FOUND; }
static const IpcCmd dir_cmds[] = {{0, "Open", dir_open, IPC_IMPL}, {1, "Read", ret_s32_0, IPC_STUB}, {2, "GetCount", ret_s32_0, IPC_STUB}, {3, "Close", svc_noop, IPC_STUB}};
static void file_open(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!bcat_valid_directory_name(ipc_in(c, 0, BCAT_NAME_SIZE)) || !bcat_valid_file_name(ipc_in(c, BCAT_NAME_SIZE, BCAT_NAME_SIZE))) { bad_name(c); return; }
    c->result = HZ_R_BCAT_NOT_FOUND;
}
static const IpcCmd file_cmds[] = {{0, "Open", file_open, IPC_IMPL}, {1, "Read", not_found, IPC_STUB}, {2, "GetSize", not_found, IPC_STUB}, {3, "GetDigest", not_found, IPC_STUB}};
static void make_dir_service(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IDeliveryCacheDirectoryService", dir_cmds, NULL, NULL)); }
static void make_file_service(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IDeliveryCacheFileService", file_cmds, NULL, NULL)); }
static const IpcCmd storage_cmds[] = {
    {0, "CreateFileService", make_file_service, IPC_IMPL}, {1, "CreateDirectoryService", make_dir_service, IPC_IMPL}, {10, "EnumerateDeliveryCacheDirectory", ret_s32_0, IPC_STUB},
};
static void make_storage(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IDeliveryCacheStorageService", storage_cmds, NULL, NULL)); }

static const IpcCmd bcat_service_cmds[] = {
    {10100, "RequestSyncDeliveryCache", make_progress, IPC_STUB}, {10101, "RequestSyncDeliveryCacheWithDirectoryName", make_progress, IPC_STUB},
    {10200, "CancelSyncDeliveryCacheRequest", svc_noop, IPC_STUB}, {20100, "RequestSyncDeliveryCacheWithApplicationId", make_progress, IPC_STUB},
    {30100, "SetPassphrase", svc_noop, IPC_STUB}, {30200, "RegisterBackgroundDeliveryTask", svc_noop, IPC_STUB}, {30201, "UnregisterBackgroundDeliveryTask", svc_noop, IPC_STUB},
    {30202, "BlockDeliveryTask", svc_noop, IPC_STUB}, {30203, "UnblockDeliveryTask", svc_noop, IPC_STUB},
    {90100, "EnumerateBackgroundDeliveryTask", ret_s32_0, IPC_STUB}, {90201, "ClearDeliveryCacheStorage", svc_noop, IPC_STUB},
};
static void make_bcat_service(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IBcatService", bcat_service_cmds, NULL, NULL)); }
static const IpcCmd bcat_cmds[] = {
    {0, "CreateBcatService", make_bcat_service, IPC_IMPL}, {1, "CreateDeliveryCacheStorageService", make_storage, IPC_IMPL},
    {2, "CreateDeliveryCacheStorageServiceWithApplicationId", make_storage, IPC_IMPL}, {3, "CreateDeliveryCacheProgressService", make_progress, IPC_STUB},
    {4, "CreateDeliveryCacheProgressServiceWithApplicationId", make_progress, IPC_STUB},
};
static IpcObject *bcat_open(void) { return IPC_OBJECT("bcat:u", bcat_cmds, NULL, NULL); }
void service_register_bcat(void) { ipc_register_service("bcat:u", bcat_open); }
