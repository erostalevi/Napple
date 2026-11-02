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
static void nifm_req_destroy(IpcObject *o) {
    NifmReq *q = o->state;
    nifm_request_cancel(&q->req); /* releases the provider connection */
    nifm_uncount(q);
    free(q);
}
static void nifm_signal(NifmReq *q) {
    for (int i = 0; i < 2; i++)
        if (q->ev_w[i]) event_signal_handle(q->ev_w[i]);
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
    nifm_uncount(q);
    nifm_request_submit(&q->req);
    if (q->req.state == NIFM_STATE_ACCEPTED) {
        __atomic_add_fetch(&nifm_accepted_requests, 1, __ATOMIC_SEQ_CST);
        q->counted = 1;
    }
    nifm_signal(q);
} /* the NullProvider rejects at once */
static void nifm_req_cancel(IpcObject *s, IpcCall *c) {
    (void)c;
    NifmReq *q = s->state;
    nifm_request_cancel(&q->req);
    nifm_uncount(q);
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
    {2, "CreateScanRequest", sysd_err_net, IPC_STUB},
    {5, "GetCurrentNetworkProfile", sysd_err_net, IPC_STUB},
    {6, "EnumerateNetworkInterfaces", sysd_z4, IPC_STUB},
    {7, "EnumerateNetworkProfiles", sysd_err_net, IPC_STUB},
    {8, "GetNetworkProfile", sysd_err_net, IPC_STUB},
    {9, "SetNetworkProfile", sysd_err_net, IPC_STUB},
    {10, "RemoveNetworkProfile", sysd_ok, IPC_STUB},
    {14, "CreateTemporaryNetworkProfile", sysd_err_net, IPC_STUB},
    {15, "GetCurrentIpConfigInfo", nifm_ip_config, IPC_IMPL},
    {23, "PutToSleep", sysd_ok, IPC_STUB},
    {24, "WakeUp", sysd_ok, IPC_STUB},
    {25, "GetSsidListVersion", sysd_z4, IPC_STUB},
    {28, "SetDefaultIpSetting", nifm_set_default_ip, IPC_IMPL},
    {31, "GetTelemetorySystemEventReadableHandle", sysd_err_net, IPC_STUB},
    {33, "ConfirmSystemAvailability", sysd_ok, IPC_STUB},
    {34, "SetBackgroundRequestEnabled", nifm_set_background, IPC_IMPL},
    {35, "GetScanData", sysd_err_net, IPC_STUB},
    {36, "GetCurrentAccessPoint", sysd_err_net, IPC_STUB},
    {37, "Shutdown", sysd_ok, IPC_STUB},
    {38, "GetAllowedChannels", sysd_err_net, IPC_STUB},
    {39, "NotifyApplicationSuspended", sysd_ok, IPC_STUB},
    {40, "SetAcceptableNetworkTypeFlag", nifm_set_acceptable, IPC_IMPL},
    {41, "GetAcceptableNetworkTypeFlag", nifm_get_acceptable, IPC_IMPL},
    {42, "NotifyConnectionStateChanged", sysd_ok, IPC_STUB},
    {43, "SetWowlDelayedWakeTime", nifm_set_wowl_wake, IPC_IMPL},
    {44, "IsWiredConnectionAvailable", sysd_z1, IPC_STUB},
    {47, "GetScanData", sysd_err_net, IPC_STUB},
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
/* EventFd(initval, flags): the guest descriptor table maps to provider sockets only, so an event descriptor cannot be made: {-1, EOPNOTSUPP} (documented default). */
static void bsd_eventfd(IpcObject *s, IpcCall *c) {
    (void)s;
    bsd_fail(c, HZ_EOPNOTSUPP);
}
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
    {31, "EventFd", bsd_eventfd, IPC_STUB},
    {32, "RegisterResourceStatisticsName", sysd_ok, IPC_STUB},
    {33, "RegisterClientShared", sysd_ok, IPC_STUB},
    {34, "GetSocketStatistics", sysd_err_generic, IPC_STUB},
    {35, "NifIoctl", sysd_err_generic, IPC_STUB},
    {37, "SetSocketBufferAttribute", sysd_ok, IPC_STUB},
    {39, "NoSfEventFd", sysd_err_generic, IPC_STUB},
    {40, "ShutdownAllSockets", sysd_ok, IPC_STUB},
    {42, "SendMMsgNoMem", sysd_err_generic, IPC_STUB},
    {43, "RecvMMsgNoMem", sysd_err_generic, IPC_STUB},
    {200, "SetThreadCoreMask", sysd_ok, IPC_STUB},
    {201, "GetThreadCoreMask", sysd_z8, IPC_STUB},
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
 * socket peer), so DoHandshake reports "not connected". Layouts of the commands past 8 follow switchbrew 'SSL services' and libnx ssl.h; the result codes
 * of the failure cases are not public ([unverified], hz_result.h). What the provider has no entry point for (the server's certificate bytes, the negotiated
 * cipher, ALPN, DTLS, client certificates) is said at each handler, with what the online path would need. */
enum { SSL_OPT_DO_NOT_CLOSE_SOCKET, SSL_OPT_GET_SERVER_CERT_CHAIN, SSL_OPT_SKIP_DEFAULT_VERIFY, SSL_OPT_ENABLE_ALPN, SSL_OPT_COUNT }; /* SslOptionType */
enum { SSL_POLL_READ = 1u << 0, SSL_POLL_WRITE = 1u << 1, SSL_POLL_EXCEPT = 1u << 2 };                                                 /* SslPollEvent */
enum { SSL_PRIV_DTLS_SESSION = 1, SSL_PRIV_SET_CIPHER = 2 };                                                                            /* SslPrivateOptionType */
#define SSL_ALPN_MAX 0x200u    /* SetNextAlpnProto list kept per connection (a limit chosen here; real lists are a few names) */
#define SSL_PEEK_MAX 0x10000u  /* Peek: bytes read from the provider and held for the next Read */
typedef struct SslCtx {
    uint32_t refs, nconn, options[8], next_pki;
    uint64_t pki[8]; /* ids handed out by ImportServerPki / ImportClientPki / RegisterInternalPki / ImportClientCertKeyPki and not yet removed */
    unsigned npki;
    uint8_t *ca[8]; /* ImportServerPki: the certificate of pki[i] (NULL for client PKI), handed to the provider as a trusted CA at the handshake */
    uint32_t ca_n[8];
    uint64_t client_id; /* the context's client certificate (any of the client imports), 0 = none: RegisterInternalPki refuses a second one */
    uint64_t *crl;      /* ImportCrl ids not yet removed (same id space as the PKIs) */
    unsigned ncrl;
    char **oid; /* AddPolicyOid strings */
    unsigned noid;
} SslCtx;                     /* shared by the context object and its connections (refcounted so a connection can outlive its context object) */
static uint32_t ssl_contexts; /* live ISslContext objects, for GetContextCount */
static uint32_t ssl_interface_version;
static void ssl_ctx_unref(SslCtx *x) {
    if (x && --x->refs == 0) {
        for (unsigned i = 0; i < x->npki; i++) free(x->ca[i]);
        for (unsigned i = 0; i < x->noid; i++) free(x->oid[i]);
        free(x->oid);
        free(x->crl);
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
    int fd_set;     /* SetSocketDescriptor / SetDtlsSocketDescriptor was called */
    int dtls;       /* SetDtlsSocketDescriptor was called (at most once) */
    uint8_t dtls_addr[32];
    uint32_t dtls_addr_n;
    uint32_t options;                         /* SetOption flags, bit per SslOptionType */
    uint32_t priv_dtls_session, priv_cipher;  /* SetPrivateOption values */
    uint8_t alpn[SSL_ALPN_MAX];               /* SetNextAlpnProto: {u8 size, data[size]}... as given */
    uint32_t alpn_n;
    uint16_t srtp[4]; /* SetSrtpCiphers: the profiles (1, 2) offered at a DTLS handshake */
    unsigned nsrtp;
    uint32_t verify_error;     /* GetVerifyCertError: the failing handshake's result, cleared when read */
    uint32_t hs_error;         /* GetVerifyCertErrors: the last handshake's result (0 = it passed or none ran) */
    uint32_t needed_cert_size; /* GetNeededServerCertBufferSize: the server certificate data the last handshake had to return */
    uint8_t *peek;             /* Peek: bytes read from the provider and not yet returned by Read */
    uint32_t npeek;
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
    free(k->peek);
    free(k);
}
static void ssl_set_fd(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    k->fd = c->in_size >= 4 ? IPC_IN(c, int32_t, 0) : -1;
    k->fd_set = 1;
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
/* 17 SetSessionCacheMode(u32): with the DtlsSession private option set, a non-zero mode is refused (switchbrew SetPrivateOption). */
static void ssl_set_cache(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    if (c->in_size < 4) return;
    uint32_t m = IPC_IN(c, uint32_t, 0);
    if (m && k->priv_dtls_session) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    k->cache_mode = m;
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
/* The handshake of DoHandshake / DoHandshakeGetServerCert: 0 or the result to report. A handshake the provider ran and refused is remembered for
 * GetVerifyCertError(s); the provider reports one result for every failure (a refused certificate and a dropped connection look the same). A DTLS
 * connection cannot be handshaken: no provider runs DTLS (that would need a datagram TLS entry point). */
static uint32_t ssl_do_handshake(SslConn *k) {
    const OnlineProvider *p = online_provider();
    void *(*mk)(void *, int) = ONLINE_FN(p, tls_new);
    int (*hs)(void *, void *, uint32_t *) = ONLINE_FN(p, tls_handshake);
    int (*sh)(void *, void *, const char *) = ONLINE_FN(p, tls_set_host);
    int h = bsdg_handle(bsdg_global(), k->fd);
    if (!mk || !hs || h < 0 || k->dtls) return HZ_R_SSL_NOT_CONNECTED; /* no socket can be connected, so no handshake can run */
    int fresh = !k->conn;
    if (!k->conn) k->conn = mk(p->ctx, h);
    if (!k->conn) return HZ_R_SSL_NOT_CONNECTED;
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
        k->verify_error = k->hs_error = res ? res : HZ_R_SSL_NOT_CONNECTED;
        return k->hs_error;
    }
    k->verify_error = k->hs_error = 0;
    k->needed_cert_size = 0; /* the provider hands back no certificate bytes (that needs a peer-certificate entry point), so none are owed */
    k->established = 1;
    return 0;
}
static void ssl_handshake(IpcObject *s, IpcCall *c) { c->result = ssl_do_handshake(s->state); }
/* 9 DoHandshakeGetServerCert(buffer type 0x6) -> u32 cert data size, u32 cert count: the handshake of 8, then the server certificate (or the chain with
 * GetServerCertChain, behind a "CertChMN" header) when PeerCa is verified. The provider exposes no certificate bytes, so a passed handshake returns
 * none (0, 0) and GetNeededServerCertBufferSize stays 0; the online path would need a provider entry point returning the peer chain as DER. */
static void ssl_handshake_get_cert(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    uint32_t r = ssl_do_handshake(k);
    if (r) {
        c->result = r;
        return;
    }
    uint32_t out[2] = {0, 0};
    ipc_out(c, out, sizeof out);
}
/* Bytes held by Peek come first: a Read after a Peek returns them (and only them) without asking the provider. */
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
    if (k->npeek) {
        uint32_t n = b->size < k->npeek ? (uint32_t)b->size : k->npeek;
        ipc_write_buf(c, b, k->peek, n);
        memmove(k->peek, k->peek + n, k->npeek - n);
        k->npeek -= n;
        IPC_OUT(c, n);
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
static int64_t ssl_provider_pending(SslConn *k) {
    const OnlineProvider *p = online_provider();
    int64_t (*pd)(void *, void *) = ONLINE_FN(p, tls_pending);
    return k->established && pd ? pd(p->ctx, k->conn) : 0;
}
static void ssl_pending(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    int32_t v = k->established ? (int32_t)(ssl_provider_pending(k) + k->npeek) : 0;
    IPC_OUT(c, v);
}
/* 13 Peek(buffer type 0x6) -> u32 size: like Read, but the bytes stay readable. The provider has no peek, so they are read from it once and held in the
 * connection until Read returns them. */
static void ssl_peek(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const OnlineProvider *p = online_provider();
    int64_t (*rd)(void *, void *, void *, size_t, int *) = ONLINE_FN(p, tls_read);
    const IpcBuf *b = ipc_out_buf(c, 0);
    int err = 0;
    if (!k->established || !rd || !b || !b->size) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    if (!k->npeek) {
        if (!k->peek && !(k->peek = malloc(SSL_PEEK_MAX))) {
            c->result = HZ_R_SSL_NOT_CONNECTED;
            return;
        }
        int64_t r = rd(p->ctx, k->conn, k->peek, b->size < SSL_PEEK_MAX ? (size_t)b->size : SSL_PEEK_MAX, &err);
        if (r < 0) {
            c->result = HZ_R_SSL_NOT_CONNECTED;
            return;
        }
        k->npeek = (uint32_t)r;
    }
    uint32_t n = b->size < k->npeek ? (uint32_t)b->size : k->npeek;
    ipc_write_buf(c, b, k->peek, n);
    IPC_OUT(c, n);
}
/* 14 Poll(u32 SslPollEvent, u32 timeout ms) -> u32 SslPollEvent: readiness of the connection's socket through the provider's poll. Bytes already
 * decrypted (the provider's pending count, or bytes held by Peek) make Read ready at once. A timeout past INT_MAX waits without limit [unverified]. */
static void ssl_poll(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const OnlineProvider *p = online_provider();
    int (*pl)(void *, OnlinePollFd *, unsigned, int, int *) = ONLINE_FN(p, sock_poll);
    uint32_t want = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0, tmo = c->in_size >= 8 ? IPC_IN(c, uint32_t, 4) : 0, got = 0;
    int h = k->fd_set ? bsdg_handle(bsdg_global(), k->fd) : -1;
    if (h < 0 || !pl) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    if ((want & SSL_POLL_READ) && (k->npeek || ssl_provider_pending(k) > 0)) got |= SSL_POLL_READ;
    int ev = ((want & SSL_POLL_READ) ? ONLINE_POLLIN : 0) | ((want & SSL_POLL_WRITE) ? ONLINE_POLLOUT : 0) | ((want & SSL_POLL_EXCEPT) ? ONLINE_POLLPRI : 0);
    OnlinePollFd f = {h, (int16_t)ev, 0};
    int err = 0;
    if (pl(p->ctx, &f, 1, got ? 0 : tmo > INT_MAX ? -1 : (int)tmo, &err) < 0) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    if ((want & SSL_POLL_READ) && (f.revents & (ONLINE_POLLIN | ONLINE_POLLHUP))) got |= SSL_POLL_READ;
    if ((want & SSL_POLL_WRITE) && (f.revents & ONLINE_POLLOUT)) got |= SSL_POLL_WRITE;
    if ((want & SSL_POLL_EXCEPT) && (f.revents & (ONLINE_POLLPRI | ONLINE_POLLERR | ONLINE_POLLNVAL))) got |= SSL_POLL_EXCEPT;
    IPC_OUT(c, got);
}
/* 15 GetVerifyCertError: no data; the result is the last refused handshake's, and reading it clears it (switchbrew). */
static void ssl_get_verify_error(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    c->result = k->verify_error;
    k->verify_error = 0;
}
/* 16 GetNeededServerCertBufferSize -> u32: the server certificate data size the last handshake had (see DoHandshakeGetServerCert). */
static void ssl_get_needed_cert_size(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    IPC_OUT(c, k->needed_cert_size);
}
/* 22 SetOption({u8 flag, u8 pad[3], u32 SslOptionType}) / 23 GetOption(u32 SslOptionType) -> u8: per-connection flags. DoNotCloseSocket can only be
 * set before SetSocketDescriptor. The flags are kept; SkipDefaultVerify and EnableAlpn change nothing the provider does (it has no switch for the
 * built-in store and offers no ALPN). */
static void ssl_set_conn_option(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    uint32_t o = c->in_size >= 8 ? IPC_IN(c, uint32_t, 4) : SSL_OPT_COUNT;
    if (o >= SSL_OPT_COUNT || (o == SSL_OPT_DO_NOT_CLOSE_SOCKET && k->fd_set)) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    if (IPC_IN(c, uint8_t, 0)) k->options |= 1u << o;
    else k->options &= ~(1u << o);
}
static void ssl_get_conn_option(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    uint32_t o = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : SSL_OPT_COUNT;
    if (o >= SSL_OPT_COUNT) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    uint8_t v = (k->options >> o) & 1;
    IPC_OUT(c, v);
}
/* 24 GetVerifyCertErrors(buffer type 0x6 of Result) -> u32 written, u32 total: the results of the last handshake (one entry when it was refused). */
static void ssl_get_verify_errors(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint32_t total = k->hs_error ? 1 : 0, cap = b ? (uint32_t)(b->size / 4) : 0, n = total < cap ? total : cap;
    if (n) ipc_write_buf(c, b, &k->hs_error, 4);
    uint32_t out[2] = {n, total};
    ipc_out(c, out, sizeof out);
}
/* 25 GetCipherInfo(u32 version = 1, buffer type 0x6 of 0x48 bytes: char cipher[0x40], char protocol_version[8]). Only a passed handshake has a cipher.
 * The provider does not say which suite it negotiated, so an established connection gets empty strings; the online path would need a cipher entry point. */
static void ssl_get_cipher_info(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (c->in_size < 4 || IPC_IN(c, uint32_t, 0) != 1 || !b || b->size != 0x48) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    if (!k->fd_set || !k->established) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    static const uint8_t info[0x48] = {0};
    ipc_write_buf(c, b, info, sizeof info);
}
/* 26 SetNextAlpnProto(buffer type 0x5: {u8 size, data[size]}..., at least 2 bytes, every entry inside the buffer): kept per connection. The provider
 * offers no ALPN (that would need an ALPN entry point), so the list never reaches a server. */
static void ssl_set_alpn(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    if (!k->fd_set) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    uint64_t n = p ? b->size : 0, i = 0;
    while (i < n && p[i] && i + 1 + p[i] <= n) i += 1u + p[i];
    if (n < 2 || n > SSL_ALPN_MAX || i != n) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    memcpy(k->alpn, p, (size_t)n);
    k->alpn_n = (uint32_t)n;
}
/* 27 GetNextAlpnProto(buffer type 0x6) -> u32 SslAlpnProtoState, u32 size. Before a handshake the answer is empty (libnx). After one the server could
 * not select a protocol because none was offered (see 26): NoSupport (0), empty. */
static void ssl_get_alpn(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    if (!k->fd_set) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
    }
    uint32_t out[2] = {0, 0};
    ipc_out(c, out, sizeof out);
}
/* 28 SetDtlsSocketDescriptor(s32 fd, buffer type 0x5: SockAddr, at least 0x10 bytes, byte 0 = its size) -> s32 fd: like SetSocketDescriptor with
 * the peer's address; only once per connection. The handshake of a DTLS connection reports "not connected" (no provider runs DTLS). */
static void ssl_set_dtls_fd(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *a = b && b->size ? ipc_buf_ptr(b) : NULL;
    if (c->in_size < 4 || !a || b->size < 0x10 || a[0] != b->size || k->dtls) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    k->fd = IPC_IN(c, int32_t, 0);
    k->fd_set = k->dtls = 1;
    k->dtls_addr_n = b->size < sizeof k->dtls_addr ? (uint32_t)b->size : (uint32_t)sizeof k->dtls_addr;
    memcpy(k->dtls_addr, a, k->dtls_addr_n);
    int32_t v = k->fd;
    IPC_OUT(c, v);
}
/* 30 SetPrivateOption({u32 value (a bool before 17.0.0), u32 SslPrivateOptionType}) [unverified order, as SetOption]: 1 DtlsSession (a non-zero
 * SetSessionCacheMode is then refused), 2 SetCipher (the only suite to offer, kept; the provider has no suite selection). */
static void ssl_set_private_option(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    uint32_t v = c->in_size >= 8 ? IPC_IN(c, uint32_t, 0) : 0, o = c->in_size >= 8 ? IPC_IN(c, uint32_t, 4) : 0;
    if (o == SSL_PRIV_DTLS_SESSION) k->priv_dtls_session = v != 0;
    else if (o == SSL_PRIV_SET_CIPHER) k->priv_cipher = v;
    else c->result = HZ_R_SSL_INVALID_ARGUMENT;
}
/* 31 SetSrtpCiphers(buffer type 0x5: u16[], 2-byte aligned, at most 4): the profiles 1 and 2 are kept for a DTLS handshake, other values ignored. */
static void ssl_set_srtp(IpcObject *s, IpcCall *c) {
    SslConn *k = s->state;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    uint64_t n = p ? b->size : 0;
    if (n % 2 || n / 2 > 4) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    k->nsrtp = 0;
    for (uint64_t i = 0; i < n / 2; i++) {
        uint16_t v;
        memcpy(&v, p + 2 * i, 2);
        if (v == 1 || v == 2) k->srtp[k->nsrtp++] = v;
    }
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
    {9, "DoHandshakeGetServerCert", ssl_handshake_get_cert, IPC_IMPL},
    {13, "Peek", ssl_peek, IPC_IMPL},
    {14, "Poll", ssl_poll, IPC_IMPL},
    {15, "GetVerifyCertError", ssl_get_verify_error, IPC_IMPL},
    {16, "GetNeededServerCertBufferSize", ssl_get_needed_cert_size, IPC_IMPL},
    {17, "SetSessionCacheMode", ssl_set_cache, IPC_IMPL},
    {18, "GetSessionCacheMode", ssl_get_cache, IPC_IMPL},
    {22, "SetOption", ssl_set_conn_option, IPC_IMPL},
    {23, "GetOption", ssl_get_conn_option, IPC_IMPL},
    {24, "GetVerifyCertErrors", ssl_get_verify_errors, IPC_IMPL},
    {25, "GetCipherInfo", ssl_get_cipher_info, IPC_IMPL},
    {26, "SetNextAlpnProto", ssl_set_alpn, IPC_IMPL},
    {27, "GetNextAlpnProto", ssl_get_alpn, IPC_IMPL},
    {28, "SetDtlsSocketDescriptor", ssl_set_dtls_fd, IPC_IMPL},
    {29, "GetDtlsHandshakeTimeout", sysd_err_ssl, IPC_STUB}, /* the retransmit timer of a running DTLS handshake: no provider runs DTLS */
    {30, "SetPrivateOption", ssl_set_private_option, IPC_IMPL},
    {31, "SetSrtpCiphers", ssl_set_srtp, IPC_IMPL},
    {32, "GetSrtpCipher", sysd_z4, IPC_STUB},             /* the profile a DTLS handshake negotiated: none can run */
    {33, "ExportKeyingMaterial", sysd_err_ssl, IPC_STUB}, /* needs the session's master secret, which the provider does not expose */
    {34, "SetIoTimeout", ssl_set_io_timeout, IPC_IMPL},
    {35, "GetIoTimeout", ssl_get_io_timeout, IPC_IMPL},
    {36, "GetSessionTicket", sysd_err_ssl, IPC_STUB}, /* 20.0.0+: no public layout */
    {37, "SetSessionTicket", sysd_ok, IPC_STUB},      /* 20.0.0+: no public layout */
};
static void ssl_create_connection(IpcObject *s, IpcCall *c) {
    SslConn *k = calloc(1, sizeof *k);
    k->fd = -1;
    k->ctx = s->state;
    if (k->ctx) k->ctx->refs++, k->ctx->nconn++;
    ipc_out_object(c, IPC_OBJECT("ISslConnection", ssl_connection_cmds, k, ssl_conn_destroy));
}
/* ISslContext: options (SetOption(option, value) / GetOption(option) over 8 slots), connection count, and certificate imports. Importing a certificate store
 * hands back an id and remembers it until RemovePki; server certificates are handed to the provider as trusted CAs at the handshake. Client
 * certificates, CRLs and policy OIDs are kept but reach no provider (it has no client-certificate, CRL or policy entry point). */
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
static void ssl_import_client_pki(IpcObject *s, IpcCall *c) {
    SslCtx *x = ssl_x(s);
    ssl_import_pki(s, c);
    if (!x->client_id) x->client_id = x->next_pki;
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
            if (x->client_id == id) x->client_id = 0;
            return;
        }
}
/* 8 RegisterInternalPki(u32 SslInternalPki) -> u64 id: 1 DeviceClientCertDefault (the console's device certificate as the client certificate) is the
 * only value; refused when the context already has a client certificate. This runtime has no device certificate, so no handshake presents one. */
static void ssl_register_internal_pki(IpcObject *s, IpcCall *c) {
    SslCtx *x = ssl_x(s);
    if (c->in_size < 4 || IPC_IN(c, uint32_t, 0) != 1 || x->client_id) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    ssl_import_client_pki(s, c);
}
/* 9 AddPolicyOid(buffer type 0x5: string): the NUL must be inside the buffer and the string at most 0xFE long. Kept per context. */
static void ssl_add_policy_oid(IpcObject *s, IpcCall *c) {
    SslCtx *x = ssl_x(s);
    const IpcBuf *b = ipc_in_buf(c, 0);
    const char *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    size_t n = p ? strnlen(p, (size_t)b->size) : 0;
    char **grown = p && n < b->size && n <= 0xFE ? realloc(x->oid, (x->noid + 1) * sizeof *x->oid) : NULL;
    if (!grown) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    x->oid = grown;
    if ((x->oid[x->noid] = malloc(n + 1))) memcpy(x->oid[x->noid], p, n + 1), x->noid++;
}
/* A whole DER element starting with tag `tag` fills [p, p + n): the outer shape of a certificate, key or CRL. */
static int ssl_der_whole(const uint8_t *p, uint64_t n, uint8_t tag) {
    if (!p || n < 2 || p[0] != tag) return 0;
    uint64_t len = p[1], hdr = 2;
    if (len & 0x80) {
        unsigned nb = (unsigned)(len & 0x7f);
        if (!nb || nb > 4 || n < 2 + nb) return 0;
        len = 0;
        for (unsigned i = 0; i < nb; i++) len = len << 8 | p[2 + i];
        hdr += nb;
    }
    return hdr + len == n;
}
/* 10 ImportCrl(buffer type 0x5: DER CRL) -> u64 id / 11 RemoveCrl(u64 id): kept per context in the PKI id space. An id that is not there is refused. */
static void ssl_import_crl(IpcObject *s, IpcCall *c) {
    SslCtx *x = ssl_x(s);
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    uint64_t *grown = ssl_der_whole(p, p ? b->size : 0, 0x30) ? realloc(x->crl, (x->ncrl + 1) * sizeof *x->crl) : NULL;
    if (!grown) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    x->crl = grown;
    uint64_t id = x->crl[x->ncrl++] = ++x->next_pki;
    IPC_OUT(c, id);
}
static void ssl_remove_crl(IpcObject *s, IpcCall *c) {
    SslCtx *x = ssl_x(s);
    uint64_t id = c->in_size >= 8 ? IPC_IN(c, uint64_t, 0) : 0;
    for (unsigned i = 0; i < x->ncrl; i++)
        if (x->crl[i] == id) {
            x->crl[i] = x->crl[--x->ncrl];
            return;
        }
    c->result = HZ_R_SSL_NOT_FOUND;
}
/* 12 ImportClientCertKeyPki(buffer type 0x5 cert, buffer type 0x5 key, u32 SslCertificateFormat: 1 PEM, 2 DER) -> u64 id: the pair becomes the
 * context's client certificate. The format must match the data's shape. The provider has no client-certificate entry point, so it is not presented. */
static void ssl_import_client_cert_key(IpcObject *s, IpcCall *c) {
    const IpcBuf *cb = ipc_in_buf(c, 0), *kb = ipc_in_buf(c, 1);
    const uint8_t *cp = cb && cb->size ? ipc_buf_ptr(cb) : NULL, *kp = kb && kb->size ? ipc_buf_ptr(kb) : NULL;
    uint32_t fmt = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0;
    int ok = cp && kp &&
             (fmt == 1   ? cb->size > 10 && kb->size > 10 && !memcmp(cp, "-----BEGIN", 10) && !memcmp(kp, "-----BEGIN", 10)
              : fmt == 2 ? ssl_der_whole(cp, cb->size, 0x30) && ssl_der_whole(kp, kb->size, 0x30)
                         : 0);
    if (!ok) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    ssl_import_client_pki(s, c);
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
    {5, "ImportClientPki", ssl_import_client_pki, IPC_IMPL},
    {6, "RemoveServerPki", ssl_remove_pki, IPC_IMPL},
    {7, "RemoveClientPki", ssl_remove_pki, IPC_IMPL},
    {8, "RegisterInternalPki", ssl_register_internal_pki, IPC_IMPL},
    {9, "AddPolicyOid", ssl_add_policy_oid, IPC_IMPL},
    {10, "ImportCrl", ssl_import_crl, IPC_IMPL},
    {11, "RemoveCrl", ssl_remove_crl, IPC_IMPL},
    {12, "ImportClientCertKeyPki", ssl_import_client_cert_key, IPC_IMPL},
    {13, "GeneratePrivateKeyAndCert", sysd_err_ssl, IPC_STUB}, /* needs RSA key generation and an X.509 signer: no crypto library in the runtime */
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
/* The built-in trusted certificates (the console's CertStore system data, ssl_TrustedCerts.bdf). Napple ships no CertStore, so the list is empty and
 * handshakes trust what the provider trusts (the host's store) plus the guest's imports; GetCertificates / GetCertificateBufSize / GetCertificateByIndex /
 * GetTrustedCertificateCount answer from this list, so they agree with each other (tests install entries to check the non-empty case). */
typedef struct {
    uint32_t id, status; /* SslCaCertificateId, SslTrustedCertStatus (1 = EnabledTrusted) */
    const uint8_t *der;
    uint32_t size;
} SslBuiltinCert;
static const SslBuiltinCert *ssl_builtin;
static unsigned ssl_nbuiltin;
#define SSL_CA_ALL 0xFFFFFFFFu  /* SslCaCertificateId_All */
#define SSL_CERT_INFO_SIZE 0x18u /* BuiltInCertificateInfo: u32 id, u32 status, u64 size, u64 data offset (from the buffer start) */
/* The entries `ids` names, in order (All = every entry); 0 or HZ_R_SSL_NOT_FOUND when an id is not in the list. *bytes = the GetCertificates size. */
static uint32_t ssl_resolve_certs(const uint32_t *ids, unsigned nids, const SslBuiltinCert **out, unsigned cap, unsigned *n, uint64_t *bytes) {
    *n = 0, *bytes = 0;
    for (unsigned i = 0; i < nids; i++) {
        int found = 0;
        for (unsigned j = 0; j < ssl_nbuiltin; j++) {
            if (ids[i] != SSL_CA_ALL && ids[i] != ssl_builtin[j].id) continue;
            found = 1;
            if (*n < cap) out[*n] = &ssl_builtin[j];
            ++*n, *bytes += SSL_CERT_INFO_SIZE + ssl_builtin[j].size;
        }
        if (!found && ids[i] != SSL_CA_ALL) return HZ_R_SSL_NOT_FOUND;
    }
    return 0;
}
/* Writes `n` entries (info array, then the DER data) into out buffer `b`, which must hold `bytes`. */
static uint32_t ssl_write_certs(IpcCall *c, const IpcBuf *b, const SslBuiltinCert *const *e, unsigned n, uint64_t bytes) {
    if (!bytes) return 0;
    uint8_t *dst = b && b->size >= bytes ? malloc((size_t)bytes) : NULL;
    if (!dst) return HZ_R_SSL_INVALID_ARGUMENT; /* the buffer cannot hold the entries [unverified result] */
    uint64_t data = (uint64_t)n * SSL_CERT_INFO_SIZE;
    for (unsigned i = 0; i < n; i++) {
        uint64_t sz = e[i]->size;
        memcpy(dst + i * SSL_CERT_INFO_SIZE, &e[i]->id, 4);
        memcpy(dst + i * SSL_CERT_INFO_SIZE + 4, &e[i]->status, 4);
        memcpy(dst + i * SSL_CERT_INFO_SIZE + 8, &sz, 8);
        memcpy(dst + i * SSL_CERT_INFO_SIZE + 16, &data, 8);
        memcpy(dst + data, e[i]->der, e[i]->size);
        data += sz;
    }
    ipc_write_buf(c, b, dst, bytes);
    free(dst);
    return 0;
}
/* 2 GetCertificates(buffer type 0x5: u32 CaCertificateId[], buffer type 0x6) -> u32 entries / 3 GetCertificateBufSize(buffer type 0x5 ids) -> u32. */
static void ssl_get_certs_common(IpcCall *c, int write) {
    const IpcBuf *ib = ipc_in_buf(c, 0);
    const uint8_t *ip = ib && ib->size ? ipc_buf_ptr(ib) : NULL;
    unsigned nids = ip ? (unsigned)(ib->size / 4) : 0, n = 0;
    uint32_t *ids = nids ? malloc(nids * 4u) : NULL;
    const SslBuiltinCert **e = ssl_nbuiltin ? malloc(ssl_nbuiltin * (size_t)(nids ? nids : 1) * sizeof *e) : NULL;
    uint64_t bytes = 0;
    if (ids) memcpy(ids, ip, nids * 4u);
    uint32_t r = ids || !nids ? ssl_resolve_certs(ids, nids, e, e ? ssl_nbuiltin * nids : 0, &n, &bytes) : HZ_R_SSL_INVALID_ARGUMENT;
    if (!r && write) r = ssl_write_certs(c, ipc_out_buf(c, 0), e, n, bytes);
    free(ids);
    free(e);
    if (r) {
        c->result = r;
        return;
    }
    uint32_t v = write ? n : (uint32_t)bytes;
    IPC_OUT(c, v);
}
static void ssl_get_certificates(IpcObject *s, IpcCall *c) {
    (void)s;
    ssl_get_certs_common(c, 1);
}
static void ssl_get_certificate_buf_size(IpcObject *s, IpcCall *c) {
    (void)s;
    ssl_get_certs_common(c, 0);
}
/* 11 GetTrustedCertificateCount -> u32 / 10 GetCertificateByIndex(u32 index, buffer type 0x6) -> u32 size [unverified layouts: 21.0.0+, switchbrew lists
 * the names only]: the EnabledTrusted entries of the list, by index, in GetCertificates' format (one info entry, then the DER). */
static const SslBuiltinCert *ssl_trusted_at(unsigned index, unsigned *count) {
    const SslBuiltinCert *hit = NULL;
    *count = 0;
    for (unsigned j = 0; j < ssl_nbuiltin; j++)
        if (ssl_builtin[j].status == 1 && (*count)++ == index) hit = &ssl_builtin[j];
    return hit;
}
static void ssl_trusted_count(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v;
    ssl_trusted_at(UINT_MAX, &v);
    IPC_OUT(c, v);
}
static void ssl_get_certificate_by_index(IpcObject *s, IpcCall *c) {
    (void)s;
    unsigned count;
    const SslBuiltinCert *e = c->in_size >= 4 ? ssl_trusted_at(IPC_IN(c, uint32_t, 0), &count) : NULL;
    if (!e) {
        c->result = HZ_R_SSL_NOT_FOUND;
        return;
    }
    uint64_t bytes = SSL_CERT_INFO_SIZE + e->size;
    uint32_t r = ssl_write_certs(c, ipc_out_buf(c, 0), &e, 1, bytes);
    if (r) {
        c->result = r;
        return;
    }
    uint32_t v = (uint32_t)bytes;
    IPC_OUT(c, v);
}
/* 6 FlushSessionCache(buffer type 0x5: host string, u32 type: 0 SingleHost, 1 AllHosts) -> u32 entries flushed. The service keeps no session cache (the
 * provider runs a full handshake every time), so after the arguments are checked nothing is flushed. */
static void ssl_flush_session_cache(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const char *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    uint32_t type = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 2;
    if (type > 1 || (type == 0 && (!p || !p[0] || strnlen(p, (size_t)b->size) == b->size))) {
        c->result = HZ_R_SSL_INVALID_ARGUMENT;
        return;
    }
    uint32_t v = 0;
    IPC_OUT(c, v);
}
/* 9 ClearTls12FallbackFlag: clears the flag that forces TLS 1.2 after repeated connection errors. No handshake here falls back (the provider picks the
 * version itself), so the flag is never set and clearing it leaves nothing to do. */
static void ssl_clear_tls12_fallback(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static const IpcCmd ssl_cmds[] = {
    {0, "CreateContext", ssl_create_context, IPC_IMPL},
    {1, "GetContextCount", ssl_context_count, IPC_IMPL},
    {5, "SetInterfaceVersion", ssl_set_interface_version, IPC_IMPL},
    {2, "GetCertificates", ssl_get_certificates, IPC_IMPL},
    {3, "GetCertificateBufSize", ssl_get_certificate_buf_size, IPC_IMPL},
    {6, "FlushSessionCache", ssl_flush_session_cache, IPC_IMPL},
    {9, "ClearTls12FallbackFlag", ssl_clear_tls12_fallback, IPC_IMPL},
    {10, "GetCertificateByIndex", ssl_get_certificate_by_index, IPC_IMPL},
    {11, "GetTrustedCertificateCount", ssl_trusted_count, IPC_IMPL},
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
