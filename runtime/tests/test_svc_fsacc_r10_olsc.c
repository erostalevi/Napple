/* Unit tests (ipc-depth-95 r10, part fsacc): olsc:u's cloud backup transfers (19, 22, 25, 26) hand out their async objects, whose outcome follows the
 * OnlineProvider: no network, network without a network service account, linked account. olsc.c is #included. */
#include "../src/services/olsc.c"
#include "svc_test.h"

static int g_linked, g_connected;
static int t_account(void *ctx, const uint8_t u[16], OnlineAccountInfo *o) {
    (void)ctx;
    (void)u;
    if (!g_linked) return 1;
    memset(o, 0, sizeof *o);
    o->linked = 1;
    return 0;
}
static int t_net(void *ctx, OnlineNetInfo *o) {
    (void)ctx;
    memset(o, 0, sizeof *o);
    o->connected = (uint8_t)g_connected;
    return 0;
}
static const OnlineProvider t_provider = {.abi_version = ONLINE_ABI_VERSION,
                                          .struct_size = sizeof(OnlineProvider),
                                          .name = "olsc test",
                                          .caps = ONLINE_CAP_NETWORK | ONLINE_CAP_ACCOUNT,
                                          .net_info = t_net,
                                          .account_info = t_account};

static void drop(IpcObject *o) {
    if (o->destroy) o->destroy(o);
    free(o);
}
static const unsigned k_cmds[4] = {19, 22, 25, 26};
static const char *const k_iface[4] = {"IAsyncResult", "IAsyncResult", "IAsyncListData", "IAsyncTransferRequest"};
/* Runs the four commands and checks each object; returns 1 when every outcome is `want`. */
static int outcomes(IpcObject *o, uint32_t want, const char *what) {
    uint8_t in[0x18] = {1, 2, 3, 4};
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        IpcCall c;
        svc_call(o, k_cmds[i], in, k_cmds[i] == 19 ? 0x10 : 0x18, &c);
        CHECK(c.result == 0 && c.nout_objects == 1 && !strcmp(c.out_objects[0]->iface, k_iface[i]), "%s: %u hands out an %s", what, k_cmds[i], k_iface[i]);
        if (!c.nout_objects) return 0;
        IpcObject *a = c.out_objects[0];
        IpcCall d;
        svc_call(a, 2, NULL, 0, &d);
        CHECK(d.result == 0 && d.out[0] == 1, "...IsAvailable");
        svc_call(a, 3, NULL, 0, &d);
        CHECK(d.nout_copy == 1 && event_is_signaled(d.out_copy[0]), "...GetSystemEvent is signalled");
        svc_call(a, 1, NULL, 0, &d);
        CHECK(d.result == 0, "...Cancel");
        svc_call(a, 0, NULL, 0, &d);
        if (d.result != want) ok = 0;
        CHECK(d.result == want, "%s: %u GetResult %#x", what, k_cmds[i], d.result);
        if (svc_find(a, 100)) {
            svc_call(a, 100, NULL, 0, &d);
            CHECK(d.result == want, "...its data / progress call fails the same way");
        }
        drop(a);
    }
    return ok;
}

int main(void) {
    IpcObject *o = olsc_open();
    online_provider_install(NULL);
    outcomes(o, HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "NullProvider");
    online_provider_install(&t_provider);
    g_connected = 0;
    g_linked = 1;
    outcomes(o, HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "provider without a connection");
    g_connected = 1;
    g_linked = 0;
    outcomes(o, HZ_R_ACC_NSA_REGISTRATION_REQUIRED, "connected, no network service account");
    g_linked = 1;
    outcomes(o, HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "connected and linked (no cloud-save hook)");
    online_provider_install(NULL);
    CHECK(svc_find(o, 18)->status == IPC_STUB && svc_find(o, 27)->status == IPC_STUB, "18 and 27 stay documented stubs");
    printf("{\"test\":\"svc_fsacc_r10_olsc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
