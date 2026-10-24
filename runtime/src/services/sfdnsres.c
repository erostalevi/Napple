/* sfdnsres (nn::socket::resolver): DNS. There is no network, so every lookup fails the way a resolver with no reachable server does
 * (netdb TRY_AGAIN / getaddrinfo EAI_AGAIN, size 0, no data written); the error-string calls are real. Output tuples follow the public
 * description as remembered by the author: GetHostByName -> {u32 netdb_error, u32 errno, u32 size}; GetAddrInfo -> {s32 ret, u32 errno,
 * u32 size}. Unverified on a console. */
#include "svc_common.h"
#include "sfdnsres_core.h"
#include "hz_result.h"

static void out_string(IpcCall *c, const char *s) {
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || !b->size) return;
    char tmp[128]; size_t n = strlen(s); if (n > sizeof tmp - 1) n = sizeof tmp - 1;
    if (n > b->size - 1) n = (size_t)b->size - 1;                    /* always leave room for the NUL */
    memcpy(tmp, s, n); tmp[n] = 0; ipc_write_buf(c, b, tmp, n + 1);
}
static void res_hstrerror(IpcObject *o, IpcCall *c) { (void)o; out_string(c, sfdns_hstrerror(IPC_IN(c, uint32_t, 0))); }
static void res_gai_strerror(IpcObject *o, IpcCall *c) { (void)o; out_string(c, sfdns_gai_strerror(IPC_IN(c, uint32_t, 0))); }
static void res_host_failed(IpcObject *o, IpcCall *c) { (void)o; uint32_t v[3] = {HZ_NETDB_TRY_AGAIN, 0, 0}; ipc_out(c, v, 12); }
static void res_gai_failed(IpcObject *o, IpcCall *c) { (void)o; int32_t v[3] = {HZ_EAI_AGAIN, 0, 0}; ipc_out(c, v, 12); }
static void res_cancel_handle(IpcObject *o, IpcCall *c) { (void)o; static uint32_t next = 1; uint32_t h = next++; IPC_OUT(c, h); }
static const IpcCmd res_cmds[] = {
    {0, "SetDnsAddressesPrivateRequest", svc_noop, IPC_STUB}, {1, "GetDnsAddressPrivateRequest", svc_noop, IPC_STUB},
    {2, "GetHostByNameRequest", res_host_failed, IPC_STUB}, {3, "GetHostByAddrRequest", res_host_failed, IPC_STUB},
    {4, "GetHostStringErrorRequest", res_hstrerror, IPC_IMPL}, {5, "GetGaiStringErrorRequest", res_gai_strerror, IPC_IMPL},
    {6, "GetAddrInfoRequest", res_gai_failed, IPC_STUB}, {7, "GetNameInfoRequest", res_gai_failed, IPC_STUB},
    {8, "RequestCancelHandleRequest", res_cancel_handle, IPC_STUB}, {9, "CancelRequest", svc_noop, IPC_STUB},
    {10, "GetHostByNameRequestWithOptions", res_host_failed, IPC_STUB}, {11, "GetHostByAddrRequestWithOptions", res_host_failed, IPC_STUB},
    {12, "GetAddrInfoRequestWithOptions", res_gai_failed, IPC_STUB}, {13, "GetNameInfoRequestWithOptions", res_gai_failed, IPC_STUB},
};
static IpcObject *res_open(void) { return IPC_OBJECT("sfdnsres", res_cmds, NULL, NULL); }
void service_register_sfdnsres(void) { ipc_register_service("sfdnsres", res_open); }
