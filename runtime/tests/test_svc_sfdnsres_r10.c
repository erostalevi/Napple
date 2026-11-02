/* Unit tests: ipc-depth-95 r10, part net: sfdnsres resolver options read back (14 / 15) and the 23.0.0 Query / Fetch / CancelQuery lookups (100-104),
 * offline (NullProvider: no DNS) and through a provider that resolves. sfdnsres.c is #included. */
#include "../src/services/sfdnsres.c"
#include "svc_test.h"

static int fake_resolve(void *ctx, const char *host, const char *service, int family, OnlineResolved *out) {
    (void)ctx, (void)service, (void)family;
    memset(out, 0, sizeof *out);
    if (strcmp(host, "example.org")) return HZ_EAI_NONAME;
    out->count = 1;
    out->addrs[0].family = 2;
    out->addrs[0].addr[0] = 10, out->addrs[0].addr[3] = 1;
    return 0;
}
static int fake_reverse(void *ctx, const OnlineAddr *a, char *host, size_t n) {
    (void)ctx, (void)a;
    snprintf(host, n, "%s", "host.example");
    return 0;
}
static void set_opt(IpcObject *o, uint32_t key, const void *v, uint32_t n, IpcCall *c) {
    memset(c, 0, sizeof *c);
    c->in = (const uint8_t *)&key;
    c->in_size = 4;
    svc_set_in_buf(c, v, n);
    svc_call_c(o, 14, c);
}
/* GetAddrInfo-style call (cmd) for host / service with an output buffer */
static void addr_call(IpcObject *o, unsigned cmd, const char *host, const char *serv, uint8_t *out, uint32_t outn, IpcCall *c) {
    memset(c, 0, sizeof *c);
    svc_set_in_buf(c, host, strlen(host) + 1);
    svc_set_in_buf(c, serv, strlen(serv) + 1);
    if (out) svc_set_out_buf(c, out, outn);
    svc_call_c(o, cmd, c);
}
static void fetch(IpcObject *o, unsigned cmd, uint32_t id, uint8_t *out, uint32_t outn, uint8_t *out2, uint32_t out2n, IpcCall *c) {
    memset(c, 0, sizeof *c);
    c->in = (const uint8_t *)&id;
    c->in_size = 4;
    if (out) svc_set_out_buf(c, out, outn);
    if (out2) svc_set_out_buf(c, out2, out2n);
    svc_call_c(o, cmd, c);
}

int main(void) {
    IpcObject *o = res_open();
    IpcCall c;
    /* ---- 14 / 15 resolver options */
    uint8_t on = 1, back[RES_OPTION_MAX];
    set_opt(o, 3, &on, 1, &c);
    CHECK(c.result == 0 && c.out_size == 8 && SVC_OUT(&c, int32_t, 0) == 0 && SVC_OUT(&c, int32_t, 4) == 0, "14 ResolverSetOptionRequest: {0, 0}");
    uint32_t key = 3;
    memset(back, 0xcc, sizeof back);
    memset(&c, 0, sizeof c);
    c.in = (const uint8_t *)&key;
    c.in_size = 4;
    svc_set_out_buf(&c, back, sizeof back);
    svc_call_c(o, 15, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 0 && SVC_OUT(&c, uint32_t, 8) == 1 && back[0] == 1 && back[1] == 0xcc, "15 ResolverGetOptionRequest reads the value back (1 byte)");
    uint32_t ttl = 300;
    set_opt(o, 3, &ttl, 4, &c);
    memset(&c, 0, sizeof c);
    c.in = (const uint8_t *)&key;
    c.in_size = 4;
    svc_set_out_buf(&c, back, sizeof back);
    svc_call_c(o, 15, &c);
    uint32_t got;
    memcpy(&got, back, 4);
    CHECK(SVC_OUT(&c, uint32_t, 8) == 4 && got == 300, "...a second Set of the key replaces the value");
    key = 9;
    svc_call(o, 15, &key, 4, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == -1 && SVC_OUT(&c, uint32_t, 4) == HZ_ENOENT && SVC_OUT(&c, uint32_t, 8) == 0, "15 of a key never set: {-1, ENOENT}");
    for (uint32_t k = 100; k < 100 + RES_OPTIONS - 1; k++) set_opt(o, k, &on, 1, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 0, "...16 keys are kept");
    set_opt(o, 999, &on, 1, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == -1 && SVC_OUT(&c, int32_t, 4) == HZ_ENOMEM, "...a 17th key: {-1, ENOMEM}");

    /* ---- Query / Fetch offline: the NullProvider has no DNS */
    addr_call(o, 100, "example.org", "80", NULL, 0, &c);
    uint32_t id = SVC_OUT(&c, uint32_t, 8);
    CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 0 && id != 0, "100 QueryAddrInfoRequestWithOptions: a query id");
    uint8_t out[512], out2[64];
    fetch(o, 102, id, out, sizeof out, NULL, 0, &c);
    CHECK(c.out_size == 12 && SVC_OUT(&c, int32_t, 0) == HZ_EAI_AGAIN && SVC_OUT(&c, uint32_t, 8) == 0, "102 Fetch offline: EAI_AGAIN, as GetAddrInfo answers");
    fetch(o, 102, id, out, sizeof out, NULL, 0, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 11 && SVC_OUT(&c, uint32_t, 4) == HZ_EINVAL, "...a second Fetch: the query is no longer pending");

    /* ---- through a provider that resolves */
    OnlineProvider p = *online_null_provider();
    p.dns_resolve = fake_resolve;
    p.dns_reverse = fake_reverse;
    p.caps |= ONLINE_CAP_DNS;
    online_provider_install(&p);
    uint8_t sync[512];
    memset(sync, 0, sizeof sync);
    addr_call(o, 12, "example.org", "80", sync, sizeof sync, &c);
    uint32_t sync_n = SVC_OUT(&c, uint32_t, 8);
    CHECK(SVC_OUT(&c, int32_t, 0) == 0 && sync_n > 0, "12 GetAddrInfoRequestWithOptions resolves (%u bytes)", sync_n);
    addr_call(o, 100, "example.org", "80", NULL, 0, &c);
    id = SVC_OUT(&c, uint32_t, 8);
    uint32_t id_bad;
    addr_call(o, 100, "nowhere.invalid", "", NULL, 0, &c);
    id_bad = SVC_OUT(&c, uint32_t, 8);
    CHECK(id && id_bad && id_bad != id, "100 two queries, two ids");
    fetch(o, 103, id, out, sizeof out, out2, sizeof out2, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 11, "103 FetchNameInfo of an addrinfo query: not pending (wrong kind)");
    memset(out, 0, sizeof out);
    fetch(o, 102, id, out, sizeof out, NULL, 0, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 0 && SVC_OUT(&c, uint32_t, 8) == sync_n && !memcmp(out, sync, sync_n), "102 Fetch writes what the synchronous call writes");
    fetch(o, 102, id_bad, out, sizeof out, NULL, 0, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == HZ_EAI_NONAME, "...an unknown host: EAI_NONAME");
    addr_call(o, 100, "example.org", "80", NULL, 0, &c);
    id = SVC_OUT(&c, uint32_t, 8);
    fetch(o, 102, id, out, 8, NULL, 0, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 14, "...into a buffer too small: EAI_OVERFLOW");
    /* name info */
    uint8_t sa[16] = {16, 2, 0, 80, 10, 0, 0, 1};
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, sa, sizeof sa);
    svc_call_c(o, 101, &c);
    id = SVC_OUT(&c, uint32_t, 8);
    CHECK(SVC_OUT(&c, int32_t, 0) == 0 && id, "101 QueryNameInfoRequestWithOptions: a query id");
    memset(out, 0, sizeof out), memset(out2, 0, sizeof out2);
    fetch(o, 103, id, out, sizeof out, out2, sizeof out2, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 0 && !strcmp((char *)out, "host.example") && !strcmp((char *)out2, "80"), "103 FetchNameInfo: the host and the service");
    /* cancel */
    addr_call(o, 100, "example.org", "80", NULL, 0, &c);
    id = SVC_OUT(&c, uint32_t, 8);
    svc_call(o, 104, &id, 4, &c);
    CHECK(c.result == 0 && c.out_size == 0, "104 CancelQuery");
    fetch(o, 102, id, out, sizeof out, NULL, 0, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 11, "...the cancelled query cannot be fetched");
    uint32_t ids[RES_QUERIES];
    for (unsigned i = 0; i < RES_QUERIES; i++) {
        addr_call(o, 100, "example.org", "80", NULL, 0, &c);
        ids[i] = SVC_OUT(&c, uint32_t, 8);
    }
    addr_call(o, 100, "example.org", "80", NULL, 0, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == -1 && SVC_OUT(&c, int32_t, 4) == 105, "a 17th pending query: {-1, ENOBUFS (105)}");
    for (unsigned i = 0; i < RES_QUERIES; i++) svc_call(o, 104, &ids[i], 4, &c);
    addr_call(o, 100, "example.org", "80", NULL, 0, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 0, "...room again once they are cancelled");
    online_provider_install(NULL);
    const unsigned real[] = {14, 15, 100, 101, 102, 103, 104};
    for (unsigned i = 0; i < sizeof real / sizeof real[0]; i++) CHECK(svc_find(o, real[i])->status == IPC_IMPL, "sfdnsres %u is real", real[i]);
    printf("{\"test\":\"svc_sfdnsres_r10\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
