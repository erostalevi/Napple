#include "ldn_core.h"
#include "hz_result.h"
#include <string.h>

#define R_INVALID_STATE HZ_RESULT(HZ_MOD_LDN, 32) /* [unverified] call not allowed in the current state */

uint32_t ldn_apply(LdnState *s, LdnOp op) {
    switch (op) {
        case LDN_OP_INITIALIZE:
            if (*s == LDN_NONE) *s = LDN_INITIALIZED;
            return 0; /* initialising twice is harmless */
        case LDN_OP_FINALIZE: *s = LDN_NONE; return 0;
        case LDN_OP_OPEN_AP:
            if (*s != LDN_INITIALIZED) return R_INVALID_STATE;
            *s = LDN_AP_OPENED;
            return 0;
        case LDN_OP_CLOSE_AP:
            if (*s != LDN_AP_OPENED && *s != LDN_AP_CREATED) return R_INVALID_STATE;
            *s = LDN_INITIALIZED;
            return 0;
        case LDN_OP_CREATE_NETWORK:
            if (*s != LDN_AP_OPENED) return R_INVALID_STATE;
            *s = LDN_AP_CREATED;
            return 0;
        case LDN_OP_DESTROY_NETWORK:
            if (*s != LDN_AP_CREATED) return R_INVALID_STATE;
            *s = LDN_AP_OPENED;
            return 0;
        case LDN_OP_OPEN_STATION:
            if (*s != LDN_INITIALIZED) return R_INVALID_STATE;
            *s = LDN_STATION_OPENED;
            return 0;
        case LDN_OP_CLOSE_STATION:
            if (*s != LDN_STATION_OPENED && *s != LDN_STATION_CONNECTED) return R_INVALID_STATE;
            *s = LDN_INITIALIZED;
            return 0;
        case LDN_OP_CONNECT:
            if (*s != LDN_STATION_OPENED) return R_INVALID_STATE;
            return HZ_R_LDN_NETWORK_NOT_AVAILABLE; /* nothing to connect to */
        case LDN_OP_DISCONNECT:
            if (*s != LDN_STATION_CONNECTED) return R_INVALID_STATE;
            *s = LDN_STATION_OPENED;
            return 0;
        case LDN_OP_SCAN:
            if (*s != LDN_STATION_OPENED) return R_INVALID_STATE;
            return 0;
    }
    return R_INVALID_STATE;
}

#define R_INVALID_SIZE HZ_RESULT(HZ_MOD_LDN, 2)    /* [unverified] */
#define R_INVALID_ARG HZ_RESULT(HZ_MOD_LDN, 4)     /* [unverified] */
#define R_FILTER_FULL HZ_RESULT(HZ_MOD_LDN, 12)    /* [unverified] */
#define R_NODE_NOT_FOUND HZ_RESULT(HZ_MOD_LDN, 24) /* [unverified] */
/* The access point's configuration can be set once it is opened, before CreateNetwork too (MK8D sets its advertise data first); CreateNetwork uses it. */
static int ap_open(LdnState s) { return s == LDN_AP_OPENED || s == LDN_AP_CREATED; }
uint32_t ldn_set_advertise(LdnState state, LdnConfig *cfg, const void *data, uint32_t n) {
    if (!ap_open(state)) return R_INVALID_STATE;
    if (n > LDN_ADVERTISE_MAX) return R_INVALID_SIZE;
    if (n) memcpy(cfg->advertise, data, n);
    cfg->advertise_len = (uint16_t)n;
    return 0;
}
uint32_t ldn_set_accept_policy(LdnState state, LdnConfig *cfg, uint32_t policy) {
    if (!ap_open(state)) return R_INVALID_STATE;
    if (policy > 3) return R_INVALID_ARG;
    cfg->accept_policy = policy;
    return 0;
}
uint32_t ldn_filter_add(LdnState state, LdnConfig *cfg, const uint8_t mac[6]) {
    if (!ap_open(state)) return R_INVALID_STATE;
    for (unsigned i = 0; i < cfg->nfilter; i++)
        if (!memcmp(cfg->filter[i], mac, 6)) return 0;
    if (cfg->nfilter >= LDN_FILTER_MAX) return R_FILTER_FULL;
    memcpy(cfg->filter[cfg->nfilter++], mac, 6);
    return 0;
}
uint32_t ldn_filter_clear(LdnState state, LdnConfig *cfg) {
    if (!ap_open(state)) return R_INVALID_STATE;
    cfg->nfilter = 0;
    return 0;
}
uint32_t ldn_reject(LdnState state) { return state == LDN_AP_CREATED ? R_NODE_NOT_FOUND : R_INVALID_STATE; }
uint32_t ldn_network_query(LdnState state) {
    (void)state;
    return HZ_R_LDN_NETWORK_NOT_AVAILABLE;
}

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint16_t get16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static uint64_t get64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
/* NetworkId.sessionId (NetworkInfo +0x10, SecurityParameter +0x10): random per network on a console. The provider ABI has no field for it, so it is
 * derived from the network's random bssid and security data: every node of a network computes the same id. */
void ldn_session_id(const OnlineLdnNetwork *n, uint8_t out[16]) {
    for (unsigned i = 0; i < 16; i++) out[i] = (uint8_t)(n->security_param[15 - i] ^ n->bssid[i % 6] ^ (0x5a + i));
}
void ldn_net_encode(const OnlineLdnNetwork *n, uint8_t out[LDN_NETINFO_SIZE]) {
    memset(out, 0, LDN_NETINFO_SIZE);
    put64(out, n->local_communication_id);
    put16(out + 0x0a, n->scene_id);
    ldn_session_id(n, out + 0x10);
    memcpy(out + 0x20, n->bssid, 6);
    size_t sl = strnlen(n->ssid, sizeof n->ssid - 1);
    out[0x26] = (uint8_t)sl;
    memcpy(out + 0x27, n->ssid, sl);
    put16(out + 0x48, n->channel);
    out[0x4a] = 3; /* link level: best */
    out[0x4b] = 2; /* network type: LDN */
    memcpy(out + 0x50, n->security_param, 16);
    put16(out + 0x60, n->security_mode);
    out[0x66] = n->node_count_max;
    out[0x67] = n->node_count;
    unsigned shown = n->node_count < 8 ? n->node_count : 8;
    for (unsigned i = 0; i < shown; i++) { /* node 0 is the host */
        uint8_t *node = out + 0x68 + i * LDN_NODE_SIZE;
        if (i == 0) {
            put32(node, n->ipv4);
            memcpy(node + 0xc, n->host_name, strnlen(n->host_name, 0x20));
        }
        node[0xa] = (uint8_t)i;
        node[0xb] = 1;
        put16(node + 0x2e, n->local_communication_version); /* NodeInfo.localCommunicationVersion: a station joins with the version it scanned */
    }
    uint32_t al = n->advertise_len > ONLINE_LDN_ADVERTISE_MAX ? ONLINE_LDN_ADVERTISE_MAX : n->advertise_len;
    put16(out + 0x268, (uint16_t)al);
    memcpy(out + 0x26a, n->advertise, al);
}
void ldn_net_encode_nodes(uint8_t out[LDN_NETINFO_SIZE], const OnlineLdnNode *nodes, unsigned n) {
    unsigned count = 0;
    uint16_t version = get16(out + 0x68 + 0x2e); /* what ldn_net_encode put on node 0 */
    if (n > 8) n = 8;
    memset(out + 0x68, 0, 8 * LDN_NODE_SIZE);
    for (unsigned i = 0; i < n; i++) {
        uint8_t *node = out + 0x68 + i * LDN_NODE_SIZE;
        put32(node, nodes[i].ipv4);
        memcpy(node + 4, nodes[i].mac, 6);
        node[0xa] = (uint8_t)i;
        node[0xb] = nodes[i].connected ? 1 : 0;
        memcpy(node + 0xc, nodes[i].user_name, strnlen(nodes[i].user_name, 0x20));
        if (nodes[i].connected) put16(node + 0x2e, version);
        count += nodes[i].connected ? 1 : 0;
    }
    out[0x67] = (uint8_t)count;
}
int ldn_net_decode(const uint8_t *in, size_t n, OnlineLdnNetwork *out) {
    memset(out, 0, sizeof *out);
    if (n < 0x270) return -1;
    out->local_communication_id = get64(in);
    out->scene_id = get16(in + 0x0a);
    memcpy(out->bssid, in + 0x20, 6);
    size_t sl = in[0x26] < sizeof out->ssid - 1 ? in[0x26] : sizeof out->ssid - 1;
    memcpy(out->ssid, in + 0x27, sl);
    out->channel = (uint8_t)get16(in + 0x48);
    memcpy(out->security_param, in + 0x50, 16);
    out->security_mode = (uint8_t)get16(in + 0x60);
    out->node_count_max = in[0x66];
    out->node_count = in[0x67];
    memcpy(out->host_name, in + 0x68 + 0xc, 0x20);
    out->host_name[32] = 0;
    out->local_communication_version = get16(in + 0x68 + 0x2e);
    uint32_t al = get16(in + 0x268);
    if (al > ONLINE_LDN_ADVERTISE_MAX) al = ONLINE_LDN_ADVERTISE_MAX;
    if (n < 0x26a + al) al = n > 0x26a ? (uint32_t)(n - 0x26a) : 0;
    out->advertise_len = (uint16_t)al;
    memcpy(out->advertise, in + 0x26a, al);
    return 0;
}
void ldn_create_decode(const uint8_t *in, size_t n, OnlineLdnNetwork *out) {
    uint8_t b[0x98] = {0};
    memset(out, 0, sizeof *out);
    memcpy(b, in, n < sizeof b ? n : sizeof b);
    out->security_mode = (uint8_t)get16(b);
    memcpy(out->host_name, b + 0x44, 0x20);
    out->host_name[32] = 0;
    out->local_communication_id = get64(b + 0x78);
    out->scene_id = get16(b + 0x78 + 0x0a);
    out->channel = (uint8_t)get16(b + 0x78 + 0x10);
    out->node_count_max = b[0x78 + 0x12];
    out->local_communication_version = get16(b + 0x78 + 0x14);
}
void ldn_config_encode(const OnlineLdnNetwork *n, uint8_t out[0x20]) {
    memset(out, 0, 0x20);
    put64(out, n->local_communication_id);
    put16(out + 0x0a, n->scene_id);
    put16(out + 0x10, n->channel);
    out[0x12] = n->node_count_max;
    put16(out + 0x14, n->local_communication_version);
}
uint32_t ldn_connect_done(LdnState *s, uint32_t r) {
    if (*s != LDN_STATION_OPENED) return R_INVALID_STATE;
    if (r) return r;
    *s = LDN_STATION_CONNECTED;
    return 0;
}
int ldn_state_has_network(LdnState s) { return s == LDN_AP_CREATED || s == LDN_STATION_CONNECTED; }

/* ---- configuration rows (ldn_core.h) ---- */
unsigned ldn_state_mask(const int *states) {
    unsigned m = 0;
    for (; *states >= 0; states++) m |= 1u << (unsigned)*states;
    return m;
}
uint32_t ldn_check_protocol(LdnState state, uint32_t protocol) {
    if (protocol == 0 || protocol >= 32 || !(LDN_PROTOCOL_NX_PERMISSION >> protocol & 1u))
        return R_INVALID_ARG; /* the console returns a separate (undocumented) result for an unpermitted value */
    if (state != LDN_INITIALIZED) return R_INVALID_STATE;
    return 0;
}
uint32_t ldn_check_action_frame_settings(const uint8_t *in, size_t n) {
    if (!in || n < LDN_ACTION_FRAME_SETTINGS_SIZE) return R_INVALID_ARG;
    uint16_t mode, size;
    memcpy(&mode, in + 0x3c, 2);
    memcpy(&size, in + 0x3e, 2);
    return mode >= 1 && mode <= 2 && size >= 0x10 && size <= 0x40 ? 0 : R_INVALID_ARG;
}
int ldn_decode_channel(const uint8_t *in, size_t n, uint16_t *band_mhz, uint16_t *channel) {
    uint16_t a = 0, b = 0;
    if (n >= 2) memcpy(&a, in, 2);
    if (n >= 4) memcpy(&b, in + 2, 2);
    if (a == 24 || a == 50) { /* before 20.0.0: s16 Band, s16 ChannelNumber */
        *band_mhz = a == 24 ? 2400 : 5000;
        *channel = b;
    } else { /* 20.0.0+: u16 band index << 10 | ChannelNumber */
        unsigned idx = a >> 10;
        *band_mhz = idx == 2 ? 2400 : idx == 5 ? 5000 : 0;
        *channel = a & 0x3ffu;
    }
    return *band_mhz && *channel ? 0 : -1;
}
