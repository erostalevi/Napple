/* Unit tests: the offline push / notification stand-ins accept requests and hand out never-firing events. */
#include "../src/services/push.c"
#include "svc_test.h"

int main(void) {
    service_register_push();
    IpcObject *n = open_npns(); IpcCall c;
    svc_call(n, 1, NULL, 0, &c); CHECK(c.result == 0 && c.nout_copy == 0, "npns ListenAll succeeds with no output");
    svc_call(n, 5, NULL, 0, &c); CHECK(c.result == 0 && c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "npns receive event never fires");
    svc_call(n, 7, NULL, 0, &c); CHECK(c.nout_copy == 1, "npns state change event");
    IpcObject *o = open_notif(); svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0, "notif:a request succeeds");
    o = open_mnpp(); svc_call(o, 3, NULL, 0, &c); CHECK(c.result == 0, "mnpp:app request succeeds");
    o = open_ngct(); svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0, "ngct:u request succeeds");
    o = open_nim_eca(); svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0, "nim:eca request succeeds");
    { /* ipc-depth-cloud r9: notif:a alarm settings */
        IpcObject *na = open_notif(); IpcCall d;
        uint64_t aruid = 0; svc_call(na, 1000, &aruid, 8, &d); CHECK(d.result == 0 && g_notif_initialized, "1000 Initialize");
        uint8_t list[NOTIF_ALARM_MAX * NOTIF_SETTING_SIZE + 0x40];
        memset(&d, 0, sizeof d); svc_set_out_buf(&d, list, sizeof list); svc_call_c(na, 520, &d); CHECK(d.result == 0 && SVC_OUT(&d, int32_t, 0) == 0, "520 ListAlarmSettings: none at first");
        uint8_t set[NOTIF_SETTING_SIZE]; memset(set, 0, sizeof set); set[2] = 1; set[0x18] = 0xab; /* kind 1, an application id byte */
        char par[] = "wake me up";
        memset(&d, 0, sizeof d); svc_set_in_buf(&d, set, sizeof set); svc_set_in_buf(&d, par, sizeof par); svc_call_c(na, 500, &d);
        uint16_t id1 = SVC_OUT(&d, uint16_t, 0); CHECK(d.result == 0 && d.out_size == 2 && id1 == 1, "500 RegisterAlarmSetting: id 1");
        memset(&d, 0, sizeof d); svc_set_in_buf(&d, set, sizeof set); svc_set_in_buf(&d, par, 4); svc_call_c(na, 500, &d);
        uint16_t id2 = SVC_OUT(&d, uint16_t, 0); CHECK(d.result == 0 && id2 == 2, "...a second alarm gets id 2");
        memset(&d, 0, sizeof d); svc_set_out_buf(&d, list, sizeof list); svc_call_c(na, 520, &d);
        uint16_t l0, l1; memcpy(&l0, list, 2); memcpy(&l1, list + NOTIF_SETTING_SIZE, 2);
        CHECK(SVC_OUT(&d, int32_t, 0) == 2 && l0 == 1 && l1 == 2 && list[2] == 1 && list[0x18] == 0xab, "...both listed in order, with the id written into the setting");
        char got[0x40] = {0}; memset(&d, 0, sizeof d); d.in = (const uint8_t *)&id1; d.in_size = 2; svc_set_out_buf(&d, got, sizeof got); svc_call_c(na, 530, &d);
        CHECK(d.result == 0 && SVC_OUT(&d, uint32_t, 0) == sizeof par && !strcmp(got, "wake me up"), "530 LoadApplicationParameter");
        uint8_t upd[NOTIF_SETTING_SIZE]; memcpy(upd, list, sizeof upd); upd[3] = 1; /* muted */
        memset(&d, 0, sizeof d); svc_set_in_buf(&d, upd, sizeof upd); svc_set_in_buf(&d, "new", 4); svc_call_c(na, 510, &d); CHECK(d.result == 0, "510 UpdateAlarmSetting");
        memset(&d, 0, sizeof d); svc_set_out_buf(&d, list, sizeof list); svc_call_c(na, 520, &d); CHECK(list[3] == 1, "...the update is listed");
        memset(&d, 0, sizeof d); d.in = (const uint8_t *)&id1; d.in_size = 2; svc_set_out_buf(&d, got, sizeof got); svc_call_c(na, 530, &d); CHECK(SVC_OUT(&d, uint32_t, 0) == 4 && !strcmp(got, "new"), "...and its new parameter");
        uint16_t bogus = 77; svc_call(na, 540, &bogus, 2, &d); CHECK(d.result == R_NOTIF_NOT_FOUND, "540 DeleteAlarmSetting of an unknown id");
        svc_call(na, 540, &id1, 2, &d); CHECK(d.result == 0, "540 DeleteAlarmSetting");
        memset(&d, 0, sizeof d); svc_set_out_buf(&d, list, sizeof list); svc_call_c(na, 520, &d); memcpy(&l0, list, 2);
        CHECK(SVC_OUT(&d, int32_t, 0) == 1 && l0 == 2, "...only alarm 2 is left");
        svc_call(na, 530, &id1, 2, &d); CHECK(d.result == R_NOTIF_NOT_FOUND, "...and 1's parameter is gone");
        memset(&d, 0, sizeof d); svc_set_in_buf(&d, set, 8); svc_call_c(na, 500, &d); CHECK(d.result == R_NOTIF_INVALID, "a short setting is refused");
        for (unsigned i = 1; i < NOTIF_ALARM_MAX; i++) { memset(&d, 0, sizeof d); svc_set_in_buf(&d, set, sizeof set); svc_call_c(na, 500, &d); }
        memset(&d, 0, sizeof d); svc_set_in_buf(&d, set, sizeof set); svc_call_c(na, 500, &d); CHECK(d.result == R_NOTIF_FULL, "the store holds %u alarms", NOTIF_ALARM_MAX);
        NotifStore st; notif_init(&st); static uint8_t big[NOTIF_PARAM_MAX + 1]; uint16_t x;
        CHECK(notif_register(&st, set, sizeof set, big, sizeof big, &x) == NOTIF_ERR_INVALID, "a parameter over 0x400 bytes is refused");
        for (unsigned i = 0; i < 6; i++) { const IpcCmd *e = svc_find(na, i == 5 ? 1000 : 500 + i * 10); CHECK(e && e->status == IPC_IMPL, "notif:a row %u is real", e ? e->cmd : 0); }
    }
    printf("{\"test\":\"svc_push\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
