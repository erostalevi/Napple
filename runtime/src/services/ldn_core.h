/* ldn core: local-wireless state machine (portable). Models the lifecycle the SDK drives (initialize, open access point / station,
 * create network, connect, ...) and which transitions are legal, so a title that polls GetState sees a coherent sequence. There is no
 * radio: scans find nothing and a station can never connect. */
#pragma once
#include <stdint.h>

typedef enum { LDN_NONE = 0, LDN_INITIALIZED = 1, LDN_AP_OPENED = 2, LDN_AP_CREATED = 3, LDN_STATION_OPENED = 4, LDN_STATION_CONNECTED = 5, LDN_ERROR = 6 } LdnState;
typedef enum { LDN_OP_INITIALIZE, LDN_OP_FINALIZE, LDN_OP_OPEN_AP, LDN_OP_CLOSE_AP, LDN_OP_CREATE_NETWORK, LDN_OP_DESTROY_NETWORK,
               LDN_OP_OPEN_STATION, LDN_OP_CLOSE_STATION, LDN_OP_CONNECT, LDN_OP_DISCONNECT, LDN_OP_SCAN } LdnOp;

/* Applies `op`. Returns 0 and updates *state, or a Horizon result describing why the call is not allowed and leaves *state alone. */
uint32_t ldn_apply(LdnState *state, LdnOp op);
