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

int main(void) {
    IpcCall c;
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
