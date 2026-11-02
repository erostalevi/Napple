/* Napple Online client core (runtime/src/online/) through the overlay function table, against a fake account + social server on localhost:
 * sign-up/verify/login results, the state getters (account, profiles, devices, friends with presence, requests, stats), friend and tier actions,
 * the change callback, the strong access-token / profile-id hooks the friend service uses, and the shared credentials file (0600, no secrets in it). */
#include "check.h"
#include "online_ui.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int fake_fd;
static int verified, accept_calls, tier_calls;

static const char *route(const char *req, int *code) {
    *code = 200;
    #define IS(m, p) (!strncmp(req, m " " p, strlen(m " " p)))
    if (IS("POST", "/account/v1/auth/signup")) { *code = 202; return "{}"; }
    if (IS("POST", "/account/v1/auth/login") && !verified) { *code = 403; return "{\"reason\":\"unverified\"}"; }
    if (IS("POST", "/account/v1/auth/verify") || IS("POST", "/account/v1/auth/login")) {
        verified = 1;
        return "{\"access_token\":\"acc.tok.sig\",\"refresh_token\":\"r-secret-1\",\"expires_in\":900,\"account_id\":\"acct-1\",\"device_id\":\"dev-1\",\"key_state\":\"bootstrap\"}";
    }
    if (IS("POST", "/account/v1/keys/grant")) return "{}";
    if (IS("GET", "/account/v1/me")) return "{\"email\":\"neto@example.com\",\"email_verified\":true}";
    if (IS("GET", "/account/v1/profiles")) return "{\"profiles\":[{\"profile_id\":\"p-1\",\"nickname\":\"Neto\",\"bio\":\"hi\",\"icon_id\":3,\"tier\":2,\"friend_code\":\"NP-123456789012\"},{\"profile_id\":\"p-2\",\"nickname\":\"Alt\"}]}";
    if (IS("GET", "/account/v1/devices")) return "{\"devices\":[{\"device_id\":\"dev-1\",\"name\":\"Mac\",\"platform\":\"macos\"}]}";
    if (IS("GET", "/account/v1/keys/requests")) return "{\"requests\":[{\"device_id\":\"dev-2\",\"name\":\"iPad\",\"platform\":\"ios\"}]}";
    if (IS("GET", "/social/v1/friends")) return "{\"friends\":[{\"profile\":{\"profile_id\":\"p-9\",\"nickname\":\"Pal\",\"icon_id\":5},\"presence\":{\"status\":\"playing\",\"title_id\":\"0100152000022000\",\"mode\":\"online\",\"joinable\":true,\"join_token\":\"np1:tok\"}}]}";
    if (IS("GET", "/social/v1/friend-requests")) return "{\"incoming\":[{\"request_id\":\"rq1\",\"from\":{\"profile_id\":\"p-7\",\"nickname\":\"Req\"},\"to\":{\"profile_id\":\"p-1\"}}],\"outgoing\":[]}";
    if (IS("POST", "/social/v1/friend-requests/rq1/accept")) { accept_calls++; return "{\"friend\":{}}"; }
    if (IS("GET", "/social/v1/profiles/")) return "{\"stats\":[{\"title_id\":\"0100152000022000\",\"key\":\"wins\",\"value\":7}]}";
    if (IS("GET", "/social/v1/blocks") || IS("GET", "/social/v1/recent")) return "{}";
    if (IS("PATCH", "/account/v1/profiles/p-1")) { tier_calls++; return "{\"tier\":4}"; }
    *code = 404;
    return "{\"error\":\"nope\"}";
}

static void *server(void *arg) {
    (void)arg;
    for (;;) {
        int c = accept(fake_fd, NULL, NULL);
        if (c < 0) return NULL;
        char buf[8192];
        size_t n = 0;
        for (;;) {
            ssize_t r = recv(c, buf + n, sizeof buf - 1 - n, 0);
            if (r <= 0) break;
            n += (size_t)r;
            buf[n] = 0;
            char *body = strstr(buf, "\r\n\r\n");
            if (!body) continue;
            const char *cl = strcasestr(buf, "content-length:");
            size_t want = cl ? (size_t)atoi(cl + 15) : 0;
            if (n - (size_t)(body + 4 - buf) >= want) break;
        }
        int code;
        const char *out = route(buf, &code);
        char head[256];
        int h = snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", code, strlen(out));
        send(c, head, (size_t)h, 0);
        send(c, out, strlen(out), 0);
        close(c);
    }
}

typedef struct { int n, code; uint64_t id; char json[2048]; } Done;
static void on_done(void *user, uint64_t id, int code, const char *json) {
    Done *d = user;
    d->n++, d->code = code, d->id = id;
    snprintf(d->json, sizeof d->json, "%s", json);
}
static int changes;
static void on_change(void *user) { (void)user; changes++; }

/* Pumps until `d` got its completion (the worker runs the request). */
static void wait_done(Done *d) {
    for (int i = 0; i < 400 && !d->n; i++) {
        napple_online_ui_pump();
        usleep(10000);
    }
}

int main(void) {
    fake_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(fake_fd, (void *)&a, sizeof a) == 0, "bind");
    listen(fake_fd, 8);
    socklen_t al = sizeof a;
    getsockname(fake_fd, (void *)&a, &al);
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%u", ntohs(a.sin_port));
    pthread_t th;
    pthread_create(&th, NULL, server, NULL);

    char home[256];
    snprintf(home, sizeof home, "%s/napple-online-ui-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid());
    setenv("NAPPLE_ACCOUNT_STORE", "file", 1);
    setenv("NAPPLE_ONLINE_POLL_MS", "100000", 1);

    const napple_online_ui_api *api = napple_online_runtime_start(home, url, url, "00112233445566778899aabbccddeeff");
    CHECK(api && api->version == NAPPLE_ONLINE_UI_VERSION, "table");
    api->set_change_callback(api->ctx, on_change, NULL);
    napple_online_account acc;
    api->get_account(api->ctx, &acc);
    CHECK(!acc.signed_in, "signed out before login");
    char tok[64];
    CHECK(napple_account_access_token(tok, sizeof tok) == 0, "no token when signed out");

    Done d = {0};
    uint64_t id = api->signup(api->ctx, "neto@example.com", "correct horse 1", "Neto", 1, 1, on_done, &d);
    CHECK(id > 0, "request id");
    wait_done(&d);
    CHECK(d.n == 1 && d.id == id && d.code == NAPPLE_ONLINE_OK, "signup completes ok (code %d)", d.code);

    memset(&d, 0, sizeof d);
    api->login(api->ctx, "neto@example.com", "pw", on_done, &d);
    wait_done(&d);
    CHECK(d.code == NAPPLE_ONLINE_ERR_UNVERIFIED && strstr(d.json, "unverified"), "unverified login: %d %s", d.code, d.json);

    memset(&d, 0, sizeof d);
    api->verify(api->ctx, "neto@example.com", "123456", on_done, &d);
    wait_done(&d);
    CHECK(d.code == NAPPLE_ONLINE_OK, "verify ok (%d %s)", d.code, d.json);
    for (int i = 0; i < 100 && !changes; i++) { napple_online_ui_pump(); usleep(10000); }
    CHECK(changes > 0, "change callback fired");

    api->get_account(api->ctx, &acc);
    CHECK(acc.signed_in && !strcmp(acc.email, "neto@example.com") && acc.email_verified && acc.server_reachable, "account");
    CHECK(!strcmp(acc.current.id, "p-1") && !strcmp(acc.current.nickname, "Neto") && acc.current.tier == 2 && acc.current.icon_id == 3, "current profile = first, automatic");
    CHECK(!strcmp(acc.current.friend_code, "NP-123456789012") && acc.profile_count == 2, "friend code / profile count");

    napple_online_profile ps[NAPPLE_ONLINE_MAX_PROFILES];
    CHECK(api->get_profiles(api->ctx, ps, 8) == 2 && !strcmp(ps[1].nickname, "Alt"), "profiles");
    napple_online_device dv[4];
    size_t nd = api->get_devices(api->ctx, dv, 4);
    CHECK(nd == 2 && dv[0].current && !dv[0].pending && dv[1].pending && !strcmp(dv[1].id, "dev-2"), "devices incl. pending approval (%zu)", nd);
    napple_online_friend fr[4];
    CHECK(api->get_friends(api->ctx, fr, 4) == 1 && fr[0].online && !strcmp(fr[0].nickname, "Pal") && !strcmp(fr[0].title_id, "0100152000022000") &&
              !strcmp(fr[0].joinable_token, "np1:tok"), "friend with presence");
    napple_online_request_entry rq[4];
    CHECK(api->get_requests(api->ctx, rq, 4) == 1 && rq[0].incoming && !strcmp(rq[0].profile_id, "p-7"), "incoming request");
    napple_online_stat st[4];
    CHECK(api->get_stats(api->ctx, st, 4) == 1 && st[0].value == 7, "stats");
    CHECK(api->get_friends(api->ctx, NULL, 0) == 1, "count only");

    { size_t tn = napple_account_access_token(tok, sizeof tok - 1); tok[tn] = 0; CHECK(tn > 0 && !strcmp(tok, "acc.tok.sig"), "friend service token hook (%zu)", tn); }
    char pid[64];
    CHECK(napple_account_profile_id(pid, sizeof pid) > 0 && !strcmp(pid, "p-1"), "friend service profile hook");

    memset(&d, 0, sizeof d);
    api->friend_accept(api->ctx, "p-7", on_done, &d);
    wait_done(&d);
    CHECK(d.code == NAPPLE_ONLINE_OK && accept_calls == 1, "friend_accept hit the service");
    memset(&d, 0, sizeof d);
    api->set_tier(api->ctx, "p-1", 4, on_done, &d);
    wait_done(&d);
    CHECK(d.code == NAPPLE_ONLINE_OK && tier_calls == 1, "set_tier");
    memset(&d, 0, sizeof d);
    api->set_tier(api->ctx, "p-1", 9, on_done, &d);
    wait_done(&d);
    CHECK(d.code == NAPPLE_ONLINE_ERR_INVALID, "bad tier rejected locally");

    char path[400];
    snprintf(path, sizeof path, "%s/online/credentials.json", home);
    struct stat sb;
    CHECK(stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0600, "credentials file is 0600");
    FILE *f = fopen(path, "r");
    char raw[4096] = "";
    if (f) { size_t n = fread(raw, 1, sizeof raw - 1, f); raw[n] = 0; fclose(f); }
    CHECK(raw[0] && !strstr(raw, "r-secret-1"), "no plain refresh token in the file");
    snprintf(path, sizeof path, "%s/online/profile_map.json", home);
    CHECK(stat(path, &sb) == 0, "profile map written");

    memset(&d, 0, sizeof d);
    api->logout(api->ctx, on_done, &d);
    wait_done(&d);
    api->get_account(api->ctx, &acc);
    CHECK(d.code == NAPPLE_ONLINE_OK && !acc.signed_in && napple_account_access_token(tok, sizeof tok) == 0, "logout");

    napple_online_runtime_stop();
    close(fake_fd);
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
