/* Small services: aoc:u (add-on content from the manifest), pctl (no parental controls). */
#include "../ipc.h"
#include "../json.h"
#include "aoc_core.h"
#include "hz_result.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "sys_default.h"

static void noop(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static void ret_u32_0(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = 0;
    IPC_OUT(c, v);
}
static void ret_u8_0(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 0;
    IPC_OUT(c, v);
}
static void ret_u8_1(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 1;
    IPC_OUT(c, v);
}
static void event(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t r = 0;
    event_create(&r);
    ipc_out_copy_handle(c, r);
}

/* aoc:u: add-on content (DLC) found in the data directory's manifest (aoc_core.c). No DLC gives the old answers: count 0, empty list, and
 * PrepareAddOnContent of any index fails. Command layouts are the public ones as remembered, [unverified] against a title:
 *   0 Count(u64 app)->u32  1 List(u32 offset,u32 count,u64 app)->u32 + s32[] out buffer  2 Count(pid)  3 List(u32 offset,u32 count,pid)
 *   4/5 GetAddOnContentBaseId  6 PrepareAddOnContent(s32 index,pid): the content is then opened by fsp-srv OpenDataStorageByDataId(base id + index),
 *   served read-only from the DLC's romfs.bin (aoc_data_id_romfs, used by fs.c). */
#define R_AOC_NOT_FOUND HZ_RESULT(2, 1) /* [unverified] PrepareAddOnContent of an index that is not installed */
static AocSet g_aoc;
static int g_aoc_loaded;
static pthread_mutex_t g_aoc_mu = PTHREAD_MUTEX_INITIALIZER;
/* Reads the "dlc" array of manifest text; relative dirs resolve against data_dir. Entries with default_enabled false are dropped. */
unsigned aoc_load_manifest(AocSet *s, uint64_t app, const char *data_dir, const char *text, size_t len) {
    aoc_init(s, app);
    JsonNode *root = text ? json_parse(text, len) : NULL;
    const JsonNode *dlc = root ? json_get(root, "dlc") : NULL;
    if (dlc && dlc->type == J_ARR) {
        for (const JsonNode *d = dlc->child; d; d = d->next) {
            if (d->type != J_OBJ) continue;
            const JsonNode *en = json_get(d, "default_enabled");
            if (en && en->type == J_BOOL && !en->num) continue;
            const char *tid = json_str(d, "title_id", NULL), *dir = json_str(d, "dir", NULL), *rom = json_str(d, "romfs", "romfs.bin");
            if (!tid || !dir || !*tid) continue;
            char *end;
            uint64_t id = strtoull(tid, &end, 16);
            char path[AOC_PATH];
            if (*end) continue;
            if (dir[0] == '/') snprintf(path, sizeof path, "%s/%s", dir, rom);
            else snprintf(path, sizeof path, "%s/%s/%s", data_dir ? data_dir : ".", dir, rom);
            if (!aoc_add(s, id, (uint32_t)json_num(d, "version", 0), path))
                LOG("aoc", "DLC %s skipped (title id outside the add-on range of this application, duplicate, or too many)", tid);
        }
    }
    json_free(root);
    return s->n;
}
static void aoc_load_once(void) {
    pthread_mutex_lock(&g_aoc_mu);
    if (!g_aoc_loaded) {
        g_aoc_loaded = 1;
        char *text = NULL;
        size_t n = 0;
        if (g_cfg.data_dir) {
            char path[AOC_PATH];
            snprintf(path, sizeof path, "%s/manifest.json", g_cfg.data_dir);
            FILE *f = fopen(path, "rb");
            if (f) {
                text = malloc(1 << 20);
                n = text ? fread(text, 1, (1 << 20) - 1, f) : 0;
                fclose(f);
            }
        }
        aoc_load_manifest(&g_aoc, g_program_id, g_cfg.data_dir, text, n);
        free(text);
        LOG("aoc", "%u add-on content item(s) found", g_aoc.n);
        for (unsigned i = 0; i < g_aoc.n; i++)
            LOG("aoc", "  index %u: title %016llx version %u (%s)", g_aoc.e[i].index, (unsigned long long)g_aoc.e[i].title_id, g_aoc.e[i].version,
                g_aoc.e[i].romfs);
    }
    pthread_mutex_unlock(&g_aoc_mu);
}
static const AocSet *aoc_set(void) {
    aoc_load_once();
    return &g_aoc;
}
/* fs.c: the romfs image behind a DLC data id, or NULL. */
const char *aoc_data_id_romfs(uint64_t data_id) {
    const AocEntry *e = aoc_find_data_id(aoc_set(), data_id);
    return e ? e->romfs : NULL;
}
static void aoc_count_cmd(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = aoc_count(aoc_set());
    IPC_OUT(c, v);
}
static void aoc_count_app(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = IPC_IN(c, uint64_t, 0) == g_program_id ? aoc_count(aoc_set()) : 0;
    IPC_OUT(c, v);
}
static void aoc_list_common(IpcCall *c, uint32_t offset, uint32_t count, int match) {
    int32_t idx[AOC_MAX];
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (count > AOC_MAX) count = AOC_MAX;
    if (b && count > b->size / 4) count = (uint32_t)(b->size / 4);
    uint32_t n = match ? aoc_list(aoc_set(), offset, count, idx) : 0;
    if (b && n) ipc_write_buf(c, b, idx, (uint64_t)n * 4);
    IPC_OUT(c, n);
}
static void aoc_list_cmd(IpcObject *s, IpcCall *c) {
    (void)s;
    aoc_list_common(c, IPC_IN(c, uint32_t, 0), IPC_IN(c, uint32_t, 4), 1);
}
static void aoc_list_app(IpcObject *s, IpcCall *c) {
    (void)s;
    aoc_list_common(c, IPC_IN(c, uint32_t, 0), IPC_IN(c, uint32_t, 4), IPC_IN(c, uint64_t, 8) == g_program_id);
}
static void aoc_base_id_cmd(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = aoc_base_id(g_program_id);
    IPC_OUT(c, v);
}
static void aoc_base_id_app(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = aoc_base_id(IPC_IN(c, uint64_t, 0));
    IPC_OUT(c, v);
}
static void aoc_prepare(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t index = IPC_IN(c, int32_t, 0);
    const AocEntry *e = aoc_find_index(aoc_set(), index);
    if (!e) {
        LOG("aoc", "PrepareAddOnContent(%d): not installed", index);
        c->result = R_AOC_NOT_FOUND;
        return;
    }
    LOG("aoc", "PrepareAddOnContent(%d): title %016llx ready", index, (unsigned long long)e->title_id);
}
static void aoc_prepare_app(IpcObject *s, IpcCall *c) {
    if (IPC_IN(c, uint64_t, 8) != g_program_id) {
        c->result = R_AOC_NOT_FOUND;
        return;
    }
    aoc_prepare(s, c);
}
/* Rows 8..14 and 50: the installed add-on set is read once from the manifest and never changes while the process runs, so the list-changed
 * events never fire, nothing is ever "lost" (error code 0), and mounting or unmounting content has no state to track: the console answers
 * success to Notify(Un)Mount for content that is installed, and CheckAddOnContentMountStatus succeeds for it. These are the real answers for an
 * immutable DLC set (documented no-ops); the actual read access is the fsp-srv data-id open served from the DLC romfs. */
/* 302 UpdateCachedList: re-reads the add-on content list (the manifest) instead of the list cached at first use, so DLC added to the data directory while the
 * process runs shows up. 200 SetRequiredAddOnContentsOnContentsAvailabilityTransition(in: u64 ids): remembers which add-on contents the title needs once
 * contents become available (up to AOC_MAX ids; a later call replaces the list). */
static uint64_t g_aoc_required[AOC_MAX];
static unsigned g_aoc_required_n;
static void aoc_update_cached_list(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    pthread_mutex_lock(&g_aoc_mu);
    g_aoc_loaded = 0;
    pthread_mutex_unlock(&g_aoc_mu);
    aoc_load_once();
}
static void aoc_set_required(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint64_t *ids = b && b->size ? ipc_buf_ptr(b) : NULL;
    pthread_mutex_lock(&g_aoc_mu);
    g_aoc_required_n = ids ? (unsigned)(b->size / 8 < AOC_MAX ? b->size / 8 : AOC_MAX) : 0;
    for (unsigned i = 0; i < g_aoc_required_n; i++) g_aoc_required[i] = ids[i];
    pthread_mutex_unlock(&g_aoc_mu);
}
/* 11 NotifyMountAddOnContent / 12 NotifyUnmountAddOnContent / 13 IsAddOnContentMountedForDebug: the content itself is read through the fsp-srv data-id open, so
 * the service only keeps how many mounts the title announced (never below zero) and answers whether any is outstanding. */
static unsigned g_aoc_mounts;
static void aoc_notify_mount(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    pthread_mutex_lock(&g_aoc_mu);
    g_aoc_mounts++;
    pthread_mutex_unlock(&g_aoc_mu);
}
static void aoc_notify_unmount(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    pthread_mutex_lock(&g_aoc_mu);
    if (g_aoc_mounts) g_aoc_mounts--;
    pthread_mutex_unlock(&g_aoc_mu);
}
static void aoc_is_mounted(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&g_aoc_mu);
    uint8_t v = g_aoc_mounts != 0;
    pthread_mutex_unlock(&g_aoc_mu);
    IPC_OUT(c, v);
}
/* 100 CreateEcPurchasedEventManager / 101 CreatePermanentEcPurchasedEventManager -> IPurchaseEventManager (switchbrew 'NS services', layout facts only):
 *   0 SetDefaultDeliveryTarget(u64 pid placeholder + pid, type-5 buffer)  1 SetDeliveryTarget(u64 application id, type-5 buffer)
 *   2 GetPurchasedEventReadableHandle -> copy handle  3 PopPurchasedProductInfo -> PurchasedProductInfo  4 PopPurchasedProductInfoWithUid(Uid) -> the same
 * Purchases arrive from the eShop, which an offline console never reaches: the delivery targets are kept (per manager), the purchased event never fires and
 * popping finds nothing, with the "no purchased product info" result the SDK checks for (napple's reading of the result, unverified). */
#define R_AOC_NO_PURCHASED_PRODUCT HZ_RESULT(166, 400) /* [unverified] module 166 (NIM shop), description 400 */
#define AOC_TARGET_MAX 0x100u
typedef struct {
    uint64_t target_app;
    uint8_t target[AOC_TARGET_MAX];
    uint32_t target_size, has_default, has_target, permanent, pops;
} AocPurchaseMgr;
static void purchase_set_target(IpcObject *s, IpcCall *c) {
    AocPurchaseMgr *m = s->state;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    uint32_t n = p ? (uint32_t)(b->size < AOC_TARGET_MAX ? b->size : AOC_TARGET_MAX) : 0;
    if (n) memcpy(m->target, p, n);
    m->target_size = n;
    if (c->cmd == 0) m->has_default = 1;
    else {
        m->has_target = 1;
        m->target_app = c->in_size >= 8 ? IPC_IN(c, uint64_t, 0) : 0;
    }
}
static void purchase_pop(IpcObject *s, IpcCall *c) {
    AocPurchaseMgr *m = s->state;
    m->pops++;
    c->result = R_AOC_NO_PURCHASED_PRODUCT;
}
static const IpcCmd purchase_cmds[] = {
    {0, "SetDefaultDeliveryTarget", purchase_set_target, IPC_IMPL},
    {1, "SetDeliveryTarget", purchase_set_target, IPC_IMPL},
    {2, "GetPurchasedEventReadableHandle", event, IPC_IMPL},
    {3, "PopPurchasedProductInfo", purchase_pop, IPC_IMPL},
    {4, "PopPurchasedProductInfoWithUid", purchase_pop, IPC_IMPL},
};
static void purchase_destroy(IpcObject *o) { free(o->state); }
static void aoc_create_purchase_mgr(IpcObject *s, IpcCall *c) {
    (void)s;
    AocPurchaseMgr *m = calloc(1, sizeof *m);
    if (!m) {
        c->result = IPC_R_UNKNOWN_COMMAND;
        return;
    }
    m->permanent = c->cmd == 101;
    ipc_out_object(c, IPC_OBJECT("IPurchaseEventManager", purchase_cmds, m, purchase_destroy));
}
static const IpcCmd aoc_cmds[] = {
    {0, "CountAddOnContentByApplicationId", aoc_count_app, IPC_IMPL},
    {1, "ListAddOnContentByApplicationId", aoc_list_app, IPC_IMPL},
    {2, "CountAddOnContent", aoc_count_cmd, IPC_IMPL},
    {3, "ListAddOnContent", aoc_list_cmd, IPC_IMPL},
    {4, "GetAddOnContentBaseIdByApplicationId", aoc_base_id_app, IPC_IMPL},
    {5, "GetAddOnContentBaseId", aoc_base_id_cmd, IPC_IMPL},
    /* 6/7: the guest sequence is List(3), Prepare(7, index), BaseId(5), fsp OpenDataStorageByDataId(202), Notify(11) (switchbrew numbering, firmware 17+). */
    {6, "PrepareAddOnContentByApplicationId", aoc_prepare_app, IPC_IMPL},
    {7, "PrepareAddOnContent", aoc_prepare, IPC_IMPL},
    {8, "GetAddOnContentListChangedEvent", event, IPC_IMPL},
    {9, "GetAddOnContentLostErrorCode", ret_u32_0, IPC_IMPL},
    {10, "GetAddOnContentListChangedEventWithProcessId", event, IPC_IMPL},
    {11, "NotifyMountAddOnContent", aoc_notify_mount, IPC_IMPL},
    {12, "NotifyUnmountAddOnContent", aoc_notify_unmount, IPC_IMPL},
    {13, "IsAddOnContentMountedForDebug", aoc_is_mounted, IPC_IMPL},
        {50, "CheckAddOnContentMountStatus", noop, IPC_IMPL},
    {100, "CreateEcPurchasedEventManager", aoc_create_purchase_mgr, IPC_IMPL},
    {101, "CreatePermanentEcPurchasedEventManager", aoc_create_purchase_mgr, IPC_IMPL},
    {110, "CreateContentsServiceManager", sysd_err_generic, IPC_STUB},
    {200, "SetRequiredAddOnContentsOnContentsAvailabilityTransition", aoc_set_required, IPC_IMPL},
    {300, "SetupHostAddOnContent", sysd_ok, IPC_STUB},
    {301, "GetRegisteredAddOnContentPath", sysd_err_generic, IPC_STUB},
    {302, "UpdateCachedList", aoc_update_cached_list, IPC_IMPL},
};
static IpcObject *aoc_open(void) { return IPC_OBJECT("aoc:u", aoc_cmds, NULL, NULL); }

/* pctl: a console with parental controls switched off. Every "is X restricted" query answers no, every "confirm permission" call succeeds and
 * returns no unlock requirement, the safety level is 0 (none). Command ids are from the author's memory of the public documentation [unverified];
 * a command that is not in the table still stops the runtime with the usual unknown-command log, which is the signal to add it. */
static const IpcCmd pctl_service_cmds[] = {
    /* rows 1006 and up are the documented answers of a console with parental controls off (no restriction, nothing locked, no play timer): implemented, not stubs */
    {1, "Initialize", noop, IPC_IMPL},
    {1001, "CheckFreeCommunicationPermission", noop, IPC_IMPL},
    {1002, "ConfirmLaunchApplicationPermission", noop, IPC_IMPL},
    {1003, "ConfirmResumeApplicationPermission", noop, IPC_IMPL},
    {1004, "ConfirmSnsPostPermission", noop, IPC_IMPL},
    {1005, "ConfirmSystemSettingsPermission", noop, IPC_IMPL},
    {1031, "IsRestrictionEnabled", ret_u8_0, IPC_IMPL},
    {1032, "GetSafetyLevel", ret_u32_0, IPC_IMPL},
    {1018, "IsFreeCommunicationAvailable", noop, IPC_IMPL},
    {1035, "GetCurrentSettings", sysd_z12, IPC_IMPL},
    {1061, "ConfirmStereoVisionRestrictionConfigurable", noop, IPC_IMPL},
    {1062, "GetStereoVisionRestriction", ret_u8_0, IPC_IMPL},
    {1065, "IsStereoVisionPermitted", ret_u8_1, IPC_IMPL},
    {1451, "StartPlayTimer", noop, IPC_IMPL},
    {1006, "IsRestrictionTemporaryUnlocked", sysd_z1, IPC_IMPL},
    {1007, "RevertRestrictionTemporaryUnlocked", sysd_ok, IPC_IMPL},
    {1008, "EnterRestrictedSystemSettings", sysd_ok, IPC_IMPL},
    {1009, "LeaveRestrictedSystemSettings", sysd_ok, IPC_IMPL},
    {1010, "IsRestrictedSystemSettingsEntered", sysd_z1, IPC_IMPL},
    {1011, "RevertRestrictedSystemSettingsEntered", sysd_ok, IPC_IMPL},
    {1012, "GetRestrictedFeatures", sysd_z4, IPC_IMPL},
    {1013, "ConfirmStereoVisionPermission", sysd_ok, IPC_IMPL},
    {1014, "ConfirmPlayableApplicationVideoOld", sysd_ok, IPC_IMPL},
    {1015, "ConfirmPlayableApplicationVideo", sysd_ok, IPC_IMPL},
    {1016, "ConfirmShowNewsPermission", sysd_ok, IPC_IMPL},
    {1017, "EndFreeCommunication", sysd_ok, IPC_IMPL},
    {1033, "SetSafetyLevel", sysd_ok, IPC_IMPL},
    {1034, "GetSafetyLevelSettings", sysd_z12, IPC_IMPL},
    {1063, "SetStereoVisionRestriction", sysd_ok, IPC_IMPL},
    {1064, "ResetConfirmedStereoVisionPermission", sysd_ok, IPC_IMPL},
    {1452, "StopPlayTimer", sysd_ok, IPC_IMPL},
    {1453, "IsPlayTimerEnabled", sysd_z1, IPC_IMPL},
    {1454, "GetPlayTimerRemainingTime", sysd_z8, IPC_IMPL},
    {1455, "IsRestrictedByPlayTimer", sysd_z1, IPC_IMPL},
    {1457, "GetPlayTimerEventToRequestSuspension", sysd_event, IPC_IMPL},
    {1458, "IsPlayTimerAlarmDisabled", sysd_z1, IPC_IMPL},
    {1471, "NotifyWrongPinCodeInputManyTimes", sysd_ok, IPC_IMPL},
    {1472, "CancelNetworkRequest", sysd_ok, IPC_IMPL},
    {1473, "GetUnlinkedEvent", sysd_event, IPC_IMPL},
    {1474, "ClearUnlinkedEvent", sysd_ok, IPC_IMPL},
    {1601, "DisableAllFeatures", sysd_ok, IPC_IMPL},
    {1602, "PostEnableAllFeatures", sysd_ok, IPC_IMPL},
    {1603, "IsAllFeaturesDisabled", sysd_z1, IPC_IMPL},
};
static void pctl_create(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IParentalControlService", pctl_service_cmds, NULL, NULL));
}
static const IpcCmd pctl_cmds[] = {{0, "CreateService", pctl_create, IPC_IMPL}, {1, "CreateServiceWithoutInitialize", pctl_create, IPC_IMPL}};
static IpcObject *pctl_open(void) { return IPC_OBJECT("pctl", pctl_cmds, NULL, NULL); }
void service_register_misc(void) {
    ipc_register_service("aoc:u", aoc_open);
    ipc_register_service("pctl", pctl_open);
    ipc_register_service("pctl:s", pctl_open);
}
