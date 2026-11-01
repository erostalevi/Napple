/* Offline stand-ins for the push / notification clients some titles list in their NPDM and connect to during play (nx-kernel S5):
 *   npns:u (push notifications), notif:a (notification settings), mnpp:app, ngct:u, nim:eca (all Animal Crossing: New Horizons 3.0.x).
 * An offline console never receives a push and has nothing to configure, so every request succeeds with no output (nxnative answers
 * the game's own olsc:u / npns clients the same way: docs/reference/services.md "Game-linked HIPC clients", behaviour only). Without
 * these the manager refuses the name and the SDK's connect aborts. The command tables below are generous ranges of ids, not a list of
 * verified commands (unverified against hardware): an id outside the range still fails loudly through the generic unknown-command policy.
 * The rows are complete for an offline console (class c): nothing is ever pushed or configured. npns commands 5 and 7 (receive / state change events) hand out an event that never fires. */
#include "svc_common.h"
#include "sys_default.h"

#define PUSH_RANGE 32
#define PUSH_EXTRA 12 /* ids above the generous range that the switchbrew tables list (npns:u 101..154, notif:a 500..1000) */
static IpcCmd g_tables[5][PUSH_RANGE + PUSH_EXTRA];
static size_t g_count[5];
static void ok(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static void fill(IpcCmd *t, unsigned event_a, unsigned event_b) {
    for (unsigned i = 0; i < PUSH_RANGE; i++) t[i] = (IpcCmd){i, "OfflineNoop", (i == event_a || i == event_b) ? svc_ret_event : ok, IPC_IMPL};
}
static IpcObject *open_table(const char *name, int t) { return ipc_object_new(name, g_tables[t], g_count[t], NULL, NULL); }
static IpcObject *open_npns(void) { return open_table("npns:u", 0); }
static IpcObject *open_notif(void) { return open_table("notif:a", 1); }
static IpcObject *open_mnpp(void) { return open_table("mnpp:app", 2); }
static IpcObject *open_ngct(void) { return open_table("ngct:u", 3); }
static IpcObject *open_nim_eca(void) { return open_table("nim:eca", 4); }
/* The ids past the range, from the switchbrew tables: offline answers (nothing is ever pushed or configured). */
static void extra(int t, unsigned id, const char *name, void (*fn)(IpcObject *, IpcCall *)) { g_tables[t][g_count[t]++] = (IpcCmd){id, name, fn, IPC_STUB}; }
void service_register_push(void) {
    fill(g_tables[0], 5, 7);
    for (int i = 1; i < 5; i++) fill(g_tables[i], PUSH_RANGE, PUSH_RANGE);
    for (int i = 0; i < 5; i++) g_count[i] = PUSH_RANGE;
    extra(0, 101, "Suspend", sysd_ok);
    extra(0, 102, "Resume", sysd_ok);
    extra(0, 103, "GetState", sysd_z4);
    extra(0, 104, "GetStatistics", sysd_err_generic);
    extra(0, 111, "GetJid", sysd_err_generic);
    extra(0, 120, "CreateNotificationReceiver", sysd_err_generic);
    extra(0, 151, "GetStateWithHandover", sysd_z4);
    extra(0, 152, "GetStateChangeEventWithHandover", sysd_event);
    extra(0, 153, "GetDropEventWithHandover", sysd_event);
    extra(0, 154, "CreateTokenAsync", sysd_err_generic);
    extra(1, 500, "RegisterAlarmSetting", sysd_ok);
    extra(1, 510, "UpdateAlarmSetting", sysd_ok);
    extra(1, 520, "ListAlarmSettings", sysd_z4);
    extra(1, 530, "LoadApplicationParameter", sysd_err_generic);
    extra(1, 540, "DeleteAlarmSetting", sysd_ok);
    extra(1, 1000, "Initialize", sysd_ok);
    ipc_register_service("npns:u", open_npns);
    ipc_register_service("notif:a", open_notif);
    ipc_register_service("mnpp:app", open_mnpp);
    ipc_register_service("ngct:u", open_ngct);
    ipc_register_service("nim:eca", open_nim_eca);
}
