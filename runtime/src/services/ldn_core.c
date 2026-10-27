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
uint32_t ldn_set_advertise(LdnState state, LdnConfig *cfg, const void *data, uint32_t n) {
    if (state != LDN_AP_CREATED) return R_INVALID_STATE;
    if (n > LDN_ADVERTISE_MAX) return R_INVALID_SIZE;
    if (n) memcpy(cfg->advertise, data, n);
    cfg->advertise_len = (uint16_t)n;
    return 0;
}
uint32_t ldn_set_accept_policy(LdnState state, LdnConfig *cfg, uint32_t policy) {
    if (state != LDN_AP_CREATED) return R_INVALID_STATE;
    if (policy > 3) return R_INVALID_ARG;
    cfg->accept_policy = policy;
    return 0;
}
uint32_t ldn_filter_add(LdnState state, LdnConfig *cfg, const uint8_t mac[6]) {
    if (state != LDN_AP_CREATED) return R_INVALID_STATE;
    for (unsigned i = 0; i < cfg->nfilter; i++)
        if (!memcmp(cfg->filter[i], mac, 6)) return 0;
    if (cfg->nfilter >= LDN_FILTER_MAX) return R_FILTER_FULL;
    memcpy(cfg->filter[cfg->nfilter++], mac, 6);
    return 0;
}
uint32_t ldn_filter_clear(LdnState state, LdnConfig *cfg) {
    if (state != LDN_AP_CREATED) return R_INVALID_STATE;
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
void ldn_net_encode(const OnlineLdnNetwork *n, uint8_t out[LDN_NETINFO_SIZE]) {
    memset(out, 0, LDN_NETINFO_SIZE);
    put64(out, n->local_communication_id);
    put16(out + 0x0a, n->scene_id);
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
    }
    uint32_t al = n->advertise_len > ONLINE_LDN_ADVERTISE_MAX ? ONLINE_LDN_ADVERTISE_MAX : n->advertise_len;
    put16(out + 0x268, (uint16_t)al);
    memcpy(out + 0x26a, n->advertise, al);
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
