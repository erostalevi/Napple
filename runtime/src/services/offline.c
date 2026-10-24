/* Services that exist on a console but have no meaning for an offline, single-user local run. Each answers a documented safe default
 * (IPC_STUB in the tables, listed in docs/runtime-services.md): the point is that the SDK's initialisation succeeds and the game falls
 * back to its offline paths. Unknown commands still fail loudly through the generic policy. */
#include "../ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void noop(IpcObject *s, IpcCall *c) { (void)s; (void)c; }
static void ret_u32_0(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = 0; IPC_OUT(c, v); }
static void ret_u32_1(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = 1; IPC_OUT(c, v); }
static void ret_u8_0(IpcObject *s, IpcCall *c) { (void)s; uint8_t v = 0; IPC_OUT(c, v); }
static void ret_u64_0(IpcObject *s, IpcCall *c) { (void)s; uint64_t v = 0; IPC_OUT(c, v); }
static void ret_event(IpcObject *s, IpcCall *c) { (void)s; uint32_t r = 0; event_create(&r); ipc_out_copy_handle(c, r); }
static void ret_event_signaled(IpcObject *s, IpcCall *c) { (void)s; uint32_t r = 0, w = event_create(&r); event_signal_handle(w); ipc_out_copy_handle(c, r); }
#define NOT_AVAILABLE(mod, desc) ((mod) | ((desc) << 9))

/* mm:u (nn::mmnv): multimedia clock requests. Modules are hardware clocks (VIC, NVDEC, ...): accepted, nothing to clock. */
static const IpcCmd mm_cmds[] = {
    {0, "InitializeOld", noop, IPC_STUB}, {1, "FinalizeOld", noop, IPC_STUB}, {2, "SetAndWaitOld", noop, IPC_STUB}, {3, "GetOld", ret_u32_0, IPC_STUB},
    {4, "Initialize", ret_u32_1, IPC_STUB}, {5, "Finalize", noop, IPC_STUB}, {6, "SetAndWait", noop, IPC_STUB}, {7, "Get", ret_u32_0, IPC_STUB},
};
static IpcObject *mm_open(void) { return IPC_OBJECT("mm:u", mm_cmds, NULL, NULL); }

/* friend:u (nn::friends): no online friends. The notification service and the friend service accept the calls games make at start-up. */
static const IpcCmd friend_notification_cmds[] = {
    {0, "GetEvent", ret_event, IPC_STUB}, {1, "Clear", noop, IPC_STUB}, {2, "Pop", ret_u32_0, IPC_STUB},
};
static const IpcCmd friend_service_cmds[] = {
    {0, "GetCompletionEvent", ret_event, IPC_STUB}, {1, "Cancel", noop, IPC_STUB}, {10100, "GetFriendListIds", ret_u32_0, IPC_STUB},
    {10101, "GetFriendList", ret_u32_0, IPC_STUB}, {10400, "GetBlockedUserListIds", ret_u32_0, IPC_STUB}, {20100, "GetFriendCount", ret_u32_0, IPC_STUB},
    {20101, "GetNewlyFriendCount", ret_u32_0, IPC_STUB}, {20200, "GetReceivedFriendRequestCount", ret_u32_0, IPC_STUB}, {10610, "UpdateUserPresence", noop, IPC_STUB},
    {10600, "DeclareOpenOnlinePlaySession", noop, IPC_STUB}, {10601, "DeclareCloseOnlinePlaySession", noop, IPC_STUB},
};
static void friend_create_service(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IFriendService", friend_service_cmds, NULL, NULL)); }
static void friend_create_notification(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("INotificationService", friend_notification_cmds, NULL, NULL)); }
static const IpcCmd friend_cmds[] = {
    {0, "CreateFriendService", friend_create_service, IPC_STUB}, {1, "CreateNotificationService", friend_create_notification, IPC_STUB},
    {2, "CreateDaemonSuspendSessionService", friend_create_service, IPC_STUB},
};
static IpcObject *friend_open(void) { return IPC_OBJECT("friend:u", friend_cmds, NULL, NULL); }

/* nifm:u (nn::nifm): there is no network. Requests stay "free" (never accepted), the connection status is "disconnected", no IP address. Games that check
 * connectivity see offline and take their offline path. */
static void ret_u32_1_ok(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = 1; IPC_OUT(c, v); }
static void ret_two_events(IpcObject *s, IpcCall *c) { (void)s; uint32_t r1 = 0, r2 = 0; event_create(&r1); event_create(&r2); ipc_out_copy_handle(c, r1); ipc_out_copy_handle(c, r2); }
static void ret_conn_status(IpcObject *s, IpcCall *c) { (void)s; uint8_t v[16] = {0}; ipc_out(c, v, 4); }   /* {u8 type, u8 pad, u8 wifi, u8 state}: disconnected */
static const IpcCmd nifm_request_cmds[] = {
    {0, "GetRequestState", ret_u32_1_ok, IPC_STUB}, {1, "GetResult", noop, IPC_STUB}, {2, "GetSystemEventReadableHandles", ret_two_events, IPC_STUB},
    {3, "Cancel", noop, IPC_STUB}, {4, "Submit", noop, IPC_STUB}, {5, "SetRequirement", noop, IPC_STUB}, {6, "SetRequirementPreset", noop, IPC_STUB},
    {8, "SetPriority", noop, IPC_STUB}, {9, "SetNetworkProfileId", noop, IPC_STUB}, {10, "SetRejectable", noop, IPC_STUB}, {11, "SetConnectionConfirmationOption", noop, IPC_STUB},
    {12, "SetPersistent", noop, IPC_STUB}, {13, "SetInstant", noop, IPC_STUB}, {14, "SetSustainable", noop, IPC_STUB}, {15, "SetRawPriority", noop, IPC_STUB},
};
static void nifm_create_request(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IRequest", nifm_request_cmds, NULL, NULL)); }
static void ret_u128_0(IpcObject *s, IpcCall *c) { (void)s; uint8_t v[16] = {0}; ipc_out(c, v, 16); }
static const IpcCmd nifm_general_cmds[] = {
    {1, "GetClientId", ret_u128_0, IPC_STUB}, {4, "CreateRequest", nifm_create_request, IPC_STUB}, {12, "GetCurrentIpAddress", ret_u32_0, IPC_STUB},
    {15, "GetInternetConnectionStatus", ret_conn_status, IPC_STUB}, {16, "IsWirelessCommunicationEnabled", ret_u8_0, IPC_STUB},
    {17, "GetInternetConnectionStatusEx", ret_conn_status, IPC_STUB}, {21, "IsAnyInternetRequestAccepted", ret_u8_0, IPC_STUB}, {22, "IsAnyForegroundRequestAccepted", ret_u8_0, IPC_STUB},
    {26, "SetExclusiveClient", noop, IPC_STUB}, {27, "GetDefaultIpSetting", noop, IPC_STUB},
};
static void nifm_create_general(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IGeneralService", nifm_general_cmds, NULL, NULL)); }
static const IpcCmd nifm_cmds[] = {{4, "CreateGeneralServiceOld", nifm_create_general, IPC_STUB}, {5, "CreateGeneralService", nifm_create_general, IPC_STUB}};
static IpcObject *nifm_open(void) { return IPC_OBJECT("nifm:u", nifm_cmds, NULL, NULL); }

/* bsd:u (nn::socket): the BSD socket service. There is no network: RegisterClient/StartMonitoring/Close succeed, every call that would touch the network
 * answers {ret -1, errno ENETDOWN (50)}, so a game's socket layer starts and its connection attempts fail like a machine without a network. */
static void bsd_ok(IpcObject *s, IpcCall *c) { (void)s; uint32_t v[2] = {0, 0}; ipc_out(c, v, 8); }
static void bsd_register_client(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = 0; IPC_OUT(c, v); }
static void bsd_netdown(IpcObject *s, IpcCall *c) { (void)s; uint32_t v[2] = {0xffffffffu, 50}; ipc_out(c, v, 8); }
static const IpcCmd bsd_cmds[] = {
    {0, "RegisterClient", bsd_register_client, IPC_STUB}, {1, "StartMonitoring", noop, IPC_STUB}, {2, "Socket", bsd_netdown, IPC_STUB}, {3, "SocketExempt", bsd_netdown, IPC_STUB},
    {4, "Open", bsd_netdown, IPC_STUB}, {5, "Select", bsd_netdown, IPC_STUB}, {6, "Poll", bsd_netdown, IPC_STUB}, {7, "Sysctl", bsd_netdown, IPC_STUB},
    {8, "Recv", bsd_netdown, IPC_STUB}, {9, "RecvFrom", bsd_netdown, IPC_STUB}, {10, "Send", bsd_netdown, IPC_STUB}, {11, "SendTo", bsd_netdown, IPC_STUB},
    {12, "Accept", bsd_netdown, IPC_STUB}, {13, "Bind", bsd_netdown, IPC_STUB}, {14, "Connect", bsd_netdown, IPC_STUB}, {15, "GetPeerName", bsd_netdown, IPC_STUB},
    {16, "GetSockName", bsd_netdown, IPC_STUB}, {17, "GetSockOpt", bsd_netdown, IPC_STUB}, {18, "Listen", bsd_netdown, IPC_STUB}, {19, "Ioctl", bsd_netdown, IPC_STUB},
    {20, "Fcntl", bsd_netdown, IPC_STUB}, {21, "SetSockOpt", bsd_netdown, IPC_STUB}, {22, "Shutdown", bsd_netdown, IPC_STUB}, {23, "ShutdownAllSockets", bsd_ok, IPC_STUB},
    {24, "Write", bsd_netdown, IPC_STUB}, {25, "Read", bsd_netdown, IPC_STUB}, {26, "Close", bsd_ok, IPC_STUB}, {27, "DuplicateSocket", bsd_netdown, IPC_STUB},
    {29, "GetResourceStatistics", bsd_netdown, IPC_STUB}, {30, "RecvMMsg", bsd_netdown, IPC_STUB}, {31, "SendMMsg", bsd_netdown, IPC_STUB},
};
static IpcObject *bsd_open(void) { return IPC_OBJECT("bsd:u", bsd_cmds, NULL, NULL); }

/* nsd:u (nn::nsd): name service settings. Environment "lp1", names resolve to themselves (nothing is ever contacted). */
static void nsd_env_name(IpcObject *s, IpcCall *c) { (void)s; const IpcBuf *b = ipc_out_buf(c, 0); if (b) { char name[8] = "lp1"; ipc_write_buf(c, b, name, b->size < sizeof name ? b->size : sizeof name); } }
static void nsd_resolve(IpcObject *s, IpcCall *c) {
    (void)s; const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0);
    if (in && out) { const void *p = ipc_buf_ptr(in); if (p) ipc_write_buf(c, out, p, in->size < out->size ? in->size : out->size); }
    uint32_t r = 0; IPC_OUT(c, r);
}
static void nsd_device_id(IpcObject *s, IpcCall *c) { (void)s; uint8_t v[16] = {0}; ipc_out(c, v, 16); }
static const IpcCmd nsd_cmds[] = {
    {10, "GetSettingName", nsd_env_name, IPC_STUB}, {11, "GetEnvironmentIdentifier", nsd_env_name, IPC_STUB}, {12, "GetDeviceId", nsd_device_id, IPC_STUB},
    {13, "DeleteSettings", noop, IPC_STUB}, {14, "ImportSettings", noop, IPC_STUB}, {20, "Resolve", nsd_resolve, IPC_STUB}, {21, "ResolveEx", nsd_resolve, IPC_STUB},
    {30, "GetNasServiceSetting", noop, IPC_STUB}, {31, "GetNasServiceSettingEx", noop, IPC_STUB}, {40, "GetNasRequestFqdn", nsd_resolve, IPC_STUB}, {41, "GetNasRequestFqdnEx", nsd_resolve, IPC_STUB},
    {42, "GetNasApiFqdn", nsd_resolve, IPC_STUB}, {43, "GetNasApiFqdnEx", nsd_resolve, IPC_STUB}, {50, "GetCurrentSetting", noop, IPC_STUB},
};
static IpcObject *nsd_open(void) { return IPC_OBJECT("nsd:u", nsd_cmds, NULL, NULL); }

/* csrng: cryptographically secure random bytes from the host. */
static void csrng_gen(IpcObject *s, IpcCall *c) {
    (void)s; const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || !b->size) return;
    uint64_t n = b->size > 0x10000 ? 0x10000 : b->size; uint8_t *tmp = malloc((size_t)n);
    if (!tmp) return;
    arc4random_buf(tmp, (size_t)n); ipc_write_buf(c, b, tmp, n); free(tmp);
}
static const IpcCmd csrng_cmds[] = {{0, "GenerateRandomBytes", csrng_gen, IPC_IMPL}};
static IpcObject *csrng_open(void) { return IPC_OBJECT("csrng", csrng_cmds, NULL, NULL); }

/* nfp:user / nfc:user (amiibo, NFC): initialised, no devices ever present. */
static void ret_i32_0(IpcObject *s, IpcCall *c) { (void)s; int32_t v = 0; IPC_OUT(c, v); }
static const IpcCmd nfp_user_cmds[] = {
    {0, "Initialize", noop, IPC_STUB}, {1, "Finalize", noop, IPC_STUB}, {2, "ListDevices", ret_i32_0, IPC_STUB}, {3, "StartDetection", noop, IPC_STUB},
    {4, "StopDetection", noop, IPC_STUB}, {5, "Mount", noop, IPC_STUB}, {6, "Unmount", noop, IPC_STUB}, {14, "AttachActivateEvent", ret_event, IPC_STUB},
    {15, "AttachDeactivateEvent", ret_event, IPC_STUB}, {16, "GetState", ret_u32_1_ok, IPC_STUB}, {17, "GetDeviceState", ret_u32_0, IPC_STUB},
    {19, "AttachAvailabilityChangeEvent", ret_event, IPC_STUB}, {23, "AttachAvailabilityChangeEvent", ret_event, IPC_STUB},
};
static void nfp_create_user(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("INfpUser", nfp_user_cmds, NULL, NULL)); }
static const IpcCmd nfp_cmds[] = {{0, "CreateUserInterface", nfp_create_user, IPC_STUB}};
static IpcObject *nfp_open(void) { return IPC_OBJECT("nfp:user", nfp_cmds, NULL, NULL); }

/* prepo:u (play reports): reports are accepted and dropped. */
static void ret_u64_1(IpcObject *s, IpcCall *c) { (void)s; uint64_t v = 1; IPC_OUT(c, v); }
static const IpcCmd prepo_cmds[] = {
    {10100, "SaveReport", noop, IPC_STUB}, {10101, "SaveReportWithUser", noop, IPC_STUB}, {10102, "SaveReportOld", noop, IPC_STUB}, {10103, "SaveReportWithUserOld", noop, IPC_STUB},
    {10200, "RequestImmediateTransmission", noop, IPC_STUB}, {10300, "GetTransmissionStatus", ret_u32_0, IPC_STUB}, {10400, "GetSystemSessionId", ret_u64_1, IPC_STUB},
    {20100, "SaveSystemReport", noop, IPC_STUB}, {20101, "SaveSystemReportWithUser", noop, IPC_STUB}, {40100, "IsUserAgreementCheckEnabled", ret_u8_0, IPC_STUB},
    {40101, "SetUserAgreementCheckEnabled", noop, IPC_STUB}, {90100, "ClearStorage", noop, IPC_STUB}, {90200, "GetStorageUsage", ret_u64_0, IPC_STUB},
};
static IpcObject *prepo_open(void) { return IPC_OBJECT("prepo:u", prepo_cmds, NULL, NULL); }

/* ssl (nn::ssl): contexts and connections can be created and configured; no handshake can complete because there is no network (bsd:u fails first). */
static const IpcCmd ssl_connection_cmds[] = {
    {0, "SetSocketDescriptor", ret_i32_0, IPC_STUB}, {1, "SetHostName", noop, IPC_STUB}, {2, "SetVerifyOption", noop, IPC_STUB}, {3, "SetIoMode", noop, IPC_STUB},
    {4, "GetSocketDescriptor", ret_i32_0, IPC_STUB}, {5, "GetHostName", ret_u32_0, IPC_STUB}, {6, "GetVerifyOption", ret_u32_0, IPC_STUB}, {7, "GetIoMode", ret_u32_0, IPC_STUB},
    {8, "DoHandshake", noop, IPC_STUB}, {19, "SetSessionCacheMode", noop, IPC_STUB}, {20, "GetSessionCacheMode", ret_u32_0, IPC_STUB}, {21, "FlushSessionCache", noop, IPC_STUB},
};
static void ssl_create_connection(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("ISslConnection", ssl_connection_cmds, NULL, NULL)); }
static const IpcCmd ssl_context_cmds[] = {
    {0, "SetOption", noop, IPC_STUB}, {1, "GetOption", ret_u32_0, IPC_STUB}, {2, "CreateConnection", ssl_create_connection, IPC_STUB}, {3, "GetConnectionCount", ret_u32_0, IPC_STUB},
    {4, "ImportServerPki", ret_u64_0, IPC_STUB}, {5, "ImportClientPki", ret_u64_0, IPC_STUB}, {6, "RemovePki", noop, IPC_STUB},
};
static void ssl_create_context(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("ISslContext", ssl_context_cmds, NULL, NULL)); }
static const IpcCmd ssl_cmds[] = {
    {0, "CreateContext", ssl_create_context, IPC_STUB}, {1, "GetContextCount", ret_u32_0, IPC_STUB}, {5, "SetInterfaceVersion", noop, IPC_STUB},
};
static IpcObject *ssl_open(void) { return IPC_OBJECT("ssl", ssl_cmds, NULL, NULL); }

void service_register_offline(void) {
    ipc_register_service("prepo:u", prepo_open); ipc_register_service("prepo:a", prepo_open); ipc_register_service("prepo:m", prepo_open); ipc_register_service("prepo:s", prepo_open);
    ipc_register_service("ssl", ssl_open);
    ipc_register_service("nfp:user", nfp_open); ipc_register_service("nfc:user", nfp_open);
    ipc_register_service("bsd:u", bsd_open); ipc_register_service("bsd:s", bsd_open); ipc_register_service("bsd:a", bsd_open);
    ipc_register_service("nsd:u", nsd_open); ipc_register_service("nsd:a", nsd_open);
    ipc_register_service("csrng", csrng_open);
    ipc_register_service("nifm:u", nifm_open); ipc_register_service("nifm:a", nifm_open); ipc_register_service("nifm:s", nifm_open);
    ipc_register_service("mm:u", mm_open);
    ipc_register_service("friend:u", friend_open); ipc_register_service("friend:v", friend_open); ipc_register_service("friend:m", friend_open);
    ipc_register_service("friend:s", friend_open); ipc_register_service("friend:a", friend_open);
    (void)ret_u8_0; (void)ret_u64_0; (void)ret_event; (void)ret_event_signaled;
}
