/* ldn:u (nn::ldn): local wireless play. A state machine (ldn_core) tracks initialize / open / create like the real service, but there is
 * no radio: scans return no networks, connecting fails, a created network never gains stations. State is per local-communication object.
 * Command ids are from the author's memory of the public documentation (unverified). */
#include "svc_common.h"
#include "ldn_core.h"
#include "hz_result.h"
#include <stdlib.h>

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
static void l_need_network(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = HZ_R_LDN_NETWORK_NOT_AVAILABLE;
} /* GetNetworkInfo, GetIpv4Address, ... : no network joined or created */
/* Nothing ever connects, so the last disconnect reason is always "none" (0). */
static void l_disconnect_reason(IpcObject *s, IpcCall *c) {
    (void)s;
    uint16_t v = 0;
    IPC_OUT(c, v);
}
/* Rows that ask for the network's description: nothing is ever joined or visible, so the answer is "not available" in every state (class c). */
static void l_query(IpcObject *s, IpcCall *c) {
    uint32_t r = ldn_network_query(*st(s));
    if (r) c->result = r;
}
static void l_set_restriction(IpcObject *s, IpcCall *c) { /* 104(u32 restriction) */
    Ldn *l = s->state;
    if (c->in_size >= 4) l->cfg.controller_restriction = IPC_IN(c, uint32_t, 0);
}
static void l_reject(IpcObject *s, IpcCall *c) { /* 205(u32 ip or node id) */
    uint32_t r = ldn_reject(*st(s));
    if (r) c->result = r;
}
static void l_set_advertise(IpcObject *s, IpcCall *c) { /* 206(buffer advertise data, <= 0x180 bytes) */
    Ldn *l = s->state;
    const IpcBuf *b = ipc_in_buf(c, 0);
    uint32_t n = b ? (uint32_t)(b->size > 0xffff ? 0xffff : b->size) : 0;
    const void *p = n ? ipc_buf_ptr(b) : NULL;
    uint32_t r = ldn_set_advertise(l->state, &l->cfg, p, p ? n : 0);
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
static void l_scan(IpcObject *s, IpcCall *c) {
    uint32_t r = ldn_apply(st(s), LDN_OP_SCAN);
    if (r) {
        c->result = r;
        return;
    }
    int16_t n = 0;
    IPC_OUT(c, n);
}
static void l_open_ap(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_OPEN_AP); }
static void l_close_ap(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_CLOSE_AP); }
static void l_create(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_CREATE_NETWORK); }
static void l_destroy(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_DESTROY_NETWORK); }
static void l_open_station(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_OPEN_STATION); }
static void l_close_station(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_CLOSE_STATION); }
static void l_connect(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_CONNECT); }
static void l_disconnect(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_DISCONNECT); }
static void l_initialize(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_INITIALIZE); }
static void l_finalize(IpcObject *s, IpcCall *c) { apply(s, c, LDN_OP_FINALIZE); }

static const IpcCmd local_cmds[] = {
    {0, "GetState", l_get_state, IPC_IMPL},
    {1, "GetNetworkInfo", l_need_network, IPC_IMPL},
    {2, "GetIpv4Address", l_need_network, IPC_IMPL},
    {3, "GetDisconnectReason", l_disconnect_reason, IPC_IMPL},
    {4, "GetSecurityParameter", l_query, IPC_IMPL},
    {5, "GetNetworkConfig", l_query, IPC_IMPL},
    {100, "AttachStateChangeEvent", l_state_event, IPC_IMPL},
    {101, "GetNetworkInfoLatestUpdate", l_query, IPC_IMPL},
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
