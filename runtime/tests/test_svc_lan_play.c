/* Unit tests: mk8d-lan-session (2026-11-02-r7), what LAN / local-wireless play needs from the services:
 *  - bsd answers with the console's (Linux) errno numbers: MK8D's Pia compares GetLastErrno() with 11 (would block), 100 (network down), 9; FreeBSD's 35
 *    for EAGAIN made Pia's receive thread stop at its first empty read and the session end with 2618-0006;
 *  - nifm GetCurrentIpConfigInfo reports the provider's interface (IpConfigInfo, in_addr fields);
 *  - GetPseudoDeviceId is a stable UUID per installation + application + user, different between installations;
 *  - friend GetPlayHistoryRegistrationKey gives a 0x40-byte key (MK8D ends a hosted local room when it fails). */
#include "../src/services/offline.c"
#include "svc_test.h"
#include "install_id_core.h"
#include <arpa/inet.h>
#include <unistd.h>

static int fake_info(void *ctx, OnlineNetInfo *o) {
    (void)ctx;
    memset(o, 0, sizeof *o);
    o->connected = 1, o->kind = 2, o->strength = 3;
    o->ip = 0x0a0d0002, o->mask = 0xffff0000, o->gateway = 0x0a0d0001, o->dns1 = 0x01010101, o->dns2 = 0x08080808;
    return 0;
}

int main(void) {
    IpcCall c;
    /* ---- errno numbering */
    CHECK(bsd_errno_to_guest(HZ_EAGAIN) == 11 && bsd_errno_to_guest(HZ_EINPROGRESS) == 115 && bsd_errno_to_guest(HZ_ECONNREFUSED) == 111 &&
              bsd_errno_to_guest(HZ_ENETDOWN) == 100 && bsd_errno_to_guest(HZ_ENOTCONN) == 107 && bsd_errno_to_guest(HZ_ETIMEDOUT) == 110 &&
              bsd_errno_to_guest(HZ_EADDRINUSE) == 98 && bsd_errno_to_guest(HZ_EMSGSIZE) == 90 && bsd_errno_to_guest(HZ_ECONNRESET) == 104,
          "FreeBSD -> Linux errno for the socket errors");
    CHECK(bsd_errno_to_guest(0) == 0 && bsd_errno_to_guest(HZ_EBADF) == 9 && bsd_errno_to_guest(HZ_EINVAL) == 22 && bsd_errno_to_guest(HZ_EPIPE) == 32 &&
              bsd_errno_to_guest(4242) == 4242,
          "1..34 are shared; unknown values pass through");
    {
        IpcObject *bs = bsd_open();
        bsdg_reset(bsd_g());
        online_null_reset();
        int32_t in[3] = {2, 1, 0}, fd = 0;
        svc_call(bs, 2, in, sizeof in, &c);
        svc_call(bs, 14, &fd, 4, &c);
        CHECK(SVC_OUT(&c, int32_t, 0) == -1 && SVC_OUT(&c, int32_t, 4) == 100, "an IPC reply carries the guest's number (ENETDOWN = 100)");
    }

    /* ---- nifm GetCurrentIpConfigInfo */
    IpcObject *nf = nifm_open();
    svc_call(nf, 5, NULL, 0, &c);
    IpcObject *gen = c.out_objects[0];
    svc_call(gen, 15, NULL, 0, &c);
    CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "15 without a network: not connected");
    {
        OnlineProvider fake = *online_null_provider();
        fake.net_info = fake_info;
        const OnlineProvider *prev = online_provider_install(&fake);
        svc_call(gen, 15, NULL, 0, &c);
        uint32_t ip, mask, gw, d1, d2;
        memcpy(&ip, c.out + 1, 4), memcpy(&mask, c.out + 5, 4), memcpy(&gw, c.out + 9, 4), memcpy(&d1, c.out + 14, 4), memcpy(&d2, c.out + 18, 4);
        CHECK(c.result == 0 && c.out_size == 0x16 && c.out[0] == 1 && c.out[13] == 1, "15: IpConfigInfo, 0x16 bytes, automatic address and DNS");
        CHECK(ntohl(ip) == 0x0a0d0002 && ntohl(mask) == 0xffff0000 && ntohl(gw) == 0x0a0d0001 && ntohl(d1) == 0x01010101 && ntohl(d2) == 0x08080808,
              "15: address, mask, gateway, DNS as in_addr (network order)");
        online_provider_install(prev == online_null_provider() ? NULL : prev);
    }

    /* ---- installation id and GetPseudoDeviceId */
    char root1[256], root2[256];
    snprintf(root1, sizeof root1, "/tmp/napple-test-install-%d-a", (int)getpid());
    snprintf(root2, sizeof root2, "/tmp/napple-test-install-%d-b", (int)getpid());
    uint8_t i1[16], i1b[16], i2[16];
    CHECK(install_id_get(root1, i1) == 0 && install_id_get(root1, i1b) == 0 && !memcmp(i1, i1b, 16), "an installation keeps its id across calls");
    CHECK(install_id_get(root2, i2) == 0 && memcmp(i1, i2, 16), "another installation has another id");
    char path[300];
    snprintf(path, sizeof path, "%s/system/installation-id", root1);
    CHECK(access(path, R_OK) == 0, "the id is stored in <root>/system/installation-id");
    static const uint8_t user[16] = {1, 2, 3}, user2[16] = {4, 5, 6};
    uint8_t p1[16], p1b[16], p2[16], pu[16], pa[16];
    install_pseudo_device_id(i1, 0x0100152000022000ull, user, p1);
    install_pseudo_device_id(i1b, 0x0100152000022000ull, user, p1b);
    install_pseudo_device_id(i2, 0x0100152000022000ull, user, p2);
    install_pseudo_device_id(i1, 0x0100152000022000ull, user2, pu);
    install_pseudo_device_id(i1, 0x0100000000010000ull, user, pa);
    CHECK(!memcmp(p1, p1b, 16) && memcmp(p1, p2, 16) && memcmp(p1, pu, 16) && memcmp(p1, pa, 16), "pseudo device id: stable, and different per installation, user, title");
    CHECK((p1[6] >> 4) == 4 && (p1[8] & 0xc0) == 0x80, "pseudo device id: a version-4 UUID");

    /* ---- friend GetPlayHistoryRegistrationKey */
    g_cfg.save_root = root1;
    IpcObject *fr = friend_open();
    svc_call(fr, 0, NULL, 0, &c);
    IpcObject *fs = c.out_objects[0];
    uint8_t kin[24] = {1, 0, 0, 0, 0, 0, 0, 0};
    memcpy(kin + 8, user, 16);
    uint8_t key[0x40];
    memset(&c, 0, sizeof c);
    c.in = kin, c.in_size = sizeof kin;
    svc_set_out_buf(&c, key, sizeof key);
    svc_call_c(fs, 10700, &c);
    static const uint8_t zero32[32];
    CHECK(c.result == 0 && key[0] == 1 && key[1] == 1 && key[2] < 8 && key[4] == 1 && !memcmp(key + 0x10, user, 16) && memcmp(key + 0x20, zero32, 32),
          "10700: type 0x101, key index, local flag, user id, keyed hash");
    uint8_t zin[24] = {0};
    memset(&c, 0, sizeof c);
    c.in = zin, c.in_size = sizeof zin;
    svc_set_out_buf(&c, key, sizeof key);
    svc_call_c(fs, 10700, &c);
    CHECK(c.result != 0 && HZ_MODULE(c.result) == 121, "10700 with no user: a friends error");

    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", root1, root2);
    if (system(cmd)) {}
    printf("test_svc_lan_play: %d checks, %d failed\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
