/* olsc:u (nn::olsc): cloud save-data synchronisation. Offline the cloud is never reachable: the service connects, the controller objects
 * can be created, and every operation that needs the cloud fails with the offline network result. Command ids follow switchbrew; an id not listed
 * fails loudly through the generic unknown-command policy. */
#include "svc_common.h"
#include "hz_result.h"
#include "sys_default.h"
#include "online_provider.h"

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
/* ---- the cloud backup transfers (ipc-depth-95 r10) -------------------------------------------------------------------------
 * 19 UpdateSaveDataBackupInfoCacheAsync (0x10 in) and 22 DeleteSaveDataBackupAsync (0x18 in) -> IAsyncResult, 25 ListDownloadableSaveDataBackupInfoAsync
 * (0x18 in + transfer memory) -> IAsyncListData, 26 DownloadSaveDataBackupAsync (0x18 in) -> IAsyncTransferRequest (switchbrew). The inputs start with the
 * user's uid [unverified]. Each operation talks to the cloud save server: without a connection it ends with nifm's no-network result; connected, a user
 * without a network service account (NAPPLE_NET=host has no ONLINE_CAP_ACCOUNT) gets NetworkServiceAccountRegistrationRequired [unverified that olsc passes
 * acc's result on]; a linked user would reach the server through a cloud-save provider hook that OnlineProvider lacks, so it is unreachable (the
 * no-network result). The outcome is known at once: the async object is available as soon as it is made. */
static uint32_t cloud_result(const IpcCall *c) {
    const OnlineProvider *p = online_provider();
    int (*net)(void *, OnlineNetInfo *) = ONLINE_FN(p, net_info);
    OnlineNetInfo ni;
    memset(&ni, 0, sizeof ni);
    if (!net || net(p->ctx, &ni) != 0 || !ni.connected) return HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
    int (*info)(void *, const uint8_t[16], OnlineAccountInfo *) = online_has(p, ONLINE_CAP_ACCOUNT) ? ONLINE_FN(p, account_info) : NULL;
    OnlineAccountInfo ai;
    memset(&ai, 0, sizeof ai);
    uint8_t uid[16] = {0};
    if (c->in_size >= 16) memcpy(uid, ipc_in(c, 0, 16), 16);
    if (!info || info(p->ctx, uid, &ai) != 0 || !ai.linked) return HZ_R_ACC_NSA_REGISTRATION_REQUIRED;
    return HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
}
static void async_get_result(IpcObject *s, IpcCall *c) { c->result = (uint32_t)(uintptr_t)s->state; }
static void async_cancel(IpcObject *s, IpcCall *c) { /* nothing is in flight; the outcome stays */
    (void)s;
    (void)c;
}
/* 100 GetDataCount / 101 GetData / 100 GetOperationProgressInfo: only a finished, successful operation has data; these fail with its result. */
static void async_data_failed(IpcObject *s, IpcCall *c) { async_get_result(s, c); }
static const IpcCmd async_result_cmds[] = {
    {0, "GetResult", async_get_result, IPC_IMPL},
    {1, "Cancel", async_cancel, IPC_IMPL},
    {2, "IsAvailable", svc_ret_u8_1, IPC_IMPL},
    {3, "GetSystemEvent", svc_ret_event_signaled, IPC_IMPL},
};
static const IpcCmd async_list_cmds[] = {
    {0, "GetResult", async_get_result, IPC_IMPL},         {1, "Cancel", async_cancel, IPC_IMPL},
    {2, "IsAvailable", svc_ret_u8_1, IPC_IMPL},           {3, "GetSystemEvent", svc_ret_event_signaled, IPC_IMPL},
    {100, "GetDataCount", async_data_failed, IPC_IMPL}, {101, "GetData", async_data_failed, IPC_IMPL},
};
static const IpcCmd async_transfer_cmds[] = {
    {0, "GetResult", async_get_result, IPC_IMPL},     {1, "Cancel", async_cancel, IPC_IMPL},
    {2, "IsAvailable", svc_ret_u8_1, IPC_IMPL},       {3, "GetSystemEvent", svc_ret_event_signaled, IPC_IMPL},
    {100, "GetOperationProgressInfo", async_data_failed, IPC_IMPL},
};
/* The object's state is its result (a 32-bit value, never a pointer). */
static void cloud_async(IpcObject *s, IpcCall *c) {
    (void)s;
    void *r = (void *)(uintptr_t)cloud_result(c);
    if (c->cmd == 25) ipc_out_object(c, IPC_OBJECT("IAsyncListData", async_list_cmds, r, NULL));
    else if (c->cmd == 26) ipc_out_object(c, IPC_OBJECT("IAsyncTransferRequest", async_transfer_cmds, r, NULL));
    else ipc_out_object(c, IPC_OBJECT("IAsyncResult", async_result_cmds, r, NULL));
}
static const IpcCmd olsc_cmds[] = {
    {0, "Initialize", svc_noop, IPC_IMPL},
    {10, "VerifySaveDataBackupLicenseAsync", offline, IPC_IMPL},
    {13, "GetSaveDataBackupSetting", zero_ok, IPC_IMPL},
    {14, "SetSaveDataBackupSettingEnabled", zero_ok, IPC_IMPL},
    {15, "SetCustomData", zero_ok, IPC_IMPL},
    {16, "DeleteSaveDataBackupSetting", zero_ok, IPC_IMPL},
    /* 18: neither the input nor the SaveDataBackupInfo output is documented, nor the result of an empty cache */
    {18, "GetSaveDataBackupInfoCache", sysd_err_generic, IPC_STUB},
    {19, "UpdateSaveDataBackupInfoCacheAsync", cloud_async, IPC_IMPL},
    {22, "DeleteSaveDataBackupAsync", cloud_async, IPC_IMPL},
    {25, "ListDownloadableSaveDataBackupInfoAsync", cloud_async, IPC_IMPL},
    {26, "DownloadSaveDataBackupAsync", cloud_async, IPC_IMPL},
    /* 27 [11.0.0+]: input and returned interface not documented */
    {27, "UploadSaveDataBackupAsync", sysd_err_generic, IPC_STUB},
};
static IpcObject *olsc_open(void) { return IPC_OBJECT("olsc:u", olsc_cmds, NULL, NULL); }
void service_register_olsc(void) { ipc_register_service("olsc:u", olsc_open); }
