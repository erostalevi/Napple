/* Unit tests: ngc:u (profanity filter) never flags text. */
#include "../src/services/ngc.c"
#include "svc_test.h"

int main(void) {
    IpcObject *o = ngc_open(); IpcCall c;
    svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 4, "GetContentVersion");
    uint32_t flags = 0;
    svc_call(o, 1, &flags, 4, &c); CHECK(c.result == 0 && c.out_size == 4 && SVC_OUT(&c, uint32_t, 0) == 0, "Check: clean");
    svc_call(o, 3, NULL, 0, &c); CHECK(c.result == 0, "Reload");
    printf("{\"test\":\"svc_ngc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
