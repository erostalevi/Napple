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
    printf("{\"test\":\"svc_push\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
