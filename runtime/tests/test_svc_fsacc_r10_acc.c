/* Unit tests (ipc-depth-95 r10, part fsacc): the acc:u0 / IManagerForApplication rows that were documented-default stubs: the asynchronous BaaS calls
 * (IManagerForApplication 2, 3, 4, 170; acc:u0 103, and 102's offline answer) through the OnlineProvider's account state, and the open contexts
 * (IManagerForApplication 160, acc:u0 130 / 131). account.c is #included. */
#include "../src/services/account.c"
#include "svc_test.h"
#ifdef __linux__
const char *host_user_name(void) { return "Tester"; }
#endif

/* A provider with a network and (when g_linked) a linked network account. */
static int g_linked, g_connected = 1;
static int t_account(void *ctx, const uint8_t u[16], OnlineAccountInfo *o) {
    (void)ctx;
    (void)u;
    if (!g_linked) return 1;
    memset(o, 0, sizeof *o);
    o->linked = 1;
    o->id = 0x42;
    return 0;
}
static int t_net(void *ctx, OnlineNetInfo *o) {
    (void)ctx;
    memset(o, 0, sizeof *o);
    o->connected = (uint8_t)g_connected;
    return 0;
}
static const OnlineProvider t_provider = {
    .abi_version = ONLINE_ABI_VERSION,
    .struct_size = sizeof(OnlineProvider),
    .name = "account test",
    .caps = ONLINE_CAP_NETWORK | ONLINE_CAP_ACCOUNT,
    .net_info = t_net,
    .account_info = t_account,
};
/* A host-like provider: a network, no account capability (what NAPPLE_NET=host is). */
static const OnlineProvider t_host = {
    .abi_version = ONLINE_ABI_VERSION, .struct_size = sizeof(OnlineProvider), .name = "host-like", .caps = ONLINE_CAP_NETWORK, .net_info = t_net};

static void drop(IpcObject *o) {
    if (o->destroy) o->destroy(o);
    free(o);
}
static IpcObject *manager_for(IpcObject *u, const uint8_t uid[16]) {
    IpcCall c;
    svc_call(u, 101, uid, 16, &c);
    CHECK(c.result == 0 && c.nout_objects == 1, "101 GetBaasAccountManagerForApplication");
    return c.nout_objects ? c.out_objects[0] : NULL;
}
/* Calls an async command and returns its context's GetResult (checking the context is done and its event signalled). */
static uint32_t async_outcome(IpcObject *o, unsigned cmd, const void *in, uint32_t n, IpcObject **keep) {
    IpcCall c;
    svc_call(o, cmd, in, n, &c);
    CHECK(c.result == 0 && c.nout_objects == 1, "%s %u hands out an async context", o->iface, cmd);
    if (!c.nout_objects) return 0xffffffffu;
    IpcObject *a = c.out_objects[0];
    IpcCall d;
    svc_call(a, 2, NULL, 0, &d);
    CHECK(d.result == 0 && d.out[0] == 1, "...HasDone");
    svc_call(a, 0, NULL, 0, &d);
    CHECK(d.result == 0 && d.nout_copy == 1 && event_is_signaled(d.out_copy[0]), "...GetSystemEvent is signalled");
    svc_call(a, 1, NULL, 0, &d);
    CHECK(d.result == 0, "...Cancel after completion is harmless");
    svc_call(a, 3, NULL, 0, &d);
    if (keep) *keep = a;
    else drop(a);
    return d.result;
}

static void t_offline(IpcObject *u, IpcObject *m) {
    online_provider_install(NULL);
    IpcCall c;
    /* 2 / 3 / 4 are napple-online's (main): a provider without an id-token hook keeps the old generic answer, no object */
    memset(&c, 0, sizeof c);
    svc_call_c(m, 2, &c);
    CHECK(c.result == IPC_R_UNKNOWN_COMMAND && c.nout_objects == 0, "NullProvider: 2 EnsureIdTokenCacheAsync keeps the generic answer");
    uint8_t tok[64];
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, tok, sizeof tok);
    svc_call_c(m, 3, &c);
    CHECK(c.result == IPC_R_UNKNOWN_COMMAND, "...3 LoadIdTokenCache: the generic answer");
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, tok, sizeof tok);
    svc_call_c(m, 4, &c);
    CHECK(c.result == IPC_R_UNKNOWN_COMMAND, "...4 LoadIdTokenCache (19.0.0+) the same");
    IpcObject *lk = NULL;
    CHECK(async_outcome(m, 170, NULL, 0, &lk) == HZ_R_ACC_NSA_REGISTRATION_REQUIRED, "170 LoadNetworkServiceLicenseKindAsync: NSA registration required");
    if (lk) {
        svc_call(lk, 100, NULL, 0, &c);
        CHECK(c.result == HZ_R_ACC_NSA_REGISTRATION_REQUIRED, "...GetNetworkServiceLicenseKind fails with the context's result");
        drop(lk);
    }
    CHECK(async_outcome(u, 103, g_user_id, 16, NULL) == HZ_R_ACC_NSA_REGISTRATION_REQUIRED, "103 CheckNetworkServiceAvailabilityAsync: NSA registration required");
    CHECK(async_outcome(u, 102, NULL, 0, NULL) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "102 AuthenticateApplicationAsync offline: no network");
    /* a manager that never ensured reports the account state */
    IpcObject *m2 = manager_for(u, g_user_id);
    if (m2) {
        memset(&c, 0, sizeof c);
        svc_set_out_buf(&c, tok, sizeof tok);
        svc_call_c(m2, 3, &c);
        CHECK(c.result == IPC_R_UNKNOWN_COMMAND, "a fresh manager's LoadIdTokenCache: the generic answer");
        drop(m2);
    }
}
static void t_providers(IpcObject *u, IpcObject *m) {
    online_provider_install(&t_host);
    {
        IpcCall e;
        memset(&e, 0, sizeof e);
        svc_call_c(m, 2, &e);
        CHECK(e.result == IPC_R_UNKNOWN_COMMAND, "host-like provider (no id-token hook): 2 keeps the generic answer");
    }
    CHECK(async_outcome(u, 103, NULL, 0, NULL) == HZ_R_ACC_NSA_REGISTRATION_REQUIRED, "...103 the same");
    online_provider_install(&t_provider);
    g_linked = 1;
    /* 2 / 3 with an id-token hook: test_net_online_client.c (main) */
    CHECK(async_outcome(m, 170, NULL, 0, NULL) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "...170 the same");
    CHECK(async_outcome(u, 103, NULL, 0, NULL) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "...103 the same");
    g_linked = 0;
    CHECK(async_outcome(m, 170, NULL, 0, NULL) == HZ_R_ACC_NSA_REGISTRATION_REQUIRED, "unlinked again: NSA registration required");
    online_provider_install(NULL);
}
static void t_open_context(IpcObject *u, IpcObject *m) {
    IpcCall c;
    uint8_t list[16 * 4];
    memset(list, 0xcc, sizeof list);
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, list, sizeof list);
    svc_call_c(u, 131, &c);
    CHECK(c.result == 0 && !memcmp(list, (uint8_t[16]){0}, 16), "131 ListOpenContextStoredUsers: none stored yet");
    CHECK(svc_call(u, 130, g_user_id, 16, &c) && c.result == R_ACC_USER_NOT_FOUND, "130 LoadOpenContext without a stored context: user not found");
    svc_call(m, 160, NULL, 0, &c);
    CHECK(c.result == 0, "160 StoreOpenContext");
    svc_call(m, 160, NULL, 0, &c);
    memset(list, 0xcc, sizeof list);
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, list, sizeof list);
    svc_call_c(u, 131, &c);
    CHECK(!memcmp(list, g_user_id, 16) && !memcmp(list + 16, (uint8_t[16]){0}, 16) && list[48] == 0, "...131 lists the user once, the rest zero");
    svc_call(u, 130, g_user_id, 16, &c);
    CHECK(c.result == 0 && c.nout_objects == 1, "130 LoadOpenContext hands out its manager");
    if (c.nout_objects) {
        IpcCall d;
        svc_call(c.out_objects[0], 1, NULL, 0, &d);
        CHECK(d.result == 0 && SVC_OUT(&d, uint64_t, 0) == 0x4e61707070304e53ULL, "...an IManagerForApplication of that user");
        CHECK(!memcmp(((AccManager *)c.out_objects[0]->state)->uid, g_user_id, 16), "...for the stored uid");
        drop(c.out_objects[0]);
    }
    uint8_t nobody[16] = {1};
    CHECK(svc_call(u, 130, nobody, 16, &c) && c.result == R_ACC_USER_NOT_FOUND, "...another uid: user not found");
    CHECK(svc_call(u, 130, NULL, 0, &c) && c.result == HZ_RESULT(124, 20), "...a missing uid: invalid user id");
}

int main(void) {
    IpcObject *u = acc_open();
    IpcObject *m = manager_for(u, g_user_id);
    if (!m) return 1;
    CHECK(!memcmp(((AccManager *)m->state)->uid, g_user_id, 16), "the manager is the requested user's");
    t_offline(u, m);
    t_providers(u, m);
    t_open_context(u, m);
    drop(m);
    printf("{\"test\":\"svc_fsacc_r10_acc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
