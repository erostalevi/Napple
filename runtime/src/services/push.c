/* Offline stand-ins for the push / notification clients some titles list in their NPDM and connect to during play (nx-kernel S5):
 *   npns:u (push notifications), notif:a (notification settings), mnpp:app, ngct:u, nim:eca (all Animal Crossing: New Horizons 3.0.x).
 * An offline console never receives a push and has nothing to configure, so every request succeeds with no output (nxnative answers
 * the game's own olsc:u / npns clients the same way: docs/reference/services.md "Game-linked HIPC clients", behaviour only). Without
 * these the manager refuses the name and the SDK's connect aborts. The command tables below are generous ranges of ids, not a list of
 * verified commands (unverified against hardware): an id outside the range still fails loudly through the generic unknown-command policy.
 * The rows are complete for an offline console (class c): nothing is ever pushed or configured. npns command 5 (receive event) hands out an event that never fires;
 * 7 (state change event) fires when Suspend / Resume change the daemon's state (see "npns:u daemon state" below). */
#include "svc_common.h"
#include "sys_default.h"
#include "notif_core.h"
#include "online_provider.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PUSH_RANGE 32
#define PUSH_EXTRA 12 /* ids above the generous range that the switchbrew tables list (npns:u 101..154, notif:a 500..1000) */
static IpcCmd g_tables[5][PUSH_RANGE + PUSH_EXTRA];
static size_t g_count[5];
static void ok(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
/* ---- npns:u local state ----
 * No push server is reachable, so the state is host-side only: a connection state (0 = not connected, the default), a queue of received
 * notifications (empty by default), receive / state-change / drop counters and the console's own JID string (empty until set). Events handed out
 * by GetReceiveEvent (5), GetStateChangeEvent (7) and GetDropEventWithHandover (153) are remembered and signalled when the matching thing
 * happens (npns_local_*), so an offline console's events are quiet only because nothing happens, not because they are dead. Record layouts
 * are [unverified]: GetState / GetStateWithHandover answer a u32, GetStatistics 0x10 bytes {u64 received, u64 dropped}, GetJid fills the
 * first output buffer with the NUL-terminated JID. */
#define NPNS_QUEUE 16
#define NPNS_PAYLOAD 0x100
#define NPNS_EVENTS 8
#define NPNS_JID_SIZE 0x100
typedef struct {
    uint32_t size;
    uint8_t data[NPNS_PAYLOAD];
} NpnsNotification;
typedef struct {
    uint32_t w[NPNS_EVENTS], n;
} NpnsEvents;
static struct {
    uint32_t state, nqueue;
    uint64_t received, dropped;
    NpnsNotification queue[NPNS_QUEUE];
    char jid[NPNS_JID_SIZE];
    NpnsEvents recv, drop;
} g_npns;
static void npns_event_out(IpcCall *c, NpnsEvents *e) {
    uint32_t r = 0, w = event_create(&r);
    if (e->n < NPNS_EVENTS) e->w[e->n++] = w;
    ipc_out_copy_handle(c, r);
}
static void npns_event_fire(NpnsEvents *e) {
    for (uint32_t i = 0; i < e->n; i++) event_signal_handle(e->w[i]);
}
static void npns_recv_event(IpcObject *s, IpcCall *c) { (void)s; npns_event_out(c, &g_npns.recv); }
static void npns_drop_event(IpcObject *s, IpcCall *c) { (void)s; npns_event_out(c, &g_npns.drop); }
static void npns_get_state(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = g_npns.state;
    IPC_OUT(c, v);
}
static void npns_get_stats(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v[2] = {g_npns.received, g_npns.dropped};
    IPC_OUT(c, v);
}
static void npns_get_jid(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) ipc_write_buf(c, b, g_npns.jid, strlen(g_npns.jid) + 1 < b->size ? strlen(g_npns.jid) + 1 : b->size);
}
/* Host hooks (a private build's push client, and the tests): a notification arrives (the oldest is dropped when the queue is full), the connection
 * state changes, the JID is assigned. */
__attribute__((unused)) static void npns_local_enqueue(const void *data, uint32_t size) {
    if (size > NPNS_PAYLOAD) size = NPNS_PAYLOAD;
    if (g_npns.nqueue == NPNS_QUEUE) {
        memmove(g_npns.queue, g_npns.queue + 1, (NPNS_QUEUE - 1) * sizeof g_npns.queue[0]);
        g_npns.nqueue--;
        g_npns.dropped++;
        npns_event_fire(&g_npns.drop);
    }
    NpnsNotification *n = &g_npns.queue[g_npns.nqueue++];
    n->size = size;
    memcpy(n->data, data, size);
    g_npns.received++;
    npns_event_fire(&g_npns.recv);
}
__attribute__((unused)) static void npns_local_set_jid(const char *jid) { snprintf(g_npns.jid, sizeof g_npns.jid, "%s", jid ? jid : ""); }
static void fill(IpcCmd *t, unsigned event_a, unsigned event_b) {
    for (unsigned i = 0; i < PUSH_RANGE; i++) t[i] = (IpcCmd){i, "OfflineNoop", (i == event_a || i == event_b) ? svc_ret_event : ok, IPC_IMPL};
}
static IpcObject *open_table(const char *name, int t) { return ipc_object_new(name, g_tables[t], g_count[t], NULL, NULL); }
static IpcObject *open_npns(void) { return open_table("npns:u", 0); }
static IpcObject *open_notif(void) { return open_table("notif:a", 1); }
static IpcObject *open_mnpp(void) { return open_table("mnpp:app", 2); }
static IpcObject *open_ngct(void) { return open_table("ngct:u", 3); }
static IpcObject *open_nim_eca(void) { return open_table("nim:eca", 4); }
/* ---- npns:u daemon state (ipc-depth-95 r10) ----
 * Layout facts from switchbrew "NPNS services" (command table, INotificationReceiver, ICreateTokenAsyncContext). npns is one system daemon, so its state is
 * process-wide here: the Suspend / Resume flag and the state-change events every GetStateChangeEvent* handed out (they fire when the daemon's state changes).
 * The daemon never reaches the push server: the OnlineProvider has no npns hook (an online path would need one that opens the XMPP connection, creates the
 * device token and delivers notifications), so nothing is ever received and no token is ever created. The page documents no State numbering, Statistics
 * layout or Jid "none" answer: 103 GetState, 151 GetStateWithHandover, 104 GetStatistics, 111 GetJid and 153 are served from the host-side "npns:u local state"
 * above (ipc-bcat-npns-local r11; the layouts are [unverified] guesses, a persistent connection never exists unless a host hook sets one). */
#define NPNS_MODULE 154u /* 2154-xxxx; switchbrew's npns:s stub results 0x7fc9a / 0x36dc9a are module 154 */
#define R_NPNS_SERVER_UNAVAILABLE HZ_RESULT(NPNS_MODULE, 101) /* [unverified] napple's choice: a network exists, the push server is not reachable from napple */
#define NPNS_MAX_EVENTS 16
static pthread_mutex_t g_npns_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_npns_suspended;
static uint32_t g_npns_state_ev[NPNS_MAX_EVENTS];
static unsigned g_npns_nstate_ev;
static void npns_state_event(IpcObject *s, IpcCall *c) { /* 7 GetStateChangeEvent, 152 GetStateChangeEventWithHandover */
    (void)s;
    uint32_t r = 0, w = event_create(&r);
    pthread_mutex_lock(&g_npns_mu);
    if (g_npns_nstate_ev < NPNS_MAX_EVENTS) g_npns_state_ev[g_npns_nstate_ev++] = w;
    pthread_mutex_unlock(&g_npns_mu);
    ipc_out_copy_handle(c, r);
}
static void npns_set_suspended(int on) {
    pthread_mutex_lock(&g_npns_mu);
    if (g_npns_suspended != on) {
        g_npns_suspended = on;
        for (unsigned i = 0; i < g_npns_nstate_ev; i++) event_signal_handle(g_npns_state_ev[i]);
    }
    pthread_mutex_unlock(&g_npns_mu);
}
/* Host hooks that touch the daemon state (defined here, after the state-change event list of the daemon state below): a state change from the host fires the
 * same state-change events a Suspend / Resume does (GetStateChangeEvent 7 and ...WithHandover 152 share them), and the reset clears both state blocks. */
__attribute__((unused)) static void npns_local_set_state(uint32_t state) {
    if (state == g_npns.state) return;
    g_npns.state = state;
    pthread_mutex_lock(&g_npns_mu);
    for (unsigned i = 0; i < g_npns_nstate_ev; i++) event_signal_handle(g_npns_state_ev[i]);
    pthread_mutex_unlock(&g_npns_mu);
}
__attribute__((unused)) static void npns_local_reset(void) {
    memset(&g_npns, 0, sizeof g_npns);
    pthread_mutex_lock(&g_npns_mu);
    g_npns_nstate_ev = 0;
    g_npns_suspended = 0;
    pthread_mutex_unlock(&g_npns_mu);
}
/* 101 Suspend / 102 Resume (no input, no output): the daemon is put to rest / woken; a change of state fires the state-change events, a repeat is a no-op. */
static void npns_suspend(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    npns_set_suspended(1);
}
static void npns_resume(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    npns_set_suspended(0);
}

/* 120 CreateNotificationReceiver [7.0.0+] -> INotificationReceiver: 1 ListenTo(u64 program id), 2 ListenToMyApplicationId(u64, PID), 3 Receive(u16, type-0x6
 * buffer), 4 GetReceiveEvent, 5 [18.0.0+] ListenToByName (no layout given: not in the table). The listened ids are kept per receiver; nothing is ever pushed,
 * so the receive event never fires and Receive writes nothing (the answer INpnsUser 3 Receive gives here; the console's "nothing queued" answer is not
 * documented [unverified]). */
#define NPNS_MAX_LISTEN 16
typedef struct {
    uint64_t ids[NPNS_MAX_LISTEN];
    unsigned n;
    int my_application; /* ListenToMyApplicationId done */
    uint64_t my_application_pid;
} NpnsReceiver;
static void rcv_listen(NpnsReceiver *r, uint64_t id) {
    for (unsigned i = 0; i < r->n; i++)
        if (r->ids[i] == id) return;
    if (r->n < NPNS_MAX_LISTEN) r->ids[r->n++] = id;
}
static void rcv_listen_to(IpcObject *s, IpcCall *c) { rcv_listen(s->state, c->in_size >= 8 ? IPC_IN(c, uint64_t, 0) : 0); }
static void rcv_listen_mine(IpcObject *s, IpcCall *c) {
    NpnsReceiver *r = s->state;
    r->my_application = 1;
    r->my_application_pid = c->has_pid ? c->pid : 0;
}
static void rcv_receive(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static const IpcCmd receiver_cmds[] = {
    {1, "ListenTo", rcv_listen_to, IPC_IMPL},
    {2, "ListenToMyApplicationId", rcv_listen_mine, IPC_IMPL},
    {3, "Receive", rcv_receive, IPC_IMPL},
    {4, "GetReceiveEvent", svc_ret_event, IPC_IMPL},
};
static void free_state(IpcObject *o) { free(o->state); }
static void npns_create_receiver(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("INotificationReceiver", receiver_cmds, calloc(1, sizeof(NpnsReceiver)), free_state));
}

/* 154 CreateTokenAsync [12.0.0+] -> ICreateTokenAsyncContext: 1 GetFinishEvent, 2 Cancel, 3 GetResult, 10 GetToken. The request finishes at once with the
 * result of trying: no network (the NullProvider, or a provider whose interface is down) -> the nifm "no network" result; a network but no push server
 * hook -> R_NPNS_SERVER_UNAVAILABLE. GetResult and GetToken answer that result, so no token layout is needed. The request's arguments (a user and an
 * application, as for CreateToken on 5.0.0+) are not read. */
typedef struct {
    uint32_t result;
} NpnsTokenAsync;
static uint32_t npns_token_result(void) {
    const OnlineProvider *p = online_provider();
    int (*info)(void *, OnlineNetInfo *) = online_has(p, ONLINE_CAP_NETWORK) ? ONLINE_FN(p, net_info) : NULL;
    OnlineNetInfo ni;
    memset(&ni, 0, sizeof ni);
    if (!info || info(p->ctx, &ni) != 0 || !ni.connected) return HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
    return R_NPNS_SERVER_UNAVAILABLE;
}
static void tok_finish_event(IpcObject *s, IpcCall *c) { svc_ret_event_signaled(s, c); } /* the request already finished */
static void tok_cancel(IpcObject *s, IpcCall *c) {                                     /* nothing in flight to cancel */
    (void)s;
    (void)c;
}
static void tok_result(IpcObject *s, IpcCall *c) { c->result = ((NpnsTokenAsync *)s->state)->result; }
static const IpcCmd token_async_cmds[] = {
    {1, "GetFinishEvent", tok_finish_event, IPC_IMPL},
    {2, "Cancel", tok_cancel, IPC_IMPL},
    {3, "GetResult", tok_result, IPC_IMPL},
    {10, "GetToken", tok_result, IPC_IMPL},
};
static void npns_create_token_async(IpcObject *s, IpcCall *c) {
    (void)s;
    NpnsTokenAsync *t = calloc(1, sizeof *t);
    if (t) t->result = npns_token_result();
    ipc_out_object(c, IPC_OBJECT("ICreateTokenAsyncContext", token_async_cmds, t, free_state));
}

/* The rows the switchbrew tables name (ids and names only). Ids inside the range keep the generous-range answer (success with no output, or
 * the never-firing event for npns 5 / 7) and only gain their name; ids past the range are offline answers (nothing is ever pushed or
 * configured). service_register_push lays them over the generous range. The fixtures in tests/ipc_reference bind these tables with
 * `# table` lines, since the runtime tables are only filled at registration. */
static const IpcCmd npns_rows[] = {
    {1, "ListenAll", ok, IPC_IMPL}, {2, "ListenTo", ok, IPC_IMPL}, {3, "Receive", ok, IPC_IMPL}, {4, "ReceiveRaw", ok, IPC_IMPL},
    {5, "GetReceiveEvent", npns_recv_event, IPC_IMPL}, {7, "GetStateChangeEvent", npns_state_event, IPC_IMPL}, {8, "ListenToByName", ok, IPC_IMPL},
    {21, "CreateToken", ok, IPC_IMPL}, {23, "DestroyToken", ok, IPC_IMPL}, {25, "QueryIsTokenValid", ok, IPC_IMPL},
    {26, "ListenToMyApplicationId", ok, IPC_IMPL},
    {101, "Suspend", npns_suspend, IPC_IMPL}, {102, "Resume", npns_resume, IPC_IMPL}, {103, "GetState", npns_get_state, IPC_IMPL},
    {104, "GetStatistics", npns_get_stats, IPC_IMPL}, {111, "GetJid", npns_get_jid, IPC_IMPL},
    {120, "CreateNotificationReceiver", npns_create_receiver, IPC_IMPL}, {151, "GetStateWithHandover", npns_get_state, IPC_IMPL},
    {152, "GetStateChangeEventWithHandover", npns_state_event, IPC_IMPL}, {153, "GetDropEventWithHandover", npns_drop_event, IPC_IMPL},
    {154, "CreateTokenAsync", npns_create_token_async, IPC_IMPL},
};
/* notif:a INotificationServicesForApplication alarm rows (notif_core.c keeps the alarms; they never fire, there is no HOME menu to raise them):
 *   500 RegisterAlarmSetting(type-5 AlarmSetting 0x40, type-5 application parameter) -> u16 alarm_setting_id
 *   510 UpdateAlarmSetting(type-5 AlarmSetting, type-5 parameter)   520 ListAlarmSettings(type-6 AlarmSetting[]) -> s32 count
 *   530 LoadApplicationParameter(u16 id, type-6 buffer) -> u32 size   540 DeleteAlarmSetting(u16 id)   1000 Initialize(u64 aruid, pid)
 * The result values for a full store, an unknown id and a malformed setting are not documented: napple uses its own (module NOTIF_MODULE, unverified). */
#define NOTIF_MODULE 252u                             /* [unverified] napple's choice */
#define R_NOTIF_INVALID HZ_RESULT(NOTIF_MODULE, 1)   /* [unverified] */
#define R_NOTIF_FULL HZ_RESULT(NOTIF_MODULE, 2)      /* [unverified] */
#define R_NOTIF_NOT_FOUND HZ_RESULT(NOTIF_MODULE, 3) /* [unverified] */
static NotifStore g_notif;
static int g_notif_initialized;
static pthread_mutex_t g_notif_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t notif_result(int r) { return r == NOTIF_OK ? 0 : r == NOTIF_ERR_FULL ? R_NOTIF_FULL : r == NOTIF_ERR_NOT_FOUND ? R_NOTIF_NOT_FOUND : R_NOTIF_INVALID; }
static const void *in_ptr(IpcCall *c, unsigned n, size_t *size) {
    const IpcBuf *b = ipc_in_buf(c, n);
    *size = b ? (size_t)b->size : 0;
    return b && b->size ? ipc_buf_ptr(b) : NULL;
}
static void notif_register_alarm(IpcObject *s, IpcCall *c) {
    (void)s;
    size_t ns, np;
    const void *set = in_ptr(c, 0, &ns), *par = in_ptr(c, 1, &np);
    uint16_t id = 0;
    pthread_mutex_lock(&g_notif_mu);
    int r = notif_register(&g_notif, set, ns, par, np, &id);
    pthread_mutex_unlock(&g_notif_mu);
    if (r) {
        c->result = notif_result(r);
        return;
    }
    IPC_OUT(c, id);
}
static void notif_update_alarm(IpcObject *s, IpcCall *c) {
    (void)s;
    size_t ns, np;
    const void *set = in_ptr(c, 0, &ns), *par = in_ptr(c, 1, &np);
    pthread_mutex_lock(&g_notif_mu);
    int r = notif_update(&g_notif, set, ns, par, np);
    pthread_mutex_unlock(&g_notif_mu);
    c->result = notif_result(r);
}
static void notif_list_alarms(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t tmp[NOTIF_ALARM_MAX * NOTIF_SETTING_SIZE];
    pthread_mutex_lock(&g_notif_mu);
    int32_t n = (int32_t)notif_list(&g_notif, tmp, b ? (b->size < sizeof tmp ? (size_t)b->size : sizeof tmp) : 0);
    pthread_mutex_unlock(&g_notif_mu);
    if (b && n) ipc_write_buf(c, b, tmp, (uint64_t)n * NOTIF_SETTING_SIZE);
    IPC_OUT(c, n);
}
static void notif_load_parameter(IpcObject *s, IpcCall *c) {
    (void)s;
    uint16_t id = IPC_IN(c, uint16_t, 0);
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t tmp[NOTIF_PARAM_MAX];
    uint32_t size = 0;
    pthread_mutex_lock(&g_notif_mu);
    int r = notif_load_param(&g_notif, id, tmp, sizeof tmp, &size);
    pthread_mutex_unlock(&g_notif_mu);
    if (r) {
        c->result = notif_result(r);
        return;
    }
    uint32_t w = b ? (uint32_t)ipc_write_buf(c, b, tmp, size) : 0;
    IPC_OUT(c, w);
}
static void notif_delete_alarm(IpcObject *s, IpcCall *c) {
    (void)s;
    uint16_t id = IPC_IN(c, uint16_t, 0);
    pthread_mutex_lock(&g_notif_mu);
    int r = notif_delete(&g_notif, id);
    pthread_mutex_unlock(&g_notif_mu);
    c->result = notif_result(r);
}
static void notif_initialize(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    pthread_mutex_lock(&g_notif_mu);
    g_notif_initialized = 1; /* the alarms belong to this process; a second Initialize keeps them */
    pthread_mutex_unlock(&g_notif_mu);
}
static const IpcCmd notif_rows[] = {
    {500, "RegisterAlarmSetting", notif_register_alarm, IPC_IMPL}, {510, "UpdateAlarmSetting", notif_update_alarm, IPC_IMPL},
    {520, "ListAlarmSettings", notif_list_alarms, IPC_IMPL},       {530, "LoadApplicationParameter", notif_load_parameter, IPC_IMPL},
    {540, "DeleteAlarmSetting", notif_delete_alarm, IPC_IMPL},     {1000, "Initialize", notif_initialize, IPC_IMPL},
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
