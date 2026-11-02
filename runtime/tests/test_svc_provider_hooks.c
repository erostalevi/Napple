/* Unit tests: ipc-provider-hooks r11, phase 1: friend:u 10110 / 10111 (profile picture), 10200 (friend request), 11000 / 11001 (picture URL) and nifm
 * IRequest 22 / 26 answer through OnlineProvider hooks; the NullProvider answers what an offline console does. offline.c is #included. */
#include "../src/services/offline.c"
#include "svc_test.h"

static uint8_t g_picture[5] = {0xff, 0xd8, 0x01, 0xff, 0xd9};
static uint64_t g_sent_to;
static uint32_t g_img_size;
static int g_connected;
static int fk_net_info(void *ctx, OnlineNetInfo *o) {
    (void)ctx;
    memset(o, 0, sizeof *o);
    o->connected = (uint8_t)g_connected, o->kind = 2, o->strength = 3, o->ip = 0xc0a80105, o->gateway = 0xc0a80101, o->dns1 = 0x08080808;
    return 0;
}
static uint32_t fk_connect(void *ctx) { (void)ctx; return 0; }
static void fk_release(void *ctx) { (void)ctx; }
static uint32_t fk_image(void *ctx, uint64_t id, uint32_t size, void *out, uint32_t max) {
    (void)ctx;
    g_img_size = size;
    if (id != 0x42 || max < sizeof g_picture) return 0;
    memcpy(out, g_picture, sizeof g_picture);
    return sizeof g_picture;
}
static int fk_url(void *ctx, const char *in, uint32_t size, char *out, size_t n) {
    (void)ctx;
    if (!*in) return 1;
    snprintf(out, n, "%s?s=%u", in, size);
    return 0;
}
static uint32_t fk_send(void *ctx, uint64_t id) {
    (void)ctx;
    g_sent_to = id;
    return id == 0xbad ? HZ_RESULT(HZ_MOD_FRIENDS, 3) : 0;
}
static uint32_t fk_count(void *ctx) { (void)ctx; return 0; }
static const OnlineProvider fake = {
    .abi_version = ONLINE_ABI_VERSION, .struct_size = sizeof(OnlineProvider), .name = "hooks (test)",
    .caps = ONLINE_CAP_NETWORK | ONLINE_CAP_FRIENDS, .net_info = fk_net_info, .net_connect = fk_connect, .net_release = fk_release, .friend_count = fk_count,
    .friend_profile_image = fk_image, .friend_image_url = fk_url, .friend_send_request = fk_send,
};

int main(void) {
    IpcCall c;
    const uint32_t NSA = HZ_R_FRIENDS_NETWORK_SERVICE_ACCOUNT_UNAVAILABLE;
    IpcObject *fo = friend_open();
    svc_call(fo, 0, NULL, 0, &c);
    IpcObject *fr = c.out_objects[0];
    uint8_t in[0x20] = {0}, img[64], url_in[0xa0] = "https://img.example/f/42.jpg", url_out[0xa0];
    memcpy(in, g_user_id, 16);
    uint64_t id = 0x42;
    memcpy(in + 0x10, &id, 8);

    /* ---- NullProvider: the offline answers */
    online_provider_install(NULL);
    for (unsigned cmd = 10110; cmd <= 10111; cmd++) {
        svc_call(fr, cmd, in, 0x20, &c);
        CHECK(c.result == NSA, "%u offline: network service account unavailable", cmd);
    }
    svc_call(fr, 10200, in, 0x18, &c);
    CHECK(c.result == NSA, "10200 offline");
    for (unsigned cmd = 11000; cmd <= 11001; cmd++) {
        svc_call(fr, cmd, in, 4, &c);
        CHECK(c.result == NSA, "%u offline", cmd);
    }

    /* ---- a provider with the hooks */
    online_provider_install(&fake);
    memset(&c, 0, sizeof c), c.in = in, c.in_size = 0x20;
    memset(img, 0, sizeof img);
    svc_set_out_buf(&c, img, sizeof img);
    memcpy(in + 0x18, &(uint32_t){256}, 4);
    svc_call_c(fr, 10111, &c);
    CHECK(c.result == 0 && c.out_size == 4 && SVC_OUT(&c, uint32_t, 0) == 5 && !memcmp(img, g_picture, 5) && g_img_size == 256, "10111 picture + size 256");
    memset(&c, 0, sizeof c), c.in = in, c.in_size = 0x20;
    svc_set_out_buf(&c, img, sizeof img);
    svc_call_c(fr, 10110, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 5 && g_img_size == 0, "10110 uses the provider's default size");
    uint64_t other = 0x43;
    memcpy(in + 0x10, &other, 8);
    memset(&c, 0, sizeof c), c.in = in, c.in_size = 0x20;
    svc_set_out_buf(&c, img, sizeof img);
    svc_call_c(fr, 10110, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0, "not a friend: size 0");

    svc_call(fr, 10200, in, 0x18, &c);
    CHECK(c.result == 0 && g_sent_to == 0x43, "10200 forwards the account id");
    uint64_t bad = 0xbad;
    memcpy(in + 0x10, &bad, 8);
    svc_call(fr, 10200, in, 0x18, &c);
    CHECK(c.result == HZ_RESULT(HZ_MOD_FRIENDS, 3), "10200 passes the provider's result");

    for (unsigned cmd = 11000; cmd <= 11001; cmd++) {
        uint32_t sz = 128;
        memset(&c, 0, sizeof c), c.in = (const uint8_t *)&sz, c.in_size = 4;
        svc_set_in_buf(&c, url_in, sizeof url_in);
        memset(url_out, 0, sizeof url_out);
        svc_set_out_buf(&c, url_out, sizeof url_out);
        svc_call_c(fr, cmd, &c);
        CHECK(c.result == 0 && !strcmp((char *)url_out, "https://img.example/f/42.jpg?s=128"), "%u rewrites the URL for the size", cmd);
    }
    {
        uint32_t sz = 128;
        char empty[0xa0] = {0};
        memset(&c, 0, sizeof c), c.in = (const uint8_t *)&sz, c.in_size = 4;
        svc_set_in_buf(&c, empty, sizeof empty);
        svc_set_out_buf(&c, url_out, sizeof url_out);
        svc_call_c(fr, 11000, &c);
        CHECK(c.result == NSA, "11000 without a URL: unavailable");
    }
    const unsigned rows[] = {10110, 10111, 10200, 11000, 11001};
    for (unsigned i = 0; i < 5; i++) CHECK(svc_find(fr, rows[i])->status == IPC_IMPL, "friend %u is real", rows[i]);

    /* ---- nifm IRequest 22 / 26 */
    OnlineProvider nonet = fake;
    nonet.caps = ONLINE_CAP_FRIENDS;
    online_provider_install(NULL);
    {
        IpcObject *nf = nifm_open();
        svc_call(nf, 5, NULL, 0, &c);
        IpcObject *gen = c.out_objects[0];
        svc_call(gen, 4, NULL, 0, &c);
        IpcObject *rq = c.out_objects[0];
        svc_call(rq, 4, NULL, 0, &c);
        svc_call(rq, 22, NULL, 0, &c);
        CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "22 offline: the request's failure");
        svc_call(rq, 26, NULL, 0, &c);
        CHECK(c.result == 0 && c.out_size == 4 && SVC_OUT(&c, uint32_t, 0) == 0, "26 offline: not accessible");

        g_connected = 1;
        online_provider_install(&fake);
        svc_call(gen, 4, NULL, 0, &c);
        IpcObject *rq2 = c.out_objects[0];
        svc_call(rq2, 22, NULL, 0, &c);
        CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "22 before Submit: none");
        svc_call(rq2, 4, NULL, 0, &c);
        svc_call(rq2, 0, NULL, 0, &c);
        CHECK(SVC_OUT(&c, uint32_t, 0) == NIFM_STATE_ACCEPTED, "the connected provider accepts the request");
        svc_call(rq2, 22, NULL, 0, &c);
        CHECK(c.result == 0 && c.out_size == 0x10 && c.out[0] == 2 && c.out[1] == 3 && SVC_OUT(&c, uint32_t, 4) == 0xc0a80105 &&
                  SVC_OUT(&c, uint32_t, 8) == 0xc0a80101 && SVC_OUT(&c, uint32_t, 12) == 0x08080808, "22 connected: the interface record");
        svc_call(rq2, 26, NULL, 0, &c);
        CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 1, "26 connected: accessible");
        CHECK(svc_find(rq2, 22)->status == IPC_IMPL && svc_find(rq2, 26)->status == IPC_IMPL, "22 and 26 are real");
        g_connected = 0;
        online_provider_install(NULL);
    }
    online_provider_install(NULL);
    printf("{\"test\":\"svc_provider_hooks\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
