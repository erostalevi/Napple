/* Unit tests: acc:u0 (local user, profile, application info) and the network-account rows routed through the OnlineProvider. */
#include "../src/services/account.c"
#include "svc_test.h"
#include "online_loopback.h"
#ifdef __linux__
const char *host_user_name(void) { return "Tester"; }
#endif

int main(void) {
    IpcCall c; IpcObject *u = acc_open();
    svc_call(u, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1, "one local user");
    svc_call(u, 50, NULL, 0, &c); CHECK(c.result == 0 && c.out[0] == 0, "IsUserRegistrationRequestPermitted = false");
    svc_call(u, 150, NULL, 0, &c); CHECK(c.result == 0 && c.out[0] == 0, "IsUserAccountSwitchLocked = false");
    AccSession *as = u->state; CHECK(as && !as->app_info_initialized, "fresh connection: application info not initialised");
    unsigned init[] = {100, 140, 160};
    for (unsigned i = 0; i < 3; i++) { as->app_info_initialized = 0; svc_call(u, init[i], NULL, 0, &c); CHECK(c.result == 0 && as->app_info_initialized, "InitializeApplicationInfo %u is remembered", init[i]); }
    IpcObject *u2 = acc_open(); CHECK(((AccSession *)u2->state)->app_info_initialized == 0, "the flag is per connection");
    svc_call(u, 101, NULL, 0, &c); CHECK(c.nout_objects == 1, "GetBaasAccountManagerForApplication"); IpcObject *m = c.out_objects[0];

    /* NullProvider: not linked; the answers titles saw before this row was routed */
    online_provider_install(NULL);
    svc_call(m, 0, NULL, 0, &c); CHECK(c.result == 0, "CheckAvailability: ok");
    svc_call(m, 1, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 0x4e61707070304e53ULL, "GetAccountId: the fixed local id");
    { uint8_t cache[ONLINE_ACCOUNT_CACHE_SIZE + 8]; memset(cache, 0xcc, sizeof cache); memset(&c, 0, sizeof c); svc_set_out_buf(&c, cache, sizeof cache); svc_call_c(m, 130, &c);
      CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 0 && cache[0] == 0 && cache[ONLINE_ACCOUNT_CACHE_SIZE - 1] == 0 && cache[ONLINE_ACCOUNT_CACHE_SIZE] == 0xcc, "NA cache: id 0 and zeros, bounded by the 0x68 size"); }

    /* loopback provider without a linked account: same answers */
    memset(&lb, 0, sizeof lb); online_provider_install(&lb_provider);
    svc_call(m, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 0x4e61707070304e53ULL, "provider without a linked account: local id");
    /* linked */
    lb.account_linked = 1;
    svc_call(m, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 0x1234abcd5678ef90ull, "GetAccountId = the provider's id");
    { uint8_t cache[ONLINE_ACCOUNT_CACHE_SIZE]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, cache, sizeof cache); svc_call_c(m, 130, &c);
      CHECK(SVC_OUT(&c, uint64_t, 0) == 0x1234abcd5678ef90ull && cache[0] == 0x77 && cache[ONLINE_ACCOUNT_CACHE_SIZE - 1] == 0x77, "NA cache = the provider's opaque bytes");
      uint8_t small[8]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, small, sizeof small); svc_call_c(m, 130, &c); CHECK(small[0] == 0x77 && small[7] == 0x77, "a small buffer gets a prefix"); }
    /* a provider table built before the account rows existed: struct_size cuts them off */
    { OnlineProvider old = lb_provider; old.struct_size = offsetof(OnlineProvider, ldn_scan); online_provider_install(&old);
      svc_call(m, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 0x4e61707070304e53ULL, "old provider table (no account_info): local id"); }
    online_provider_install(NULL);
    printf("{\"test\":\"svc_account\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
