/* Unit tests: ipc-provider-hooks r11, phase 2: olsc:u 18 GetSaveDataBackupInfoCache and 27 UploadSaveDataBackupAsync through the cloud-save hooks. */
#include "../src/services/olsc.c"
#include "svc_test.h"

static int g_connected, g_linked, g_have_backup, g_uploads;
static uint32_t g_upload_result;
static int fk_net(void *ctx, OnlineNetInfo *o) { (void)ctx; memset(o, 0, sizeof *o); o->connected = (uint8_t)g_connected; return 0; }
static int fk_account(void *ctx, const uint8_t u[16], OnlineAccountInfo *o) {
    (void)ctx; (void)u;
    if (!g_linked) return 1;
    memset(o, 0, sizeof *o); o->linked = 1;
    return 0;
}
static int fk_info(void *ctx, const uint8_t u[16], OnlineCloudBackup *o) {
    (void)ctx;
    if (u[0] != 7) return 1;
    memset(o, 0, sizeof *o);
    if (g_have_backup) o->exists = 1, o->size = 0x12345, o->last_s = 1700000000, o->title_id = 0x0100000000010000ull;
    o->pending = (uint8_t)(g_uploads > 0);
    return 0;
}
static uint32_t fk_upload(void *ctx, const uint8_t u[16], int *done) {
    (void)ctx; (void)u;
    *done = 0;
    if (!g_upload_result) g_uploads++;
    return g_upload_result;
}
static OnlineProvider fake = {.abi_version = ONLINE_ABI_VERSION, .struct_size = sizeof(OnlineProvider), .name = "cloud hooks (test)",
                              .caps = ONLINE_CAP_NETWORK | ONLINE_CAP_ACCOUNT, .net_info = fk_net, .account_info = fk_account,
                              .cloud_backup_info = fk_info, .cloud_backup_upload = fk_upload};

static uint32_t async_result(IpcObject *o, unsigned cmd, const uint8_t *in, uint32_t n, const char *iface) {
    IpcCall c;
    svc_call(o, cmd, in, n, &c);
    CHECK(c.result == 0 && c.nout_objects == 1 && !strcmp(c.out_objects[0]->iface, iface), "%u hands out %s", cmd, iface);
    if (c.nout_objects != 1) return 0xffffffffu;
    IpcCall r;
    svc_call(c.out_objects[0], 0, NULL, 0, &r);
    return r.result;
}

int main(void) {
    IpcCall c;
    IpcObject *o = olsc_open();
    uint8_t uid[16] = {7};

    /* no provider: nothing is cached and nothing is uploaded */
    online_provider_install(NULL);
    svc_call(o, 18, uid, 16, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "18 offline: no network");
    CHECK(async_result(o, 27, uid, 16, "IAsyncTransferRequest") == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "27 offline: no network");

    online_provider_install(&fake);
    svc_call(o, 18, uid, 16, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "18 provider without a connection");
    g_connected = 1;
    svc_call(o, 18, uid, 16, &c);
    CHECK(c.result == HZ_R_ACC_NSA_REGISTRATION_REQUIRED, "18 connected, no network service account");
    CHECK(async_result(o, 27, uid, 16, "IAsyncTransferRequest") == HZ_R_ACC_NSA_REGISTRATION_REQUIRED && g_uploads == 0, "27 connected, no account: nothing uploaded");

    g_linked = 1;
    svc_call(o, 18, uid, 16, &c);
    CHECK(c.result == 0 && c.out_size == 0x20 && !SVC_OUT(&c, uint64_t, 0) && !c.out[24], "18 linked, no backup yet: an empty record");
    g_have_backup = 1;
    svc_call(o, 18, uid, 16, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 0x12345 && SVC_OUT(&c, uint64_t, 8) == 1700000000 && SVC_OUT(&c, uint64_t, 16) == 0x0100000000010000ull &&
              c.out[24] == 1 && c.out[25] == 0, "18 linked, backup present");
    CHECK(async_result(o, 27, uid, 16, "IAsyncTransferRequest") == 0 && g_uploads == 1, "27 linked: the upload is accepted by the provider");
    svc_call(o, 18, uid, 16, &c);
    CHECK(c.out[25] == 1, "18 shows the upload pending");
    g_upload_result = HZ_RESULT(124, 1234);
    CHECK(async_result(o, 27, uid, 16, "IAsyncTransferRequest") == HZ_RESULT(124, 1234), "27 passes the provider's refusal");
    uid[0] = 9;
    svc_call(o, 18, uid, 16, &c);
    CHECK(c.result == 0 && !SVC_OUT(&c, uint64_t, 0) && !c.out[24], "18 for a user the cloud does not know: an empty cache");

    OnlineProvider nohook = fake;
    nohook.cloud_backup_info = NULL, nohook.cloud_backup_upload = NULL;
    online_provider_install(&nohook);
    svc_call(o, 18, uid, 16, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "18 without the hooks: unreachable");
    CHECK(async_result(o, 27, uid, 16, "IAsyncTransferRequest") == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "27 without the hooks: unreachable");
    online_provider_install(NULL);
    printf("{\"test\":\"svc_provider_hooks_olsc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
