/* Unit tests: pctl reports a console with parental controls off; aoc mount/event rows are accepted. */
#include "../src/services/misc.c"
#ifdef __linux__
#include "../src/json.c"
#endif
#include "svc_test.h"

int main(void) {
    IpcObject *root = pctl_open(); IpcCall c;
    svc_call(root, 0, NULL, 0, &c); CHECK(c.nout_objects == 1, "CreateService"); IpcObject *p = c.out_objects[0];
    svc_call(root, 1, NULL, 0, &c); CHECK(c.nout_objects == 1, "CreateServiceWithoutInitialize");
    svc_call(p, 1, NULL, 0, &c); CHECK(c.result == 0, "Initialize");
    for (unsigned id = 1001; id <= 1005; id++) { svc_call(p, id, NULL, 0, &c); CHECK(c.result == 0, "permission %u confirmed", id); }
    svc_call(p, 1031, NULL, 0, &c); CHECK(c.out_size == 1 && c.out[0] == 0, "no restriction enabled");
    svc_call(p, 1032, NULL, 0, &c); CHECK(c.out_size == 4 && SVC_OUT(&c, uint32_t, 0) == 0, "safety level none");
    svc_call(p, 1018, NULL, 0, &c); CHECK(c.out[0] == 0, "no temporary unlock needed");
    svc_call(p, 1451, NULL, 0, &c); CHECK(c.out[0] == 1, "stereo vision is permitted without controls");
    /* ipc-depth-sys: the application-reachable rows beyond the first set */
    svc_call(p, 1006, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == 0, "IsRestrictionTemporaryUnlocked: false");
    svc_call(p, 1015, NULL, 0, &c); CHECK(c.result == 0, "ConfirmPlayableApplicationVideo: allowed");
    svc_call(p, 1454, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 8, "GetPlayTimerRemainingTime: a zero s64");
    svc_call(p, 1457, NULL, 0, &c); CHECK(c.result == 0 && c.nout_copy + c.nout_move >= 1, "GetPlayTimerEventToRequestSuspension: an event");
    svc_call(p, 1603, NULL, 0, &c); CHECK(c.result == 0 && c.out[0] == 0, "IsAllFeaturesDisabled: false");
    for (unsigned i = 0; i < ((IpcObject *)p)->ncmds; i++) CHECK(p->cmds[i].status == IPC_IMPL, "pctl row %u is resolved", p->cmds[i].cmd);
    IpcObject *aoc = aoc_open();
    for (unsigned i = 0; i < aoc->ncmds; i++) { /* rows 100..110 (purchase managers) and 300 / 301 (host add-on content setup) are documented defaults; 200 and 302 are real (ipc-local-stubs) */
        unsigned id = aoc->cmds[i].cmd, dflt = id == 100 || id == 101 || id == 110 || id == 300 || id == 301;
        CHECK(aoc->cmds[i].status == (dflt ? IPC_STUB : IPC_IMPL), "aoc row %u is %s", id, dflt ? "a documented default" : "resolved");
    }
    printf("{\"test\":\"svc_pctl\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
