/* ldn:u (nn::ldn): local wireless play. A state machine (ldn_core) tracks initialize / open / create like the real service. The transport is the active
 * OnlineProvider's (ONLINE_CAP_LDN: scan, host, join, network description); the NullProvider has no radio, so scans return no networks, connecting fails and a
 * created network never gains stations. State is per local-communication object.
 * Command ids are from the author's memory of the public documentation (unverified). */
#include "svc_common.h"
#include "ldn_core.h"
#include "hz_result.h"
#include "online_provider.h"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include "sys_default.h"

/* The provider's LDN table when it can carry local wireless (ONLINE_CAP_LDN), else NULL: the NullProvider answers apply. */
int ldn_push_action_frame(uint64_t local_communication_id, const uint8_t src[6], const uint8_t bssid[6], uint16_t band_mhz, uint16_t channel, const void *data, size_t n);
static const OnlineProvider *ldn_provider(void) {
    const OnlineProvider *p = online_provider();
    return online_has(p, ONLINE_CAP_LDN) ? p : NULL;
}

struct LdnMonitor;
typedef struct Ldn {
    LdnState state;
    uint32_t event_w;
    LdnConfig cfg;
    int watching; /* registered for the provider's change notifications */
    /* configuration rows (105, 106, 403, 500-505, 600, 601): what the guest set, per object as on the console */
    uint32_t audio_policy, protocol, operation_mode;
    int action_frame; /* EnableActionFrame done (settings kept below) */
    uint8_t action_frame_settings[LDN_ACTION_FRAME_SETTINGS_SIZE];
    uint16_t home_band, home_channel; /* SetHomeChannel (MHz, channel number); 0 = not set */
    int32_t tx_power;                 /* SetTxPower; -1 = the default (ResetTxPower) */
    uint64_t frames_sent;             /* SendActionFrame calls that went out */
    struct LdnMonitor *monitor;       /* the IClientProcessMonitor that holds this client (RegisterClient), or NULL */
    int orphan;                       /* the service object was closed while a monitor still held it */
    LdnActionQueue afq;               /* received action frames (503), guarded by g_af_mu */
    struct Ldn *af_next;              /* the registry of live objects ldn_push_action_frame walks */
} Ldn;
static LdnState *st(IpcObject *s) { return &((Ldn *)s->state)->state; }
static void notify(IpcObject *s) {
    Ldn *l = s->state;
    if (l->event_w) event_signal_handle(l->event_w);
}
static void apply(IpcObject *s, IpcCall *c, LdnOp op) {
    uint32_t r = ldn_apply(st(s), op);
    if (r) c->result = r;
    else notify(s);
}

/* The provider reports network changes (a station joined or left, the host went away) through ldn_set_notify: the state change event fires. */
static void provider_changed(void *arg) {
    Ldn *l = arg;
    uint32_t w = __atomic_load_n(&l->event_w, __ATOMIC_ACQUIRE);
    if (w) event_signal_handle(w);
}
static void watch(IpcObject *s, int on) {
    Ldn *l = s->state;
    const OnlineProvider *p = ldn_provider();
    void (*set)(void *, void (*)(void *), void *) = p ? ONLINE_FN(p, ldn_set_notify) : NULL;
    if (!on && !l->watching) return; /* never clears another object's registration */
    if (set) set(p->ctx, on ? provider_changed : NULL, on ? l : NULL);
    l->watching = on;
}
/* A station whose host went away (the provider no longer has the network) is back to StationOpened, as on the console; the reason stays readable. */
static void sync_state(IpcObject *s) {
    if (*st(s) != LDN_STATION_CONNECTED) return;
    const OnlineProvider *p = ldn_provider();
    uint32_t (*get)(void *, OnlineLdnNetwork *, uint16_t *) = p ? ONLINE_FN(p, ldn_get_network) : NULL;
    OnlineLdnNetwork net;
    uint16_t reason;
    if (get && get(p->ctx, &net, &reason) != 0) {
        watch(s, 0);
        ldn_apply(st(s), LDN_OP_DISCONNECT);
    }
}

static void l_get_state(IpcObject *s, IpcCall *c) {
    sync_state(s);
    uint32_t v = (uint32_t)*st(s);
    IPC_OUT(c, v);
}
/* The network the provider says is hosted or joined, only while the state machine holds one. Returns 0 or the result for the guest. */
static uint32_t current_network(IpcObject *s, OnlineLdnNetwork *net, uint16_t *reason) {
    const OnlineProvider *p = ldn_provider();
    uint32_t (*get)(void *, OnlineLdnNetwork *, uint16_t *) = p ? ONLINE_FN(p, ldn_get_network) : NULL;
    uint16_t dummy;
    memset(net, 0, sizeof *net);
    sync_state(s);
    if (!get || !ldn_state_has_network(*st(s))) return ldn_network_query(*st(s));
    return get(p->ctx, net, reason ? reason : &dummy);
}
static void l_network_info(IpcObject *s, IpcCall *c) { /* 1 GetNetworkInfo -> NetworkInfo (0x480) in buffer 0 */
    OnlineLdnNetwork net;
    uint32_t r = current_network(s, &net, NULL);
    if (r) {
        c->result = r;
        return;
    }
    uint8_t info[LDN_NETINFO_SIZE];
    ldn_net_encode(&net, info);
    const OnlineProvider *p = ldn_provider();
    int (*nodes)(void *, OnlineLdnNode *, uint32_t, uint32_t *) = p ? ONLINE_FN(p, ldn_get_nodes) : NULL;
    OnlineLdnNode nd[ONLINE_LDN_MAX_NODES];
    int n = nodes ? nodes(p->ctx, nd, ONLINE_LDN_MAX_NODES, NULL) : -1;
    if (n >= 0) ldn_net_encode_nodes(info, nd, (unsigned)n); /* every node as the provider knows it, instead of the host alone */
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) ipc_write_buf(c, b, info, sizeof info);
}
static void l_ipv4(IpcObject *s, IpcCall *c) { /* 2 GetIpv4Address -> {u32 address, u32 mask} (host byte order) */
    OnlineLdnNetwork net;
    uint32_t r = current_network(s, &net, NULL);
    if (r) {
        c->result = r;
        return;
    }
    uint32_t v[2] = {net.ipv4, 0xffff0000u};
    ipc_out(c, v, 8);
}
/* 3 GetDisconnectReason: the provider says why the last connection ended; without a network nothing ever connected, so "none" (0). */
static void l_disconnect_reason(IpcObject *s, IpcCall *c) {
    const OnlineProvider *p = ldn_provider();
    uint32_t (*get)(void *, OnlineLdnNetwork *, uint16_t *) = p ? ONLINE_FN(p, ldn_get_network) : NULL;
    uint16_t v = 0;
    if (get) {
        OnlineLdnNetwork net;
        get(p->ctx, &net, &v); /* the reason is reported whether or not a network exists any more */
    }
    (void)s;
    IPC_OUT(c, v);
}
static void l_security_parameter(IpcObject *s, IpcCall *c) { /* 4 -> SecurityParameter (0x20 bytes: 16 bytes of data, then the network's SessionId) */
    OnlineLdnNetwork net;
    uint32_t r = current_network(s, &net, NULL);
    if (r) {
        c->result = r;
        return;
    }
    uint8_t v[0x20];
    memcpy(v, net.security_param, 16);
    ldn_session_id(&net, v + 16);
    ipc_out(c, v, sizeof v);
}
static void l_network_config(IpcObject *s, IpcCall *c) { /* 5 -> NetworkConfig (0x20 bytes) */
    OnlineLdnNetwork net;
    uint32_t r = current_network(s, &net, NULL);
    if (r) {
        c->result = r;
        return;
    }
    uint8_t cfg[0x20];
    ldn_config_encode(&net, cfg);
    ipc_out(c, cfg, sizeof cfg);
}
static void l_set_restriction(IpcObject *s, IpcCall *c) { /* 104(u32 restriction) */
    Ldn *l = s->state;
    if (c->in_size >= 4) l->cfg.controller_restriction = IPC_IN(c, uint32_t, 0);
}
static void l_reject(IpcObject *s, IpcCall *c) { /* 205(u32 ip or node id) */
    uint32_t r = ldn_reject(*st(s));             /* without a provider no station exists, so no node can be named */
    const OnlineProvider *p = ldn_provider();
    uint32_t (*rej)(void *, uint32_t) = p ? ONLINE_FN(p, ldn_reject) : NULL;
    if (rej && *st(s) == LDN_AP_CREATED) r = rej(p->ctx, c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0);
    if (r) c->result = r;
}
static void l_set_advertise(IpcObject *s, IpcCall *c) { /* 206(buffer advertise data, <= 0x180 bytes) */
    Ldn *l = s->state;
    const IpcBuf *b = ipc_in_buf(c, 0);
    uint32_t n = b ? (uint32_t)(b->size > 0xffff ? 0xffff : b->size) : 0;
    const void *p = n ? ipc_buf_ptr(b) : NULL;
    uint32_t r = ldn_set_advertise(l->state, &l->cfg, p, p ? n : 0);
    const OnlineProvider *pr = ldn_provider();
    uint32_t (*adv)(void *, const void *, uint32_t) = pr ? ONLINE_FN(pr, ldn_set_advertise) : NULL;
    if (!r && adv && l->state == LDN_AP_CREATED) r = adv(pr->ctx, l->cfg.advertise, l->cfg.advertise_len); /* before CreateNetwork it is only stored */
    if (r) c->result = r;
}
static void l_set_accept_policy(IpcObject *s, IpcCall *c) { /* 207(u8 policy) */
    Ldn *l = s->state;
    uint32_t r = ldn_set_accept_policy(l->state, &l->cfg, c->in_size >= 1 ? IPC_IN(c, uint8_t, 0) : 0);
    if (r) c->result = r;
}
static void l_add_filter(IpcObject *s, IpcCall *c) { /* 208(6-byte address) */
    Ldn *l = s->state;
    uint8_t mac[6] = {0};
    if (c->in_size >= 6) memcpy(mac, c->in, 6);
    uint32_t r = ldn_filter_add(l->state, &l->cfg, mac);
    if (r) c->result = r;
}
static void l_clear_filter(IpcObject *s, IpcCall *c) { /* 209 */
    Ldn *l = s->state;
    uint32_t r = ldn_filter_clear(l->state, &l->cfg);
    if (r) c->result = r;
}
static void l_state_event(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    uint32_t r = 0, w = event_create(&r);
    if (!l->event_w) __atomic_store_n(&l->event_w, w, __ATOMIC_RELEASE);
    ipc_out_copy_handle(c, r);
}
/* 102/103 Scan(...) -> s16 count, NetworkInfo array in buffer 0. The filter argument is not read: the provider filters by the title's local communication id. */
/* A scan takes time on the console: it listens on each channel for about 100 ms (all three for channel 0). Answering at once makes a title's scan screen
 * loop at full speed, creating and destroying its networking objects tens of times a second (MK8D's Wireless Play lobby crashed on a stale job one of
 * those rounds left behind). NAPPLE_LDN_SCAN_MS overrides the time for a full scan (default 400, 0 = instant). */
static void scan_pace(const IpcCall *c, uint64_t t0_ns) {
    const char *e = getenv("NAPPLE_LDN_SCAN_MS");
    int64_t full = e && *e ? atoi(e) : 400;
    uint16_t channel = c->in_size >= 2 ? IPC_IN(c, uint16_t, 0) : 0;
    int64_t want = channel ? full / 3 : full;
    int64_t left = want - (int64_t)((runtime_now_ns() - t0_ns) / 1000000u);
    if (left <= 0) return;
    int tok;
    thread_core_away(&tok);
    struct timespec ts = {left / 1000, (long)(left % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    thread_core_back(&tok);
}
static void l_scan(IpcObject *s, IpcCall *c) {
    uint64_t t0 = runtime_now_ns();
    uint32_t r = ldn_apply(st(s), LDN_OP_SCAN);
    if (r) {
        c->result = r;
        return;
    }
    Ldn *l = s->state;
    int16_t n = 0;
    if (c->in_size >= 0x68) l->cfg.local_communication_id = IPC_IN(c, uint64_t, 8); /* Scan(u16 channel, ScanFilter at +8: NetworkId starts with the id) */
    const OnlineProvider *p = ldn_provider();
    int (*scan)(void *, uint64_t, OnlineLdnNetwork *, uint32_t) = p ? ONLINE_FN(p, ldn_scan) : NULL;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (scan && b) {
        OnlineLdnNetwork nets[ONLINE_LDN_MAX_NETWORKS];
        uint32_t max = (uint32_t)(b->size / LDN_NETINFO_SIZE);
        if (max > ONLINE_LDN_MAX_NETWORKS) max = ONLINE_LDN_MAX_NETWORKS;
        int got = max ? scan(p->ctx, l->cfg.local_communication_id, nets, max) : 0;
        if (got < 0) {
            scan_pace(c, t0);
            c->result = HZ_R_LDN_NETWORK_NOT_AVAILABLE;
            return;
        }
        if ((uint32_t)got > max) got = (int)max;
        for (int i = 0; i < got; i++) {
            uint8_t info[LDN_NETINFO_SIZE];
            ldn_net_encode(&nets[i], info);
            memcpy((uint8_t *)ipc_buf_ptr(b) + (size_t)i * LDN_NETINFO_SIZE, info, sizeof info);
        }
        n = (int16_t)got;
    }
    scan_pace(c, t0);
    IPC_OUT(c, n);
}
static void l_open_ap(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_OPEN_AP); }
/* Leaving a state that holds a network ends it at the provider too. */
static void drop_network(IpcObject *s) {
    if (!ldn_state_has_network(*st(s))) return;
    watch(s, 0);
    const OnlineProvider *p = ldn_provider();
    void (*destroy)(void *) = p ? ONLINE_FN(p, ldn_destroy) : NULL;
    void (*disc)(void *) = p ? ONLINE_FN(p, ldn_disconnect) : NULL;
    void (*f)(void *) = *st(s) == LDN_AP_CREATED ? destroy : disc;
    if (f) f(p->ctx);
}
static void l_close_ap(IpcObject *s, IpcCall *c) {
    drop_network(s);
    apply(s, c, LDN_OP_CLOSE_AP);
}
/* 202/203 CreateNetwork(SecurityConfig, UserConfig, NetworkConfig) */
static void l_create(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    OnlineLdnNetwork cfg;
    ldn_create_decode(c->in, c->in_size, &cfg);
    cfg.advertise_len = l->cfg.advertise_len; /* advertise data set while the access point was only opened */
    memcpy(cfg.advertise, l->cfg.advertise, l->cfg.advertise_len);
    LdnState probe = l->state;
    uint32_t r = ldn_apply(&probe, LDN_OP_CREATE_NETWORK);
    const OnlineProvider *p = ldn_provider();
    uint32_t (*create)(void *, const OnlineLdnNetwork *) = p ? ONLINE_FN(p, ldn_create) : NULL;
    if (!r && create) r = create(p->ctx, &cfg);
    if (r) {
        c->result = r;
        return;
    }
    l->cfg.local_communication_id = cfg.local_communication_id;
    l->state = probe;
    watch(s, 1);
    notify(s);
}
static void l_destroy(IpcObject *s, IpcCall *c) {
    drop_network(s);
    apply(s, c, LDN_OP_DESTROY_NETWORK);
}
static void l_open_station(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_OPEN_STATION); }
static void l_close_station(IpcObject *s, IpcCall *c) {
    drop_network(s);
    apply(s, c, LDN_OP_CLOSE_STATION);
}
/* 302/303 Connect(..., NetworkInfo in buffer 0): the provider joins the network the guest picked from a scan. */
static void l_connect(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    LdnState probe = l->state;
    uint32_t r = ldn_apply(&probe, LDN_OP_CONNECT); /* illegal state, or "nothing to connect to" */
    const OnlineProvider *p = ldn_provider();
    uint32_t (*connect)(void *, const OnlineLdnNetwork *) = p ? ONLINE_FN(p, ldn_connect) : NULL;
    if (connect && l->state == LDN_STATION_OPENED) {
        OnlineLdnNetwork net;
        const IpcBuf *b = ipc_in_buf(c, 0);
        const uint8_t *raw = b && b->size ? ipc_buf_ptr(b) : NULL;
        r = raw && ldn_net_decode(raw, (size_t)b->size, &net) == 0 ? connect(p->ctx, &net) : HZ_R_LDN_NETWORK_NOT_AVAILABLE;
        r = ldn_connect_done(&l->state, r);
        if (!r) watch(s, 1);
    }
    if (r) {
        c->result = r;
        return;
    }
    notify(s);
}
static void l_disconnect(IpcObject *s, IpcCall *c) {
    drop_network(s);
    apply(s, c, LDN_OP_DISCONNECT);
}
/* The ldn client registry RegisterClient searches (switchbrew: "The initialization cmds add an entry to the above global state"): pid -> client. */
#define LDN_MAX_CLIENTS 16
static struct {
    uint64_t pid;
    Ldn *ldn;
} g_clients[LDN_MAX_CLIENTS];
static pthread_mutex_t g_clients_mu = PTHREAD_MUTEX_INITIALIZER;
static void clients_add(uint64_t pid, Ldn *l) {
    pthread_mutex_lock(&g_clients_mu);
    int slot = -1;
    for (int i = 0; i < LDN_MAX_CLIENTS && slot < 0; i++)
        if (g_clients[i].ldn && g_clients[i].pid == pid) slot = i; /* the process initialised again: its newest object */
    for (int i = 0; i < LDN_MAX_CLIENTS && slot < 0; i++)
        if (!g_clients[i].ldn) slot = i;
    if (slot >= 0) g_clients[slot].pid = pid, g_clients[slot].ldn = l;
    pthread_mutex_unlock(&g_clients_mu);
}
static void clients_forget(Ldn *l) {
    pthread_mutex_lock(&g_clients_mu);
    for (int i = 0; i < LDN_MAX_CLIENTS; i++)
        if (g_clients[i].ldn == l) g_clients[i].ldn = NULL;
    pthread_mutex_unlock(&g_clients_mu);
}
/* Action-frame delivery. The provider side (the room / lan layer) calls ldn_push_action_frame when a type-0x21 frame reaches this node; the frame is queued on every
 * live local-communication object that enabled action frames (500) for that LocalCommunicationId (0 = every object), and a blocked 503 is woken. */
static pthread_mutex_t g_af_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_af_cv = PTHREAD_COND_INITIALIZER;
static Ldn *g_af_list;
static void af_register(Ldn *l) {
    pthread_mutex_lock(&g_af_mu);
    l->af_next = g_af_list;
    g_af_list = l;
    pthread_mutex_unlock(&g_af_mu);
}
static void af_unregister(Ldn *l) {
    pthread_mutex_lock(&g_af_mu);
    for (Ldn **p = &g_af_list; *p; p = &(*p)->af_next)
        if (*p == l) {
            *p = l->af_next;
            break;
        }
    pthread_mutex_unlock(&g_af_mu);
}
/* Returns how many objects queued the frame (0 when nobody listens or the frame is empty / too large). */
int ldn_push_action_frame(uint64_t local_communication_id, const uint8_t src[6], const uint8_t bssid[6], uint16_t band_mhz, uint16_t channel, const void *data, size_t n) {
    int queued = 0;
    pthread_mutex_lock(&g_af_mu);
    for (Ldn *l = g_af_list; l; l = l->af_next) {
        uint64_t id;
        memcpy(&id, l->action_frame_settings, 8);
        if (!l->action_frame || (local_communication_id && id && id != local_communication_id)) continue;
        if (ldn_afq_push(&l->afq, src, bssid, band_mhz, channel, data, n) == 0) queued++;
    }
    if (queued) pthread_cond_broadcast(&g_af_cv);
    pthread_mutex_unlock(&g_af_mu);
    return queued;
}
static void af_clear(Ldn *l) {
    pthread_mutex_lock(&g_af_mu);
    ldn_afq_clear(&l->afq);
    pthread_mutex_unlock(&g_af_mu);
}
static void l_initialize(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    int fresh = l->state == LDN_NONE;
    apply(s, c, LDN_OP_INITIALIZE);
    if (c->result || !fresh) return;
    /* "The Initialize* cmds configure state the same as using SetProtocol with Protocol NX"; the rest is the state init of a new client */
    l->protocol = LDN_PROTOCOL_NX;
    l->operation_mode = 0; /* Stable */
    l->action_frame = 0;
    af_clear(l);
    l->home_band = l->home_channel = 0;
    l->tx_power = -1;
    if (c->has_pid && c->pid) clients_add(c->pid, l);
}
static void l_finalize(IpcObject *s, IpcCall *c) {
    drop_network(s); /* Finalize from any state ends a hosted or joined network too */
    apply(s, c, LDN_OP_FINALIZE);
}

/* 105 SetWirelessAudioPolicy(u32 BluetoothAudioDeviceConnectableMode): the page gives no State rule; the value is kept (it reaches btm on the console,
 * which napple does not model further). */
static void l_set_audio_policy(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    if (c->in_size >= 4) l->audio_policy = IPC_IN(c, uint32_t, 0);
}
/* 106 SetProtocol(u32 Protocol), the 20.0.0+ behaviour: non-zero and permitted (1 or 3 on NX), State Initialized; kept. */
static void l_set_protocol(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    uint32_t v = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0;
    uint32_t r = ldn_check_protocol(l->state, v);
    if (r) c->result = r;
    else l->protocol = v;
}
/* 403 SetOperationMode(u32 OperationMode: 0 Stable, 1 HighSpeed): State must be Initialized, "the input value is written into state". */
static void l_set_operation_mode(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    if (l->state != LDN_INITIALIZED) {
        c->result = LDN_R_INVALID_STATE;
        return;
    }
    l->operation_mode = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0;
}
/* 500 EnableActionFrame(ActionFrameSettings 0x80) / 501 DisableActionFrame: State Initialized; the settings are checked and kept. */
static void l_enable_action_frame(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    if (l->state != LDN_INITIALIZED) {
        c->result = LDN_R_INVALID_STATE;
        return;
    }
    uint32_t r = ldn_check_action_frame_settings(c->in, c->in_size);
    if (r) {
        c->result = r;
        return;
    }
    pthread_mutex_lock(&g_af_mu);
    memcpy(l->action_frame_settings, c->in, LDN_ACTION_FRAME_SETTINGS_SIZE);
    ldn_afq_clear(&l->afq);
    l->action_frame = 1;
    pthread_mutex_unlock(&g_af_mu);
}
static void l_disable_action_frame(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    if (l->state != LDN_INITIALIZED) c->result = LDN_R_INVALID_STATE;
    else {
        pthread_mutex_lock(&g_af_mu);
        l->action_frame = 0;
        ldn_afq_clear(&l->afq);
        pthread_cond_broadcast(&g_af_cv);
        pthread_mutex_unlock(&g_af_mu);
    }
}
/* 502 SendActionFrame(type-0x21 frame; MacAddress destination at 0, MacAddress bssid at 6, channel argument at 0xC (ldn_decode_channel), MessageFlagSet at
 * 0x10 [offsets unverified: the page lists the fields in this order, not their offsets]): State AccessPointCreated / Station / StationConnected,
 * ChannelNumber non-zero. The frame goes out over the radio and nobody answers: the OnlineProvider has no action-frame hook, so neither the NullProvider
 * nor NAPPLE_NET=host carries it to another node (the receive side is ldn_push_action_frame, below; a send hook is still missing). Sending into the
 * air succeeds, blocking (flag bit0 clear) or not. The buffer limit is lp2p SendToOtherGroup's 0x400 bytes [unverified for ldn]. */
static void l_send_action_frame(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    if (!ldn_state_in(l->state, LDN_STATES(LDN_AP_CREATED, LDN_STATION_OPENED, LDN_STATION_CONNECTED))) {
        c->result = LDN_R_INVALID_STATE;
        return;
    }
    const IpcBuf *b = ipc_in_buf(c, 0);
    uint16_t band, ch;
    if (!b || !b->size || b->size > LDN_ACTION_FRAME_MAX || c->in_size < 0x10 || ldn_decode_channel(c->in + 0xc, c->in_size - 0xc, &band, &ch)) {
        c->result = LDN_R_INVALID_ARGUMENT;
        return;
    }
    l->frames_sent++;
}
/* 503 RecvActionFrame(u32 MessageFlagSet; bit 0 set = do not block, as in 502) -> {src mac, bssid, band, channel, u32 size} (ldn_action_recv_encode) + the frame in output
 * buffer 0. EnableActionFrame must come first (invalid state otherwise). The oldest queued frame is handed out; a frame longer than the buffer is cut and the size says
 * how many bytes were written. With nothing queued a non-blocking call answers "network not available" (the console's answer is undocumented); a blocking call waits
 * up to LDN_RECV_WAIT_MS for a push and then answers the same, since a service handler cannot wait forever. */
#define LDN_RECV_WAIT_MS 200
static void l_recv_action_frame(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    if (!l->action_frame) {
        c->result = LDN_R_INVALID_STATE;
        return;
    }
    int nonblock = c->in_size >= 4 && (IPC_IN(c, uint32_t, 0) & 1u);
    LdnActionFrame *f = malloc(sizeof *f);
    if (!f) {
        c->result = LDN_R_INVALID_STATE;
        return;
    }
    int got;
    pthread_mutex_lock(&g_af_mu);
    got = ldn_afq_pop(&l->afq, f);
    if (got && !nonblock) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += LDN_RECV_WAIT_MS * 1000000L;
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        while (!l->afq.count && l->action_frame && pthread_cond_timedwait(&g_af_cv, &g_af_mu, &ts) == 0) {
        }
        got = ldn_afq_pop(&l->afq, f);
    }
    pthread_mutex_unlock(&g_af_mu);
    if (got) {
        free(f);
        c->result = HZ_R_LDN_NETWORK_NOT_AVAILABLE;
        return;
    }
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint32_t n = b ? (uint32_t)ipc_write_buf(c, b, f->data, f->size) : 0;
    uint8_t out[LDN_ACTION_RECV_OUT_SIZE];
    ldn_action_recv_encode(f, n, out);
    free(f);
    ipc_out(c, out, sizeof out);
}
/* 505 SetHomeChannel(channel argument, ldn_decode_channel): State Station, band 2.4 or 5 GHz, ChannelNumber non-zero; kept. */
static void l_set_home_channel(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    uint16_t band, ch;
    if (l->state != LDN_STATION_OPENED) c->result = LDN_R_INVALID_STATE;
    else if (ldn_decode_channel(c->in, c->in_size, &band, &ch)) c->result = LDN_R_INVALID_ARGUMENT;
    else l->home_band = band, l->home_channel = ch;
}
/* 600 SetTxPower(s16 Power, 0..0xFF) / 601 ResetTxPower: State AccessPoint, AccessPointCreated, Station or StationConnected. The page also names a "state
 * field" that must be non-zero without saying which; napple has no such field and does not check it. */
static int tx_state_ok(LdnState s) { return ldn_state_in(s, LDN_STATES(LDN_AP_OPENED, LDN_AP_CREATED, LDN_STATION_OPENED, LDN_STATION_CONNECTED)); }
static void l_set_tx_power(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    int16_t v = c->in_size >= 2 ? IPC_IN(c, int16_t, 0) : -1;
    if (v < 0 || v > 0xff) c->result = LDN_R_INVALID_ARGUMENT;
    else if (!tx_state_ok(l->state)) c->result = LDN_R_INVALID_STATE;
    else l->tx_power = v;
}
static void l_reset_tx_power(IpcObject *s, IpcCall *c) {
    Ldn *l = s->state;
    if (!tx_state_ok(l->state)) c->result = LDN_R_INVALID_STATE;
    else l->tx_power = -1;
}

static const IpcCmd local_cmds[] = {
    {0, "GetState", l_get_state, IPC_IMPL},
    {1, "GetNetworkInfo", l_network_info, IPC_IMPL},
    {2, "GetIpv4Address", l_ipv4, IPC_IMPL},
    {3, "GetDisconnectReason", l_disconnect_reason, IPC_IMPL},
    {4, "GetSecurityParameter", l_security_parameter, IPC_IMPL},
    {5, "GetNetworkConfig", l_network_config, IPC_IMPL},
    {100, "GetStateChangeEvent", l_state_event, IPC_IMPL},
    {101, "GetNetworkInfoAndHistory", l_network_info, IPC_IMPL},
    {102, "Scan", l_scan, IPC_IMPL},
    {103, "ScanPrivate", l_scan, IPC_IMPL},
    {104, "SetWirelessControllerPolicy", l_set_restriction, IPC_IMPL},
    {200, "OpenAccessPoint", l_open_ap, IPC_IMPL},
    {201, "CloseAccessPoint", l_close_ap, IPC_IMPL},
    {202, "CreateNetwork", l_create, IPC_IMPL},
    {203, "CreateNetworkPrivate", l_create, IPC_IMPL},
    {204, "DestroyNetwork", l_destroy, IPC_IMPL},
    {205, "Reject", l_reject, IPC_IMPL},
    {206, "SetAdvertiseData", l_set_advertise, IPC_IMPL},
    {207, "SetStationAcceptPolicy", l_set_accept_policy, IPC_IMPL},
    {208, "AddAcceptFilterEntry", l_add_filter, IPC_IMPL},
    {209, "ClearAcceptFilter", l_clear_filter, IPC_IMPL},
    {300, "OpenStation", l_open_station, IPC_IMPL},
    {301, "CloseStation", l_close_station, IPC_IMPL},
    {302, "Connect", l_connect, IPC_IMPL},
    {303, "ConnectPrivate", l_connect, IPC_IMPL},
    {304, "Disconnect", l_disconnect, IPC_IMPL},
    {400, "Initialize", l_initialize, IPC_IMPL},
    {401, "Finalize", l_finalize, IPC_IMPL},
    {402, "InitializeWithVersion", l_initialize, IPC_IMPL},
    {105, "SetWirelessAudioPolicy", l_set_audio_policy, IPC_IMPL},
    {106, "SetProtocol", l_set_protocol, IPC_IMPL},
    {403, "SetOperationMode", l_set_operation_mode, IPC_IMPL},
    {500, "EnableActionFrame", l_enable_action_frame, IPC_IMPL},
    {501, "DisableActionFrame", l_disable_action_frame, IPC_IMPL},
    {502, "SendActionFrame", l_send_action_frame, IPC_IMPL},
    {503, "RecvActionFrame", l_recv_action_frame, IPC_IMPL},
    {505, "SetHomeChannel", l_set_home_channel, IPC_IMPL},
    {600, "SetTxPower", l_set_tx_power, IPC_IMPL},
    {601, "ResetTxPower", l_reset_tx_power, IPC_IMPL},
};
/* Closing the service object ends a network it still hosts or joined, and stops the provider's notifications to it. */
static void ldn_teardown(Ldn *l) {
    IpcObject o = {.state = l};
    drop_network(&o);
    watch(&o, 0); /* no notification may reach the freed state */
    af_unregister(l);
    free(l);
}
/* An IClientProcessMonitor that registered this client holds a reference to it (switchbrew: the object "is then incref'd and written into the
 * IClientProcessMonitor state"): the client, and a network it hosts or joined, then lives until the monitor is closed too. */
static void ldn_local_destroy(IpcObject *o) {
    Ldn *l = o->state;
    clients_forget(l);
    if (l->monitor) {
        l->orphan = 1;
        return;
    }
    ldn_teardown(l);
}
static void ldn_create_local(IpcObject *s, IpcCall *c) {
    (void)s;
    Ldn *l = calloc(1, sizeof *l);
    af_register(l);
    ipc_out_object(c, IPC_OBJECT("IUserLocalCommunicationService", local_cmds, l, ldn_local_destroy));
}
/* ldn:u 1 CreateClientProcessMonitor [18.0.0+] -> IClientProcessMonitor { 0 RegisterClient(PID, u64 pid_placeholder) }; sdknso uses the pair at the end of
 * its ldn initialisation. RegisterClient, per switchbrew: already registered -> 0; no registry entry for the PID -> 0; PID 0 -> 0; otherwise the client
 * is referenced by the monitor and its registry entry is cleared; 0. Every path answers success. */
typedef struct LdnMonitor {
    Ldn *client;
} LdnMonitor;
static void mon_register_client(IpcObject *s, IpcCall *c) {
    LdnMonitor *m = s->state;
    uint64_t pid = c->has_pid ? c->pid : 0;
    if (m->client || !pid) return;
    pthread_mutex_lock(&g_clients_mu);
    for (int i = 0; i < LDN_MAX_CLIENTS; i++)
        if (g_clients[i].ldn && g_clients[i].pid == pid) {
            Ldn *l = g_clients[i].ldn;
            g_clients[i].ldn = NULL;
            if (!l->monitor) { /* a client is held by one monitor */
                m->client = l;
                l->monitor = m;
            }
            break;
        }
    pthread_mutex_unlock(&g_clients_mu);
}
static const IpcCmd monitor_cmds[] = {{0, "RegisterClient", mon_register_client, IPC_IMPL}};
static void monitor_destroy(IpcObject *o) {
    LdnMonitor *m = o->state;
    Ldn *l = m->client;
    if (l) {
        l->monitor = NULL;
        if (l->orphan) ldn_teardown(l); /* the last reference to a client whose service object is already closed */
    }
    free(m);
}
static void ldn_create_monitor(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IClientProcessMonitor", monitor_cmds, calloc(1, sizeof(LdnMonitor)), monitor_destroy));
}
static const IpcCmd ldn_cmds[] = {
    {0, "CreateUserLocalCommunicationService", ldn_create_local, IPC_IMPL},
    {1, "CreateClientProcessMonitor", ldn_create_monitor, IPC_IMPL},
};
static IpcObject *ldn_open(void) { return IPC_OBJECT("ldn:u", ldn_cmds, NULL, NULL); }
void service_register_ldn(void) { ipc_register_service("ldn:u", ldn_open); }
