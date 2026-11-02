/* Unit tests: friend:u rows backed by the local friend store (friend_core.c) and the provider (ipc-depth-95). Offline (NullProvider: no account linked)
 * every cache / sync row gives the "no network service account" answer while the local rows (play history, in-person requests, own presence) work;
 * with the loopback provider (account linked, friends, network) the cache rows answer from its friend list and the sync rows succeed. */
#include "../src/services/offline.c"
#include "svc_test.h"
#include "online_loopback.h"

static IpcCall k;
static uint8_t kin[0x40];
/* A call with raw input {u32 a, u32 b, uid} (or {uid, u64 x} when lead == 0), optional input buffers and one output buffer. */
static IpcCall *fc(IpcObject *o, unsigned cmd, int lead, uint32_t a, uint32_t b, const uint8_t *uid, uint64_t x, const void *i0, uint64_t n0,
                   const void *i1, uint64_t n1, const void *i2, uint64_t n2, void *out, uint64_t m) {
    memset(&k, 0, sizeof k);
    memset(kin, 0, sizeof kin);
    if (lead) {
        memcpy(kin, &a, 4), memcpy(kin + 4, &b, 4);
        if (uid) memcpy(kin + 8, uid, 16);
        k.in_size = 24;
    } else {
        if (uid) memcpy(kin, uid, 16);
        memcpy(kin + 16, &x, 8);
        k.in_size = 24;
    }
    k.in = kin;
    if (i0) svc_set_in_buf(&k, i0, n0);
    if (i1) svc_set_in_buf(&k, i1, n1);
    if (i2) svc_set_in_buf(&k, i2, n2);
    if (out) svc_set_out_buf(&k, out, m);
    svc_call_c(o, cmd, &k);
    return &k;
}
#define U(o, cmd) fc(o, cmd, 0, 0, 0, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0)
static void make_key(uint8_t key[0x40], uint8_t last, int local) {
    uint8_t other[16] = {0x4f, 0x74, 0x68, 0x65, 0x72};
    other[15] = last;
    uint8_t inst[16] = {1, 2, 3};
    install_play_history_key(inst, other, local, key);
}
static void make_name(uint8_t n[FRIEND_SCREEN_NAME_SIZE], const char *s) {
    memset(n, 0, FRIEND_SCREEN_NAME_SIZE);
    snprintf((char *)n, 0x40, "%s", s);
}
static const unsigned cache_rows[] = {10120, 10420, 10500, 10501, 10502, 10503, 20102, 20107, 20110, 20111, 20203, 20300, 20501, 20500, 20502,
                                      20800, 20802, 22001, 22003, 22010};
static const unsigned list_rows[] = {20201, 20202, 20301, 20400, 20402, 22000, 22002};
static const unsigned sync_rows[] = {10102, 10121, 10421, 20103, 20104, 20401, 20801};

int main(void) {
    IpcCall c;
    const uint32_t NSA = HZ_R_FRIENDS_NETWORK_SERVICE_ACCOUNT_UNAVAILABLE;
    /* ---- friend_core ---- */
    FriendStore st;
    memset(&st, 0, sizeof st);
    static const uint8_t zero[16];
    CHECK(friend_user(&st, zero) == NULL, "a zero user id has no record");
    FriendUser *fu = friend_user(&st, g_user_id);
    CHECK(fu && friend_user(&st, g_user_id) == fu && fu->presence_permission == 2 && fu->friend_request_reception == 1, "user record with defaults, found again");
    uint8_t key[0x40], key2[0x40], own[0x40], nm[FRIEND_SCREEN_NAME_SIZE], me[FRIEND_SCREEN_NAME_SIZE];
    make_key(key, 1, 1), make_key(key2, 2, 0), make_name(nm, "Peach"), make_name(me, "Mario");
    uint8_t inst[16] = {9};
    install_play_history_key(inst, g_user_id, 1, own);
    CHECK(friend_key_valid(key) && !friend_key_valid(zero), "key shape");
    CHECK(friend_history_add(fu, own, nm, me, 1, 10) == -2, "own key refused");
    uint8_t bad[0x40] = {0};
    CHECK(friend_history_add(fu, bad, nm, me, 1, 10) == -1, "malformed key refused");
    CHECK(friend_history_add(fu, key, nm, me, 0x0100000000010000ull, 100) == 0 && friend_history_add(fu, key2, nm, me, 2, 200) == 0 &&
              friend_history_add(fu, key, nm, me, 3, 300) == 0, "three entries");
    uint64_t stv[3];
    friend_history_stats(fu, stv);
    CHECK(stv[0] == 3 && stv[1] == 2 && stv[2] == 1, "statistics total/local/online");
    uint8_t rec[4 * FRIEND_HISTORY_RECORD_SIZE];
    CHECK(friend_history_encode(fu, 0, rec, 4) == 3 && rec[15] == 1 && *(int64_t *)(rec + 0xa8) == 300 && *(uint64_t *)(rec + 0xa0) == 3, "newest first");
    CHECK(friend_history_encode(fu, 2, rec, 4) == 1 && *(int64_t *)(rec + 0xa8) == 100 && !strcmp((char *)rec + 0x10, "Peach") &&
              !strcmp((char *)rec + 0x58, "Mario") && rec[0xb0] == 1, "offset, names, local flag");
    CHECK(friend_candidates_encode(fu, rec, 4) == 2, "two distinct players");
    for (int i = 0; i < FRIEND_HISTORY_MAX + 5; i++) friend_history_add(fu, key2, nm, me, 9, 1000 + i);
    CHECK(fu->nhistory == FRIEND_HISTORY_MAX && fu->history[FRIEND_HISTORY_MAX - 1].time == 1000 + FRIEND_HISTORY_MAX + 4, "full history drops the oldest");
    CHECK(friend_faced_add(fu, key, nm, me, 1, 5) == 0 && friend_faced_add(fu, key, nm, me, 1, 6) == -3 && fu->nfaced == 1 && friend_faced_pending(fu, key + 0x10),
          "faced request queued once");
    char code[0x20], code2[0x20];
    friend_code(inst, g_user_id, code, sizeof code);
    friend_code(inst, g_user_id, code2, sizeof code2);
    CHECK(strlen(code) == 17 && !strncmp(code, "SW-", 3) && code[7] == '-' && code[12] == '-' && !strcmp(code, code2), "friend code shape, stable");
    inst[0] = 8;
    friend_code(inst, g_user_id, code2, sizeof code2);
    CHECK(strcmp(code, code2) != 0, "another installation, another code");
    uint8_t us[FRIEND_USER_SETTING_SIZE];
    friend_user_setting_encode(fu, inst, us);
    CHECK(!memcmp(us, g_user_id, 16) && *(uint32_t *)(us + 0x10) == 2 && *(uint64_t *)(us + 0x18) == 1 && !strcmp((char *)us + 0x20, code2), "FriendsUserSetting layout");

    /* ---- offline (NullProvider, no account linked) ---- */
    online_provider_install(NULL);
    memset(&friend_store, 0, sizeof friend_store);
    IpcObject *fr = friend_open();
    svc_call(fr, 0, NULL, 0, &c);
    IpcObject *fs = c.out_objects[0];
    for (size_t i = 0; i < sizeof cache_rows / sizeof cache_rows[0]; i++)
        CHECK(U(fs, cache_rows[i])->result == NSA, "offline cache row %u: no network service account", cache_rows[i]);
    for (size_t i = 0; i < sizeof list_rows / sizeof list_rows[0]; i++)
        CHECK(fc(fs, list_rows[i], 1, 0, 1, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0)->result == NSA, "offline list row %u", list_rows[i]);
    for (size_t i = 0; i < sizeof sync_rows / sizeof sync_rows[0]; i++)
        CHECK(U(fs, sync_rows[i])->result == NSA, "offline sync row %u", sync_rows[i]);
    CHECK(fc(fs, 10701, 1, 1, 0, NULL, 0x1234abcd5678ef90ull, NULL, 0, NULL, 0, NULL, 0, NULL, 0)->result == NSA, "no linked account id offline");
    /* local rows work offline */
    CHECK(fc(fs, 10702, 0, 0, 0, zero, 0, key, 0x40, nm, sizeof nm, me, sizeof me, NULL, 0)->result == NSA, "AddPlayHistory: zero user");
    CHECK(fc(fs, 10702, 0, 0, 0, g_user_id, 0, bad, 0x40, nm, sizeof nm, me, sizeof me, NULL, 0)->result == HZ_R_FRIENDS_INVALID_KEY, "AddPlayHistory: bad key");
    CHECK(fc(fs, 10702, 0, 0, 0, g_user_id, 0, key, 0x40, nm, sizeof nm, me, sizeof me, NULL, 0)->result == 0 &&
              fc(fs, 10702, 0, 0, 0, g_user_id, 0, key2, 0x40, nm, sizeof nm, me, sizeof me, NULL, 0)->result == 0, "AddPlayHistory offline");
    uint8_t lst[8 * FRIEND_HISTORY_RECORD_SIZE];
    IpcCall *r = fc(fs, 20700, 1, 0, 0, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, lst, sizeof lst);
    CHECK(r->result == 0 && SVC_OUT(r, uint32_t, 0) == 2 && lst[15] == 2 && !strcmp((char *)lst + 0x10, "Peach"), "GetPlayHistoryListV1 reads back newest first");
    r = fc(fs, 20702, 1, 1, 0, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, lst, sizeof lst);
    CHECK(r->result == 0 && SVC_OUT(r, uint32_t, 0) == 1 && lst[15] == 1, "GetPlayHistoryListV2 with an offset");
    r = U(fs, 20701);
    CHECK(r->result == 0 && r->out_size == 24 && SVC_OUT(r, uint64_t, 0) == 2 && SVC_OUT(r, uint64_t, 8) == 1 && SVC_OUT(r, uint64_t, 16) == 1, "GetPlayHistoryStatistics");
    CHECK(fc(fs, 10211, 0, 0, 0, g_user_id, 0, key, 0x40, nm, sizeof nm, me, sizeof me, NULL, 0)->result == 0 &&
              fc(fs, 10211, 0, 0, 0, g_user_id, 0, key, 0x40, nm, sizeof nm, me, sizeof me, NULL, 0)->result == 0 &&
              fc(fs, 10211, 0, 0, 0, g_user_id, 0, own, 0x40, nm, sizeof nm, me, sizeof me, NULL, 0)->result == HZ_R_FRIENDS_INVALID_KEY,
          "AddFacedFriendRequestForApplication queues (idempotent), refuses the user's own key");
    CHECK(friend_user(&friend_store, g_user_id)->nfaced == 1, "one faced request");
    uint8_t pres[0x40] = {0xab, 0xcd}, view[FRIEND_PRESENCE_MAX];
    fc(fs, 10610, 0, 0, 0, g_user_id, 0, pres, sizeof pres, NULL, 0, NULL, 0, NULL, 0);
    memset(view, 0, sizeof view);
    r = fc(fs, 20600, 0, 0, 0, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, view, sizeof view);
    CHECK(r->result == 0 && view[0] == 0xab && view[1] == 0xcd, "GetUserPresenceViewV1 reads back UpdateUserPresence");
    memset(view, 0, sizeof view);
    CHECK(fc(fs, 20601, 0, 0, 0, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, view, sizeof view)->result == 0 && view[0] == 0xab, "GetUserPresenceViewV2");
    CHECK(U(fs, 20900)->result == 0, "RequestListSummaryOverlayNotification accepted (no overlay process)");
    CHECK(fc(fs, 20701, 0, 0, 0, zero, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0)->result == NSA, "local rows still need a user");

    /* ---- provider: account linked, friends, network ---- */
    memset(&lb, 0, sizeof lb);
    online_provider_install(&lb_provider);
    for (size_t i = 0; i < sizeof sync_rows / sizeof sync_rows[0]; i++)
        CHECK(U(fs, sync_rows[i])->result == NSA, "provider without a linked account: sync row %u", sync_rows[i]);
    lb.account_linked = 1;
    for (size_t i = 0; i < sizeof sync_rows / sizeof sync_rows[0]; i++) CHECK(U(fs, sync_rows[i])->result == 0, "sync row %u with the provider", sync_rows[i]);
    CHECK(U(fs, 10120)->result == 0 && U(fs, 10420)->result == 0, "Check*Availability with a linked account");
    svc_call(fs, 0, NULL, 0, &c);
    uint32_t ev = c.out_copy[0];
    CHECK(!event_is_signaled(ev), "completion event clear");
    CHECK(U(fs, 20104)->result == 0 && event_is_signaled(ev), "RequestSyncFriendList signals completion");
    uint64_t ids[3] = {0x1001, 0x9999, 0x1000};
    uint8_t prof[3 * 0x40];
    memset(prof, 0xee, sizeof prof);
    r = fc(fs, 10500, 0, 0, 0, g_user_id, 0, ids, sizeof ids, NULL, 0, NULL, 0, prof, sizeof prof);
    CHECK(r->result == 0 && *(uint64_t *)prof == 0x1001 && !strcmp((char *)prof + 8, "friend1") && prof[0x30] == 1 && prof[0x40 + 0x30] == 0 &&
              *(uint64_t *)(prof + 0x40) == 0 && *(uint64_t *)(prof + 0x80) == 0x1000, "GetProfileList: friends filled, a stranger zero");
    for (unsigned cmd = 10501; cmd <= 10503; cmd++) {
        memset(prof, 0, sizeof prof);
        CHECK(fc(fs, cmd, 0, 0, 0, g_user_id, 0, ids, sizeof ids, NULL, 0, NULL, 0, prof, sizeof prof)->result == 0 && *(uint64_t *)prof == 0x1001, "profile row %u", cmd);
    }
    memset(prof, 0, sizeof prof);
    CHECK(fc(fs, 20500, 0, 0, 0, g_user_id, 0, ids, sizeof ids, NULL, 0, NULL, 0, prof, sizeof prof)->result == 0 && prof[0x80 + 0x30] == 1, "GetProfileExtraListV1");
    CHECK(fc(fs, 20502, 0, 0, 0, g_user_id, 0, ids, sizeof ids, NULL, 0, NULL, 0, prof, sizeof prof)->result == 0, "GetProfileExtraListV2");
    static uint8_t det[FRIEND_DETAIL_RECORD_SIZE];
    r = fc(fs, 20102, 0, 0, 0, g_user_id, 0x1000, NULL, 0, NULL, 0, NULL, 0, det, sizeof det);
    CHECK(r->result == 0 && *(uint64_t *)det == 0x1000 && !strcmp((char *)det + 8, "friend0") && det[0x30] == 1 && det[0x31] == 1, "GetFriendDetailedInfo");
    CHECK(fc(fs, 20107, 0, 0, 0, g_user_id, 0x5555, NULL, 0, NULL, 0, NULL, 0, det, sizeof det)->result == HZ_R_FRIENDS_NOT_FOUND, "V2: not a friend");
    uint8_t set[FRIEND_SETTING_RECORD_SIZE];
    r = fc(fs, 20110, 0, 0, 0, g_user_id, 0x1001, NULL, 0, NULL, 0, NULL, 0, set, sizeof set);
    CHECK(r->result == 0 && *(uint64_t *)set == 0x1001 && set[8] == 1 && set[9] == 0 && set[10] == 1, "LoadFriendSettingV1 defaults");
    CHECK(fc(fs, 20111, 0, 0, 0, g_user_id, 0x42, NULL, 0, NULL, 0, NULL, 0, set, sizeof set)->result == HZ_R_FRIENDS_NOT_FOUND, "LoadFriendSettingV2 stranger");
    r = fc(fs, 20501, 0, 0, 0, g_user_id, 0x1000, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    CHECK(r->result == 0 && r->out_size == 8 && r->out[0] == 1 && r->out[1] == 0, "GetRelationship: friend");
    uint64_t other_id;
    memcpy(&other_id, key + 0x10, 8);
    r = fc(fs, 20501, 0, 0, 0, g_user_id, other_id, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    CHECK(r->result == 0 && r->out[0] == 0 && r->out[1] == 1, "GetRelationship: in-person request pending");
    uint8_t reqs[4 * FRIEND_FACED_RECORD_SIZE];
    r = fc(fs, 20201, 1, 0, 1, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, reqs, sizeof reqs);
    CHECK(r->result == 0 && SVC_OUT(r, uint32_t, 0) == 1 && !strcmp((char *)reqs + 0x10, "Peach"), "GetFriendRequestListV1 (sent) lists the faced requests");
    r = fc(fs, 20202, 1, 0, 0, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, reqs, sizeof reqs);
    CHECK(r->result == 0 && SVC_OUT(r, uint32_t, 0) == 0, "GetFriendRequestListV2 (received) empty");
    for (size_t i = 2; i < sizeof list_rows / sizeof list_rows[0]; i++) {
        r = fc(fs, list_rows[i], 1, 0, 0, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
        CHECK(r->result == 0 && r->out_size == 4 && SVC_OUT(r, uint32_t, 0) == 0, "empty cached list %u", list_rows[i]);
    }
    CHECK(U(fs, 20203)->result == 0 && k.out_size == 4 && U(fs, 22010)->result == 0, "cached counts");
    CHECK(U(fs, 22001)->result == HZ_R_FRIENDS_NOT_FOUND && U(fs, 22003)->result == HZ_R_FRIENDS_NOT_FOUND, "no invitation was received");
    uint8_t cand[8 * FRIEND_HISTORY_RECORD_SIZE];
    r = fc(fs, 20300, 0, 0, 0, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, cand, sizeof cand);
    CHECK(r->result == 0 && SVC_OUT(r, uint32_t, 0) == 2, "GetFriendCandidateList: the two players met");
    r = fc(fs, 20800, 0, 0, 0, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, us, sizeof us);
    CHECK(r->result == 0 && !memcmp(us, g_user_id, 16) && !strncmp((char *)us + 0x20, "SW-", 3), "LoadUserSettingV1");
    memset(us, 0, sizeof us);
    CHECK(fc(fs, 20802, 0, 0, 0, g_user_id, 0, NULL, 0, NULL, 0, NULL, 0, us, sizeof us)->result == 0 && us[0x20] == 'S', "LoadUserSettingV2");
    uint8_t hk[0x40];
    r = fc(fs, 10701, 1, 1, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, hk, sizeof hk);
    CHECK(r->result == NSA, "10701: id 0");
    memset(kin, 0, sizeof kin);
    memset(&k, 0, sizeof k);
    uint32_t local = 1;
    uint64_t nsa_id = 0x1234abcd5678ef90ull;
    memcpy(kin, &local, 4), memcpy(kin + 8, &nsa_id, 8);
    k.in = kin, k.in_size = 16;
    svc_set_out_buf(&k, hk, sizeof hk);
    svc_call_c(fs, 10701, &k);
    CHECK(k.result == 0 && friend_key_valid(hk) && hk[4] == 1 && !memcmp(hk + 0x10, g_user_id, 16), "10701: the key of the linked user");
    nsa_id ^= 1;
    memcpy(kin + 8, &nsa_id, 8);
    svc_call_c(fs, 10701, &k);
    CHECK(k.result == NSA, "10701: another account id");
    /* linked but no network: sync rows fail with "no network", cache rows still answer */
    OnlineProvider nonet = lb_provider;
    nonet.net_info = NULL;
    online_provider_install(&nonet);
    CHECK(U(fs, 20103)->result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED && U(fs, 10120)->result == 0, "sync needs the network, the cache does not");
    online_provider_install(NULL);

    printf("{\"test\":\"svc_friend_r10\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
