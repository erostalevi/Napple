/* Services that exist on a console but have no meaning for an offline, single-user local run. Each answers a documented safe default
 * (state where a title can observe it, otherwise a documented answer; rows still IPC_STUB are listed in docs/architecture/runtime-services.md): the point is that the SDK's initialisation succeeds and the game falls
 * back to its offline paths. Unknown commands still fail loudly through the generic policy. */
#include "../ipc.h"
#include "../napple.h"
#include "offline_core.h"
#include "offline_state_core.h"
#include "hz_result.h"
#include "online_provider.h"
#include "bsd_core.h"
#include "nfp_core.h"
#include "amiibo_lib_core.h"
#include "install_id_core.h"
#include <limits.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
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
static void ret_u64_0(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = 0;
    IPC_OUT(c, v);
}
static void ret_event(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t r = 0;
    event_create(&r);
    ipc_out_copy_handle(c, r);
}
static void ret_event_signaled(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t r = 0, w = event_create(&r);
    event_signal_handle(w);
    ipc_out_copy_handle(c, r);
}
#define NOT_AVAILABLE(mod, desc) ((mod) | ((desc) << 9))

/* mm:u (nn::mmnv): multimedia clock requests. Modules are hardware clocks (VIC, NVDEC, ...): a request is remembered and "applied" at once (offline_state_core.c),
 * Get reads back what was set. Argument layouts (module, priority, event-clear mode / request id, min, max) are the author's memory (unverified). */
static MmTable mm_tab;
static pthread_mutex_t mm_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t mm_arg(const IpcCall *c, uint32_t off) { return c->in_size >= off + 4 ? IPC_IN(c, uint32_t, off) : 0; }
static void mm_init_old(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&mm_lock);
    mm_module_open(&mm_tab, mm_arg(c, 0));
    pthread_mutex_unlock(&mm_lock);
}
static void mm_fini_old(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&mm_lock);
    mm_module_close(&mm_tab, mm_arg(c, 0));
    pthread_mutex_unlock(&mm_lock);
}
static void mm_set_old(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&mm_lock);
    mm_module_set(&mm_tab, mm_arg(c, 0), mm_arg(c, 4), (int32_t)mm_arg(c, 8));
    pthread_mutex_unlock(&mm_lock);
}
static void mm_get_old(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&mm_lock);
    uint32_t v = mm_module_get(&mm_tab, mm_arg(c, 0));
    pthread_mutex_unlock(&mm_lock);
    IPC_OUT(c, v);
}
static void mm_init_new(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&mm_lock);
    uint32_t id = mm_request_open(&mm_tab, mm_arg(c, 0));
    pthread_mutex_unlock(&mm_lock);
    IPC_OUT(c, id);
}
static void mm_fini_new(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&mm_lock);
    mm_request_close(&mm_tab, mm_arg(c, 0));
    pthread_mutex_unlock(&mm_lock);
}
static void mm_set_new(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&mm_lock);
    mm_request_set(&mm_tab, mm_arg(c, 0), mm_arg(c, 4), (int32_t)mm_arg(c, 8));
    pthread_mutex_unlock(&mm_lock);
}
static void mm_get_new(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&mm_lock);
    uint32_t v = mm_request_get(&mm_tab, mm_arg(c, 0));
    pthread_mutex_unlock(&mm_lock);
    IPC_OUT(c, v);
}
static const IpcCmd mm_cmds[] = {
    {0, "InitializeOld", mm_init_old, IPC_IMPL}, {1, "FinalizeOld", mm_fini_old, IPC_IMPL}, {2, "SetAndWaitOld", mm_set_old, IPC_IMPL},
    {3, "GetOld", mm_get_old, IPC_IMPL},         {4, "Initialize", mm_init_new, IPC_IMPL},  {5, "Finalize", mm_fini_new, IPC_IMPL},
    {6, "SetAndWait", mm_set_new, IPC_IMPL},     {7, "Get", mm_get_new, IPC_IMPL},
};
static IpcObject *mm_open(void) { return IPC_OBJECT("mm:u", mm_cmds, NULL, NULL); }

/* friend:u (nn::friends): the friend list and presence come from the active OnlineProvider; with the NullProvider there are no online friends.
 * The notification service and the friend service accept the calls games make at start-up. Record layouts of the list calls are unverified. */
static void friend_pop_empty(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = friend_pop_notification_result();
} /* an empty queue is an error, not a zeroed record */
/* INotificationService: the queue is always empty (no provider pushes notifications), so Clear has nothing to drop and Pop reports the empty-queue result. */
static const IpcCmd friend_notification_cmds[] = {
    {0, "GetEvent", ret_event, IPC_IMPL},
    {1, "Clear", noop, IPC_IMPL},
    {2, "Pop", friend_pop_empty, IPC_IMPL},
};
static uint32_t friend_provider_count(void) {
    const OnlineProvider *p = online_provider();
    uint32_t (*cnt)(void *) = ONLINE_FN(p, friend_count);
    return cnt ? cnt(p->ctx) : 0;
}
static void friend_count_call(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = friend_provider_count();
    IPC_OUT(c, v);
}
/* 10100 GetFriendListIds / 10101 GetFriendList: {u32 offset, ...} + a buffer of ids (u64) or records (0x300 bytes: u64 id at 0, UTF-8 nickname at 0x40 [proposed
 * layout]); returns the number of entries written. */
#define FRIEND_RECORD_SIZE 0x300
static void friend_list_ids(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t off = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0, written = 0;
    const OnlineProvider *p = online_provider();
    uint32_t (*list)(void *, uint32_t, OnlineFriend *, uint32_t) = ONLINE_FN(p, friend_list);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (list && b && b->size >= 8) {
        uint32_t max = (uint32_t)(b->size / 8 > 64 ? 64 : b->size / 8);
        OnlineFriend f[64];
        uint32_t n = list(p->ctx, off, f, max);
        uint64_t ids[64];
        for (uint32_t i = 0; i < n && i < max; i++) ids[written++] = f[i].id;
        ipc_write_buf(c, b, ids, (uint64_t)written * 8);
    }
    IPC_OUT(c, written);
}
static void friend_list_records(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t off = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0, written = 0;
    const OnlineProvider *p = online_provider();
    uint32_t (*list)(void *, uint32_t, OnlineFriend *, uint32_t) = ONLINE_FN(p, friend_list);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (list && b && b->size >= FRIEND_RECORD_SIZE) {
        uint32_t max = (uint32_t)(b->size / FRIEND_RECORD_SIZE > 16 ? 16 : b->size / FRIEND_RECORD_SIZE);
        OnlineFriend f[16];
        uint32_t n = list(p->ctx, off, f, max);
        static uint8_t rec[16 * FRIEND_RECORD_SIZE];
        memset(rec, 0, sizeof rec);
        for (uint32_t i = 0; i < n && i < max; i++, written++) {
            memcpy(rec + (size_t)i * FRIEND_RECORD_SIZE, &f[i].id, 8);
            memcpy(rec + (size_t)i * FRIEND_RECORD_SIZE + 0x40, f[i].name, sizeof f[i].name);
        }
        ipc_write_buf(c, b, rec, (uint64_t)written * FRIEND_RECORD_SIZE);
    }
    IPC_OUT(c, written);
}
static void friend_presence(IpcObject *s, IpcCall *c) {
    (void)s;
    const OnlineProvider *p = online_provider();
    void (*up)(void *, const void *, size_t) = ONLINE_FN(p, presence_update);
    const IpcBuf *b = ipc_in_buf(c, 0);
    if (up && b && b->size && ipc_buf_ptr(b)) up(p->ctx, ipc_buf_ptr(b), (size_t)b->size);
    else if (up) up(p->ctx, c->in, c->in_size);
}
/* Blocked users, new friends and pending requests come from the account service; the provider has no hook for them, so the counts are 0 (class c). The
 * online-play declarations are remembered per service object (online_play) so a title can bracket a session; the provider hears it through presence updates. */
static void friend_declare_open(IpcObject *s, IpcCall *c) {
    (void)c;
    if (s->state) *(uint8_t *)s->state = 1;
}
static void friend_declare_close(IpcObject *s, IpcCall *c) {
    (void)c;
    if (s->state) *(uint8_t *)s->state = 0;
}
/* 10700 GetPlayHistoryRegistrationKey(u32 local, user id at +8) -> buffer 0x40: the key a title hands the other players of a session so they can record
 * having played together (MK8D asks for it as soon as it hosts a local-wireless room, and ends the room when it fails). A zero user id is refused
 * like an unknown user [result unverified]. */
static void friend_play_history_key(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t user[16] = {0}, key[0x40], inst[16];
    if (c->in_size >= 24) memcpy(user, c->in + 8, 16);
    static const uint8_t zero[16];
    if (!memcmp(user, zero, 16)) {
        c->result = HZ_RESULT(121, 15);
        return;
    }
    install_id_get(g_cfg.save_root, inst);
    install_play_history_key(inst, user, c->in_size >= 4 && IPC_IN(c, uint32_t, 0) != 0, key);
    if (b && b->size) ipc_write_buf(c, b, key, b->size < sizeof key ? b->size : sizeof key);
}
static const IpcCmd friend_service_cmds[] = {
    {0, "GetCompletionEvent", ret_event, IPC_IMPL},
    {1, "Cancel", noop, IPC_IMPL},
    {10100, "GetFriendListIds", friend_list_ids, IPC_IMPL},
    {10101, "GetFriendList", friend_list_records, IPC_IMPL},
    {10400, "GetBlockedUserListIds", ret_u32_0, IPC_IMPL},
    {20100, "GetFriendCount", friend_count_call, IPC_IMPL},
    {20101, "GetNewlyFriendCount", ret_u32_0, IPC_IMPL},
    {20200, "GetReceivedFriendRequestCount", ret_u32_0, IPC_IMPL},
    {10610, "UpdateUserPresence", friend_presence, IPC_IMPL},
    {10600, "DeclareOpenOnlinePlaySession", friend_declare_open, IPC_IMPL},
    {10601, "DeclareCloseOnlinePlaySession", friend_declare_close, IPC_IMPL},
    {10102, "UpdateFriendInfo", sysd_ok, IPC_STUB},
    {10110, "GetFriendProfileImage", sysd_err_friend, IPC_STUB},
    {10111, "GetFriendProfileImageWithImageSize", sysd_err_friend, IPC_STUB},
    {10120, "CheckFriendListAvailability", sysd_err_friend, IPC_STUB},
    {10121, "EnsureFriendListAvailable", sysd_err_friend, IPC_STUB},
    {10200, "SendFriendRequestForApplication", sysd_err_friend, IPC_STUB},
    {10211, "AddFacedFriendRequestForApplication", sysd_ok, IPC_STUB},
    {10420, "CheckBlockedUserListAvailability", sysd_err_friend, IPC_STUB},
    {10421, "EnsureBlockedUserListAvailable", sysd_err_friend, IPC_STUB},
    {10500, "GetProfileListLegacyV1", sysd_err_friend, IPC_STUB},
    {10501, "GetProfileListLegacyV2", sysd_err_friend, IPC_STUB},
    {10502, "GetProfileListV1", sysd_err_friend, IPC_STUB},
    {10503, "GetProfileListV2", sysd_err_friend, IPC_STUB},
    {10700, "GetPlayHistoryRegistrationKey", friend_play_history_key, IPC_IMPL},
    {10701, "GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId", sysd_err_friend, IPC_STUB},
    {10702, "AddPlayHistory", sysd_ok, IPC_STUB},
    {11000, "GetProfileImageUrl", sysd_err_friend, IPC_STUB},
    {11001, "GetProfileImageUrlV2", sysd_err_friend, IPC_STUB},
    {20102, "GetFriendDetailedInfo", sysd_err_friend, IPC_STUB},
    {20103, "SyncFriendList", sysd_ok, IPC_STUB},
    {20104, "RequestSyncFriendList", sysd_ok, IPC_STUB},
    {20107, "GetFriendDetailedInfoV2", sysd_err_friend, IPC_STUB},
    {20110, "LoadFriendSettingV1", sysd_err_friend, IPC_STUB},
    {20111, "LoadFriendSettingV2", sysd_err_friend, IPC_STUB},
    {20201, "GetFriendRequestListV1", sysd_err_friend, IPC_STUB},
    {20202, "GetFriendRequestListV2", sysd_err_friend, IPC_STUB},
    {20203, "GetFriendRequestReceivedNotificationCount", sysd_z4, IPC_STUB},
    {20300, "GetFriendCandidateList", sysd_err_friend, IPC_STUB},
    {20301, "GetNintendoNetworkIdInfo", sysd_err_friend, IPC_STUB},
    {20400, "GetBlockedUserListV1", sysd_err_friend, IPC_STUB},
    {20401, "SyncBlockedUserList", sysd_ok, IPC_STUB},
    {20402, "GetBlockedUserListV2", sysd_err_friend, IPC_STUB},
    {20500, "GetProfileExtraListV1", sysd_err_friend, IPC_STUB},
    {20501, "GetRelationship", sysd_err_friend, IPC_STUB},
    {20502, "GetProfileExtraListV2", sysd_err_friend, IPC_STUB},
    {20600, "GetUserPresenceViewV1", sysd_err_friend, IPC_STUB},
    {20601, "GetUserPresenceViewV2", sysd_err_friend, IPC_STUB},
    {20700, "GetPlayHistoryListV1", sysd_err_friend, IPC_STUB},
    {20701, "GetPlayHistoryStatistics", sysd_err_friend, IPC_STUB},
    {20702, "GetPlayHistoryListV2", sysd_err_friend, IPC_STUB},
    {20800, "LoadUserSettingV1", sysd_err_friend, IPC_STUB},
    {20801, "SyncUserSetting", sysd_ok, IPC_STUB},
    {20802, "LoadUserSettingV2", sysd_err_friend, IPC_STUB},
    {20900, "RequestListSummaryOverlayNotification", sysd_ok, IPC_STUB},
    {21000, "GetExternalApplicationCatalog", sysd_err_friend, IPC_STUB},
    {22000, "GetReceivedFriendInvitationListV1", sysd_err_friend, IPC_STUB},
    {22001, "GetReceivedFriendInvitationDetailedInfoV1", sysd_err_friend, IPC_STUB},
    {22002, "GetReceivedFriendInvitationListV2", sysd_err_friend, IPC_STUB},
    {22003, "GetReceivedFriendInvitationDetailedInfoV2", sysd_err_friend, IPC_STUB},
    {22010, "GetReceivedFriendInvitationCountCache", sysd_err_friend, IPC_STUB},
};
static void friend_service_destroy(IpcObject *o) { free(o->state); }
static void friend_create_service(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IFriendService", friend_service_cmds, calloc(1, 1), friend_service_destroy)); /* state: online_play flag */
}
static void friend_create_notification(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("INotificationService", friend_notification_cmds, NULL, NULL));
}
static const IpcCmd friend_cmds[] = {
    {0, "CreateFriendService", friend_create_service, IPC_IMPL},
    {1, "CreateNotificationService", friend_create_notification, IPC_IMPL},
    {2, "CreateDaemonSuspendSessionService", friend_create_service, IPC_IMPL},
};
static IpcObject *friend_open(void) { return IPC_OBJECT("friend:u", friend_cmds, NULL, NULL); }

/* nifm:u (nn::nifm): the network interface manager. The NullProvider has no network: requests stay "free" (rejected at submit), the connection status is
 * "disconnected", no IP address. A provider with ONLINE_CAP_NETWORK accepts requests and reports its interface. */
static int nifm_info(OnlineNetInfo *o) {
    const OnlineProvider *p = online_provider();
    int (*f)(void *, OnlineNetInfo *) = ONLINE_FN(p, net_info);
    memset(o, 0, sizeof *o);
    return f && f(p->ctx, o) == 0 && o->connected;
}
/* {u8 type (1 wireless, 2 wired), u8 wifi strength, u8 status (4 connected), pad}: all zero when disconnected [layout unverified]. */
static void ret_conn_status(IpcObject *s, IpcCall *c) {
    (void)s;
    OnlineNetInfo ni;
    uint8_t v[4] = {0};
    if (nifm_info(&ni)) {
        v[0] = ni.kind ? ni.kind : 1;
        v[1] = ni.strength;
        v[2] = 4;
    }
    ipc_out(c, v, 4);
}
/* GetCurrentIpConfigInfo: IpConfigInfo {IpAddressSetting {u8 automatic, ip, mask, gateway}, DnsSetting {u8 automatic, dns1, dns2}}, 0x16 bytes, addresses
 * as in_addr (network order). Not connected: the nifm "no network" result. */
static void nifm_ip_config(IpcObject *s, IpcCall *c) {
    (void)s;
    OnlineNetInfo ni;
    if (!nifm_info(&ni)) {
        c->result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
        return;
    }
    uint8_t v[0x18] = {0};
    uint32_t a[5] = {htonl(ni.ip), htonl(ni.mask), htonl(ni.gateway), htonl(ni.dns1), htonl(ni.dns2)};
    v[0] = 1;
    memcpy(v + 1, &a[0], 4), memcpy(v + 5, &a[1], 4), memcpy(v + 9, &a[2], 4);
    v[13] = 1;
    memcpy(v + 14, &a[3], 4), memcpy(v + 18, &a[4], 4);
    ipc_out(c, v, 0x16);
}
static void nifm_ip(IpcObject *s, IpcCall *c) {
    (void)s;
    OnlineNetInfo ni;
    uint32_t v = nifm_info(&ni) ? ni.ip : 0;
    IPC_OUT(c, v);
}
/* Communication settings of the nifm service (service-wide, shared by all clients): wireless on by default (the console's airplane mode is off), ethernet off (no
 * LAN adapter), the acceptable network type flags and the Wake-on-WLAN timers are stored as set. */
static uint8_t nifm_wireless_on = 1, nifm_ethernet_on, nifm_background_requests;
static uint32_t nifm_acceptable_types, nifm_wowl_wake_time, nifm_wowl_keepalive;
static uint8_t nifm_default_ip_setting[0x80];
static uint8_t nifm_flag_in(const IpcCall *c) { return c->in_size >= 1 && IPC_IN(c, uint8_t, 0) ? 1 : 0; }
static void nifm_set_wireless(IpcObject *s, IpcCall *c) {
    (void)s;
    __atomic_store_n(&nifm_wireless_on, nifm_flag_in(c), __ATOMIC_SEQ_CST);
}
static void nifm_wireless_enabled(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = __atomic_load_n(&nifm_wireless_on, __ATOMIC_SEQ_CST);
    IPC_OUT(c, v);
}
static void nifm_set_ethernet(IpcObject *s, IpcCall *c) {
    (void)s;
    __atomic_store_n(&nifm_ethernet_on, nifm_flag_in(c), __ATOMIC_SEQ_CST);
}
static void nifm_ethernet_enabled(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = __atomic_load_n(&nifm_ethernet_on, __ATOMIC_SEQ_CST);
    IPC_OUT(c, v);
}
static void nifm_set_background(IpcObject *s, IpcCall *c) {
    (void)s;
    __atomic_store_n(&nifm_background_requests, nifm_flag_in(c), __ATOMIC_SEQ_CST);
}
static void nifm_set_acceptable(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size >= 4) __atomic_store_n(&nifm_acceptable_types, IPC_IN(c, uint32_t, 0), __ATOMIC_SEQ_CST);
}
static void nifm_get_acceptable(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = __atomic_load_n(&nifm_acceptable_types, __ATOMIC_SEQ_CST);
    IPC_OUT(c, v);
}
static void nifm_set_wowl_wake(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size >= 4) __atomic_store_n(&nifm_wowl_wake_time, IPC_IN(c, uint32_t, 0), __ATOMIC_SEQ_CST);
}
static void nifm_set_wowl_keepalive(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size >= 4) __atomic_store_n(&nifm_wowl_keepalive, IPC_IN(c, uint32_t, 0), __ATOMIC_SEQ_CST);
}
static volatile int nifm_accepted_requests; /* requests currently in the accepted state, for IsAnyInternetRequestAccepted */
static void nifm_any_accepted(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = __atomic_load_n(&nifm_accepted_requests, __ATOMIC_SEQ_CST) > 0;
    IPC_OUT(c, v);
}
typedef struct {
    NifmRequest req;
    uint32_t ev_w[2];
    int counted;
    /* parameters the title set before Submit (nothing without a network reads them, but Submit validates and they read back through the unit tests) */
    uint32_t requirement_preset, priority, raw_priority, profile_id[4], revision;
    uint8_t rejectable, persistent, instant, sustainable, confirmation_option, greedy, sharable, kept_in_sleep;
    uint8_t requirement[0x10];
    int32_t sockets[8];
    unsigned nsockets;
} NifmReq; /* the request state machine (offline_core.c) plus the writable ends of its two events */
static void nifm_uncount(NifmReq *q) {
    if (q->counted) __atomic_sub_fetch(&nifm_accepted_requests, 1, __ATOMIC_SEQ_CST);
    q->counted = 0;
}
/* Live requests, so the service-wide transitions (PutToSleep, Shutdown, NotifyConnectionStateChanged) can reach every request the way nifm does; and the
 * service's power state: awake, asleep (PutToSleep until WakeUp) or shut down (Shutdown, for the rest of the session). Asleep or shut down, nifm makes no
 * connection: Submit is rejected with the communication-disabled result without asking the provider. */
#define NIFM_MAX_LIVE 64
enum { NIFM_POWER_AWAKE, NIFM_POWER_ASLEEP, NIFM_POWER_SHUTDOWN };
static NifmReq *nifm_live[NIFM_MAX_LIVE];
static int nifm_power;
static pthread_mutex_t nifm_live_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t nifm_telemetry_w, nifm_telemetry_r; /* GetTelemetorySystemEventReadableHandle: signalled when the connection state changes */
static void nifm_telemetry_signal(void) {
    pthread_mutex_lock(&nifm_live_lock);
    uint32_t w = nifm_telemetry_w;
    pthread_mutex_unlock(&nifm_live_lock);
    if (w) event_signal_handle(w);
}
static void nifm_live_add(NifmReq *q) {
    pthread_mutex_lock(&nifm_live_lock);
    for (unsigned i = 0; i < NIFM_MAX_LIVE; i++)
        if (!nifm_live[i]) {
            nifm_live[i] = q;
            break;
        }
    pthread_mutex_unlock(&nifm_live_lock);
}
static void nifm_live_remove(NifmReq *q) {
    pthread_mutex_lock(&nifm_live_lock);
    for (unsigned i = 0; i < NIFM_MAX_LIVE; i++)
        if (nifm_live[i] == q) nifm_live[i] = NULL;
    pthread_mutex_unlock(&nifm_live_lock);
}
static void nifm_req_destroy(IpcObject *o) {
    NifmReq *q = o->state;
    nifm_live_remove(q);
    int was_accepted = q->req.state == NIFM_STATE_ACCEPTED;
    nifm_request_cancel(&q->req); /* releases the provider connection */
    nifm_uncount(q);
    free(q);
    if (was_accepted) nifm_telemetry_signal();
}
static void nifm_signal(NifmReq *q) {
    for (int i = 0; i < 2; i++)
        if (q->ev_w[i]) event_signal_handle(q->ev_w[i]);
}
/* An accepted request loses its connection (sleep without SetKeptInSleep, Shutdown, the provider went down): the connection goes back to the provider and
 * the request ends free with the communication-disabled result, as a request nifm had to drop; its events fire so the title re-reads the state. */
static void nifm_drop(NifmReq *q) {
    nifm_request_cancel(&q->req);
    q->req.submitted = 1;
    q->req.result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
    nifm_uncount(q);
    nifm_signal(q);
}
/* Drops every accepted live request (under the registry lock: a request cannot be destroyed meanwhile); keep(q) = 1 leaves one alone. */
static void nifm_drop_accepted(int (*keep)(const NifmReq *)) {
    int changed = 0;
    pthread_mutex_lock(&nifm_live_lock);
    for (unsigned i = 0; i < NIFM_MAX_LIVE; i++) {
        NifmReq *q = nifm_live[i];
        if (q && q->req.state == NIFM_STATE_ACCEPTED && !(keep && keep(q))) nifm_drop(q), changed = 1;
    }
    pthread_mutex_unlock(&nifm_live_lock);
    if (changed) nifm_telemetry_signal();
}
static void nifm_req_state(IpcObject *s, IpcCall *c) {
    NifmReq *q = s->state;
    uint32_t v = (uint32_t)q->req.state;
    IPC_OUT(c, v);
}
static void nifm_req_result(IpcObject *s, IpcCall *c) {
    NifmReq *q = s->state;
    c->result = nifm_request_result(&q->req);
}
static void nifm_req_events(IpcObject *s, IpcCall *c) {
    NifmReq *q = s->state;
    uint32_t r1 = 0, r2 = 0;
    q->ev_w[0] = event_create(&r1);
    q->ev_w[1] = event_create(&r2);
    if (q->req.submitted) nifm_signal(q);
    ipc_out_copy_handle(c, r1);
    ipc_out_copy_handle(c, r2);
}
static void nifm_req_submit(IpcObject *s, IpcCall *c) {
    (void)c;
    NifmReq *q = s->state;
    int was_accepted = q->req.state == NIFM_STATE_ACCEPTED;
    nifm_uncount(q);
    if (__atomic_load_n(&nifm_power, __ATOMIC_SEQ_CST) != NIFM_POWER_AWAKE) { /* asleep or shut down: no connection is made */
        nifm_request_cancel(&q->req);
        q->req.submitted = 1;
        q->req.state = NIFM_STATE_FREE;
        q->req.result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
    } else nifm_request_submit(&q->req);
    if (q->req.state == NIFM_STATE_ACCEPTED) {
        __atomic_add_fetch(&nifm_accepted_requests, 1, __ATOMIC_SEQ_CST);
        q->counted = 1;
    }
    nifm_signal(q);
    if (was_accepted || q->req.state == NIFM_STATE_ACCEPTED) nifm_telemetry_signal();
} /* the NullProvider rejects at once */
static void nifm_req_cancel(IpcObject *s, IpcCall *c) {
    (void)c;
    NifmReq *q = s->state;
    int was_accepted = q->req.state == NIFM_STATE_ACCEPTED;
    nifm_request_cancel(&q->req);
    nifm_uncount(q);
    if (was_accepted) nifm_telemetry_signal();
}
static uint32_t nifm_u32(const IpcCall *c) { return c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0; }
static uint8_t nifm_u8(const IpcCall *c) { return c->in_size >= 1 ? IPC_IN(c, uint8_t, 0) : 0; }
static void nifm_set_requirement(IpcObject *s, IpcCall *c) {
    NifmReq *q = s->state;
    memset(q->requirement, 0, sizeof q->requirement);
    memcpy(q->requirement, c->in, c->in_size < sizeof q->requirement ? c->in_size : sizeof q->requirement);
}
static void nifm_set_preset(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->requirement_preset = nifm_u32(c); }
static void nifm_set_priority(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->priority = nifm_u32(c); }
static void nifm_set_profile(IpcObject *s, IpcCall *c) {
    NifmReq *q = s->state;
    memset(q->profile_id, 0, sizeof q->profile_id);
    memcpy(q->profile_id, c->in, c->in_size < 16 ? c->in_size : 16);
}
static void nifm_set_rejectable(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->rejectable = nifm_u8(c); }
static void nifm_set_confirmation(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->confirmation_option = nifm_u8(c); }
static void nifm_set_persistent(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->persistent = nifm_u8(c); }
static void nifm_set_instant(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->instant = nifm_u8(c); }
static void nifm_set_sustainable(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->sustainable = nifm_u8(c); }
static void nifm_set_raw_priority(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->raw_priority = nifm_u32(c); }
static void nifm_set_greedy(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->greedy = nifm_u8(c); }
static void nifm_set_sharable(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->sharable = nifm_u8(c); }
static void nifm_set_kept_in_sleep(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->kept_in_sleep = nifm_u8(c); }
static void nifm_set_revision(IpcObject *s, IpcCall *c) { ((NifmReq *)s->state)->revision = nifm_u32(c); }
static void nifm_get_revision(IpcObject *s, IpcCall *c) {
    uint32_t v = ((NifmReq *)s->state)->revision;
    IPC_OUT(c, v);
}
/* GetRequirement: 0x20 bytes of Requirement plus, since 3.0.0, 4 more; the first 0x10 are what SetRequirement stored (zero when the title only chose a preset). */
static void nifm_get_requirement(IpcObject *s, IpcCall *c) {
    NifmReq *q = s->state;
    uint8_t v[0x24] = {0};
    memcpy(v, q->requirement, sizeof q->requirement);
    ipc_out(c, v, sizeof v);
}
/* GetAppletInfo(u32 theme color) -> buffer {s32 size, data}* ended by size -1, u32 AppletId, u32 LibraryAppletMode, u32 bytes of the buffer used (switchbrew). A
 * request that failed (an offline console rejects at once) is resolved by the user in the network-connection library applet (netConnect, applet id 0x0f, mode 0,
 * foreground). The SDK (nn::nifm::detail::RequestClient::PrepareLibraryApplet, MK8D update sdk +0x150e84) creates the applet and pushes exactly the storages
 * listed here, nothing of its own, so the list carries what netConnect reads (its readers in firmware 22.1.0 0100000000001006 +0x245e40 / +0x245f80):
 * CommonArguments {u32 version 1, u32 size 0x20, u32 la version, u32 theme color, u8 play startup sound, pad, u64 system tick} and its argument
 * {u32 0, Result of the request, Uuid network profile (zero: none)}, 0x18 bytes, the layout of the SDK's nn::la::NifmToNetConnectArgumentsWriter.
 * The la version nifm passes is not known ([unverified] 0). A request that did not fail has nothing for an applet to fix. */
#define NIFM_APPLET_NET_CONNECT 0x0fu
#define NIFM_NET_CONNECT_ARG_SIZE 0x18u
static void nifm_get_applet_info(IpcObject *s, IpcCall *c) {
    NifmReq *q = s->state;
    uint32_t res = nifm_request_result(&q->req);
    if (!res) {
        c->result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
        return;
    }
    uint32_t theme = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0;
    uint8_t list[4 + 0x20 + 4 + NIFM_NET_CONNECT_ARG_SIZE + 4] = {0}, *p = list;
    int32_t sz = 0x20;
    memcpy(p, &sz, 4);
    uint32_t common_head[4] = {1, 0x20, 0, theme};
    memcpy(p + 4, common_head, sizeof common_head);
    uint64_t tick = runtime_guest_ticks();
    memcpy(p + 4 + 0x18, &tick, 8);
    p += 4 + 0x20;
    sz = NIFM_NET_CONNECT_ARG_SIZE;
    memcpy(p, &sz, 4);
    memcpy(p + 8, &res, 4); /* {u32 0, Result, Uuid 0} */
    p += 4 + NIFM_NET_CONNECT_ARG_SIZE;
    sz = -1;
    memcpy(p, &sz, 4);
    uint32_t used = sizeof list;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || b->size < sizeof list) used = b && b->size >= 4 ? 4 : 0; /* too small for the arguments: only the end marker */
    if (used == 4) ipc_write_buf(c, b, &sz, 4);
    else if (used) ipc_write_buf(c, b, list, used);
    uint32_t v[3] = {NIFM_APPLET_NET_CONNECT, 0, used};
    ipc_out(c, v, 12);
}
/* GetAdditionalInfo (undocumented layout): a failed request reports its own failure, a request that did not fail has no additional information. */
static void nifm_get_additional_info(IpcObject *s, IpcCall *c) {
    NifmReq *q = s->state;
    uint32_t res = nifm_request_result(&q->req);
    c->result = res ? res : HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
}
/* RegisterSocketDescriptor / UnregisterSocketDescriptor(s32 fd): the descriptors the request keeps alive (up to 8). */
static void nifm_register_socket(IpcObject *s, IpcCall *c) {
    NifmReq *q = s->state;
    int32_t fd = c->in_size >= 4 ? IPC_IN(c, int32_t, 0) : -1;
    if (q->nsockets < 8) q->sockets[q->nsockets++] = fd;
}
static void nifm_unregister_socket(IpcObject *s, IpcCall *c) {
    NifmReq *q = s->state;
    int32_t fd = c->in_size >= 4 ? IPC_IN(c, int32_t, 0) : -1;
    for (unsigned i = 0; i < q->nsockets; i++)
        if (q->sockets[i] == fd) {
            q->sockets[i] = q->sockets[--q->nsockets];
            return;
        }
}
static const IpcCmd nifm_request_cmds[] = {
    {0, "GetRequestState", nifm_req_state, IPC_IMPL},
    {1, "GetResult", nifm_req_result, IPC_IMPL},
    {2, "GetSystemEventReadableHandles", nifm_req_events, IPC_IMPL},
    {3, "Cancel", nifm_req_cancel, IPC_IMPL},
    {4, "Submit", nifm_req_submit, IPC_IMPL},
    {5, "SetRequirement", nifm_set_requirement, IPC_IMPL},
    {6, "SetRequirementPreset", nifm_set_preset, IPC_IMPL},
    {8, "SetPriority", nifm_set_priority, IPC_IMPL},
    {9, "SetNetworkProfileId", nifm_set_profile, IPC_IMPL},
    {10, "SetRejectable", nifm_set_rejectable, IPC_IMPL},
    {11, "SetConnectionConfirmationOption", nifm_set_confirmation, IPC_IMPL},
    {12, "SetPersistent", nifm_set_persistent, IPC_IMPL},
    {13, "SetInstant", nifm_set_instant, IPC_IMPL},
    {14, "SetSustainable", nifm_set_sustainable, IPC_IMPL},
    {15, "SetRawPriority", nifm_set_raw_priority, IPC_IMPL},
    {16, "SetGreedy", nifm_set_greedy, IPC_IMPL},
    {17, "SetSharable", nifm_set_sharable, IPC_IMPL},
    {18, "SetRequirementByRevision", nifm_set_revision, IPC_IMPL},
    {19, "GetRequirement", nifm_get_requirement, IPC_IMPL},
    {20, "GetRevision", nifm_get_revision, IPC_IMPL},
    {21, "GetAppletInfo", nifm_get_applet_info, IPC_IMPL},
    {22, "GetAdditionalInfo", nifm_get_additional_info, IPC_STUB},
    {23, "SetKeptInSleep", nifm_set_kept_in_sleep, IPC_IMPL},
    {24, "RegisterSocketDescriptor", nifm_register_socket, IPC_IMPL},
    {25, "UnregisterSocketDescriptor", nifm_unregister_socket, IPC_IMPL},
    {26, "GetNetworkAccessStatus", sysd_z4, IPC_STUB},
};
static void nifm_create_request(IpcObject *s, IpcCall *c) {
    (void)s;
    NifmReq *q = calloc(1, sizeof *q);
    nifm_request_init(&q->req);
    nifm_live_add(q);
    ipc_out_object(c, IPC_OBJECT("IRequest", nifm_request_cmds, q, nifm_req_destroy));
}
/* GetClientId: a stable id per general-service object (a counter in the low word, 16 bytes). SetExclusiveClient remembers the client id given. */
typedef struct {
    uint32_t client_id;
    uint8_t exclusive[16];
} NifmGen;
static uint32_t nifm_client_counter;
static void nifm_get_client_id(IpcObject *s, IpcCall *c) {
    NifmGen *g = s->state;
    uint8_t v[16] = {0};
    memcpy(v, &g->client_id, 4);
    ipc_out(c, v, 16);
}
static void nifm_set_exclusive(IpcObject *s, IpcCall *c) {
    NifmGen *g = s->state;
    memset(g->exclusive, 0, sizeof g->exclusive);
    memcpy(g->exclusive, c->in, c->in_size < 16 ? c->in_size : 16);
}
/* GetDefaultIpSetting / SetDefaultIpSetting: the IpSettingData buffer (up to 0x80 bytes kept); no profile exists, so it is zero (DHCP, no addresses) until set. */
static void nifm_default_ip(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b && b->size) ipc_write_buf(c, b, nifm_default_ip_setting, b->size < sizeof nifm_default_ip_setting ? b->size : sizeof nifm_default_ip_setting);
}
static void nifm_set_default_ip(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    if (!b) return;
    memset(nifm_default_ip_setting, 0, sizeof nifm_default_ip_setting);
    memcpy(nifm_default_ip_setting, ipc_buf_ptr(b), b->size < sizeof nifm_default_ip_setting ? b->size : sizeof nifm_default_ip_setting);
}
/* Network profiles. SfNetworkProfileData, 0x17c bytes (offsets per switchbrew, field names per libnx): IpSettingData @0 (0xc2: IpAddressSetting {u8 auto, ip,
 * mask, gateway}, DnsSetting {u8 auto, dns1, dns2}, ProxySetting 0xaa, u16 mtu @0xc0), Uuid @0xc2, name[0x40] @0xd2, u8 NetworkProfileType @0x112,
 * u8 NetworkInterfaceType @0x113 (1 Ieee80211, 2 Ethernet), u8 IsAutoConnect @0x114, u8 IsLargeCapacity @0x115 (libnx: these four convert to
 * NetworkProfileData +0x50 / +0x54 / +0x58 / +0x59), SfWirelessSettingData @0x116 {u8 ssid_len, ssid[0x20], u8 x21, x22, x23, passphrase[0x41]}, pad.
 * SfNetworkProfileBasicInfo, 0x75 bytes (libnx field list): Uuid, name[0x40], u8 type, u8 interface type, u8 ssid_len, ssid[0x20], u8 authentication,
 * u8 encryption; authentication / encryption are copied from the wireless setting's x22 / x23 [unverified: libnx names them unknown].
 * NetworkProfileType: 0 user, 1 ssid list, 2 temporary; EnumerateNetworkProfiles takes a mask of 1 << type (libnx: NifmNetworkProfileType_User = BIT(0)).
 * This console has no saved profile until one is saved (SetNetworkProfile, a nifm:a command the shared table also serves); up to NIFM_MAX_PROFILES are
 * kept for the session ([unverified] limit; a console keeps them in its system save). With a provider that reports a connection (NAPPLE_NET=host: the
 * Mac's primary interface) that connection is the current profile: built from net_info (automatic addressing reporting its addresses, the interface kind,
 * auto-connect, MTU 1400 [unverified]) under a fixed uuid, listed first by Enumerate and found by GetNetworkProfile. Temporary profiles live as long as
 * their INetworkProfile object. */
#define NIFM_PROFILE_SIZE 0x17cu
#define NIFM_BASIC_INFO_SIZE 0x75u
#define NIFM_MAX_PROFILES 16
#define NIFM_PROF_UUID 0xc2u
#define NIFM_PROF_NAME 0xd2u
#define NIFM_PROF_TYPE 0x112u
#define NIFM_PROF_IFACE 0x113u
#define NIFM_PROF_AUTO 0x114u
#define NIFM_PROF_WIRELESS 0x116u
enum { NIFM_PROFILE_USER = 0, NIFM_PROFILE_SSID_LIST = 1, NIFM_PROFILE_TEMPORARY = 2 };
typedef struct {
    uint8_t used;
    uint8_t d[NIFM_PROFILE_SIZE];
} NifmProfile;
static NifmProfile nifm_profiles[NIFM_MAX_PROFILES]; /* saved and temporary profiles (the type byte tells them apart) */
static pthread_mutex_t nifm_prof_lock = PTHREAD_MUTEX_INITIALIZER;
static const uint8_t nifm_host_uuid[16] = {0x6e, 0x61, 0x70, 0x70, 0x6c, 0x65, 0x4e, 0x45, 0x80, 0x48, 0x6f, 0x73, 0x74, 0x6e, 0x65, 0x74}; /* a v4-form constant */
static int nifm_host_profile(uint8_t *d) {
    OnlineNetInfo ni;
    if (!nifm_info(&ni)) return 0;
    memset(d, 0, NIFM_PROFILE_SIZE);
    uint32_t a[5] = {htonl(ni.ip), htonl(ni.mask), htonl(ni.gateway), htonl(ni.dns1), htonl(ni.dns2)};
    d[0] = 1;
    memcpy(d + 1, &a[0], 4), memcpy(d + 5, &a[1], 4), memcpy(d + 9, &a[2], 4);
    d[0xd] = 1;
    memcpy(d + 0xe, &a[3], 4), memcpy(d + 0x12, &a[4], 4);
    uint16_t mtu = 1400;
    memcpy(d + 0xc0, &mtu, 2);
    memcpy(d + NIFM_PROF_UUID, nifm_host_uuid, 16);
    uint8_t kind = ni.kind ? ni.kind : 1;
    snprintf((char *)d + NIFM_PROF_NAME, 0x40, "%s", kind == 2 ? "Wired Connection" : "Wireless Connection");
    d[NIFM_PROF_TYPE] = NIFM_PROFILE_USER;
    d[NIFM_PROF_IFACE] = kind;
    d[NIFM_PROF_AUTO] = 1;
    return 1;
}
static int nifm_uuid_zero(const uint8_t *u) {
    for (int i = 0; i < 16; i++)
        if (u[i]) return 0;
    return 1;
}
static void nifm_new_uuid(uint8_t *u) {
    arc4random_buf(u, 16);
    u[6] = (uint8_t)((u[6] & 0x0f) | 0x40), u[8] = (uint8_t)((u[8] & 0x3f) | 0x80); /* random (version 4) uuid */
}
/* The profile with `uuid` (the connection's, a saved or a temporary one) copied into d: 1, or 0 when there is none. */
static int nifm_profile_find(const uint8_t *uuid, uint8_t *d) {
    if (!memcmp(uuid, nifm_host_uuid, 16)) return nifm_host_profile(d);
    int found = 0;
    pthread_mutex_lock(&nifm_prof_lock);
    for (unsigned i = 0; i < NIFM_MAX_PROFILES && !found; i++)
        if (nifm_profiles[i].used && !memcmp(nifm_profiles[i].d + NIFM_PROF_UUID, uuid, 16)) memcpy(d, nifm_profiles[i].d, NIFM_PROFILE_SIZE), found = 1;
    pthread_mutex_unlock(&nifm_prof_lock);
    return found;
}
/* Stores d as a profile of `type`, replacing the one with the same uuid; a zero uuid (or the connection's) gets a new one, written back into d. */
static uint32_t nifm_profile_store(uint8_t *d, uint8_t type) {
    if (nifm_uuid_zero(d + NIFM_PROF_UUID) || !memcmp(d + NIFM_PROF_UUID, nifm_host_uuid, 16)) nifm_new_uuid(d + NIFM_PROF_UUID);
    d[NIFM_PROF_TYPE] = type;
    int slot = -1;
    pthread_mutex_lock(&nifm_prof_lock);
    for (unsigned i = 0; i < NIFM_MAX_PROFILES && slot < 0; i++)
        if (nifm_profiles[i].used && !memcmp(nifm_profiles[i].d + NIFM_PROF_UUID, d + NIFM_PROF_UUID, 16)) slot = (int)i;
    for (unsigned i = 0; i < NIFM_MAX_PROFILES && slot < 0; i++)
        if (!nifm_profiles[i].used) slot = (int)i;
    if (slot >= 0) nifm_profiles[slot].used = 1, memcpy(nifm_profiles[slot].d, d, NIFM_PROFILE_SIZE);
    pthread_mutex_unlock(&nifm_prof_lock);
    return slot < 0 ? HZ_R_NIFM_NETWORK_PROFILE_FULL : 0;
}
/* Removes the stored profile with `uuid` when its type is in `mask`; 1 when one was removed. */
static int nifm_profile_remove(const uint8_t *uuid, unsigned mask) {
    int done = 0;
    pthread_mutex_lock(&nifm_prof_lock);
    for (unsigned i = 0; i < NIFM_MAX_PROFILES; i++)
        if (nifm_profiles[i].used && !memcmp(nifm_profiles[i].d + NIFM_PROF_UUID, uuid, 16) && (mask >> nifm_profiles[i].d[NIFM_PROF_TYPE] & 1))
            nifm_profiles[i].used = 0, done = 1;
    pthread_mutex_unlock(&nifm_prof_lock);
    return done;
}
/* The SfNetworkProfileData of input buffer 0 (missing bytes are zero). */
static void nifm_profile_in(const IpcCall *c, uint8_t *d) {
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    memset(d, 0, NIFM_PROFILE_SIZE);
    if (p) memcpy(d, p, b->size < NIFM_PROFILE_SIZE ? (size_t)b->size : NIFM_PROFILE_SIZE);
}
static void nifm_profile_out(IpcCall *c, const uint8_t *d) {
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b && b->size) ipc_write_buf(c, b, d, b->size < NIFM_PROFILE_SIZE ? b->size : NIFM_PROFILE_SIZE);
}
static const uint8_t *nifm_uuid_in(const IpcCall *c, uint8_t *tmp) {
    memset(tmp, 0, 16);
    memcpy(tmp, c->in, c->in_size < 16 ? c->in_size : 16);
    return tmp;
}
/* 5 GetCurrentNetworkProfile -> buffer SfNetworkProfileData: the profile of the current connection; with none, the no-network result. */
static void nifm_current_profile(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t d[NIFM_PROFILE_SIZE];
    if (!nifm_host_profile(d)) {
        c->result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
        return;
    }
    nifm_profile_out(c, d);
}
static void nifm_basic_info(const uint8_t *d, uint8_t *o) {
    const uint8_t *w = d + NIFM_PROF_WIRELESS;
    memset(o, 0, NIFM_BASIC_INFO_SIZE);
    memcpy(o, d + NIFM_PROF_UUID, 16);
    memcpy(o + 0x10, d + NIFM_PROF_NAME, 0x40);
    o[0x50] = d[NIFM_PROF_TYPE], o[0x51] = d[NIFM_PROF_IFACE];
    o[0x52] = w[0] > 0x20 ? 0x20 : w[0];
    memcpy(o + 0x53, w + 1, 0x20);
    o[0x73] = w[0x22], o[0x74] = w[0x23];
}
/* 7 EnumerateNetworkProfiles(u8 type mask) -> buffer SfNetworkProfileBasicInfo[], s32 total: every profile of those types is counted, as many as fit
 * are written (the connection's first). */
static void nifm_enum_profiles(IpcObject *s, IpcCall *c) {
    (void)s;
    unsigned mask = nifm_u8(c);
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint32_t cap = b ? (uint32_t)(b->size / NIFM_BASIC_INFO_SIZE) : 0, n = 0;
    uint8_t tmp[(NIFM_MAX_PROFILES + 1) * NIFM_BASIC_INFO_SIZE], d[NIFM_PROFILE_SIZE];
    int32_t total = 0;
    if ((mask & 1u << NIFM_PROFILE_USER) && nifm_host_profile(d)) {
        if (n < cap) nifm_basic_info(d, tmp + n++ * NIFM_BASIC_INFO_SIZE);
        total++;
    }
    pthread_mutex_lock(&nifm_prof_lock);
    for (unsigned i = 0; i < NIFM_MAX_PROFILES; i++)
        if (nifm_profiles[i].used && (mask >> nifm_profiles[i].d[NIFM_PROF_TYPE] & 1)) {
            if (n < cap) nifm_basic_info(nifm_profiles[i].d, tmp + n++ * NIFM_BASIC_INFO_SIZE);
            total++;
        }
    pthread_mutex_unlock(&nifm_prof_lock);
    if (b && n) ipc_write_buf(c, b, tmp, (uint64_t)n * NIFM_BASIC_INFO_SIZE);
    IPC_OUT(c, total);
}
/* 8 GetNetworkProfile(Uuid) -> buffer SfNetworkProfileData; an unknown uuid is the profile-not-found result. */
static void nifm_get_profile(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t u[16], d[NIFM_PROFILE_SIZE];
    if (!nifm_profile_find(nifm_uuid_in(c, u), d)) {
        c->result = HZ_R_NIFM_NETWORK_PROFILE_NOT_FOUND;
        return;
    }
    nifm_profile_out(c, d);
}
/* 9 SetNetworkProfile(buffer SfNetworkProfileData) -> Uuid: saved as a user profile (replacing the one with its uuid; a zero uuid gets a new one). */
static void nifm_set_profile_data(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t d[NIFM_PROFILE_SIZE];
    nifm_profile_in(c, d);
    uint32_t r = nifm_profile_store(d, NIFM_PROFILE_USER);
    if (r) {
        c->result = r;
        return;
    }
    ipc_out(c, d + NIFM_PROF_UUID, 16);
}
/* 10 RemoveNetworkProfile(Uuid): a saved profile (user or ssid list) is removed; the connection's, a temporary one or an unknown uuid: not found. */
static void nifm_remove_profile(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t u[16];
    if (!nifm_profile_remove(nifm_uuid_in(c, u), 1u << NIFM_PROFILE_USER | 1u << NIFM_PROFILE_SSID_LIST)) c->result = HZ_R_NIFM_NETWORK_PROFILE_NOT_FOUND;
}
/* 14 CreateTemporaryNetworkProfile(buffer SfNetworkProfileData) -> Uuid, INetworkProfile [inputs / outputs unverified: switchbrew only names the object].
 * The profile is a temporary one (type 2) for as long as the object lives. INetworkProfile: 0 Update(buffer SfNetworkProfileData) replaces its data,
 * 1 PersistOld / 2 Persist -> Uuid save a copy as a user profile under a new uuid [unverified]. */
typedef struct {
    uint8_t uuid[16];
} NifmTempProfile;
static void nifm_temp_destroy(IpcObject *o) {
    NifmTempProfile *t = o->state;
    nifm_profile_remove(t->uuid, 1u << NIFM_PROFILE_TEMPORARY);
    free(t);
}
static void nifm_temp_update(IpcObject *s, IpcCall *c) {
    NifmTempProfile *t = s->state;
    uint8_t d[NIFM_PROFILE_SIZE];
    nifm_profile_in(c, d);
    memcpy(d + NIFM_PROF_UUID, t->uuid, 16);
    c->result = nifm_profile_store(d, NIFM_PROFILE_TEMPORARY);
}
static void nifm_temp_persist(IpcObject *s, IpcCall *c) {
    NifmTempProfile *t = s->state;
    uint8_t d[NIFM_PROFILE_SIZE];
    if (!nifm_profile_find(t->uuid, d)) {
        c->result = HZ_R_NIFM_NETWORK_PROFILE_NOT_FOUND;
        return;
    }
    memset(d + NIFM_PROF_UUID, 0, 16);
    uint32_t r = nifm_profile_store(d, NIFM_PROFILE_USER);
    if (r) {
        c->result = r;
        return;
    }
    ipc_out(c, d + NIFM_PROF_UUID, 16);
}
static const IpcCmd nifm_profile_cmds[] = {
    {0, "Update", nifm_temp_update, IPC_IMPL},
    {1, "PersistOld", nifm_temp_persist, IPC_IMPL},
    {2, "Persist", nifm_temp_persist, IPC_IMPL},
};
static void nifm_create_temp_profile(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t d[NIFM_PROFILE_SIZE];
    nifm_profile_in(c, d);
    memset(d + NIFM_PROF_UUID, 0, 16);
    uint32_t r = nifm_profile_store(d, NIFM_PROFILE_TEMPORARY);
    if (r) {
        c->result = r;
        return;
    }
    NifmTempProfile *t = calloc(1, sizeof *t);
    memcpy(t->uuid, d + NIFM_PROF_UUID, 16);
    ipc_out(c, t->uuid, 16);
    ipc_out_object(c, IPC_OBJECT("INetworkProfile", nifm_profile_cmds, t, nifm_temp_destroy));
}
/* 2 CreateScanRequest -> IScanRequest (switchbrew: 0 Submit, 1 IsProcessing, 2 GetResult, 3 GetSystemEventReadableHandle, 4 SetChannels, 5 Cancel). A scan
 * needs the radio: with wireless communication off or nifm asleep / shut down it fails with the communication-disabled result. Otherwise it finishes inside
 * Submit and finds what the provider can see: no provider has a scan hook (the Mac's Wi-Fi list would need one in OnlineProvider), so the scan succeeds
 * with no access point and GetScanData then reports none. SetChannels(buffer s16[]) keeps the channel list [layout unverified]. */
static uint32_t nifm_scan_count; /* access points found by the last scan */
typedef struct {
    uint32_t ev_w, ev_r;
    int submitted;
    uint32_t result;
    int16_t channels[64];
    unsigned nchannels;
} NifmScan;
static void nifm_scan_destroy(IpcObject *o) { free(o->state); }
static void nifm_scan_submit(IpcObject *s, IpcCall *c) {
    (void)c;
    NifmScan *q = s->state;
    int radio = __atomic_load_n(&nifm_wireless_on, __ATOMIC_SEQ_CST) && __atomic_load_n(&nifm_power, __ATOMIC_SEQ_CST) == NIFM_POWER_AWAKE;
    q->result = radio ? 0 : HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
    if (radio) __atomic_store_n(&nifm_scan_count, 0, __ATOMIC_SEQ_CST);
    q->submitted = 1;
    if (q->ev_w) event_signal_handle(q->ev_w);
}
static void nifm_scan_processing(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 0; /* the scan finished inside Submit */
    IPC_OUT(c, v);
}
static void nifm_scan_result(IpcObject *s, IpcCall *c) { c->result = ((NifmScan *)s->state)->result; }
static void nifm_scan_event(IpcObject *s, IpcCall *c) {
    NifmScan *q = s->state;
    if (!q->ev_w) q->ev_w = event_create(&q->ev_r); /* one event per scan request, handed out again on a second call */
    if (q->submitted) event_signal_handle(q->ev_w);
    ipc_out_copy_handle(c, q->ev_r);
}
static void nifm_scan_set_channels(IpcObject *s, IpcCall *c) {
    NifmScan *q = s->state;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    q->nchannels = p ? (unsigned)(b->size / 2 < 64 ? b->size / 2 : 64) : 0;
    if (q->nchannels) memcpy(q->channels, p, q->nchannels * 2);
}
static const IpcCmd nifm_scan_cmds[] = {
    {0, "Submit", nifm_scan_submit, IPC_IMPL},
    {1, "IsProcessing", nifm_scan_processing, IPC_IMPL},
    {2, "GetResult", nifm_scan_result, IPC_IMPL},
    {3, "GetSystemEventReadableHandle", nifm_scan_event, IPC_IMPL},
    {4, "SetChannels", nifm_scan_set_channels, IPC_IMPL},
    {5, "Cancel", noop, IPC_IMPL}, /* nothing is in progress: the scan finished inside Submit */
};
static void nifm_create_scan(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IScanRequest", nifm_scan_cmds, calloc(1, sizeof(NifmScan)), nifm_scan_destroy));
}
/* 35 / 47 GetScanData -> buffer AccessPointData[], s32 count: the access points of the last scan, none (see CreateScanRequest) [layout unverified; switchbrew
 * documents neither AccessPointData nor the reply]. */
static void nifm_scan_data(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t n = (int32_t)__atomic_load_n(&nifm_scan_count, __ATOMIC_SEQ_CST);
    IPC_OUT(c, n);
}
/* 36 GetCurrentAccessPoint -> buffer AccessPointData: only a wireless connection has one. A provider reports no SSID / BSSID (the host provider always
 * reports a wired-style connection), so a wireless connection's data is zero [layout unverified]; no connection or a wired one: the no-network result. */
static void nifm_current_ap(IpcObject *s, IpcCall *c) {
    (void)s;
    OnlineNetInfo ni;
    if (!nifm_info(&ni) || ni.kind == 2) {
        c->result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
        return;
    }
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t z[0x200] = {0};
    if (b && b->size) ipc_write_buf(c, b, z, b->size < sizeof z ? b->size : sizeof z);
}
/* 23 PutToSleep: accepted requests that did not ask SetKeptInSleep lose their connection; nifm makes no new one until 24 WakeUp. 37 Shutdown: every
 * connection is dropped and nifm stays down (WakeUp does not bring it back); 33 ConfirmSystemAvailability then fails with the communication-disabled
 * result [unverified which result a console gives]. */
static int nifm_kept_in_sleep(const NifmReq *q) { return q->kept_in_sleep; }
static void nifm_put_to_sleep(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    int awake = NIFM_POWER_AWAKE;
    if (__atomic_compare_exchange_n(&nifm_power, &awake, NIFM_POWER_ASLEEP, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) nifm_drop_accepted(nifm_kept_in_sleep);
}
static void nifm_wake_up(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    int asleep = NIFM_POWER_ASLEEP;
    __atomic_compare_exchange_n(&nifm_power, &asleep, NIFM_POWER_AWAKE, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static void nifm_shutdown(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    __atomic_store_n(&nifm_power, NIFM_POWER_SHUTDOWN, __ATOMIC_SEQ_CST);
    nifm_drop_accepted(NULL);
}
static void nifm_confirm_available(IpcObject *s, IpcCall *c) {
    (void)s;
    if (__atomic_load_n(&nifm_power, __ATOMIC_SEQ_CST) == NIFM_POWER_SHUTDOWN) c->result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
}
/* 31 GetTelemetorySystemEventReadableHandle: one service-wide event, signalled whenever a request gains or loses its connection. */
static void nifm_telemetry_event(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&nifm_live_lock);
    if (!nifm_telemetry_w) nifm_telemetry_w = event_create(&nifm_telemetry_r);
    uint32_t r = nifm_telemetry_r;
    pthread_mutex_unlock(&nifm_live_lock);
    ipc_out_copy_handle(c, r);
}
/* 42 NotifyConnectionStateChanged: nifm re-reads the connection. When the provider no longer reports one, every accepted request is dropped; the events of
 * every request and the telemetry event fire so their owners re-read the state. */
static void nifm_notify_conn_changed(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    OnlineNetInfo ni;
    if (!nifm_info(&ni)) nifm_drop_accepted(NULL);
    pthread_mutex_lock(&nifm_live_lock);
    for (unsigned i = 0; i < NIFM_MAX_LIVE; i++)
        if (nifm_live[i]) nifm_signal(nifm_live[i]);
    pthread_mutex_unlock(&nifm_live_lock);
    nifm_telemetry_signal();
}
/* 44 IsWiredConnectionAvailable -> bool: a wired connection is up (the provider reports a connection of kind 2: the host provider's). */
static void nifm_wired_available(IpcObject *s, IpcCall *c) {
    (void)s;
    OnlineNetInfo ni;
    uint8_t v = nifm_info(&ni) && ni.kind == 2;
    IPC_OUT(c, v);
}
static const IpcCmd nifm_general_cmds[] = {
    {1, "GetClientId", nifm_get_client_id, IPC_IMPL},
    {4, "CreateRequest", nifm_create_request, IPC_IMPL},
    {12, "GetCurrentIpAddress", nifm_ip, IPC_IMPL},
    {16, "SetWirelessCommunicationEnabled", nifm_set_wireless, IPC_IMPL},
    {17, "IsWirelessCommunicationEnabled", nifm_wireless_enabled, IPC_IMPL},
    {18, "GetInternetConnectionStatus", ret_conn_status, IPC_IMPL},
    {19, "SetEthernetCommunicationEnabled", nifm_set_ethernet, IPC_IMPL},
    {20, "IsEthernetCommunicationEnabled", nifm_ethernet_enabled, IPC_IMPL},
    {21, "IsAnyInternetRequestAccepted", nifm_any_accepted, IPC_IMPL},
    {22, "IsAnyForegroundRequestAccepted", nifm_any_accepted, IPC_IMPL},
    {26, "SetExclusiveClient", nifm_set_exclusive, IPC_IMPL},
    {27, "GetDefaultIpSetting", nifm_default_ip, IPC_IMPL},
    {2, "CreateScanRequest", nifm_create_scan, IPC_IMPL},
    {5, "GetCurrentNetworkProfile", nifm_current_profile, IPC_IMPL},
    {6, "EnumerateNetworkInterfaces", sysd_z4, IPC_STUB}, /* NetworkInterfaceInfo has no public layout */
    {7, "EnumerateNetworkProfiles", nifm_enum_profiles, IPC_IMPL},
    {8, "GetNetworkProfile", nifm_get_profile, IPC_IMPL},
    {9, "SetNetworkProfile", nifm_set_profile_data, IPC_IMPL},
    {10, "RemoveNetworkProfile", nifm_remove_profile, IPC_IMPL},
    {14, "CreateTemporaryNetworkProfile", nifm_create_temp_profile, IPC_IMPL},
    {15, "GetCurrentIpConfigInfo", nifm_ip_config, IPC_IMPL},
    {23, "PutToSleep", nifm_put_to_sleep, IPC_IMPL},
    {24, "WakeUp", nifm_wake_up, IPC_IMPL},
    {25, "GetSsidListVersion", sysd_z4, IPC_STUB}, /* SsidListVersion has no public layout */
    {28, "SetDefaultIpSetting", nifm_set_default_ip, IPC_IMPL},
    {31, "GetTelemetorySystemEventReadableHandle", nifm_telemetry_event, IPC_IMPL},
    {33, "ConfirmSystemAvailability", nifm_confirm_available, IPC_IMPL},
    {34, "SetBackgroundRequestEnabled", nifm_set_background, IPC_IMPL},
    {35, "GetScanData", nifm_scan_data, IPC_IMPL},
    {36, "GetCurrentAccessPoint", nifm_current_ap, IPC_IMPL},
    {37, "Shutdown", nifm_shutdown, IPC_IMPL},
    {38, "GetAllowedChannels", sysd_err_net, IPC_STUB}, /* the channel list layout is not public */
    {39, "NotifyApplicationSuspended", sysd_ok, IPC_STUB}, /* what nifm does with it is not public */
    {40, "SetAcceptableNetworkTypeFlag", nifm_set_acceptable, IPC_IMPL},
    {41, "GetAcceptableNetworkTypeFlag", nifm_get_acceptable, IPC_IMPL},
    {42, "NotifyConnectionStateChanged", nifm_notify_conn_changed, IPC_IMPL},
    {43, "SetWowlDelayedWakeTime", nifm_set_wowl_wake, IPC_IMPL},
    {44, "IsWiredConnectionAvailable", nifm_wired_available, IPC_IMPL},
    {47, "GetScanData", nifm_scan_data, IPC_IMPL},
    {57, "SetWowlTcpKeepAliveTimeout", nifm_set_wowl_keepalive, IPC_IMPL},
};
static void nifm_gen_destroy(IpcObject *o) { free(o->state); }
static void nifm_create_general(IpcObject *s, IpcCall *c) {
    (void)s;
    NifmGen *g = calloc(1, sizeof *g);
    g->client_id = __atomic_add_fetch(&nifm_client_counter, 1, __ATOMIC_SEQ_CST);
    ipc_out_object(c, IPC_OBJECT("IGeneralService", nifm_general_cmds, g, nifm_gen_destroy));
}
static const IpcCmd nifm_cmds[] = {{4, "CreateGeneralServiceOld", nifm_create_general, IPC_IMPL}, {5, "CreateGeneralService", nifm_create_general, IPC_IMPL}};
static IpcObject *nifm_open(void) { return IPC_OBJECT("nifm:u", nifm_cmds, NULL, NULL); }

/* bsd:u (nn::socket): the BSD socket service. Guest descriptors map to the active OnlineProvider's sockets (bsd_core.c). With the NullProvider this is the
 * console with no network: sockets can be created, closed, bound and configured; anything that needs a peer answers {ret -1, errno} the way a machine
 * without a network does (ENETDOWN, ENOTCONN, EAGAIN, EDESTADDRREQ, EBADF for an unknown fd). Errno values are FreeBSD's (hz_result.h). Results are
 * {s32 ret, s32 errno} plus a u32 length for the calls that return an address or an option value. Argument layouts are the author's memory of the
 * public description (unverified on a console); a request shorter than expected is answered, never fatal. */
static pthread_mutex_t bsd_lock = PTHREAD_MUTEX_INITIALIZER; /* kept for the unit tests that reset the table; the table has its own lock */
static BsdGuest *bsd_g(void) { return bsdg_global(); }
static void bsd_reply(IpcCall *c, int ret, int err) {
    int32_t v[2] = {ret, bsd_errno_to_guest(err)}; /* the console's bsd answers with Linux errno numbers */
    ipc_out(c, v, 8);
}
static void bsd_reply3(IpcCall *c, int ret, int err, uint32_t extra) {
    int32_t v[3] = {ret, bsd_errno_to_guest(err), (int32_t)extra};
    ipc_out(c, v, 12);
}
/* RegisterClient: the library registers once per process. The reply is 0 on every registration (the guest's nn::socket initialiser treats any other value as a
 * failure: SMO aborts in al::NetworkTaskSystemInitializer when it gets 1); registrations are only counted. StartMonitoring marks the client as monitored. */
static uint32_t bsd_clients, bsd_monitored;
static void bsd_register_client(IpcObject *s, IpcCall *c) {
    (void)s;
    __atomic_add_fetch(&bsd_clients, 1, __ATOMIC_SEQ_CST);
    uint32_t v = 0;
    IPC_OUT(c, v);
}
static void bsd_start_monitoring(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    __atomic_store_n(&bsd_monitored, 1, __ATOMIC_SEQ_CST);
}
/* GetResourceStatistics: all counters zero (no socket memory is pooled on the host); {ret 0, errno 0} and a zeroed statistics block. */
static void bsd_resource_stats(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t z[0x40] = {0};
    if (b && b->size) ipc_write_buf(c, b, z, b->size < sizeof z ? b->size : sizeof z);
    bsd_reply(c, 0, 0);
}
static void bsd_fail(IpcCall *c, int err) { bsd_reply(c, -1, err); }
static void bsd_enoent(IpcObject *s, IpcCall *c) {
    (void)s;
    bsd_fail(c, HZ_ENOENT);
}
static int32_t bsd_in32(const IpcCall *c, uint32_t off, int32_t dflt) { return c->in_size >= off + 4 ? IPC_IN(c, int32_t, off) : dflt; }
static int bsd_in_fd(const IpcCall *c) { return bsd_in32(c, 0, -1); }
/* A guest sockaddr from input buffer n, or NULL when absent or malformed. */
static OnlineAddr *bsd_in_addr(const IpcCall *c, unsigned n, OnlineAddr *tmp) {
    const IpcBuf *b = ipc_in_buf(c, n);
    const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    return p && bsd_sockaddr_decode(p, (size_t)b->size, tmp) == 0 ? tmp : NULL;
}
static uint32_t bsd_out_addr(IpcCall *c, unsigned n, const OnlineAddr *a) {
    const IpcBuf *b = ipc_out_buf(c, n);
    uint8_t tmp[28];
    size_t len = bsd_sockaddr_encode(a, tmp, sizeof tmp);
    if (b && b->size) ipc_write_buf(c, b, tmp, len < b->size ? len : b->size);
    return (uint32_t)len;
}
static void bsd_socket_call(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = HZ_EINVAL, fd = -1;
    if (c->in_size >= 12) fd = bsdg_socket(bsd_g(), IPC_IN(c, int32_t, 0), IPC_IN(c, int32_t, 4), IPC_IN(c, int32_t, 8), &err);
    bsd_reply(c, fd, fd < 0 ? err : 0);
}
static void bsd_close_call(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    int r = bsdg_close(bsd_g(), bsd_in_fd(c), &err);
    bsd_reply(c, r, err);
}
static void bsd_shutdown_all(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&bsd_lock);
    bsdg_reset(bsd_g());
    pthread_mutex_unlock(&bsd_lock);
    bsd_reply(c, 0, 0);
}
static void bsd_bind(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    OnlineAddr tmp, *a = bsd_in_addr(c, 0, &tmp);
    if (!a) memset(&tmp, 0, sizeof tmp), tmp.family = BSD_AF_INET, a = &tmp;
    int r_ = bsdg_bind(bsd_g(), bsd_in_fd(c), a, &err);
    bsd_reply(c, r_, err);
}
static void bsd_connect(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    OnlineAddr tmp, *a = bsd_in_addr(c, 0, &tmp);
    if (!a) memset(&tmp, 0, sizeof tmp), tmp.family = BSD_AF_INET, a = &tmp;
    int r_ = bsdg_connect(bsd_g(), bsd_in_fd(c), a, &err);
    bsd_reply(c, r_, err);
}
static void bsd_listen(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    int r_ = bsdg_listen(bsd_g(), bsd_in_fd(c), bsd_in32(c, 4, 0), &err);
    bsd_reply(c, r_, err);
}
static void bsd_accept(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    OnlineAddr peer;
    memset(&peer, 0, sizeof peer);
    int r = bsdg_accept(bsd_g(), bsd_in_fd(c), &peer, &err);
    uint32_t len = r >= 0 ? bsd_out_addr(c, 0, &peer) : 0;
    bsd_reply3(c, r, err, len);
}
static void bsd_recv_common(IpcCall *c, int with_addr) {
    int err = 0;
    const IpcBuf *b = ipc_out_buf(c, 0);
    size_t cap = b ? (size_t)(b->size > 0x20000 ? 0x20000 : b->size) : 0;
    static __thread uint8_t buf[0x20000];
    OnlineAddr from;
    memset(&from, 0, sizeof from);
    int64_t r = bsdg_recv(bsd_g(), bsd_in_fd(c), buf, cap, bsd_in32(c, 4, 0), with_addr ? &from : NULL, &err);
    if (r > 0 && b) ipc_write_buf(c, b, buf, (uint64_t)r);
    if (with_addr) bsd_reply3(c, (int)r, err, r >= 0 ? bsd_out_addr(c, 1, &from) : 0);
    else bsd_reply(c, (int)r, err);
}
static void bsd_recv(IpcObject *s, IpcCall *c) {
    (void)s;
    bsd_recv_common(c, 0);
}
static void bsd_recvfrom(IpcObject *s, IpcCall *c) {
    (void)s;
    bsd_recv_common(c, 1);
}
static void bsd_send_common(IpcCall *c, int with_addr) {
    int err = 0;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const void *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    OnlineAddr tmp, *to = with_addr ? bsd_in_addr(c, 1, &tmp) : NULL;
    int64_t r = bsdg_send(bsd_g(), bsd_in_fd(c), p, p ? (size_t)b->size : 0, bsd_in32(c, 4, 0),
                          with_addr ? (to ? to : (memset(&tmp, 0, sizeof tmp), &tmp)) : NULL, &err);
    bsd_reply(c, (int)r, err);
}
static void bsd_send(IpcObject *s, IpcCall *c) {
    (void)s;
    bsd_send_common(c, 0);
}
static void bsd_sendto(IpcObject *s, IpcCall *c) {
    (void)s;
    bsd_send_common(c, 1);
}
static void bsd_name_common(IpcCall *c, int peer) {
    int err = 0;
    OnlineAddr a;
    memset(&a, 0, sizeof a);
    int r = peer ? bsdg_getpeername(bsd_g(), bsd_in_fd(c), &a, &err) : bsdg_getsockname(bsd_g(), bsd_in_fd(c), &a, &err);
    bsd_reply3(c, r, err, r == 0 ? bsd_out_addr(c, 0, &a) : 0);
}
static void bsd_getpeername(IpcObject *s, IpcCall *c) {
    (void)s;
    bsd_name_common(c, 1);
}
static void bsd_getsockname(IpcObject *s, IpcCall *c) {
    (void)s;
    bsd_name_common(c, 0);
}
static void bsd_getsockopt(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t val[64] = {0};
    size_t n = b && b->size < sizeof val ? (size_t)b->size : sizeof val;
    int level = bsd_in32(c, 4, 0), opt = bsd_in32(c, 8, 0);
    int r = bsdg_getsockopt(bsd_g(), bsd_in_fd(c), level, opt, val, &n, &err);
    if (r == 0 && level == 0xffff && opt == 0x1007 && n >= 4) { /* SO_ERROR: the pending error, in the guest's numbering too */
        int32_t pe;
        memcpy(&pe, val, 4);
        pe = bsd_errno_to_guest(pe);
        memcpy(val, &pe, 4);
    }
    if (r == 0 && b && n) ipc_write_buf(c, b, val, n);
    bsd_reply3(c, r, err, r == 0 ? (uint32_t)n : 0);
}
static void bsd_setsockopt(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const void *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    int r_ = bsdg_setsockopt(bsd_g(), bsd_in_fd(c), bsd_in32(c, 4, 0), bsd_in32(c, 8, 0), p, p ? (size_t)b->size : 0, &err);
    bsd_reply(c, r_, err);
}
static void bsd_shutdown(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    int r_ = bsdg_shutdown(bsd_g(), bsd_in_fd(c), bsd_in32(c, 4, 0), &err);
    bsd_reply(c, r_, err);
}
/* Ioctl(fd, request, ...): FIONBIO (0x8004667e) switches non-blocking mode; every other request is accepted and ignored [unverified]. */
static void bsd_ioctl(IpcObject *s, IpcCall *c) {
    (void)s;
    int err, fd = bsd_in_fd(c);
    if ((uint32_t)bsd_in32(c, 4, 0) == 0x8004667eu) {
        const IpcBuf *b = ipc_in_buf(c, 0);
        const int32_t *on = b && b->size >= 4 ? ipc_buf_ptr(b) : NULL;
        int r = bsdg_nonblock(bsd_g(), fd, on ? *on != 0 : 0, &err);
        if (r == 0 && fd >= 0 && fd < BSD_MAX_FDS) bsd_g()->nb[fd] = on && *on;
        bsd_reply(c, r, err);
        return;
    }
    int h = bsdg_handle(bsd_g(), fd);
    bsd_reply(c, h < 0 ? -1 : 0, h < 0 ? HZ_EBADF : 0);
}
/* Fcntl(fd, cmd, arg): F_GETFL (3) returns O_NONBLOCK (4) when set, F_SETFL (4) sets or clears it; other commands succeed with 0. */
static void bsd_fcntl(IpcObject *s, IpcCall *c) {
    (void)s;
    int err, fd = bsd_in_fd(c), cmd = bsd_in32(c, 4, 0);
    BsdGuest *g = bsd_g();
    int h = bsdg_handle(g, fd);
    if (h < 0) {
        bsd_fail(c, HZ_EBADF);
        return;
    }
    if (cmd == 3) {
        bsd_reply(c, g->nb[fd] ? 4 : 0, 0);
        return;
    }
    if (cmd == 4) {
        int on = (bsd_in32(c, 8, 0) & 4) != 0;
        int r = bsdg_nonblock(g, fd, on, &err);
        if (r == 0) g->nb[fd] = (uint8_t)on;
        bsd_reply(c, r, err);
        return;
    }
    bsd_reply(c, 0, 0);
}
/* DuplicateSocket(fd) -> {new fd, errno}: a second descriptor for the same provider socket (bsdg_dup). */
static void bsd_dup(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    int r = bsdg_dup(bsd_g(), bsd_in_fd(c), &err);
    bsd_reply(c, r, err);
}
/* SendMMsg / RecvMMsg(fd, vlen, flags): the mmsghdr array is in buffer 0 (an input buffer for send, the output buffer for recv). Layout is FreeBSD's LP64 one
 * (msghdr 0x30 bytes: name@0 namelen@8 iov@0x10 iovlen@0x18 control@0x20 controllen@0x28 flags@0x2c, then u32 msg_len@0x30, 0x38 bytes per entry; iovec {base, len});
 * guest pointers are read through ipc_buf_ptr [unverified against a console: no SDK path was observed to use it]. Control data is ignored. */
#define MMSG_SIZE 0x38u
#define MMSG_MAX_VLEN 32u
#define MMSG_MAX_IOV 16u
#define MMSG_MAX_BYTES 0x20000u
static void *mmsg_ptr(uint64_t addr, uint64_t size) {
    IpcBuf b = {addr, size, 0};
    return addr && size ? ipc_buf_ptr(&b) : NULL;
}
static void mmsg_common(IpcCall *c, int recv) {
    int err = 0;
    const IpcBuf *b = recv ? ipc_out_buf(c, 0) : ipc_in_buf(c, 0);
    uint32_t vlen = (uint32_t)bsd_in32(c, 4, 0);
    int flags = bsd_in32(c, 8, 0), fd = bsd_in_fd(c);
    if (vlen > MMSG_MAX_VLEN) vlen = MMSG_MAX_VLEN;
    uint8_t *mm = b && b->size >= MMSG_SIZE ? ipc_buf_ptr(b) : NULL;
    if (mm && (uint64_t)vlen * MMSG_SIZE > b->size) vlen = (uint32_t)(b->size / MMSG_SIZE);
    if (!mm || !vlen) {
        bsd_reply(c, bsdg_handle(bsd_g(), fd) < 0 ? -1 : 0, bsdg_handle(bsd_g(), fd) < 0 ? HZ_EBADF : 0);
        return;
    }
    static __thread uint8_t buf[MMSG_MAX_BYTES];
    uint32_t done = 0;
    for (; done < vlen; done++) {
        uint8_t *h = mm + (size_t)done * MMSG_SIZE;
        uint64_t name_p, iov_p;
        uint32_t namelen;
        int32_t iovlen;
        memcpy(&name_p, h, 8), memcpy(&namelen, h + 8, 4), memcpy(&iov_p, h + 0x10, 8), memcpy(&iovlen, h + 0x18, 4);
        if (iovlen < 0 || (uint32_t)iovlen > MMSG_MAX_IOV) iovlen = (int32_t)MMSG_MAX_IOV;
        uint8_t *iov = iovlen ? mmsg_ptr(iov_p, (uint64_t)iovlen * 16) : NULL;
        size_t total = 0;
        struct {
            uint8_t *p;
            size_t n;
        } seg[MMSG_MAX_IOV];
        int nseg = 0;
        for (int i = 0; iov && i < iovlen && total < MMSG_MAX_BYTES; i++) {
            uint64_t base, len;
            memcpy(&base, iov + (size_t)i * 16, 8), memcpy(&len, iov + (size_t)i * 16 + 8, 8);
            if (len > MMSG_MAX_BYTES - total) len = MMSG_MAX_BYTES - total;
            uint8_t *p = len ? mmsg_ptr(base, len) : NULL;
            if (!p) continue;
            seg[nseg].p = p, seg[nseg].n = (size_t)len, nseg++;
            total += (size_t)len;
        }
        OnlineAddr peer;
        memset(&peer, 0, sizeof peer);
        int64_t r;
        uint8_t *name = namelen >= 8 ? mmsg_ptr(name_p, namelen) : NULL;
        if (recv) {
            /* only the first message may block: later ones return what is already queued */
            r = bsdg_recv(bsd_g(), fd, buf, total, done ? (flags | 0x80) : flags, name ? &peer : NULL, &err); /* 0x80 = MSG_DONTWAIT */
            if (r > 0) {
                size_t off = 0;
                for (int i = 0; i < nseg && off < (size_t)r; i++) {
                    size_t k = (size_t)r - off < seg[i].n ? (size_t)r - off : seg[i].n;
                    memcpy(seg[i].p, buf + off, k), off += k;
                }
                if (name) {
                    uint8_t enc[28];
                    size_t len = bsd_sockaddr_encode(&peer, enc, sizeof enc);
                    memcpy(name, enc, len < namelen ? len : namelen);
                    uint32_t nl = (uint32_t)len;
                    memcpy(h + 8, &nl, 4);
                }
            }
        } else {
            size_t off = 0;
            for (int i = 0; i < nseg; i++) memcpy(buf + off, seg[i].p, seg[i].n), off += seg[i].n;
            OnlineAddr to;
            r = bsdg_send(bsd_g(), fd, buf, total, flags, name && bsd_sockaddr_decode(name, namelen, &to) == 0 ? &to : NULL, &err);
        }
        if (r < 0) break;
        uint32_t ml = (uint32_t)r;
        memcpy(h + 0x30, &ml, 4);
    }
    if (done == 0) bsd_reply(c, -1, err);
    else bsd_reply(c, (int)done, 0); /* a failure after the first message reports the messages that went through */
}
static void bsd_recvmmsg(IpcObject *s, IpcCall *c) {
    (void)s;
    mmsg_common(c, 1);
}
static void bsd_sendmmsg(IpcObject *s, IpcCall *c) {
    (void)s;
    mmsg_common(c, 0);
}
/* Poll(nfds, timeout ms) with the pollfd array in an input buffer (s32 fd, s16 events, s16 revents) and the result array in an output buffer. */
static void bsd_poll(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0);
    const uint8_t *raw = in && in->size >= 8 ? ipc_buf_ptr(in) : NULL;
    unsigned n = raw ? (unsigned)(in->size / 8) : 0;
    if (c->in_size >= 4 && IPC_IN(c, uint32_t, 0) < n) n = IPC_IN(c, uint32_t, 0);
    if (!raw || !n) { /* Poll(0, timeout): waits out the timeout with a provider that has sockets (bsdg_poll), answers at once offline */
        OnlinePollFd none;
        int r = bsdg_poll(bsd_g(), &none, 0, bsd_in32(c, 4, 0), &err);
        bsd_reply(c, r < 0 ? -1 : 0, r < 0 ? err : 0);
        return;
    }
    if (n > BSD_MAX_FDS) n = BSD_MAX_FDS;
    OnlinePollFd fds[BSD_MAX_FDS];
    uint8_t res[BSD_MAX_FDS * 8];
    for (unsigned i = 0; i < n; i++) {
        int32_t fd;
        int16_t ev;
        memcpy(&fd, raw + i * 8, 4);
        memcpy(&ev, raw + i * 8 + 4, 2);
        fds[i] = (OnlinePollFd){fd, ev, 0};
    }
    int r = bsdg_poll(bsd_g(), fds, n, bsd_in32(c, 4, 0), &err);
    for (unsigned i = 0; i < n; i++) {
        memcpy(res + i * 8, raw + i * 8, 8);
        memcpy(res + i * 8 + 6, &fds[i].revents, 2);
    }
    if (out) ipc_write_buf(c, out, res, (uint64_t)n * 8);
    bsd_reply(c, r, err);
}
/* Select(nfds, timeout {s64 sec, s64 usec}) with the three fd_sets in input buffers and rewritten copies in output buffers [layout unverified]. */
static void bsd_select(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0, nfds = bsd_in32(c, 0, 0);
    if (nfds <= 0 || nfds > BSD_MAX_FDS) {
        bsd_reply(c, 0, 0);
        return;
    }
    uint8_t set[3][BSD_MAX_FDS / 8 + 8];
    size_t bytes = ((size_t)nfds + 7) / 8;
    int have[3] = {0};
    for (unsigned k = 0; k < 3; k++) {
        const IpcBuf *b = ipc_in_buf(c, k);
        const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
        memset(set[k], 0, sizeof set[k]);
        if (p) memcpy(set[k], p, bytes < (size_t)b->size ? bytes : (size_t)b->size), have[k] = 1;
    }
    int64_t sec = c->in_size >= 24 ? IPC_IN(c, int64_t, 8) : 0, usec = c->in_size >= 24 ? IPC_IN(c, int64_t, 16) : 0;
    int timeout = -1;
    if (sec >= 0 && usec >= 0) { /* guest values: no overflow, saturate at INT_MAX */
        int64_t ms = sec > 0x1fffff ? INT_MAX : sec * 1000 + usec / 1000;
        timeout = ms > INT_MAX ? INT_MAX : (int)ms;
    }
    int r = bsdg_select(bsd_g(), nfds, have[0] ? set[0] : NULL, have[1] ? set[1] : NULL, have[2] ? set[2] : NULL, timeout, &err);
    for (unsigned k = 0; k < 3; k++) {
        const IpcBuf *b = ipc_out_buf(c, k);
        if (b && have[k] && r >= 0) ipc_write_buf(c, b, set[k], bytes < (size_t)b->size ? bytes : (size_t)b->size);
    }
    bsd_reply(c, r, err);
}
/* 31 EventFd(u64 initval, u32 flags) / 39 NoSfEventFd (0x10 bytes in, 8 out per switchbrew; the same two arguments [unverified]) -> {s32 fd, errno}: an
 * event descriptor with eventfd(2) semantics, kept by the service (bsd_core.c): Read / Write / Recv / Send move the 64-bit counter, Poll / Select see it,
 * Fcntl / Ioctl(FIONBIO) switch it to non-blocking, Close frees it with its last DuplicateSocket alias. No provider is involved, so it works offline. */
static void bsd_eventfd(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    uint64_t initval = c->in_size >= 8 ? IPC_IN(c, uint64_t, 0) : 0;
    int fd = bsdg_eventfd(bsd_g(), initval, bsd_in32(c, 8, 0), &err);
    bsd_reply(c, fd, fd < 0 ? err : 0);
}
/* 40 ShutdownAllSockets(s32 how) [20.0.0+; the argument as libnx's bsdShutdownAllSockets(how) gives it for 23]: shutdown(how) on every socket of this
 * client through the provider; the descriptors stay open (23 keeps its older close-everything answer). */
static void bsd_shutdown_all_how(IpcObject *s, IpcCall *c) {
    (void)s;
    int err = 0;
    int r = bsdg_shutdown_all(bsd_g(), bsd_in32(c, 0, 0), &err);
    bsd_reply(c, r, err);
}
/* 200 SetThreadCoreMask(u64 mask) / 201 GetThreadCoreMask -> u64: the affinity of the service's worker threads, read back as set [layout unverified].
 * Before any Set it is core 3's bit, where the system modules run [unverified]. */
static uint64_t bsd_core_mask = 1u << 3;
static void bsd_set_core_mask(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size >= 8) __atomic_store_n(&bsd_core_mask, IPC_IN(c, uint64_t, 0), __ATOMIC_SEQ_CST);
}
static void bsd_get_core_mask(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = __atomic_load_n(&bsd_core_mask, __ATOMIC_SEQ_CST);
    IPC_OUT(c, v);
}
static const IpcCmd bsd_cmds[] = {
    {0, "RegisterClient", bsd_register_client, IPC_IMPL},
    {1, "StartMonitoring", bsd_start_monitoring, IPC_IMPL},
    {2, "Socket", bsd_socket_call, IPC_IMPL},
    {3, "SocketExempt", bsd_socket_call, IPC_IMPL},
    {4, "Open", bsd_enoent, IPC_IMPL} /* the guest has no device files: every path is ENOENT */,
    {5, "Select", bsd_select, IPC_IMPL},
    {6, "Poll", bsd_poll, IPC_IMPL},
    {7, "Sysctl", bsd_enoent, IPC_IMPL} /* no sysctl names exist: ENOENT */,
    {8, "Recv", bsd_recv, IPC_IMPL},
    {9, "RecvFrom", bsd_recvfrom, IPC_IMPL},
    {10, "Send", bsd_send, IPC_IMPL},
    {11, "SendTo", bsd_sendto, IPC_IMPL},
    {12, "Accept", bsd_accept, IPC_IMPL},
    {13, "Bind", bsd_bind, IPC_IMPL},
    {14, "Connect", bsd_connect, IPC_IMPL},
    {15, "GetPeerName", bsd_getpeername, IPC_IMPL},
    {16, "GetSockName", bsd_getsockname, IPC_IMPL},
    {17, "GetSockOpt", bsd_getsockopt, IPC_IMPL},
    {18, "Listen", bsd_listen, IPC_IMPL},
    {19, "Ioctl", bsd_ioctl, IPC_IMPL},
    {20, "Fcntl", bsd_fcntl, IPC_IMPL},
    {21, "SetSockOpt", bsd_setsockopt, IPC_IMPL},
    {22, "Shutdown", bsd_shutdown, IPC_IMPL},
    {23, "ShutdownAllSockets", bsd_shutdown_all, IPC_IMPL},
    {24, "Write", bsd_send, IPC_IMPL},
    {25, "Read", bsd_recv, IPC_IMPL},
    {26, "Close", bsd_close_call, IPC_IMPL},
    {27, "DuplicateSocket", bsd_dup, IPC_IMPL},
    {28, "GetResourceStatistics", bsd_resource_stats, IPC_IMPL},
    {29, "RecvMMsg", bsd_recvmmsg, IPC_IMPL},
    {30, "SendMMsg", bsd_sendmmsg, IPC_IMPL},
    {31, "EventFd", bsd_eventfd, IPC_IMPL},
    {32, "RegisterResourceStatisticsName", sysd_ok, IPC_STUB}, /* input layout not public, nothing reads the name back */
    {33, "RegisterClientShared", bsd_register_client, IPC_IMPL}, /* RegisterClient without the transfer memory handle (switchbrew) */
    {34, "GetSocketStatistics", sysd_err_generic, IPC_STUB}, /* statistics layout not public */
    {35, "NifIoctl", sysd_err_generic, IPC_STUB}, /* interface ioctls: no public layout */
    {37, "SetSocketBufferAttribute", sysd_ok, IPC_STUB}, /* attribute layout not public */
    {39, "NoSfEventFd", bsd_eventfd, IPC_IMPL},
    {40, "ShutdownAllSockets", bsd_shutdown_all_how, IPC_IMPL},
    {42, "SendMMsgNoMem", bsd_sendmmsg, IPC_IMPL}, /* SendMMsg's arguments [unverified] */
    {43, "RecvMMsgNoMem", bsd_recvmmsg, IPC_IMPL}, /* RecvMMsg's arguments [unverified] */
    {200, "SetThreadCoreMask", bsd_set_core_mask, IPC_IMPL},
    {201, "GetThreadCoreMask", bsd_get_core_mask, IPC_IMPL},
};
static IpcObject *bsd_open(void) { return IPC_OBJECT("bsd:u", bsd_cmds, NULL, NULL); }

/* nsd:u (nn::nsd): name service settings. Environment "lp1", names resolve to themselves (nothing is ever contacted). */
static void nsd_env_name(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) {
        char name[8] = "lp1";
        ipc_write_buf(c, b, name, b->size < sizeof name ? b->size : sizeof name);
    }
}
/* Resolve / GetNas*Fqdn: the name goes to the provider's nsd_resolve (the NullProvider returns it unchanged: nothing is contacted); a provider
 * without nsd support leaves the identity mapping. {u32 result} follows. */
static void nsd_resolve(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0);
    uint32_t r = 0;
    if (in && out) {
        const char *p = ipc_buf_ptr(in);
        if (p) {
            char name[256], res[256];
            size_t n = in->size < sizeof name - 1 ? (size_t)in->size : sizeof name - 1;
            memcpy(name, p, n);
            name[n] = 0;
            const OnlineProvider *pr = online_provider();
            int (*f)(void *, const char *, char *, size_t) = ONLINE_FN(pr, nsd_resolve);
            if (f && f(pr->ctx, name, res, sizeof res) == 0) {
                ipc_write_buf(c, out, res, strlen(res) + 1 < out->size ? strlen(res) + 1 : out->size);
            } else {
                ipc_write_buf(c, out, p, in->size < out->size ? in->size : out->size);
            }
        }
    }
    IPC_OUT(c, r);
}
/* GetDeviceId: a fixed zero id (the console has none to give; the NullProvider never uses it). */
static void nsd_device_id(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v[16] = {0};
    ipc_out(c, v, 16);
}
/* The imported settings blob: ImportSettings stores it (up to 0x100 bytes), DeleteSettings clears it. Nothing reads it back but the unit tests: the
 * environment name stays "lp1". */
static uint8_t nsd_import[0x100];
static uint32_t nsd_import_len;
static void nsd_import_settings(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    nsd_import_len = p ? (uint32_t)(b->size < sizeof nsd_import ? b->size : sizeof nsd_import) : 0;
    if (nsd_import_len) memcpy(nsd_import, p, nsd_import_len);
}
static void nsd_delete_settings(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    nsd_import_len = 0;
}
/* GetNasServiceSetting / GetCurrentSetting: struct answers with no service configured (zero-filled to the buffer; layouts unverified). */
static void nsd_zero_struct(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t z[0x1000] = {0};
    if (b && b->size) ipc_write_buf(c, b, z, b->size < sizeof z ? b->size : sizeof z);
}
/* 15 SetChangeEnvironmentIdentifierDisabled(bool) / 63 IsChangeEnvironmentIdentifierDisabled -> bool: whether the environment identifier may be changed (default no). */
static uint8_t nsd_change_env_disabled;
static void nsd_set_change_env_disabled(IpcObject *s, IpcCall *c) {
    (void)s;
    nsd_change_env_disabled = c->in_size >= 1 && IPC_IN(c, uint8_t, 0) != 0;
}
static void nsd_is_change_env_disabled(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = nsd_change_env_disabled;
    IPC_OUT(c, v);
}
static const IpcCmd nsd_cmds[] = {
    {10, "GetSettingName", nsd_env_name, IPC_IMPL},
    {11, "GetEnvironmentIdentifier", nsd_env_name, IPC_IMPL},
    {12, "GetDeviceId", nsd_device_id, IPC_IMPL},
    {13, "DeleteSettings", nsd_delete_settings, IPC_IMPL},
    {14, "ImportSettings", nsd_import_settings, IPC_IMPL},
    {20, "Resolve", nsd_resolve, IPC_IMPL},
    {21, "ResolveEx", nsd_resolve, IPC_IMPL},
    {30, "GetNasServiceSetting", nsd_zero_struct, IPC_IMPL},
    {31, "GetNasServiceSettingEx", nsd_zero_struct, IPC_IMPL},
    {40, "GetNasRequestFqdn", nsd_resolve, IPC_IMPL},
    {41, "GetNasRequestFqdnEx", nsd_resolve, IPC_IMPL},
    {42, "GetNasApiFqdn", nsd_resolve, IPC_IMPL},
    {43, "GetNasApiFqdnEx", nsd_resolve, IPC_IMPL},
    {50, "GetCurrentSetting", nsd_zero_struct, IPC_IMPL},
    {5, "GetSettingUrl", sysd_err_generic, IPC_STUB},
    {15, "SetChangeEnvironmentIdentifierDisabled", nsd_set_change_env_disabled, IPC_IMPL},
    {63, "IsChangeEnvironmentIdentifierDisabled", nsd_is_change_env_disabled, IPC_IMPL},
    {64, "SetWithoutDomainExchangeFqdns", sysd_ok, IPC_STUB},
    {100, "GetApplicationServerEnvironmentType", sysd_z4, IPC_STUB},
    {101, "SetApplicationServerEnvironmentType", sysd_ok, IPC_STUB},
    {102, "DeleteApplicationServerEnvironmentType", sysd_ok, IPC_STUB},
};
static IpcObject *nsd_open(void) { return IPC_OBJECT("nsd:u", nsd_cmds, NULL, NULL); }

/* csrng: cryptographically secure random bytes from the host. */
static void csrng_gen(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || !b->size) return;
    uint64_t n = b->size > 0x10000 ? 0x10000 : b->size;
    uint8_t *tmp = malloc((size_t)n);
    if (!tmp) return;
    arc4random_buf(tmp, (size_t)n);
    ipc_write_buf(c, b, tmp, n);
    free(tmp);
}
static const IpcCmd csrng_cmds[] = {{0, "GenerateRandomBytes", csrng_gen, IPC_IMPL}};
static IpcObject *csrng_open(void) { return IPC_OBJECT("csrng", csrng_cmds, NULL, NULL); }

/* nfp:user / nfc:user / nfc:mf:u (amiibo, NFC): a reader over an NfcTagSource (nfc_source.h). With no source installed there is no reader, exactly as
 * before: ListDevices reports no device, the device state is Unavailable and detection calls are accepted and do nothing. A source is installed by the
 * frontend (nfp_service_set_source, or the amiibo picker: amiibo_service_picker) or, headless, by NAPPLE_AMIIBO (amiibo_lib_core.h). The state machine
 * and the structures are in nfp_core.c; command ids and argument offsets are the author's memory of the public documentation (unverified). */
static pthread_mutex_t nfp_lock = PTHREAD_MUTEX_INITIALIZER;
static NfcTagSource nfp_src;
static int nfp_have_src, nfp_env_checked, nfp_script_started;
static AmiiboPicker nfp_picker;
static int nfp_picker_ready;
static AmiiboScript nfp_script;
typedef struct {
    NfpDevice dev;
    uint32_t w[3][8]; /* writable ends of the activate, deactivate and availability events handed out so far */
    unsigned nw[3];
    uint8_t keep_session; /* nfc:user KeepPassThroughSession */
    uint8_t system;       /* handed out by nfc:sys CreateSystemInterface (the firmware applets' view of the reader) */
} NfpSvc;
static NfpSvc *nfp_svcs[8];

static void nfp_signal(NfpSvc *s, uint32_t ev) { /* caller holds nfp_lock */
    for (int k = 0; k < 2; k++)
        if (ev & (1u << k))
            for (unsigned i = 0; i < s->nw[k]; i++) event_signal_handle(s->w[k][i]);
}
static void nfp_on_change(void *unused) {
    (void)unused;
    pthread_mutex_lock(&nfp_lock);
    for (unsigned i = 0; i < 8; i++)
        if (nfp_svcs[i]) {
            nfp_poll(&nfp_svcs[i]->dev);
            nfp_signal(nfp_svcs[i], nfp_take_events(&nfp_svcs[i]->dev));
        }
    pthread_mutex_unlock(&nfp_lock);
}
void nfp_service_set_source(const NfcTagSource *src) {
    pthread_mutex_lock(&nfp_lock);
    nfp_env_checked = 1; /* an explicit source wins over the environment */
    nfp_have_src = src != NULL;
    if (src) nfp_src = *src;
    for (unsigned i = 0; i < 8; i++)
        if (nfp_svcs[i]) nfp_set_source(&nfp_svcs[i]->dev, src);
    pthread_mutex_unlock(&nfp_lock);
    if (src && src->set_listener) src->set_listener(src->ctx, nfp_on_change, NULL);
}
AmiiboPicker *amiibo_service_picker(void) {
    pthread_mutex_lock(&nfp_lock);
    if (!nfp_picker_ready) {
        AmiiboConfig cfg;
        amiibo_config_default(&cfg, napple_support_dir() ? napple_support_dir() : ".");
        amiibo_picker_init(&nfp_picker, &cfg);
        nfp_picker_ready = 1;
    }
    pthread_mutex_unlock(&nfp_lock);
    return &nfp_picker;
}
/* headless: NAPPLE_AMIIBO installs the picker as the reader and remembers the script */
static void nfp_ensure_source(void) {
    pthread_mutex_lock(&nfp_lock);
    int need = !nfp_env_checked;
    nfp_env_checked = 1;
    pthread_mutex_unlock(&nfp_lock);
    if (!need) return;
    const char *spec = getenv("NAPPLE_AMIIBO");
    if (!spec || !*spec || nfp_have_src) return;
    AmiiboPicker *p = amiibo_service_picker();
    pthread_mutex_lock(&nfp_lock);
    if (amiibo_script_parse(spec, &nfp_script) != 0) nfp_script.valid = 0;
    pthread_mutex_unlock(&nfp_lock);
    nfp_service_set_source(amiibo_picker_source(p));
}
static void *nfp_script_thread(void *arg) {
    (void)arg;
    AmiiboScript s = nfp_script;
    struct timespec ts = {s.insert_ms / 1000, (long)(s.insert_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    int rc = amiibo_script_apply(&nfp_picker, &s);
    LOG("nfp", "NAPPLE_AMIIBO: amiibo %s on the reader%s%s", rc ? "NOT placed" : "placed", rc ? ": " : "", rc ? nfp_picker.last_error : "");
    if (s.remove_ms) {
        ts = (struct timespec){s.remove_ms / 1000, (long)(s.remove_ms % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        amiibo_picker_remove(&nfp_picker);
        LOG("nfp", "NAPPLE_AMIIBO: amiibo removed from the reader");
    }
    return NULL;
}
static void nfp_run_script(void) { /* on the first StartDetection */
    pthread_mutex_lock(&nfp_lock);
    int run = nfp_script.valid && !nfp_script_started;
    nfp_script_started = 1;
    AmiiboScript s = nfp_script;
    pthread_mutex_unlock(&nfp_lock);
    if (!run) return;
    if (s.insert_ms == 0 && s.remove_ms == 0) {
        if (amiibo_script_apply(&nfp_picker, &s)) LOG("nfp", "NAPPLE_AMIIBO: amiibo NOT placed: %s", nfp_picker.last_error);
        else LOG("nfp", "NAPPLE_AMIIBO: amiibo placed on the reader");
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, nfp_script_thread, NULL) == 0) pthread_detach(t);
}

static void nfp_svc_destroy(IpcObject *o) {
    NfpSvc *s = o->state;
    pthread_mutex_lock(&nfp_lock);
    for (unsigned i = 0; i < 8; i++)
        if (nfp_svcs[i] == s) nfp_svcs[i] = NULL;
    pthread_mutex_unlock(&nfp_lock);
    free(s);
}
static NfpSvc *nfp_svc_new(void) {
    nfp_ensure_source();
    NfpSvc *s = calloc(1, sizeof *s);
    nfp_device_init(&s->dev);
    pthread_mutex_lock(&nfp_lock);
    if (nfp_have_src) nfp_set_source(&s->dev, &nfp_src);
    for (unsigned i = 0; i < 8; i++)
        if (!nfp_svcs[i]) {
            nfp_svcs[i] = s;
            break;
        }
    pthread_mutex_unlock(&nfp_lock);
    return s;
}
/* run `op` on the device under the lock, then signal the events it raised */
#define NFP_LOCKED(s, body)                                                                                                                                    \
    do {                                                                                                                                                       \
        pthread_mutex_lock(&nfp_lock);                                                                                                                         \
        body;                                                                                                                                                  \
        nfp_signal(s, nfp_take_events(&(s)->dev));                                                                                                             \
        pthread_mutex_unlock(&nfp_lock);                                                                                                                       \
    } while (0)
static int nfp_handle_ok(const IpcCall *c) { return c->in_size < 8 || IPC_IN(c, uint64_t, 0) == NFP_DEVICE_HANDLE; }
#define NFP_CHECK_HANDLE(c)                                                                                                                                    \
    if (!nfp_handle_ok(c)) {                                                                                                                                   \
        (c)->result = NFP_R_DEVICE_NOT_FOUND;                                                                                                                  \
        return;                                                                                                                                                \
    }
#define NFP_SVC(s) NfpSvc *svc = (s)->state
#define NFP_NO_READER (!svc->dev.have_src)

static void nfp_initialize_call(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    uint32_t r;
    NFP_LOCKED(svc, r = nfp_initialize(&svc->dev, g_program_id));
    if (r && r != NFP_R_WRONG_DEVICE_STATE) c->result = r; /* a second Initialize is accepted: the stub never complained */
}
static void nfp_finalize_call(IpcObject *s, IpcCall *c) {
    (void)c;
    NFP_SVC(s);
    NFP_LOCKED(svc, nfp_finalize(&svc->dev));
}
static void nfp_list_devices(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    int32_t n = svc->dev.have_src && svc->dev.initialized ? 1 : 0;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (n && b && b->size >= 8) {
        uint64_t h = NFP_DEVICE_HANDLE;
        ipc_write_buf(c, b, &h, 8);
    }
    IPC_OUT(c, n);
}
static void nfp_start(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    if (NFP_NO_READER) return;
    nfp_run_script();
    uint32_t r;
    NFP_LOCKED(svc, r = nfp_start_detection(&svc->dev));
    c->result = r;
}
static void nfp_stop(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    if (NFP_NO_READER) return;
    uint32_t r;
    NFP_LOCKED(svc, r = nfp_stop_detection(&svc->dev));
    c->result = r;
}
static void nfp_mount_call(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    if (NFP_NO_READER) return;
    uint32_t r, mt = c->in_size >= 12 ? IPC_IN(c, uint32_t, 8) : 0, tg = c->in_size >= 16 ? IPC_IN(c, uint32_t, 12) : 0;
    NFP_LOCKED(svc, r = nfp_mount(&svc->dev, mt, tg));
    c->result = r;
}
static void nfp_unmount_call(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    if (NFP_NO_READER) return;
    uint32_t r;
    NFP_LOCKED(svc, r = nfp_unmount(&svc->dev));
    c->result = r;
}
static void nfp_flush_call(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    uint32_t r;
    NFP_LOCKED(svc, r = nfp_flush(&svc->dev));
    c->result = r;
}
static void nfp_restore_call(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    uint32_t r;
    NFP_LOCKED(svc, r = nfp_restore(&svc->dev));
    c->result = r;
}
/* the info structures come back in an output buffer */
#define NFP_INFO_CALL(name, fn, size)                                                                                                                          \
    static void name(IpcObject *s, IpcCall *c) {                                                                                                               \
        NFP_SVC(s);                                                                                                                                            \
        NFP_CHECK_HANDLE(c);                                                                                                                                   \
        uint8_t out[size];                                                                                                                                     \
        uint32_t r;                                                                                                                                            \
        NFP_LOCKED(svc, r = fn(&svc->dev, out));                                                                                                               \
        const IpcBuf *b = ipc_out_buf(c, 0);                                                                                                                   \
        if (!r && b) ipc_write_buf(c, b, out, size);                                                                                                           \
        c->result = r;                                                                                                                                         \
    }
NFP_INFO_CALL(nfp_tag_info_call, nfp_get_tag_info, NFP_TAG_INFO_SIZE)
NFP_INFO_CALL(nfp_register_info_call, nfp_get_register_info, NFP_REGISTER_INFO_SIZE)
NFP_INFO_CALL(nfp_common_info_call, nfp_get_common_info, NFP_COMMON_INFO_SIZE)
NFP_INFO_CALL(nfp_model_info_call, nfp_get_model_info, NFP_MODEL_INFO_SIZE)

static void nfp_attach_event(NfpSvc *svc, IpcCall *c, int kind) {
    uint32_t r = 0, w = event_create(&r);
    pthread_mutex_lock(&nfp_lock);
    if (svc->nw[kind] < 8) svc->w[kind][svc->nw[kind]++] = w;
    pthread_mutex_unlock(&nfp_lock);
    ipc_out_copy_handle(c, r);
}
static void nfp_attach_activate(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    nfp_attach_event(svc, c, 0);
}
static void nfp_attach_deactivate(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    nfp_attach_event(svc, c, 1);
}
static void nfp_attach_avail(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    nfp_attach_event(svc, c, 2);
}
static void nfp_get_state_call(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    uint32_t v = (uint32_t)nfp_state(&svc->dev);
    IPC_OUT(c, v);
}
static void nfp_get_device_state(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    uint32_t v;
    if (NFP_NO_READER) v = NFP_DS_UNAVAILABLE;
    else NFP_LOCKED(svc, v = (uint32_t)nfp_device_state(&svc->dev));
    IPC_OUT(c, v);
}
static void nfp_get_npad_id(IpcObject *s, IpcCall *c) {
    (void)s;
    NFP_CHECK_HANDLE(c);
    uint32_t v = 0; /* Player 1 */
    IPC_OUT(c, v);
}
static void nfp_area_open_call(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    uint32_t r, id = c->in_size >= 12 ? IPC_IN(c, uint32_t, 8) : 0;
    NFP_LOCKED(svc, r = nfp_area_open(&svc->dev, id));
    c->result = r;
}
static void nfp_area_size_call(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    uint32_t r, v = 0;
    NFP_LOCKED(svc, r = nfp_area_size(&svc->dev, &v));
    if (r) c->result = r;
    else IPC_OUT(c, v);
}
static void nfp_area_get(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t tmp[AMIIBO_APP_AREA_SIZE];
    size_t n = 0;
    uint32_t r;
    NFP_LOCKED(svc, r = nfp_area_read(&svc->dev, tmp, b ? (size_t)b->size : 0, &n));
    if (r) {
        c->result = r;
        return;
    }
    if (b) ipc_write_buf(c, b, tmp, n);
    uint32_t v = (uint32_t)n;
    IPC_OUT(c, v);
}
static void nfp_area_set(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    const IpcBuf *b = ipc_in_buf(c, 0);
    const void *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    uint32_t r;
    NFP_LOCKED(svc, r = nfp_area_write(&svc->dev, p ? p : "", p ? (size_t)b->size : 0));
    c->result = r;
}
static void nfp_area_create_call(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    const IpcBuf *b = ipc_in_buf(c, 0);
    const void *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    uint32_t r, id = c->in_size >= 12 ? IPC_IN(c, uint32_t, 8) : 0;
    NFP_LOCKED(svc, r = nfp_area_create(&svc->dev, id, p ? p : "", p ? (size_t)b->size : 0));
    c->result = r;
}
static void nfp_area_recreate_call(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    NFP_CHECK_HANDLE(c);
    const IpcBuf *b = ipc_in_buf(c, 0);
    const void *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    uint32_t r, id = c->in_size >= 12 ? IPC_IN(c, uint32_t, 8) : 0;
    NFP_LOCKED(svc, r = nfp_area_recreate(&svc->dev, id, p ? p : "", p ? (size_t)b->size : 0));
    c->result = r;
}
static const IpcCmd nfp_user_cmds[] = {
    {0, "Initialize", nfp_initialize_call, IPC_IMPL},
    {1, "Finalize", nfp_finalize_call, IPC_IMPL},
    {2, "ListDevices", nfp_list_devices, IPC_IMPL},
    {3, "StartDetection", nfp_start, IPC_IMPL},
    {4, "StopDetection", nfp_stop, IPC_IMPL},
    {5, "Mount", nfp_mount_call, IPC_IMPL},
    {6, "Unmount", nfp_unmount_call, IPC_IMPL},
    {7, "OpenApplicationArea", nfp_area_open_call, IPC_IMPL},
    {8, "GetApplicationArea", nfp_area_get, IPC_IMPL},
    {9, "SetApplicationArea", nfp_area_set, IPC_IMPL},
    {10, "Flush", nfp_flush_call, IPC_IMPL},
    {11, "Restore", nfp_restore_call, IPC_IMPL},
    {12, "CreateApplicationArea", nfp_area_create_call, IPC_IMPL},
    {13, "GetTagInfo", nfp_tag_info_call, IPC_IMPL},
    {14, "GetRegisterInfo", nfp_register_info_call, IPC_IMPL},
    {15, "GetCommonInfo", nfp_common_info_call, IPC_IMPL},
    {16, "GetModelInfo", nfp_model_info_call, IPC_IMPL},
    {17, "AttachActivateEvent", nfp_attach_activate, IPC_IMPL},
    {18, "AttachDeactivateEvent", nfp_attach_deactivate, IPC_IMPL},
    {19, "GetState", nfp_get_state_call, IPC_IMPL},
    {20, "GetDeviceState", nfp_get_device_state, IPC_IMPL},
    {21, "GetNpadId", nfp_get_npad_id, IPC_IMPL},
    {22, "GetApplicationAreaSize", nfp_area_size_call, IPC_IMPL},
    {23, "AttachAvailabilityChangeEvent", nfp_attach_avail, IPC_IMPL},
    {24, "RecreateApplicationArea", nfp_area_recreate_call, IPC_IMPL},
};
static void nfp_create_user(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("INfpUser", nfp_user_cmds, nfp_svc_new(), nfp_svc_destroy));
}
static const IpcCmd nfp_cmds[] = {{0, "CreateUserInterface", nfp_create_user, IPC_IMPL}};

/* nfc:user INfc: 0 Initialize, 1 Finalize, 2 GetState (1 = initialized), 3 IsNfcEnabled, 100 ListDevices, 101 GetDeviceState, 102 GetNpadId, 103 AttachAvailabilityChangeEvent,
 * 104 StartDetection, 105 StopDetection, 106 GetTagInfo, 107/108 activate/deactivate events, 400 SendCommandByPassThrough (type 2 READ, FAST_READ, GET_VERSION over the
 * tag memory), 401/402 pass-through session keep/release. */
static void nfc_get_state_call(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    uint32_t v = svc->dev.initialized ? 1 : 0;
    if (!v && !svc->dev.have_src) v = 1; /* the stub always reported 1; with no reader nothing can differ */
    IPC_OUT(c, v);
}
static void nfc_is_enabled(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    uint8_t v = svc->dev.have_src;
    IPC_OUT(c, v);
}
static void nfc_pass_through(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0);
    const uint8_t *cmd = in && in->size ? ipc_buf_ptr(in) : NULL;
    uint8_t resp[0x100];
    long n = -1;
    if (!nfp_handle_ok(c) || !cmd) {
        if (!svc->system) {
            c->result = NFP_R_DEVICE_NOT_FOUND;
            return;
        }
        /* The Mii Maker probes the console's reader with a pass-through on the system interface right after CreateSystemInterface, before any ListDevices
         * or tag: the console answers it, so an empty success keeps the applet going (an error here is fatal to it: ThrowFatal 2115-0064, seen 2026-10-27). */
        LOG("nfp", "nfc:sys pass-through before a device is selected: answered empty");
        n = 0;
    } else NFP_LOCKED(svc, n = nfc_passthrough(&svc->dev, cmd, (size_t)in->size, resp, sizeof resp));
    if (n < 0) {
        c->result = NFP_R_WRONG_DEVICE_STATE;
        return;
    }
    if (out) ipc_write_buf(c, out, resp, (uint64_t)n);
    uint32_t v = (uint32_t)n;
    IPC_OUT(c, v);
}
/* Keep / ReleasePassThroughSession bracket a run of pass-through commands: the session flag lives in the per-object service state (the tag stays selected
 * while it is set; with the file-backed tag source nothing else depends on it). */
static void nfc_keep_session(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    (void)c;
    svc->keep_session = 1;
}
static void nfc_release_session(IpcObject *s, IpcCall *c) {
    NFP_SVC(s);
    (void)c;
    svc->keep_session = 0;
}
static const IpcCmd nfc_user_cmds[] = {
    {0, "Initialize", nfp_initialize_call, IPC_IMPL},
    {1, "Finalize", nfp_finalize_call, IPC_IMPL},
    {2, "GetState", nfc_get_state_call, IPC_IMPL},
    {3, "IsNfcEnabled", nfc_is_enabled, IPC_IMPL},
    {100, "ListDevices", nfp_list_devices, IPC_IMPL},
    {101, "GetDeviceState", nfp_get_device_state, IPC_IMPL},
    {102, "GetNpadId", nfp_get_npad_id, IPC_IMPL},
    {103, "AttachAvailabilityChangeEvent", nfp_attach_avail, IPC_IMPL},
    {104, "StartDetection", nfp_start, IPC_IMPL},
    {105, "StopDetection", nfp_stop, IPC_IMPL},
    {106, "GetTagInfo", nfp_tag_info_call, IPC_IMPL},
    {107, "AttachActivateEvent", nfp_attach_activate, IPC_IMPL},
    {108, "AttachDeactivateEvent", nfp_attach_deactivate, IPC_IMPL},
    {400, "SendCommandByPassThrough", nfc_pass_through, IPC_IMPL},
    {401, "KeepPassThroughSession", nfc_keep_session, IPC_IMPL},
    {402, "ReleasePassThroughSession", nfc_release_session, IPC_IMPL},
};
static void nfc_create_user(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("INfc", nfc_user_cmds, nfp_svc_new(), nfp_svc_destroy));
}
static const IpcCmd nfc_cmds[] = {{0, "CreateUserInterface", nfc_create_user, IPC_IMPL}};
static IpcObject *nfc_open(void) { return IPC_OBJECT("nfc:user", nfc_cmds, NULL, NULL); }
static IpcObject *nfp_open(void) { return IPC_OBJECT("nfp:user", nfp_cmds, NULL, NULL); }
/* nfc:sys / nfp:sys (the firmware's NFC applets): command 0 CreateSystemInterface hands out the same simulated interfaces as the user services (the system interfaces
 * add only commands a title never calls). */
static void nfc_create_sys(IpcObject *s, IpcCall *c) {
    (void)s;
    NfpSvc *svc = nfp_svc_new();
    svc->system = 1;
    ipc_out_object(c, IPC_OBJECT("INfcSystem", nfc_user_cmds, svc, nfp_svc_destroy));
}
static const IpcCmd nfp_sys_cmds[] = {{0, "CreateSystemInterface", nfp_create_user, IPC_IMPL}};
static const IpcCmd nfc_sys_cmds[] = {{0, "CreateSystemInterface", nfc_create_sys, IPC_IMPL}};
static IpcObject *nfp_sys_open(void) { return IPC_OBJECT("nfp:sys", nfp_sys_cmds, NULL, NULL); }
static IpcObject *nfc_sys_open(void) { return IPC_OBJECT("nfc:sys", nfc_sys_cmds, NULL, NULL); }

/* nfc:mf:u IMifare: the reader calls work like nfc:user's (ListDevices, StartDetection, StopDetection, GetTagInfo, events, states). Read / Write of Mifare
 * Classic blocks cannot be served by an NTAG215 amiibo tag: they report the tag as not a Mifare card. */
static void mf_not_supported(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = NFP_R_WRONG_DEVICE_STATE;
}
static const IpcCmd mifare_cmds[] = {
    {0, "Initialize", nfp_initialize_call, IPC_IMPL},
    {1, "Finalize", nfp_finalize_call, IPC_IMPL},
    {2, "ListDevices", nfp_list_devices, IPC_IMPL},
    {3, "StartDetection", nfp_start, IPC_IMPL},
    {4, "StopDetection", nfp_stop, IPC_IMPL},
    {5, "Read", mf_not_supported, IPC_IMPL},
    {6, "Write", mf_not_supported, IPC_IMPL},
    {7, "GetTagInfo", nfp_tag_info_call, IPC_IMPL},
    {8, "GetActivateEventHandle", nfp_attach_activate, IPC_IMPL},
    {9, "GetDeactivateEventHandle", nfp_attach_deactivate, IPC_IMPL},
    {10, "GetState", nfc_get_state_call, IPC_IMPL},
    {11, "GetDeviceState", nfp_get_device_state, IPC_IMPL},
    {12, "GetNpadId", nfp_get_npad_id, IPC_IMPL},
    {13, "GetAvailabilityChangeEventHandle", nfp_attach_avail, IPC_IMPL},
};
static void mifare_create_user(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IMifare", mifare_cmds, nfp_svc_new(), nfp_svc_destroy));
}
static const IpcCmd mifare_root_cmds[] = {{0, "CreateUserInterface", mifare_create_user, IPC_IMPL}};
static IpcObject *mifare_open(void) { return IPC_OBJECT("nfc:mf:u", mifare_root_cmds, NULL, NULL); }

/* prepo:u (play reports): reports are accepted and dropped. */
static void ret_u64_1(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = 1;
    IPC_OUT(c, v);
}
/* The report payload (event id + a serialised buffer) goes to the provider's report hook; the NullProvider has none, so reports are dropped. Every accepted
 * report is counted in the store (offline_state_core.c) so GetStorageUsage / ClearStorage and the transmission status mean something: nothing is ever
 * transmitted, so the status stays 0 ("nothing pending to send"). */
static PrepoStore prepo_store;
static int prepo_store_ready;
static pthread_mutex_t prepo_lock = PTHREAD_MUTEX_INITIALIZER;
static PrepoStore *prepo_st(void) { /* caller holds prepo_lock */
    if (!prepo_store_ready) prepo_init(&prepo_store), prepo_store_ready = 1;
    return &prepo_store;
}
static void prepo_deliver(IpcCall *c, unsigned buf_index) {
    const OnlineProvider *p = online_provider();
    void (*rep)(void *, const void *, size_t) = ONLINE_FN(p, report);
    const IpcBuf *b = ipc_in_buf(c, buf_index);
    if (!b) b = ipc_in_buf(c, 0);
    const void *d = b && b->size ? ipc_buf_ptr(b) : NULL;
    pthread_mutex_lock(&prepo_lock);
    prepo_account(prepo_st(), d ? b->size : 0);
    pthread_mutex_unlock(&prepo_lock);
    if (rep && d) rep(p->ctx, d, (size_t)b->size);
}
static void prepo_save(IpcObject *s, IpcCall *c) {
    (void)s;
    prepo_deliver(c, 1);
}
static void prepo_save_system(IpcObject *s, IpcCall *c) {
    (void)s;
    prepo_deliver(c, 0);
}
static void prepo_storage_usage(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&prepo_lock);
    uint64_t v[2] = {prepo_st()->bytes, PREPO_STORAGE_LIMIT}; /* {used, capacity} */
    pthread_mutex_unlock(&prepo_lock);
    ipc_out(c, v, 16);
}
static void prepo_clear_storage(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    pthread_mutex_lock(&prepo_lock);
    prepo_clear(prepo_st());
    pthread_mutex_unlock(&prepo_lock);
}
static void prepo_get_agreement(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&prepo_lock);
    uint8_t v = prepo_st()->agreement_check;
    pthread_mutex_unlock(&prepo_lock);
    IPC_OUT(c, v);
}
static void prepo_set_agreement(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&prepo_lock);
    prepo_st()->agreement_check = c->in_size >= 1 && IPC_IN(c, uint8_t, 0) ? 1 : 0;
    pthread_mutex_unlock(&prepo_lock);
}
/* 20200 SetOperationMode(u32): the report store's operation mode, kept for the session (0 normal, the default). */
static uint32_t prepo_operation_mode;
static void prepo_set_operation_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&prepo_lock);
    if (c->in_size >= 4) prepo_operation_mode = IPC_IN(c, uint32_t, 0);
    pthread_mutex_unlock(&prepo_lock);
}
/* ids per switchbrew 'BCAT services' (nn::prepo::detail::ipc::IPrepoService): the SaveReport / SaveReportWithUser pairs were renumbered three times (10100 / 10101 up to
 * 5.1.0, 10102 / 10103 up to 9.2.0, 10104 / 10105 up to 20.5.0, 10106 / 10107 from 21.0.0); all are accepted and counted in the report storage, none is kept. */
static const IpcCmd prepo_cmds[] = {
    {10100, "SaveReportOld", prepo_save, IPC_IMPL},
    {10101, "SaveReportWithUserOld", prepo_save, IPC_IMPL},
    {10102, "SaveReportOld2", prepo_save, IPC_IMPL},
    {10103, "SaveReportWithUserOld2", prepo_save, IPC_IMPL},
    {10104, "SaveReportOld3", prepo_save, IPC_IMPL},
    {10105, "SaveReportWithUserOld3", prepo_save, IPC_IMPL}, /* Animal Crossing 3.0.3 (SDK 20) reports with 10105 */
    {10106, "SaveReport", prepo_save, IPC_IMPL},
    {10107, "SaveReportWithUser", prepo_save, IPC_IMPL},
    {10200, "RequestImmediateTransmission", noop, IPC_IMPL}, /* offline: queued reports stay queued */
    {10300, "GetTransmissionStatus", ret_u32_0, IPC_IMPL},
    {10400, "GetSystemSessionId", ret_u64_1, IPC_IMPL},
    {20100, "SaveSystemReportOld", prepo_save_system, IPC_IMPL},
    {20101, "SaveSystemReportWithUserOld", prepo_save_system, IPC_IMPL},
    {20102, "SaveSystemReport", prepo_save_system, IPC_IMPL},
    {20103, "SaveSystemReportWithUser", prepo_save_system, IPC_IMPL},
    {20200, "SetOperationMode", prepo_set_operation_mode, IPC_IMPL},
    {30100, "ClearStorage", prepo_clear_storage, IPC_IMPL},
    {30300, "GetStorageUsage", prepo_storage_usage, IPC_IMPL},
    {40100, "IsUserAgreementCheckEnabled", prepo_get_agreement, IPC_IMPL},
    {40101, "SetUserAgreementCheckEnabled", prepo_set_agreement, IPC_IMPL},
    {50100, "ReadAllApplicationReportFiles", sysd_err_generic, IPC_STUB},
    {90100, "ReadAllReportFiles", sysd_err_generic, IPC_STUB}, /* reports are not kept, so there is nothing to read back */
};
static IpcObject *prepo_open(void) { return IPC_OBJECT("prepo:u", prepo_cmds, NULL, NULL); }

/* ssl (nn::ssl): contexts and connections can be created and configured. Handshake and I/O go through the active OnlineProvider's TLS entry points over
 * the bsd descriptor the connection was given (bsd_core.c maps it to a provider socket). With the NullProvider no handshake can complete (there is no
 * socket peer), so DoHandshake reports "not connected". Commands past 8 are the author's memory of the public description (unverified). */
typedef struct SslCtx {
    uint32_t refs, nconn, options[8], next_pki;
    uint64_t pki[8]; /* ids handed out by ImportServerPki / ImportClientPki and not yet removed */
    unsigned npki;
    uint8_t *ca[8]; /* ImportServerPki: the certificate of pki[i] (NULL for client PKI), handed to the provider as a trusted CA at the handshake */
    uint32_t ca_n[8];
} SslCtx;                     /* shared by the context object and its connections (refcounted so a connection can outlive its context object) */
static uint32_t ssl_contexts; /* live ISslContext objects, for GetContextCount */
static uint32_t ssl_interface_version;
static void ssl_ctx_unref(SslCtx *x) {
    if (x && --x->refs == 0) {
        for (unsigned i = 0; i < x->npki; i++) free(x->ca[i]);
        free(x);
    }
}
typedef struct {
    SslCtx *ctx;
    int32_t fd;
    char host[256];
    uint32_t verify, io_mode, cache_mode, io_timeout, renegotiation_mode;
    void *conn;
    int established;
    int verify_set; /* SetVerifyOption was called: the provider gets that option instead of its own default (every check) */
} SslConn;
static void ssl_conn_destroy(IpcObject *o) {
    SslConn *k = o->state;
    const OnlineProvider *p = online_provider();
    void (*fr)(void *, void *) = ONLINE_FN(p, tls_free);
    if (k->conn && fr) fr(p->ctx, k->conn);
    if (k->ctx) {
        if (k->ctx->nconn) k->ctx->nconn--;
        ssl_ctx_unref(k->ctx);
    }
    free(k);
}
static void ssl_set_fd(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    k->fd = c->in_size >= 4 ? IPC_IN(c, int32_t, 0) : -1;
    int32_t v = k->fd;
    IPC_OUT(c, v);
}
static void ssl_get_fd(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    int32_t v = k->fd;
    IPC_OUT(c, v);
}
static void ssl_set_host(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const char *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    size_t n = p ? (size_t)(b->size < sizeof k->host - 1 ? b->size : sizeof k->host - 1) : 0;
    if (p) memcpy(k->host, p, n);
    k->host[n] = 0;
}
static void ssl_set_verify(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    if (c->in_size >= 4) k->verify = IPC_IN(c, uint32_t, 0), k->verify_set = 1;
}
static void ssl_set_io_mode(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    if (c->in_size >= 4) k->io_mode = IPC_IN(c, uint32_t, 0);
}
static void ssl_get_verify(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    IPC_OUT(c, k->verify);
}
static void ssl_get_io_mode(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    IPC_OUT(c, k->io_mode);
}
static void ssl_get_host_len(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    uint32_t v = (uint32_t)strlen(k->host);
    IPC_OUT(c, v);
}
static void ssl_set_cache(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    if (c->in_size >= 4) k->cache_mode = IPC_IN(c, uint32_t, 0);
}
static void ssl_get_cache(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    IPC_OUT(c, k->cache_mode);
}
/* 34 SetIoTimeout(u32 milliseconds) / 35 GetIoTimeout -> u32: the connection's I/O timeout (0 = none, the default), kept per connection. */
static void ssl_set_reneg(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    if (c->in_size >= 4) k->renegotiation_mode = IPC_IN(c, uint32_t, 0);
}
static void ssl_get_reneg(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    IPC_OUT(c, k->renegotiation_mode);
}
static void ssl_set_io_timeout(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    if (c->in_size >= 4) k->io_timeout = IPC_IN(c, uint32_t, 0);
}
static void ssl_get_io_timeout(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    IPC_OUT(c, k->io_timeout);
}
static void ssl_handshake(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const OnlineProvider *p = online_provider();
    void *(*mk)(void *, int) = ONLINE_FN(p, tls_new);
    int (*hs)(void *, void *, uint32_t *) = ONLINE_FN(p, tls_handshake);
    int (*sh)(void *, void *, const char *) = ONLINE_FN(p, tls_set_host);
    int h = bsdg_handle(bsdg_global(), k->fd);
    if (!mk || !hs || h < 0) {
        c->result = HZ_R_SSL_NOT_CONNECTED; /* no socket can be connected, so no handshake can run */
        return;
    }
    int fresh = !k->conn;
    if (!k->conn) k->conn = mk(p->ctx, h);
    if (!k->conn) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    if (fresh) { /* the context's imported server CAs and the guest's verify option (providers without these entry points keep their own policy) */
        int (*ca)(void *, void *, const void *, size_t) = ONLINE_FN(p, tls_add_ca);
        int (*vf)(void *, void *, uint32_t) = ONLINE_FN(p, tls_set_verify);
        for (unsigned i = 0; ca && k->ctx && i < k->ctx->npki; i++)
            if (k->ctx->ca[i]) ca(p->ctx, k->conn, k->ctx->ca[i], k->ctx->ca_n[i]);
        if (vf && k->verify_set) vf(p->ctx, k->conn, k->verify);
    }
    if (sh && k->host[0]) sh(p->ctx, k->conn, k->host);
    uint32_t res = 0;
    if (hs(p->ctx, k->conn, &res) != 0) {
        c->result = res ? res : HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    k->established = 1;
}
static void ssl_read(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const OnlineProvider *p = online_provider();
    int64_t (*rd)(void *, void *, void *, size_t, int *) = ONLINE_FN(p, tls_read);
    const IpcBuf *b = ipc_out_buf(c, 0);
    int err = 0;
    if (!k->established || !rd || !b || !b->size) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    static __thread uint8_t buf[0x10000];
    int64_t r = rd(p->ctx, k->conn, buf, b->size < sizeof buf ? (size_t)b->size : sizeof buf, &err);
    if (r < 0) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    ipc_write_buf(c, b, buf, (uint64_t)r);
    uint32_t n = (uint32_t)r;
    IPC_OUT(c, n);
}
static void ssl_write(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const OnlineProvider *p = online_provider();
    int64_t (*wr)(void *, void *, const void *, size_t, int *) = ONLINE_FN(p, tls_write);
    const IpcBuf *b = ipc_in_buf(c, 0);
    int err = 0;
    const void *src = b && b->size ? ipc_buf_ptr(b) : NULL;
    if (!k->established || !wr || !src) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    int64_t r = wr(p->ctx, k->conn, src, (size_t)b->size, &err);
    if (r < 0) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    uint32_t n = (uint32_t)r;
    IPC_OUT(c, n);
}
static void ssl_pending(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const OnlineProvider *p = online_provider();
    int64_t (*pd)(void *, void *) = ONLINE_FN(p, tls_pending);
    int32_t v = k->established && pd ? (int32_t)pd(p->ctx, k->conn) : 0;
    IPC_OUT(c, v);
}
static void ssl_flush_cache(IpcObject *s, IpcCall *c) { /* no session cache is kept: nothing to flush */
    (void)s;
    (void)c;
}
static const IpcCmd ssl_connection_cmds[] = {
    {0, "SetSocketDescriptor", ssl_set_fd, IPC_IMPL},
    {1, "SetHostName", ssl_set_host, IPC_IMPL},
    {2, "SetVerifyOption", ssl_set_verify, IPC_IMPL},
    {3, "SetIoMode", ssl_set_io_mode, IPC_IMPL},
    {4, "GetSocketDescriptor", ssl_get_fd, IPC_IMPL},
    {5, "GetHostName", ssl_get_host_len, IPC_IMPL},
    {6, "GetVerifyOption", ssl_get_verify, IPC_IMPL},
    {7, "GetIoMode", ssl_get_io_mode, IPC_IMPL},
    {8, "DoHandshake", ssl_handshake, IPC_IMPL},
    {10, "Read", ssl_read, IPC_IMPL},
    {11, "Write", ssl_write, IPC_IMPL},
    {12, "Pending", ssl_pending, IPC_IMPL},
    {19, "FlushSessionCache", ssl_flush_cache, IPC_IMPL},
    {20, "SetRenegotiationMode", ssl_set_reneg, IPC_IMPL},
    {21, "GetRenegotiationMode", ssl_get_reneg, IPC_IMPL},
    {9, "DoHandshakeGetServerCert", sysd_err_ssl, IPC_STUB},
    {13, "Peek", sysd_err_ssl, IPC_STUB},
    {14, "Poll", sysd_event, IPC_STUB},
    {15, "GetVerifyCertError", sysd_z4, IPC_STUB},
    {16, "GetNeededServerCertBufferSize", sysd_z4, IPC_STUB},
    {17, "SetSessionCacheMode", ssl_set_cache, IPC_IMPL},
    {18, "GetSessionCacheMode", ssl_get_cache, IPC_IMPL},
    {22, "SetOption", sysd_ok, IPC_STUB},
    {23, "GetOption", sysd_z4, IPC_STUB},
    {24, "GetVerifyCertErrors", sysd_err_ssl, IPC_STUB},
    {25, "GetCipherInfo", sysd_err_ssl, IPC_STUB},
    {26, "SetNextAlpnProto", sysd_ok, IPC_STUB},
    {27, "GetNextAlpnProto", sysd_err_ssl, IPC_STUB},
    {28, "SetDtlsSocketDescriptor", sysd_z4, IPC_STUB},
    {29, "GetDtlsHandshakeTimeout", sysd_err_ssl, IPC_STUB},
    {30, "SetPrivateOption", sysd_ok, IPC_STUB},
    {31, "SetSrtpCiphers", sysd_ok, IPC_STUB},
    {32, "GetSrtpCipher", sysd_z4, IPC_STUB},
    {33, "ExportKeyingMaterial", sysd_err_ssl, IPC_STUB},
    {34, "SetIoTimeout", ssl_set_io_timeout, IPC_IMPL},
    {35, "GetIoTimeout", ssl_get_io_timeout, IPC_IMPL},
    {36, "GetSessionTicket", sysd_err_ssl, IPC_STUB},
    {37, "SetSessionTicket", sysd_ok, IPC_STUB},
};
static void ssl_create_connection(IpcObject *s, IpcCall *c) {
    SslConn *k = calloc(1, sizeof *k);
    k->fd = -1;
    k->ctx = s->state;
    if (k->ctx) k->ctx->refs++, k->ctx->nconn++;
    ipc_out_object(c, IPC_OBJECT("ISslConnection", ssl_connection_cmds, k, ssl_conn_destroy));
}
/* ISslContext: options (SetOption(option, value) / GetOption(option) over 8 slots), connection count, and certificate imports. Importing a certificate store
 * hands back an id and remembers it until RemovePki; no certificate is parsed or trusted because no handshake can complete without a provider. */
static SslCtx *ssl_x(IpcObject *s) { return s->state; }
static void ssl_set_option(IpcObject *s, IpcCall *c) {
    if (c->in_size >= 8) {
        uint32_t o = IPC_IN(c, uint32_t, 0);
        if (o < 8) ssl_x(s)->options[o] = IPC_IN(c, uint32_t, 4);
    }
}
static void ssl_get_option(IpcObject *s, IpcCall *c) {
    uint32_t o = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0, v = o < 8 ? ssl_x(s)->options[o] : 0;
    IPC_OUT(c, v);
}
static void ssl_conn_count(IpcObject *s, IpcCall *c) {
    uint32_t v = ssl_x(s)->nconn;
    IPC_OUT(c, v);
}
static void ssl_import_pki(IpcObject *s, IpcCall *c) {
    SslCtx *x = ssl_x(s);
    uint64_t id = ++x->next_pki;
    if (x->npki < 8) x->ca[x->npki] = NULL, x->ca_n[x->npki] = 0, x->pki[x->npki++] = id;
    IPC_OUT(c, id);
}
/* 4 ImportServerPki(u32 format: 0 PEM, 1 DER; buffer certificate) -> u64 id: the certificate is kept (at most 64 KiB) so a provider can trust it as a CA. */
static void ssl_import_server_pki(IpcObject *s, IpcCall *c) {
    SslCtx *x = ssl_x(s);
    unsigned slot = x->npki;
    ssl_import_pki(s, c);
    const IpcBuf *b = ipc_in_buf(c, 0);
    const void *p = b && b->size && b->size <= 0x10000 ? ipc_buf_ptr(b) : NULL;
    if (slot < 8 && x->npki == slot + 1 && p && (x->ca[slot] = malloc((size_t)b->size))) {
        memcpy(x->ca[slot], p, (size_t)b->size);
        x->ca_n[slot] = (uint32_t)b->size;
    }
}
static void ssl_remove_pki(IpcObject *s, IpcCall *c) {
    SslCtx *x = ssl_x(s);
    uint64_t id = c->in_size >= 8 ? IPC_IN(c, uint64_t, 0) : 0;
    for (unsigned i = 0; i < x->npki; i++)
        if (x->pki[i] == id) {
            free(x->ca[i]);
            --x->npki;
            x->pki[i] = x->pki[x->npki], x->ca[i] = x->ca[x->npki], x->ca_n[i] = x->ca_n[x->npki];
            return;
        }
}
static void ssl_ctx_destroy(IpcObject *o) {
    if (ssl_contexts) ssl_contexts--;
    ssl_ctx_unref(o->state);
}
static const IpcCmd ssl_context_cmds[] = {
    {0, "SetOption", ssl_set_option, IPC_IMPL},
    {1, "GetOption", ssl_get_option, IPC_IMPL},
    {2, "CreateConnection", ssl_create_connection, IPC_IMPL},
    {3, "GetConnectionCount", ssl_conn_count, IPC_IMPL},
    {4, "ImportServerPki", ssl_import_server_pki, IPC_IMPL},
    {5, "ImportClientPki", ssl_import_pki, IPC_IMPL},
    {6, "RemoveServerPki", ssl_remove_pki, IPC_IMPL},
    {7, "RemoveClientPki", ssl_remove_pki, IPC_IMPL},
    {8, "RegisterInternalPki", sysd_z8, IPC_STUB},
    {9, "AddPolicyOid", sysd_ok, IPC_STUB},
    {10, "ImportCrl", sysd_z8, IPC_STUB},
    {11, "RemoveCrl", sysd_ok, IPC_STUB},
    {12, "ImportClientCertKeyPki", sysd_z8, IPC_STUB},
    {13, "GeneratePrivateKeyAndCert", sysd_err_ssl, IPC_STUB},
};
static void ssl_create_context(IpcObject *s, IpcCall *c) {
    (void)s;
    SslCtx *x = calloc(1, sizeof *x);
    x->refs = 1;
    ssl_contexts++;
    ipc_out_object(c, IPC_OBJECT("ISslContext", ssl_context_cmds, x, ssl_ctx_destroy));
}
static void ssl_context_count(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = ssl_contexts;
    IPC_OUT(c, v);
}
static void ssl_set_interface_version(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size >= 4) ssl_interface_version = IPC_IN(c, uint32_t, 0);
}
static const IpcCmd ssl_cmds[] = {
    {0, "CreateContext", ssl_create_context, IPC_IMPL},
    {1, "GetContextCount", ssl_context_count, IPC_IMPL},
    {5, "SetInterfaceVersion", ssl_set_interface_version, IPC_IMPL},
    {2, "GetCertificates", sysd_err_ssl, IPC_STUB},
    {3, "GetCertificateBufSize", sysd_z4, IPC_STUB},
    {6, "FlushSessionCache", sysd_z4, IPC_STUB},
    {9, "ClearTls12FallbackFlag", sysd_ok, IPC_STUB},
    {10, "GetCertificateByIndex", sysd_err_ssl, IPC_STUB},
    {11, "GetTrustedCertificateCount", sysd_z4, IPC_STUB},
};
static IpcObject *ssl_open(void) { return IPC_OBJECT("ssl", ssl_cmds, NULL, NULL); }

void service_register_offline(void) {
    ipc_register_service("prepo:u", prepo_open);
    ipc_register_service("prepo:a", prepo_open);
    ipc_register_service("prepo:m", prepo_open);
    ipc_register_service("prepo:s", prepo_open);
    ipc_register_service("ssl", ssl_open);
    ipc_register_service("nfp:user", nfp_open);
    ipc_register_service("nfc:user", nfc_open);
    ipc_register_service("nfp:sys", nfp_sys_open);
    ipc_register_service("nfc:sys", nfc_sys_open);
    ipc_register_service("nfc:mf:u", mifare_open);
    ipc_register_service("bsd:u", bsd_open);
    ipc_register_service("bsd:s", bsd_open);
    ipc_register_service("bsd:a", bsd_open);
    ipc_register_service("nsd:u", nsd_open);
    ipc_register_service("nsd:a", nsd_open);
    ipc_register_service("csrng", csrng_open);
    ipc_register_service("nifm:u", nifm_open);
    ipc_register_service("nifm:a", nifm_open);
    ipc_register_service("nifm:s", nifm_open);
    ipc_register_service("mm:u", mm_open);
    ipc_register_service("friend:u", friend_open);
    ipc_register_service("friend:v", friend_open);
    ipc_register_service("friend:m", friend_open);
    ipc_register_service("friend:s", friend_open);
    ipc_register_service("friend:a", friend_open);
    (void)ret_u8_0;
    (void)ret_u64_0;
    (void)ret_event;
    (void)ret_event_signaled;
}
