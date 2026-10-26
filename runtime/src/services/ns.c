/* ns:am2, ns:ec (nn::ns): application manager getters. ns:am2 hands out the application-manager, version, content-management and
 * e-commerce interfaces; the host has no installed-application list, no eShop and no downloads, so list calls return nothing and
 * network-bound calls fail with the offline result. Language conversion/selection and free-space queries are real. Command ids are
 * from the author's memory of the public documentation and are unverified. */
#include "svc_common.h"
#include "ns_core.h"
#include "host_info.h"
#include "hz_result.h"

static void ret_s32_0(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = 0;
    IPC_OUT(c, v);
}
static void offline_error(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
}
static void app_not_found(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = HZ_R_NS_APPLICATION_NOT_FOUND;
}

/* IApplicationManagerInterface */
static void am_desired_language(IpcObject *s, IpcCall *c) { /* 401(u32 supported language flags) -> u8 ApplicationLanguage */
    (void)s;
    int l = ns_select_language(IPC_IN(c, uint32_t, 0), host_desired_language());
    if (l < 0) {
        c->result = HZ_RESULT(HZ_MOD_NS, 30);
        return;
    } /* [unverified] "no supported language" */
    uint8_t v = (uint8_t)l;
    IPC_OUT(c, v);
}
static void am_language_to_code(IpcObject *s, IpcCall *c) { /* 402(u8) -> u64 */
    (void)s;
    uint64_t v = ns_language_code(IPC_IN(c, uint8_t, 0));
    if (!v) {
        c->result = HZ_RESULT(HZ_MOD_NS, 30);
        return;
    }
    IPC_OUT(c, v);
}
static void am_code_to_language(IpcObject *s, IpcCall *c) { /* 403(u64) -> u8 */
    (void)s;
    int l = ns_language_from_code(IPC_IN(c, uint64_t, 0));
    if (l < 0) {
        c->result = HZ_RESULT(HZ_MOD_NS, 30);
        return;
    }
    uint8_t v = (uint8_t)l;
    IPC_OUT(c, v);
}
static const IpcCmd am_cmds[] = {
    {0, "ListApplicationRecord", ret_s32_0, IPC_STUB},
    {1, "GenerateApplicationRecordCount", ret_s32_0, IPC_STUB},
    {2, "GetApplicationRecordUpdateSystemEvent", svc_ret_event, IPC_STUB},
    {400, "GetApplicationControlData", app_not_found, IPC_STUB},
    {401, "GetApplicationDesiredLanguage", am_desired_language, IPC_IMPL},
    {402, "ConvertApplicationLanguageToLanguageCode", am_language_to_code, IPC_IMPL},
    {403, "ConvertLanguageCodeToApplicationLanguage", am_code_to_language, IPC_IMPL},
};

/* IContentManagementInterface: 47/48 report the volume that holds the save directory for the user/SD storages. */
static uint64_t storage_bytes(IpcCall *c, int total) {
    uint8_t storage = IPC_IN(c, uint8_t, 0);
    if (storage != 4 && storage != 5) return 0; /* BuiltInUser / SdCard */
    return total ? ns_total_space(host_save_dir()) : ns_free_space(host_save_dir());
}
static void cm_total_space(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = storage_bytes(c, 1);
    IPC_OUT(c, v);
}
static void cm_free_space(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = storage_bytes(c, 0);
    IPC_OUT(c, v);
}
static void cm_occupied(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_zero(c, 0x80);
}
static const IpcCmd cm_cmds[] = {
    {11, "CalculateApplicationOccupiedSize", cm_occupied, IPC_STUB},
    {43, "CheckSdCardMountStatus", svc_noop, IPC_IMPL},
    {47, "GetTotalSpaceSize", cm_total_space, IPC_IMPL},
    {48, "GetFreeSpaceSize", cm_free_space, IPC_IMPL},
};

/* IApplicationVersionInterface: the host runs the one version it was built from, so no launch version is ever required and no update list exists; auto update
 * and the eShop calls need the network and answer with the offline result. All complete answers (class c). Still stubs below: ListApplicationRecord and
 * friends (the host has no installed-application database: needs the napple-build manifest to list the running title) and GetApplicationControlData
 * / content-path queries (need control.nacp and the content layout from napple-build; follow-up for whoever owns the build layout). */
static const IpcCmd ver_cmds[] = {
    {0, "GetLaunchRequiredVersion", svc_ret_u32_0, IPC_IMPL},
    {1, "UpgradeLaunchRequiredVersion", svc_noop, IPC_IMPL},
    {35, "UpdateVersionList", svc_noop, IPC_IMPL},
    {36, "PushLaunchVersion", svc_noop, IPC_IMPL},
    {37, "ListRequiredVersion", ret_s32_0, IPC_IMPL},
    {1000, "PerformAutoUpdate", offline_error, IPC_IMPL},
};
/* IECommerceInterface: every call needs the eShop */
static const IpcCmd ec_cmds[] = {
    {0, "RequestLinkDevice", offline_error, IPC_IMPL},
    {1, "RequestCleanupAllPreInstalledApplications", offline_error, IPC_IMPL},
    {2, "RequestSyncRights", offline_error, IPC_IMPL},
    {3, "RequestUnlinkDevice", offline_error, IPC_IMPL},
};
/* IDownloadTaskInterface / IDocumentInterface: nothing is downloading */
static const IpcCmd dl_cmds[] = {{701, "ClearTaskStatusList", svc_noop, IPC_IMPL}, {702, "RequestDownloadTaskList", ret_s32_0, IPC_IMPL}};
static const IpcCmd doc_cmds[] = {{21, "GetApplicationContentPath", app_not_found, IPC_STUB}, {23, "ResolveApplicationContentPath", app_not_found, IPC_STUB}};

#define SUB(fn, name, table)                                                                                                                                   \
    static void fn(IpcObject *s, IpcCall *c) {                                                                                                                 \
        (void)s;                                                                                                                                               \
        ipc_out_object(c, IPC_OBJECT(name, table, NULL, NULL));                                                                                                \
    }
SUB(get_am, "IApplicationManagerInterface", am_cmds)
SUB(get_cm, "IContentManagementInterface", cm_cmds)
SUB(get_ver, "IApplicationVersionInterface", ver_cmds)
SUB(get_ec, "IECommerceInterface", ec_cmds)
SUB(get_dl, "IDownloadTaskInterface", dl_cmds)
SUB(get_doc, "IDocumentInterface", doc_cmds)
static const IpcCmd getter_cmds[] = {
    {7992, "GetECommerceInterface", get_ec, IPC_IMPL},          {7993, "GetApplicationVersionInterface", get_ver, IPC_IMPL},
    {7996, "GetApplicationManagerInterface", get_am, IPC_IMPL}, {7997, "GetDownloadTaskInterface", get_dl, IPC_IMPL},
    {7998, "GetContentManagementInterface", get_cm, IPC_IMPL},  {7999, "GetDocumentInterface", get_doc, IPC_IMPL},
};
static IpcObject *am2_open(void) { return IPC_OBJECT("ns:am2", getter_cmds, NULL, NULL); }
static IpcObject *ec_open(void) { return IPC_OBJECT("ns:ec", getter_cmds, NULL, NULL); }

/* ns:vm (IVulnerabilityManagerInterface): the emulated system is always up to date, so there is nothing to patch. */
static const IpcCmd vm_cmds[] = {{1200, "NeedsUpdateVulnerability", svc_ret_u8_0, IPC_IMPL}, {1201, "UpdateSafeSystemVersionForDebug", svc_noop, IPC_IMPL}};
static IpcObject *vm_open(void) { return IPC_OBJECT("ns:vm", vm_cmds, NULL, NULL); }
void service_register_ns(void) {
    ipc_register_service("ns:am2", am2_open);
    ipc_register_service("ns:ec", ec_open);
}
void service_register_ns_vm(void) { ipc_register_service("ns:vm", vm_open); }
