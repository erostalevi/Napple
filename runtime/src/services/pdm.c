/* pdm:qry (nn::pdm): play data. The running title's statistics are real (one launch, minutes since start); every other title and every
 * event or range query is empty. Command ids and the PlayStatistics layout are from the author's memory of the public documentation
 * (unverified). */
#include "svc_common.h"
#include "pdm_core.h"
#include "sys_default.h"
#include <string.h>
#include <time.h>

static uint64_t start_ticks;
static int started;
static uint64_t since_start(void) {
    if (!started) {
        started = 1;
        start_ticks = runtime_guest_ticks();
    }
    return runtime_guest_ticks() - start_ticks;
}

static void q_stats_by_app(IpcObject *s, IpcCall *c) { /* 1(u8 flag, u64 application id) */
    (void)s;
    PdmStats st;
    pdm_stats_for(IPC_IN(c, uint64_t, 8), g_program_id, since_start(), &st);
    IPC_OUT(c, st);
}
static void q_stats_by_app_user(IpcObject *s, IpcCall *c) { /* 2(u8 flag, u64 application id, u128 user id) */
    (void)s;
    PdmStats st;
    pdm_stats_for(IPC_IN(c, uint64_t, 8), g_program_id, since_start(), &st);
    IPC_OUT(c, st);
}
static void q_count_0(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = 0;
    IPC_OUT(c, v);
} /* event queries: nothing recorded */
static void q_range_0(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v[3] = {0, 0, 0};
    ipc_out(c, v, 12);
}
/* 17 QueryLastPlayTime(u8 flag; in: u64 application ids; out: LastPlayTime[]) -> s32 entries. LastPlayTime (0x18): u64 application id, two u32 timestamps (the launch time of
 * the last session, twice), u32 minutes since it was last played, u8 "minutes valid", 3 pad. One entry per requested id; only the running title has been played: its
 * last session is the current one (0 minutes ago, valid), every other id is never-played (all zero, not valid). */
static void q_last_play_time(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0);
    const uint64_t *ids = in ? ipc_buf_ptr(in) : NULL;
    int32_t n = 0;
    if (ids && out) {
        uint64_t cap = out->size / 0x18, want = in->size / 8;
        if (want > cap) want = cap;
        if (want > 256) want = 256;
        uint8_t ent[256][0x18];
        memset(ent, 0, sizeof ent);
        uint32_t launch = (uint32_t)((uint64_t)time(NULL) - since_start() / 19200000ull);
        for (uint64_t i = 0; i < want; i++) {
            memcpy(ent[i], &ids[i], 8);
            if (ids[i] != g_program_id) continue;
            memcpy(ent[i] + 8, &launch, 4);
            memcpy(ent[i] + 12, &launch, 4);
            ent[i][0x14] = 1;
        }
        if (want) ipc_write_buf(c, out, ent, want * 0x18);
        n = (int32_t)want;
    }
    IPC_OUT(c, n);
}
/* 18 QueryApplicationPlayStatisticsForSystem(u8 flag; type-5 u64 application ids; type-6 ApplicationPlayStatistics[]) -> s32 entries, and
 * 19 ...ByUserAccountIdForSystem (u8 flag, pad, Uid; same buffers). ApplicationPlayStatistics (0x18, libnx layout): u64 application id, u64 play time in
 * nanoseconds, u64 launch count. One entry per requested id, as 17 does: the running title has one launch and the time since start (the same source as
 * 4 / 5); every other id has never been played (zero play time and launches). The user is not told apart: one user plays on this host. */
static void q_app_stats_system(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *in = ipc_in_buf(c, 0), *out = ipc_out_buf(c, 0);
    const uint64_t *ids = in ? ipc_buf_ptr(in) : NULL;
    int32_t n = 0;
    if (ids && out) {
        uint64_t want = in->size / 8, cap = out->size / 0x18;
        if (want > cap) want = cap;
        if (want > 256) want = 256;
        uint64_t ent[256][3];
        uint64_t ticks = since_start();
        for (uint64_t i = 0; i < want; i++) {
            ent[i][0] = ids[i];
            ent[i][1] = ids[i] == g_program_id ? ticks / 192 * 10000 + ticks % 192 * 10000 / 192 : 0; /* 19.2 MHz ticks -> ns */
            ent[i][2] = ids[i] == g_program_id ? 1 : 0;
        }
        if (want) ipc_write_buf(c, out, ent, want * 0x18);
        n = (int32_t)want;
    }
    IPC_OUT(c, n);
}
/* Event history: this host keeps no play log (only the running title's statistics above are tracked), which is how a console that was just
 * reset answers: zero events returned, an empty available range {0, 0, 0}. Documented no-ops rather than stubs. */
static const IpcCmd pdm_cmds[] = {
    /* ids per switchbrew 'Shared Database services': 4 / 5 current statistics queries (the 1.0.0-6.2.0 ids 1-3 and 6 were other queries and are gone),
     * 8-12 play / account event queries, 17 QueryLastPlayTime (the layout (u8 flag, u64 application id[, u128 user]) is what the handlers read) */
    {0, "QueryAppletEvent", q_count_0, IPC_IMPL},
    {4, "QueryPlayStatisticsByApplicationId", q_stats_by_app, IPC_IMPL},
    {5, "QueryPlayStatisticsByApplicationIdAndUserAccountId", q_stats_by_app_user, IPC_IMPL},
    {8, "QueryPlayEvent", q_count_0, IPC_IMPL},
    {9, "GetAvailablePlayEventRange", q_range_0, IPC_IMPL},
    {10, "QueryAccountEvent", q_count_0, IPC_IMPL},
    {11, "QueryAccountPlayEvent", q_count_0, IPC_IMPL},
    {12, "GetAvailableAccountPlayEventRange", q_range_0, IPC_IMPL},
    {17, "QueryLastPlayTime", q_last_play_time, IPC_IMPL},
    {18, "QueryApplicationPlayStatisticsForSystem", q_app_stats_system, IPC_IMPL},
    {19, "QueryApplicationPlayStatisticsByUserAccountIdForSystem", q_app_stats_system, IPC_IMPL},
};
static IpcObject *pdm_open(void) { return IPC_OBJECT("pdm:qry", pdm_cmds, NULL, NULL); }
void service_register_pdm(void) { ipc_register_service("pdm:qry", pdm_open); }
