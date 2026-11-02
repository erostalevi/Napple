/* Unit tests: the offline-module rows that became real in ipc-local-stubs (2026-11-01-r3): ISslConnection session cache mode (17, 18) and I/O timeout (34, 35),
 * nsd:u change-environment flag (15, 63), prepo:u 20102 / 20200 / 30100. offline.c is #included. */
#include "../src/services/offline.c"
#include "svc_test.h"

int main(void) {
    IpcCall c;
    /* ---- ssl connection */
    IpcObject *sl = ssl_open();
    svc_call(sl, 0, NULL, 0, &c);
    IpcObject *cx = c.out_objects[0];
    svc_call(cx, 2, NULL, 0, &c);
    IpcObject *cn = c.out_objects[0], *cn2;
    svc_call(cx, 2, NULL, 0, &c);
    cn2 = c.out_objects[0];
    uint32_t mode = 2;
    svc_call(cn, 17, &mode, 4, &c);
    CHECK(c.result == 0, "17 SetSessionCacheMode");
    svc_call(cn, 18, NULL, 0, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 2, "18 GetSessionCacheMode reads it back");
    svc_call(cn2, 18, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "...per connection");
    svc_call(cn, 20, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 2, "...and the earlier numbering (19 / 20) sees the same state");
    svc_call(cn, 35, NULL, 0, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0, "35 GetIoTimeout: none by default");
    uint32_t ms = 3000;
    svc_call(cn, 34, &ms, 4, &c);
    CHECK(c.result == 0, "34 SetIoTimeout");
    svc_call(cn, 35, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 3000, "...35 reads it back");
    svc_call(cn2, 35, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "...per connection");
    const unsigned ssl_rows[] = {17, 18, 34, 35};
    for (unsigned i = 0; i < 4; i++) CHECK(svc_find(cn, ssl_rows[i])->status == IPC_IMPL, "ISslConnection %u is real now", ssl_rows[i]);

    /* ---- nsd */
    IpcObject *ns = nsd_open();
    svc_call(ns, 63, NULL, 0, &c);
    CHECK(c.result == 0 && c.out_size == 1 && SVC_OUT(&c, uint8_t, 0) == 0, "63 IsChangeEnvironmentIdentifierDisabled: no by default");
    uint8_t on = 1;
    svc_call(ns, 15, &on, 1, &c);
    CHECK(c.result == 0, "15 SetChangeEnvironmentIdentifierDisabled");
    svc_call(ns, 63, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint8_t, 0) == 1, "...63 now says disabled");
    on = 0;
    svc_call(ns, 15, &on, 1, &c);
    svc_call(ns, 63, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint8_t, 0) == 0, "...and enabled again");
    CHECK(svc_find(ns, 15)->status == IPC_IMPL && svc_find(ns, 63)->status == IPC_IMPL, "nsd:u 15 and 63 are real now");

    /* ---- prepo */
    IpcObject *pr = prepo_open();
    pthread_mutex_lock(&prepo_lock);
    prepo_init(prepo_st());
    pthread_mutex_unlock(&prepo_lock);
    uint8_t rep[0x40] = {0};
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, rep, sizeof rep);
    svc_call_c(pr, 20102, &c);
    CHECK(c.result == 0, "20102 SaveSystemReport");
    svc_call(pr, 90200, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint64_t, 0) == 0x40, "...is accounted in the report storage like 20100");
    svc_call(pr, 30100, NULL, 0, &c);
    CHECK(c.result == 0, "30100 ClearStorage");
    svc_call(pr, 90200, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint64_t, 0) == 0, "...empties the storage like 90100");
    uint32_t om = 2;
    svc_call(pr, 20200, &om, 4, &c);
    CHECK(c.result == 0 && prepo_operation_mode == 2, "20200 SetOperationMode keeps the mode");
    svc_call(pr, 20200, NULL, 0, &c);
    CHECK(c.result == 0 && prepo_operation_mode == 2, "...a request without a mode changes nothing");
    const unsigned prepo_rows[] = {20102, 20200, 30100};
    for (unsigned i = 0; i < 3; i++) CHECK(svc_find(pr, prepo_rows[i])->status == IPC_IMPL, "prepo:u %u is real now", prepo_rows[i]);
    CHECK(svc_find(pr, 50100)->status == IPC_STUB, "50100 ReadAllApplicationReportFiles stays a documented default (reports are not kept)");
    printf("{\"test\":\"svc_local_offline\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
