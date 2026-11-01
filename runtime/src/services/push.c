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
/* The rows the switchbrew tables name (ids and names only). Ids inside the range keep the generous-range answer (success with no output, or
 * the never-firing event for npns 5 / 7) and only gain their name; ids past the range are offline answers (nothing is ever pushed or
 * configured). service_register_push lays them over the generous range. The fixtures in tests/ipc_reference bind these tables with
 * `# table` lines, since the runtime tables are only filled at registration. */
static const IpcCmd npns_rows[] = {
    {1, "ListenAll", ok, IPC_IMPL}, {2, "ListenTo", ok, IPC_IMPL}, {3, "Receive", ok, IPC_IMPL}, {4, "ReceiveRaw", ok, IPC_IMPL},
    {5, "GetReceiveEvent", svc_ret_event, IPC_IMPL}, {7, "GetStateChangeEvent", svc_ret_event, IPC_IMPL}, {8, "ListenToByName", ok, IPC_IMPL},
    {21, "CreateToken", ok, IPC_IMPL}, {23, "DestroyToken", ok, IPC_IMPL}, {25, "QueryIsTokenValid", ok, IPC_IMPL},
    {26, "ListenToMyApplicationId", ok, IPC_IMPL},
    {101, "Suspend", sysd_ok, IPC_STUB}, {102, "Resume", sysd_ok, IPC_STUB}, {103, "GetState", sysd_z4, IPC_STUB},
    {104, "GetStatistics", sysd_err_generic, IPC_STUB}, {111, "GetJid", sysd_err_generic, IPC_STUB},
    {120, "CreateNotificationReceiver", sysd_err_generic, IPC_STUB}, {151, "GetStateWithHandover", sysd_z4, IPC_STUB},
    {152, "GetStateChangeEventWithHandover", sysd_event, IPC_STUB}, {153, "GetDropEventWithHandover", sysd_event, IPC_STUB},
    {154, "CreateTokenAsync", sysd_err_generic, IPC_STUB},
};
static const IpcCmd notif_rows[] = {
    {500, "RegisterAlarmSetting", sysd_ok, IPC_STUB}, {510, "UpdateAlarmSetting", sysd_ok, IPC_STUB}, {520, "ListAlarmSettings", sysd_z4, IPC_STUB},
    {530, "LoadApplicationParameter", sysd_err_generic, IPC_STUB}, {540, "DeleteAlarmSetting", sysd_ok, IPC_STUB}, {1000, "Initialize", sysd_ok, IPC_STUB},
};
static const IpcCmd mnpp_rows[] = {{0, "Initialize", ok, IPC_IMPL}, {1, "SubmitEvent", ok, IPC_IMPL}};
static const IpcCmd ngct_rows[] = {{0, "Match", ok, IPC_IMPL}, {1, "Filter", ok, IPC_IMPL}};
static const IpcCmd nim_eca_rows[] = {{0, "CreateServerInterface", ok, IPC_IMPL}, {4, "IsLargeResourceAvailable", ok, IPC_IMPL}, {5, "CreateServerInterface2", ok, IPC_IMPL}};
static void lay(int t, const IpcCmd *rows, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (rows[i].cmd < PUSH_RANGE) g_tables[t][rows[i].cmd] = rows[i];
        else g_tables[t][g_count[t]++] = rows[i];
    }
}
#define LAY(t, rows) lay(t, rows, sizeof rows / sizeof *rows)
void service_register_push(void) {
    fill(g_tables[0], 5, 7);
    for (int i = 1; i < 5; i++) fill(g_tables[i], PUSH_RANGE, PUSH_RANGE);
    for (int i = 0; i < 5; i++) g_count[i] = PUSH_RANGE;
    LAY(0, npns_rows);
    LAY(1, notif_rows);
    LAY(2, mnpp_rows);
    LAY(3, ngct_rows);
    LAY(4, nim_eca_rows);
    ipc_register_service("npns:u", open_npns);
    ipc_register_service("notif:a", open_notif);
    ipc_register_service("mnpp:app", open_mnpp);
    ipc_register_service("ngct:u", open_ngct);
    ipc_register_service("nim:eca", open_nim_eca);
}
