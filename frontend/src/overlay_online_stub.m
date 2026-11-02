/* Canned-data implementation of napple_online_ui_api: every Online screen works without servers (UI work, screenshots, tests).
   Verification code is 123456; password "wrongpassword" is rejected; the email "unverified@napple.lat" answers like an unverified account. */
#import <Foundation/Foundation.h>
#include <string.h>
#include "overlay_online.h"

static napple_online_account g_acc;
static napple_online_profile g_prof[NAPPLE_ONLINE_MAX_PROFILES];
static size_t g_nprof;
static napple_online_device g_dev[4];
static size_t g_ndev;
static napple_online_friend g_fr[3];
static napple_online_request_entry g_req[2];
static napple_online_stat g_st[2];
static napple_online_save_status g_sv[3];
static napple_online_change_fn g_chg;
static void *g_chg_user;
static uint64_t g_next = 1;
static char g_pending_email[96];

static void cp(char *d, size_t n, const char *s) { strlcpy(d, s ? s : "", n); }
void fe_online_stub_reset(int in) {
    memset(&g_acc, 0, sizeof g_acc);
    memset(g_prof, 0, sizeof g_prof);
    memset(g_dev, 0, sizeof g_dev);
    g_nprof = g_ndev = 0;
    g_acc.server_reachable = 1;
    if (in) {
        g_acc.signed_in = g_acc.email_verified = 1;
        cp(g_acc.email, sizeof g_acc.email, "player@example.com");
        cp(g_acc.account_id, sizeof g_acc.account_id, "acc-0001");
        napple_online_profile *p = &g_prof[g_nprof++];
        cp(p->id, sizeof p->id, "prof-1");
        cp(p->nickname, sizeof p->nickname, "Eros");
        cp(p->bio, sizeof p->bio, "Racing at weekends.");
        p->icon_id = 7;
        p->tier = NAPPLE_TIER_PUBLIC;
        cp(p->friend_code, sizeof p->friend_code, "NP-123456789018");
        p->link_count = 1;
        cp(p->links[0].kind, sizeof p->links[0].kind, "web");
        cp(p->links[0].value, sizeof p->links[0].value, "https://napple.lat");
        p = &g_prof[g_nprof++];
        cp(p->id, sizeof p->id, "prof-2");
        cp(p->nickname, sizeof p->nickname, "Eros.alt");
        p->icon_id = 21;
        p->tier = NAPPLE_TIER_GHOST;
        cp(p->friend_code, sizeof p->friend_code, "NP-987654321098");
        g_acc.current = g_prof[0];
        g_acc.profile_count = (int)g_nprof;
        napple_online_device *d = &g_dev[g_ndev++];
        cp(d->id, sizeof d->id, "dev-1");
        cp(d->name, sizeof d->name, "This Mac");
        cp(d->platform, sizeof d->platform, "macos");
        d->current = 1;
        d->last_seen_unix = 1793600000;
        d = &g_dev[g_ndev++];
        cp(d->id, sizeof d->id, "dev-2");
        cp(d->name, sizeof d->name, "MacBook Air");
        cp(d->platform, sizeof d->platform, "macos");
        d->pending = 1;
        d = &g_dev[g_ndev++];
        cp(d->id, sizeof d->id, "dev-3");
        cp(d->name, sizeof d->name, "Mac mini");
        cp(d->platform, sizeof d->platform, "macos");
        d->last_seen_unix = 1793000000;
        cp(g_fr[0].profile_id, 40, "prof-f1");
        cp(g_fr[0].nickname, 24, "Luma");
        g_fr[0].online = 1;
        cp(g_fr[0].title_id, 20, "0100152000022000");
        cp(g_fr[0].mode, 40, "Racing");
        cp(g_fr[1].profile_id, 40, "prof-f2");
        cp(g_fr[1].nickname, 24, "Toadette_99");
        cp(g_req[0].profile_id, 40, "prof-r1");
        cp(g_req[0].nickname, 24, "Kamek");
        g_req[0].incoming = 1;
        cp(g_st[0].title_id, 20, "0100152000022000");
        cp(g_st[0].title_name, 64, "Mario Kart 8 Deluxe");
        cp(g_st[0].key, 40, "races");
        g_st[0].value = 42;
        cp(g_sv[0].title_id, 20, "0100152000022000");
        cp(g_sv[0].title_name, 64, "Mario Kart 8 Deluxe");
        g_sv[0].state = 1;
        g_sv[0].last_sync_unix = 1793600000;
        g_sv[0].bytes = 1048576;
        cp(g_sv[1].title_id, 20, "0100000000010000");
        cp(g_sv[1].title_name, 64, "Super Mario Odyssey");
        g_sv[1].state = 2;
        g_sv[1].bytes = 3 << 20;
        cp(g_sv[2].title_id, 20, "01006BB00C6F0000");
        cp(g_sv[2].title_name, 64, "Link's Awakening");
        g_sv[2].state = 4;
    }
}
static size_t copy_out(const void *src, size_t n, size_t esz, void *out, size_t cap) {
    if (out && cap) memcpy(out, src, MIN(n, cap) * esz);
    return n;
}
static void notify(void) {
    if (g_chg) g_chg(g_chg_user);
}
/* Completes after a short delay so the busy state is visible; mutate runs at completion. */
static uint64_t finish(int rc, const char *json, napple_online_done_fn done, void *user, void (^mutate)(void)) {
    uint64_t id = g_next++;
    NSString *j = json ? @(json) : @"{}";
    double delay = getenv("NAPPLE_ONLINE_STUB_INSTANT") ? 0 : 0.35;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (rc == 0 && mutate) mutate();
        if (done) done(user, id, rc, j.UTF8String);
        if (rc == 0) notify();
    });
    return id;
}
static napple_online_profile *find_prof(const char *id) {
    for (size_t i = 0; i < g_nprof; i++)
        if (!strcmp(g_prof[i].id, id)) return &g_prof[i];
    return NULL;
}
static void sync_current(void) {
    napple_online_profile *p = find_prof(g_acc.current.id);
    if (p) g_acc.current = *p;
    g_acc.profile_count = (int)g_nprof;
}

static void s_get_account(void *c, napple_online_account *o) { (void)c; *o = g_acc; }
#define GETTER(name, arr, n, T) static size_t name(void *c, T *o, size_t cap) { (void)c; return copy_out(arr, g_acc.signed_in ? (n) : 0, sizeof(T), o, cap); }
GETTER(s_get_profiles, g_prof, g_nprof, napple_online_profile)
GETTER(s_get_devices, g_dev, g_ndev, napple_online_device)
GETTER(s_get_friends, g_fr, 2, napple_online_friend)
GETTER(s_get_requests, g_req, 1, napple_online_request_entry)
GETTER(s_get_stats, g_st, 1, napple_online_stat)
GETTER(s_get_saves, g_sv, 3, napple_online_save_status)
static size_t s_get_none(void *c, napple_online_request_entry *o, size_t cap) { (void)c; (void)o; (void)cap; return 0; }

static uint64_t s_signup(void *c, const char *email, const char *pw, const char *nick, int age, int terms, napple_online_done_fn d, void *u) {
    (void)c;
    if (!age || !terms || strlen(pw) < 10) return finish(NAPPLE_ONLINE_ERR_REJECTED, "{\"reason\":\"password_too_short\"}", d, u, nil);
    strlcpy(g_pending_email, email, sizeof g_pending_email);
    (void)nick;
    return finish(0, "{}", d, u, nil);
}
static uint64_t s_verify(void *c, const char *email, const char *code, napple_online_done_fn d, void *u) {
    (void)c;
    if (strcmp(code, "123456")) return finish(NAPPLE_ONLINE_ERR_REJECTED, "{\"reason\":\"bad_code\"}", d, u, nil);
    NSString *e = @(email);
    return finish(0, "{}", d, u, ^{
        fe_online_stub_reset(1);
        strlcpy(g_acc.email, e.UTF8String, sizeof g_acc.email);
    });
}
static uint64_t s_login(void *c, const char *email, const char *pw, napple_online_done_fn d, void *u) {
    (void)c;
    if (!strcmp(email, "unverified@napple.lat")) return finish(NAPPLE_ONLINE_ERR_UNVERIFIED, "{\"reason\":\"unverified\"}", d, u, nil);
    if (!strcmp(pw, "wrongpassword")) return finish(NAPPLE_ONLINE_ERR_REJECTED, "{\"reason\":\"invalid_credentials\"}", d, u, nil);
    return finish(0, "{}", d, u, ^{ fe_online_stub_reset(1); });
}
static uint64_t s_logout(void *c, napple_online_done_fn d, void *u) { (void)c; return finish(0, "{}", d, u, ^{ fe_online_stub_reset(0); }); }
static uint64_t s_reset_start(void *c, const char *e, napple_online_done_fn d, void *u) { (void)c; (void)e; return finish(0, "{}", d, u, nil); }
static uint64_t s_reset_finish(void *c, const char *e, const char *code, const char *pw, napple_online_done_fn d, void *u) {
    (void)c; (void)e;
    if (strcmp(code, "123456") || strlen(pw) < 10) return finish(NAPPLE_ONLINE_ERR_REJECTED, "{\"reason\":\"bad_code\"}", d, u, nil);
    return finish(0, "{}", d, u, nil);
}
static uint64_t s_switch(void *c, const char *id, napple_online_done_fn d, void *u) {
    (void)c;
    NSString *i = @(id);
    return finish(0, "{}", d, u, ^{
        napple_online_profile *p = find_prof(i.UTF8String);
        if (p) g_acc.current = *p;
    });
}
static uint64_t s_create(void *c, const char *nick, napple_online_done_fn d, void *u) {
    (void)c;
    if (g_nprof >= NAPPLE_ONLINE_MAX_PROFILES) return finish(NAPPLE_ONLINE_ERR_REJECTED, "{\"reason\":\"profile_limit\"}", d, u, nil);
    NSString *n = @(nick);
    return finish(0, "{}", d, u, ^{
        napple_online_profile *p = &g_prof[g_nprof++];
        memset(p, 0, sizeof *p);
        snprintf(p->id, sizeof p->id, "prof-%zu", g_nprof);
        strlcpy(p->nickname, n.UTF8String, sizeof p->nickname);
        p->tier = 1;
        p->icon_id = (int)g_nprof * 5 % 48;
        snprintf(p->friend_code, sizeof p->friend_code, "NP-%012zu", 100000000000ul + g_nprof);
        sync_current();
    });
}
static uint64_t s_update(void *c, const char *id, const char *json, napple_online_done_fn d, void *u) {
    (void)c;
    NSDictionary *j = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:json length:strlen(json)] options:0 error:nil];
    NSString *i = @(id);
    if ([j[@"nickname"] isKindOfClass:NSString.class] && [j[@"nickname"] isEqual:@"taken"]) return finish(NAPPLE_ONLINE_ERR_REJECTED, "{\"reason\":\"nickname_taken\"}", d, u, nil);
    return finish(0, "{}", d, u, ^{
        napple_online_profile *p = find_prof(i.UTF8String);
        if (!p || ![j isKindOfClass:NSDictionary.class]) return;
        if ([j[@"nickname"] isKindOfClass:NSString.class]) strlcpy(p->nickname, [j[@"nickname"] UTF8String], sizeof p->nickname);
        if ([j[@"bio"] isKindOfClass:NSString.class]) strlcpy(p->bio, [j[@"bio"] UTF8String], sizeof p->bio);
        if ([j[@"icon_id"] isKindOfClass:NSNumber.class]) p->icon_id = [j[@"icon_id"] intValue];
        if ([j[@"links"] isKindOfClass:NSArray.class]) {
            p->link_count = 0;
            for (NSDictionary *l in j[@"links"]) {
                if (p->link_count >= NAPPLE_ONLINE_MAX_LINKS) break;
                strlcpy(p->links[p->link_count].kind, [l[@"kind"] UTF8String], 12);
                strlcpy(p->links[p->link_count].value, [l[@"value"] UTF8String], 128);
                p->link_count++;
            }
        }
        sync_current();
    });
}
static uint64_t s_photo(void *c, const char *id, const char *path, napple_online_done_fn d, void *u) {
    (void)c;
    NSString *i = @(id), *p = @(path);
    if (![NSFileManager.defaultManager fileExistsAtPath:p]) return finish(NAPPLE_ONLINE_ERR_REJECTED, "{\"reason\":\"unreadable_image\"}", d, u, nil);
    return finish(0, "{}", d, u, ^{
        napple_online_profile *q = find_prof(i.UTF8String);
        if (q) strlcpy(q->photo_path, p.UTF8String, sizeof q->photo_path);
        sync_current();
    });
}
static uint64_t s_tier(void *c, const char *id, int tier, napple_online_done_fn d, void *u) {
    (void)c;
    NSString *i = @(id);
    return finish(0, "{}", d, u, ^{
        napple_online_profile *p = find_prof(i.UTF8String);
        if (p) p->tier = tier;
        sync_current();
    });
}
static uint64_t s_p1(void *c, const char *a, napple_online_done_fn d, void *u) { (void)c; (void)a; return finish(0, "{}", d, u, nil); }
static uint64_t s_search(void *c, const char *q, napple_online_done_fn d, void *u) {
    (void)c; (void)q;
    return finish(0, "{\"results\":[]}", d, u, nil);
}
static uint64_t s_p2(void *c, const char *a, const char *b, napple_online_done_fn d, void *u) { (void)c; (void)a; (void)b; return finish(0, "{}", d, u, nil); }
static uint64_t s_dev_approve(void *c, const char *id, napple_online_done_fn d, void *u) {
    (void)c;
    NSString *i = @(id);
    return finish(0, "{}", d, u, ^{
        for (size_t k = 0; k < g_ndev; k++)
            if (!strcmp(g_dev[k].id, i.UTF8String)) g_dev[k].pending = 0;
    });
}
static uint64_t s_dev_revoke(void *c, const char *id, napple_online_done_fn d, void *u) {
    (void)c;
    NSString *i = @(id);
    return finish(0, "{}", d, u, ^{
        for (size_t k = 0; k < g_ndev; k++)
            if (!strcmp(g_dev[k].id, i.UTF8String)) {
                memmove(&g_dev[k], &g_dev[k + 1], (g_ndev - k - 1) * sizeof g_dev[0]);
                g_ndev--;
                break;
            }
    });
}
static uint64_t s_delete(void *c, const char *pw, napple_online_done_fn d, void *u) {
    (void)c;
    if (!strcmp(pw, "wrongpassword")) return finish(NAPPLE_ONLINE_ERR_REJECTED, "{\"reason\":\"invalid_credentials\"}", d, u, nil);
    return finish(0, "{}", d, u, ^{ fe_online_stub_reset(0); });
}
static void s_set_cb(void *c, napple_online_change_fn f, void *u) { (void)c; g_chg = f; g_chg_user = u; }

const napple_online_ui_api *napple_online_ui_stub(void) {
    static napple_online_ui_api api;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        fe_online_stub_reset(getenv("NAPPLE_ONLINE_STUB_SIGNED_IN") != NULL);
        api.version = NAPPLE_ONLINE_UI_VERSION;
        api.get_account = s_get_account;
        api.get_profiles = s_get_profiles;
        api.get_devices = s_get_devices;
        api.get_friends = s_get_friends;
        api.get_requests = s_get_requests;
        api.get_blocks = s_get_none;
        api.get_recent = s_get_none;
        api.get_stats = s_get_stats;
        api.get_save_status = s_get_saves;
        api.signup = s_signup;
        api.verify = s_verify;
        api.login = s_login;
        api.logout = s_logout;
        api.reset_start = s_reset_start;
        api.reset_finish = s_reset_finish;
        api.switch_profile = s_switch;
        api.create_profile = s_create;
        api.update_profile = s_update;
        api.upload_photo = s_photo;
        api.set_tier = s_tier;
        api.friend_request = s_p1;
        api.friend_accept = s_p1;
        api.friend_decline = s_p1;
        api.friend_remove = s_p1;
        api.block = s_p1;
        api.unblock = s_p1;
        api.search = s_search;
        api.invite = s_p1;
        api.join = s_p2;
        api.report = s_p2;
        api.approve_device = s_dev_approve;
        api.revoke_device = s_dev_revoke;
        api.delete_account = s_delete;
        api.set_change_callback = s_set_cb;
    });
    return &api;
}
