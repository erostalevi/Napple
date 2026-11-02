/* Unit tests (ipc-depth-95 r10, part local): the IUserLocalCommunicationService configuration rows (105, 106, 403, 500-505, 600, 601) and ldn:u 1
 * CreateClientProcessMonitor / IClientProcessMonitor RegisterClient. The rules are switchbrew's "LDN services" State and argument checks. */
#include "../src/services/ldn.c"
#include "svc_test.h"
#include "online_loopback.h"
#include <unistd.h>

static void *push_later(void *arg) {
    uint8_t m[6] = {0}, f[8] = {7};
    usleep(30000);
    ldn_push_action_frame((uint64_t)(uintptr_t)arg, m, m, 2400, 1, f, sizeof f);
    return NULL;
}
static IpcObject *new_local(IpcObject *root) {
    IpcCall c;
    svc_call(root, 0, NULL, 0, &c);
    return c.out_objects[0];
}
static uint32_t call_u32(IpcObject *o, unsigned cmd, uint32_t v) {
    IpcCall c;
    svc_call(o, cmd, &v, 4, &c);
    return c.result;
}
static uint32_t call_pid(IpcObject *o, unsigned cmd, uint64_t pid) {
    IpcCall c;
    uint64_t placeholder = 0;
    memset(&c, 0, sizeof c);
    c.in = (const uint8_t *)&placeholder; c.in_size = 8; c.has_pid = 1; c.pid = pid;
    svc_call_c(o, cmd, &c);
    return c.result;
}
static void settings(uint8_t s[LDN_ACTION_FRAME_SETTINGS_SIZE], uint16_t mode, uint16_t size) {
    memset(s, 0, LDN_ACTION_FRAME_SETTINGS_SIZE);
    uint64_t lcid = 0x0100abcd00010000ull;
    memcpy(s, &lcid, 8); memcpy(s + 0x3c, &mode, 2); memcpy(s + 0x3e, &size, 2); memset(s + 0x40, 'p', size <= 0x40 ? size : 0x40);
}
static uint32_t send_frame(IpcObject *o, const void *frame, uint64_t n, uint16_t band, uint16_t ch) {
    uint8_t in[0x14] = {0};
    memset(in, 0xff, 6);            /* destination: broadcast */
    memset(in + 6, 0x22, 6);        /* bssid */
    memcpy(in + 0xc, &band, 2); memcpy(in + 0xe, &ch, 2);
    IpcCall c; memset(&c, 0, sizeof c); c.in = in; c.in_size = sizeof in;
    if (frame) svc_set_in_buf(&c, frame, n);
    svc_call_c(o, 502, &c);
    return c.result;
}

int main(void) {
    /* ---- core helpers ---- */
    CHECK(ldn_check_protocol(LDN_INITIALIZED, 1) == 0 && ldn_check_protocol(LDN_INITIALIZED, 3) == 0, "protocols 1 (NX) and 3 are permitted on NX");
    CHECK(ldn_check_protocol(LDN_INITIALIZED, 0) == LDN_R_INVALID_ARGUMENT && ldn_check_protocol(LDN_INITIALIZED, 2) == LDN_R_INVALID_ARGUMENT &&
              ldn_check_protocol(LDN_INITIALIZED, 4) == LDN_R_INVALID_ARGUMENT && ldn_check_protocol(LDN_INITIALIZED, 99) == LDN_R_INVALID_ARGUMENT,
          "0, the Switch 2 values and out-of-range values are refused");
    CHECK(ldn_check_protocol(LDN_STATION_OPENED, 1) == LDN_R_INVALID_STATE && ldn_check_protocol(LDN_NONE, 1) == LDN_R_INVALID_STATE, "SetProtocol needs Initialized");
    uint8_t afs[LDN_ACTION_FRAME_SETTINGS_SIZE];
    settings(afs, 1, 0x10); CHECK(ldn_check_action_frame_settings(afs, sizeof afs) == 0, "ActionFrameSettings: mode 1, passphrase 0x10");
    settings(afs, 2, 0x40); CHECK(ldn_check_action_frame_settings(afs, sizeof afs) == 0, "...mode 2, passphrase 0x40");
    settings(afs, 0, 0x20); CHECK(ldn_check_action_frame_settings(afs, sizeof afs) == LDN_R_INVALID_ARGUMENT, "...mode 0 refused");
    settings(afs, 3, 0x20); CHECK(ldn_check_action_frame_settings(afs, sizeof afs) == LDN_R_INVALID_ARGUMENT, "...mode 3 refused");
    settings(afs, 1, 0x0f); CHECK(ldn_check_action_frame_settings(afs, sizeof afs) == LDN_R_INVALID_ARGUMENT, "...passphrase under 0x10 refused");
    settings(afs, 1, 0x41); CHECK(ldn_check_action_frame_settings(afs, sizeof afs) == LDN_R_INVALID_ARGUMENT, "...passphrase over 0x40 refused");
    settings(afs, 1, 0x20); CHECK(ldn_check_action_frame_settings(afs, 0x40) == LDN_R_INVALID_ARGUMENT, "...short input refused");
    { uint16_t band, ch; int16_t old[2] = {24, 6}, old5[2] = {50, 36}, bad[2] = {24, 0}, other[2] = {60, 1};
      CHECK(ldn_decode_channel((uint8_t *)old, 4, &band, &ch) == 0 && band == 2400 && ch == 6, "channel: pre-20.0.0 Band 24, channel 6");
      CHECK(ldn_decode_channel((uint8_t *)old5, 4, &band, &ch) == 0 && band == 5000 && ch == 36, "channel: pre-20.0.0 Band 50, channel 36");
      CHECK(ldn_decode_channel((uint8_t *)bad, 4, &band, &ch) == -1, "channel 0 refused");
      CHECK(ldn_decode_channel((uint8_t *)other, 4, &band, &ch) == -1, "an unknown band refused");
      uint16_t v = (2u << 10) | 11; CHECK(ldn_decode_channel((uint8_t *)&v, 2, &band, &ch) == 0 && band == 2400 && ch == 11, "channel: 20.0.0+ u16, band index 2");
      v = (5u << 10) | 40; CHECK(ldn_decode_channel((uint8_t *)&v, 2, &band, &ch) == 0 && band == 5000 && ch == 40, "channel: 20.0.0+ u16, band index 5");
      v = (6u << 10) | 5; CHECK(ldn_decode_channel((uint8_t *)&v, 2, &band, &ch) == -1, "channel: band index 6 (6 GHz) is not valid on NX");
      v = (2u << 10); CHECK(ldn_decode_channel((uint8_t *)&v, 2, &band, &ch) == -1, "channel: 20.0.0+ channel 0 refused"); }
    CHECK(LDN_STATES(LDN_AP_CREATED, LDN_STATION_OPENED) == ((1u << 3) | (1u << 4)) && ldn_state_in(LDN_AP_CREATED, LDN_STATES(LDN_AP_CREATED)) &&
              !ldn_state_in(LDN_ERROR, LDN_STATES(LDN_AP_CREATED)), "state masks");

    /* ---- the rows, NullProvider ---- */
    IpcObject *root = ldn_open(); IpcCall c;
    IpcObject *a = new_local(root);
    Ldn *la = a->state;
    for (unsigned i = 0; i < sizeof local_cmds / sizeof *local_cmds; i++) {
        const IpcCmd *e = &local_cmds[i];
        CHECK(e->status == IPC_IMPL, "row %u %s is real", e->cmd, e->name);
    }
    CHECK(svc_find(root, 1) && svc_find(root, 1)->status == IPC_IMPL, "ldn:u 1 CreateClientProcessMonitor is real");

    CHECK(call_u32(a, 105, 1) == 0 && la->audio_policy == 1, "105 SetWirelessAudioPolicy is kept (no State rule)");
    CHECK(call_u32(a, 105, 0) == 0 && la->audio_policy == 0, "...and replaced");
    CHECK(call_u32(a, 106, 1) == LDN_R_INVALID_STATE, "106 SetProtocol before Initialize: invalid state");
    CHECK(call_u32(a, 403, 1) == LDN_R_INVALID_STATE, "403 SetOperationMode before Initialize: invalid state");
    settings(afs, 1, 0x20);
    svc_call(a, 500, afs, sizeof afs, &c); CHECK(c.result == LDN_R_INVALID_STATE && !la->action_frame, "500 EnableActionFrame before Initialize: invalid state");
    svc_call(a, 501, NULL, 0, &c); CHECK(c.result == LDN_R_INVALID_STATE, "501 DisableActionFrame before Initialize: invalid state");
    { int16_t v = 10; svc_call(a, 600, &v, 2, &c); CHECK(c.result == LDN_R_INVALID_STATE, "600 SetTxPower before an access point / station: invalid state"); }
    svc_call(a, 601, NULL, 0, &c); CHECK(c.result == LDN_R_INVALID_STATE, "601 ResetTxPower likewise");

    svc_call(a, 400, NULL, 0, &c);
    CHECK(la->protocol == LDN_PROTOCOL_NX && la->tx_power == -1 && !la->action_frame, "Initialize: protocol NX, default tx power, action frames off");
    CHECK(call_u32(a, 106, 3) == 0 && la->protocol == 3, "106 SetProtocol(3) while Initialized is kept");
    CHECK(call_u32(a, 106, 2) == LDN_R_INVALID_ARGUMENT && la->protocol == 3, "...a Switch 2 protocol is refused, the old value stays");
    CHECK(call_u32(a, 106, 0) == LDN_R_INVALID_ARGUMENT, "...protocol 0 is refused at the service (the SDK maps Default to 1 itself)");
    CHECK(call_u32(a, 403, 1) == 0 && la->operation_mode == 1, "403 SetOperationMode(HighSpeed) while Initialized");
    CHECK(call_u32(a, 403, 0) == 0 && la->operation_mode == 0, "...Stable again");
    settings(afs, 0, 0x20); svc_call(a, 500, afs, sizeof afs, &c); CHECK(c.result == LDN_R_INVALID_ARGUMENT && !la->action_frame, "500 with security mode 0 is refused");
    settings(afs, 2, 0x30); svc_call(a, 500, afs, sizeof afs, &c);
    CHECK(c.result == 0 && la->action_frame && !memcmp(la->action_frame_settings, afs, sizeof afs), "500 EnableActionFrame keeps the settings");
    svc_call(a, 503, (uint32_t[]){1}, 4, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "503 RecvActionFrame once enabled: nothing has arrived");
    /* 503 with frames pushed by the provider side */
    {
        uint8_t src[6] = {1, 2, 3, 4, 5, 6}, bss[6] = {9, 8, 7, 6, 5, 4}, fr[40], buf[64], big[LDN_ACTION_FRAME_MAX + 1] = {1};
        for (unsigned i = 0; i < sizeof fr; i++) fr[i] = (uint8_t)(0xa0 + i);
        uint64_t myid = 0; memcpy(&myid, afs, 8);
        CHECK(ldn_push_action_frame(myid, src, bss, 2400, 6, fr, sizeof fr) == 1, "a pushed frame is queued for the object that enabled action frames");
        CHECK(ldn_push_action_frame(myid + 1, src, bss, 2400, 6, fr, sizeof fr) == 0, "...but not for another LocalCommunicationId");
        CHECK(ldn_push_action_frame(myid, src, bss, 2400, 6, big, sizeof big) == 0 && ldn_push_action_frame(myid, src, bss, 2400, 6, fr, 0) == 0, "an empty or oversized frame is refused");
        memset(&c, 0, sizeof c); c.in = (const uint8_t *)(uint32_t[]){1}; c.in_size = 4; svc_set_out_buf(&c, buf, sizeof buf); svc_call_c(a, 503, &c);
        CHECK(c.result == 0 && c.out_size == LDN_ACTION_RECV_OUT_SIZE && !memcmp(c.out, src, 6) && !memcmp(c.out + 6, bss, 6) && SVC_OUT(&c, uint16_t, 0xc) == 2400 &&
                  SVC_OUT(&c, uint16_t, 0xe) == 6 && SVC_OUT(&c, uint32_t, 0x10) == sizeof fr && !memcmp(buf, fr, sizeof fr), "503 hands out the frame with its source, bssid, channel and size");
        svc_call(a, 503, (uint32_t[]){1}, 4, &c); CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE, "...once; then the queue is empty again");
        for (unsigned i = 0; i < LDN_AF_QUEUE_MAX + 3; i++) { fr[0] = (uint8_t)i; ldn_push_action_frame(0, src, bss, 5000, 36, fr, 8); }
        CHECK(la->afq.count == LDN_AF_QUEUE_MAX && la->afq.dropped == 3, "the queue is bounded: the surplus frames are dropped and counted");
        uint8_t small[4];
        memset(&c, 0, sizeof c); c.in = (const uint8_t *)(uint32_t[]){1}; c.in_size = 4; svc_set_out_buf(&c, small, sizeof small); svc_call_c(a, 503, &c);
        CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0x10) == 4 && small[0] == 0, "a short buffer cuts the frame and the size says how much was written; the oldest comes first");
        memset(&c, 0, sizeof c); c.in = (const uint8_t *)(uint32_t[]){1}; c.in_size = 4; svc_set_out_buf(&c, buf, sizeof buf); svc_call_c(a, 503, &c);
        CHECK(buf[0] == 1, "FIFO order");
        svc_call(a, 501, NULL, 0, &c); CHECK(la->afq.count == 0, "DisableActionFrame empties the queue");
        CHECK(ldn_push_action_frame(0, src, bss, 2400, 6, fr, 8) == 0, "...and nothing is queued while disabled");
        svc_call(a, 500, afs, sizeof afs, &c);
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        svc_call(a, 503, (uint32_t[]){0}, 4, &c); clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        CHECK(c.result == HZ_R_LDN_NETWORK_NOT_AVAILABLE && ms >= LDN_RECV_WAIT_MS - 20 && ms < 2000, "a blocking 503 with nothing queued waits a bounded time (%ld ms)", ms);
        pthread_t th; pthread_create(&th, NULL, (void *(*)(void *))(void *)push_later, (void *)(uintptr_t)myid);
        svc_call(a, 503, (uint32_t[]){0}, 4, &c); pthread_join(th, NULL);
        CHECK(c.result == 0, "a blocking 503 is woken by a push from another thread");
    }
    svc_call(a, 501, NULL, 0, &c); CHECK(c.result == 0 && !la->action_frame, "501 DisableActionFrame");
    svc_call(a, 503, (uint32_t[]){1}, 4, &c); CHECK(c.result == LDN_R_INVALID_STATE, "503 after Disable: EnableActionFrame must come first");
    svc_call(a, 500, afs, sizeof afs, &c); CHECK(c.result == 0 && la->action_frame, "...enable again");

    uint8_t frame[0x40]; memset(frame, 0x5c, sizeof frame);
    CHECK(send_frame(a, frame, sizeof frame, 24, 6) == LDN_R_INVALID_STATE, "502 SendActionFrame while only Initialized: invalid state");
    { int16_t hc[2] = {24, 6}; svc_call(a, 505, hc, 4, &c); CHECK(c.result == LDN_R_INVALID_STATE, "505 SetHomeChannel outside Station: invalid state"); }

    svc_call(a, 300, NULL, 0, &c); CHECK(*st(a) == LDN_STATION_OPENED, "OpenStation");
    CHECK(call_u32(a, 106, 1) == LDN_R_INVALID_STATE && la->protocol == 3, "106 SetProtocol from Station: invalid state");
    CHECK(call_u32(a, 403, 1) == LDN_R_INVALID_STATE && la->operation_mode == 0, "403 from Station: invalid state");
    svc_call(a, 501, NULL, 0, &c); CHECK(c.result == LDN_R_INVALID_STATE && la->action_frame, "501 from Station: invalid state, still enabled");
    { int16_t hc[2] = {50, 36}; svc_call(a, 505, hc, 4, &c); CHECK(c.result == 0 && la->home_band == 5000 && la->home_channel == 36, "505 SetHomeChannel(5 GHz, 36) from Station"); }
    { uint16_t v = (2u << 10) | 1; svc_call(a, 505, &v, 2, &c); CHECK(c.result == 0 && la->home_band == 2400 && la->home_channel == 1, "505 with the 20.0.0+ u16 argument"); }
    { int16_t hc[2] = {24, 0}; svc_call(a, 505, hc, 4, &c); CHECK(c.result == LDN_R_INVALID_ARGUMENT && la->home_channel == 1, "505 with channel 0: refused, old value kept"); }
    CHECK(send_frame(a, frame, sizeof frame, 24, 6) == 0 && la->frames_sent == 1, "502 SendActionFrame from Station goes out (nobody answers)");
    CHECK(send_frame(a, frame, sizeof frame, 24, 0) == LDN_R_INVALID_ARGUMENT && la->frames_sent == 1, "502 with ChannelNumber 0 is refused");
    CHECK(send_frame(a, NULL, 0, 24, 6) == LDN_R_INVALID_ARGUMENT, "502 without a frame is refused");
    { static uint8_t big[LDN_ACTION_FRAME_MAX + 1]; CHECK(send_frame(a, big, sizeof big, 24, 6) == LDN_R_INVALID_ARGUMENT, "502 over 0x400 bytes is refused"); }
    { int16_t v = 0x80; svc_call(a, 600, &v, 2, &c); CHECK(c.result == 0 && la->tx_power == 0x80, "600 SetTxPower(0x80) from Station"); }
    { int16_t v = 0x100; svc_call(a, 600, &v, 2, &c); CHECK(c.result == LDN_R_INVALID_ARGUMENT && la->tx_power == 0x80, "600 over 0xFF refused"); }
    { int16_t v = -1; svc_call(a, 600, &v, 2, &c); CHECK(c.result == LDN_R_INVALID_ARGUMENT, "600 negative refused"); }
    svc_call(a, 601, NULL, 0, &c); CHECK(c.result == 0 && la->tx_power == -1, "601 ResetTxPower restores the default");
    svc_call(a, 301, NULL, 0, &c); svc_call(a, 200, NULL, 0, &c); CHECK(*st(a) == LDN_AP_OPENED, "CloseStation, OpenAccessPoint");
    { int16_t v = 3; svc_call(a, 600, &v, 2, &c); CHECK(c.result == 0 && la->tx_power == 3, "600 from AccessPoint"); }
    CHECK(send_frame(a, frame, sizeof frame, 24, 6) == LDN_R_INVALID_STATE, "502 from AccessPoint (not created): invalid state");
    uint8_t cr[0x98] = {0}; cr[0] = 1; uint64_t lc = 0x0100abcd12345678ull; memcpy(cr + 0x78, &lc, 8); cr[0x78 + 0x12] = 4;
    svc_call(a, 202, cr, sizeof cr, &c); CHECK(c.result == 0 && *st(a) == LDN_AP_CREATED, "CreateNetwork (NullProvider)");
    CHECK(send_frame(a, frame, 8, 50, 40) == 0 && la->frames_sent == 2, "502 from AccessPointCreated goes out");
    { int16_t hc[2] = {24, 6}; svc_call(a, 505, hc, 4, &c); CHECK(c.result == LDN_R_INVALID_STATE, "505 from AccessPointCreated: invalid state"); }
    svc_call(a, 401, NULL, 0, &c); CHECK(*st(a) == LDN_NONE, "Finalize");
    { int16_t v = 3; svc_call(a, 600, &v, 2, &c); CHECK(c.result == LDN_R_INVALID_STATE, "600 after Finalize: invalid state"); }
    svc_call(a, 400, NULL, 0, &c); CHECK(la->protocol == LDN_PROTOCOL_NX && !la->action_frame && la->tx_power == -1 && !la->home_channel, "a new Initialize starts from the defaults again");
    IpcObject *b = new_local(root);
    CHECK(call_u32(b, 105, 7) == 0 && ((Ldn *)b->state)->audio_policy == 7 && la->audio_policy == 0, "the configuration is per object");

    /* ---- ldn:u 1 CreateClientProcessMonitor / RegisterClient ---- */
    svc_call(root, 1, NULL, 0, &c); CHECK(c.result == 0 && c.nout_objects == 1 && !strcmp(c.out_objects[0]->iface, "IClientProcessMonitor"), "1 CreateClientProcessMonitor returns an IClientProcessMonitor");
    IpcObject *mon = c.out_objects[0]; LdnMonitor *m = mon->state;
    CHECK(call_pid(mon, 0, 0x51) == 0 && !m->client, "RegisterClient for a process that never initialised: success, nothing held");
    CHECK(call_pid(mon, 0, 0) == 0 && !m->client, "RegisterClient with PID 0: success, nothing held");
    IpcObject *p = new_local(root); Ldn *lp = p->state;
    CHECK(call_pid(p, 402, 0x51) == 0 && *st(p) == LDN_INITIALIZED, "InitializeWithVersion with a PID registers the client");
    CHECK(call_pid(mon, 0, 0x51) == 0 && m->client == lp && lp->monitor == m, "RegisterClient finds it and holds it");
    IpcObject *mon2 = NULL; svc_call(root, 1, NULL, 0, &c); mon2 = c.out_objects[0];
    CHECK(call_pid(mon2, 0, 0x51) == 0 && !((LdnMonitor *)mon2->state)->client, "...the registry entry was cleared: a second monitor finds nothing");
    CHECK(call_pid(mon, 0, 0x99) == 0 && m->client == lp, "a second RegisterClient on a holding monitor is a no-op");
    monitor_destroy(mon2); free(mon2);

    /* the monitor's reference keeps a hosted network up after the service object is closed (loopback provider) */
    memset(&lb, 0, sizeof lb); online_provider_install(&lb_provider);
    svc_call(p, 200, NULL, 0, &c); svc_call(p, 202, cr, sizeof cr, &c); CHECK(c.result == 0 && lb.ldn_hosting, "the held client hosts a network at the provider");
    ldn_local_destroy(p); free(p);
    CHECK(lb.ldn_hosting && lp->orphan && m->client == lp, "closing its service object leaves the network up while the monitor holds the client");
    monitor_destroy(mon); free(mon);
    CHECK(!lb.ldn_hosting, "closing the monitor ends it");
    IpcObject *q = new_local(root);
    CHECK(call_pid(q, 402, 0x52) == 0, "another client initialises");
    svc_call(q, 200, NULL, 0, &c); svc_call(q, 202, cr, sizeof cr, &c); CHECK(lb.ldn_hosting, "...and hosts");
    ldn_local_destroy(q); free(q);
    CHECK(!lb.ldn_hosting, "without a monitor, closing the service object ends the network at once");
    svc_call(root, 1, NULL, 0, &c); IpcObject *mon3 = c.out_objects[0];
    CHECK(call_pid(mon3, 0, 0x52) == 0 && !((LdnMonitor *)mon3->state)->client, "a closed client left the registry");
    monitor_destroy(mon3); free(mon3);
    online_provider_install(NULL);

    printf("{\"test\":\"svc_local_r10_ldn\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
