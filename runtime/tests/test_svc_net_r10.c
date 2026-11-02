/* Unit tests: ipc-depth-95 r10, part net: the nifm IGeneralService rows that became real (network profiles, scan requests, the access point, the power
 * state, the telemetry event, NotifyConnectionStateChanged, IsWiredConnectionAvailable), the bsd rows (EventFd / NoSfEventFd as real event descriptors,
 * RegisterClientShared, SetSocketBufferAttribute, ShutdownAllSockets(how), the NoMem mmsg calls, the thread core mask) and the nsd application server
 * environment type. Both the offline answers (NullProvider) and a provider that reports a connection are checked. offline.c is #included. */
#include "../src/services/offline.c"
#include "svc_test.h"

/* ---- a provider with a network: net_info reports what the test sets, net_connect accepts and counts */
static OnlineNetInfo g_ni;
static int g_connects, g_releases;
static int fake_net_info(void *ctx, OnlineNetInfo *o) {
    (void)ctx;
    *o = g_ni;
    return 0;
}
static uint32_t fake_connect(void *ctx) {
    (void)ctx;
    g_connects++;
    return 0;
}
static void fake_release(void *ctx) {
    (void)ctx;
    g_releases++;
}
static OnlineProvider g_fake;
static void use_fake(int connected, uint8_t kind) {
    g_fake = *online_null_provider();
    g_fake.net_info = fake_net_info;
    g_fake.net_connect = fake_connect;
    g_fake.net_release = fake_release;
    g_fake.caps |= ONLINE_CAP_NETWORK;
    memset(&g_ni, 0, sizeof g_ni);
    g_ni.connected = (uint8_t)connected;
    g_ni.kind = kind;
    g_ni.strength = 3;
    g_ni.ip = 0xc0a8010a;      /* 192.168.1.10 */
    g_ni.mask = 0xffffff00;    /* 255.255.255.0 */
    g_ni.gateway = 0xc0a80101; /* 192.168.1.1 */
    g_ni.dns1 = 0x01010101;
    g_ni.dns2 = 0x08080808;
    online_provider_install(&g_fake);
}
static void use_null(void) { online_provider_install(NULL); }

static IpcObject *new_request(IpcObject *gen) {
    IpcCall c;
    svc_call(gen, 4, NULL, 0, &c);
    return c.out_objects[0];
}
static uint32_t req_state(IpcObject *rq) {
    IpcCall c;
    svc_call(rq, 0, NULL, 0, &c);
    return SVC_OUT(&c, uint32_t, 0);
}
static uint32_t req_result(IpcObject *rq) {
    IpcCall c;
    svc_call(rq, 1, NULL, 0, &c);
    return c.result;
}
/* EnumerateNetworkProfiles(mask) into buf (cap entries); returns the total and leaves the call in *c */
static int32_t enum_profiles(IpcObject *gen, uint8_t mask, uint8_t *buf, unsigned cap, IpcCall *c) {
    memset(c, 0, sizeof *c);
    c->in = &mask;
    c->in_size = 1;
    if (cap) svc_set_out_buf(c, buf, (uint64_t)cap * NIFM_BASIC_INFO_SIZE);
    svc_call_c(gen, 7, c);
    return c->out_size >= 4 ? SVC_OUT(c, int32_t, 0) : -1;
}
static void make_profile(uint8_t *d, const char *name, uint8_t iface) {
    memset(d, 0, NIFM_PROFILE_SIZE);
    d[0] = 1;
    d[0xd] = 1;
    snprintf((char *)d + NIFM_PROF_NAME, 0x40, "%s", name);
    d[NIFM_PROF_IFACE] = iface;
    d[NIFM_PROF_AUTO] = 1;
    uint8_t *w = d + NIFM_PROF_WIRELESS;
    w[0] = 4;
    memcpy(w + 1, "home", 4);
    w[0x22] = 6; /* WPA2-PSK */
    w[0x23] = 4; /* AES */
}

static void test_nifm_profiles(IpcObject *gen) {
    IpcCall c;
    uint8_t d[NIFM_PROFILE_SIZE], back[NIFM_PROFILE_SIZE], info[4 * NIFM_BASIC_INFO_SIZE], uuid[16];
    use_null();
    /* ---- offline: no connection, no saved profile */
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, back, sizeof back);
    svc_call_c(gen, 5, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "5 GetCurrentNetworkProfile offline: no connection, no profile");
    CHECK(enum_profiles(gen, 0x7, info, 4, &c) == 0 && c.result == 0, "7 EnumerateNetworkProfiles offline: none saved");
    memset(uuid, 0x11, sizeof uuid);
    memset(&c, 0, sizeof c);
    c.in = uuid;
    c.in_size = 16;
    svc_set_out_buf(&c, back, sizeof back);
    svc_call_c(gen, 8, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_PROFILE_NOT_FOUND, "8 GetNetworkProfile of an unknown uuid: not found");
    svc_call(gen, 10, uuid, 16, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_PROFILE_NOT_FOUND, "10 RemoveNetworkProfile of an unknown uuid: not found");

    /* ---- SetNetworkProfile / Enumerate / Get / Remove */
    make_profile(d, "Home", 1);
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, d, sizeof d);
    svc_call_c(gen, 9, &c);
    CHECK(c.result == 0 && c.out_size == 16, "9 SetNetworkProfile returns a uuid");
    memcpy(uuid, c.out, 16);
    CHECK(!nifm_uuid_zero(uuid) && (uuid[6] >> 4) == 4 && (uuid[8] & 0xc0) == 0x80, "...a new version-4 uuid for a profile saved with none");
    memset(info, 0xcc, sizeof info);
    CHECK(enum_profiles(gen, 1u << NIFM_PROFILE_USER, info, 4, &c) == 1, "7 Enumerate(user): one profile");
    CHECK(!memcmp(info, uuid, 16) && !strcmp((char *)info + 0x10, "Home") && info[0x50] == NIFM_PROFILE_USER && info[0x51] == 1, "...basic info: uuid, name, type user, Ieee80211");
    CHECK(info[0x52] == 4 && !memcmp(info + 0x53, "home", 4) && info[0x73] == 6 && info[0x74] == 4, "...ssid, authentication, encryption from the wireless setting");
    CHECK(info[NIFM_BASIC_INFO_SIZE] == 0xcc, "...one entry of 0x75 bytes written, nothing after it");
    CHECK(enum_profiles(gen, 1u << NIFM_PROFILE_TEMPORARY, info, 4, &c) == 0, "7 Enumerate(temporary): the user profile is not counted");
    CHECK(enum_profiles(gen, 1u << NIFM_PROFILE_USER, info, 0, &c) == 1 && c.result == 0, "7 Enumerate with no room still reports the total");
    memset(&c, 0, sizeof c);
    c.in = uuid;
    c.in_size = 16;
    memset(back, 0, sizeof back);
    svc_set_out_buf(&c, back, sizeof back);
    svc_call_c(gen, 8, &c);
    CHECK(c.result == 0 && !strcmp((char *)back + NIFM_PROF_NAME, "Home") && !memcmp(back + NIFM_PROF_UUID, uuid, 16) && back[NIFM_PROF_WIRELESS] == 4,
          "8 GetNetworkProfile reads back what was saved, under its uuid");
    memcpy(d + NIFM_PROF_UUID, uuid, 16);
    snprintf((char *)d + NIFM_PROF_NAME, 0x40, "%s", "Office");
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, d, sizeof d);
    svc_call_c(gen, 9, &c);
    CHECK(c.result == 0 && !memcmp(c.out, uuid, 16), "9 SetNetworkProfile with an existing uuid keeps it");
    CHECK(enum_profiles(gen, 1, info, 4, &c) == 1 && !strcmp((char *)info + 0x10, "Office"), "...and replaces that profile (still one, renamed)");
    svc_call(gen, 10, uuid, 16, &c);
    CHECK(c.result == 0, "10 RemoveNetworkProfile");
    CHECK(enum_profiles(gen, 1, info, 4, &c) == 0, "...the profile is gone");
    svc_call(gen, 10, uuid, 16, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_PROFILE_NOT_FOUND, "...a second remove: not found");
    /* the store is bounded */
    uint8_t ids[NIFM_MAX_PROFILES][16];
    for (unsigned i = 0; i < NIFM_MAX_PROFILES; i++) {
        make_profile(d, "n", 1);
        memset(&c, 0, sizeof c);
        svc_set_in_buf(&c, d, sizeof d);
        svc_call_c(gen, 9, &c);
        memcpy(ids[i], c.out, 16);
    }
    make_profile(d, "one too many", 1);
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, d, sizeof d);
    svc_call_c(gen, 9, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_PROFILE_FULL && c.out_size == 0, "9 SetNetworkProfile with every slot taken: full");
    CHECK(enum_profiles(gen, 1, info, 4, &c) == NIFM_MAX_PROFILES, "...the total counts every profile though only 4 fit the buffer");
    for (unsigned i = 0; i < NIFM_MAX_PROFILES; i++) svc_call(gen, 10, ids[i], 16, &c);
    CHECK(enum_profiles(gen, 7, info, 4, &c) == 0, "...all removed again");

    /* ---- CreateTemporaryNetworkProfile / INetworkProfile */
    make_profile(d, "Temp", 2);
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, d, sizeof d);
    svc_call_c(gen, 14, &c);
    CHECK(c.result == 0 && c.nout_objects == 1 && c.out_size == 16, "14 CreateTemporaryNetworkProfile: a uuid and an INetworkProfile");
    IpcObject *tp = c.out_objects[0];
    memcpy(uuid, c.out, 16);
    CHECK(enum_profiles(gen, 1u << NIFM_PROFILE_TEMPORARY, info, 4, &c) == 1 && info[0x50] == NIFM_PROFILE_TEMPORARY && !memcmp(info, uuid, 16),
          "...listed as a temporary profile");
    CHECK(enum_profiles(gen, 1u << NIFM_PROFILE_USER, info, 4, &c) == 0, "...not as a user profile");
    svc_call(gen, 10, uuid, 16, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_PROFILE_NOT_FOUND, "10 RemoveNetworkProfile does not remove a temporary profile");
    snprintf((char *)d + NIFM_PROF_NAME, 0x40, "%s", "Temp2");
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, d, sizeof d);
    svc_call_c(tp, 0, &c);
    CHECK(c.result == 0, "INetworkProfile 0 Update");
    memset(&c, 0, sizeof c);
    c.in = uuid;
    c.in_size = 16;
    svc_set_out_buf(&c, back, sizeof back);
    svc_call_c(gen, 8, &c);
    CHECK(c.result == 0 && !strcmp((char *)back + NIFM_PROF_NAME, "Temp2") && back[NIFM_PROF_TYPE] == NIFM_PROFILE_TEMPORARY && back[NIFM_PROF_IFACE] == 2,
          "...8 GetNetworkProfile reads the updated temporary profile");
    svc_call(tp, 2, NULL, 0, &c);
    uint8_t saved[16];
    memcpy(saved, c.out, 16);
    CHECK(c.result == 0 && c.out_size == 16 && memcmp(saved, uuid, 16) != 0, "INetworkProfile 2 Persist: saved as a user profile under a new uuid");
    CHECK(enum_profiles(gen, 1, info, 4, &c) == 1 && !strcmp((char *)info + 0x10, "Temp2") && !memcmp(info, saved, 16), "...listed as a user profile");
    tp->destroy(tp);
    CHECK(enum_profiles(gen, 1u << NIFM_PROFILE_TEMPORARY, info, 4, &c) == 0, "closing the INetworkProfile removes the temporary profile");
    CHECK(enum_profiles(gen, 1, info, 4, &c) == 1, "...the persisted copy stays");
    svc_call(gen, 10, saved, 16, &c);

    /* ---- a provider with a wired connection (NAPPLE_NET=host reports one): the connection is the current profile */
    use_fake(1, 2);
    memset(&c, 0, sizeof c);
    memset(back, 0xcc, sizeof back);
    svc_set_out_buf(&c, back, sizeof back);
    svc_call_c(gen, 5, &c);
    uint8_t ip[4] = {192, 168, 1, 10}, mask[4] = {255, 255, 255, 0}, gw[4] = {192, 168, 1, 1}, dns[4] = {1, 1, 1, 1};
    CHECK(c.result == 0 && back[0] == 1 && !memcmp(back + 1, ip, 4) && !memcmp(back + 5, mask, 4) && !memcmp(back + 9, gw, 4), "5 GetCurrentNetworkProfile connected: automatic addressing with the provider's addresses");
    CHECK(back[0xd] == 1 && !memcmp(back + 0xe, dns, 4) && back[0x12] == 8, "...the provider's DNS servers");
    CHECK(!memcmp(back + NIFM_PROF_UUID, nifm_host_uuid, 16) && !strcmp((char *)back + NIFM_PROF_NAME, "Wired Connection") && back[NIFM_PROF_IFACE] == 2 && back[NIFM_PROF_AUTO] == 1,
          "...a fixed uuid, the wired name, interface Ethernet, auto-connect");
    uint16_t mtu;
    memcpy(&mtu, back + 0xc0, 2);
    CHECK(mtu == 1400 && back[NIFM_PROFILE_SIZE - 1] == 0, "...MTU 1400, 0x17c bytes");
    CHECK(enum_profiles(gen, 1, info, 4, &c) == 1 && !memcmp(info, nifm_host_uuid, 16) && info[0x51] == 2, "7 Enumerate lists the connection's profile");
    CHECK(enum_profiles(gen, 1u << NIFM_PROFILE_TEMPORARY, info, 4, &c) == 0, "...as a user profile only");
    memset(&c, 0, sizeof c);
    c.in = nifm_host_uuid;
    c.in_size = 16;
    svc_set_out_buf(&c, back, sizeof back);
    svc_call_c(gen, 8, &c);
    CHECK(c.result == 0 && back[NIFM_PROF_IFACE] == 2, "8 GetNetworkProfile finds it by its uuid");
    svc_call(gen, 10, nifm_host_uuid, 16, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_PROFILE_NOT_FOUND, "10 the connection's profile cannot be removed");
    g_ni.kind = 1;
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, back, sizeof back);
    svc_call_c(gen, 5, &c);
    CHECK(c.result == 0 && !strcmp((char *)back + NIFM_PROF_NAME, "Wireless Connection") && back[NIFM_PROF_IFACE] == 1, "...a wireless connection is named and typed as one");
    g_ni.connected = 0;
    svc_call(gen, 5, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "...the provider lost its connection: no current profile again");
    CHECK(enum_profiles(gen, 1, info, 4, &c) == 0, "...and nothing listed");
    use_null();
}

static void test_nifm_scan_ap(IpcObject *gen) {
    IpcCall c;
    use_null();
    nifm_scan_count = 5; /* as if a previous scan had found some */
    svc_call(gen, 2, NULL, 0, &c);
    CHECK(c.result == 0 && c.nout_objects == 1 && !strcmp(c.out_objects[0]->iface, "IScanRequest"), "2 CreateScanRequest: an IScanRequest");
    IpcObject *sc = c.out_objects[0];
    svc_call(sc, 3, NULL, 0, &c);
    uint32_t ev = c.out_copy[0];
    CHECK(c.nout_copy == 1 && ev && !event_is_signaled(ev), "IScanRequest 3: an event, quiet before Submit");
    svc_call(sc, 3, NULL, 0, &c);
    CHECK(c.out_copy[0] == ev, "...the same event on a second call");
    svc_call(sc, 0, NULL, 0, &c);
    CHECK(c.result == 0 && event_is_signaled(ev), "IScanRequest 0 Submit: done, the event fires");
    svc_call(sc, 1, NULL, 0, &c);
    CHECK(c.out_size == 1 && c.out[0] == 0, "IScanRequest 1 IsProcessing: no (the scan finished inside Submit)");
    svc_call(sc, 2, NULL, 0, &c);
    CHECK(c.result == 0, "IScanRequest 2 GetResult: the scan succeeded");
    int16_t ch[3] = {1, 6, 11};
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, ch, sizeof ch);
    svc_call_c(sc, 4, &c);
    CHECK(((NifmScan *)sc->state)->nchannels == 3 && ((NifmScan *)sc->state)->channels[2] == 11, "IScanRequest 4 SetChannels keeps the list");
    svc_call(gen, 35, NULL, 0, &c);
    CHECK(c.result == 0 && c.out_size == 4 && SVC_OUT(&c, int32_t, 0) == 0, "35 GetScanData: the scan saw no access point (no provider scan source)");
    svc_call(gen, 47, NULL, 0, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 0, "47 GetScanData (19.0.0+): same");
    uint8_t off = 0, on = 1;
    svc_call(gen, 16, &off, 1, &c);
    svc_call(gen, 2, NULL, 0, &c);
    IpcObject *sc2 = c.out_objects[0];
    svc_call(sc2, 0, NULL, 0, &c);
    svc_call(sc2, 2, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "a scan with wireless communication off fails: communication disabled");
    svc_call(gen, 16, &on, 1, &c);
    sc->destroy(sc);
    sc2->destroy(sc2);

    /* ---- 36 GetCurrentAccessPoint / 44 IsWiredConnectionAvailable */
    svc_call(gen, 36, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "36 GetCurrentAccessPoint offline: no network");
    svc_call(gen, 44, NULL, 0, &c);
    CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == 0, "44 IsWiredConnectionAvailable offline: no");
    use_fake(1, 2);
    svc_call(gen, 36, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "36 a wired connection has no access point");
    svc_call(gen, 44, NULL, 0, &c);
    CHECK(c.out[0] == 1, "44 IsWiredConnectionAvailable with the provider's wired connection: yes");
    g_ni.kind = 1;
    uint8_t ap[0x40];
    memset(ap, 0xcc, sizeof ap);
    memset(&c, 0, sizeof c);
    svc_set_out_buf(&c, ap, sizeof ap);
    svc_call_c(gen, 36, &c);
    CHECK(c.result == 0 && ap[0] == 0 && ap[0x3f] == 0, "36 a wireless connection: access point data (zero: the provider reports no SSID)");
    svc_call(gen, 44, NULL, 0, &c);
    CHECK(c.out[0] == 0, "44 a wireless connection is not a wired one");
    use_null();
}

static void test_nifm_power(IpcObject *gen) {
    IpcCall c;
    use_fake(1, 2);
    nifm_power = NIFM_POWER_AWAKE;
    svc_call(gen, 31, NULL, 0, &c);
    uint32_t tel = c.out_copy[0];
    CHECK(c.result == 0 && c.nout_copy == 1 && tel, "31 GetTelemetorySystemEventReadableHandle: an event");
    svc_call(gen, 31, NULL, 0, &c);
    CHECK(c.out_copy[0] == tel, "...one event for the service");
    event_clear_handle(tel);
    IpcObject *keep = new_request(gen), *drop = new_request(gen);
    uint8_t on = 1;
    svc_call(keep, 23, &on, 1, &c);
    svc_call(keep, 4, NULL, 0, &c);
    svc_call(drop, 4, NULL, 0, &c);
    CHECK(req_state(keep) == NIFM_STATE_ACCEPTED && req_state(drop) == NIFM_STATE_ACCEPTED && g_connects == 2, "two requests accepted by the provider");
    CHECK(event_is_signaled(tel), "...the telemetry event fired");
    svc_call(drop, 2, NULL, 0, &c);
    uint32_t drop_ev = c.out_copy[0];
    event_clear_handle(drop_ev);
    event_clear_handle(tel);
    svc_call(gen, 33, NULL, 0, &c);
    CHECK(c.result == 0, "33 ConfirmSystemAvailability: available");
    int rel0 = g_releases;
    svc_call(gen, 23, NULL, 0, &c);
    CHECK(c.result == 0 && nifm_power == NIFM_POWER_ASLEEP, "23 PutToSleep");
    CHECK(req_state(keep) == NIFM_STATE_ACCEPTED, "...a request kept in sleep keeps its connection");
    CHECK(req_state(drop) == NIFM_STATE_FREE && req_result(drop) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED && g_releases == rel0 + 1, "...the other one is dropped (connection released)");
    CHECK(event_is_signaled(drop_ev) && event_is_signaled(tel), "...its event and the telemetry event fire");
    svc_call(gen, 21, NULL, 0, &c);
    CHECK(c.out[0] == 1, "...IsAnyInternetRequestAccepted counts the kept one");
    int con0 = g_connects;
    svc_call(drop, 4, NULL, 0, &c);
    CHECK(req_state(drop) == NIFM_STATE_FREE && req_result(drop) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED && g_connects == con0, "Submit while asleep: rejected without a connection attempt");
    svc_call(gen, 24, NULL, 0, &c);
    CHECK(nifm_power == NIFM_POWER_AWAKE, "24 WakeUp");
    svc_call(drop, 4, NULL, 0, &c);
    CHECK(req_state(drop) == NIFM_STATE_ACCEPTED && g_connects == con0 + 1, "...Submit connects again");

    /* ---- 42 NotifyConnectionStateChanged */
    svc_call(keep, 2, NULL, 0, &c);
    uint32_t keep_ev = c.out_copy[0];
    event_clear_handle(keep_ev);
    svc_call(gen, 42, NULL, 0, &c);
    CHECK(c.result == 0 && event_is_signaled(keep_ev) && req_state(keep) == NIFM_STATE_ACCEPTED, "42 NotifyConnectionStateChanged with the connection up: events fire, requests stay");
    g_ni.connected = 0;
    event_clear_handle(tel);
    svc_call(gen, 42, NULL, 0, &c);
    CHECK(req_state(keep) == NIFM_STATE_FREE && req_state(drop) == NIFM_STATE_FREE && req_result(keep) == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "42 with the connection gone: every accepted request is dropped");
    CHECK(event_is_signaled(tel), "...the telemetry event fires");
    svc_call(gen, 21, NULL, 0, &c);
    CHECK(c.out[0] == 0, "...no request accepted any more");
    g_ni.connected = 1;

    /* ---- 37 Shutdown */
    svc_call(keep, 4, NULL, 0, &c);
    CHECK(req_state(keep) == NIFM_STATE_ACCEPTED, "accepted again");
    svc_call(gen, 37, NULL, 0, &c);
    CHECK(c.result == 0 && req_state(keep) == NIFM_STATE_FREE && nifm_power == NIFM_POWER_SHUTDOWN, "37 Shutdown drops every connection, kept in sleep or not");
    svc_call(gen, 33, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "33 ConfirmSystemAvailability after Shutdown: not available");
    svc_call(gen, 24, NULL, 0, &c);
    svc_call(keep, 4, NULL, 0, &c);
    CHECK(nifm_power == NIFM_POWER_SHUTDOWN && req_state(keep) == NIFM_STATE_FREE, "...WakeUp does not bring nifm back; Submit is rejected");
    svc_call(gen, 23, NULL, 0, &c);
    CHECK(nifm_power == NIFM_POWER_SHUTDOWN, "...PutToSleep does not either");
    nifm_power = NIFM_POWER_AWAKE; /* the rest of the tests run awake */
    void *ks = keep->state, *ds = drop->state;
    keep->destroy(keep);
    drop->destroy(drop);
    int listed = 0;
    for (unsigned i = 0; i < NIFM_MAX_LIVE; i++) listed += nifm_live[i] == ks || nifm_live[i] == ds;
    CHECK(listed == 0, "closed requests leave the registry");
    use_null();
}

/* ---- bsd */
static int32_t g_ret, g_errno;
static void bsd_do(IpcObject *b, unsigned cmd, const void *in, uint32_t n, IpcCall *c) {
    svc_call(b, cmd, in, n, c);
    g_ret = c->out_size >= 8 ? SVC_OUT(c, int32_t, 0) : -99;
    g_errno = c->out_size >= 8 ? SVC_OUT(c, int32_t, 4) : -99;
}
static int new_eventfd(IpcObject *b, unsigned cmd, uint64_t init, uint32_t flags) {
    IpcCall c;
    uint8_t in[16] = {0};
    memcpy(in, &init, 8), memcpy(in + 8, &flags, 4);
    bsd_do(b, cmd, in, cmd == 39 ? 16 : 12, &c);
    return g_ret;
}
static int64_t ev_rd(IpcObject *b, int fd, uint64_t *v) {
    IpcCall c;
    memset(&c, 0, sizeof c);
    c.in = (const uint8_t *)&fd;
    c.in_size = 4;
    svc_set_out_buf(&c, v, 8);
    svc_call_c(b, 25, &c);
    g_ret = SVC_OUT(&c, int32_t, 0), g_errno = SVC_OUT(&c, int32_t, 4);
    return g_ret;
}
static int64_t ev_wr(IpcObject *b, int fd, uint64_t v, uint32_t len) {
    IpcCall c;
    memset(&c, 0, sizeof c);
    c.in = (const uint8_t *)&fd;
    c.in_size = 4;
    svc_set_in_buf(&c, &v, len);
    svc_call_c(b, 24, &c);
    g_ret = SVC_OUT(&c, int32_t, 0), g_errno = SVC_OUT(&c, int32_t, 4);
    return g_ret;
}
static int16_t poll1(IpcObject *b, int fd, int16_t events, int32_t timeout) {
    IpcCall c;
    uint8_t pin[8], pout[8];
    memcpy(pin, &fd, 4), memcpy(pin + 4, &events, 2), memset(pin + 6, 0, 2);
    uint32_t in[2] = {1, (uint32_t)timeout};
    memset(&c, 0, sizeof c);
    c.in = (const uint8_t *)in;
    c.in_size = 8;
    svc_set_in_buf(&c, pin, 8);
    svc_set_out_buf(&c, pout, 8);
    svc_call_c(b, 6, &c);
    g_ret = SVC_OUT(&c, int32_t, 0), g_errno = SVC_OUT(&c, int32_t, 4);
    int16_t rev;
    memcpy(&rev, pout + 6, 2);
    return rev;
}
static void set_nonblock(IpcObject *b, int fd) {
    IpcCall c;
    int32_t in[3] = {fd, 4, 4}; /* F_SETFL O_NONBLOCK */
    bsd_do(b, 20, in, 12, &c);
}
static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
typedef struct {
    IpcObject *b;
    int fd;
    uint64_t v;
} Writer;
static void *writer_thread(void *arg) {
    Writer *w = arg;
    struct timespec d = {0, 30 * 1000000};
    nanosleep(&d, NULL);
    ev_wr(w->b, w->fd, w->v, 8);
    return NULL;
}
static int g_shutdowns, g_last_how;
static int count_shutdown(void *ctx, int h, int how, int *err) {
    (void)ctx, (void)h, (void)err;
    g_shutdowns++;
    g_last_how = how;
    return 0;
}
static void test_bsd(void) {
    IpcCall c;
    IpcObject *b = bsd_open();
    use_null();
    bsdg_reset(bsd_g());
    online_null_reset();
    uint64_t v = 0;
    /* ---- EventFd: a counter */
    int fd = new_eventfd(b, 31, 5, 0);
    CHECK(fd >= 0 && g_errno == 0, "31 EventFd(5, 0): a descriptor (%d)", fd);
    CHECK(bsdg_is_eventfd(bsd_g(), fd), "...an event descriptor");
    CHECK(ev_rd(b, fd, &v) == 8 && v == 5 && g_errno == 0, "Read returns the counter (5)");
    set_nonblock(b, fd);
    CHECK(g_ret == 0, "Fcntl(F_SETFL, O_NONBLOCK) on it");
    {
        int32_t in[3] = {fd, 3, 0};
        bsd_do(b, 20, in, 12, &c);
        CHECK(g_ret == 4, "...F_GETFL reads O_NONBLOCK back");
    }
    CHECK(ev_rd(b, fd, &v) == -1 && g_errno == 11, "...a read of a zero counter: EAGAIN (11 in the guest's numbering)");
    CHECK(poll1(b, fd, ONLINE_POLLIN | ONLINE_POLLOUT, 0) == ONLINE_POLLOUT && g_ret == 1, "Poll: writable, not readable");
    CHECK(ev_wr(b, fd, 3, 8) == 8 && ev_wr(b, fd, 4, 8) == 8, "Write adds to the counter");
    CHECK(poll1(b, fd, ONLINE_POLLIN, 0) == ONLINE_POLLIN && g_ret == 1, "Poll: readable now");
    CHECK(ev_rd(b, fd, &v) == 8 && v == 7, "...Read returns the sum (7) and zeroes it");
    CHECK(ev_wr(b, fd, UINT64_MAX, 8) == -1 && g_errno == 22, "Write of UINT64_MAX: EINVAL");
    CHECK(ev_wr(b, fd, 1, 4) == -1 && g_errno == 22, "Write of fewer than 8 bytes: EINVAL");
    {
        uint64_t small = 0;
        IpcCall r;
        memset(&r, 0, sizeof r);
        r.in = (const uint8_t *)&fd;
        r.in_size = 4;
        svc_set_out_buf(&r, &small, 4);
        svc_call_c(b, 25, &r);
        CHECK(SVC_OUT(&r, int32_t, 0) == -1 && SVC_OUT(&r, int32_t, 4) == 22, "Read into fewer than 8 bytes: EINVAL");
    }
    {
        uint8_t addr[16] = {16, 2};
        IpcCall r;
        memset(&r, 0, sizeof r);
        r.in = (const uint8_t *)&fd;
        r.in_size = 4;
        svc_set_in_buf(&r, addr, sizeof addr);
        svc_call_c(b, 13, &r);
        CHECK(SVC_OUT(&r, int32_t, 0) == -1 && SVC_OUT(&r, int32_t, 4) == 88, "Bind on an event descriptor: ENOTSOCK (88)");
    }
    /* semaphore mode, through NoSfEventFd */
    int sfd = new_eventfd(b, 39, 2, BSD_EFD_SEMAPHORE | BSD_EFD_NONBLOCK);
    CHECK(sfd >= 0 && sfd != fd, "39 NoSfEventFd(2, semaphore | nonblock): another descriptor");
    CHECK(ev_rd(b, sfd, &v) == 8 && v == 1 && ev_rd(b, sfd, &v) == 8 && v == 1, "semaphore: each read returns 1");
    CHECK(ev_rd(b, sfd, &v) == -1 && g_errno == 11, "...then EAGAIN (the nonblock flag)");
    /* a full counter */
    int ffd = new_eventfd(b, 31, 0xfffffffffffffffeull, BSD_EFD_NONBLOCK_LINUX);
    CHECK(ffd >= 0 && ev_wr(b, ffd, 1, 8) == -1 && g_errno == 11, "a counter at its maximum: Write would block -> EAGAIN (Linux nonblock bit)");
    CHECK(poll1(b, ffd, ONLINE_POLLOUT, 0) == 0 && g_ret == 0, "...and Poll does not report it writable");
    CHECK(new_eventfd(b, 31, UINT64_MAX, 0) == -1 && g_errno == 22, "EventFd with initval UINT64_MAX: EINVAL");
    /* duplicate / close */
    {
        int32_t in = fd;
        bsd_do(b, 27, &in, 4, &c);
        int dfd = g_ret;
        CHECK(dfd >= 0 && dfd != fd, "DuplicateSocket of an event descriptor");
        ev_wr(b, fd, 9, 8);
        bsd_do(b, 26, &in, 4, &c);
        CHECK(g_ret == 0, "Close the original");
        CHECK(ev_rd(b, dfd, &v) == 8 && v == 9, "...the alias still reads the shared counter (9)");
        CHECK(ev_rd(b, fd, &v) == -1 && g_errno == 9, "...the closed number is EBADF");
        in = dfd;
        bsd_do(b, 26, &in, 4, &c);
        CHECK(g_ret == 0 && bsdg_handle(bsd_g(), dfd) == -1, "Close the alias: the descriptor is free");
    }
    /* blocking: a read and a poll wait for a write from another thread */
    {
        int bfd = new_eventfd(b, 31, 0, 0);
        Writer w = {b, bfd, 42};
        pthread_t t;
        pthread_create(&t, NULL, writer_thread, &w);
        int64_t t0 = now_ms();
        CHECK(ev_rd(b, bfd, &v) == 8 && v == 42 && now_ms() - t0 >= 20, "a blocking Read waits for the writer (42 after %lld ms)", (long long)(now_ms() - t0));
        pthread_join(t, NULL);
        w.v = 7;
        pthread_create(&t, NULL, writer_thread, &w);
        t0 = now_ms();
        int16_t rev = poll1(b, bfd, ONLINE_POLLIN, -1);
        CHECK(rev == ONLINE_POLLIN && g_ret == 1 && now_ms() - t0 >= 20, "Poll(-1) wakes when the other thread writes");
        pthread_join(t, NULL);
        ev_rd(b, bfd, &v);
        t0 = now_ms();
        rev = poll1(b, bfd, ONLINE_POLLIN, 40);
        int64_t el = now_ms() - t0;
        CHECK(rev == 0 && g_ret == 0 && el >= 35 && el < 1000, "Poll(40 ms) of a zero counter times out (%lld ms)", (long long)el);
        /* a mixed poll: a socket and the event descriptor */
        int32_t sa[3] = {2, 2, 0};
        bsd_do(b, 2, sa, 12, &c);
        int sock = g_ret;
        ev_wr(b, bfd, 1, 8);
        uint8_t pin[16], pout[16];
        int16_t evin = ONLINE_POLLIN, z = 0;
        memcpy(pin, &sock, 4), memcpy(pin + 4, &evin, 2), memcpy(pin + 6, &z, 2);
        memcpy(pin + 8, &bfd, 4), memcpy(pin + 12, &evin, 2), memcpy(pin + 14, &z, 2);
        uint32_t pa[2] = {2, 100};
        memset(&c, 0, sizeof c);
        c.in = (const uint8_t *)pa;
        c.in_size = 8;
        svc_set_in_buf(&c, pin, 16);
        svc_set_out_buf(&c, pout, 16);
        svc_call_c(b, 6, &c);
        int16_t r_sock, r_ev;
        memcpy(&r_sock, pout + 6, 2), memcpy(&r_ev, pout + 14, 2);
        CHECK(sock >= 0 && SVC_OUT(&c, int32_t, 0) == 1 && r_ev == ONLINE_POLLIN && r_sock == 0, "Poll over a socket and an event descriptor: the event is ready");
    }
    /* ShutdownAllSockets(how) closes nothing */
    {
        OnlineProvider p = *online_null_provider();
        p.sock_shutdown = count_shutdown;
        online_provider_install(&p);
        bsdg_reset(bsd_g());
        int32_t sa[3] = {2, 1, 0};
        bsd_do(b, 2, sa, 12, &c);
        int s1 = g_ret;
        bsd_do(b, 2, sa, 12, &c);
        int s2 = g_ret;
        int32_t in = s1;
        bsd_do(b, 27, &in, 4, &c);
        int e1 = new_eventfd(b, 31, 1, BSD_EFD_NONBLOCK);
        int32_t how = 2;
        g_shutdowns = 0;
        bsd_do(b, 40, &how, 4, &c);
        CHECK(g_ret == 0 && g_errno == 0, "40 ShutdownAllSockets(2): {0, 0}");
        CHECK(g_shutdowns == 2 && g_last_how == 2, "...shutdown(2) once per provider socket (a duplicate shares its socket; %d calls)", g_shutdowns);
        CHECK(bsdg_handle(bsd_g(), s1) >= 0 && bsdg_handle(bsd_g(), s2) >= 0 && ev_rd(b, e1, &v) == 8 && v == 1, "...the descriptors stay open, the event descriptor untouched");
        how = 3;
        bsd_do(b, 40, &how, 4, &c);
        CHECK(g_ret == -1 && g_errno == 22, "ShutdownAllSockets(3): EINVAL");
        bsdg_reset(bsd_g());
        CHECK(bsdg_handle(bsd_g(), e1) == -1 && !bsd_g()->ev[0].used && !bsd_g()->ev[1].used, "bsdg_reset frees the event descriptors too");
        use_null();
    }
    /* RegisterClientShared, the NoMem mmsg calls, the core mask */
    uint32_t before = bsd_clients;
    uint8_t reg[0x30] = {0};
    svc_call(b, 33, reg, sizeof reg, &c);
    CHECK(c.result == 0 && c.out_size == 4 && SVC_OUT(&c, uint32_t, 0) == 0 && bsd_clients == before + 1, "33 RegisterClientShared: registered like RegisterClient (0)");
    int32_t bad[3] = {77, 1, 0};
    bsd_do(b, 42, bad, 12, &c);
    CHECK(g_ret == -1 && g_errno == 9, "42 SendMMsgNoMem on an unknown descriptor: EBADF");
    bsd_do(b, 43, bad, 12, &c);
    CHECK(g_ret == -1 && g_errno == 9, "43 RecvMMsgNoMem on an unknown descriptor: EBADF");
    {
        int32_t sa[3] = {2, 2, 0};
        bsd_do(b, 2, sa, 12, &c);
        int32_t ok[3] = {g_ret, 0, 0};
        bsd_do(b, 43, ok, 12, &c);
        CHECK(g_ret == 0 && g_errno == 0, "43 RecvMMsgNoMem with no messages on a socket: 0");
    }
    svc_call(b, 201, NULL, 0, &c);
    CHECK(c.out_size == 8 && SVC_OUT(&c, uint64_t, 0) == 8, "201 GetThreadCoreMask: core 3 before any Set");
    uint64_t mask = 0x3;
    svc_call(b, 200, &mask, 8, &c);
    svc_call(b, 201, NULL, 0, &c);
    CHECK(SVC_OUT(&c, uint64_t, 0) == 3, "200 SetThreadCoreMask reads back through 201");
    const unsigned real[] = {31, 33, 39, 40, 42, 43, 200, 201}, stub[] = {32, 34, 35, 37};
    for (unsigned i = 0; i < sizeof real / sizeof real[0]; i++) CHECK(svc_find(b, real[i])->status == IPC_IMPL, "bsd %u is real", real[i]);
    for (unsigned i = 0; i < sizeof stub / sizeof stub[0]; i++) CHECK(svc_find(b, stub[i])->status == IPC_STUB, "bsd %u stays a documented default", stub[i]);
    bsdg_reset(bsd_g());
}

/* ---- nsd application server environment type, prepo rows kept as defaults */
static void test_nsd_prepo(void) {
    IpcCall c;
    IpcObject *n = nsd_open();
    svc_call(n, 100, NULL, 0, &c);
    CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == 1, "nsd 100 GetApplicationServerEnvironmentType: lp (1), from the environment identifier lp1");
    uint8_t sd = 2;
    svc_call(n, 101, &sd, 1, &c);
    CHECK(c.result == 0 && c.out_size == 0, "nsd 101 SetApplicationServerEnvironmentType(sd): no output");
    svc_call(n, 100, NULL, 0, &c);
    CHECK(c.out[0] == 2, "...100 reads back the configured type");
    IpcObject *n2 = nsd_open();
    svc_call(n2, 100, NULL, 0, &c);
    CHECK(c.out[0] == 2, "...the setting belongs to the service, not the session");
    svc_call(n, 102, NULL, 0, &c);
    svc_call(n, 100, NULL, 0, &c);
    CHECK(c.result == 0 && c.out[0] == 1, "nsd 102 DeleteApplicationServerEnvironmentType: back to the identifier's type");
    for (unsigned id = 100; id <= 102; id++) CHECK(svc_find(n, id)->status == IPC_IMPL, "nsd %u is real", id);
    { uint8_t url[0x100]; memset(url, 0xcc, sizeof url); IpcCall u; memset(&u, 0, sizeof u); svc_set_out_buf(&u, url, sizeof url); svc_call_c(n, 5, &u);
      CHECK(u.result == 0 && url[0] == 0 && url[0xff] == 0, "nsd 5 GetSettingUrl: success, the empty Url of the lp1 settings"); }
    CHECK(svc_find(n, 5)->status == IPC_IMPL && svc_find(n, 64)->status == IPC_STUB, "nsd 5 real, 64 stays a documented default");
    IpcObject *pr = prepo_open();
    CHECK(svc_find(pr, 50100)->status == IPC_STUB && svc_find(pr, 90100)->status == IPC_STUB, "prepo 50100 / 90100 stay documented defaults");
}

int main(void) {
    IpcCall c;
    test_bsd();
    test_nsd_prepo();
    IpcObject *nf = nifm_open();
    svc_call(nf, 5, NULL, 0, &c);
    IpcObject *gen = c.out_objects[0];
    test_nifm_profiles(gen);
    test_nifm_scan_ap(gen);
    test_nifm_power(gen);
    const unsigned gen_real[] = {2, 5, 7, 8, 9, 10, 14, 23, 24, 31, 33, 35, 36, 37, 42, 44, 47};
    for (unsigned i = 0; i < sizeof gen_real / sizeof gen_real[0]; i++) CHECK(svc_find(gen, gen_real[i])->status == IPC_IMPL, "IGeneralService %u is real", gen_real[i]);
    const unsigned gen_stub[] = {6, 25, 38, 39};
    for (unsigned i = 0; i < sizeof gen_stub / sizeof gen_stub[0]; i++) CHECK(svc_find(gen, gen_stub[i])->status == IPC_STUB, "IGeneralService %u stays a documented default", gen_stub[i]);
    printf("{\"test\":\"svc_net_r10\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
