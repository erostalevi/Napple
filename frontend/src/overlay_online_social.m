/* Overlay Online tab, social sub-tabs: Friends (presence, join/invite, remove, block), Requests (accept/decline), Add (friend code or tier-aware
   search), Blocked, Recent players (add, block, report) and Stats (all titles of the current profile). Built on napple_online_ui.h only.
   Every control is a plain NSButton so the overlay's controller navigation (FENav) reaches it; text goes through the host's on-screen keyboard. */
#include <stdio.h>
#include <string.h>
#include "fe_internal.h"
#include "napple_online_social.h"

#define CAP 64
static napple_online_social_host g_host;
static NSHashTable<NSView *> *g_pages;       /* live pages, refreshed on every change */
static NSMutableSet<NSString *> *g_seen_req; /* incoming requests already toasted */
static BOOL g_seen_primed;

typedef NS_ENUM(int, SocialKind) { SK_FRIENDS, SK_REQUESTS, SK_ADD, SK_BLOCKED, SK_RECENT, SK_STATS };

static NSString *S(const char *c) { return c && c[0] ? @(c) : @""; }
static NSButton *btn(NSString *title, CGFloat w, void (^blk)(void)) {
    NSButton *b = [NSButton buttonWithTitle:title target:nil action:nil];
    b.bezelStyle = NSBezelStyleRounded;
    b.controlSize = NSControlSizeSmall;
    b.font = [NSFont systemFontOfSize:11];
    fe_bind(b, ^(id x) { (void)x; blk(); });
    [b.widthAnchor constraintEqualToConstant:w].active = YES;
    return b;
}
static NSTextField *dim(NSString *t) {
    NSTextField *n = fe_label(t, 10, NO);
    n.textColor = NSColor.secondaryLabelColor;
    n.lineBreakMode = NSLineBreakByTruncatingTail;
    return n;
}
/* Round avatar: the cached photo when there is one, else a coloured disc with the built-in icon number (the shell owns the real icon art). */
static NSView *avatar(const char *photo, int icon_id) {
    NSImage *img = photo && photo[0] ? [[NSImage alloc] initWithContentsOfFile:@(photo)] : nil;
    NSImageView *iv = [NSImageView new];
    NSSize sz = NSMakeSize(30, 30);
    [iv.widthAnchor constraintEqualToConstant:sz.width].active = YES;
    [iv.heightAnchor constraintEqualToConstant:sz.height].active = YES;
    if (!img) {
        img = [NSImage imageWithSize:sz flipped:NO drawingHandler:^BOOL(NSRect r) {
            [[NSColor colorWithHue:fmod(icon_id * 0.137, 1.0) saturation:0.55 brightness:0.8 alpha:1] setFill];
            [[NSBezierPath bezierPathWithOvalInRect:r] fill];
            NSString *s = [NSString stringWithFormat:@"%d", icon_id];
            NSDictionary *a = @{NSFontAttributeName : [NSFont boldSystemFontOfSize:11], NSForegroundColorAttributeName : NSColor.whiteColor};
            NSSize ts = [s sizeWithAttributes:a];
            [s drawAtPoint:NSMakePoint((r.size.width - ts.width) / 2, (r.size.height - ts.height) / 2) withAttributes:a];
            return YES;
        }];
    }
    iv.image = img;
    iv.wantsLayer = YES;
    iv.layer.cornerRadius = 15;
    iv.layer.masksToBounds = YES;
    return iv;
}
static NSView *entry_row(NSView *av, NSString *name, NSString *sub, NSArray<NSView *> *buttons) {
    NSTextField *n = fe_label(name, 12, YES);
    n.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView *txt = [NSStackView stackViewWithViews:@[ n, dim(sub) ]];
    txt.orientation = NSUserInterfaceLayoutOrientationVertical;
    txt.alignment = NSLayoutAttributeLeading;
    txt.spacing = 0;
    [txt setContentCompressionResistancePriority:200 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSMutableArray *v = [NSMutableArray arrayWithObjects:av, txt, nil];
    NSView *flex = [NSView new];
    [flex setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [v addObject:flex];
    [v addObjectsFromArray:buttons];
    NSStackView *r = [NSStackView stackViewWithViews:v];
    r.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    r.spacing = 6;
    r.alignment = NSLayoutAttributeCenterY;
    return r;
}

static void done_tramp(void *user, uint64_t rid, int code, const char *json) {
    (void)rid;
    void (^blk)(int, NSString *) = (__bridge_transfer void (^)(int, NSString *))user;
    blk(code, json ? @(json) : @"");
}

@interface FESocialPage : NSStackView
@property(nonatomic) const napple_online_ui_api *api;
@property(nonatomic) SocialKind kind;
@property(nonatomic, strong) NSTextField *status, *field;
@property(nonatomic, strong) NSPopUpButton *reason;
@property(nonatomic, strong) NSArray<NSDictionary *> *results; /* last search */
- (void)refresh;
@end

@implementation FESocialPage
- (void)say:(NSString *)t { self.status.stringValue = t ?: @""; }
/* Start an async action; its completion fills the status line and refreshes the page. */
- (void)act:(uint64_t (^)(napple_online_done_fn, void *))start ok:(NSString *)okText {
    __weak FESocialPage *w = self;
    void (^done)(int, NSString *) = ^(int code, NSString *json) {
        FESocialPage *s = w;
        if (!s) return;
        id o = json.length ? [NSJSONSerialization JSONObjectWithData:[json dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil] : nil;
        NSDictionary *d = [o isKindOfClass:NSDictionary.class] ? o : nil;
        NSString *why = [d[@"reason"] isKindOfClass:NSString.class] ? d[@"reason"] : @"";
        if (code == NAPPLE_ONLINE_OK) {
            if ([d[@"results"] isKindOfClass:NSArray.class]) {
                s.results = d[@"results"];
                [s say:s.results.count ? @"" : @"No players found."];
            } else [s say:okText];
        } else if (code == NAPPLE_ONLINE_ERR_NETWORK) [s say:@"Server unreachable."];
        else if (code == NAPPLE_ONLINE_ERR_RATE_LIMITED) [s say:@"Too many requests, try again later."];
        else [s say:why.length ? why : @"That did not work."];
        [s refresh];
    };
    void *user = (__bridge_retained void *)[done copy];
    if (!start(done_tramp, user)) done_tramp(user, 0, NAPPLE_ONLINE_ERR_INTERNAL, "");
}
- (void)addRow:(NSView *)r {
    [self addArrangedSubview:r];
    [r.widthAnchor constraintEqualToAnchor:self.widthAnchor constant:-24].active = YES;
}
- (void)empty:(NSString *)t { [self addArrangedSubview:dim(t)]; }
- (NSString *)typed:(NSString *)title {
    char out[64] = "";
    if (g_host.text_input && g_host.text_input(g_host.user, title.UTF8String, self.field.stringValue.UTF8String, out, sizeof out)) {
        self.field.stringValue = @(out);
        return @(out);
    }
    return self.field.stringValue;
}

- (void)refresh {
    const napple_online_ui_api *api = self.api;
    for (NSView *v in [self.arrangedSubviews copy]) {
        if (v == self.status) continue;
        [self removeArrangedSubview:v];
        [v removeFromSuperview];
    }
    __weak FESocialPage *w = self;
    switch (self.kind) {
    case SK_FRIENDS: {
        napple_online_friend f[CAP];
        size_t n = api->get_friends(api->ctx, f, CAP), c = n < CAP ? n : CAP;
        int on = 0;
        for (size_t i = 0; i < c; i++) on += f[i].online;
        [self addArrangedSubview:fe_label([NSString stringWithFormat:@"Friends (%zu), %d online", n, on], 12, YES)];
        if (!n) [self empty:@"No friends yet. Use Add to send a request."];
        /* playing first, then online, then offline */
        for (int pass = 0; pass < 3; pass++)
            for (size_t i = 0; i < c; i++) {
                napple_online_friend fr = f[i];
                int cls = fr.online ? (fr.title_id[0] ? 0 : 1) : 2;
                if (cls != pass) continue;
                NSString *pid = @(fr.profile_id), *tok = @(fr.joinable_token);
                NSString *sub = !fr.online ? @"Offline" : fr.title_id[0] ? [NSString stringWithFormat:@"Playing %s%s%s%s", fr.title_id, fr.mode[0] ? " (" : "", fr.mode, fr.mode[0] ? ")" : ""] : @"Online";
                NSMutableArray *b = [NSMutableArray array];
                if (fr.joinable_token[0]) [b addObject:btn(@"Join", 44, ^(void) { [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->join(api->ctx, pid.UTF8String, tok.UTF8String, d, u); } ok:@"Joining..."]; })];
                if (fr.online) [b addObject:btn(@"Invite", 50, ^(void) { [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->invite(api->ctx, pid.UTF8String, d, u); } ok:@"Invite sent."]; })];
                [b addObject:btn(@"Remove", 56, ^(void) { [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->friend_remove(api->ctx, pid.UTF8String, d, u); } ok:@"Friend removed."]; })];
                [b addObject:btn(@"Block", 44, ^(void) { [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->block(api->ctx, pid.UTF8String, d, u); } ok:@"Blocked."]; })];
                [self addRow:entry_row(avatar(fr.photo_path, fr.icon_id), @(fr.nickname), sub, b)];
            }
        break;
    }
    case SK_REQUESTS: {
        napple_online_request_entry r[CAP];
        size_t n = api->get_requests(api->ctx, r, CAP), c = n < CAP ? n : CAP;
        [self addArrangedSubview:fe_label(@"Friend requests", 12, YES)];
        if (!n) [self empty:@"No pending requests."];
        for (int pass = 1; pass >= 0; pass--)
            for (size_t i = 0; i < c; i++) {
                if (r[i].incoming != pass) continue;
                NSString *pid = @(r[i].profile_id);
                NSArray *b = pass ? @[ btn(@"Accept", 52, ^(void) { [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->friend_accept(api->ctx, pid.UTF8String, d, u); } ok:@"Friend added."]; }),
                                       btn(@"Decline", 54, ^(void) { [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->friend_decline(api->ctx, pid.UTF8String, d, u); } ok:@"Declined."]; }) ]
                                 : @[ btn(@"Cancel", 52, ^(void) { [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->friend_decline(api->ctx, pid.UTF8String, d, u); } ok:@"Request cancelled."]; }) ];
                [self addRow:entry_row(avatar(r[i].photo_path, r[i].icon_id), @(r[i].nickname), pass ? @"Wants to be your friend" : @"Request sent", b)];
            }
        break;
    }
    case SK_ADD: {
        [self addArrangedSubview:fe_label(@"Add a friend", 12, YES)];
        NSTextField *hint = dim(@"Enter a friend code (NP-...) or a nickname. Players who hid their profile only appear when you know their code.");
        hint.lineBreakMode = NSLineBreakByWordWrapping;
        hint.preferredMaxLayoutWidth = 300;
        [self addArrangedSubview:hint];
        if (!self.field) {
            self.field = [NSTextField textFieldWithString:@""];
            self.field.placeholderString = @"NP-123456789012 or nickname";
            self.field.font = [NSFont systemFontOfSize:12];
        }
        [self addRow:self.field];
        NSString *(^q)(void) = ^NSString *{ return [w.field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]; };
        NSView *keys = [NSStackView stackViewWithViews:@[
            btn(@"Type...", 60, ^(void) { [w typed:@"Friend code or nickname"]; }),
            btn(@"Search", 56, ^(void) {
                NSString *s = q();
                if (s.length < 2) { [w say:@"Type at least 2 characters."]; return; }
                [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->search(api->ctx, s.UTF8String, d, u); } ok:@""];
            }),
            btn(@"Send request", 92, ^(void) {
                NSString *s = q();
                if (![s.uppercaseString hasPrefix:@"NP-"]) { [w say:@"Send by code needs an NP- friend code; use Search for nicknames."]; return; }
                [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->friend_request(api->ctx, s.uppercaseString.UTF8String, d, u); } ok:@"Request sent."];
            }) ]];
        [self addArrangedSubview:keys];
        for (NSDictionary *res in self.results) {
            NSString *pid = res[@"profile_id"] ?: @"", *nick = res[@"nickname"] ?: @"?";
            NSString *photo = res[@"photo_path"] ?: @"";
            [self addRow:entry_row(avatar(photo.UTF8String, [res[@"icon_id"] intValue]), nick, @"Found by search",
                                   @[ btn(@"Add", 40, ^(void) { [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->friend_request(api->ctx, pid.UTF8String, d, u); } ok:@"Request sent."]; }) ])];
        }
        break;
    }
    case SK_BLOCKED: {
        napple_online_request_entry r[CAP];
        size_t n = api->get_blocks(api->ctx, r, CAP), c = n < CAP ? n : CAP;
        [self addArrangedSubview:fe_label([NSString stringWithFormat:@"Blocked players (%zu)", n], 12, YES)];
        if (!n) [self empty:@"You have not blocked anyone."];
        for (size_t i = 0; i < c; i++) {
            NSString *pid = @(r[i].profile_id);
            [self addRow:entry_row(avatar(r[i].photo_path, r[i].icon_id), @(r[i].nickname), @"Cannot see you or contact you",
                                   @[ btn(@"Unblock", 56, ^(void) { [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->unblock(api->ctx, pid.UTF8String, d, u); } ok:@"Unblocked."]; }) ])];
        }
        break;
    }
    case SK_RECENT: {
        napple_online_request_entry r[CAP];
        size_t n = api->get_recent(api->ctx, r, CAP), c = n < CAP ? n : CAP;
        [self addArrangedSubview:fe_label(@"Recent players", 12, YES)];
        if (!self.reason) self.reason = fe_popup(@[ @"Cheating", @"Harassment", @"Inappropriate name or photo", @"Other" ], 0, ^(id x) { (void)x; });
        [self addRow:[NSStackView stackViewWithViews:@[ fe_label(@"Report reason", 11, NO), self.reason ]]];
        if (!n) [self empty:@"Nobody yet. Players you meet online show up here."];
        for (size_t i = 0; i < c; i++) {
            NSString *pid = @(r[i].profile_id);
            NSArray *b = @[
                btn(@"Add", 36, ^(void) { [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->friend_request(api->ctx, pid.UTF8String, d, u); } ok:@"Request sent."]; }),
                btn(@"Report", 50, ^(void) {
                    NSString *why = w.reason.titleOfSelectedItem ?: @"Other";
                    [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->report(api->ctx, pid.UTF8String, why.UTF8String, d, u); } ok:@"Report sent. Thank you."];
                }),
                btn(@"Block", 44, ^(void) { [w act:^uint64_t(napple_online_done_fn d, void *u) { return api->block(api->ctx, pid.UTF8String, d, u); } ok:@"Blocked."]; })
            ];
            [self addRow:entry_row(avatar(r[i].photo_path, r[i].icon_id), @(r[i].nickname), @"Played with you recently", b)];
        }
        break;
    }
    case SK_STATS: {
        napple_online_stat st[256];
        size_t n = api->get_stats(api->ctx, st, 256), c = n < 256 ? n : 256;
        [self addArrangedSubview:fe_label(@"Stats", 12, YES)];
        if (!n) [self empty:@"No stats yet. They fill in as you play online."];
        NSString *last = nil;
        for (size_t i = 0; i < c; i++) {
            NSString *t = st[i].title_name[0] ? @(st[i].title_name) : @(st[i].title_id);
            if (![t isEqualToString:last]) { [self addArrangedSubview:fe_label(t, 11, YES)]; last = t; }
            NSTextField *k = fe_label(@(st[i].key), 11, NO), *v = fe_label([NSString stringWithFormat:@"%lld", (long long)st[i].value], 11, NO);
            v.alignment = NSTextAlignmentRight;
            NSView *flex = [NSView new];
            [flex setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
            [self addRow:[NSStackView stackViewWithViews:@[ k, flex, v ]]];
        }
        break;
    }
    }
    /* keep the status line last */
    if ([self.arrangedSubviews containsObject:self.status]) [self removeArrangedSubview:self.status];
    [self addArrangedSubview:self.status];
}
@end

@interface FEFlipView : NSView
@end
@implementation FEFlipView
- (BOOL)isFlipped { return YES; }
@end

static void *make_page(const napple_online_ui_api *api, SocialKind kind) {
    FESocialPage *p = [FESocialPage new];
    p.api = api;
    p.kind = kind;
    p.orientation = NSUserInterfaceLayoutOrientationVertical;
    p.alignment = NSLayoutAttributeLeading;
    p.spacing = 8;
    p.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
    p.status = dim(@"");
    p.status.maximumNumberOfLines = 2;
    [p refresh];
    if (!g_pages) g_pages = [NSHashTable weakObjectsHashTable];
    [g_pages addObject:p];
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 330, 500)];
    sv.hasVerticalScroller = YES;
    sv.drawsBackground = NO;
    sv.autohidesScrollers = YES;
    /* A flipped document view keeps the list at the top whatever its height. */
    FEFlipView *doc = [FEFlipView new];
    p.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:p];
    sv.documentView = doc;
    NSClipView *clip = sv.contentView;
    doc.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [doc.topAnchor constraintEqualToAnchor:clip.topAnchor], [doc.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
        [doc.widthAnchor constraintEqualToAnchor:clip.widthAnchor],
        [p.topAnchor constraintEqualToAnchor:doc.topAnchor], [p.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor],
        [p.trailingAnchor constraintEqualToAnchor:doc.trailingAnchor], [p.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor]
    ]];
    return (__bridge_retained void *)sv;
}
static void *page_friends(const napple_online_ui_api *a) { return make_page(a, SK_FRIENDS); }
static void *page_requests(const napple_online_ui_api *a) { return make_page(a, SK_REQUESTS); }
static void *page_add(const napple_online_ui_api *a) { return make_page(a, SK_ADD); }
static void *page_blocked(const napple_online_ui_api *a) { return make_page(a, SK_BLOCKED); }
static void *page_recent(const napple_online_ui_api *a) { return make_page(a, SK_RECENT); }
static void *page_stats(const napple_online_ui_api *a) { return make_page(a, SK_STATS); }

static void toast(NSString *t) {
    fprintf(stderr, "[online-social] toast: %s\n", t.UTF8String);
    if (g_host.toast) g_host.toast(g_host.user, t.UTF8String);
}

void napple_online_social_init(const napple_online_social_host *host) {
    if (host) g_host = *host;
    static BOOL done;
    if (done) return;
    done = YES;
    napple_online_register_subtab("Friends", page_friends);
    napple_online_register_subtab("Requests", page_requests);
    napple_online_register_subtab("Add", page_add);
    napple_online_register_subtab("Recent", page_recent);
    napple_online_register_subtab("Blocked", page_blocked);
    napple_online_register_subtab("Stats", page_stats);
}

void napple_online_social_changed(const napple_online_ui_api *api) {
    if (!api) return;
    napple_online_request_entry r[CAP];
    size_t n = api->get_requests(api->ctx, r, CAP), c = n < CAP ? n : CAP;
    if (!g_seen_req) g_seen_req = [NSMutableSet set];
    NSMutableSet *now = [NSMutableSet set];
    for (size_t i = 0; i < c; i++) {
        if (!r[i].incoming) continue;
        NSString *id_ = @(r[i].profile_id);
        [now addObject:id_];
        if (g_seen_primed && ![g_seen_req containsObject:id_]) toast([NSString stringWithFormat:@"%s sent you a friend request", r[i].nickname]);
    }
    [g_seen_req setSet:now];
    g_seen_primed = YES; /* the first call only records what was already pending */
    for (FESocialPage *p in g_pages.allObjects) [p refresh];
}

void napple_online_social_invite_event(int kind, const char *nickname, const char *title_name) {
    NSString *nick = S(nickname), *title = S(title_name);
    toast(kind == 0 ? [NSString stringWithFormat:@"%@ invited you to play%@", nick, title.length ? [@" " stringByAppendingString:title] : @""]
                    : [NSString stringWithFormat:@"%@ accepted your invite", nick]);
}
