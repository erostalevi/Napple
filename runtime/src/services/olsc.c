/* olsc:u (nn::olsc): cloud save-data synchronisation. Offline the cloud is never reachable: the service connects, the controller objects
 * can be created, and every operation that needs the cloud fails with the offline network result. Command ids follow switchbrew; an id not listed
 * fails loudly through the generic unknown-command policy. */
#include "svc_common.h"
#include "hz_result.h"
#include "sys_default.h"

static void offline(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
}
static void ret_s32_0(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = 0;
    IPC_OUT(c, v);
}
/* The cloud save backup service for applications (switchbrew 'OLSC services': 0 Initialize, 10 VerifySaveDataBackupLicenseAsync, 13-16 the device-side backup
 * setting calls, 18-27 the backup info / transfer calls). The device side answers success with zeroed output ("backup is off for this user"): Animal Crossing 3.0.3
 * asks for every user at boot (cmd 13, GetSaveDataBackupSetting) and switches the backup off where it is, and ends the process on any error (result 0x8ae6e).
 * Only the cloud itself is offline: the license check and every transfer fail with the offline network result. The transfer task list and remote storage
 * controllers are objects of the system-side service (olsc:s), not reachable from here: their tables follow the same ids and answer offline. */
static void zero_ok(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static const IpcCmd tasks_cmds[] = {
    {0, "GetTransferTaskCountForOcean", ret_s32_0, IPC_IMPL},
    {1, "GetTransferTaskInfoForOcean", offline, IPC_IMPL},
    {2, "ListTransferTaskInfoForOcean", ret_s32_0, IPC_IMPL},
    {5, "GetTransferTaskEndEventNativeHandleHolder", svc_ret_event, IPC_IMPL},
    {9, "GetTransferTaskStartEventNativeHandleHolder", svc_ret_event, IPC_IMPL},
    {16, "GetTransferTaskCount", ret_s32_0, IPC_IMPL},
    {17, "GetTransferTaskInfo", offline, IPC_IMPL},
    {18, "ListTransferTaskInfo", ret_s32_0, IPC_IMPL},
};
static const IpcCmd remote_cmds[] = {
    {0, "RegisterUploadSaveDataTransferTask", offline, IPC_IMPL},
    {1, "RegisterDownloadSaveDataTransferTask", offline, IPC_IMPL},
    {3, "GetCount", ret_s32_0, IPC_IMPL},
};
/* olsc:s opens these (OpenTransferTaskListController / OpenRemoteStorageController); the application service has no way to reach them, so these constructors are
 * only used by the unit tests. */
__attribute__((unused)) static void make_tasks(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("ITransferTaskListController", tasks_cmds, NULL, NULL));
}
__attribute__((unused)) static void make_remote(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IRemoteStorageController", remote_cmds, NULL, NULL));
}
static const IpcCmd olsc_cmds[] = {
    {0, "Initialize", svc_noop, IPC_IMPL},
    {10, "VerifySaveDataBackupLicenseAsync", offline, IPC_IMPL},
    {13, "GetSaveDataBackupSetting", zero_ok, IPC_IMPL},
    {14, "SetSaveDataBackupSettingEnabled", zero_ok, IPC_IMPL},
    {15, "SetCustomData", zero_ok, IPC_IMPL},
    {16, "DeleteSaveDataBackupSetting", zero_ok, IPC_IMPL},
    {18, "GetSaveDataBackupInfoCache", sysd_err_generic, IPC_STUB},
    {19, "UpdateSaveDataBackupInfoCacheAsync", sysd_err_generic, IPC_STUB},
    {22, "DeleteSaveDataBackupAsync", sysd_err_generic, IPC_STUB},
    {25, "ListDownloadableSaveDataBackupInfoAsync", sysd_err_generic, IPC_STUB},
    {26, "DownloadSaveDataBackupAsync", sysd_err_generic, IPC_STUB},
    {27, "UploadSaveDataBackupAsync", sysd_err_generic, IPC_STUB},
};
static IpcObject *olsc_open(void) { return IPC_OBJECT("olsc:u", olsc_cmds, NULL, NULL); }
void service_register_olsc(void) { ipc_register_service("olsc:u", olsc_open); }
