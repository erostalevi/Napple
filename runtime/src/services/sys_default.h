/* Documented safe defaults for application-reachable commands that had no row (ipc-depth-sys, 2026-11-01-r2). Every row that uses these is marked
 * IPC_STUB in its table. The command lists come from the switchbrew service tables (runtime/tests/ipc_reference/ one file per interface, layout facts only):
 *   sysd_ok       success, no output (Set* / Register* / Notify* style commands and permission confirmations of an offline console with no restrictions)
 *   sysd_zN       success with N zero bytes of output (a getter whose documented answer for this console is "none / false / 0")
 *   sysd_event    success with a readable event that never fires (the thing the guest waits on never happens offline)
 *   sysd_err_*    the result the real service gives when what the command needs is not there: no network (nifm, bcat), no network service account (friends),
 *                 not connected (ssl), no network (ldn); sysd_err_generic is the sf "unknown command" result, what NAPPLE_IPC_LENIENT answers, for object /
 *                 buffer / handle returning commands this console cannot back (the SDK treats it as a failed call, the runtime keeps running). */
#pragma once
#include "svc_common.h"
#include "hz_result.h"

static inline void sysd_ok(IpcObject *s, IpcCall *c) { svc_noop(s, c); }
static inline void sysd_event(IpcObject *s, IpcCall *c) { svc_ret_event(s, c); }
static inline void sysd_err_generic(IpcObject *s, IpcCall *c) { (void)s; c->result = IPC_R_UNKNOWN_COMMAND; }
static inline void sysd_err_net(IpcObject *s, IpcCall *c) { (void)s; c->result = HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED; }
static inline void sysd_err_friend(IpcObject *s, IpcCall *c) { (void)s; c->result = HZ_R_FRIENDS_NETWORK_SERVICE_ACCOUNT_UNAVAILABLE; }
static inline void sysd_err_ssl(IpcObject *s, IpcCall *c) { (void)s; c->result = HZ_R_SSL_NOT_CONNECTED; }
static inline void sysd_err_ldn(IpcObject *s, IpcCall *c) { (void)s; c->result = HZ_R_LDN_NETWORK_NOT_AVAILABLE; }
static inline void sysd_z1(IpcObject *s, IpcCall *c) { (void)s; ipc_out_zero(c, 1); }
static inline void sysd_z4(IpcObject *s, IpcCall *c) { (void)s; ipc_out_zero(c, 4); }
static inline void sysd_z8(IpcObject *s, IpcCall *c) { (void)s; ipc_out_zero(c, 8); }
static inline void sysd_z12(IpcObject *s, IpcCall *c) { (void)s; ipc_out_zero(c, 12); }
static inline void sysd_z16(IpcObject *s, IpcCall *c) { (void)s; ipc_out_zero(c, 16); }
static inline void sysd_z24(IpcObject *s, IpcCall *c) { (void)s; ipc_out_zero(c, 24); }
static inline void sysd_z32(IpcObject *s, IpcCall *c) { (void)s; ipc_out_zero(c, 32); }
static inline void sysd_z60(IpcObject *s, IpcCall *c) { (void)s; ipc_out_zero(c, 60); }
