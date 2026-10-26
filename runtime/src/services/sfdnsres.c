/* sfdnsres (nn::socket::resolver): DNS, served by the active OnlineProvider (online_provider.h). With the NullProvider there is no network, so every
 * lookup fails the way a resolver with no reachable server does (netdb TRY_AGAIN / getaddrinfo EAI_AGAIN, size 0, no data written); the
 * error-string calls are real. A provider's answers are serialised in the client library's format (sfdnsres_core.c). Output tuples follow the public
 * description as remembered by the author: GetHostByName -> {u32 netdb_error, u32 errno, u32 size}; GetAddrInfo -> {s32 ret, u32 errno,
 * u32 size}. Unverified on a console. */
#include "svc_common.h"
#include "sfdnsres_core.h"
#include "hz_result.h"
#include "bsd_core.h"
#include <stdio.h>

static void out_string(IpcCall *c, const char *s) {
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || !b->size) return;
    char tmp[128];
    size_t n = strlen(s);
    if (n > sizeof tmp - 1) n = sizeof tmp - 1;
    if (n > b->size - 1) n = (size_t)b->size - 1; /* always leave room for the NUL */
    memcpy(tmp, s, n);
    tmp[n] = 0;
    ipc_write_buf(c, b, tmp, n + 1);
}
static void res_hstrerror(IpcObject *o, IpcCall *c) {
    (void)o;
    out_string(c, sfdns_hstrerror(IPC_IN(c, uint32_t, 0)));
}
static void res_gai_strerror(IpcObject *o, IpcCall *c) {
    (void)o;
    out_string(c, sfdns_gai_strerror(IPC_IN(c, uint32_t, 0)));
}
/* The request strings and addresses arrive in input buffers (A before X); a call without them is treated as an empty name. */
static const char *in_string(const IpcCall *c, unsigned n, char *tmp, size_t cap) {
    const IpcBuf *b = ipc_in_buf(c, n);
    const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    if (!p) return "";
    size_t l = (size_t)b->size < cap - 1 ? (size_t)b->size : cap - 1;
    memcpy(tmp, p, l);
    tmp[l] = 0;
    return tmp;
}
static void out3(IpcCall *c, uint32_t a, uint32_t b, uint32_t sz) {
    uint32_t v[3] = {a, b, sz};
    ipc_out(c, v, 12);
}
/* A hostname or address lookup that the provider could not answer: {netdb error, 0, 0} and nothing written. */
static void res_host_by_name(IpcObject *o, IpcCall *c) {
    (void)o;
    char host[256];
    const char *name = in_string(c, 0, host, sizeof host);
    const OnlineProvider *p = online_provider();
    int (*resolve)(void *, const char *, const char *, int, OnlineResolved *) = ONLINE_FN(p, dns_resolve);
    OnlineResolved r;
    memset(&r, 0, sizeof r);
    int e = resolve ? resolve(p->ctx, name, NULL, 2, &r) : HZ_EAI_AGAIN;
    if (!resolve || !*name) e = e ? e : HZ_EAI_NONAME;
    if (e || !r.count) {
        out3(c, e ? sfdns_eai_to_netdb(e) : HZ_NETDB_NO_DATA, 0, 0);
        return;
    }
    const IpcBuf *ob = ipc_out_buf(c, 0);
    uint8_t tmp[512];
    size_t n = sfdns_serialize_hostent(r.canonname[0] ? r.canonname : name, &r, tmp, sizeof tmp);
    if (!ob || !n || n > ob->size) {
        out3(c, HZ_NETDB_NO_RECOVERY, HZ_ENOMEM, 0);
        return;
    }
    ipc_write_buf(c, ob, tmp, n);
    out3(c, 0, 0, (uint32_t)n);
}
static void res_host_by_addr(IpcObject *o, IpcCall *c) {
    (void)o;
    const IpcBuf *ib = ipc_in_buf(c, 0);
    OnlineAddr a;
    const uint8_t *raw = ib && ib->size ? ipc_buf_ptr(ib) : NULL;
    const OnlineProvider *p = online_provider();
    int (*rev)(void *, const OnlineAddr *, char *, size_t) = ONLINE_FN(p, dns_reverse);
    if (!rev || !raw) {
        out3(c, rev ? HZ_NETDB_NO_RECOVERY : HZ_NETDB_TRY_AGAIN, 0, 0);
        return;
    }
    memset(&a, 0, sizeof a);
    a.family = c->in_size >= 8 ? (uint8_t)IPC_IN(c, uint32_t, 4) : 2; /* {u32 len, u32 af} */
    memcpy(a.addr, raw, ib->size < 16 ? ib->size : 16);
    char host[256];
    int e = rev(p->ctx, &a, host, sizeof host);
    const IpcBuf *ob = ipc_out_buf(c, 0);
    OnlineResolved r;
    memset(&r, 0, sizeof r);
    r.count = 1;
    r.addrs[0] = a;
    uint8_t tmp[512];
    size_t n = e ? 0 : sfdns_serialize_hostent(host, &r, tmp, sizeof tmp);
    if (e || !n || !ob || n > ob->size) {
        out3(c, e ? sfdns_eai_to_netdb(e) : HZ_NETDB_NO_RECOVERY, 0, 0);
        return;
    }
    ipc_write_buf(c, ob, tmp, n);
    out3(c, 0, 0, (uint32_t)n);
}
static void res_addr_info(IpcObject *o, IpcCall *c) {
    (void)o;
    char host[256], serv[64];
    const char *h = in_string(c, 0, host, sizeof host), *sv = in_string(c, 1, serv, sizeof serv);
    int flags = 0, family = 0, st = 0, pr = 0;
    const IpcBuf *hb = ipc_in_buf(c, 2);
    if (hb && hb->size) sfdns_parse_hints(ipc_buf_ptr(hb), (size_t)hb->size, &flags, &family, &st, &pr);
    const OnlineProvider *p = online_provider();
    int (*resolve)(void *, const char *, const char *, int, OnlineResolved *) = ONLINE_FN(p, dns_resolve);
    OnlineResolved r;
    memset(&r, 0, sizeof r);
    int e = resolve ? resolve(p->ctx, h, sv, family, &r) : HZ_EAI_AGAIN;
    if (!e && !r.count) e = HZ_EAI_NONAME;
    if (e) {
        out3(c, (uint32_t)e, 0, 0);
        return;
    }
    uint16_t port = sfdns_port_from_service(sv);
    for (uint32_t i = 0; i < r.count && i < ONLINE_MAX_ADDRS; i++)
        if (!r.addrs[i].port) r.addrs[i].port = port;
    const IpcBuf *ob = ipc_out_buf(c, 0);
    uint8_t tmp[2048];
    size_t n = sfdns_serialize_addrinfo(&r, flags, st, pr, tmp, sizeof tmp);
    if (!ob || !n || n > ob->size) {
        out3(c, 14 /* EAI_OVERFLOW */, 0, 0);
        return;
    }
    ipc_write_buf(c, ob, tmp, n);
    out3(c, 0, 0, (uint32_t)n);
}
/* GetNameInfo: the host part comes from the provider's reverse lookup, the service part is the port as a number. */
static void res_name_info(IpcObject *o, IpcCall *c) {
    (void)o;
    const IpcBuf *ib = ipc_in_buf(c, 0);
    const uint8_t *raw = ib && ib->size ? ipc_buf_ptr(ib) : NULL;
    const OnlineProvider *p = online_provider();
    int (*rev)(void *, const OnlineAddr *, char *, size_t) = ONLINE_FN(p, dns_reverse);
    OnlineAddr a;
    if (!rev || !raw || bsd_sockaddr_decode(raw, (size_t)ib->size, &a)) {
        out3(c, rev ? (uint32_t)HZ_EAI_FAIL : (uint32_t)HZ_EAI_AGAIN, 0, 0);
        return;
    }
    char host[256], svc[16];
    int e = rev(p->ctx, &a, host, sizeof host);
    if (e) {
        out3(c, (uint32_t)e, 0, 0);
        return;
    }
    snprintf(svc, sizeof svc, "%u", a.port);
    const IpcBuf *hb = ipc_out_buf(c, 0), *sb = ipc_out_buf(c, 1);
    if (hb) ipc_write_buf(c, hb, host, strlen(host) + 1);
    if (sb) ipc_write_buf(c, sb, svc, strlen(svc) + 1);
    out3(c, 0, 0, 0);
}
static void res_cancel_handle(IpcObject *o, IpcCall *c) {
    (void)o;
    static uint32_t next = 1;
    uint32_t h = next++;
    IPC_OUT(c, h);
}
static const IpcCmd res_cmds[] = {
    {0, "SetDnsAddressesPrivateRequest", svc_noop, IPC_STUB},
    {1, "GetDnsAddressPrivateRequest", svc_noop, IPC_STUB},
    {2, "GetHostByNameRequest", res_host_by_name, IPC_IMPL},
    {3, "GetHostByAddrRequest", res_host_by_addr, IPC_IMPL},
    {4, "GetHostStringErrorRequest", res_hstrerror, IPC_IMPL},
    {5, "GetGaiStringErrorRequest", res_gai_strerror, IPC_IMPL},
    {6, "GetAddrInfoRequest", res_addr_info, IPC_IMPL},
    {7, "GetNameInfoRequest", res_name_info, IPC_IMPL},
    {8, "RequestCancelHandleRequest", res_cancel_handle, IPC_STUB},
    {9, "CancelRequest", svc_noop, IPC_STUB},
    {10, "GetHostByNameRequestWithOptions", res_host_by_name, IPC_IMPL},
    {11, "GetHostByAddrRequestWithOptions", res_host_by_addr, IPC_IMPL},
    {12, "GetAddrInfoRequestWithOptions", res_addr_info, IPC_IMPL},
    {13, "GetNameInfoRequestWithOptions", res_name_info, IPC_IMPL},
};
static IpcObject *res_open(void) { return IPC_OBJECT("sfdnsres", res_cmds, NULL, NULL); }
void service_register_sfdnsres(void) { ipc_register_service("sfdnsres", res_open); }
