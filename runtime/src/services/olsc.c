/* olsc:u (nn::olsc): cloud save-data synchronisation. Offline the cloud is never reachable: the service connects, the controller objects
 * can be created, and every operation that needs the cloud fails with the offline network result. Command ids are the author's
 * guesses from memory of the public documentation (unverified); an id not listed fails loudly through the generic unknown-command policy. */
#include "svc_common.h"
#include "hz_result.h"

static void offline(IpcObject *s, IpcCall *c) { (void)s; c->result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED; }
static void ret_s32_0(IpcObject *s, IpcCall *c) { (void)s; int32_t v = 0; IPC_OUT(c, v); }
static const IpcCmd tasks_cmds[] = {
    {0, "GetTransferTaskCount", ret_s32_0, IPC_STUB}, {1, "ListTransferTasks", ret_s32_0, IPC_STUB}, {2, "GetTransferTaskListChangedEvent", svc_ret_event, IPC_STUB},
};
static const IpcCmd remote_cmds[] = {
    {0, "GetDataTransferState", offline, IPC_STUB}, {1, "SynchronizeSaveData", offline, IPC_STUB}, {2, "GetSyncCompletionEvent", svc_ret_event, IPC_STUB},
};
static void make_tasks(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("ITransferTaskListController", tasks_cmds, NULL, NULL)); }
static void make_remote(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IRemoteStorageController", remote_cmds, NULL, NULL)); }
/* The device side of the backup settings answers success with zeroed output ("backup is off for this user"): Animal Crossing 3.0.3 asks for every user at boot and
 * switches the backup off where it is (DeleteUserSaveDataInSyncArea and its neighbours), and ends the process on any error (result 0x8ae6e). Only the cloud itself is offline. */
static void zero_ok(IpcObject *s, IpcCall *c) { (void)s; (void)c; }
static const IpcCmd olsc_cmds[] = {
    {0, "Initialize", svc_noop, IPC_STUB}, {10, "GetTransferTaskListController", make_tasks, IPC_STUB}, {11, "GetRemoteStorageController", make_remote, IPC_STUB},
    {12, "GetDataTransferPolicyInfo", zero_ok, IPC_STUB}, {13, "DeleteUserSaveDataInSyncArea", zero_ok, IPC_STUB},
    {1, "GetSaveDataBackupSetting", zero_ok, IPC_STUB}, {2, "SetSaveDataBackupSettingEnabled", zero_ok, IPC_STUB}, {3, "SetCloudBackupSetting", zero_ok, IPC_STUB},
    {4, "GetCloudBackupSetting", zero_ok, IPC_STUB}, {5, "UpdateCloudBackupSetting", zero_ok, IPC_STUB}, {6, "GetCloudBackupSettingEvent", zero_ok, IPC_STUB},
    {7, "IsSaveDataBackupEnabled", zero_ok, IPC_STUB}, {8, "SetSaveDataBackupEnabled", zero_ok, IPC_STUB}, {9, "GetBackupSettingChangedEvent", zero_ok, IPC_STUB},
    {14, "GetSyncAreaUsage", zero_ok, IPC_STUB}, {15, "ResetSyncArea", zero_ok, IPC_STUB}, {16, "GetSyncAreaStatus", zero_ok, IPC_STUB},
};
static IpcObject *olsc_open(void) { return IPC_OBJECT("olsc:u", olsc_cmds, NULL, NULL); }
void service_register_olsc(void) { ipc_register_service("olsc:u", olsc_open); }
