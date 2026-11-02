/* Napple online: friends and presence from napple-social (see net_online.h). One Napple player profile per run: its friend list is polled into a
 * cache that friend:u reads, and the running title is published as the profile's presence.
 *
 *   identity  a napple-account access token (napple_account_access_token(), NAPPLE_ACCOUNT_TOKEN_FILE or NAPPLE_ACCOUNT_TOKEN) and a profile id
 *             (NAPPLE_SOCIAL_PROFILE, else the account's first profile from GET /account/v1/profiles);
 *   presence  PUT /social/v1/presence {status "playing", title_id, title_name, mode "online" while the title declares online play, join_token =
 *             base64url of the title's own presence app data}; heartbeats every heartbeat_interval_secs - 10; DELETE on stop;
 *   friends   GET /social/v1/friends every NAPPLE_SOCIAL_POLL_MS; a changed entry queues a "friend list updated" notification, a new incoming
 *             request (GET /friend-requests) a "new friend request" one.
 *
 * Friend ids the title sees: a friend on the same Napple online server publishes its game-server account id (the NEX principal id, which is also
 * the network service account id the title reads from acc) in its join token, and is listed under that id, so the title can look the friend's
 * sessions up on the game server (MK8D's Friends mode: MatchmakeExtension GetPlayingSession). Until then (and for friends never seen online) the id
 * is a 63-bit hash of the profile UUID (napple-social ids are UUIDs, nn::friends ids are 64-bit). A friend's app data
 * is handed only to a title with the same title id (the console shows it only within one presence group). Polling instead of the WebSocket stream
 * keeps this client to plain HTTP/1.1 over the host provider; the cost is up to one poll interval of delay. */
#include "net_online.h"
#include "net_host.h"
#include "json.h"
#include "napple.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define NS_MAX_FRIENDS 300 /* napple-social's friend limit */
#define NS_MAX_NOTES 64
#define NS_MAX_REQUESTS 64

struct NetSocial {
    const OnlineProvider *host;
    NetSocialConfig cfg;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t th;
    int thread, stop;
    /* identity */
    int ready, failed_logged;
    char token[4096];
    char profile[64], nickname[64], friend_code[32];
    /* presence */
    uint8_t app[ONLINE_FRIEND_APP_FIELD];
    size_t app_n;
    int online_play, presence_dirty, presence_live;
    int64_t presence_ms, heartbeat_ms;
    uint64_t self_pid;
    struct {
        uint64_t hash, pid;
    } pidmap[NS_MAX_FRIENDS]; /* profile hash -> game-server id, kept while a friend goes offline during the run */
    uint32_t npidmap;
    int (*pid_cb)(void *arg, const uint64_t *pids, uint32_t n); /* told the friends' game-server ids when they change */
    void *pid_arg;
    uint64_t pid_reported; /* hash of the last set the listener accepted */
    /* friends */
    OnlineFriendInfo friends[NS_MAX_FRIENDS];
    uint32_t nfriends;
    int have_list;
    char requests[NS_MAX_REQUESTS][40];
    uint32_t nrequests;
    int64_t requests_ms, start_ms;
    struct {
        uint32_t type;
        uint64_t id;
    } notes[NS_MAX_NOTES];
    uint32_t nnotes;
    void (*notify)(void *);
    void *notify_arg;
    unsigned round;
};

/* ---- pure helpers ---- */

uint64_t net_social_friend_id(const char *profile_id) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *p = profile_id; p && *p; p++)
        if (*p != '-') h = (h ^ (uint8_t)(*p | 0x20)) * 0x100000001b3ull; /* case and dashes do not change the id */
    h &= 0x7fffffffffffffffull;
    return h ? h : 1;
}

static const char b64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        size_t k = n - i >= 3 ? 4 : n - i + 1;
        for (size_t j = 0; j < k; j++) {
            if (o + 1 >= cap) return 0;
            out[o++] = b64url[v >> (18 - 6 * j) & 63];
        }
    }
    out[o] = 0;
    return o;
}
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}
/* base64url or standard base64, padding optional. Returns the bytes written, or (size_t)-1 when malformed. */
static size_t b64_decode(const char *in, uint8_t *out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (const char *p = in; *p && *p != '='; p++) {
        int v = b64_val(*p);
        if (v < 0) return (size_t)-1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return (size_t)-1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

static void json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < cap; p++) {
        if (*p == '"' || *p == '\\') out[o++] = '\\', out[o++] = (char)*p;
        else if (*p < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", *p);
        else out[o++] = (char)*p;
    }
    out[o] = 0;
}

int net_social_presence_body(uint64_t title_id, const char *title_name, int online_play, const char *key, uint64_t pid, const uint8_t *app, size_t app_n,
                             char *out, size_t n) {
    char name[200] = "", app64[400] = "", tok[800] = "";
    json_escape(title_name && *title_name ? title_name : "", name, sizeof name);
    int has_app = 0;
    for (size_t i = 0; i < app_n; i++) has_app |= app[i] != 0;
    if (has_app) {
        while (app_n && !app[app_n - 1]) app_n--; /* trailing zeros are implied */
        b64_encode(app, app_n, app64, sizeof app64);
    }
    if (pid || has_app) snprintf(tok, sizeof tok, "np1:%s:%llu:%s", key ? key : "", (unsigned long long)pid, app64);
    int w = snprintf(out, n, "{\"status\":\"playing\",\"title_id\":\"%016llX\"", (unsigned long long)title_id);
    if (name[0]) w += snprintf(out + w, n > (size_t)w ? n - (size_t)w : 0, ",\"title_name\":\"%s\"", name);
    if (online_play) w += snprintf(out + w, n > (size_t)w ? n - (size_t)w : 0, ",\"mode\":\"online\"");
    if (tok[0]) w += snprintf(out + w, n > (size_t)w ? n - (size_t)w : 0, ",\"join_token\":\"%s\"", tok);
    w += snprintf(out + w, n > (size_t)w ? n - (size_t)w : 0, "}");
    return w > 0 && (size_t)w < n ? 0 : -1;
}

/* The join token: "np1:<online server key>:<NEX pid>:<base64url app data>" from this client; any other token is taken as bare app data. *pid is the
 * friend's game-server account id when the friend uses the same Napple online server (the id the title's game server knows the friend by). */
static void friend_from_json(const JsonNode *f, uint64_t running_title, const char *key, int64_t since_ms, OnlineFriendInfo *o, uint64_t *pid) {
    memset(o, 0, sizeof *o);
    *pid = 0;
    const JsonNode *prof = json_get(f, "profile"), *pr = json_get(f, "presence");
    o->id = net_social_friend_id(json_str(prof, "profile_id", ""));
    snprintf(o->name, sizeof o->name, "%s", json_str(prof, "nickname", ""));
    const JsonNode *icon = json_get(prof, "icon_id");
    if (icon && icon->type == J_STR) snprintf(o->icon_id, sizeof o->icon_id, "%s", icon->str);
    else if (icon && icon->type == J_NUM) snprintf(o->icon_id, sizeof o->icon_id, "%.0f", icon->num);
    o->is_new = since_ms > 0 && json_num(f, "friends_since_ms", 0) >= (double)since_ms;
    const char *st = json_str(pr, "status", "offline"), *mode = json_str(pr, "mode", "");
    o->status = !strcmp(st, "playing") ? (!strcasecmp(mode, "online") ? 2 : 1) : !strcmp(st, "online") ? 1 : 0;
    o->title_id = strtoull(json_str(pr, "title_id", "0"), NULL, 16);
    o->same_app = o->status && o->title_id && o->title_id == running_title;
    double seen = json_num(pr, "last_seen_ms", 0);
    o->last_online_s = seen > 0 ? (uint64_t)(seen / 1000) : 0;
    const char *tok = json_str(pr, "join_token", NULL), *app = tok;
    if (tok && !strncmp(tok, "np1:", 4)) {
        const char *k = tok + 4, *p = strchr(k, ':'), *a = p ? strchr(p + 1, ':') : NULL;
        app = a ? a + 1 : "";
        if (p && a && key && (size_t)(p - k) == strlen(key) && !strncmp(k, key, (size_t)(p - k))) *pid = strtoull(p + 1, NULL, 10);
    }
    if (o->same_app && app && *app) {
        size_t m = b64_decode(app, o->app_field, sizeof o->app_field);
        o->has_app_field = m != (size_t)-1 && m > 0;
        if (!o->has_app_field) memset(o->app_field, 0, sizeof o->app_field);
    }
}

uint32_t net_social_parse_friends(const char *json, uint64_t running_title, const char *key, int64_t since_ms, OnlineFriendInfo *out, uint64_t *pids,
                                  uint32_t max) {
    JsonNode *root = json ? json_parse(json, strlen(json)) : NULL;
    const JsonNode *arr = json_get(root, "friends");
    uint32_t n = 0;
    if (arr && arr->type == J_ARR)
        for (const JsonNode *f = arr->child; f && n < max; f = f->next)
            if (f->type == J_OBJ) {
                uint64_t pid;
                friend_from_json(f, running_title, key, since_ms, &out[n], &pid);
                if (pids) pids[n] = pid;
                else if (pid) out[n].id = pid;
                n++;
            }
    json_free(root);
    return n;
}

/* ---- configuration ---- */

static void server_or(const char *env, const NetOnlineServer *dflt, NetOnlineServer *out) {
    const char *v = getenv(env);
    char url[512];
    if (v && *v) {
        snprintf(url, sizeof url, "%s", v);
        size_t n = strlen(url);
        while (n && url[n - 1] == '/') url[--n] = 0;
        if (n > 11 && !strcmp(url + n - 11, "/account/v1")) url[n - 11] = 0; /* napple-cloud's form of NAPPLE_ACCOUNT_URL */
        if (n > 10 && !strcmp(url + n - 10, "/social/v1")) url[n - 10] = 0;
        if (net_online_parse_server(url, out) == 0) return;
        log_line("online", "napple social: %s='%s' is not an http(s) URL; using the online server", env, v);
    }
    *out = *dflt;
}

int net_social_config(const NetOnlineServer *online, NetSocialConfig *out) {
    memset(out, 0, sizeof *out);
    server_or(NET_ONLINE_ENV_SOCIAL, online, &out->social);
    server_or(NET_ONLINE_ENV_ACCOUNT, online, &out->account);
    const char *v;
    if ((v = getenv(NET_ONLINE_ENV_TOKEN)) && *v) snprintf(out->token, sizeof out->token, "%s", v);
    if ((v = getenv(NET_ONLINE_ENV_TOKEN_FILE)) && *v) snprintf(out->token_file, sizeof out->token_file, "%s", v);
    if ((v = getenv(NET_ONLINE_ENV_PROFILE)) && *v) snprintf(out->profile, sizeof out->profile, "%s", v);
    if (!out->profile[0]) napple_account_profile_id(out->profile, sizeof out->profile); /* the local user's mapped Napple profile (src/online/) */
    out->poll_ms = (v = getenv(NET_ONLINE_ENV_SOCIAL_POLL)) && atoi(v) >= 500 ? atoi(v) : 5000;
    snprintf(out->online_key, sizeof out->online_key, "%s", online->key);
    out->title_id = strtoull(g_cfg.title_id, NULL, 16);
    snprintf(out->title_name, sizeof out->title_name, "%s", g_cfg.title_name);
    char probe[4096];
    return out->token[0] || out->token_file[0] || napple_account_access_token(probe, sizeof probe) ? 0 : -1;
}

__attribute__((weak)) size_t napple_account_access_token(char *out, size_t n) {
    (void)out, (void)n;
    return 0;
}
__attribute__((weak)) size_t napple_account_profile_id(char *out, size_t n) {
    (void)out, (void)n;
    return 0;
}

/* ---- requests ---- */

/* The current access token: the host application's, else the token file, else the environment. 0 = one exists. */
static int load_token(NetSocial *s) {
    char t[4096] = "";
    size_t n = 0;
    n = napple_account_access_token(t, sizeof t - 1);
    t[n < sizeof t ? n : sizeof t - 1] = 0;
    if (!n && s->cfg.token_file[0]) {
        FILE *f = fopen(s->cfg.token_file, "r");
        if (f) {
            n = fread(t, 1, sizeof t - 1, f);
            fclose(f);
            t[n] = 0;
        }
    }
    if (!t[0]) snprintf(t, sizeof t, "%s", s->cfg.token);
    for (n = strlen(t); n && (t[n - 1] == '\n' || t[n - 1] == '\r' || t[n - 1] == ' '); n--) t[n - 1] = 0;
    pthread_mutex_lock(&s->mu);
    snprintf(s->token, sizeof s->token, "%s", t);
    pthread_mutex_unlock(&s->mu);
    return t[0] ? 0 : -1;
}

/* A request to napple-social (social = 1) or napple-account with the token (and the profile header for social). Returns the status; *out the body. */
static int call(NetSocial *s, int social, const char *method, const char *path, const char *body, char **out) {
    char *hdr = malloc(4400);
    if (!hdr) return -1;
    pthread_mutex_lock(&s->mu);
    int w = snprintf(hdr, 4400, "Authorization: Bearer %s\r\nAccept: application/json\r\n", s->token);
    if (social && s->profile[0]) snprintf(hdr + w, 4400 - (size_t)w, "X-Napple-Profile: %s\r\n", s->profile);
    pthread_mutex_unlock(&s->mu);
    char *resp = NULL;
    int st = net_online_request(s->host, social ? &s->cfg.social : &s->cfg.account, method, path, hdr, "application/json", body, &resp);
    free(hdr);
    if (st == 401) { /* expired or rotated: the next round reads the token source again */
        pthread_mutex_lock(&s->mu);
        s->ready = 0;
        pthread_mutex_unlock(&s->mu);
    }
    if (out) *out = resp;
    else free(resp);
    return st;
}

static void error_line(const char *what, int st, const char *body) {
    JsonNode *j = body ? json_parse(body, strlen(body)) : NULL;
    const JsonNode *e = json_get(j, "error");
    const char *code = e && e->type == J_OBJ ? json_str(e, "code", "") : e && e->type == J_STR ? e->str : "";
    log_line("online", "napple social: %s failed (HTTP %d%s%s)", what, st, code[0] ? ", " : "", code);
    json_free(j);
}

/* Token, profile and GET /me. 0 = signed in. */
static int sign_in(NetSocial *s) {
    if (load_token(s)) return -1;
    char *body = NULL;
    int st;
    if (!s->cfg.profile[0] && !s->profile[0]) {
        st = call(s, 0, "GET", "/account/v1/profiles", NULL, &body);
        JsonNode *j = st == 200 && body ? json_parse(body, strlen(body)) : NULL;
        const JsonNode *arr = j && j->type == J_ARR ? j : json_get(j, "profiles");
        const JsonNode *first = arr && arr->type == J_ARR ? arr->child : NULL;
        const char *id = json_str(first, "profile_id", json_str(first, "id", ""));
        pthread_mutex_lock(&s->mu);
        snprintf(s->profile, sizeof s->profile, "%s", id);
        pthread_mutex_unlock(&s->mu);
        json_free(j);
        if (!s->profile[0]) {
            if (!s->failed_logged++) error_line("profile lookup on napple-account", st, body);
            free(body);
            return -1;
        }
        free(body);
        body = NULL;
    } else if (!s->profile[0]) snprintf(s->profile, sizeof s->profile, "%s", s->cfg.profile);
    st = call(s, 1, "GET", "/social/v1/me", NULL, &body);
    JsonNode *me = st == 200 && body ? json_parse(body, strlen(body)) : NULL;
    if (!me) {
        if (!s->failed_logged++) error_line("sign-in to napple-social", st, body);
        free(body);
        return -1;
    }
    pthread_mutex_lock(&s->mu);
    snprintf(s->friend_code, sizeof s->friend_code, "%s", json_str(me, "friend_code", ""));
    snprintf(s->nickname, sizeof s->nickname, "%s", json_str(me, "nickname", ""));
    s->ready = 1;
    s->presence_dirty = 1;
    s->failed_logged = 0;
    pthread_mutex_unlock(&s->mu);
    log_line("online", "napple social: profile %s (%s), friend code %s, %.0f friends, on %s:%u", s->profile, s->nickname, s->friend_code,
             json_num(me, "friend_count", 0), s->cfg.social.host, s->cfg.social.port);
    json_free(me);
    free(body);
    return 0;
}

static void queue_note(NetSocial *s, uint32_t type, uint64_t id) { /* s->mu held */
    for (uint32_t i = 0; i < s->nnotes; i++)
        if (s->notes[i].type == type && s->notes[i].id == id) return; /* one pending note per friend and kind */
    if (s->nnotes == NS_MAX_NOTES) memmove(s->notes, s->notes + 1, sizeof s->notes[0] * --s->nnotes);
    s->notes[s->nnotes].type = type;
    s->notes[s->nnotes].id = id;
    s->nnotes++;
}

static int same_friend(const OnlineFriendInfo *a, const OnlineFriendInfo *b) {
    return a->status == b->status && a->same_app == b->same_app && a->title_id == b->title_id && !strcmp(a->name, b->name) &&
           !memcmp(a->app_field, b->app_field, sizeof a->app_field);
}

static int presence_step(NetSocial *s, int64_t now) {
    pthread_mutex_lock(&s->mu);
    int dirty = s->presence_dirty, live = s->presence_live;
    int64_t due = s->heartbeat_ms;
    char body[1024];
    net_social_presence_body(s->cfg.title_id, s->cfg.title_name, s->online_play, s->cfg.online_key, s->self_pid, s->app, s->app_n, body, sizeof body);
    s->presence_dirty = 0;
    pthread_mutex_unlock(&s->mu);
    if (!dirty && live && now < due) return 0;
    char *resp = NULL;
    int st = 0;
    if (!dirty && live) {
        st = call(s, 1, "POST", "/social/v1/presence/heartbeat", NULL, &resp);
        if (st == 409) dirty = 1; /* presence_expired: set it again */
    }
    if (dirty || !live) {
        free(resp);
        resp = NULL;
        st = call(s, 1, "PUT", "/social/v1/presence", body, &resp);
    }
    JsonNode *j = st == 200 && resp ? json_parse(resp, strlen(resp)) : NULL;
    double hb = json_num(j, "heartbeat_interval_secs", 60);
    pthread_mutex_lock(&s->mu);
    if (st == 200) {
        if (!s->presence_live || dirty) log_line("online", "napple social: presence %s", body);
        s->presence_live = 1;
        s->heartbeat_ms = now + (int64_t)((hb > 15 ? hb - 10 : 5) * 1000);
    } else {
        s->presence_dirty = 1;
        s->presence_live = 0;
    }
    pthread_mutex_unlock(&s->mu);
    if (st != 200) error_line("presence update", st, resp);
    json_free(j);
    free(resp);
    return st == 200 ? 0 : -1;
}

/* Hands the friends' game-server ids to the listener (net_online.c tells the Napple online server) when the set changed since it last accepted it. */
static void report_pids(NetSocial *s) {
    uint64_t pids[NS_MAX_FRIENDS], h = 0xcbf29ce484222325ull;
    pthread_mutex_lock(&s->mu);
    uint32_t n = s->npidmap;
    for (uint32_t i = 0; i < n; i++) pids[i] = s->pidmap[i].pid, h = (h ^ pids[i]) * 0x100000001b3ull;
    int (*cb)(void *, const uint64_t *, uint32_t) = s->pid_cb;
    void *arg = s->pid_arg;
    int due = cb && h != s->pid_reported;
    pthread_mutex_unlock(&s->mu);
    if (due && cb(arg, pids, n) == 0) {
        pthread_mutex_lock(&s->mu);
        s->pid_reported = h;
        pthread_mutex_unlock(&s->mu);
    }
}

static int friends_step(NetSocial *s) {
    char *resp = NULL;
    int st = call(s, 1, "GET", "/social/v1/friends", NULL, &resp);
    if (st != 200 || !resp) {
        error_line("friend list", st, resp);
        free(resp);
        return -1;
    }
    static OnlineFriendInfo fresh[NS_MAX_FRIENDS]; /* only the client thread (or the test driving it) runs this */
    static uint64_t pids[NS_MAX_FRIENDS];
    uint32_t n = net_social_parse_friends(resp, s->cfg.title_id, s->cfg.online_key, s->start_ms, fresh, pids, NS_MAX_FRIENDS);
    free(resp);
    pthread_mutex_lock(&s->mu);
    for (uint32_t i = 0; i < n; i++) { /* a friend known to the same game server is shown under that id (the title looks its sessions up by it) */
        uint32_t k = 0;
        while (k < s->npidmap && s->pidmap[k].hash != fresh[i].id) k++;
        if (pids[i] && k == s->npidmap && k < NS_MAX_FRIENDS) s->npidmap++;
        if (pids[i] && k < NS_MAX_FRIENDS) s->pidmap[k].hash = fresh[i].id, s->pidmap[k].pid = pids[i];
        if (k < s->npidmap) fresh[i].id = s->pidmap[k].pid;
    }
    uint32_t before = s->nnotes;
    int changed = 0;
    if (s->have_list) {
        for (uint32_t i = 0; i < n; i++) {
            int found = 0;
            for (uint32_t k = 0; k < s->nfriends && !found; k++)
                if (s->friends[k].id == fresh[i].id) found = 1 + same_friend(&s->friends[k], &fresh[i]);
            if (found != 2) queue_note(s, ONLINE_FRIEND_NOTE_LIST_UPDATED, fresh[i].id), changed = 1; /* new, or presence / name / app data changed */
        }
        for (uint32_t k = 0; k < s->nfriends; k++) {
            int found = 0;
            for (uint32_t i = 0; i < n && !found; i++) found = s->friends[k].id == fresh[i].id;
            if (!found) queue_note(s, ONLINE_FRIEND_NOTE_LIST_UPDATED, s->friends[k].id), changed = 1; /* removed */
        }
    }
    int first = !s->have_list;
    memcpy(s->friends, fresh, sizeof fresh[0] * n);
    s->nfriends = n;
    s->have_list = 1;
    int notify = s->nnotes != before;
    void (*cb)(void *) = s->notify;
    void *arg = s->notify_arg;
    pthread_mutex_unlock(&s->mu);
    report_pids(s);
    if (first) {
        log_line("online", "napple social: %u friends", n);
        for (uint32_t i = 0; i < n; i++)
            log_line("online", "napple social: friend %016llx '%s' %s%s%s", (unsigned long long)fresh[i].id, fresh[i].name,
                     fresh[i].status == 2 ? "online play" : fresh[i].status ? "online" : "offline", fresh[i].same_app ? ", same title" : "",
                     fresh[i].has_app_field ? ", joinable" : "");
    } else if (changed) {
        for (uint32_t i = 0; i < n; i++)
            log_line("online", "napple social: friend %016llx '%s' now %s%s%s", (unsigned long long)fresh[i].id, fresh[i].name,
                     fresh[i].status == 2 ? "online play" : fresh[i].status ? "online" : "offline", fresh[i].same_app ? ", same title" : "",
                     fresh[i].has_app_field ? ", joinable" : "");
    }
    if (notify && cb) cb(arg);
    return 0;
}

static int requests_step(NetSocial *s) {
    char *resp = NULL;
    int st = call(s, 1, "GET", "/social/v1/friend-requests", NULL, &resp);
    JsonNode *j = st == 200 && resp ? json_parse(resp, strlen(resp)) : NULL;
    free(resp);
    if (!j) return -1;
    const JsonNode *in = json_get(j, "incoming");
    char ids[NS_MAX_REQUESTS][40];
    uint32_t n = 0;
    if (in && in->type == J_ARR)
        for (const JsonNode *r = in->child; r && n < NS_MAX_REQUESTS; r = r->next) snprintf(ids[n++], sizeof ids[0], "%s", json_str(r, "request_id", ""));
    pthread_mutex_lock(&s->mu);
    uint32_t before = s->nnotes;
    for (uint32_t i = 0; i < n; i++) {
        int known = 0;
        for (uint32_t k = 0; k < s->nrequests && !known; k++) known = !strcmp(ids[i], s->requests[k]);
        if (!known) {
            const JsonNode *r = in->child;
            for (uint32_t k = 0; k < i; k++) r = r->next;
            const JsonNode *from = json_get(r, "from");
            queue_note(s, ONLINE_FRIEND_NOTE_NEW_REQUEST, net_social_friend_id(json_str(from, "profile_id", "")));
            log_line("online", "napple social: friend request from '%s'", json_str(from, "nickname", "?"));
        }
    }
    memcpy(s->requests, ids, sizeof ids[0] * n);
    s->nrequests = n;
    int notify = s->nnotes != before;
    void (*cb)(void *) = s->notify;
    void *arg = s->notify_arg;
    pthread_mutex_unlock(&s->mu);
    json_free(j);
    if (notify && cb) cb(arg);
    return 0;
}

int net_social_sync(NetSocial *s) {
    pthread_mutex_lock(&s->mu);
    int ready = s->ready;
    pthread_mutex_unlock(&s->mu);
    if (!ready && sign_in(s)) return -1;
    int64_t now = nh_now_ms();
    int rc = presence_step(s, now);
    rc |= friends_step(s);
    if (s->round++ % 6 == 0 || now - s->requests_ms > 30000) {
        s->requests_ms = now;
        rc |= requests_step(s);
    }
    return rc ? -1 : 0;
}

static void *worker(void *arg) {
    NetSocial *s = arg;
    pthread_mutex_lock(&s->mu);
    while (!s->stop) {
        pthread_mutex_unlock(&s->mu);
        int rc = net_social_sync(s);
        pthread_mutex_lock(&s->mu);
        if (s->stop) break;
        int64_t wait = rc && !s->ready ? 10000 : s->cfg.poll_ms;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += wait / 1000;
        ts.tv_nsec += (wait % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        if (!s->presence_dirty) pthread_cond_timedwait(&s->cv, &s->mu, &ts); /* a presence change wakes the thread at once */
    }
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

NetSocial *net_social_start(const OnlineProvider *host, const NetSocialConfig *cfg, int thread) {
    NetSocial *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->host = host;
    s->cfg = *cfg;
    s->start_ms = (int64_t)time(NULL) * 1000;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    if (thread && pthread_create(&s->th, NULL, worker, s) == 0) s->thread = 1;
    return s;
}

void net_social_quit(NetSocial *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    s->stop = 1;
    pthread_cond_broadcast(&s->cv);
    int live = s->presence_live, joined = !s->thread;
    s->presence_live = 0;
    s->thread = 0;
    pthread_mutex_unlock(&s->mu);
    if (!joined) pthread_join(s->th, NULL);
    if (live) call(s, 1, "DELETE", "/social/v1/presence", NULL, NULL); /* offline now, not after three missed heartbeats */
}

void net_social_stop(NetSocial *s) {
    if (!s) return;
    net_social_quit(s);
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

/* ---- what friend:u reads ---- */

uint32_t net_social_list(NetSocial *s, uint32_t offset, OnlineFriendInfo *out, uint32_t max) {
    pthread_mutex_lock(&s->mu);
    uint32_t n = 0;
    for (uint32_t i = offset; i < s->nfriends && n < max; i++) out[n++] = s->friends[i];
    pthread_mutex_unlock(&s->mu);
    return n;
}
int net_social_find(NetSocial *s, uint64_t id, OnlineFriendInfo *out) {
    pthread_mutex_lock(&s->mu);
    int rc = -1;
    for (uint32_t i = 0; i < s->nfriends && rc; i++)
        if (s->friends[i].id == id) *out = s->friends[i], rc = 0;
    pthread_mutex_unlock(&s->mu);
    return rc;
}
uint32_t net_social_count(NetSocial *s) {
    pthread_mutex_lock(&s->mu);
    uint32_t n = s->nfriends;
    pthread_mutex_unlock(&s->mu);
    return n;
}
uint32_t net_social_request_count(NetSocial *s) {
    pthread_mutex_lock(&s->mu);
    uint32_t n = s->nrequests;
    pthread_mutex_unlock(&s->mu);
    return n;
}
int net_social_pop_note(NetSocial *s, uint32_t *type, uint64_t *id) {
    pthread_mutex_lock(&s->mu);
    int rc = -1;
    if (s->nnotes) {
        *type = s->notes[0].type;
        *id = s->notes[0].id;
        memmove(s->notes, s->notes + 1, sizeof s->notes[0] * --s->nnotes);
        rc = 0;
    }
    pthread_mutex_unlock(&s->mu);
    return rc;
}
void net_social_set_notify(NetSocial *s, void (*cb)(void *), void *arg) {
    pthread_mutex_lock(&s->mu);
    s->notify = cb;
    s->notify_arg = arg;
    int pending = cb && s->nnotes;
    pthread_mutex_unlock(&s->mu);
    if (pending) cb(arg);
}
void net_social_presence(NetSocial *s, const void *record, size_t n) {
    uint8_t app[ONLINE_FRIEND_APP_FIELD] = {0};
    size_t m = n > 0x20 ? n - 0x20 : 0;
    if (m > sizeof app) m = sizeof app;
    if (m) memcpy(app, (const uint8_t *)record + 0x20, m);
    pthread_mutex_lock(&s->mu);
    if (memcmp(app, s->app, sizeof app) || !s->app_n) {
        memcpy(s->app, app, sizeof app);
        s->app_n = sizeof app;
        s->presence_dirty = 1;
        pthread_cond_broadcast(&s->cv);
    }
    pthread_mutex_unlock(&s->mu);
}
void net_social_online_play(NetSocial *s, int open) {
    pthread_mutex_lock(&s->mu);
    if (s->online_play != !!open) {
        s->online_play = !!open;
        s->presence_dirty = 1;
        pthread_cond_broadcast(&s->cv);
    }
    pthread_mutex_unlock(&s->mu);
}
const char *net_social_friend_code(NetSocial *s) { return s->friend_code; }
void net_social_set_pid_listener(NetSocial *s, int (*cb)(void *arg, const uint64_t *pids, uint32_t n), void *arg) {
    pthread_mutex_lock(&s->mu);
    s->pid_cb = cb;
    s->pid_arg = arg;
    s->pid_reported = 0;
    pthread_mutex_unlock(&s->mu);
}
void net_social_set_self_id(NetSocial *s, uint64_t pid) {
    pthread_mutex_lock(&s->mu);
    if (s->self_pid != pid) {
        s->self_pid = pid;
        s->pid_reported = 0;
        s->presence_dirty = 1;
        pthread_cond_broadcast(&s->cv);
    }
    pthread_mutex_unlock(&s->mu);
}
