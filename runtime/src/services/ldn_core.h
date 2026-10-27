/* ldn core: local-wireless state machine (portable). Models the lifecycle the SDK drives (initialize, open access point / station,
 * create network, connect, ...) and which transitions are legal, so a title that polls GetState sees a coherent sequence. There is no
 * radio: scans find nothing and a station can never connect. */
#pragma once
#include "online_provider.h"
#include <stddef.h>
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
    uint64_t local_communication_id; /* the title's id: from CreateNetwork's NetworkConfig, or a scan's filter */
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

/* ---- provider-backed networks (ONLINE_CAP_LDN) ----
 * The wire shapes are the guest's: NetworkInfo is 0x480 bytes. Offsets (the author's memory of the public description, unverified on a console):
 * 0x00 u64 local communication id, 0x0a u16 scene id | 0x20 bssid[6], 0x26 u8 ssid length, 0x27 ssid[33], 0x48 s16 channel, 0x4a s8 link level, 0x4b u8 network type
 * | 0x50 security parameter[16], 0x60 u16 security mode, 0x62 u8 accept policy, 0x66 u8 node count max, 0x67 u8 node count, 0x68 nodes[8] x 0x40
 * (u32 ipv4 at +0, mac at +4, s8 node id at +0xa, u8 connected at +0xb, user name at +0xc) | 0x268 u16 advertise size, 0x26a advertise[0x180]. */
#define LDN_NETINFO_SIZE 0x480u
#define LDN_NODE_SIZE 0x40u
void ldn_net_encode(const OnlineLdnNetwork *n, uint8_t out[LDN_NETINFO_SIZE]);
/* Reads the identifying fields back (what a station passes to Connect). Returns 0, or -1 when the buffer is shorter than a NetworkInfo. */
int ldn_net_decode(const uint8_t *in, size_t n, OnlineLdnNetwork *out);
/* CreateNetwork arguments: SecurityConfig (u16 mode, u16 passphrase size, passphrase[0x40]) at 0, UserConfig (user name[0x21]) at 0x44, NetworkConfig
 * (u64 local communication id, u16 scene id at +0x0a, s16 channel at +0x10, u8 node count max at +0x12, u16 version at +0x14) at 0x78. Short input leaves zeros. */
void ldn_create_decode(const uint8_t *in, size_t n, OnlineLdnNetwork *out);
/* NetworkConfig (0x20 bytes) of a network, as GetNetworkConfig returns it. */
void ldn_config_encode(const OnlineLdnNetwork *n, uint8_t out[0x20]);
/* Connect finished with the provider's result: 0 moves STATION_OPENED to STATION_CONNECTED; a failure leaves the state alone and is returned. */
uint32_t ldn_connect_done(LdnState *state, uint32_t provider_result);
/* Whether the state holds a network the provider has to tear down when the state is left (AP created, station connected). */
int ldn_state_has_network(LdnState state);
