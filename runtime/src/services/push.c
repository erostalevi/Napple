/* Offline stand-ins for the push / notification clients some titles list in their NPDM and connect to during play (nx-kernel S5):
 *   npns:u (push notifications), notif:a (notification settings), mnpp:app, ngct:u, nim:eca (all Animal Crossing: New Horizons 3.0.x).
 * An offline console never receives a push and has nothing to configure, so every request succeeds with no output (nxnative answers
 * the game's own olsc:u / npns clients the same way: docs/reference/services.md "Game-linked HIPC clients", behaviour only). Without
 * these the manager refuses the name and the SDK's connect aborts. The command tables below are generous ranges of ids, not a list of
 * verified commands (unverified against hardware): an id outside the range still fails loudly through the generic unknown-command policy.
 * npns commands 5 and 7 (receive / state change events) hand out an event that never fires. */
#include "svc_common.h"

#define PUSH_RANGE 32
static IpcCmd g_tables[5][PUSH_RANGE];
static void ok(IpcObject *s, IpcCall *c) { (void)s; (void)c; }
static void fill(IpcCmd *t, unsigned event_a, unsigned event_b) {
    for (unsigned i = 0; i < PUSH_RANGE; i++) t[i] = (IpcCmd){i, "OfflineNoop", (i == event_a || i == event_b) ? svc_ret_event : ok, IPC_STUB};
}
static IpcObject *open_npns(void) { return IPC_OBJECT("npns:u", g_tables[0], NULL, NULL); }
static IpcObject *open_notif(void) { return IPC_OBJECT("notif:a", g_tables[1], NULL, NULL); }
static IpcObject *open_mnpp(void) { return IPC_OBJECT("mnpp:app", g_tables[2], NULL, NULL); }
static IpcObject *open_ngct(void) { return IPC_OBJECT("ngct:u", g_tables[3], NULL, NULL); }
static IpcObject *open_nim_eca(void) { return IPC_OBJECT("nim:eca", g_tables[4], NULL, NULL); }
void service_register_push(void) {
    fill(g_tables[0], 5, 7); for (int i = 1; i < 5; i++) fill(g_tables[i], PUSH_RANGE, PUSH_RANGE);
    ipc_register_service("npns:u", open_npns); ipc_register_service("notif:a", open_notif); ipc_register_service("mnpp:app", open_mnpp);
    ipc_register_service("ngct:u", open_ngct); ipc_register_service("nim:eca", open_nim_eca);
}
