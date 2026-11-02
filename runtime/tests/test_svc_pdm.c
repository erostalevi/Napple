/* Unit tests: pdm play statistics. */
#include "../src/services/pdm.c"
#include "svc_test.h"
#ifdef __linux__
extern uint64_t mock_ticks;   /* ipc_mock.c: the guest tick counter; on macOS the IPC-level checks below are skipped (real clock) */
#endif

int main(void) {
    PdmStats st;
    pdm_stats_for(5, 5, 19200000ull * 60 * 90, &st);
    CHECK(st.application_id == 5 && st.play_minutes == 90 && st.launch_count == 1, "own title: 90 minutes, one launch");
    pdm_stats_for(6, 5, 19200000ull * 60 * 90, &st); CHECK(st.application_id == 6 && st.play_minutes == 0 && st.launch_count == 0, "other title: never played");
    pdm_stats_for(5, 5, 59ull * 19200000ull, &st); CHECK(st.play_minutes == 0, "under a minute");
    pdm_stats_for(5, 5, ~0ull, &st); CHECK(st.play_minutes > 0, "huge tick count does not wrap to 0");
    CHECK(sizeof(PdmStats) == 0x18, "size");

    IpcObject *o = pdm_open(); IpcCall c;
#ifdef __linux__
    mock_ticks = 1000; since_start();                      /* start = 1000 */
    mock_ticks = 1000 + 19200000ull * 60 * 7;
    uint8_t in[16] = {1}; memcpy(in + 8, &g_program_id, 8);
    svc_call(o, 4, in, sizeof in, &c); CHECK(c.result == 0 && c.out_size == 0x18, "query by application id");
    PdmStats got; memcpy(&got, c.out, sizeof got); CHECK(got.application_id == g_program_id && got.play_minutes == 7 && got.launch_count == 1, "seven minutes since start");
    uint64_t other = 42; memcpy(in + 8, &other, 8); svc_call(o, 4, in, sizeof in, &c); memcpy(&got, c.out, sizeof got); CHECK(got.application_id == 42 && got.play_minutes == 0, "other id");
    uint8_t in2[32] = {0}; memcpy(in2 + 8, &g_program_id, 8); svc_call(o, 5, in2, sizeof in2, &c); memcpy(&got, c.out, sizeof got); CHECK(got.play_minutes == 7, "by application id and user");
#endif
    svc_call(o, 8, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "no play events");
    svc_call(o, 9, NULL, 0, &c); CHECK(c.out_size == 12, "event range");
    svc_call(o, 10, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "no account events (10 QueryAccountEvent)");
    { /* ipc-local-stubs: the 10.0.0+ numbering of the statistics queries and QueryLastPlayTime */
        g_program_id = 0x0100000000010000ULL;
        uint8_t in[16] = {1}; memcpy(in + 8, &g_program_id, 8);
        svc_call(o, 4, in, sizeof in, &c); PdmStats ps; memcpy(&ps, c.out, sizeof ps);
        CHECK(c.result == 0 && c.out_size == 0x18 && ps.application_id == g_program_id && ps.launch_count == 1, "4 QueryPlayStatisticsByApplicationId: the running title was launched once");
        uint64_t other = 42; memcpy(in + 8, &other, 8); svc_call(o, 4, in, sizeof in, &c); memcpy(&ps, c.out, sizeof ps);
        CHECK(ps.application_id == 42 && ps.launch_count == 0 && ps.play_minutes == 0, "...another title was never played");
        uint8_t in2[32] = {1}; memcpy(in2 + 8, &g_program_id, 8);
        svc_call(o, 5, in2, sizeof in2, &c); memcpy(&ps, c.out, sizeof ps);
        CHECK(c.result == 0 && c.out_size == 0x18 && ps.application_id == g_program_id && ps.launch_count == 1, "5 QueryPlayStatisticsByApplicationIdAndUserAccountId");
        svc_call(o, 11, (uint8_t[32]){0}, 32, &c); CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 0, "11 QueryAccountPlayEvent: no events recorded");
        svc_call(o, 12, (uint8_t[16]){0}, 16, &c); CHECK(c.result == 0 && c.out_size == 12 && SVC_OUT(&c, int32_t, 0) == 0 && SVC_OUT(&c, int32_t, 4) == 0 && SVC_OUT(&c, int32_t, 8) == 0, "12 GetAvailableAccountPlayEventRange: {0, 0, 0}");
        uint64_t ids[3] = {42, g_program_id, 7}; uint8_t lt[3 * 0x18]; memset(lt, 0xcc, sizeof lt);
        memset(&c, 0, sizeof c); c.in = in; c.in_size = 1; svc_set_in_buf(&c, ids, sizeof ids); svc_set_out_buf(&c, lt, sizeof lt); svc_call_c(o, 17, &c);
        CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 3, "17 QueryLastPlayTime: one entry per requested title");
        CHECK(!memcmp(lt, &ids[0], 8) && lt[0x14] == 0 && *(uint32_t *)(lt + 0x10) == 0 && *(uint32_t *)(lt + 8) == 0, "...a never-played title: id echoed, not valid, no timestamps");
        uint32_t launch; memcpy(&launch, lt + 0x18 + 8, 4);
        CHECK(!memcmp(lt + 0x18, &g_program_id, 8) && lt[0x18 + 0x14] == 1 && launch > 1700000000u && !memcmp(lt + 0x18 + 8, lt + 0x18 + 12, 4) && *(uint32_t *)(lt + 0x18 + 0x10) == 0, "...the running title: valid, launched in the past, 0 minutes since it was last played");
        CHECK(lt[2 * 0x18 + 0x14] == 0, "...the third title is never-played too");
        uint8_t small[0x18 + 4]; memset(&c, 0, sizeof c); c.in = in; c.in_size = 1; svc_set_in_buf(&c, ids, sizeof ids); svc_set_out_buf(&c, small, sizeof small); svc_call_c(o, 17, &c);
        CHECK(SVC_OUT(&c, int32_t, 0) == 1, "...a small output buffer limits the entries written");
        memset(&c, 0, sizeof c); c.in = in; c.in_size = 1; svc_call_c(o, 17, &c); CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 0, "...no buffers: 0 entries");
        const unsigned rows[] = {4, 5, 11, 12, 17}; for (unsigned i = 0; i < 5; i++) CHECK(svc_find(o, rows[i])->status == IPC_IMPL, "pdm:qry %u is real now", rows[i]);
    }
    printf("{\"test\":\"svc_pdm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
