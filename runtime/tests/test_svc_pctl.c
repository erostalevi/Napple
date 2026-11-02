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
    for (unsigned i = 0; i < aoc->ncmds; i++) { /* rows 300 / 301 (host add-on content setup) are documented defaults; 200 and 302 are real (ipc-local-stubs), 100 / 101 since r9, 110 since r10 */
        unsigned id = aoc->cmds[i].cmd, dflt = id == 300 || id == 301;
        CHECK(aoc->cmds[i].status == (dflt ? IPC_STUB : IPC_IMPL), "aoc row %u is %s", id, dflt ? "a documented default" : "resolved");
    }
    { /* ipc-reference-audit: aoc 11 / 12 / 13 per switchbrew (NotifyMount, NotifyUnmount, IsAddOnContentMountedForDebug) keep a mount count */
        CHECK(svc_find(aoc, 14) == NULL, "14 is not a row");
        svc_call(aoc, 13, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == 0, "13 IsAddOnContentMountedForDebug: nothing mounted at first");
        svc_call(aoc, 11, NULL, 0, &c); CHECK(c.result == 0, "11 NotifyMountAddOnContent");
        svc_call(aoc, 13, NULL, 0, &c); CHECK(c.out[0] == 1, "...13 reports it mounted");
        svc_call(aoc, 12, NULL, 0, &c); svc_call(aoc, 12, NULL, 0, &c); svc_call(aoc, 13, NULL, 0, &c); CHECK(c.out[0] == 0, "12 NotifyUnmountAddOnContent (twice is harmless): not mounted");
    }
    { /* ipc-depth-cloud r9: local parental-control settings */
        CHECK(pctl_default_org(0) == PCTL_ORG_CERO && pctl_default_org(1) == PCTL_ORG_ESRB && pctl_default_org(2) == PCTL_ORG_PEGI && pctl_default_org(3) == PCTL_ORG_ACB && pctl_default_org(99) == PCTL_ORG_PEGI, "default rating organization by region");
        svc_call(p, 1406, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 8 && SVC_OUT(&c, int64_t, 0) == 0, "1406 GetSettingsLastUpdated: never changed");
        svc_call(p, 1037, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 4 && SVC_OUT(&c, int32_t, 0) == PCTL_ORG_PEGI, "1037 GetDefaultRatingOrganization: PEGI without a region override");
        int32_t org = PCTL_ORG_USK; svc_call(p, 1038, &org, 4, &c); CHECK(c.result == 0, "1038 SetDefaultRatingOrganization");
        svc_call(p, 1037, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == PCTL_ORG_USK, "...read back");
        org = 77; svc_call(p, 1038, &org, 4, &c); CHECK(c.result == R_PCTL_INVALID_ARGUMENT, "...an unknown organization is refused");
        svc_call(p, 1406, NULL, 0, &c); CHECK(SVC_OUT(&c, int64_t, 0) > 1700000000, "1406: the change time is kept");

        svc_call(p, 1206, NULL, 0, &c); CHECK(c.out_size == 4 && SVC_OUT(&c, int32_t, 0) == 0, "1206 GetPinCodeLength: no PIN");
        char any[4] = "999"; IpcCall d; memset(&d, 0, sizeof d); svc_set_in_buf(&d, any, sizeof any); svc_call_c(p, 1201, &d);
        CHECK(d.result == 0, "1201 UnlockRestrictionTemporarily without a PIN succeeds");
        svc_call(p, 1006, NULL, 0, &c); CHECK(c.out[0] == 1, "...1006 IsRestrictionTemporaryUnlocked: yes");
        svc_call(p, 1007, NULL, 0, &c); svc_call(p, 1006, NULL, 0, &c); CHECK(c.out[0] == 0, "1007 RevertRestrictionTemporaryUnlocked clears it");
        svc_call(p, 1207, NULL, 0, &c); uint32_t ev = c.nout_copy ? c.out_copy[0] : 0; CHECK(ev && !event_is_signaled(ev), "1207 GetPinCodeChangedEvent");
        char pin[9] = "4321"; memset(&d, 0, sizeof d); svc_set_in_buf(&d, pin, sizeof pin); svc_call_c(p, 1203, &d);
        CHECK(d.result == 0 && event_is_signaled(ev), "1203 SetPinCode signals the change event");
        svc_call(p, 1206, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 4, "...length 4");
        char got[16]; memset(got, 'x', sizeof got); memset(&d, 0, sizeof d); svc_set_out_buf(&d, got, sizeof got); svc_call_c(p, 1208, &d);
        CHECK(d.result == 0 && SVC_OUT(&d, int32_t, 0) == 4 && !strcmp(got, "4321"), "1208 GetPinCode");
        memset(&d, 0, sizeof d); svc_set_in_buf(&d, any, sizeof any); svc_call_c(p, 1201, &d); CHECK(d.result == R_PCTL_WRONG_PIN, "1201 with a wrong PIN is refused");
        svc_call(p, 1006, NULL, 0, &c); CHECK(c.out[0] == 0, "...and grants nothing");
        memset(&d, 0, sizeof d); svc_set_in_buf(&d, pin, sizeof pin); svc_call_c(p, 1201, &d); CHECK(d.result == 0, "1201 with the right PIN");
        svc_call(p, 1006, NULL, 0, &c); CHECK(c.out[0] == 1, "...unlocks temporarily");
        memset(&d, 0, sizeof d); svc_set_in_buf(&d, any, sizeof any); svc_call_c(p, 1202, &d); CHECK(d.result == R_PCTL_WRONG_PIN, "1202 UnlockSystemSettingsRestriction checks the PIN");
        memset(&d, 0, sizeof d); svc_set_in_buf(&d, pin, sizeof pin); svc_call_c(p, 1209, &d); CHECK(d.result == 0, "1209 (newer id) with the right PIN");
        char badpin[4] = "12a"; memset(&d, 0, sizeof d); svc_set_in_buf(&d, badpin, sizeof badpin); svc_call_c(p, 1203, &d); CHECK(d.result == R_PCTL_INVALID_ARGUMENT, "a PIN that is not 4-8 digits is refused");
        char none[1] = ""; memset(&d, 0, sizeof d); svc_set_in_buf(&d, none, sizeof none); svc_call_c(p, 1203, &d); CHECK(d.result == 0, "an empty PIN removes it");
        svc_call(p, 1006, NULL, 0, &c); CHECK(c.out[0] == 0, "...which ends the temporary unlock");

        svc_call(p, 1039, NULL, 0, &c); CHECK(c.out_size == 4 && SVC_OUT(&c, int32_t, 0) == 0, "1039 GetFreeCommunicationApplicationListCount: empty");
        uint64_t a1 = 0x0100000000001000ull, a2 = 0x0100000000002000ull;
        svc_call(p, 1042, &a1, 8, &c); svc_call(p, 1050, &a2, 8, &c); svc_call(p, 1050, &a1, 8, &c);
        svc_call(p, 1039, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 2, "1042 / 1050 add (an id already listed is kept once)");
        uint8_t ent[4 * 0x10]; int32_t off = 0; memset(&d, 0, sizeof d); d.in = (const uint8_t *)&off; d.in_size = 4; svc_set_out_buf(&d, ent, sizeof ent); svc_call_c(p, 1044, &d);
        CHECK(SVC_OUT(&d, int32_t, 0) == 2 && !memcmp(ent, &a1, 8) && ent[8] == 1 && !memcmp(ent + 0x10, &a2, 8), "1044 GetFreeCommunicationApplicationList: both, restricted");
        uint8_t upd[0x10] = {0}; memcpy(upd, &a2, 8); upd[8] = 0; memset(&d, 0, sizeof d); svc_set_in_buf(&d, upd, sizeof upd); svc_call_c(p, 1045, &d);
        off = 1; memset(&d, 0, sizeof d); d.in = (const uint8_t *)&off; d.in_size = 4; svc_set_out_buf(&d, ent, sizeof ent); svc_call_c(p, 1044, &d);
        CHECK(SVC_OUT(&d, int32_t, 0) == 1 && !memcmp(ent, &a2, 8) && ent[8] == 0, "1045 Update lifts the restriction of a2; 1044 from offset 1");
        svc_call(p, 1049, NULL, 0, &c); svc_call(p, 1039, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "1049 ResetFreeCommunicationApplicationList");
        const unsigned rows[] = {1006, 1007, 1037, 1038, 1039, 1042, 1044, 1045, 1049, 1050, 1201, 1202, 1203, 1206, 1207, 1208, 1209, 1406};
        for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) CHECK(svc_find(p, rows[i]) && svc_find(p, rows[i])->status == IPC_IMPL, "pctl %u is a real row", rows[i]);
    }
    printf("{\"test\":\"svc_pctl\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
