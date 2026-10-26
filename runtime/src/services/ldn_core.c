#include "ldn_core.h"
#include "hz_result.h"

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
