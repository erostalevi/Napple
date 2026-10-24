/* spsm: system power state manager. The host never sleeps the emulated console: the state is always Awake, and requests to sleep or
 * standby return immediately (the system "slept" and woke at once). Command ids are from the author's memory of the public
 * documentation (unverified); other commands fail loudly. */
#include "svc_common.h"

enum { SPSM_STATE_AWAKE = 0 };
static unsigned sleep_requests;
static void spsm_get_state(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = SPSM_STATE_AWAKE; IPC_OUT(c, v); }
static void spsm_sleep(IpcObject *s, IpcCall *c) { (void)s; (void)c; sleep_requests++; }
static const IpcCmd spsm_cmds[] = {
    {0, "GetState", spsm_get_state, IPC_IMPL}, {1, "SleepSystemAndWaitAwake", spsm_sleep, IPC_STUB},
    {2, "GetNotificationMessage", svc_ret_u32_0, IPC_STUB}, {3, "SetAwake", svc_noop, IPC_STUB},
};
static IpcObject *spsm_open(void) { return IPC_OBJECT("spsm", spsm_cmds, NULL, NULL); }
void service_register_spsm(void) { ipc_register_service("spsm", spsm_open); }
