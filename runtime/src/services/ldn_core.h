/* ldn core: local-wireless state machine (portable). Models the lifecycle the SDK drives (initialize, open access point / station,
 * create network, connect, ...) and which transitions are legal, so a title that polls GetState sees a coherent sequence. There is no
 * radio: scans find nothing and a station can never connect. */
#pragma once
#include <stdint.h>

typedef enum {
    LDN_NONE = 0,
    LDN_INITIALIZED = 1,
    LDN_AP_OPENED = 2,
    LDN_AP_CREATED = 3,
    LDN_STATION_OPENED = 4,
    LDN_STATION_CONNECTED = 5,
    LDN_ERROR = 6
} LdnState;
typedef enum {
    LDN_OP_INITIALIZE,
    LDN_OP_FINALIZE,
    LDN_OP_OPEN_AP,
    LDN_OP_CLOSE_AP,
    LDN_OP_CREATE_NETWORK,
    LDN_OP_DESTROY_NETWORK,
    LDN_OP_OPEN_STATION,
    LDN_OP_CLOSE_STATION,
    LDN_OP_CONNECT,
    LDN_OP_DISCONNECT,
    LDN_OP_SCAN
} LdnOp;

/* Applies `op`. Returns 0 and updates *state, or a Horizon result describing why the call is not allowed and leaves *state alone. */
uint32_t ldn_apply(LdnState *state, LdnOp op);

/* Access-point configuration held per local-communication object (set by the host side of a session). */
#define LDN_ADVERTISE_MAX 0x180
#define LDN_FILTER_MAX 8
typedef struct {
    uint8_t advertise[LDN_ADVERTISE_MAX];
    uint16_t advertise_len;
    uint32_t accept_policy; /* 0 allow all, 1 deny all, 2 allow only listed, 3 deny listed */
    uint8_t filter[LDN_FILTER_MAX][6];
    unsigned nfilter;
    uint32_t controller_restriction;
} LdnConfig;
/* The calls below are only legal while an access point has created its network (state AP_CREATED); otherwise they return the invalid-state result. */
uint32_t ldn_set_advertise(LdnState state, LdnConfig *cfg, const void *data, uint32_t n); /* n > LDN_ADVERTISE_MAX: invalid size */
uint32_t ldn_set_accept_policy(LdnState state, LdnConfig *cfg, uint32_t policy);
uint32_t ldn_filter_add(LdnState state, LdnConfig *cfg, const uint8_t mac[6]);
uint32_t ldn_filter_clear(LdnState state, LdnConfig *cfg);
/* Reject(ip): the created network has no stations, so no node can be named. */
uint32_t ldn_reject(LdnState state);
/* GetNetworkInfo-style queries: the host never joins or hosts a network anyone can see, so they report "not available" in every state. */
uint32_t ldn_network_query(LdnState state);
