/* Unit tests: acc:u0 (local user, profile, application info) and the network-account rows routed through the OnlineProvider. */
#define NAPPLE_ACCOUNT_TEST 1
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

    /* more than one profile (NAPPLE_PROFILES): count, existence, list, per-uid profile, and the player-select lister */
    setenv("NAPPLE_PROFILES", "3", 1);
    profile_count_reset_for_test();
    svc_call(u, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 3, "NAPPLE_PROFILES=3: three users");
    { uint8_t uid[16]; account_profile_uid(g_user_id, 2, uid);
      svc_call(u, 1, uid, 16, &c); CHECK(c.result == 0 && c.out[0] == 1, "the third user exists");
      uid[15] = 9; svc_call(u, 1, uid, 16, &c); CHECK(c.out[0] == 0, "an unknown uid does not exist");
      account_profile_uid(g_user_id, 1, uid); svc_call(u, 5, uid, 16, &c); CHECK(c.nout_objects == 1, "GetProfile for the second user");
      svc_call(c.out_objects[0], 1, NULL, 0, &c); CHECK(c.result == 0 && !memcmp(c.out, uid, 16) && !strcmp((char *)c.out + 24, "Player 2"), "GetBase: second user's uid and name"); }
    { uint8_t tab[8][16]; CHECK(profile_lister(tab, 8) == 3 && !memcmp(tab[0], g_user_id, 16) && tab[2][15] == 3, "profile lister: three uids, first is the local user");
      CHECK(profile_lister(tab, 2) == 2, "profile lister respects the cap");
      service_register_account(); CHECK(applet_profiles(tab, 8, g_user_id) == 3, "account registered the lister with the applet core"); }
    unsetenv("NAPPLE_PROFILES"); profile_count_reset_for_test();
    CHECK(account_profile_count_spec("99") == ACCOUNT_MAX_PROFILES && account_profile_count_spec("0") == 1 && account_profile_count_spec("x") == 1 && account_profile_count_spec(NULL) == 1, "NAPPLE_PROFILES clamps");
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
