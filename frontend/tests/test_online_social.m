/* Overlay Online social sub-tabs against a canned API: builds each page, clicks the buttons, checks the calls and the toasts, and renders every page
   to a PNG (NAPPLE_SOCIAL_SHOT=<dir>, default $TMPDIR). No servers, no game. */
#import <Cocoa/Cocoa.h>
#include <string.h>
#include "napple_online_social.h"
#include "overlay_online.h"

static int g_checked, g_failed;
#define CHECK(c, ...) do { g_checked++; if (!(c)) { g_failed++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- the shell side, faked ---- */
/* The real shell registry (overlay_online.m) collects the sub-tabs; the pages are built with this test's canned API. */

/* ---- canned API ---- */
static napple_online_friend g_friends[4];
static size_t g_nfriends;
static napple_online_request_entry g_reqs[3], g_blocks[2], g_recent[3];
static size_t g_nreqs, g_nblocks, g_nrecent;
static napple_online_stat g_stats[6];
static char g_calls[1024];
static void log_call(const char *what, const char *arg) { char b[96]; snprintf(b, sizeof b, "%s:%s;", what, arg ? arg : ""); strlcat(g_calls, b, sizeof g_calls); }
static uint64_t finish(napple_online_done_fn d, void *u, int code, const char *json) { d(u, 1, code, json); return 1; }
static size_t cp(const void *src, size_t n, size_t sz, void *out, size_t cap) { memcpy(out, src, (n < cap ? n : cap) * sz); return n; }
static size_t q_friends(void *c, napple_online_friend *o, size_t cap) { (void)c; return cp(g_friends, g_nfriends, sizeof *o, o, cap); }
static size_t q_reqs(void *c, napple_online_request_entry *o, size_t cap) { (void)c; return cp(g_reqs, g_nreqs, sizeof *o, o, cap); }
static size_t q_blocks(void *c, napple_online_request_entry *o, size_t cap) { (void)c; return cp(g_blocks, g_nblocks, sizeof *o, o, cap); }
static size_t q_recent(void *c, napple_online_request_entry *o, size_t cap) { (void)c; return cp(g_recent, g_nrecent, sizeof *o, o, cap); }
static size_t q_stats(void *c, napple_online_stat *o, size_t cap) { (void)c; return cp(g_stats, 6, sizeof *o, o, cap); }
#define ACT1(name, field) static uint64_t name(void *c, const char *a, napple_online_done_fn d, void *u) { (void)c; log_call(field, a); return finish(d, u, 0, "{}"); }
ACT1(a_accept, "accept") ACT1(a_decline, "decline") ACT1(a_remove, "remove") ACT1(a_block, "block") ACT1(a_unblock, "unblock") ACT1(a_invite, "invite") ACT1(a_freq, "freq")
static uint64_t a_join(void *c, const char *p, const char *t, napple_online_done_fn d, void *u) { (void)c; char b[80]; snprintf(b, sizeof b, "%s/%s", p, t); log_call("join", b); return finish(d, u, 0, "{}"); }
static uint64_t a_report(void *c, const char *p, const char *r, napple_online_done_fn d, void *u) { (void)c; char b[80]; snprintf(b, sizeof b, "%s/%s", p, r); log_call("report", b); return finish(d, u, 0, "{}"); }
static uint64_t a_search(void *c, const char *q, napple_online_done_fn d, void *u) {
    (void)c; log_call("search", q);
    return finish(d, u, 0, "{\"results\":[{\"profile_id\":\"p-zed\",\"nickname\":\"zedmaster\",\"icon_id\":9,\"photo_path\":\"\"}]}");
}
static uint64_t a_rejected(void *c, const char *q, napple_online_done_fn d, void *u) { (void)c; (void)q; return finish(d, u, NAPPLE_ONLINE_ERR_REJECTED, "{\"reason\":\"No such friend code.\"}"); }

static napple_online_ui_api g_api;
static NSMutableArray<NSString *> *g_toasts;
static void host_toast(void *u, const char *t) { (void)u; [g_toasts addObject:@(t)]; }

static void fill(void) {
    g_nfriends = 4;
    const char *nick[] = {"mario_fan", "luigi.k", "peach", "bowser99"};
    for (int i = 0; i < 4; i++) { snprintf(g_friends[i].profile_id, 40, "p%d", i); strlcpy(g_friends[i].nickname, nick[i], 24); g_friends[i].icon_id = i * 7; }
    g_friends[0].online = 1; strlcpy(g_friends[0].title_id, "0100152000022000", 20); strlcpy(g_friends[0].mode, "Race", 40); strlcpy(g_friends[0].joinable_token, "tok0", 64);
    g_friends[1].online = 1;
    g_nreqs = 2;
    strlcpy(g_reqs[0].profile_id, "r0", 40); strlcpy(g_reqs[0].nickname, "toad", 24); g_reqs[0].incoming = 1;
    strlcpy(g_reqs[1].profile_id, "r1", 40); strlcpy(g_reqs[1].nickname, "daisy", 24);
    g_nblocks = 1; strlcpy(g_blocks[0].profile_id, "b0", 40); strlcpy(g_blocks[0].nickname, "griefer", 24);
    g_nrecent = 2;
    strlcpy(g_recent[0].profile_id, "x0", 40); strlcpy(g_recent[0].nickname, "ghost_racer", 24);
    strlcpy(g_recent[1].profile_id, "x1", 40); strlcpy(g_recent[1].nickname, "yoshi_main", 24);
    const char *ts[] = {"Mario Kart 8 Deluxe", "Mario Kart 8 Deluxe", "Mario Kart 8 Deluxe", "Super Smash Bros. Ultimate", "Super Smash Bros. Ultimate", "Super Smash Bros. Ultimate"};
    const char *ks[] = {"races", "wins", "best_lap_ms", "matches", "wins", "kos"};
    int64_t vs[] = {120, 41, 83211, 77, 30, 412};
    for (int i = 0; i < 6; i++) { strlcpy(g_stats[i].title_name, ts[i], 64); strlcpy(g_stats[i].key, ks[i], 40); g_stats[i].value = vs[i]; }
    g_api = (napple_online_ui_api){.version = NAPPLE_ONLINE_UI_VERSION, .get_friends = q_friends, .get_requests = q_reqs, .get_blocks = q_blocks, .get_recent = q_recent,
        .get_stats = q_stats, .friend_accept = a_accept, .friend_decline = a_decline, .friend_remove = a_remove, .block = a_block, .unblock = a_unblock, .invite = a_invite,
        .friend_request = a_freq, .join = a_join, .report = a_report, .search = a_search};
}

static void buttons(NSView *v, NSMutableArray *out) {
    if ([v isKindOfClass:NSButton.class]) [out addObject:v];
    for (NSView *s in v.subviews) buttons(s, out);
}
static NSButton *find_btn(NSView *page, NSString *title, int nth) {
    NSMutableArray *b = [NSMutableArray array];
    buttons(page, b);
    for (NSButton *x in b) if ([x.title isEqualToString:title] && nth-- == 0) return x;
    return nil;
}
static NSView *page_for(const char *title) {
    for (NSUInteger i = 0; i < fe_online_subtab_count(); i++) if (!strcmp(fe_online_subtab_title(i).UTF8String, title)) return (__bridge_transfer NSView *)fe_online_subtab_fn(i)(&g_api);
    return nil;
}
static NSWindow *g_win;
static void shot(NSView *page, const char *name) {
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 330, 520) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    w.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    w.backgroundColor = [NSColor colorWithWhite:0.14 alpha:1];
    w.contentView = page;
    [w orderFrontRegardless];
    [page layoutSubtreeIfNeeded];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.15]];
    NSView *frame = w.contentView.superview;
    NSBitmapImageRep *rep = [frame bitmapImageRepForCachingDisplayInRect:frame.bounds];
    [frame cacheDisplayInRect:frame.bounds toBitmapImageRep:rep];
    const char *dir = getenv("NAPPLE_SOCIAL_SHOT") ?: getenv("TMPDIR") ?: "/tmp";
    NSString *path = [NSString stringWithFormat:@"%s/online_social_%s.png", dir, name];
    CHECK([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES], "png %s", path.UTF8String);
    [w orderOut:nil];
    g_win = w;
}
static void clear(void) { g_calls[0] = 0; }

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        g_toasts = [NSMutableArray array];
        fill();
        napple_online_social_host host = {.toast = host_toast};
        napple_online_social_init(&host);
        CHECK(fe_online_subtab_count() == 6, "six sub-tabs registered, got %d", (int)fe_online_subtab_count());
        napple_online_social_init(&host);
        CHECK(fe_online_subtab_count() == 6, "init is idempotent");

        NSView *fr = page_for("Friends");
        CHECK(fr && find_btn(fr, @"Join", 0) && !find_btn(fr, @"Join", 1), "exactly one friend is joinable");
        shot(fr, "friends");
        clear(); [find_btn(fr, @"Join", 0) performClick:nil];
        CHECK(!strcmp(g_calls, "join:p0/tok0;"), "join call %s", g_calls);
        clear(); [find_btn(fr, @"Invite", 1) performClick:nil];
        CHECK(!strcmp(g_calls, "invite:p1;"), "invite goes to the second online friend: %s", g_calls);
        clear(); [find_btn(fr, @"Remove", 0) performClick:nil];
        CHECK(!strcmp(g_calls, "remove:p0;"), "remove %s", g_calls);
        clear(); [find_btn(fr, @"Block", 2) performClick:nil];
        CHECK(!strcmp(g_calls, "block:p2;"), "block %s", g_calls);

        NSView *rq = page_for("Requests");
        shot(rq, "requests");
        clear(); [find_btn(rq, @"Accept", 0) performClick:nil]; [find_btn(rq, @"Decline", 0) performClick:nil]; [find_btn(rq, @"Cancel", 0) performClick:nil];
        CHECK(!strcmp(g_calls, "accept:r0;decline:r0;decline:r1;"), "request buttons %s", g_calls);

        NSView *ad = page_for("Add");
        NSTextField *tf = nil;
        for (NSView *s in ad.subviews.firstObject.subviews) (void)s;
        NSMutableArray *all = [NSMutableArray array];
        buttons(ad, all);
        NSMutableArray *stack = [NSMutableArray arrayWithObject:ad];
        while (stack.count) { NSView *v = stack.lastObject; [stack removeLastObject]; if ([v isKindOfClass:NSTextField.class] && ((NSTextField *)v).isEditable) tf = (NSTextField *)v; [stack addObjectsFromArray:v.subviews]; }
        CHECK(tf != nil, "search field present");
        tf.stringValue = @"zed";
        clear(); [find_btn(ad, @"Search", 0) performClick:nil];
        CHECK(!strcmp(g_calls, "search:zed;"), "search %s", g_calls);
        CHECK(find_btn(ad, @"Add", 0) != nil, "a result row has Add");
        shot(ad, "add");
        clear(); [find_btn(ad, @"Add", 0) performClick:nil];
        CHECK(!strcmp(g_calls, "freq:p-zed;"), "add from result %s", g_calls);
        tf.stringValue = @"np-123456789012";
        clear(); [find_btn(ad, @"Send request", 0) performClick:nil];
        CHECK(!strcmp(g_calls, "freq:NP-123456789012;"), "request by code, upper-cased: %s", g_calls);
        tf.stringValue = @"somebody";
        clear(); [find_btn(ad, @"Send request", 0) performClick:nil];
        CHECK(!g_calls[0], "a nickname is not sent as a code");
        tf.stringValue = @"z";
        clear(); [find_btn(ad, @"Search", 0) performClick:nil];
        CHECK(!g_calls[0], "one-character search is refused locally");
        g_api.friend_request = a_rejected;
        tf.stringValue = @"NP-000000000000";
        [find_btn(ad, @"Send request", 0) performClick:nil];
        stack = [NSMutableArray arrayWithObject:ad]; BOOL saw = NO;
        while (stack.count) { NSView *v = stack.lastObject; [stack removeLastObject]; if ([v isKindOfClass:NSTextField.class] && [((NSTextField *)v).stringValue isEqualToString:@"No such friend code."]) saw = YES; [stack addObjectsFromArray:v.subviews]; }
        CHECK(saw, "server reason shown");

        g_api.friend_request = a_freq;
        NSView *rc = page_for("Recent");
        shot(rc, "recent");
        clear(); [find_btn(rc, @"Report", 1) performClick:nil];
        CHECK(!strcmp(g_calls, "report:x1/Cheating;"), "report %s", g_calls);
        clear(); [find_btn(rc, @"Add", 0) performClick:nil];
        CHECK(!strcmp(g_calls, "freq:x0;"), "recent add %s", g_calls);

        NSView *bl = page_for("Blocked");
        shot(bl, "blocked");
        clear(); [find_btn(bl, @"Unblock", 0) performClick:nil];
        CHECK(!strcmp(g_calls, "unblock:b0;"), "unblock %s", g_calls);

        shot(page_for("Stats"), "stats");

        /* toasts: the first change only primes, a new incoming request toasts once, outgoing ones never */
        napple_online_social_changed(&g_api);
        CHECK(g_toasts.count == 0, "no toast for requests that were already pending");
        g_nreqs = 3;
        strlcpy(g_reqs[2].profile_id, "r2", 40); strlcpy(g_reqs[2].nickname, "wario", 24); g_reqs[2].incoming = 1;
        napple_online_social_changed(&g_api);
        napple_online_social_changed(&g_api);
        CHECK(g_toasts.count == 1 && [g_toasts[0] isEqualToString:@"wario sent you a friend request"], "request toast: %s", g_toasts.count ? g_toasts[0].UTF8String : "none");
        napple_online_social_invite_event(0, "luigi.k", "Mario Kart 8 Deluxe");
        napple_online_social_invite_event(1, "peach", "");
        CHECK(g_toasts.count == 3 && [g_toasts[1] isEqualToString:@"luigi.k invited you to play Mario Kart 8 Deluxe"] && [g_toasts[2] isEqualToString:@"peach accepted your invite"], "invite toasts");
    }
    printf("online social: %d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
