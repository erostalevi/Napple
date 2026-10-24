/* Small services with fixed offline answers: aoc:u (no add-on content yet), pctl (no parental controls). */
#include "../ipc.h"
#include <stdio.h>

static void noop(IpcObject *s, IpcCall *c) { (void)s; (void)c; }
static void ret_u32_0(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = 0; IPC_OUT(c, v); }
static void ret_u8_0(IpcObject *s, IpcCall *c) { (void)s; uint8_t v = 0; IPC_OUT(c, v); }
static void event(IpcObject *s, IpcCall *c) { (void)s; uint32_t r = 0; event_create(&r); ipc_out_copy_handle(c, r); }

/* aoc:u: CountAddOnContent / ListAddOnContent report no DLC; the base id of add-on content is the application id + 0x1000. */
static void aoc_base_id(IpcObject *s, IpcCall *c) { (void)s; uint64_t v = g_program_id + 0x1000; IPC_OUT(c, v); }
static const IpcCmd aoc_cmds[] = {
    {0, "CountAddOnContentByApplicationId", ret_u32_0, IPC_STUB}, {1, "ListAddOnContentByApplicationId", ret_u32_0, IPC_STUB},
    {2, "CountAddOnContent", ret_u32_0, IPC_STUB}, {3, "ListAddOnContent", ret_u32_0, IPC_STUB},
    {5, "GetAddOnContentBaseId", aoc_base_id, IPC_STUB}, {6, "PrepareAddOnContent", noop, IPC_STUB},
    {7, "GetAddOnContentListChangedEvent", event, IPC_STUB}, {8, "GetAddOnContentLostErrorCode", ret_u32_0, IPC_STUB},
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
