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
static void l_disconnect_reason(IpcObject *s, IpcCall *c) {
    (void)s;
    uint16_t v = 0;
    IPC_OUT(c, v);
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
    {3, "GetDisconnectReason", l_disconnect_reason, IPC_STUB},
    {4, "GetSecurityParameter", l_need_network, IPC_STUB},
    {5, "GetNetworkConfig", l_need_network, IPC_STUB},
    {100, "AttachStateChangeEvent", l_state_event, IPC_IMPL},
    {101, "GetNetworkInfoLatestUpdate", l_need_network, IPC_STUB},
    {102, "Scan", l_scan, IPC_IMPL},
    {103, "ScanPrivate", l_scan, IPC_IMPL},
    {104, "SetWirelessControllerRestriction", svc_noop, IPC_STUB},
    {200, "OpenAccessPoint", l_open_ap, IPC_IMPL},
    {201, "CloseAccessPoint", l_close_ap, IPC_IMPL},
    {202, "CreateNetwork", l_create, IPC_IMPL},
    {203, "CreateNetworkPrivate", l_create, IPC_IMPL},
    {204, "DestroyNetwork", l_destroy, IPC_IMPL},
    {205, "Reject", svc_noop, IPC_STUB},
    {206, "SetAdvertiseData", svc_noop, IPC_STUB},
    {207, "SetStationAcceptPolicy", svc_noop, IPC_STUB},
    {208, "AddAcceptFilterEntry", svc_noop, IPC_STUB},
    {209, "ClearAcceptFilter", svc_noop, IPC_STUB},
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
