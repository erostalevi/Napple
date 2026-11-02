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
    /* ---- r11: npns:u local state ---- */
    {   npns_local_reset(); service_register_push(); n = open_npns();
        static const unsigned ids[5] = {103, 104, 111, 151, 153};
        for (int i = 0; i < 5; i++) CHECK(svc_find(n, ids[i]) && svc_find(n, ids[i])->status == IPC_IMPL, "npns %u is IPC_IMPL", ids[i]);
        svc_call(n, 103, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0, "GetState: not connected by default");
        svc_call(n, 151, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0, "GetStateWithHandover: same state");
        svc_call(n, 104, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 0 && SVC_OUT(&c, uint64_t, 8) == 0, "GetStatistics: nothing received or dropped");
        char jb[NPNS_JID_SIZE]; memset(jb, 0x55, sizeof jb); memset(&c, 0, sizeof c); svc_set_out_buf(&c, jb, sizeof jb); svc_call_c(n, 111, &c); CHECK(c.result == 0 && jb[0] == 0, "GetJid: empty until assigned");
        npns_local_set_jid("abc123@npns.example"); memset(&c, 0, sizeof c); svc_set_out_buf(&c, jb, sizeof jb); svc_call_c(n, 111, &c); CHECK(!strcmp(jb, "abc123@npns.example"), "GetJid returns the local JID");
        svc_call(n, 5, NULL, 0, &c); uint32_t rev = c.out_copy[0]; svc_call(n, 7, NULL, 0, &c); uint32_t cev = c.out_copy[0]; svc_call(n, 153, NULL, 0, &c); uint32_t dev = c.out_copy[0];
        CHECK(!event_is_signaled(rev) && !event_is_signaled(cev) && !event_is_signaled(dev), "all three events quiet at first");
        npns_local_set_state(2); CHECK(event_is_signaled(cev) && !event_is_signaled(rev), "a state change fires only the state-change event");
        svc_call(n, 103, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 2, "GetState reads the new state"); svc_call(n, 151, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 2, "...and so does the handover form");
        npns_local_enqueue("hi", 2); CHECK(event_is_signaled(rev) && !event_is_signaled(dev) && g_npns.nqueue == 1 && !memcmp(g_npns.queue[0].data, "hi", 2), "a notification queues and fires the receive event");
        svc_call(n, 104, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 1 && SVC_OUT(&c, uint64_t, 8) == 0, "statistics: one received");
        for (int i = 0; i < NPNS_QUEUE; i++) npns_local_enqueue("x", 1);
        CHECK(event_is_signaled(dev) && g_npns.nqueue == NPNS_QUEUE, "overflowing the queue drops the oldest and fires the drop event");
        svc_call(n, 104, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 17 && SVC_OUT(&c, uint64_t, 8) == 1, "statistics: 17 received, 1 dropped");
        npns_local_reset(); }
    printf("{\"test\":\"svc_push\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
