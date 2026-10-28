/* acc:u0: one offline local user. */
#include "../ipc.h"
#include "account_core.h"
#include "applet_lib_core.h"
#include "host_info.h"
#include "online_provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int account_linked(OnlineAccountInfo *out) {
    const OnlineProvider *p = online_provider();
    int (*info)(void *, const uint8_t[16], OnlineAccountInfo *) = online_has(p, ONLINE_CAP_ACCOUNT) ? ONLINE_FN(p, account_info) : NULL;
    memset(out, 0, sizeof *out);
    return info && info(p->ctx, g_user_id, out) == 0 && out->linked;
}
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
static const IpcCmd manager_cmds[] = {{0, "CheckAvailability", manager_check, IPC_IMPL},
                                      {1, "GetAccountId", manager_account_id, IPC_IMPL},
                                      {130, "GetNintendoAccountUserResourceCacheForApplication", manager_na_cache, IPC_IMPL}};

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
    ipc_out_object(c, IPC_OBJECT("IManagerForApplication", manager_cmds, NULL, NULL));
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
};
static IpcObject *acc_open(void) { return IPC_OBJECT("acc:u0", acc_cmds, calloc(1, sizeof(AccSession)), acc_destroy); }
void service_register_account(void) {
    applet_set_profile_lister(profile_lister);
    ipc_register_service("acc:u0", acc_open);
    ipc_register_service("acc:u1", acc_open);
    (void)acc_bool_true;
}
