/* acc:u0: one offline local user. */
#include "../ipc.h"
#include "account_core.h"
#include "applet_lib_core.h"
#include "host_info.h"
#include "online_provider.h"
#include "hz_result.h"
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sys_default.h"

const uint8_t g_user_id[16] = {0x4e, 0x61, 0x70, 0x70, 0x6c, 0x65, 0x55, 0x73, 0x65, 0x72, 0, 0, 0, 0, 0, 1};

/* The profiles this process offers (NAPPLE_PROFILES, read once). */
static unsigned profile_n;
static unsigned profile_count(void) {
    if (!profile_n) profile_n = account_profile_count_spec(getenv("NAPPLE_PROFILES"));
    return profile_n;
}
#ifdef NAPPLE_ACCOUNT_TEST
static void profile_count_reset_for_test(void) { profile_n = 0; }
#endif
static unsigned profile_lister(uint8_t (*uids)[16], unsigned max) {
    unsigned n = profile_count();
    if (n > max) n = max;
    for (unsigned i = 0; i < n; i++) account_profile_uid(g_user_id, i, uids[i]);
    return n;
}
/* The IProfile object's state is its profile index + 1 (NULL = the first). */
static unsigned profile_index(const IpcObject *s) { return s && s->state ? (unsigned)(uintptr_t)s->state - 1 : 0; }
static void profile_get_base(IpcObject *s, IpcCall *c) {
    uint8_t base[0x38] = {0};
    account_profile_uid(g_user_id, profile_index(s), base);
    uint64_t t = 1600000000;
    memcpy(base + 16, &t, 8);
    char name[0x20];
    account_profile_name(host_user_name(), profile_index(s), name, sizeof name);
    snprintf((char *)base + 24, 0x20, "%s", name);
    ipc_out(c, base, sizeof base);
}
static void profile_get(IpcObject *s, IpcCall *c) {
    profile_get_base(s, c);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) {
        uint8_t z[0x80] = {0};
        ipc_write_buf(c, b, z, b->size < sizeof z ? b->size : sizeof z);
    }
}
/* The profile picture: the firmware's own default avatar (SubstituteUserIcon.jpg, extracted by `napple build` into <data dir>/../system/icon or
 * <app support>/system/icon). Without a firmware the profile has no image (size 0), which titles treat as "use your own placeholder". */
static uint8_t *icon_data;
static uint32_t icon_size;
static int icon_tried;
static void icon_load(void) {
    if (icon_tried) return;
    icon_tried = 1;
    const char *roots[3] = {g_cfg.data_dir, g_cfg.data_dir, napple_support_dir()};
    const char *sub[3] = {"%s/system/icon/SubstituteUserIcon.jpg", "%s/../system/icon/SubstituteUserIcon.jpg", "%s/system/icon/SubstituteUserIcon.jpg"};
    for (int i = 0; i < 3 && !icon_data; i++) {
        char path[1400];
        if (!roots[i] || !*roots[i]) continue;
        snprintf(path, sizeof path, sub[i], roots[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n <= (1 << 20) && (icon_data = malloc((size_t)n)) && fread(icon_data, 1, (size_t)n, f) == (size_t)n) icon_size = (uint32_t)n;
        else {
            free(icon_data);
            icon_data = NULL;
        }
        fclose(f);
        if (icon_data) LOG("acc", "profile picture: %u bytes from %s", icon_size, path);
    }
}
static void profile_get_image_size(IpcObject *s, IpcCall *c) {
    (void)s;
    icon_load();
    uint32_t v = icon_size;
    IPC_OUT(c, v);
}
static void profile_load_image(IpcObject *s, IpcCall *c) {
    (void)s;
    icon_load();
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint32_t n = 0;
    if (b && icon_data) {
        n = b->size < icon_size ? (uint32_t)b->size : icon_size;
        ipc_write_buf(c, b, icon_data, n);
    }
    IPC_OUT(c, n);
}
static const IpcCmd profile_cmds[] = {
    {0, "Get", profile_get, IPC_IMPL},
    {1, "GetBase", profile_get_base, IPC_IMPL},
    {10, "GetImageSize", profile_get_image_size, IPC_IMPL},
    {11, "LoadImage", profile_load_image, IPC_IMPL},
};
/* The network account of the local user, from the active provider (ONLINE_CAP_ACCOUNT). The NullProvider has none: the user is not linked, which titles
 * tolerate as "nothing cached" and offline play. Returns 1 and fills *out when the provider reports a linked account. */
static int account_linked_uid(const uint8_t uid[16], OnlineAccountInfo *out) {
    const OnlineProvider *p = online_provider();
    int (*info)(void *, const uint8_t[16], OnlineAccountInfo *) = online_has(p, ONLINE_CAP_ACCOUNT) ? ONLINE_FN(p, account_info) : NULL;
    memset(out, 0, sizeof *out);
    return info && info(p->ctx, uid, out) == 0 && out->linked;
}
static int account_linked(OnlineAccountInfo *out) { return account_linked_uid(g_user_id, out); }

/* ---- network operations (ipc-depth-95 r10) ---------------------------------------------------------------------------------
 * The BaaS calls (id token, license kind, application authentication, service availability) are asynchronous on the console: the command hands out an
 * IAsyncContext and the outcome is read with GetResult. The outcome is known here at once, so the context is done when it is made. */
/* The outcome of a network service account operation for `uid`. A user without a network service account (every user of the NullProvider and of
 * NAPPLE_NET=host, which has no ONLINE_CAP_ACCOUNT) gets NetworkServiceAccountRegistrationRequired whether or not there is a network. A linked user
 * (a custom provider module) would talk to the account server: OnlineProvider has no hook for that exchange (it would need an id-token / license-kind
 * call next to account_info), so the server is unreachable and the answer is nifm's no-network result, connected or not. */
static uint32_t acc_nsa_result(const uint8_t uid[16]) {
    OnlineAccountInfo ai;
    if (!account_linked_uid(uid, &ai)) return HZ_R_ACC_NSA_REGISTRATION_REQUIRED;
    return HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
}
typedef struct {
    uint32_t result;
    uint32_t license_kind; /* IAsyncNetworkServiceLicenseKindContext */
} AccAsync;
static void async_event(IpcObject *s, IpcCall *c) { svc_ret_event_signaled(s, c); } /* 0 GetSystemEvent: signalled, the work is done */
static void async_cancel(IpcObject *s, IpcCall *c) { /* 1 Cancel: nothing is in flight; the outcome stays */
    (void)s;
    (void)c;
}
static void async_has_done(IpcObject *s, IpcCall *c) { svc_ret_u8_1(s, c); }
static void async_get_result(IpcObject *s, IpcCall *c) { c->result = ((AccAsync *)s->state)->result; }
/* 100 GetNetworkServiceLicenseKind -> u32 (0 = no Nintendo Switch Online license [unverified value]): the context's result when it failed. */
static void async_license_kind(IpcObject *s, IpcCall *c) {
    AccAsync *a = s->state;
    if (a->result) {
        c->result = a->result;
        return;
    }
    IPC_OUT(c, a->license_kind);
}
static const IpcCmd async_cmds[] = {
    {0, "GetSystemEvent", async_event, IPC_IMPL},
    {1, "Cancel", async_cancel, IPC_IMPL},
    {2, "HasDone", async_has_done, IPC_IMPL},
    {3, "GetResult", async_get_result, IPC_IMPL},
};
static const IpcCmd license_ctx_cmds[] = {
    {0, "GetSystemEvent", async_event, IPC_IMPL},     {1, "Cancel", async_cancel, IPC_IMPL},
    {2, "HasDone", async_has_done, IPC_IMPL},         {3, "GetResult", async_get_result, IPC_IMPL},
    {100, "GetNetworkServiceLicenseKind", async_license_kind, IPC_IMPL},
};
static void async_destroy(IpcObject *o) { free(o->state); }
static void out_async(IpcCall *c, uint32_t result, int license) {
    AccAsync *a = calloc(1, sizeof *a);
    a->result = result;
    if (license) ipc_out_object(c, IPC_OBJECT("IAsyncNetworkServiceLicenseKindContext", license_ctx_cmds, a, async_destroy));
    else ipc_out_object(c, IPC_OBJECT("IAsyncContext", async_cmds, a, async_destroy));
}
/* The IManagerForApplication object is the account manager of one user (GetBaasAccountManagerForApplication(uid)); it remembers how its last
 * EnsureIdTokenCacheAsync ended. */
typedef struct {
    uint8_t uid[16];
    int ensured;
    uint32_t ensure_result;
} AccManager;
static IpcObject *manager_new(const uint8_t uid[16]);
/* Users whose open context was stored (160 StoreOpenContext), for the next application to pick up (acc:u0 130 / 131). */
static pthread_mutex_t g_open_ctx_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_open_ctx[ACCOUNT_MAX_PROFILES][16];
static unsigned g_open_ctx_n;
/* 0 CheckAvailability: the local user is always usable; whether a network account sits behind it is the provider's business and is not an error either way. */
static void manager_check(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
/* 1 GetAccountId -> u64: the provider's id for a linked account, else the fixed local id. */
static void manager_account_id(IpcObject *s, IpcCall *c) {
    (void)s;
    OnlineAccountInfo ai;
    uint64_t v = account_linked(&ai) ? ai.id : 0x4e61707070304e53ULL;
    IPC_OUT(c, v);
}
/* GetNintendoAccountUserResourceCacheForApplication: -> u64 id and the 0x68-byte cache buffer, both the provider's opaque values when the user is linked; else id 0
 * and an all-zero cache (no result error: titles tolerate "nothing cached"). */
static void manager_na_cache(IpcObject *s, IpcCall *c) {
    (void)s;
    OnlineAccountInfo ai;
    int linked = account_linked(&ai);
    uint64_t v = linked ? ai.id : 0;
    IPC_OUT(c, v);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) {
        uint8_t z[ONLINE_ACCOUNT_CACHE_SIZE] = {0};
        if (linked) memcpy(z, ai.resource_cache, sizeof z);
        ipc_write_buf(c, b, z, b->size < sizeof z ? b->size : sizeof z);
    }
}
/* 2 EnsureIdTokenCacheAsync -> IAsyncContext: fetches the user's id token into the manager's cache; the outcome is acc_nsa_result (no token can be
 * fetched without an id-token provider hook, so the cache never fills). */
static void manager_ensure_token(IpcObject *s, IpcCall *c) {
    AccManager *m = s->state;
    uint32_t r = acc_nsa_result(m ? m->uid : g_user_id);
    if (m) m->ensured = 1, m->ensure_result = r;
    out_async(c, r, 0);
}
/* 3 LoadIdTokenCache (LoadIdTokenCacheDeprecated since 19.0.0) / 4 LoadIdTokenCache [19.0.0+] (out buffer) -> u32 size: copies the cached id token. The
 * cache is empty: the call fails with how the last EnsureIdTokenCacheAsync ended, or (none yet) with the user's account state. A cache that could be
 * filled would be copied here; with r == 0 and nothing cached the size is 0. */
static void manager_load_token(IpcObject *s, IpcCall *c) {
    AccManager *m = s->state;
    uint32_t r = m && m->ensured ? m->ensure_result : acc_nsa_result(m ? m->uid : g_user_id);
    if (r) {
        c->result = r;
        return;
    }
    uint32_t n = 0;
    IPC_OUT(c, n);
}
/* 160 StoreOpenContext: keeps this manager's user as an open context the next application can load (acc:u0 130 LoadOpenContext). */
static void manager_store_open_context(IpcObject *s, IpcCall *c) {
    (void)c;
    AccManager *m = s->state;
    const uint8_t *uid = m ? m->uid : g_user_id;
    pthread_mutex_lock(&g_open_ctx_mu);
    unsigned i = 0;
    while (i < g_open_ctx_n && memcmp(g_open_ctx[i], uid, 16)) i++;
    if (i == g_open_ctx_n && g_open_ctx_n < ACCOUNT_MAX_PROFILES) memcpy(g_open_ctx[g_open_ctx_n++], uid, 16);
    pthread_mutex_unlock(&g_open_ctx_mu);
}
/* 170 LoadNetworkServiceLicenseKindAsync -> IAsyncNetworkServiceLicenseKindContext: the license kind needs the account server (acc_nsa_result). */
static void manager_license_kind(IpcObject *s, IpcCall *c) {
    AccManager *m = s->state;
    out_async(c, acc_nsa_result(m ? m->uid : g_user_id), 1);
}
static const IpcCmd manager_cmds[] = {{0, "CheckAvailability", manager_check, IPC_IMPL},
                                      {1, "GetAccountId", manager_account_id, IPC_IMPL},
                                      {130, "GetNintendoAccountUserResourceCacheForApplication", manager_na_cache, IPC_IMPL},
    {2, "EnsureIdTokenCacheAsync", manager_ensure_token, IPC_IMPL},
    {3, "LoadIdTokenCacheDeprecated", manager_load_token, IPC_IMPL},
    {4, "LoadIdTokenCache", manager_load_token, IPC_IMPL},
    /* 150: IAuthorizationRequest (an OAuth flow with the Nintendo Account server through transfer memory); the input layout is not documented */
    {150, "CreateAuthorizationRequest", sysd_err_generic, IPC_STUB},
    {160, "StoreOpenContext", manager_store_open_context, IPC_IMPL},
    {170, "LoadNetworkServiceLicenseKindAsync", manager_license_kind, IPC_IMPL}};
static void manager_destroy(IpcObject *o) { free(o->state); }
static IpcObject *manager_new(const uint8_t uid[16]) {
    AccManager *m = calloc(1, sizeof *m);
    if (m) memcpy(m->uid, uid, 16);
    return IPC_OBJECT("IManagerForApplication", manager_cmds, m, manager_destroy);
}

static void acc_user_count(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = (int32_t)profile_count();
    IPC_OUT(c, v);
}
static void acc_user_exists(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = account_profile_find(g_user_id, profile_count(), ipc_in(c, 0, 16)) >= 0;
    IPC_OUT(c, v);
}
static void acc_list_users(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) {
        uint8_t list[16 * ACCOUNT_MAX_PROFILES] = {0};
        for (unsigned i = 0; i < profile_count(); i++) account_profile_uid(g_user_id, i, list + 16 * i);
        uint64_t n = b->size < sizeof list ? b->size : sizeof list;
        ipc_write_buf(c, b, list, n);
    }
}
static void acc_last_opened(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out(c, g_user_id, 16);
}
static void acc_get_profile(IpcObject *s, IpcCall *c) {
    (void)s;
    int i = account_profile_find(g_user_id, profile_count(), ipc_in(c, 0, 16));
    ipc_out_object(c, IPC_OBJECT("IProfile", profile_cmds, (void *)(uintptr_t)(i > 0 ? (unsigned)i + 1 : 0), NULL));
}
/* 6 GetProfileDigest(uid) -> ProfileDigest (0x10): a digest of the profile's base data (uid, creation time, nickname), so it changes exactly when the profile does. An
 * unknown uid is the usual "user not found". FNV-1a over 128 bits' worth of two lanes; the console's own hash is not reproduced (the SDK only compares digests). */
#define R_ACC_USER_NOT_FOUND HZ_RESULT(124, 100) /* [unverified] */
static void profile_base(unsigned index, uint8_t base[0x38]) {
    memset(base, 0, 0x38);
    account_profile_uid(g_user_id, index, base);
    uint64_t t = 1600000000;
    memcpy(base + 16, &t, 8);
    char name[0x20];
    account_profile_name(host_user_name(), index, name, sizeof name);
    snprintf((char *)base + 24, 0x20, "%s", name);
}
static void acc_profile_digest(IpcObject *s, IpcCall *c) {
    (void)s;
    int i = account_profile_find(g_user_id, profile_count(), ipc_in(c, 0, 16));
    if (i < 0) {
        c->result = R_ACC_USER_NOT_FOUND;
        return;
    }
    uint8_t base[0x38];
    profile_base((unsigned)i, base);
    uint64_t h[2] = {1469598103934665603ull, 0x9e3779b97f4a7c15ull};
    for (unsigned k = 0; k < sizeof base; k++) {
        h[0] = (h[0] ^ base[k]) * 1099511628211ull;
        h[1] = (h[1] ^ base[k]) * 0x100000001b3ull + h[0];
    }
    ipc_out(c, h, sizeof h);
}
/* 110 StoreSaveDataThumbnail(uid; in: thumbnail) / 111 ClearSaveDataThumbnail(uid): the thumbnail the save-data list shows for this title and user, kept as a file
 * below the title's save root. */
static void thumbnail_path(const uint8_t *uid, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s/thumbnail-%02x%02x%02x%02x%02x%02x%02x%02x.bin", g_cfg.save_root, g_cfg.title_id, uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7]);
}
static void acc_store_thumbnail(IpcObject *s, IpcCall *c) {
    (void)s;
    const uint8_t *uid = ipc_in(c, 0, 16);
    const IpcBuf *b = ipc_in_buf(c, 0);
    const void *p = b ? ipc_buf_ptr(b) : NULL;
    if (account_profile_find(g_user_id, profile_count(), uid) < 0) {
        c->result = R_ACC_USER_NOT_FOUND;
        return;
    }
    if (!p || !b->size || b->size > (1u << 20)) {
        c->result = HZ_RESULT(124, 20); /* [unverified] invalid argument */
        return;
    }
    char path[1400];
    thumbnail_path(uid, path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, (size_t)b->size, f) != (size_t)b->size) c->result = HZ_RESULT(124, 20);
    if (f) fclose(f);
}
static void acc_clear_thumbnail(IpcObject *s, IpcCall *c) {
    (void)s;
    const uint8_t *uid = ipc_in(c, 0, 16);
    if (account_profile_find(g_user_id, profile_count(), uid) < 0) {
        c->result = R_ACC_USER_NOT_FOUND;
        return;
    }
    char path[1400];
    thumbnail_path(uid, path, sizeof path);
    unlink(path); /* clearing a thumbnail that was never stored is not an error */
}
static void acc_bool_false(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 0;
    IPC_OUT(c, v);
}
static void acc_bool_true(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 1;
    IPC_OUT(c, v);
}
/* InitializeApplicationInfo{V0,,V2}: the SDK calls one of them before it asks for the account manager; remembered per connection (the title's own pid is the only
 * application there is, so the info itself carries nothing to keep). The manager is handed out either way: a title that skipped it still gets its answers. */
typedef struct {
    uint8_t app_info_initialized;
} AccSession;
static void acc_initialize(IpcObject *s, IpcCall *c) {
    (void)c;
    if (s->state) ((AccSession *)s->state)->app_info_initialized = 1;
}
static void acc_baas_manager(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, manager_new(c->in_size >= 16 ? ipc_in(c, 0, 16) : g_user_id));
}
/* 102 AuthenticateApplicationAsync -> IAsyncContext: authenticates the application (not a user) with the account server. Offline that ends with nifm's
 * no-network result. With a connection (NAPPLE_NET=host) a console would succeed, but the exchange needs an application-authentication provider hook that
 * OnlineProvider lacks, so the answer stays the offline one: the row stays IPC_STUB until such a hook exists. */
static void acc_authenticate_app(IpcObject *s, IpcCall *c) {
    (void)s;
    out_async(c, HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, 0);
}
/* 103 CheckNetworkServiceAvailabilityAsync [4.0.0+] -> IAsyncContext: whether the user can use network services. The input is not documented: a leading
 * uid is read when present [unverified], else the local user. */
static void acc_check_nsa_availability(IpcObject *s, IpcCall *c) {
    (void)s;
    out_async(c, acc_nsa_result(c->in_size >= 16 ? ipc_in(c, 0, 16) : g_user_id), 0);
}
/* 130 LoadOpenContext(uid [unverified layout]) -> IManagerForApplication of a user whose context an application stored; another uid is UserNotExist. */
static void acc_load_open_context(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 16) {
        c->result = HZ_RESULT(124, 20); /* InvalidUserId */
        return;
    }
    const uint8_t *uid = ipc_in(c, 0, 16);
    pthread_mutex_lock(&g_open_ctx_mu);
    int found = 0;
    for (unsigned i = 0; i < g_open_ctx_n && !found; i++) found = !memcmp(g_open_ctx[i], uid, 16);
    pthread_mutex_unlock(&g_open_ctx_mu);
    if (!found) {
        c->result = R_ACC_USER_NOT_FOUND;
        return;
    }
    ipc_out_object(c, manager_new(uid));
}
/* 131 ListOpenContextStoredUsers [6.0.0+] (out buffer uid[]): the users with a stored open context, the rest of the buffer zero (like ListAllUsers). */
static void acc_list_open_context_users(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b) return;
    uint8_t list[16 * ACCOUNT_MAX_PROFILES] = {0};
    pthread_mutex_lock(&g_open_ctx_mu);
    memcpy(list, g_open_ctx, 16 * g_open_ctx_n);
    pthread_mutex_unlock(&g_open_ctx_mu);
    ipc_write_buf(c, b, list, b->size < sizeof list ? b->size : sizeof list);
}
static void acc_destroy(IpcObject *o) { free(o->state); }
static const IpcCmd acc_cmds[] = {
    {0, "GetUserCount", acc_user_count, IPC_IMPL},
    {1, "GetUserExistence", acc_user_exists, IPC_IMPL},
    {2, "ListAllUsers", acc_list_users, IPC_IMPL},
    {3, "ListOpenUsers", acc_list_users, IPC_IMPL},
    {4, "GetLastOpenedUser", acc_last_opened, IPC_IMPL},
    {5, "GetProfile", acc_get_profile, IPC_IMPL},
    {50, "IsUserRegistrationRequestPermitted", acc_bool_false, IPC_IMPL}, /* no registration flow exists: the one local user cannot be added to */
    {51, "TrySelectUserWithoutInteraction", acc_last_opened, IPC_IMPL},
    {100, "InitializeApplicationInfoV0", acc_initialize, IPC_IMPL},
    {101, "GetBaasAccountManagerForApplication", acc_baas_manager, IPC_IMPL},
    {140, "InitializeApplicationInfo", acc_initialize, IPC_IMPL},
    {141, "ListQualifiedUsers", acc_list_users, IPC_IMPL},
    {150, "IsUserAccountSwitchLocked", acc_bool_false, IPC_IMPL},
    {160, "InitializeApplicationInfoV2", acc_initialize, IPC_IMPL},
    {6, "GetProfileDigest", acc_profile_digest, IPC_IMPL},
    {52, "TrySelectUserWithoutInteraction", acc_last_opened, IPC_IMPL}, /* 19.0.0+ name of 51: the one local user (or the last opened one) is selected without a dialog */
    {102, "AuthenticateApplicationAsync", acc_authenticate_app, IPC_STUB}, /* offline answer only: no application-authentication provider hook */
    {103, "CheckNetworkServiceAvailabilityAsync", acc_check_nsa_availability, IPC_IMPL},
    {110, "StoreSaveDataThumbnail", acc_store_thumbnail, IPC_IMPL},
    {111, "ClearSaveDataThumbnail", acc_clear_thumbnail, IPC_IMPL},
    /* 120: IGuestLoginRequest is filled by the Nintendo Account web login (a system applet + server); the transfer-memory input is not documented */
    {120, "CreateGuestLoginRequest", sysd_err_generic, IPC_STUB},
    {130, "LoadOpenContext", acc_load_open_context, IPC_IMPL},
    {131, "ListOpenContextStoredUsers", acc_list_open_context_users, IPC_IMPL},
};
static IpcObject *acc_open(void) { return IPC_OBJECT("acc:u0", acc_cmds, calloc(1, sizeof(AccSession)), acc_destroy); }
void service_register_account(void) {
    applet_set_profile_lister(profile_lister);
    ipc_register_service("acc:u0", acc_open);
    ipc_register_service("acc:u1", acc_open);
    (void)acc_bool_true;
}
