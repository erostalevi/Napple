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
    svc_call(o, 1, in, sizeof in, &c); CHECK(c.result == 0 && c.out_size == 0x18, "query by application id");
    PdmStats got; memcpy(&got, c.out, sizeof got); CHECK(got.application_id == g_program_id && got.play_minutes == 7 && got.launch_count == 1, "seven minutes since start");
    uint64_t other = 42; memcpy(in + 8, &other, 8); svc_call(o, 1, in, sizeof in, &c); memcpy(&got, c.out, sizeof got); CHECK(got.application_id == 42 && got.play_minutes == 0, "other id");
    uint8_t in2[32] = {0}; memcpy(in2 + 8, &g_program_id, 8); svc_call(o, 2, in2, sizeof in2, &c); memcpy(&got, c.out, sizeof got); CHECK(got.play_minutes == 7, "by application id and user");
#endif
    svc_call(o, 6, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "no play events");
    svc_call(o, 7, NULL, 0, &c); CHECK(c.out_size == 12, "event range");
    printf("{\"test\":\"svc_pdm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
