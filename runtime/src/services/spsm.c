/* spsm: system power state manager (IPowerStateInterface, switchbrew OMM services). The host never sleeps the emulated console: the state is
 * always Awake, and requests to sleep return immediately (the system "slept" and woke at once). Ids follow switchbrew since 2026-11-01 (2 and 3
 * were named GetNotificationMessage / SetAwake before; the answers did not change: 2 = no wake reason, 3 = a shutdown the host ignores). */
#include "svc_common.h"

enum { SPSM_STATE_AWAKE = 0 };
static unsigned sleep_requests;
static void spsm_get_state(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = SPSM_STATE_AWAKE;
    IPC_OUT(c, v);
}
static void spsm_sleep(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    sleep_requests++;
}
/* The host never sleeps: SleepSystemAndWaitAwake returns at once (the system slept and woke instantly), no power message is ever pending
 * (GetNotificationMessage = 0) and SetAwake has nothing to wake. These are the real answers for a console that cannot be put to sleep. */
static void spsm_message_event(IpcObject *s, IpcCall *c) { /* 4: power notifications; the host never sends one, so the event never fires */
    (void)s;
    uint32_t r = 0;
    event_create(&r);
    ipc_out_copy_handle(c, r);
}
static const IpcCmd spsm_cmds[] = {
    {0, "GetCurrentState", spsm_get_state, IPC_IMPL},
    {1, "EnterSleep", spsm_sleep, IPC_IMPL},
    {2, "GetLastWakeReason", svc_ret_u32_0, IPC_IMPL},
    {3, "Shutdown", svc_noop, IPC_IMPL}, /* the guest cannot power the host off; the request is accepted and the process keeps running */
    {4, "GetNotificationMessageEventHandle", spsm_message_event, IPC_IMPL},
    {5, "ReceiveNotificationMessage", svc_ret_u32_0, IPC_IMPL}, /* no message pending */
};
static IpcObject *spsm_open(void) { return IPC_OBJECT("spsm", spsm_cmds, NULL, NULL); }
void service_register_spsm(void) { ipc_register_service("spsm", spsm_open); }
