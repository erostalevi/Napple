/* Small services: aoc:u (add-on content from the manifest), pctl (no parental controls). */
#include "../ipc.h"
#include "../json.h"
#include "aoc_core.h"
#include "hz_result.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static void noop(IpcObject *s, IpcCall *c) { (void)s; (void)c; }
static void ret_u32_0(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = 0; IPC_OUT(c, v); }
static void ret_u8_0(IpcObject *s, IpcCall *c) { (void)s; uint8_t v = 0; IPC_OUT(c, v); }
static void event(IpcObject *s, IpcCall *c) { (void)s; uint32_t r = 0; event_create(&r); ipc_out_copy_handle(c, r); }

/* aoc:u: add-on content (DLC) found in the data directory's manifest (aoc_core.c). No DLC gives the old answers: count 0, empty list, and
 * PrepareAddOnContent of any index fails. Command layouts are the public ones as remembered, [unverified] against a title:
 *   0 Count(u64 app)->u32  1 List(u32 offset,u32 count,u64 app)->u32 + s32[] out buffer  2 Count(pid)  3 List(u32 offset,u32 count,pid)
 *   4/5 GetAddOnContentBaseId  6 PrepareAddOnContent(s32 index,pid): the content is then opened by fsp-srv OpenDataStorageByDataId(base id + index),
 *   served read-only from the DLC's romfs.bin (aoc_data_id_romfs, used by fs.c). */
#define R_AOC_NOT_FOUND HZ_RESULT(2, 1)   /* [unverified] PrepareAddOnContent of an index that is not installed */
static AocSet g_aoc; static int g_aoc_loaded; static pthread_mutex_t g_aoc_mu = PTHREAD_MUTEX_INITIALIZER;
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
            char *end; uint64_t id = strtoull(tid, &end, 16); char path[AOC_PATH];
            if (*end) continue;
            if (dir[0] == '/') snprintf(path, sizeof path, "%s/%s", dir, rom); else snprintf(path, sizeof path, "%s/%s/%s", data_dir ? data_dir : ".", dir, rom);
            if (!aoc_add(s, id, (uint32_t)json_num(d, "version", 0), path)) LOG("aoc", "DLC %s skipped (title id outside the add-on range of this application, duplicate, or too many)", tid);
        }
    }
    json_free(root);
    return s->n;
}
static void aoc_load_once(void) {
    pthread_mutex_lock(&g_aoc_mu);
    if (!g_aoc_loaded) {
        g_aoc_loaded = 1; char *text = NULL; size_t n = 0;
        if (g_cfg.data_dir) {
            char path[AOC_PATH]; snprintf(path, sizeof path, "%s/manifest.json", g_cfg.data_dir);
            FILE *f = fopen(path, "rb");
            if (f) { text = malloc(1 << 20); n = text ? fread(text, 1, (1 << 20) - 1, f) : 0; fclose(f); }
        }
        aoc_load_manifest(&g_aoc, g_program_id, g_cfg.data_dir, text, n); free(text);
        LOG("aoc", "%u add-on content item(s) found", g_aoc.n);
        for (unsigned i = 0; i < g_aoc.n; i++) LOG("aoc", "  index %u: title %016llx version %u (%s)", g_aoc.e[i].index, (unsigned long long)g_aoc.e[i].title_id, g_aoc.e[i].version, g_aoc.e[i].romfs);
    }
    pthread_mutex_unlock(&g_aoc_mu);
}
static const AocSet *aoc_set(void) { aoc_load_once(); return &g_aoc; }
/* fs.c: the romfs image behind a DLC data id, or NULL. */
const char *aoc_data_id_romfs(uint64_t data_id) { const AocEntry *e = aoc_find_data_id(aoc_set(), data_id); return e ? e->romfs : NULL; }
static void aoc_count_cmd(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = aoc_count(aoc_set()); IPC_OUT(c, v); }
static void aoc_count_app(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = IPC_IN(c, uint64_t, 0) == g_program_id ? aoc_count(aoc_set()) : 0; IPC_OUT(c, v); }
static void aoc_list_common(IpcCall *c, uint32_t offset, uint32_t count, int match) {
    int32_t idx[AOC_MAX]; const IpcBuf *b = ipc_out_buf(c, 0);
    if (count > AOC_MAX) count = AOC_MAX;
    if (b && count > b->size / 4) count = (uint32_t)(b->size / 4);
    uint32_t n = match ? aoc_list(aoc_set(), offset, count, idx) : 0;
    if (b && n) ipc_write_buf(c, b, idx, (uint64_t)n * 4);
    IPC_OUT(c, n);
}
static void aoc_list_cmd(IpcObject *s, IpcCall *c) { (void)s; aoc_list_common(c, IPC_IN(c, uint32_t, 0), IPC_IN(c, uint32_t, 4), 1); }
static void aoc_list_app(IpcObject *s, IpcCall *c) { (void)s; aoc_list_common(c, IPC_IN(c, uint32_t, 0), IPC_IN(c, uint32_t, 4), IPC_IN(c, uint64_t, 8) == g_program_id); }
static void aoc_base_id_cmd(IpcObject *s, IpcCall *c) { (void)s; uint64_t v = aoc_base_id(g_program_id); IPC_OUT(c, v); }
static void aoc_base_id_app(IpcObject *s, IpcCall *c) { (void)s; uint64_t v = aoc_base_id(IPC_IN(c, uint64_t, 0)); IPC_OUT(c, v); }
static void aoc_prepare(IpcObject *s, IpcCall *c) {
    (void)s; int32_t index = IPC_IN(c, int32_t, 0); const AocEntry *e = aoc_find_index(aoc_set(), index);
    if (!e) { LOG("aoc", "PrepareAddOnContent(%d): not installed", index); c->result = R_AOC_NOT_FOUND; return; }
    LOG("aoc", "PrepareAddOnContent(%d): title %016llx ready", index, (unsigned long long)e->title_id);
}
static void aoc_prepare_app(IpcObject *s, IpcCall *c) { if (IPC_IN(c, uint64_t, 8) != g_program_id) { c->result = R_AOC_NOT_FOUND; return; } aoc_prepare(s, c); }
static const IpcCmd aoc_cmds[] = {
    {0, "CountAddOnContentByApplicationId", aoc_count_app, IPC_IMPL}, {1, "ListAddOnContentByApplicationId", aoc_list_app, IPC_IMPL},
    {2, "CountAddOnContent", aoc_count_cmd, IPC_IMPL}, {3, "ListAddOnContent", aoc_list_cmd, IPC_IMPL},
    {4, "GetAddOnContentBaseIdByApplicationId", aoc_base_id_app, IPC_IMPL},
    {5, "GetAddOnContentBaseId", aoc_base_id_cmd, IPC_IMPL},
    /* 6/7: the guest sequence is List(3), Prepare(7, index), BaseId(5), fsp OpenDataStorageByDataId(202), Notify(11) (switchbrew numbering, firmware 17+). */
    {6, "PrepareAddOnContentByApplicationId", aoc_prepare_app, IPC_IMPL}, {7, "PrepareAddOnContent", aoc_prepare, IPC_IMPL},
    {8, "GetAddOnContentListChangedEvent", event, IPC_STUB}, {9, "GetAddOnContentLostErrorCode", ret_u32_0, IPC_STUB},
    {10, "GetAddOnContentListChangedEventWithProcessId", event, IPC_STUB},
    {11, "NotifyMountAddOnContent", noop, IPC_STUB}, {12, "NotifyUnmountAddOnContent", noop, IPC_STUB},
    {13, "NotifyMountAddOnContentWithProcessId", noop, IPC_STUB}, {14, "NotifyUnmountAddOnContentWithProcessId", noop, IPC_STUB},
    {50, "CheckAddOnContentMountStatus", noop, IPC_STUB},
};
static IpcObject *aoc_open(void) { return IPC_OBJECT("aoc:u", aoc_cmds, NULL, NULL); }

/* pctl: an unrestricted device. IParentalControlService commands that ask "is X restricted" answer no. */
static const IpcCmd pctl_service_cmds[] = {
    {1, "Initialize", noop, IPC_STUB}, {1001, "CheckFreeCommunicationPermission", noop, IPC_STUB}, {1031, "IsRestrictionEnabled", ret_u8_0, IPC_STUB},
    {1032, "GetSafetyLevel", ret_u32_0, IPC_STUB}, {1035, "IsFreeCommunicationDefaultValue", ret_u8_0, IPC_STUB}, {1018, "IsRestrictionTemporaryUnlocked", ret_u8_0, IPC_STUB},
    {1061, "ConfirmStereoVisionPermission", noop, IPC_STUB}, {1062, "EndFreeCommunication", noop, IPC_STUB},
    {1451, "IsStereoVisionPermitted", ret_u8_0, IPC_STUB},
};
static void pctl_create(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IParentalControlService", pctl_service_cmds, NULL, NULL)); }
static const IpcCmd pctl_cmds[] = {{0, "CreateService", pctl_create, IPC_IMPL}, {1, "CreateServiceWithoutInitialize", pctl_create, IPC_IMPL}};
static IpcObject *pctl_open(void) { return IPC_OBJECT("pctl", pctl_cmds, NULL, NULL); }
void service_register_misc(void) { ipc_register_service("aoc:u", aoc_open); ipc_register_service("pctl", pctl_open); ipc_register_service("pctl:s", pctl_open); }
