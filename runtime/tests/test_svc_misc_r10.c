/* Unit tests (ipc-depth-95 r10, applet part, the small services): btm IBtmUserCore::GetBleScanFilterParameter (the documented constants), lm
 * ILogger::OpenSharedMemoryAccessorForHashedLog (the retail accessor object), ovln ISender::GetUnreceivedMessageCount (per-sender queue with its length and
 * overflow options) and aoc:u CreateContentsServiceManager (the manager, its token request offline / online without a linked account, through a test
 * OnlineProvider). btm.c, lm.c, ovln.c and misc.c are #included. */
#include "../src/services/btm.c"
#include "../src/services/lm.c"
#include "../src/services/ovln.c"
#include "../src/services/misc.c"
#include "svc_test.h"
#ifdef __linux__
const uint8_t g_user_id[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
#endif

static int g_connected, g_linked;
static int fake_net_info(void *ctx, OnlineNetInfo *out) { (void)ctx; memset(out, 0, sizeof *out); out->connected = (uint8_t)g_connected; return 0; }
static int fake_account(void *ctx, const uint8_t uid[16], OnlineAccountInfo *out) { (void)ctx; (void)uid; memset(out, 0, sizeof *out); out->linked = (uint8_t)g_linked; return 0; }

static uint32_t send_msg(IpcObject *snd, uint32_t tag, uint8_t pos, uint8_t overflow) {
    uint8_t m[0x88]; memset(m, 0, sizeof m); memcpy(m, &tag, 4); m[0x80] = pos; m[0x81] = overflow;
    IpcCall c; svc_call(snd, 0, m, sizeof m, &c); return c.result;
}
static uint32_t unreceived(IpcObject *snd) { IpcCall c; svc_call(snd, 1, NULL, 0, &c); return SVC_OUT(&c, uint32_t, 0); }
static IpcObject *open_sender(IpcObject *svc, const char *name, uint32_t len) {
    uint8_t in[0x18]; memset(in, 0, sizeof in); strncpy((char *)in, name, 16); memcpy(in + 0x10, &len, 4);
    IpcCall c; svc_call(svc, 0, in, sizeof in, &c); return c.nout_objects ? c.out_objects[0] : NULL;
}
static uint32_t token_request(IpcObject *mgr, unsigned cmd, int *signalled) {
    uint8_t in[0x50] = {0}; IpcCall c; svc_call(mgr, cmd, in, sizeof in, &c);
    *signalled = c.nout_copy == 1 && event_is_signaled(c.out_copy[0]);
    if (c.result || !c.nout_objects) return 0xdead;
    IpcObject *async = c.out_objects[0];
    IpcCall g; svc_call(async, 1, NULL, 0, &g); uint32_t r = g.result;
    svc_call(async, 0, NULL, 0, &g); CHECK(g.result == r, "GetSize reports the same result (%#x)", g.result);
    svc_call(async, 2, NULL, 0, &g); CHECK(g.result == 0, "Cancel accepted");
    return r;
}

int main(void) {
    IpcCall c;
    /* ---- btm: GetBleScanFilterParameter answers the system's constants for ids 1 and 0xFFFF ---- */
    IpcObject *btm = btm_open(); svc_call(btm, 0, NULL, 0, &c); IpcObject *core = c.out_objects[0];
    uint16_t id = 1; svc_call(core, 1, &id, 2, &c);
    static const uint8_t want1[8] = {0x53, 0x05, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00}, wantf[8] = {0x53, 0x05, 0xAD, 0xDE, 0x00, 0xEF, 0xBE, 0x00}, zero[8] = {0};
    CHECK(c.result == 0 && c.out_size == 8 && !memcmp(c.out, want1, 8), "id 1 (Palma): company 0x0553, pattern 00 01 00 00 01 00");
    id = 0xFFFF; svc_call(core, 1, &id, 2, &c); CHECK(c.out_size == 8 && !memcmp(c.out, wantf, 8), "id 0xFFFF: company 0x0553, pattern AD DE 00 EF BE 00");
    id = 7; svc_call(core, 1, &id, 2, &c); CHECK(c.result == 0 && c.out_size == 8 && !memcmp(c.out, zero, 8), "another id: an all-zero parameter");
    CHECK(svc_find(core, 1)->status == IPC_IMPL && svc_find(core, 2)->status == IPC_STUB, "1 real, 2 (GattAttributeUuid) a documented default");

    /* ---- lm: the retail accessor object, whose commands succeed and do nothing ---- */
    IpcObject *lm = lm_open(); svc_call(lm, 0, NULL, 0, &c); IpcObject *lg = c.out_objects[0];
    svc_call(lg, 11, NULL, 0, &c);
    CHECK(c.result == 0 && c.nout_objects == 1 && !strcmp(c.out_objects[0]->iface, "ISharedMemoryAccessor"), "OpenSharedMemoryAccessorForHashedLog -> ISharedMemoryAccessor");
    if (c.nout_objects) for (unsigned k = 0; k < 4; k++) { IpcCall a; svc_call(c.out_objects[0], k, NULL, 0, &a); CHECK(a.result == 0 && a.out_size == 0 && a.nout_copy == 0, "accessor %u: success, nothing", k); }

    /* ---- ovln: each sender keeps its queue; no overlay receives on this host ---- */
    IpcObject *ov = ovln_open();
    IpcObject *s1 = open_sender(ov, "test.a", 3), *s2 = open_sender(ov, "test.b", 0);
    CHECK(s1 && s2, "OpenSender with a source name and queue attribute");
    CHECK(unreceived(s1) == 0, "a new sender has nothing unreceived");
    CHECK(send_msg(s1, 0x11, OVLN_POS_BACK, OVLN_OVERFLOW_ERROR) == 0 && send_msg(s1, 0x22, OVLN_POS_BACK, OVLN_OVERFLOW_ERROR) == 0 && unreceived(s1) == 2, "two sent, two unreceived");
    CHECK(unreceived(s2) == 0, "the count is per sender");
    CHECK(send_msg(s1, 0x00, OVLN_POS_FRONT, OVLN_OVERFLOW_ERROR) == 0 && unreceived(s1) == 3 && ((OvlnSender *)s1->state)->tags[0] == 0x00, "EnqueuePosition Front puts it first");
    CHECK(send_msg(s1, 0x33, OVLN_POS_BACK, OVLN_OVERFLOW_ERROR) == HZ_R_OVLN_QUEUE_FULL && unreceived(s1) == 3, "full queue (QueueLength 3), OverflowOption Error: refused");
    CHECK(send_msg(s1, 0x33, OVLN_POS_BACK, OVLN_OVERFLOW_BLOCK) == HZ_R_OVLN_QUEUE_FULL, "Block fails the same way (no receiver would ever make room)");
    OvlnSender *q1 = s1->state;
    CHECK(send_msg(s1, 0x44, OVLN_POS_BACK, OVLN_OVERFLOW_REMOVE_FRONT) == 0 && unreceived(s1) == 3 && q1->tags[0] == 0x11 && q1->tags[2] == 0x44, "RemoveFront drops the oldest");
    CHECK(send_msg(s1, 0x55, OVLN_POS_BACK, OVLN_OVERFLOW_REMOVE_BACK) == 0 && unreceived(s1) == 3 && q1->tags[1] == 0x22 && q1->tags[2] == 0x55, "RemoveBack replaces the newest");
    CHECK(((OvlnSender *)s2->state)->queue_length == OVLN_DEFAULT_QUEUE, "QueueLength 0: the default length");

    /* ---- aoc:u 110: the contents service manager and its token request ---- */
    IpcObject *aoc = aoc_open(); svc_call(aoc, 110, NULL, 0, &c);
    CHECK(c.result == 0 && c.nout_objects == 1 && !strcmp(c.out_objects[0]->iface, "IContentsServiceManager"), "CreateContentsServiceManager -> IContentsServiceManager");
    IpcObject *mgr = c.out_objects[0]; int sig = 0;
    CHECK(token_request(mgr, 1, &sig) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED && sig, "offline (NullProvider): the request completes with no network");
    CHECK(token_request(mgr, 0, &sig) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED && sig, "the deprecated form answers the same");
    static OnlineProvider fake; memset(&fake, 0, sizeof fake);
    fake.abi_version = ONLINE_ABI_VERSION; fake.struct_size = sizeof fake; fake.name = "test"; fake.caps = ONLINE_CAP_NETWORK | ONLINE_CAP_ACCOUNT;
    fake.net_info = fake_net_info; fake.account_info = fake_account;
    online_provider_install(&fake);
    g_connected = 0; CHECK(token_request(mgr, 1, &sig) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "a provider that is not connected: no network");
    g_connected = 1; g_linked = 0; CHECK(token_request(mgr, 1, &sig) == HZ_R_EC_NETWORK_SERVICE_ACCOUNT_UNAVAILABLE, "connected, no linked Nintendo account: account unavailable");
    g_linked = 1; CHECK(token_request(mgr, 1, &sig) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "connected and linked: no ec-server hook, the server is unreachable");
    online_provider_install(NULL);
    CHECK(svc_find(aoc, 300)->status == IPC_STUB && svc_find(aoc, 301)->status == IPC_STUB, "300 / 301 stay documented defaults");

    printf("{\"test\":\"svc_misc_r10\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
