/* Unit tests: fgm requests are accepted and remembered. */
#include "../src/services/fgm.c"
#include "svc_test.h"

int main(void) {
    IpcObject *o = fgm_open(); IpcCall c;
    svc_call(o, 0, NULL, 0, &c); CHECK(c.nout_objects == 1, "Initialize returns an IRequest");
    IpcObject *rq = c.out_objects[0], *rq2; uint32_t init[2] = {3, 1};
    svc_call(rq, 0, init, 8, &c); CHECK(c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]) && ((FgmRequest *)rq->state)->module == 3, "request Initialize: unsignalled event, module stored");
    uint32_t range[2] = {100, 400}; svc_call(rq, 1, range, 8, &c); svc_call(rq, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 400, "Set then Get");
    range[0] = 500; range[1] = 200; svc_call(rq, 1, range, 8, &c); svc_call(rq, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 500, "reversed range collapses to the lower bound");
    svc_call(rq, 3, NULL, 0, &c); svc_call(rq, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "Cancel clears");
    svc_call(o, 0, NULL, 0, &c); rq2 = c.out_objects[0]; CHECK(rq2->state != rq->state, "each request has its own state");
    printf("{\"test\":\"svc_fgm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
