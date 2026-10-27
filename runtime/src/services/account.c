/* acc:u0: one offline local user. */
#include "../ipc.h"
#include "host_info.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint8_t g_user_id[16] = {0x4e, 0x61, 0x70, 0x70, 0x6c, 0x65, 0x55, 0x73, 0x65, 0x72, 0, 0, 0, 0, 0, 1};

static void noop(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static void profile_get_base(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t base[0x38] = {0};
    memcpy(base, g_user_id, 16);
    uint64_t t = 1600000000;
    memcpy(base + 16, &t, 8);
    snprintf((char *)base + 24, 0x20, "%s", host_user_name());
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
    const char *roots[3] = {g_cfg.data_dir, g_cfg.data_dir, g_cfg.save_root};
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
static void manager_check(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static void manager_account_id(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = 0x4e61707070304e53ULL;
    IPC_OUT(c, v);
}
/* GetNintendoAccountUserResourceCacheForApplication: the local user is not linked to a Nintendo Account: id 0 and an all-zero cache (no result error: titles tolerate "nothing cached"). */
static void manager_na_cache(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = 0;
    IPC_OUT(c, v);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) {
        uint8_t z[0x68] = {0};
        ipc_write_buf(c, b, z, b->size < sizeof z ? b->size : sizeof z);
    }
}
static const IpcCmd manager_cmds[] = {{0, "CheckAvailability", manager_check, IPC_STUB},
                                      {1, "GetAccountId", manager_account_id, IPC_STUB},
                                      {130, "GetNintendoAccountUserResourceCacheForApplication", manager_na_cache, IPC_STUB}};

static void acc_user_count(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = 1;
    IPC_OUT(c, v);
}
static void acc_user_exists(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = !memcmp(ipc_in(c, 0, 16), g_user_id, 16);
    IPC_OUT(c, v);
}
static void acc_list_users(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) {
        uint8_t list[16 * 8] = {0};
        memcpy(list, g_user_id, 16);
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
    ipc_out_object(c, IPC_OBJECT("IProfile", profile_cmds, NULL, NULL));
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
static void acc_baas_manager(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IManagerForApplication", manager_cmds, NULL, NULL));
}
static const IpcCmd acc_cmds[] = {
    {0, "GetUserCount", acc_user_count, IPC_IMPL},
    {1, "GetUserExistence", acc_user_exists, IPC_IMPL},
    {2, "ListAllUsers", acc_list_users, IPC_IMPL},
    {3, "ListOpenUsers", acc_list_users, IPC_IMPL},
    {4, "GetLastOpenedUser", acc_last_opened, IPC_IMPL},
    {5, "GetProfile", acc_get_profile, IPC_IMPL},
    {50, "IsUserRegistrationRequestPermitted", acc_bool_false, IPC_STUB},
    {51, "TrySelectUserWithoutInteraction", acc_last_opened, IPC_IMPL},
    {100, "InitializeApplicationInfoV0", noop, IPC_STUB},
    {101, "GetBaasAccountManagerForApplication", acc_baas_manager, IPC_IMPL},
    {140, "InitializeApplicationInfo", noop, IPC_STUB},
    {141, "ListQualifiedUsers", acc_list_users, IPC_IMPL},
    {150, "IsUserAccountSwitchLocked", acc_bool_false, IPC_STUB},
    {160, "InitializeApplicationInfoV2", noop, IPC_STUB},
};
static IpcObject *acc_open(void) { return IPC_OBJECT("acc:u0", acc_cmds, NULL, NULL); }
void service_register_account(void) {
    ipc_register_service("acc:u0", acc_open);
    ipc_register_service("acc:u1", acc_open);
    (void)acc_bool_true;
}
