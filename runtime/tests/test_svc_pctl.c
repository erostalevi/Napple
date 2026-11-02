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
    /* ipc-reference-audit: ids per switchbrew (1018 IsFreeCommunicationAvailable, 1035 GetCurrentSettings, 1061/1062/1065 stereo vision, 1451 StartPlayTimer) */
    svc_call(p, 1018, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 0, "IsFreeCommunicationAvailable: available");
    svc_call(p, 1035, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 12 && c.out[0] == 0, "GetCurrentSettings: a zero 12-byte restriction block");
    svc_call(p, 1061, NULL, 0, &c); CHECK(c.result == 0, "ConfirmStereoVisionRestrictionConfigurable: ok");
    svc_call(p, 1062, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == 0, "GetStereoVisionRestriction: not restricted");
    svc_call(p, 1065, NULL, 0, &c); CHECK(c.out[0] == 1, "stereo vision is permitted without controls");
    svc_call(p, 1451, NULL, 0, &c); CHECK(c.result == 0, "StartPlayTimer: ok");
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
    { /* ipc-reference-audit: aoc 11 / 12 / 13 per switchbrew (NotifyMount, NotifyUnmount, IsAddOnContentMountedForDebug) keep a mount count */
        CHECK(svc_find(aoc, 14) == NULL, "14 is not a row");
        svc_call(aoc, 13, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == 0, "13 IsAddOnContentMountedForDebug: nothing mounted at first");
        svc_call(aoc, 11, NULL, 0, &c); CHECK(c.result == 0, "11 NotifyMountAddOnContent");
        svc_call(aoc, 13, NULL, 0, &c); CHECK(c.out[0] == 1, "...13 reports it mounted");
        svc_call(aoc, 12, NULL, 0, &c); svc_call(aoc, 12, NULL, 0, &c); svc_call(aoc, 13, NULL, 0, &c); CHECK(c.out[0] == 0, "12 NotifyUnmountAddOnContent (twice is harmless): not mounted");
    }
    printf("{\"test\":\"svc_pctl\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
