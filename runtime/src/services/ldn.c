/* ldn:u (nn::ldn): local wireless play. A state machine (ldn_core) tracks initialize / open / create like the real service. The transport is the active
 * OnlineProvider's (ONLINE_CAP_LDN: scan, host, join, network description); the NullProvider has no radio, so scans return no networks, connecting fails and a
 * created network never gains stations. State is per local-communication object.
 * Command ids are from the author's memory of the public documentation (unverified). */
#include "svc_common.h"
#include "ldn_core.h"
#include "hz_result.h"
#include "online_provider.h"
#include <stdlib.h>

/* The provider's LDN table when it can carry local wireless (ONLINE_CAP_LDN), else NULL: the NullProvider answers apply. */
static const OnlineProvider *ldn_provider(void) {
    const OnlineProvider *p = online_provider();
    return online_has(p, ONLINE_CAP_LDN) ? p : NULL;
}

typedef struct {
    LdnState state;
    uint32_t event_w;
    LdnConfig cfg;
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

static void l_get_state(IpcObject *s, IpcCall *c) {
    uint32_t v = (uint32_t)*st(s);
    IPC_OUT(c, v);
}
/* The network the provider says is hosted or joined, only while the state machine holds one. Returns 0 or the result for the guest. */
static uint32_t current_network(IpcObject *s, OnlineLdnNetwork *net, uint16_t *reason) {
    const OnlineProvider *p = ldn_provider();
    uint32_t (*get)(void *, OnlineLdnNetwork *, uint16_t *) = p ? ONLINE_FN(p, ldn_get_network) : NULL;
    uint16_t dummy;
    memset(net, 0, sizeof *net);
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
static void l_security_parameter(IpcObject *s, IpcCall *c) { /* 4 -> security parameter (16 bytes) */
    OnlineLdnNetwork net;
    uint32_t r = current_network(s, &net, NULL);
    if (r) {
        c->result = r;
        return;
    }
    ipc_out(c, net.security_param, 16);
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
    if (!r && adv) r = adv(pr->ctx, l->cfg.advertise, l->cfg.advertise_len);
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
    if (!l->event_w) l->event_w = w;
    ipc_out_copy_handle(c, r);
}
/* 102/103 Scan(...) -> s16 count, NetworkInfo array in buffer 0. The filter argument is not read: the provider filters by the title's local communication id. */
static void l_scan(IpcObject *s, IpcCall *c) {
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
    IPC_OUT(c, n);
}
static void l_open_ap(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_OPEN_AP); }
/* Leaving a state that holds a network ends it at the provider too. */
static void drop_network(IpcObject *s) {
    if (!ldn_state_has_network(*st(s))) return;
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
static void l_initialize(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_INITIALIZE); }
static void l_finalize(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_FINALIZE); }

static const IpcCmd local_cmds[] = {
    {0, "GetState", l_get_state, IPC_IMPL},
    {1, "GetNetworkInfo", l_network_info, IPC_IMPL},
    {2, "GetIpv4Address", l_ipv4, IPC_IMPL},
    {3, "GetDisconnectReason", l_disconnect_reason, IPC_IMPL},
    {4, "GetSecurityParameter", l_security_parameter, IPC_IMPL},
    {5, "GetNetworkConfig", l_network_config, IPC_IMPL},
    {100, "AttachStateChangeEvent", l_state_event, IPC_IMPL},
    {101, "GetNetworkInfoLatestUpdate", l_network_info, IPC_IMPL},
    {102, "Scan", l_scan, IPC_IMPL},
    {103, "ScanPrivate", l_scan, IPC_IMPL},
    {104, "SetWirelessControllerRestriction", l_set_restriction, IPC_IMPL},
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
    {402, "InitializeSystem2", l_initialize, IPC_IMPL},
};
static void ldn_create_local(IpcObject *s, IpcCall *c) {
    (void)s;
    Ldn *l = calloc(1, sizeof *l);
    ipc_out_object(c, IPC_OBJECT("IUserLocalCommunicationService", local_cmds, l, NULL));
}
static const IpcCmd ldn_cmds[] = {{0, "CreateUserLocalCommunicationService", ldn_create_local, IPC_IMPL}};
static IpcObject *ldn_open(void) { return IPC_OBJECT("ldn:u", ldn_cmds, NULL, NULL); }
void service_register_ldn(void) { ipc_register_service("ldn:u", ldn_open); }
