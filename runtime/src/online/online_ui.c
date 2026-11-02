#include "online_ui.h"
#include "../json.h"
#include "napple_cloud.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int napple_online_open(const char *home, const char *account_url, const char *social_url, const char *uid_hex);
void napple_online_close(void);
int napple_online_act(const char *action, const char *args_json, char **json_out);
int napple_online_refresh(void);
char *napple_online_state(void);
size_t napple_online_access_token(char *out, size_t n);
size_t napple_online_profile_id(char *out, size_t n);
void napple_online_free(char *p);

/* ---- tiny JSON writer for action arguments ---- */
typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bstr(Buf *b, const char *s) {
    bput(b, "\"", 1);
    for (const unsigned char *c = (const unsigned char *)(s ? s : ""); *c; c++) {
        char t[8];
        if (*c == '"' || *c == '\\') { t[0] = '\\'; t[1] = (char)*c; bput(b, t, 2); }
        else if (*c < 0x20) { int k = snprintf(t, sizeof t, "\\u%04x", *c); bput(b, t, (size_t)k); }
        else bput(b, (const char *)c, 1);
    }
    bput(b, "\"", 1);
}
/* args("k", "v", "k2", "v2", NULL) -> {"k":"v",...}; a key starting with '#' takes an int (as a number), '?' a bool int. */
static char *mkargs(const char *first, ...) {
    Buf b = {0};
    bput(&b, "{", 1);
    va_list ap;
    va_start(ap, first);
    int comma = 0;
    for (const char *k = first; k; k = va_arg(ap, const char *)) {
        if (comma) bput(&b, ",", 1);
        comma = 1;
        int kind = *k == '#' ? 1 : *k == '?' ? 2 : *k == '{' ? 3 : 0;
        bstr(&b, kind ? k + 1 : k);
        bput(&b, ":", 1);
        if (kind == 1 || kind == 2) {
            int v = va_arg(ap, int);
            char t[16];
            int n = snprintf(t, sizeof t, kind == 2 ? (v ? "true" : "false") : "%d", v);
            bput(&b, t, (size_t)n);
        } else if (kind == 3) {
            const char *raw = va_arg(ap, const char *); /* already JSON */
            bput(&b, raw && *raw ? raw : "null", strlen(raw && *raw ? raw : "null"));
        } else {
            bstr(&b, va_arg(ap, const char *));
        }
    }
    va_end(ap);
    bput(&b, "}", 1);
    return b.p;
}

/* ---- the runtime ---- */
typedef struct Job {
    uint64_t id;
    char *action, *args;
    napple_online_done_fn done;
    void *user;
    struct Job *next;
    int result;
    char *json;
} Job;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t th;
    int running, opened, dirty, refresh_req;
    uint64_t next_id;
    Job *todo, *todo_tail, *finished, *finished_tail;
    JsonNode *state;
    napple_online_change_fn change;
    void *change_user;
    napple_online_ui_api api;
} G = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void set_state(char *text) {
    JsonNode *n = text ? json_parse(text, strlen(text)) : NULL;
    pthread_mutex_lock(&G.mu);
    JsonNode *old = G.state;
    G.state = n;
    pthread_mutex_unlock(&G.mu);
    json_free(old);
}

static int poll_ms(void) {
    const char *v = getenv("NAPPLE_ONLINE_POLL_MS");
    return v && atoi(v) >= 100 ? atoi(v) : 30000;
}

static void refresh_state(void) {
    int changed = napple_online_refresh();
    char *s = napple_online_state();
    set_state(s);
    napple_online_free(s);
    if (changed) {
        pthread_mutex_lock(&G.mu);
        G.dirty = 1;
        pthread_mutex_unlock(&G.mu);
    }
}

static void *worker(void *arg) {
    (void)arg;
    refresh_state();
    pthread_mutex_lock(&G.mu);
    while (G.running) {
        if (!G.todo && !G.refresh_req) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int ms = poll_ms();
            ts.tv_sec += ms / 1000;
            ts.tv_nsec += (long)(ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&G.cv, &G.mu, &ts);
        }
        if (!G.running) break;
        Job *j = G.todo;
        if (j) {
            G.todo = j->next;
            if (!G.todo) G.todo_tail = NULL;
            pthread_mutex_unlock(&G.mu);
            j->result = napple_online_act(j->action, j->args, &j->json);
            refresh_state();
            pthread_mutex_lock(&G.mu);
            j->next = NULL;
            if (G.finished_tail) G.finished_tail->next = j; else G.finished = j;
            G.finished_tail = j;
            continue;
        }
        G.refresh_req = 0;
        pthread_mutex_unlock(&G.mu);
        refresh_state();
        pthread_mutex_lock(&G.mu);
    }
    pthread_mutex_unlock(&G.mu);
    return NULL;
}

static uint64_t submit(napple_online_done_fn done, void *user, const char *action, char *args) {
    Job *j = calloc(1, sizeof *j);
    j->action = strdup(action);
    j->args = args;
    j->done = done;
    j->user = user;
    pthread_mutex_lock(&G.mu);
    j->id = ++G.next_id;
    if (G.todo_tail) G.todo_tail->next = j; else G.todo = j;
    G.todo_tail = j;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
    return j->id;
}

int napple_online_ui_pump(void) {
    int n = 0;
    for (;;) {
        pthread_mutex_lock(&G.mu);
        Job *j = G.finished;
        if (j) {
            G.finished = j->next;
            if (!G.finished) G.finished_tail = NULL;
        }
        pthread_mutex_unlock(&G.mu);
        if (!j) break;
        if (j->done) j->done(j->user, j->id, j->result, j->json ? j->json : "{}");
        napple_online_free(j->json);
        free(j->action);
        free(j->args);
        free(j);
        n++;
    }
    pthread_mutex_lock(&G.mu);
    int dirty = G.dirty;
    G.dirty = 0;
    napple_online_change_fn cb = G.change;
    void *u = G.change_user;
    pthread_mutex_unlock(&G.mu);
    if (dirty && cb) { cb(u); n++; }
    return n;
}

void napple_online_refresh_now(void) {
    pthread_mutex_lock(&G.mu);
    G.refresh_req = 1;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}

/* ---- state getters (cache -> caller structs) ---- */
static void cp(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }
static int inum(const JsonNode *o, const char *k, int d) { return (int)json_num(o, k, d); }
static int jbool(const JsonNode *o, const char *k) {
    const JsonNode *v = json_get(o, k);
    return v && ((v->type == J_BOOL && v->num != 0) || (v->type == J_NUM && v->num != 0));
}
static size_t arr_len(const JsonNode *a) {
    size_t n = 0;
    if (a && a->type == J_ARR) for (const JsonNode *c = a->child; c; c = c->next) n++;
    return n;
}

static void fill_profile(napple_online_profile *p, const JsonNode *j) {
    memset(p, 0, sizeof *p);
    cp(p->id, sizeof p->id, json_str(j, "id", ""));
    cp(p->nickname, sizeof p->nickname, json_str(j, "nickname", ""));
    cp(p->bio, sizeof p->bio, json_str(j, "bio", ""));
    p->icon_id = inum(j, "icon_id", 0);
    cp(p->photo_path, sizeof p->photo_path, json_str(j, "photo_path", ""));
    p->tier = inum(j, "tier", NAPPLE_TIER_PUBLIC);
    cp(p->friend_code, sizeof p->friend_code, json_str(j, "friend_code", ""));
    const JsonNode *l = json_get(j, "links");
    if (l && l->type == J_ARR)
        for (const JsonNode *c = l->child; c && p->link_count < NAPPLE_ONLINE_MAX_LINKS; c = c->next) {
            cp(p->links[p->link_count].kind, sizeof p->links[0].kind, json_str(c, "kind", ""));
            cp(p->links[p->link_count].value, sizeof p->links[0].value, json_str(c, "value", ""));
            p->link_count++;
        }
}

static void u_get_account(void *ctx, napple_online_account *out) {
    (void)ctx;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&G.mu);
    const JsonNode *s = G.state;
    if (s) {
        out->signed_in = jbool(s, "signed_in");
        cp(out->email, sizeof out->email, json_str(s, "email", ""));
        cp(out->account_id, sizeof out->account_id, json_str(s, "account_id", ""));
        out->email_verified = jbool(s, "email_verified");
        out->server_reachable = jbool(s, "server_reachable");
        out->profile_count = (int)arr_len(json_get(s, "profiles"));
        const JsonNode *c = json_get(s, "current");
        if (out->signed_in && c) fill_profile(&out->current, c);
    }
    pthread_mutex_unlock(&G.mu);
}

typedef void (*fill_fn)(void *dst, const JsonNode *j);
static size_t list_copy(const char *key, void *out, size_t cap, size_t esz, fill_fn fill) {
    size_t total = 0;
    pthread_mutex_lock(&G.mu);
    const JsonNode *a = G.state ? json_get(G.state, key) : NULL;
    if (a && a->type == J_ARR)
        for (const JsonNode *c = a->child; c; c = c->next, total++)
            if (out && total < cap) fill((char *)out + total * esz, c);
    pthread_mutex_unlock(&G.mu);
    return total;
}

static void f_profile(void *d, const JsonNode *j) { fill_profile(d, j); }
static void f_device(void *d, const JsonNode *j) {
    napple_online_device *o = d;
    memset(o, 0, sizeof *o);
    cp(o->id, sizeof o->id, json_str(j, "id", ""));
    cp(o->name, sizeof o->name, json_str(j, "name", ""));
    cp(o->platform, sizeof o->platform, json_str(j, "platform", ""));
    o->current = jbool(j, "current");
    o->pending = jbool(j, "pending");
    o->last_seen_unix = (int64_t)json_num(j, "last_seen_unix", 0);
}
static void f_friend(void *d, const JsonNode *j) {
    napple_online_friend *o = d;
    memset(o, 0, sizeof *o);
    cp(o->profile_id, sizeof o->profile_id, json_str(j, "profile_id", ""));
    cp(o->nickname, sizeof o->nickname, json_str(j, "nickname", ""));
    cp(o->photo_path, sizeof o->photo_path, json_str(j, "photo_path", ""));
    o->icon_id = inum(j, "icon_id", 0);
    o->online = jbool(j, "online");
    cp(o->title_id, sizeof o->title_id, json_str(j, "title_id", ""));
    cp(o->mode, sizeof o->mode, json_str(j, "mode", ""));
    cp(o->joinable_token, sizeof o->joinable_token, json_str(j, "joinable_token", ""));
}
static void f_request(void *d, const JsonNode *j) {
    napple_online_request_entry *o = d;
    memset(o, 0, sizeof *o);
    cp(o->profile_id, sizeof o->profile_id, json_str(j, "profile_id", ""));
    cp(o->nickname, sizeof o->nickname, json_str(j, "nickname", ""));
    cp(o->photo_path, sizeof o->photo_path, json_str(j, "photo_path", ""));
    o->icon_id = inum(j, "icon_id", 0);
    o->incoming = jbool(j, "incoming");
}
static void f_stat(void *d, const JsonNode *j) {
    napple_online_stat *o = d;
    memset(o, 0, sizeof *o);
    cp(o->title_id, sizeof o->title_id, json_str(j, "title_id", ""));
    cp(o->title_name, sizeof o->title_name, json_str(j, "title_name", ""));
    cp(o->key, sizeof o->key, json_str(j, "key", ""));
    o->value = (int64_t)json_num(j, "value", 0);
}

static size_t u_get_profiles(void *c, napple_online_profile *o, size_t n) { (void)c; return list_copy("profiles", o, n, sizeof *o, f_profile); }
static size_t u_get_devices(void *c, napple_online_device *o, size_t n) { (void)c; return list_copy("devices", o, n, sizeof *o, f_device); }
static size_t u_get_friends(void *c, napple_online_friend *o, size_t n) { (void)c; return list_copy("friends", o, n, sizeof *o, f_friend); }
static size_t u_get_requests(void *c, napple_online_request_entry *o, size_t n) { (void)c; return list_copy("requests", o, n, sizeof *o, f_request); }
static size_t u_get_blocks(void *c, napple_online_request_entry *o, size_t n) { (void)c; return list_copy("blocks", o, n, sizeof *o, f_request); }
static size_t u_get_recent(void *c, napple_online_request_entry *o, size_t n) { (void)c; return list_copy("recent", o, n, sizeof *o, f_request); }
static size_t u_get_stats(void *c, napple_online_stat *o, size_t n) { (void)c; return list_copy("stats", o, n, sizeof *o, f_stat); }

/* Cloud saves: the running title's targets from napple_cloud_runtime_status(); {"active":bool,"title_id","targets":[{"status","version","detail"}]} */
static size_t u_get_save_status(void *ctx, napple_online_save_status *out, size_t cap) {
    (void)ctx;
    const char *st = napple_cloud_runtime_status();
    JsonNode *j = st ? json_parse(st, strlen(st)) : NULL;
    size_t total = 0;
    if (j && jbool(j, "active")) {
        const char *tid = json_str(j, "title_id", "");
        const JsonNode *t = json_get(j, "targets");
        for (const JsonNode *c = t && t->type == J_ARR ? t->child : NULL; c; c = c->next, total++) {
            if (!out || total >= cap) continue;
            napple_online_save_status *o = &out[total];
            memset(o, 0, sizeof *o);
            cp(o->title_id, sizeof o->title_id, tid);
            const char *s = json_str(c, "status", "");
            o->state = !strcmp(s, "synced") || !strcmp(s, "in_sync") || !strcmp(s, "pushed") || !strcmp(s, "pulled") ? 1
                     : !strcmp(s, "uploading") || !strcmp(s, "pushing") ? 2
                     : !strcmp(s, "downloading") || !strcmp(s, "pulling") ? 3
                     : !strcmp(s, "conflict") ? 4 : !strcmp(s, "error") || !strcmp(s, "quota") ? 5 : 1;
            o->bytes = (int64_t)json_num(c, "bytes", 0);
        }
    }
    json_free(j);
    return total;
}

/* ---- actions ---- */
#define ACT(...) submit(done, user, __VA_ARGS__)
static uint64_t u_signup(void *c, const char *email, const char *pw, const char *nick, int age, int terms, napple_online_done_fn done, void *user) {
    (void)c; return ACT("signup", mkargs("email", email, "password", pw, "nickname", nick, "?age_ok", age, "?terms_ok", terms, NULL));
}
static uint64_t u_verify(void *c, const char *email, const char *code, napple_online_done_fn done, void *user) { (void)c; return ACT("verify", mkargs("email", email, "code", code, NULL)); }
static uint64_t u_login(void *c, const char *email, const char *pw, napple_online_done_fn done, void *user) { (void)c; return ACT("login", mkargs("email", email, "password", pw, NULL)); }
static uint64_t u_logout(void *c, napple_online_done_fn done, void *user) { (void)c; return ACT("logout", mkargs(NULL)); }
static uint64_t u_reset_start(void *c, const char *email, napple_online_done_fn done, void *user) { (void)c; return ACT("reset_start", mkargs("email", email, NULL)); }
static uint64_t u_reset_finish(void *c, const char *email, const char *code, const char *pw, napple_online_done_fn done, void *user) {
    (void)c; return ACT("reset_finish", mkargs("email", email, "code", code, "new_password", pw, NULL));
}
static uint64_t u_switch_profile(void *c, const char *id, napple_online_done_fn done, void *user) { (void)c; return ACT("switch_profile", mkargs("profile_id", id, NULL)); }
static uint64_t u_create_profile(void *c, const char *nick, napple_online_done_fn done, void *user) { (void)c; return ACT("create_profile", mkargs("nickname", nick, NULL)); }
static uint64_t u_update_profile(void *c, const char *id, const char *pj, napple_online_done_fn done, void *user) { (void)c; return ACT("update_profile", mkargs("profile_id", id, "{profile", pj, NULL)); }
static uint64_t u_upload_photo(void *c, const char *id, const char *path, napple_online_done_fn done, void *user) { (void)c; return ACT("upload_photo", mkargs("profile_id", id, "path", path, NULL)); }
static uint64_t u_set_tier(void *c, const char *id, int tier, napple_online_done_fn done, void *user) { (void)c; return ACT("set_tier", mkargs("profile_id", id, "#tier", tier, NULL)); }
static uint64_t u_friend_request(void *c, const char *code, napple_online_done_fn done, void *user) { (void)c; return ACT("friend_request", mkargs("code", code, NULL)); }
static uint64_t u_friend_accept(void *c, const char *id, napple_online_done_fn done, void *user) { (void)c; return ACT("friend_accept", mkargs("profile_id", id, NULL)); }
static uint64_t u_friend_decline(void *c, const char *id, napple_online_done_fn done, void *user) { (void)c; return ACT("friend_decline", mkargs("profile_id", id, NULL)); }
static uint64_t u_friend_remove(void *c, const char *id, napple_online_done_fn done, void *user) { (void)c; return ACT("friend_remove", mkargs("profile_id", id, NULL)); }
static uint64_t u_block(void *c, const char *id, napple_online_done_fn done, void *user) { (void)c; return ACT("block", mkargs("profile_id", id, NULL)); }
static uint64_t u_unblock(void *c, const char *id, napple_online_done_fn done, void *user) { (void)c; return ACT("unblock", mkargs("profile_id", id, NULL)); }
static uint64_t u_search(void *c, const char *q, napple_online_done_fn done, void *user) { (void)c; return ACT("search", mkargs("query", q, NULL)); }
static uint64_t u_invite(void *c, const char *id, napple_online_done_fn done, void *user) { (void)c; return ACT("invite", mkargs("profile_id", id, NULL)); }
static uint64_t u_join(void *c, const char *id, const char *tok, napple_online_done_fn done, void *user) { (void)c; return ACT("join", mkargs("profile_id", id, "token", tok, NULL)); }
static uint64_t u_report(void *c, const char *id, const char *why, napple_online_done_fn done, void *user) { (void)c; return ACT("report", mkargs("profile_id", id, "reason", why, NULL)); }
static uint64_t u_approve(void *c, const char *id, napple_online_done_fn done, void *user) { (void)c; return ACT("approve_device", mkargs("device_id", id, NULL)); }
static uint64_t u_revoke(void *c, const char *id, napple_online_done_fn done, void *user) { (void)c; return ACT("revoke_device", mkargs("device_id", id, NULL)); }
static uint64_t u_delete(void *c, const char *pw, napple_online_done_fn done, void *user) { (void)c; return ACT("delete_account", mkargs("password", pw, NULL)); }
static void u_set_change(void *c, napple_online_change_fn fn, void *user) {
    (void)c;
    pthread_mutex_lock(&G.mu);
    G.change = fn;
    G.change_user = user;
    pthread_mutex_unlock(&G.mu);
}

const napple_online_ui_api *napple_online_runtime_start(const char *home, const char *account_url, const char *social_url, const char *uid_hex) {
    pthread_mutex_lock(&G.mu);
    if (G.opened) { pthread_mutex_unlock(&G.mu); return &G.api; }
    pthread_mutex_unlock(&G.mu);
    if (!napple_online_open(home, account_url, social_url, uid_hex)) return NULL;
    napple_online_ui_api a = {
        .version = NAPPLE_ONLINE_UI_VERSION,
        .get_account = u_get_account, .get_profiles = u_get_profiles, .get_devices = u_get_devices, .get_friends = u_get_friends,
        .get_requests = u_get_requests, .get_blocks = u_get_blocks, .get_recent = u_get_recent, .get_stats = u_get_stats, .get_save_status = u_get_save_status,
        .signup = u_signup, .verify = u_verify, .login = u_login, .logout = u_logout, .reset_start = u_reset_start, .reset_finish = u_reset_finish,
        .switch_profile = u_switch_profile, .create_profile = u_create_profile, .update_profile = u_update_profile, .upload_photo = u_upload_photo,
        .set_tier = u_set_tier, .friend_request = u_friend_request, .friend_accept = u_friend_accept, .friend_decline = u_friend_decline,
        .friend_remove = u_friend_remove, .block = u_block, .unblock = u_unblock, .search = u_search, .invite = u_invite, .join = u_join,
        .report = u_report, .approve_device = u_approve, .revoke_device = u_revoke, .delete_account = u_delete, .set_change_callback = u_set_change,
    };
    pthread_mutex_lock(&G.mu);
    G.api = a;
    G.opened = G.running = 1;
    pthread_mutex_unlock(&G.mu);
    pthread_create(&G.th, NULL, worker, NULL);
    return &G.api;
}

void napple_online_runtime_stop(void) {
    pthread_mutex_lock(&G.mu);
    if (!G.opened) { pthread_mutex_unlock(&G.mu); return; }
    G.running = 0;
    pthread_cond_broadcast(&G.cv);
    pthread_mutex_unlock(&G.mu);
    pthread_join(G.th, NULL);
    pthread_mutex_lock(&G.mu);
    G.opened = 0;
    pthread_mutex_unlock(&G.mu);
    napple_online_close();
    set_state(NULL);
    while (napple_online_ui_pump()) {}
}

/* ---- the game's friend service (net_online_social.c): strong versions of its weak hooks ---- */
static int lazy_open(void) {
    pthread_mutex_lock(&G.mu);
    int o = G.opened;
    pthread_mutex_unlock(&G.mu);
    return o || napple_online_runtime_start(NULL, NULL, NULL, NULL) != NULL;
}
size_t napple_account_access_token(char *out, size_t n) { return lazy_open() ? napple_online_access_token(out, n) : 0; }
size_t napple_account_profile_id(char *out, size_t n) { return lazy_open() ? napple_online_profile_id(out, n) : 0; }
