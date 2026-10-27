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
