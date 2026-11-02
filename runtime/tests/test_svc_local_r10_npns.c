/* Unit tests (ipc-depth-95 r10, part local): npns:u 101 Suspend, 102 Resume, 120 CreateNotificationReceiver, 152 GetStateChangeEventWithHandover,
 * 154 CreateTokenAsync, with the NullProvider and with a provider that has a network (the loopback test provider). */
#include "../src/services/push.c"
#include "svc_test.h"
#include "online_loopback.h"

int main(void) {
    service_register_push();
    IpcObject *n = open_npns(); IpcCall c;
    const unsigned real[] = {101, 102, 120, 152, 154}, kept[] = {103, 104, 111, 151, 153};
    for (unsigned i = 0; i < 5; i++) {
        const IpcCmd *e = svc_find(n, real[i]);
        CHECK(e && e->status == IPC_IMPL, "npns:u %u is real", real[i]);
        e = svc_find(n, kept[i]);
        CHECK(e && e->status == IPC_IMPL, "npns:u %u is real (served from the host-side local state since r11 ipc-bcat-npns-local)", kept[i]);
    }

    /* Suspend / Resume and the state-change events */
    svc_call(n, 7, NULL, 0, &c); CHECK(c.result == 0 && c.nout_copy == 1, "7 GetStateChangeEvent"); uint32_t e7 = c.out_copy[0];
    svc_call(n, 152, NULL, 0, &c); CHECK(c.result == 0 && c.nout_copy == 1, "152 GetStateChangeEventWithHandover"); uint32_t e152 = c.out_copy[0];
    CHECK(!event_is_signaled(e7) && !event_is_signaled(e152), "...both clear at first");
    svc_call(n, 101, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 0 && g_npns_suspended, "101 Suspend: the daemon is suspended");
    CHECK(event_is_signaled(e7) && event_is_signaled(e152), "...and the state-change events fire");
    event_clear_handle(e7); event_clear_handle(e152);
    svc_call(n, 101, NULL, 0, &c); CHECK(c.result == 0 && g_npns_suspended && !event_is_signaled(e7) && !event_is_signaled(e152), "a second Suspend changes nothing: no event");
    svc_call(n, 102, NULL, 0, &c); CHECK(c.result == 0 && !g_npns_suspended && event_is_signaled(e7) && event_is_signaled(e152), "102 Resume wakes it and fires the events");
    event_clear_handle(e7);
    svc_call(n, 102, NULL, 0, &c); CHECK(c.result == 0 && !g_npns_suspended && !event_is_signaled(e7), "a second Resume changes nothing");
    IpcObject *n2 = open_npns(); svc_call(n2, 152, NULL, 0, &c); uint32_t e2 = c.out_copy[0];
    svc_call(n, 101, NULL, 0, &c); CHECK(event_is_signaled(e2), "the daemon is shared: another session's event fires too");
    svc_call(n2, 102, NULL, 0, &c); CHECK(!g_npns_suspended, "...and another session can resume it");

    /* INotificationReceiver */
    svc_call(n, 120, NULL, 0, &c); CHECK(c.result == 0 && c.nout_objects == 1 && !strcmp(c.out_objects[0]->iface, "INotificationReceiver"), "120 CreateNotificationReceiver");
    IpcObject *r = c.out_objects[0]; NpnsReceiver *rs = r->state;
    uint64_t tid = 0x0100000000001000ull;
    svc_call(r, 1, &tid, 8, &c); CHECK(c.result == 0 && rs->n == 1 && rs->ids[0] == tid, "receiver 1 ListenTo keeps the program id");
    svc_call(r, 1, &tid, 8, &c); CHECK(rs->n == 1, "...once");
    uint64_t tid2 = tid + 0x2000; svc_call(r, 1, &tid2, 8, &c); CHECK(rs->n == 2 && rs->ids[1] == tid2, "...a second id is added");
    { uint64_t z = 0; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&z; c.in_size = 8; c.has_pid = 1; c.pid = 0x77; svc_call_c(r, 2, &c);
      CHECK(c.result == 0 && rs->my_application && rs->my_application_pid == 0x77, "receiver 2 ListenToMyApplicationId records the caller"); }
    svc_call(r, 4, NULL, 0, &c); CHECK(c.result == 0 && c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "receiver 4 GetReceiveEvent: nothing is ever pushed");
    { uint8_t buf[0x80]; memset(buf, 0xee, sizeof buf); uint16_t arg = 0; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&arg; c.in_size = 2; svc_set_out_buf(&c, buf, sizeof buf);
      svc_call_c(r, 3, &c); CHECK(c.result == 0 && buf[0] == 0xee && c.out_size == 0, "receiver 3 Receive: nothing written"); }
    svc_call(n, 120, NULL, 0, &c); IpcObject *r2 = c.out_objects[0]; CHECK(((NpnsReceiver *)r2->state)->n == 0, "listened ids are per receiver");
    free_state(r); free_state(r2);

    /* CreateTokenAsync: NullProvider */
    online_provider_install(NULL);
    svc_call(n, 154, NULL, 0, &c); CHECK(c.result == 0 && c.nout_objects == 1 && !strcmp(c.out_objects[0]->iface, "ICreateTokenAsyncContext"), "154 CreateTokenAsync returns an async context");
    IpcObject *t = c.out_objects[0];
    svc_call(t, 1, NULL, 0, &c); CHECK(c.result == 0 && c.nout_copy == 1 && event_is_signaled(c.out_copy[0]), "GetFinishEvent: already finished");
    svc_call(t, 3, NULL, 0, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "GetResult offline: no network");
    { uint8_t tok[0x28]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, tok, sizeof tok); svc_call_c(t, 10, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "GetToken offline: the same failure"); }
    svc_call(t, 2, NULL, 0, &c); CHECK(c.result == 0, "Cancel after the finish is harmless");
    svc_call(t, 3, NULL, 0, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "...and the result stays");
    free_state(t);
    /* with a network (the loopback provider): the push server is still out of reach */
    memset(&lb, 0, sizeof lb); online_provider_install(&lb_provider);
    svc_call(n, 154, NULL, 0, &c); t = c.out_objects[0];
    svc_call(t, 3, NULL, 0, &c); CHECK(c.result == R_NPNS_SERVER_UNAVAILABLE && HZ_MODULE(c.result) == 154, "GetResult with a network: the push server is unavailable (npns module)");
    svc_call(t, 10, NULL, 0, &c); CHECK(c.result == R_NPNS_SERVER_UNAVAILABLE, "GetToken with a network: the same");
    free_state(t);
    online_provider_install(NULL);

    printf("{\"test\":\"svc_local_r10_npns\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
