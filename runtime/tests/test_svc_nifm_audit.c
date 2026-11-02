/* Unit tests: ipc-reference-audit (2026-11-02-r4): the nifm IRequest / IGeneralService rows answer like an offline console with ids per switchbrew
 * (GetResult, GetAppletInfo naming the netConnect library applet, GetAdditionalInfo, GetRequirement, the communication settings), the bsd ids 28-31, and the
 * prepo renumbering. offline.c is #included. */
#include "../src/services/offline.c"
#include "svc_test.h"

static int g_released;
static uint32_t fake_connect(void *ctx) {
    (void)ctx;
    return 0;
}
static void fake_release(void *ctx) {
    (void)ctx;
    g_released++;
}

int main(void) {
    IpcCall c;
    IpcObject *nf = nifm_open();
    svc_call(nf, 5, NULL, 0, &c);
    IpcObject *gen = c.out_objects[0];
    svc_call(gen, 4, NULL, 0, &c);
    IpcObject *rq = c.out_objects[0];

    /* ---- IRequest on a console with no network */
    svc_call(rq, 1, NULL, 0, &c);
    CHECK(c.result == 0, "1 GetResult before Submit: no error yet");
    svc_call(rq, 21, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "21 GetAppletInfo before Submit: nothing to resolve in an applet");
    svc_call(rq, 22, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "22 GetAdditionalInfo before Submit: none");
    svc_call(rq, 4, NULL, 0, &c);
    svc_call(rq, 0, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == NIFM_STATE_FREE, "after Submit the NullProvider leaves the request in the free state");
    svc_call(rq, 1, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "1 GetResult: why the request was rejected");
    {
        uint8_t buf[0x1000];
        memset(buf, 0xcc, sizeof buf);
        uint32_t theme = 0x10;
        memset(&c, 0, sizeof c);
        c.in = (const uint8_t *)&theme;
        c.in_size = 4;
        svc_set_out_buf(&c, buf, sizeof buf);
        svc_call_c(rq, 21, &c);
        CHECK(c.result == 0 && c.out_size == 12, "21 GetAppletInfo after the rejection: three words");
        CHECK(SVC_OUT(&c, uint32_t, 0) == NIFM_APPLET_NET_CONNECT && NIFM_APPLET_NET_CONNECT == 0x0f, "...AppletId is netConnect (0x0f)");
        CHECK(SVC_OUT(&c, uint32_t, 4) == 0 && SVC_OUT(&c, uint32_t, 8) == 0x44, "...LibraryAppletMode 0 (all foreground), 0x44 bytes of storages");
        int32_t w[17];
        memcpy(w, buf, sizeof w);
        CHECK(w[0] == 0x20 && w[1] == 1 && w[2] == 0x20 && w[4] == 0x10, "...storage 1: CommonArguments (version 1, 0x20 bytes, the theme color given)");
        uint64_t tick;
        memcpy(&tick, buf + 4 + 0x18, 8);
        CHECK(tick != 0, "...with the system tick");
        CHECK(w[9] == 0x18 && w[10] == 0 && (uint32_t)w[11] == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED && !w[12] && !w[15], "...storage 2: netConnect's argument {0, the request's result, no profile uuid}");
        CHECK(w[16] == -1 && buf[0x44] == 0xcc, "...then the -1 terminator and nothing else");
        uint8_t small[8];
        memset(small, 0xcc, sizeof small);
        memset(&c, 0, sizeof c);
        svc_set_out_buf(&c, small, sizeof small);
        svc_call_c(rq, 21, &c);
        int32_t first;
        memcpy(&first, small, 4);
        CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 8) == 4 && first == -1 && small[4] == 0xcc, "...a buffer too small for the arguments gets only the terminator");
    }
    svc_call(rq, 22, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "22 GetAdditionalInfo reports the failure");
    svc_call(rq, 3, NULL, 0, &c);
    svc_call(rq, 1, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "3 Cancel on a rejected request keeps its result (the SDK cancels before HandleNetworkRequestResult)");
    svc_call(rq, 21, NULL, 0, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == NIFM_APPLET_NET_CONNECT, "...and GetAppletInfo still names netConnect");
    svc_call(rq, 0, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == NIFM_STATE_FREE, "...the request stays free");

    /* ---- Cancel on an accepted request (a provider that connects): the connection is released and the request starts over */
    {
        OnlineProvider fake = *online_null_provider();
        fake.net_connect = fake_connect;
        fake.net_release = fake_release;
        const OnlineProvider *prev = online_provider_install(&fake);
        svc_call(rq, 4, NULL, 0, &c);
        svc_call(rq, 0, NULL, 0, &c);
        CHECK(SVC_OUT(&c, uint32_t, 0) == NIFM_STATE_ACCEPTED, "Submit with a network: accepted");
        svc_call(gen, 21, NULL, 0, &c);
        CHECK(c.out_size >= 1 && c.out[0] == 1, "...IsAnyInternetRequestAccepted");
        svc_call(rq, 3, NULL, 0, &c);
        svc_call(rq, 0, NULL, 0, &c);
        uint32_t st = SVC_OUT(&c, uint32_t, 0);
        svc_call(rq, 1, NULL, 0, &c);
        CHECK(g_released == 1 && st == NIFM_STATE_FREE && c.result == 0, "3 Cancel on an accepted request releases the connection and resets it (released %d)", g_released);
        svc_call(gen, 21, NULL, 0, &c);
        CHECK(c.out[0] == 0, "...no request accepted any more");
        svc_call(rq, 21, NULL, 0, &c);
        CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "...and GetAppletInfo has nothing to offer");
        svc_call(rq, 3, NULL, 0, &c);
        CHECK(g_released == 1, "...a second Cancel releases nothing");
        online_provider_install(prev == online_null_provider() ? NULL : prev);
    }

    /* ---- requirement, revision, flags */
    uint8_t req[0x10];
    for (unsigned i = 0; i < sizeof req; i++) req[i] = (uint8_t)(i + 1);
    svc_call(rq, 19, NULL, 0, &c);
    CHECK(c.result == 0 && c.out_size == 0x24 && SVC_OUT(&c, uint32_t, 0) == 0, "19 GetRequirement: 0x24 bytes, zero until set");
    svc_call(rq, 5, req, sizeof req, &c);
    svc_call(rq, 19, NULL, 0, &c);
    CHECK(c.out_size == 0x24 && !memcmp(c.out, req, sizeof req) && SVC_OUT(&c, uint32_t, 0x10) == 0 && SVC_OUT(&c, uint32_t, 0x20) == 0, "...reads back what SetRequirement stored, padded with zeros");
    uint32_t rev = 7;
    svc_call(rq, 18, &rev, 4, &c);
    svc_call(rq, 20, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 7, "18 SetRequirementByRevision / 20 GetRevision");
    uint8_t on = 1;
    svc_call(rq, 16, &on, 1, &c);
    svc_call(rq, 17, &on, 1, &c);
    svc_call(rq, 23, &on, 1, &c);
    CHECK(((NifmReq *)rq->state)->greedy == 1 && ((NifmReq *)rq->state)->sharable == 1 && ((NifmReq *)rq->state)->kept_in_sleep == 1, "16 SetGreedy, 17 SetSharable, 23 SetKeptInSleep are kept");
    int32_t fd1 = 11, fd2 = 12;
    svc_call(rq, 24, &fd1, 4, &c);
    svc_call(rq, 24, &fd2, 4, &c);
    CHECK(((NifmReq *)rq->state)->nsockets == 2, "24 RegisterSocketDescriptor");
    svc_call(rq, 25, &fd1, 4, &c);
    CHECK(((NifmReq *)rq->state)->nsockets == 1 && ((NifmReq *)rq->state)->sockets[0] == 12, "25 UnregisterSocketDescriptor removes the right one");
    svc_call(rq, 25, &fd1, 4, &c);
    CHECK(c.result == 0 && ((NifmReq *)rq->state)->nsockets == 1, "...an unknown descriptor is ignored");
    const unsigned real_rows[] = {1, 16, 17, 18, 19, 20, 21, 23, 24, 25};
    for (unsigned i = 0; i < sizeof real_rows / sizeof real_rows[0]; i++) CHECK(svc_find(rq, real_rows[i])->status == IPC_IMPL, "IRequest %u is real", real_rows[i]);
    CHECK(svc_find(rq, 22)->status == IPC_STUB, "22 GetAdditionalInfo stays a documented default (its layout is undocumented)");

    /* ---- IGeneralService communication settings (ids 15-20 per switchbrew) */
    svc_call(gen, 17, NULL, 0, &c);
    CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == 1, "17 IsWirelessCommunicationEnabled: on by default");
    uint8_t off = 0;
    svc_call(gen, 16, &off, 1, &c);
    svc_call(gen, 17, NULL, 0, &c);
    CHECK(c.out[0] == 0, "16 SetWirelessCommunicationEnabled(false) reads back");
    svc_call(gen, 16, &on, 1, &c);
    svc_call(gen, 20, NULL, 0, &c);
    CHECK(c.out_size == 1 && c.out[0] == 0, "20 IsEthernetCommunicationEnabled: off without an adapter");
    svc_call(gen, 19, &on, 1, &c);
    svc_call(gen, 20, NULL, 0, &c);
    CHECK(c.out[0] == 1, "19 SetEthernetCommunicationEnabled reads back");
    svc_call(gen, 19, &off, 1, &c);
    svc_call(gen, 18, NULL, 0, &c);
    CHECK(c.result == 0 && c.out_size == 4 && c.out[2] == 0, "18 GetInternetConnectionStatus: disconnected");
    svc_call(gen, 15, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "15 GetCurrentIpConfigInfo: no connection");
    CHECK(svc_find(gen, 15)->status == IPC_STUB && svc_find(gen, 18)->status == IPC_IMPL, "...a documented default; 18 is real");
    uint32_t types = 3;
    svc_call(gen, 41, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "41 GetAcceptableNetworkTypeFlag: none by default");
    svc_call(gen, 40, &types, 4, &c);
    svc_call(gen, 41, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 3, "40 SetAcceptableNetworkTypeFlag reads back");
    svc_call(gen, 34, &on, 1, &c);
    CHECK(nifm_background_requests == 1, "34 SetBackgroundRequestEnabled");
    uint32_t t = 9;
    svc_call(gen, 43, &t, 4, &c);
    svc_call(gen, 57, &t, 4, &c);
    CHECK(nifm_wowl_wake_time == 9 && nifm_wowl_keepalive == 9, "43 SetWowlDelayedWakeTime, 57 SetWowlTcpKeepAliveTimeout");
    {
        uint8_t ip_in[0x20], ip_out[0x20];
        memset(ip_in, 0x5a, sizeof ip_in);
        memset(ip_out, 0xcc, sizeof ip_out);
        memset(&c, 0, sizeof c);
        svc_set_out_buf(&c, ip_out, sizeof ip_out);
        svc_call_c(gen, 27, &c);
        CHECK(ip_out[0] == 0 && ip_out[0x1f] == 0, "27 GetDefaultIpSetting: zero before any set");
        memset(&c, 0, sizeof c);
        svc_set_in_buf(&c, ip_in, sizeof ip_in);
        svc_call_c(gen, 28, &c);
        memset(ip_out, 0xcc, sizeof ip_out);
        memset(&c, 0, sizeof c);
        svc_set_out_buf(&c, ip_out, sizeof ip_out);
        svc_call_c(gen, 27, &c);
        CHECK(!memcmp(ip_in, ip_out, sizeof ip_in), "28 SetDefaultIpSetting reads back through 27");
    }
    const unsigned gen_real[] = {16, 17, 18, 19, 20, 28, 34, 40, 41, 43, 57};
    for (unsigned i = 0; i < sizeof gen_real / sizeof gen_real[0]; i++) CHECK(svc_find(gen, gen_real[i])->status == IPC_IMPL, "IGeneralService %u is real", gen_real[i]);

    /* ---- bsd ids 28-31 (switchbrew): GetResourceStatistics, RecvMMsg, SendMMsg, EventFd */
    IpcObject *bs = bsd_open();
    CHECK(!strcmp(svc_find(bs, 28)->name, "GetResourceStatistics") && svc_find(bs, 28)->status == IPC_IMPL, "bsd 28 GetResourceStatistics");
    CHECK(!strcmp(svc_find(bs, 29)->name, "RecvMMsg") && !strcmp(svc_find(bs, 30)->name, "SendMMsg") && !strcmp(svc_find(bs, 31)->name, "EventFd"), "bsd 29 RecvMMsg, 30 SendMMsg, 31 EventFd");
    svc_call(bs, 31, NULL, 0, &c);
    CHECK(c.result == 0 && c.out_size == 8 && SVC_OUT(&c, int32_t, 0) == -1 && SVC_OUT(&c, int32_t, 4) == HZ_EOPNOTSUPP, "bsd 31 EventFd: {-1, EOPNOTSUPP}");

    /* ---- prepo renumbering */
    IpcObject *pr = prepo_open();
    pthread_mutex_lock(&prepo_lock);
    prepo_init(prepo_st());
    pthread_mutex_unlock(&prepo_lock);
    uint8_t rep[0x20] = {0};
    for (unsigned id = 10100; id <= 10107; id++) {
        memset(&c, 0, sizeof c);
        svc_set_in_buf(&c, rep, sizeof rep);
        svc_call_c(pr, id, &c);
        CHECK(c.result == 0 && svc_find(pr, id)->status == IPC_IMPL, "prepo %u SaveReport* is accepted", id);
    }
    svc_call(pr, 30300, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint64_t, 0) == 8 * sizeof rep, "all eight report ids are counted in the storage");
    CHECK(svc_find(pr, 90200) == NULL && svc_find(pr, 90100)->status == IPC_STUB && !strcmp(svc_find(pr, 90100)->name, "ReadAllReportFiles"), "90100 is ReadAllReportFiles (no stored reports); the invented 90200 is gone");
    CHECK(!strcmp(svc_find(pr, 20100)->name, "SaveSystemReportOld") && !strcmp(svc_find(pr, 20102)->name, "SaveSystemReport"), "20100 is the old SaveSystemReport, 20102 the current one");

    printf("{\"test\":\"svc_nifm_audit\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
