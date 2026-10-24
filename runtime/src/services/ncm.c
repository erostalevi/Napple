/* ncm: content manager. The host runs one unpacked title from a data directory, so every content-meta database and content storage is
 * empty: queries answer "none", lookups answer ContentMetaNotFound / ContentNotFound. Command ids are from the author's memory of the
 * public documentation and are unverified; unlisted commands fail loudly. */
#include "svc_common.h"
#include "hz_result.h"

static void out_s32_0(IpcObject *s, IpcCall *c) { (void)s; int32_t v = 0; IPC_OUT(c, v); }
static void out_two_s32_0(IpcObject *s, IpcCall *c) { (void)s; int32_t v[2] = {0, 0}; ipc_out(c, v, 8); }   /* {total, written} */
static void meta_not_found(IpcObject *s, IpcCall *c) { (void)s; c->result = HZ_R_NCM_CONTENT_META_NOT_FOUND; }
static void content_not_found(IpcObject *s, IpcCall *c) { (void)s; c->result = HZ_R_NCM_CONTENT_NOT_FOUND; }

static const IpcCmd meta_db_cmds[] = {
    {0, "Set", svc_noop, IPC_STUB}, {1, "Get", meta_not_found, IPC_STUB}, {2, "Remove", svc_noop, IPC_STUB}, {3, "GetContentIdByType", meta_not_found, IPC_STUB},
    {4, "ListContentInfo", out_s32_0, IPC_STUB}, {5, "List", out_two_s32_0, IPC_STUB}, {6, "GetLatestContentMetaKey", meta_not_found, IPC_STUB},
    {7, "ListApplication", out_two_s32_0, IPC_STUB}, {8, "Has", svc_ret_u8_0, IPC_STUB}, {9, "HasAll", svc_ret_u8_0, IPC_STUB},
    {10, "GetSize", meta_not_found, IPC_STUB}, {11, "GetRequiredSystemVersion", meta_not_found, IPC_STUB}, {12, "GetPatchId", meta_not_found, IPC_STUB},
    {13, "DisableForcibly", svc_noop, IPC_STUB}, {14, "LookupOrphanContent", svc_noop, IPC_STUB}, {15, "Commit", svc_noop, IPC_STUB},
    {16, "HasContent", svc_ret_u8_0, IPC_STUB}, {17, "ListContentMetaInfo", out_two_s32_0, IPC_STUB}, {18, "GetAttributes", meta_not_found, IPC_STUB},
};
static const IpcCmd storage_cmds[] = {
    {0, "GeneratePlaceHolderId", svc_noop, IPC_STUB}, {1, "CreatePlaceHolder", svc_noop, IPC_STUB}, {2, "DeletePlaceHolder", svc_noop, IPC_STUB},
    {3, "HasPlaceHolder", svc_ret_u8_0, IPC_STUB}, {5, "Register", svc_noop, IPC_STUB}, {6, "Delete", content_not_found, IPC_STUB},
    {7, "Has", svc_ret_u8_0, IPC_STUB}, {8, "GetPath", content_not_found, IPC_STUB}, {9, "GetPlaceHolderPath", content_not_found, IPC_STUB},
    {10, "CleanupAllPlaceHolder", svc_noop, IPC_STUB}, {11, "ListPlaceHolder", out_s32_0, IPC_STUB}, {12, "GetContentCount", out_s32_0, IPC_STUB},
    {13, "ListContentId", out_s32_0, IPC_STUB}, {14, "GetSizeFromContentId", content_not_found, IPC_STUB},
};
static void ncm_open_storage(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IContentStorage", storage_cmds, NULL, NULL)); }
static void ncm_open_meta_db(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IContentMetaDatabase", meta_db_cmds, NULL, NULL)); }
static const IpcCmd ncm_cmds[] = {
    {0, "CreateContentStorage", svc_noop, IPC_STUB}, {1, "CreateContentMetaDatabase", svc_noop, IPC_STUB}, {2, "VerifyContentStorage", svc_noop, IPC_STUB},
    {3, "VerifyContentMetaDatabase", svc_noop, IPC_STUB}, {4, "OpenContentStorage", ncm_open_storage, IPC_STUB}, {5, "OpenContentMetaDatabase", ncm_open_meta_db, IPC_STUB},
    {6, "CloseContentStorageForcibly", svc_noop, IPC_STUB}, {7, "CloseContentMetaDatabaseForcibly", svc_noop, IPC_STUB}, {8, "CleanupContentMetaDatabase", svc_noop, IPC_STUB},
    {9, "ActivateContentStorage", svc_noop, IPC_STUB}, {10, "InactivateContentStorage", svc_noop, IPC_STUB}, {11, "ActivateContentMetaDatabase", svc_noop, IPC_STUB},
    {12, "InactivateContentMetaDatabase", svc_noop, IPC_STUB}, {13, "InvalidateRightsIdCache", svc_noop, IPC_STUB},
};
static IpcObject *ncm_open(void) { return IPC_OBJECT("ncm", ncm_cmds, NULL, NULL); }
void service_register_ncm(void) { ipc_register_service("ncm", ncm_open); }
