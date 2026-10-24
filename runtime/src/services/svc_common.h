/* Small IPC handlers shared by the services added in services-2026-10-24 (static inline: no state, no macOS dependency). */
#pragma once
#include "../ipc.h"
#include <string.h>

static inline void svc_noop(IpcObject *s, IpcCall *c) { (void)s; (void)c; }
static inline void svc_ret_u8_0(IpcObject *s, IpcCall *c) { (void)s; uint8_t v = 0; IPC_OUT(c, v); }
static inline void svc_ret_u8_1(IpcObject *s, IpcCall *c) { (void)s; uint8_t v = 1; IPC_OUT(c, v); }
static inline void svc_ret_u32_0(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = 0; IPC_OUT(c, v); }
static inline void svc_ret_i32_0(IpcObject *s, IpcCall *c) { (void)s; int32_t v = 0; IPC_OUT(c, v); }
static inline void svc_ret_u64_0(IpcObject *s, IpcCall *c) { (void)s; uint64_t v = 0; IPC_OUT(c, v); }
/* A readable event handle that is never signalled (the thing the guest waits on never happens offline). */
static inline void svc_ret_event(IpcObject *s, IpcCall *c) { (void)s; uint32_t r = 0; event_create(&r); ipc_out_copy_handle(c, r); }
/* A readable event handle that is already signalled. */
static inline void svc_ret_event_signaled(IpcObject *s, IpcCall *c) { (void)s; uint32_t r = 0, w = event_create(&r); event_signal_handle(w); ipc_out_copy_handle(c, r); }
