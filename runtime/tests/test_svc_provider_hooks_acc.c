/* Unit tests: ipc-provider-hooks r11, phase 2: acc:u0 102 AuthenticateApplicationAsync, 120 CreateGuestLoginRequest and IManagerForApplication 150
 * CreateAuthorizationRequest answer through the provider's ID token (the Napple account path). account.c is #included. */
#include "../src/services/account.c"
#include "svc_test.h"

static int g_connected, g_linked, g_token;
static int fk_net(void *ctx, OnlineNetInfo *o) { (void)ctx; memset(o, 0, sizeof *o); o->connected = (uint8_t)g_connected; return 0; }
static int fk_account(void *ctx, const uint8_t u[16], OnlineAccountInfo *o) {
    (void)ctx; (void)u;
    if (!g_linked) return 1;
    memset(o, 0, sizeof *o); o->linked = 1; o->id = 99;
    return 0;
}
static size_t fk_token(void *ctx, const uint8_t u[16], char *out, size_t n) {
    (void)ctx; (void)u;
    static const char t[] = "napple.id.token";
    if (!g_token || n < sizeof t - 1) return 0;
    memcpy(out, t, sizeof t - 1);
    return sizeof t - 1;
}
static const OnlineProvider fake = {.abi_version = ONLINE_ABI_VERSION, .struct_size = sizeof(OnlineProvider), .name = "acc hooks (test)",
                                    .caps = ONLINE_CAP_NETWORK | ONLINE_CAP_ACCOUNT, .net_info = fk_net, .account_info = fk_account, .account_id_token = fk_token};

static uint32_t async_res(IpcCall *c, const char *what) {
    CHECK(c->result == 0 && c->nout_objects == 1, "%s hands out its async context", what);
    if (c->nout_objects != 1) return 0xffffffffu;
    IpcCall r;
    svc_call(c->out_objects[0], 3, NULL, 0, &r);
    return r.result;
}
static uint32_t result_of_async_context(IpcObject *o, unsigned cmd, const char *what) {
    IpcCall c;
    svc_call(o, cmd, NULL, 0, &c);
    return async_res(&c, what);
}

int main(void) {
    IpcCall c;
    IpcObject *u = acc_open();
    svc_call(u, 140, NULL, 0, &c);
    svc_call(u, 101, g_user_id, 16, &c);
    IpcObject *mgr = c.out_objects[0];

    /* ---- NullProvider: as before */
    online_provider_install(NULL);
    CHECK(result_of_async_context(u, 102, "102") == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "102 offline: no network");
    svc_call(u, 120, NULL, 0, &c);
    CHECK(c.result != 0 && c.nout_objects == 0, "120 without an account provider: refused");
    svc_call(mgr, 150, NULL, 0, &c);
    CHECK(c.result != 0 && c.nout_objects == 0, "150 without an account provider: refused");

    /* ---- a provider with the Napple account path */
    online_provider_install(&fake);
    CHECK(result_of_async_context(u, 102, "102") == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "102 without a connection");
    g_connected = 1;
    CHECK(result_of_async_context(u, 102, "102") == HZ_R_ACC_NSA_REGISTRATION_REQUIRED, "102 connected, not linked");
    g_linked = 1;
    CHECK(result_of_async_context(u, 102, "102") == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "102 linked, the server issues no token");
    g_token = 1;
    CHECK(result_of_async_context(u, 102, "102") == 0, "102 linked with a token: authenticated");

    /* guest login */
    svc_call(u, 120, NULL, 0, &c);
    CHECK(c.result == 0 && c.nout_objects == 1 && !strcmp(c.out_objects[0]->iface, "IGuestLoginRequest"), "120 hands out IGuestLoginRequest");
    IpcObject *g = c.out_objects[0];
    uint8_t sid[16], tokbuf[0x40];
    svc_call(g, 0, NULL, 0, &c);
    CHECK(c.result == 0 && c.out_size == 16, "GetSessionId: a uuid");
    memcpy(sid, c.out, 16);
    memset(&c, 0, sizeof c);
    memset(tokbuf, 0, sizeof tokbuf);
    svc_set_out_buf(&c, tokbuf, sizeof tokbuf);
    svc_call_c(g, 11, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 15 && !memcmp(tokbuf, "napple.id.token", 15), "LoadIdTokenCache: the account's token");
    g_token = 0;
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, tokbuf, sizeof tokbuf);
    svc_call_c(g, 11, &c);
    CHECK(c.result != 0, "LoadIdTokenCache without a token fails");
    g_token = 1;
    svc_call(u, 120, NULL, 0, &c);
    svc_call(c.out_objects[0], 0, NULL, 0, &c);
    CHECK(memcmp(sid, c.out, 16) != 0, "another request, another session id");

    /* authorization request */
    svc_call(mgr, 150, NULL, 0, &c);
    CHECK(c.result == 0 && c.nout_objects == 1 && !strcmp(c.out_objects[0]->iface, "IAuthorizationRequest"), "150 hands out IAuthorizationRequest");
    IpcObject *a = c.out_objects[0];
    svc_call(a, 19, NULL, 0, &c);
    CHECK(c.result == 0 && c.out[0] == 0, "IsAuthorized before the invoke: no");
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, tokbuf, sizeof tokbuf);
    svc_call_c(a, 21, &c);
    CHECK(c.result != 0, "GetIdToken before the invoke fails");
    CHECK(result_of_async_context(a, 10, "10") == 0, "InvokeWithoutInteractionAsync succeeds with a token");
    svc_call(a, 19, NULL, 0, &c);
    CHECK(c.out[0] == 1, "IsAuthorized after the invoke");
    memset(&c, 0, sizeof c);
    memset(tokbuf, 0, sizeof tokbuf);
    svc_set_out_buf(&c, tokbuf, sizeof tokbuf);
    svc_call_c(a, 21, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 15 && !memcmp(tokbuf, "napple.id.token", 15), "GetIdToken: the token");
    svc_call(a, 20, NULL, 0, &c);
    CHECK(c.result != 0, "GetAuthorizationCode needs a Nintendo Account server: refused");
    g_token = 0;
    CHECK(result_of_async_context(a, 10, "10") == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "invoke without a token fails");
    svc_call(a, 19, NULL, 0, &c);
    CHECK(c.out[0] == 0, "...and the request is not authorized");

    CHECK(svc_find(u, 102)->status == IPC_IMPL && svc_find(u, 120)->status == IPC_IMPL && svc_find(mgr, 150)->status == IPC_IMPL, "102, 120, 150 are real");
    online_provider_install(NULL);
    printf("{\"test\":\"svc_provider_hooks_acc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
