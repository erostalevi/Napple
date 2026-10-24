/* Unit tests: resolver error strings and the offline answers. */
#include "../src/services/sfdnsres.c"
#include "svc_test.h"

int main(void) {
    CHECK(!strcmp(sfdns_hstrerror(1), "Unknown host") && !strcmp(sfdns_hstrerror(2), "Host name lookup failure"), "hstrerror table");
    CHECK(!strcmp(sfdns_hstrerror(77), "Unknown resolver error"), "hstrerror out of range");
    CHECK(!strcmp(sfdns_gai_strerror(0), "Success") && !strcmp(sfdns_gai_strerror(8), "hostname nor servname provided, or not known") && !strcmp(sfdns_gai_strerror(2), "Temporary failure in name resolution"), "gai_strerror table");
    CHECK(!strcmp(sfdns_gai_strerror(99), "Unknown error"), "gai_strerror out of range");
    IpcObject *o = res_open(); IpcCall c; char buf[64]; uint32_t e = 2;
    memset(&c, 0, sizeof c); c.in = (const uint8_t *)&e; c.in_size = 4; svc_set_out_buf(&c, buf, sizeof buf); svc_call_c(o, 4, &c);
    CHECK(c.result == 0 && !strcmp(buf, "Host name lookup failure"), "GetHostStringErrorRequest writes the text");
    e = 8; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&e; c.in_size = 4; svc_set_out_buf(&c, buf, 6); svc_call_c(o, 5, &c);
    CHECK(!strncmp(buf, "hostn", 5) && buf[5] == 0, "text is truncated to the buffer and NUL-terminated");
    svc_call(o, 2, NULL, 0, &c); CHECK(c.out_size == 12 && SVC_OUT(&c, uint32_t, 0) == HZ_NETDB_TRY_AGAIN && SVC_OUT(&c, uint32_t, 8) == 0, "GetHostByName: TRY_AGAIN, no data");
    svc_call(o, 6, NULL, 0, &c); CHECK(c.out_size == 12 && SVC_OUT(&c, int32_t, 0) == HZ_EAI_AGAIN && SVC_OUT(&c, uint32_t, 8) == 0, "GetAddrInfo: EAI_AGAIN, no data");
    svc_call(o, 8, NULL, 0, &c); uint32_t h1 = SVC_OUT(&c, uint32_t, 0); svc_call(o, 8, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == h1 + 1, "cancel handles are distinct");
    printf("{\"test\":\"svc_sfdnsres\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
