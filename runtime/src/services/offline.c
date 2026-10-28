/* Services that exist on a console but have no meaning for an offline, single-user local run. Each answers a documented safe default
 * (state where a title can observe it, otherwise a documented answer; rows still IPC_STUB are listed in docs/architecture/runtime-services.md): the point is that the SDK's initialisation succeeds and the game falls
 * back to its offline paths. Unknown commands still fail loudly through the generic policy. */
#include "../ipc.h"
#include "offline_core.h"
#include "offline_state_core.h"
#include "hz_result.h"
#include "online_provider.h"
#include "bsd_core.h"
#include "nfp_core.h"
#include "amiibo_lib_core.h"
#include <limits.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

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
static void nifm_ip(IpcObject *s, IpcCall *c) {
    (void)s;
    OnlineNetInfo ni;
    uint32_t v = nifm_info(&ni) ? ni.ip : 0;
    IPC_OUT(c, v);
}
static void nifm_wireless_enabled(IpcObject *s, IpcCall *c) {
    (void)s;
    OnlineNetInfo ni;
    uint8_t v = nifm_info(&ni) && ni.kind != 2;
    IPC_OUT(c, v);
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
    uint32_t requirement_preset, priority, raw_priority, profile_id[4];
    uint8_t rejectable, persistent, instant, sustainable, confirmation_option;
    uint8_t requirement[0x10];
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
/* GetDefaultIpSetting: no network profile exists, so the IpSettingData buffer is zero (DHCP, no addresses). */
static void nifm_default_ip(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t z[0x20] = {0};
    if (b && b->size) ipc_write_buf(c, b, z, b->size < sizeof z ? b->size : sizeof z);
}
static const IpcCmd nifm_general_cmds[] = {
    {1, "GetClientId", nifm_get_client_id, IPC_IMPL},
    {4, "CreateRequest", nifm_create_request, IPC_IMPL},
    {12, "GetCurrentIpAddress", nifm_ip, IPC_IMPL},
    {15, "GetInternetConnectionStatus", ret_conn_status, IPC_IMPL},
    {16, "IsWirelessCommunicationEnabled", nifm_wireless_enabled, IPC_IMPL},
    {17, "GetInternetConnectionStatusEx", ret_conn_status, IPC_IMPL},
    {21, "IsAnyInternetRequestAccepted", nifm_any_accepted, IPC_IMPL},
    {22, "IsAnyForegroundRequestAccepted", nifm_any_accepted, IPC_IMPL},
    {26, "SetExclusiveClient", nifm_set_exclusive, IPC_IMPL},
    {27, "GetDefaultIpSetting", nifm_default_ip, IPC_IMPL},
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
    int32_t v[2] = {ret, err};
    ipc_out(c, v, 8);
}
static void bsd_reply3(IpcCall *c, int ret, int err, uint32_t extra) {
    int32_t v[3] = {ret, err, (int32_t)extra};
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
    int r = bsdg_getsockopt(bsd_g(), bsd_in_fd(c), bsd_in32(c, 4, 0), bsd_in32(c, 8, 0), val, &n, &err);
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
    if (!raw || !n) {
        bsd_reply(c, 0, 0);
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
    {29, "GetResourceStatistics", bsd_resource_stats, IPC_IMPL},
    {30, "RecvMMsg", bsd_recvmmsg, IPC_IMPL},
    {31, "SendMMsg", bsd_sendmmsg, IPC_IMPL},
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
static const IpcCmd prepo_cmds[] = {
    {10100, "SaveReport", prepo_save, IPC_IMPL},
    {10101, "SaveReportWithUser", prepo_save, IPC_IMPL},
    {10102, "SaveReportOld", prepo_save, IPC_IMPL},
    {10103, "SaveReportWithUserOld", prepo_save, IPC_IMPL},
    {10104, "SaveReportForApplication", prepo_save, IPC_IMPL},
    {10105, "SaveReportWithUserForApplication", prepo_save, IPC_IMPL}, /* Animal Crossing 3.0.3 (SDK 20) reports with 10105 */
    {10200, "RequestImmediateTransmission", noop, IPC_IMPL},           /* offline: queued reports stay queued */
    {10300, "GetTransmissionStatus", ret_u32_0, IPC_IMPL},
    {10400, "GetSystemSessionId", ret_u64_1, IPC_IMPL},
    {20100, "SaveSystemReport", prepo_save_system, IPC_IMPL},
    {20101, "SaveSystemReportWithUser", prepo_save_system, IPC_IMPL},
    {40100, "IsUserAgreementCheckEnabled", prepo_get_agreement, IPC_IMPL},
    {40101, "SetUserAgreementCheckEnabled", prepo_set_agreement, IPC_IMPL},
    {90100, "ClearStorage", prepo_clear_storage, IPC_IMPL},
    {90200, "GetStorageUsage", prepo_storage_usage, IPC_IMPL},
};
static IpcObject *prepo_open(void) { return IPC_OBJECT("prepo:u", prepo_cmds, NULL, NULL); }

/* ssl (nn::ssl): contexts and connections can be created and configured. Handshake and I/O go through the active OnlineProvider's TLS entry points over
 * the bsd descriptor the connection was given (bsd_core.c maps it to a provider socket). With the NullProvider no handshake can complete (there is no
 * socket peer), so DoHandshake reports "not connected". Commands past 8 are the author's memory of the public description (unverified). */
typedef struct SslCtx {
    uint32_t refs, nconn, options[8], next_pki;
    uint64_t pki[8]; /* ids handed out by ImportServerPki / ImportClientPki and not yet removed */
    unsigned npki;
} SslCtx;                     /* shared by the context object and its connections (refcounted so a connection can outlive its context object) */
static uint32_t ssl_contexts; /* live ISslContext objects, for GetContextCount */
static uint32_t ssl_interface_version;
static void ssl_ctx_unref(SslCtx *x) {
    if (x && --x->refs == 0) free(x);
}
typedef struct {
    SslCtx *ctx;
    int32_t fd;
    char host[256];
    uint32_t verify, io_mode, cache_mode;
    void *conn;
    int established;
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
    if (c->in_size >= 4) k->verify = IPC_IN(c, uint32_t, 0);
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
    if (!k->conn) k->conn = mk(p->ctx, h);
    if (!k->conn) {
        c->result = HZ_R_SSL_NOT_CONNECTED;
        return;
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
    {19, "SetSessionCacheMode", ssl_set_cache, IPC_IMPL},
    {20, "GetSessionCacheMode", ssl_get_cache, IPC_IMPL},
    {21, "FlushSessionCache", ssl_flush_cache, IPC_IMPL},
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
    if (x->npki < 8) x->pki[x->npki++] = id;
    IPC_OUT(c, id);
}
static void ssl_remove_pki(IpcObject *s, IpcCall *c) {
    SslCtx *x = ssl_x(s);
    uint64_t id = c->in_size >= 8 ? IPC_IN(c, uint64_t, 0) : 0;
    for (unsigned i = 0; i < x->npki; i++)
        if (x->pki[i] == id) {
            x->pki[i] = x->pki[--x->npki];
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
    {4, "ImportServerPki", ssl_import_pki, IPC_IMPL},
    {5, "ImportClientPki", ssl_import_pki, IPC_IMPL},
    {6, "RemovePki", ssl_remove_pki, IPC_IMPL},
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
