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
#include <pthread.h>

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
/* The answer of a GetAddrInfo / GetNameInfo lookup, computed from the request and written to the reply separately, so a Query (100 / 101) can keep it until
 * its Fetch (102 / 103). */
enum { RES_ADDR_INFO = 1, RES_NAME_INFO = 2 };
typedef struct {
    uint32_t id, kind, ret, size;
    uint8_t data[2048]; /* serialised addrinfo list (size bytes; 0 = it did not fit) */
    char host[256], serv[16];
} ResAnswer;
static void res_addr_answer(const IpcCall *c, ResAnswer *a) {
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
    a->kind = RES_ADDR_INFO, a->ret = (uint32_t)e, a->size = 0;
    if (e) return;
    uint16_t port = sfdns_port_from_service(sv);
    for (uint32_t i = 0; i < r.count && i < ONLINE_MAX_ADDRS; i++)
        if (!r.addrs[i].port) r.addrs[i].port = port;
    a->size = (uint32_t)sfdns_serialize_addrinfo(&r, flags, st, pr, a->data, sizeof a->data);
}
static void res_addr_reply(IpcCall *c, const ResAnswer *a) {
    if (a->ret) {
        out3(c, a->ret, 0, 0);
        return;
    }
    const IpcBuf *ob = ipc_out_buf(c, 0);
    if (!ob || !a->size || a->size > ob->size) {
        out3(c, 14 /* EAI_OVERFLOW */, 0, 0);
        return;
    }
    ipc_write_buf(c, ob, a->data, a->size);
    out3(c, 0, 0, a->size);
}
static void res_addr_info(IpcObject *o, IpcCall *c) {
    (void)o;
    static __thread ResAnswer a;
    res_addr_answer(c, &a);
    res_addr_reply(c, &a);
}
/* GetNameInfo: the host part comes from the provider's reverse lookup, the service part is the port as a number. */
static void res_name_answer(const IpcCall *c, ResAnswer *ans) {
    const IpcBuf *ib = ipc_in_buf(c, 0);
    const uint8_t *raw = ib && ib->size ? ipc_buf_ptr(ib) : NULL;
    const OnlineProvider *p = online_provider();
    int (*rev)(void *, const OnlineAddr *, char *, size_t) = ONLINE_FN(p, dns_reverse);
    OnlineAddr a;
    ans->kind = RES_NAME_INFO, ans->size = 0, ans->host[0] = ans->serv[0] = 0;
    if (!rev || !raw || bsd_sockaddr_decode(raw, (size_t)ib->size, &a)) {
        ans->ret = rev ? (uint32_t)HZ_EAI_FAIL : (uint32_t)HZ_EAI_AGAIN;
        return;
    }
    ans->ret = (uint32_t)rev(p->ctx, &a, ans->host, sizeof ans->host);
    snprintf(ans->serv, sizeof ans->serv, "%u", a.port);
}
static void res_name_reply(IpcCall *c, const ResAnswer *a) {
    if (a->ret) {
        out3(c, a->ret, 0, 0);
        return;
    }
    const IpcBuf *hb = ipc_out_buf(c, 0), *sb = ipc_out_buf(c, 1);
    if (hb) ipc_write_buf(c, hb, a->host, strlen(a->host) + 1);
    if (sb) ipc_write_buf(c, sb, a->serv, strlen(a->serv) + 1);
    out3(c, 0, 0, 0);
}
static void res_name_info(IpcObject *o, IpcCall *c) {
    (void)o;
    static __thread ResAnswer a;
    res_name_answer(c, &a);
    res_name_reply(c, &a);
}
/* RequestCancelHandle hands out a request handle and CancelRequest(handle) marks it. Lookups finish synchronously inside the call that made them, so a
 * cancel can only arrive for a request that has already completed: it succeeds and changes nothing, exactly as on a console when the lookup won the race. */
#define RES_HANDLES 64
static uint32_t res_next_handle = 1, res_cancelled[RES_HANDLES];
static void res_cancel_handle(IpcObject *o, IpcCall *c) {
    (void)o;
    uint32_t h = res_next_handle++;
    IPC_OUT(c, h);
}
static void res_cancel(IpcObject *o, IpcCall *c) {
    (void)o;
    uint32_t h = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0;
    if (h) res_cancelled[h % RES_HANDLES] = h;
}
/* Private (per-application) DNS server addresses, set by SetDnsAddressesPrivateRequest and read back by GetDnsAddressPrivateRequest. The runtime resolves
 * through the OnlineProvider, which owns server choice, so the stored list is only a record the title can read back. */
#define RES_DNS_MAX 0x80
static uint8_t res_dns[RES_DNS_MAX];
static uint32_t res_dns_len;
static void res_set_dns(IpcObject *o, IpcCall *c) {
    (void)o;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    uint32_t n = p ? (uint32_t)(b->size > RES_DNS_MAX ? RES_DNS_MAX : b->size) : 0;
    if (n) memcpy(res_dns, p, n);
    res_dns_len = n;
    out3(c, 0, 0, 0);
}
static void res_get_dns(IpcObject *o, IpcCall *c) {
    (void)o;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint32_t n = res_dns_len;
    if (b && n > b->size) n = (uint32_t)b->size;
    if (b && n) ipc_write_buf(c, b, res_dns, n);
    out3(c, 0, 0, n);
}
/* 14 ResolverSetOptionRequest(u32 key, buffer value) -> {s32 ret, u32 errno} / 15 ResolverGetOptionRequest(u32 key) -> buffer value, {s32 ret, u32 errno,
 * u32 size}: the resolver options (nn::socket::ResolverOption) the title set, kept per key and read back as set. Switchbrew names the commands only: the
 * argument layout and the answer for a key never set ({-1, ENOENT} here) are [unverified]. Up to RES_OPTIONS keys of up to 0x100 bytes; a further key is
 * {-1, ENOMEM}. The provider resolves with its own settings, so an option changes nothing but what reads back. */
#define RES_OPTIONS 16
#define RES_OPTION_MAX 0x100
static struct {
    uint32_t used, key, size;
    uint8_t v[RES_OPTION_MAX];
} res_opt[RES_OPTIONS];
static pthread_mutex_t res_lock = PTHREAD_MUTEX_INITIALIZER; /* options and pending queries */
static void res_set_option(IpcObject *o, IpcCall *c) {
    (void)o;
    uint32_t key = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b && b->size ? ipc_buf_ptr(b) : NULL;
    uint32_t n = p ? (uint32_t)(b->size > RES_OPTION_MAX ? RES_OPTION_MAX : b->size) : 0;
    int slot = -1;
    pthread_mutex_lock(&res_lock);
    for (int i = 0; i < RES_OPTIONS && slot < 0; i++)
        if (res_opt[i].used && res_opt[i].key == key) slot = i;
    for (int i = 0; i < RES_OPTIONS && slot < 0; i++)
        if (!res_opt[i].used) slot = i;
    if (slot >= 0) {
        res_opt[slot].used = 1, res_opt[slot].key = key, res_opt[slot].size = n;
        if (n) memcpy(res_opt[slot].v, p, n);
    }
    pthread_mutex_unlock(&res_lock);
    int32_t v[2] = {slot < 0 ? -1 : 0, slot < 0 ? bsd_errno_to_guest(HZ_ENOMEM) : 0};
    ipc_out(c, v, 8);
}
static void res_get_option(IpcObject *o, IpcCall *c) {
    (void)o;
    uint32_t key = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0, n = 0;
    uint8_t v[RES_OPTION_MAX];
    int found = 0;
    pthread_mutex_lock(&res_lock);
    for (int i = 0; i < RES_OPTIONS && !found; i++)
        if (res_opt[i].used && res_opt[i].key == key) found = 1, n = res_opt[i].size, memcpy(v, res_opt[i].v, n);
    pthread_mutex_unlock(&res_lock);
    if (!found) {
        out3(c, (uint32_t)-1, HZ_ENOENT, 0);
        return;
    }
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b && n > b->size) n = (uint32_t)b->size;
    if (b && n) ipc_write_buf(c, b, v, n);
    out3(c, 0, 0, n);
}
/* 100 QueryAddrInfoRequestWithOptions / 101 QueryNameInfoRequestWithOptions [23.0.0+]: the lookup of GetAddrInfoRequestWithOptions / GetNameInfoRequest-
 * WithOptions (same buffers), started on its own and fetched later: here it runs inside the Query (through the provider) and its answer waits under a
 * query id, {s32 0, u32 0, u32 id}. 102 / 103 Fetch*(u32 id) write that answer exactly as the synchronous call would and forget the query; 104
 * CancelQuery(u32 id) forgets it unanswered. A Fetch of an id that is not pending (never made, cancelled, fetched already, or of the other kind) is
 * {EAI_SYSTEM 11, EINVAL, 0}. Up to RES_QUERIES pending; a further Query is {-1, ENOBUFS, 0}. Switchbrew names these commands only: every layout here
 * is [unverified]. */
#define RES_QUERIES 16
static ResAnswer res_q[RES_QUERIES]; /* id 0 = free */
static uint32_t res_next_query = 1;
static void res_query(IpcCall *c, int kind) {
    static __thread ResAnswer a;
    if (kind == RES_ADDR_INFO) res_addr_answer(c, &a);
    else res_name_answer(c, &a);
    int slot = -1;
    pthread_mutex_lock(&res_lock);
    for (int i = 0; i < RES_QUERIES && slot < 0; i++)
        if (!res_q[i].id) slot = i;
    if (slot >= 0) {
        if (!res_next_query) res_next_query = 1; /* 0 marks a free slot */
        a.id = res_next_query++;
        res_q[slot] = a;
    }
    pthread_mutex_unlock(&res_lock);
    if (slot < 0) out3(c, (uint32_t)-1, (uint32_t)bsd_errno_to_guest(HZ_ENOBUFS), 0);
    else out3(c, 0, 0, a.id);
}
static void res_query_addr(IpcObject *o, IpcCall *c) {
    (void)o;
    res_query(c, RES_ADDR_INFO);
}
static void res_query_name(IpcObject *o, IpcCall *c) {
    (void)o;
    res_query(c, RES_NAME_INFO);
}
/* Takes the pending query `id` of `kind` out of the table into *a; 0 when there is none. */
static int res_take(uint32_t id, uint32_t kind, ResAnswer *a) {
    int found = 0;
    pthread_mutex_lock(&res_lock);
    for (int i = 0; i < RES_QUERIES && !found && id; i++)
        if (res_q[i].id == id && (!kind || res_q[i].kind == kind)) {
            if (a) *a = res_q[i];
            res_q[i].id = 0, found = 1;
        }
    pthread_mutex_unlock(&res_lock);
    return found;
}
static void res_fetch(IpcCall *c, uint32_t kind) {
    static __thread ResAnswer a;
    uint32_t id = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0;
    if (!res_take(id, kind, &a)) {
        out3(c, 11 /* EAI_SYSTEM */, HZ_EINVAL, 0);
        return;
    }
    if (kind == RES_ADDR_INFO) res_addr_reply(c, &a);
    else res_name_reply(c, &a);
}
static void res_fetch_addr(IpcObject *o, IpcCall *c) {
    (void)o;
    res_fetch(c, RES_ADDR_INFO);
}
static void res_fetch_name(IpcObject *o, IpcCall *c) {
    (void)o;
    res_fetch(c, RES_NAME_INFO);
}
static void res_cancel_query(IpcObject *o, IpcCall *c) {
    (void)o;
    res_take(c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0, 0, NULL);
}
static const IpcCmd res_cmds[] = {
    {0, "SetDnsAddressesPrivateRequest", res_set_dns, IPC_IMPL},
    {1, "GetDnsAddressPrivateRequest", res_get_dns, IPC_IMPL},
    {2, "GetHostByNameRequest", res_host_by_name, IPC_IMPL},
    {3, "GetHostByAddrRequest", res_host_by_addr, IPC_IMPL},
    {4, "GetHostStringErrorRequest", res_hstrerror, IPC_IMPL},
    {5, "GetGaiStringErrorRequest", res_gai_strerror, IPC_IMPL},
    {6, "GetAddrInfoRequest", res_addr_info, IPC_IMPL},
    {7, "GetNameInfoRequest", res_name_info, IPC_IMPL},
    {8, "GetCancelHandleRequest", res_cancel_handle, IPC_IMPL},
    {9, "CancelRequest", res_cancel, IPC_IMPL},
    {10, "GetHostByNameRequestWithOptions", res_host_by_name, IPC_IMPL},
    {11, "GetHostByAddrRequestWithOptions", res_host_by_addr, IPC_IMPL},
    {12, "GetAddrInfoRequestWithOptions", res_addr_info, IPC_IMPL},
    {13, "GetNameInfoRequestWithOptions", res_name_info, IPC_IMPL},
    {14, "ResolverSetOptionRequest", res_set_option, IPC_IMPL},
    {15, "ResolverGetOptionRequest", res_get_option, IPC_IMPL},
    {100, "QueryAddrInfoRequestWithOptions", res_query_addr, IPC_IMPL},
    {101, "QueryNameInfoRequestWithOptions", res_query_name, IPC_IMPL},
    {102, "FetchAddrInfoRequestWithOptions", res_fetch_addr, IPC_IMPL},
    {103, "FetchNameInfoRequestWithOptions", res_fetch_name, IPC_IMPL},
    {104, "CancelQuery", res_cancel_query, IPC_IMPL},
};
static IpcObject *res_open(void) { return IPC_OBJECT("sfdnsres", res_cmds, NULL, NULL); }
void service_register_sfdnsres(void) { ipc_register_service("sfdnsres", res_open); }
