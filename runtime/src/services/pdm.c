/* pdm:qry (nn::pdm): play data. The running title's statistics are real (one launch, minutes since start); every other title and every
 * event or range query is empty. Command ids and the PlayStatistics layout are from the author's memory of the public documentation
 * (unverified). */
#include "svc_common.h"
#include "pdm_core.h"

static uint64_t start_ticks; static int started;
static uint64_t since_start(void) { if (!started) { started = 1; start_ticks = runtime_guest_ticks(); } return runtime_guest_ticks() - start_ticks; }

static void q_stats_by_app(IpcObject *s, IpcCall *c) {        /* 1(u8 flag, u64 application id) */
    (void)s; PdmStats st; pdm_stats_for(IPC_IN(c, uint64_t, 8), g_program_id, since_start(), &st); IPC_OUT(c, st);
}
static void q_stats_by_app_user(IpcObject *s, IpcCall *c) {   /* 2(u8 flag, u64 application id, u128 user id) */
    (void)s; PdmStats st; pdm_stats_for(IPC_IN(c, uint64_t, 8), g_program_id, since_start(), &st); IPC_OUT(c, st);
}
static void q_count_0(IpcObject *s, IpcCall *c) { (void)s; int32_t v = 0; IPC_OUT(c, v); }          /* event queries: nothing recorded */
static void q_range_0(IpcObject *s, IpcCall *c) { (void)s; int32_t v[3] = {0, 0, 0}; ipc_out(c, v, 12); }
static const IpcCmd pdm_cmds[] = {
    {0, "QueryAppletEvent", q_count_0, IPC_STUB}, {1, "QueryPlayStatisticsByApplicationId", q_stats_by_app, IPC_IMPL},
    {2, "QueryPlayStatisticsByApplicationIdAndUserAccountId", q_stats_by_app_user, IPC_IMPL},
    {6, "QueryPlayEvent", q_count_0, IPC_STUB}, {7, "GetAvailablePlayEventRange", q_range_0, IPC_STUB},
    {8, "QueryAccountEvent", q_count_0, IPC_STUB}, {9, "QueryAccountPlayEvent", q_count_0, IPC_STUB}, {10, "GetAvailableAccountPlayEventRange", q_range_0, IPC_STUB},
};
static IpcObject *pdm_open(void) { return IPC_OBJECT("pdm:qry", pdm_cmds, NULL, NULL); }
void service_register_pdm(void) { ipc_register_service("pdm:qry", pdm_open); }
