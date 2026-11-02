/* The game-side identity of a signed-in Mac (net_online_http.c): the principal id equals napple-online's identity::principal_id, the account needs no
 * /napple/v1/account round trip and the ID token is the Napple Account access token. The two hooks are defined here like src/online/ does. */
#include "check.h"
#include "svc_test.h"
#include "../src/net/net_online.h"

static const char *g_token = "hdr.payload.sig";
size_t napple_account_access_token(char *out, size_t n) {
    size_t l = strlen(g_token);
    if (l > n) return 0;
    memcpy(out, g_token, l);
    return l;
}
size_t napple_account_profile_id(char *out, size_t n) {
    const char *p = "01A19E82-70F0-73F3-9A8C-5A6770D17407";
    if (strlen(p) + 1 > n) return 0;
    strcpy(out, p);
    return strlen(p);
}

int main(void) {
    /* value computed with crates/napple-online identity::principal_id (SHA-256 of the lower-cased profile id, 47 bits) */
    CHECK(net_online_principal_id("01a19e82-70f0-73f3-9a8c-5a6770d17407") == 0x4e41d30e91ef35a9ull, "principal id %llx", (unsigned long long)net_online_principal_id("01a19e82-70f0-73f3-9a8c-5a6770d17407"));
    CHECK(net_online_principal_id("01A19E82-70F0-73F3-9A8C-5A6770D17407") == net_online_principal_id("01a19e82-70f0-73f3-9a8c-5a6770d17407"), "case-insensitive");
    NetOnlineAccount a;
    uint8_t uid[16] = {1};
    CHECK(net_online_account(NULL, NULL, NULL, uid, &a) == 0, "account without any server call");
    CHECK(a.pid == 0x4e41d30e91ef35a9ull && net_online_account_is_identity(&a), "identity account");
    char tok[64];
    size_t n = net_online_token(NULL, NULL, &a, 0x0100152000022000ull, tok, sizeof tok);
    CHECK(n == strlen(g_token) && !strcmp(tok, g_token), "the ID token is the account access token (%zu)", n);
    CHECK(net_online_report_friends(NULL, NULL, &a, NULL, 0) == 0, "friend report is a no-op for identity accounts");
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
