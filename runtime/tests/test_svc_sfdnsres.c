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
    svc_call(o, 9, &h1, 4, &c); CHECK(c.result == 0 && res_cancelled[h1 % RES_HANDLES] == h1, "CancelRequest records the handle and succeeds");
    uint32_t zero = 0; svc_call(o, 9, &zero, 4, &c); CHECK(c.result == 0, "CancelRequest of an unknown handle is harmless");
    uint8_t addrs[8] = {8, 8, 8, 8, 1, 1, 1, 1}, back[16] = {0};
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, addrs, 8); svc_call_c(o, 0, &c); CHECK(c.result == 0 && res_dns_len == 8, "SetDnsAddressesPrivateRequest stores the list");
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, back, sizeof back); svc_call_c(o, 1, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 8) == 8 && !memcmp(back, addrs, 8), "GetDnsAddressPrivateRequest reads it back");
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, back, 4); svc_call_c(o, 1, &c); CHECK(SVC_OUT(&c, uint32_t, 8) == 4, "read-back is cut to the buffer");
    printf("{\"test\":\"svc_sfdnsres\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
