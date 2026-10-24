/* Unit tests: ncm tables answer empty databases. */
#include "../src/services/ncm.c"
#include "svc_test.h"

int main(void) {
    IpcObject *o = ncm_open(); IpcCall c;
    svc_call(o, 5, "\x01", 1, &c); CHECK(c.nout_objects == 1, "OpenContentMetaDatabase returns an object");
    IpcObject *db = c.out_objects[0];
    svc_call(db, 8, NULL, 0, &c); CHECK(c.result == 0 && c.out[0] == 0, "Has(key) = false");
    svc_call(db, 5, NULL, 0, &c); CHECK(c.out_size == 8 && SVC_OUT(&c, int32_t, 0) == 0 && SVC_OUT(&c, int32_t, 4) == 0, "List: total 0, written 0");
    svc_call(db, 6, NULL, 0, &c); CHECK(c.result == HZ_R_NCM_CONTENT_META_NOT_FOUND && HZ_MODULE(c.result) == HZ_MOD_NCM, "GetLatestContentMetaKey: ContentMetaNotFound");
    svc_call(db, 15, NULL, 0, &c); CHECK(c.result == 0, "Commit is accepted");
    svc_call(o, 4, "\x01", 1, &c); IpcObject *st = c.out_objects[0];
    svc_call(st, 7, NULL, 0, &c); CHECK(c.out[0] == 0, "storage Has(content id) = false");
    svc_call(st, 8, NULL, 0, &c); CHECK(c.result == HZ_R_NCM_CONTENT_NOT_FOUND, "storage GetPath: ContentNotFound");
    svc_call(st, 12, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "storage GetContentCount = 0");
    CHECK(HZ_DESC(HZ_R_NCM_CONTENT_NOT_FOUND) == 310, "result packing");
    printf("{\"test\":\"svc_ncm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
