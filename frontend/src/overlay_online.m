/* The overlay's Online tab: signed-out screens (sign in, create account, 6-digit code, password reset), signed-in header, profile editor, tier picker,
   profile switcher, devices, cloud-save status and account deletion. Uses only napple_online_ui_api (the runtime's table, or the stub).
   Every text field is a button that opens the software keyboard (applet text bar), so the controller can drive every screen. */
#include <string.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "overlay_online.h"

static const napple_online_ui_api *g_api;     /* installed table (copied) */
static napple_online_ui_api g_api_copy;
static NSView *g_host;                         /* the page returned by fe_online_page */
static __weak NappleFE *g_fe;
static NSString *g_screen = @"signin";
static NSMutableDictionary<NSString *, id> *g_form;
static NSString *g_status;
static BOOL g_status_err, g_busy;
static NSInteger g_subtab;                     /* 0 Account, 1.. registered sub-tabs */
static NSString *g_photo_preview;              /* chosen but not uploaded photo path */

typedef struct {
    NSString *title;
    napple_online_page_fn make;
} subtab_t;
static NSMutableArray<NSValue *> *g_subtabs;
static NSMutableArray<NSString *> *g_subtab_titles;

void napple_online_register_subtab(const char *title, napple_online_page_fn make) {
    if (!g_subtabs) {
        g_subtabs = [NSMutableArray new];
        g_subtab_titles = [NSMutableArray new];
    }
    [g_subtab_titles addObject:@(title)];
    [g_subtabs addObject:[NSValue valueWithPointer:(const void *)make]];
}
NSUInteger fe_online_subtab_count(void) { return g_subtabs.count; }
NSString *fe_online_subtab_title(NSUInteger i) { return g_subtab_titles[i]; }
napple_online_page_fn fe_online_subtab_fn(NSUInteger i) { return (napple_online_page_fn)[g_subtabs[i] pointerValue]; }
NSView *fe_online_subtab_make(NSUInteger i) {
    napple_online_page_fn f = (napple_online_page_fn)[g_subtabs[i] pointerValue];
    return (__bridge_transfer NSView *)f(fe_online_api());
}

const napple_online_ui_api *fe_online_api(void) { return g_api ? g_api : napple_online_ui_stub(); }
void napple_frontend_set_online_ui(struct napple_frontend *fe, const napple_online_ui_api *api) {
    (void)fe;
    if (api && api->version == NAPPLE_ONLINE_UI_VERSION) {
        g_api_copy = *api;
        g_api = &g_api_copy;
    } else g_api = NULL;
    g_api = g_api ? g_api : NULL;
    if (g_api) g_api->set_change_callback(g_api->ctx, NULL, NULL);
}

static void rebuild(void);
static void on_change(void *u) {
    (void)u;
    rebuild();
}
#define API (fe_online_api())
#define CTX (API->ctx)

/* ---- small widgets ---- */
static NSString *S(const char *s) { return [NSString stringWithUTF8String:s]; }
static NSMutableDictionary *form(void) { return g_form ?: (g_form = [NSMutableDictionary new]); }
static NSString *fv(NSString *k) { return form()[k] ?: @""; }
void fe_online_debug_set(NSString *key, NSString *value) { form()[key] = value; }

static NSButton *btn(NSString *title, CGFloat w, void (^blk)(void)) {
    NSButton *b = [NSButton buttonWithTitle:title target:nil action:nil];
    b.bezelStyle = NSBezelStyleRounded;
    b.controlSize = NSControlSizeSmall;
    b.font = [NSFont systemFontOfSize:11];
    if (w > 0) [b.widthAnchor constraintEqualToConstant:w].active = YES;
    fe_bind(b, ^(id x) { (void)x; blk(); });
    return b;
}
static NSTextField *wrap(NSString *t, CGFloat size, NSColor *col) {
    NSTextField *l = [NSTextField wrappingLabelWithString:t];
    l.font = [NSFont systemFontOfSize:size];
    l.textColor = col ?: NSColor.secondaryLabelColor;
    l.preferredMaxLayoutWidth = 340;
    return l;
}
static NSTextField *head(NSString *t) { return fe_label(t, 13, YES); }
static NSStackView *hrow(NSArray *views) {
    NSStackView *r = [NSStackView stackViewWithViews:views];
    r.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    r.spacing = 8;
    r.alignment = NSLayoutAttributeCenterY;
    return r;
}

static BOOL accept_env_answer(NSString *key, BOOL pw) {
    const char *a = getenv("NAPPLE_SWKBD_ANSWER");
    (void)pw;
    if (!a) return NO;
    if (strcmp(a, "<cancel>")) form()[key] = S(a);
    return YES;
}
static void text_done(void *u, int ok, const char *t) {
    void (^got)(int, NSString *) = (__bridge_transfer void (^)(int, NSString *))u;
    got(ok, t ? S(t) : nil);
}
/* A text field: a button showing the value (dots for passwords); pressing it opens the software keyboard. */
static NSView *field(NSString *label, NSString *key, NSString *guide, BOOL password, uint32_t maxlen, BOOL numeric) {
    NSString *v = fv(key);
    NSString *shown = v.length ? (password ? [@"" stringByPaddingToLength:v.length withString:@"•" startingAtIndex:0] : v) : guide;
    NSButton *b = btn(shown, 0, ^{
        if (accept_env_answer(key, password)) {
            rebuild();
            return;
        }
        if (!g_fe) return;
        napple_text_request rq = {.header = label.UTF8String, .guide = guide.UTF8String, .initial = fv(key).UTF8String, .max_length = maxlen, .password = password, .numeric = numeric};
        void (^got)(int, NSString *) = ^(int ok, NSString *t) {
            if (ok && t) form()[key] = t;
            rebuild();
        };
        fe_textbar_show(g_fe, &rq, text_done, (__bridge_retained void *)[got copy]);
    });
    b.alignment = NSTextAlignmentLeft;
    b.contentTintColor = v.length ? nil : NSColor.tertiaryLabelColor;
    [b.widthAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
    NSTextField *l = fe_label(label, 11, NO);
    [l.widthAnchor constraintEqualToConstant:84].active = YES;
    return hrow(@[ l, b ]);
}

/* ---- request plumbing ---- */
static void set_status(NSString *s, BOOL err) {
    g_status = s;
    g_status_err = err;
}
static NSString *reason_text(int rc, NSDictionary *j) {
    NSString *r = [j isKindOfClass:NSDictionary.class] ? j[@"reason"] : nil;
    NSDictionary *map = @{
        @"invalid_credentials" : @"Email or password is wrong.",
        @"bad_code" : @"That code is wrong or expired.",
        @"password_too_short" : @"Password must be at least 10 characters.",
        @"nickname_taken" : @"That nickname is taken.",
        @"profile_limit" : @"You already have 8 profiles.",
        @"unreadable_image" : @"That image could not be read (JPEG, PNG or WebP, up to 5 MiB).",
        @"unverified" : @"This account is not verified yet. A new code was sent.",
    };
    if (r && map[r]) return map[r];
    switch (rc) {
    case NAPPLE_ONLINE_ERR_NETWORK: return @"Can't reach the Napple server. Check your connection and try again.";
    case NAPPLE_ONLINE_ERR_RATE_LIMITED: return @"Too many attempts. Wait a few minutes and try again.";
    case NAPPLE_ONLINE_ERR_UNVERIFIED: return map[@"unverified"];
    case NAPPLE_ONLINE_ERR_INVALID: return r ?: @"Check the highlighted fields.";
    default: return r ? [NSString stringWithFormat:@"Request failed (%@).", r] : @"Something went wrong. Try again.";
    }
}
static void completion(void *user, uint64_t id, int rc, const char *json) {
    (void)id;
    void (^blk)(int, NSDictionary *) = (__bridge_transfer void (^)(int, NSDictionary *))user;
    NSDictionary *j = json ? [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:json length:strlen(json)] options:0 error:nil] : nil;
    g_busy = NO;
    blk(rc, j);
}
/* Starts an action: start(done, user) calls the API; then(rc, json) runs at completion on the main thread (set status / next screen there). */
static void run(NSString *busy, uint64_t (^start)(napple_online_done_fn, void *), void (^then)(int, NSDictionary *)) {
    if (g_busy) return;
    g_busy = YES;
    set_status(busy, NO);
    void (^blk)(int, NSDictionary *) = ^(int rc, NSDictionary *j) {
        then(rc, j);
        rebuild();
    };
    start(completion, (__bridge_retained void *)[blk copy]);
    rebuild();
}
static void go(NSString *screen) {
    g_screen = screen;
    set_status(nil, NO);
    rebuild();
}
static void fail(NSString *msg) {
    set_status(msg, YES);
    rebuild();
}
static BOOL valid_email(NSString *e) {
    NSRange at = [e rangeOfString:@"@"];
    return at.location != NSNotFound && at.location > 0 && [e rangeOfString:@"." options:0 range:NSMakeRange(at.location, e.length - at.location)].location != NSNotFound;
}
static BOOL valid_nick(NSString *n) {
    if (n.length < 3 || n.length > 20) return NO;
    NSCharacterSet *ok = [NSCharacterSet characterSetWithCharactersInString:@"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-"];
    return [n rangeOfCharacterFromSet:ok.invertedSet].location == NSNotFound;
}

/* ---- icons / photo ---- */
static NSArray<NSString *> *icon_symbols(void) {
    static NSArray *syms;
    if (!syms) {
        syms = @[ @"star.fill", @"heart.fill", @"bolt.fill", @"flame.fill", @"leaf.fill", @"moon.fill", @"sun.max.fill", @"cloud.fill", @"snowflake", @"drop.fill",
                  @"pawprint.fill", @"hare.fill", @"tortoise.fill", @"bird.fill", @"fish.fill", @"ant.fill", @"ladybug.fill", @"car.fill", @"airplane", @"bicycle",
                  @"gamecontroller.fill", @"trophy.fill", @"crown.fill", @"flag.fill", @"bell.fill", @"music.note", @"guitars.fill", @"paintbrush.fill", @"camera.fill", @"gift.fill",
                  @"cup.and.saucer.fill", @"fork.knife", @"birthday.cake.fill", @"globe", @"mountain.2.fill", @"tree.fill", @"camera.macro", @"sparkles", @"paperplane.fill", @"shield.fill",
                  @"key.fill", @"wand.and.stars", @"puzzlepiece.fill", @"dice.fill", @"cube.fill", @"diamond.fill", @"hexagon.fill", @"seal.fill" ];
    }
    return syms;
}
/* Test aid: how many built-in icon symbols this macOS does not have (they draw as plain circles). */
int fe_online_missing_icon_symbols(void) {
    int n = 0;
    for (NSString *s in icon_symbols())
        if (![NSImage imageWithSystemSymbolName:s accessibilityDescription:nil]) n++;
    return n;
}
static NSImage *icon_image(int iid, CGFloat sz) {
    NSArray *syms = icon_symbols();
    iid = iid < 0 ? 0 : iid % NAPPLE_ONLINE_ICON_COUNT;
    return [NSImage imageWithSize:NSMakeSize(sz, sz) flipped:NO drawingHandler:^BOOL(NSRect r) {
        NSColor *c = [NSColor colorWithHue:(iid * 37 % 360) / 360.0 saturation:0.55 brightness:0.75 alpha:1];
        [c setFill];
        [[NSBezierPath bezierPathWithOvalInRect:r] fill];
        NSImage *g = [NSImage imageWithSystemSymbolName:syms[(NSUInteger)iid] accessibilityDescription:nil];
        if (g) {
            NSImageSymbolConfiguration *cfg = [NSImageSymbolConfiguration configurationWithPointSize:sz * 0.45 weight:NSFontWeightSemibold];
            g = [g imageWithSymbolConfiguration:cfg];
            NSImage *w = [g copy];
            [w lockFocus];
            [NSColor.whiteColor set];
            NSRectFillUsingOperation(NSMakeRect(0, 0, w.size.width, w.size.height), NSCompositingOperationSourceAtop);
            [w unlockFocus];
            NSSize gs = w.size;
            [w drawInRect:NSMakeRect(NSMidX(r) - gs.width / 2, NSMidY(r) - gs.height / 2, gs.width, gs.height)];
        }
        return YES;
    }];
}
/* Profile picture as a circle: the photo when there is one, else the built-in icon. */
static NSImageView *avatar(const char *photo, int icon, CGFloat sz) {
    NSImage *im = nil;
    if (photo && photo[0]) {
        NSImage *p = [[NSImage alloc] initWithContentsOfFile:S(photo)];
        if (p)
            im = [NSImage imageWithSize:NSMakeSize(sz, sz) flipped:NO drawingHandler:^BOOL(NSRect r) {
                [[NSBezierPath bezierPathWithOvalInRect:r] addClip];
                [p drawInRect:r];
                return YES;
            }];
    }
    NSImageView *v = [NSImageView imageViewWithImage:im ?: icon_image(icon, sz)];
    [v.widthAnchor constraintEqualToConstant:sz].active = YES;
    [v.heightAnchor constraintEqualToConstant:sz].active = YES;
    return v;
}
static NSString *tier_name(int t) {
    switch (t) {
    case 1: return @"Public";
    case 2: return @"Friends (searchable)";
    case 3: return @"Friends (hidden)";
    case 4: return @"Private";
    case 5: return @"Ghost";
    }
    return @"?";
}
static NSString *tier_desc(int t) {
    switch (t) {
    case 1: return @"Anyone can find you and send a friend request. Everyone sees your profile.";
    case 2: return @"Anyone can find you and ask to be friends. Only friends see your presence and stats.";
    case 3: return @"Nobody can find you. Requests only from players you met in a lobby. Friends see you.";
    case 4: return @"Nobody can find you or send requests. Friends still see you.";
    case 5: return @"Invisible: other players see a random bot name, no profile, and no trace. Not even friends see you.";
    }
    return @"";
}

/* ---- screens ---- */
static NSStackView *new_page(void) {
    NSStackView *s = [NSStackView new];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 9;
    s.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
    return s;
}
static void status_line(NSStackView *p) {
    if (!g_status.length) return;
    NSStackView *r;
    if (g_busy) {
        NSProgressIndicator *pi = [NSProgressIndicator new];
        pi.style = NSProgressIndicatorStyleSpinning;
        pi.controlSize = NSControlSizeSmall;
        [pi startAnimation:nil];
        r = hrow(@[ pi, wrap(g_status, 11, NSColor.labelColor) ]);
    } else
        r = hrow(@[ wrap(g_status, 11, g_status_err ? NSColor.systemRedColor : NSColor.systemGreenColor) ]);
    [p addArrangedSubview:r];
}
static NSButton *back_btn(NSString *to) { return btn(@"‹ Back", 70, ^{ go(to); }); }

static void legal(NSStackView *p) {
    NSButton *t = btn(@"Terms of Service", 0, ^{ [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"https://napple.lat/terms"]]; });
    NSButton *pr = btn(@"Privacy Policy", 0, ^{ [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"https://napple.lat/privacy"]]; });
    [p addArrangedSubview:hrow(@[ t, pr ])];
}

static void screen_signin(NSStackView *p) {
    [p addArrangedSubview:head(@"Napple Online")];
    [p addArrangedSubview:wrap(@"Sign in to play online, keep your saves in the cloud and add friends. One account works on every Mac.", 11, nil)];
    [p addArrangedSubview:field(@"Email", @"email", @"you@example.com", NO, 120, NO)];
    [p addArrangedSubview:field(@"Password", @"password", @"password", YES, 100, NO)];
    status_line(p);
    [p addArrangedSubview:hrow(@[
        btn(@"Sign in", 90, ^{
            if (!valid_email(fv(@"email")) || !fv(@"password").length) return fail(@"Enter your email and password.");
            run(@"Signing in…", ^uint64_t(napple_online_done_fn d, void *u) { return API->login(CTX, fv(@"email").UTF8String, fv(@"password").UTF8String, d, u); },
                ^(int rc, NSDictionary *j) {
                    if (rc == 0) { set_status(nil, NO); form()[@"password"] = @""; g_screen = @"home"; }
                    else if (rc == NAPPLE_ONLINE_ERR_UNVERIFIED) { g_screen = @"verify"; set_status(reason_text(rc, j), NO); }
                    else set_status(reason_text(rc, j), YES);
                });
        }),
        btn(@"Create account", 110, ^{ go(@"signup"); }),
    ])];
    [p addArrangedSubview:btn(@"Forgot password?", 0, ^{ go(@"reset_start"); })];
}
static void screen_signup(NSStackView *p) {
    [p addArrangedSubview:back_btn(@"signin")];
    [p addArrangedSubview:head(@"Create account")];
    [p addArrangedSubview:field(@"Email", @"email", @"you@example.com", NO, 120, NO)];
    [p addArrangedSubview:field(@"Password", @"password", @"10+ characters", YES, 100, NO)];
    [p addArrangedSubview:field(@"Nickname", @"nickname", @"3-20: letters, digits _ . -", NO, 20, NO)];
    [p addArrangedSubview:fe_check(@"I am 13 or older", [form()[@"age"] boolValue], ^(NSButton *b) { form()[@"age"] = @(b.state == NSControlStateValueOn); })];
    [p addArrangedSubview:fe_check(@"I accept the Terms and Privacy Policy", [form()[@"terms"] boolValue], ^(NSButton *b) { form()[@"terms"] = @(b.state == NSControlStateValueOn); })];
    legal(p);
    status_line(p);
    [p addArrangedSubview:btn(@"Create account", 120, ^{
        if (!valid_email(fv(@"email"))) return fail(@"Enter a valid email address.");
        if (fv(@"password").length < 10) return fail(@"Password must be at least 10 characters.");
        if (!valid_nick(fv(@"nickname"))) return fail(@"Nickname: 3-20 characters, letters, digits, _ . -");
        if (![form()[@"age"] boolValue] || ![form()[@"terms"] boolValue]) return fail(@"Confirm that you are 13+ and accept the terms.");
        run(@"Creating account…", ^uint64_t(napple_online_done_fn d, void *u) {
                return API->signup(CTX, fv(@"email").UTF8String, fv(@"password").UTF8String, fv(@"nickname").UTF8String, 1, 1, d, u);
            }, ^(int rc, NSDictionary *j) {
                if (rc == 0) { g_screen = @"verify"; set_status(@"We sent a 6-digit code to your email.", NO); }
                else set_status(reason_text(rc, j), YES);
            });
    })];
}
static void screen_verify(NSStackView *p) {
    [p addArrangedSubview:back_btn(@"signin")];
    [p addArrangedSubview:head(@"Enter your code")];
    [p addArrangedSubview:wrap([NSString stringWithFormat:@"Type the 6-digit code we emailed to %@. It expires in 10 minutes.", fv(@"email")], 11, nil)];
    [p addArrangedSubview:field(@"Code", @"code", @"123456", NO, 6, YES)];
    status_line(p);
    [p addArrangedSubview:btn(@"Verify", 90, ^{
        if (fv(@"code").length != 6) return fail(@"The code has 6 digits.");
        run(@"Verifying…", ^uint64_t(napple_online_done_fn d, void *u) { return API->verify(CTX, fv(@"email").UTF8String, fv(@"code").UTF8String, d, u); },
            ^(int rc, NSDictionary *j) {
                if (rc == 0) { form()[@"code"] = @""; form()[@"password"] = @""; g_screen = @"home"; set_status(nil, NO); }
                else set_status(reason_text(rc, j), YES);
            });
    })];
}
static void screen_reset_start(NSStackView *p) {
    [p addArrangedSubview:back_btn(@"signin")];
    [p addArrangedSubview:head(@"Reset password")];
    [p addArrangedSubview:wrap(@"We will email you a 6-digit code.", 11, nil)];
    [p addArrangedSubview:field(@"Email", @"email", @"you@example.com", NO, 120, NO)];
    status_line(p);
    [p addArrangedSubview:btn(@"Send code", 90, ^{
        if (!valid_email(fv(@"email"))) return fail(@"Enter a valid email address.");
        run(@"Sending…", ^uint64_t(napple_online_done_fn d, void *u) { return API->reset_start(CTX, fv(@"email").UTF8String, d, u); },
            ^(int rc, NSDictionary *j) {
                if (rc == 0) { g_screen = @"reset_finish"; set_status(@"If that email has an account, a code is on its way.", NO); }
                else set_status(reason_text(rc, j), YES);
            });
    })];
}
static void screen_reset_finish(NSStackView *p) {
    [p addArrangedSubview:back_btn(@"reset_start")];
    [p addArrangedSubview:head(@"Choose a new password")];
    [p addArrangedSubview:field(@"Code", @"code", @"123456", NO, 6, YES)];
    [p addArrangedSubview:field(@"New password", @"newpw", @"10+ characters", YES, 100, NO)];
    status_line(p);
    [p addArrangedSubview:btn(@"Change password", 130, ^{
        if (fv(@"code").length != 6) return fail(@"The code has 6 digits.");
        if (fv(@"newpw").length < 10) return fail(@"Password must be at least 10 characters.");
        run(@"Changing password…", ^uint64_t(napple_online_done_fn d, void *u) {
                return API->reset_finish(CTX, fv(@"email").UTF8String, fv(@"code").UTF8String, fv(@"newpw").UTF8String, d, u);
            }, ^(int rc, NSDictionary *j) {
                if (rc == 0) { form()[@"newpw"] = @""; form()[@"code"] = @""; g_screen = @"signin"; set_status(@"Password changed. Sign in with the new one.", NO); }
                else set_status(reason_text(rc, j), YES);
            });
    })];
}

/* signed-in */
static void header(NSStackView *p, const napple_online_account *a) {
    NSImageView *av = avatar(a->current.photo_path, a->current.icon_id, 44);
    NSTextField *nm = fe_label(S(a->current.nickname), 14, YES);
    NSTextField *sub = fe_label([NSString stringWithFormat:@"%@  ·  %@", tier_name(a->current.tier), S(a->current.friend_code)], 10, NO);
    sub.textColor = NSColor.secondaryLabelColor;
    NSStackView *col = [NSStackView stackViewWithViews:@[ nm, sub ]];
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.alignment = NSLayoutAttributeLeading;
    col.spacing = 1;
    [p addArrangedSubview:hrow(@[ av, col ])];
}
static void screen_home(NSStackView *p, const napple_online_account *a) {
    header(p, a);
    NSTextField *em = fe_label(S(a->email), 10, NO);
    em.textColor = NSColor.secondaryLabelColor;
    [p addArrangedSubview:em];
    if (!a->server_reachable) [p addArrangedSubview:wrap(@"Offline: the Napple server can't be reached. Your game still works.", 10, NSColor.systemOrangeColor)];
    status_line(p);
    NSArray *items = @[ @[ @"Edit profile", @"profile" ], @[ @"Visibility", @"tier" ], @[ @"Profiles", @"profiles" ], @[ @"Devices", @"devices" ], @[ @"Cloud saves", @"saves" ] ];
    for (NSArray *it in items) {
        NSString *to = it[1];
        NSButton *b = btn([NSString stringWithFormat:@"%@  ›", it[0]], 0, ^{ go(to); });
        b.alignment = NSTextAlignmentLeft;
        [b.widthAnchor constraintEqualToConstant:340].active = YES;
        [p addArrangedSubview:b];
    }
    napple_online_device ds[16];
    size_t nd = API->get_devices(CTX, ds, 16), pend = 0;
    for (size_t i = 0; i < MIN(nd, 16); i++) pend += ds[i].pending;
    if (pend) [p addArrangedSubview:wrap([NSString stringWithFormat:@"%zu device%s waiting for your approval.", pend, pend == 1 ? "" : "s"], 11, NSColor.systemOrangeColor)];
    [p addArrangedSubview:hrow(@[
        btn(@"Sign out", 80, ^{
            run(@"Signing out…", ^uint64_t(napple_online_done_fn d, void *u) { return API->logout(CTX, d, u); },
                ^(int rc, NSDictionary *j) { if (rc == 0) { g_screen = @"signin"; set_status(nil, NO); } else set_status(reason_text(rc, j), YES); });
        }),
        btn(@"Delete account…", 120, ^{ go(@"delete"); }),
    ])];
}
static NSArray<NSString *> *kKinds(void) { return @[ @"discord", @"instagram", @"x", @"web" ]; }
static void screen_profile(NSStackView *p, const napple_online_account *a) {
    const napple_online_profile *c = &a->current;
    NSString *pid = S(c->id);
    [p addArrangedSubview:back_btn(@"home")];
    [p addArrangedSubview:head(@"Edit profile")];
    NSString *key = [@"loaded:" stringByAppendingString:S(c->id)];
    if (!form()[key]) { /* load the stored values once per profile */
        form()[key] = @YES;
        form()[@"p_nick"] = S(c->nickname);
        form()[@"p_bio"] = S(c->bio);
        form()[@"p_icon"] = @(c->icon_id);
        for (int i = 0; i < NAPPLE_ONLINE_MAX_LINKS; i++) {
            form()[[NSString stringWithFormat:@"p_lk%d", i]] = i < c->link_count ? S(c->links[i].kind) : @"web";
            form()[[NSString stringWithFormat:@"p_lv%d", i]] = i < c->link_count ? S(c->links[i].value) : @"";
        }
        g_photo_preview = nil;
    }
    NSString *prev = g_photo_preview;
    [p addArrangedSubview:hrow(@[
        avatar((prev ?: S(c->photo_path)).UTF8String, [form()[@"p_icon"] intValue], 64),
        btn(@"Choose photo…", 110, ^{
            NSOpenPanel *op = [NSOpenPanel openPanel];
            op.allowedContentTypes = @[ [UTType typeWithIdentifier:@"public.jpeg"], [UTType typeWithIdentifier:@"public.png"], [UTType typeWithIdentifier:@"org.webmproject.webp"] ];
            op.message = @"Pick a JPEG, PNG or WebP image (up to 5 MiB). It is cropped square and resized to 256x256.";
            if ([op runModal] == NSModalResponseOK) { g_photo_preview = op.URL.path; rebuild(); }
        }),
        prev ? btn(@"Upload photo", 100, ^{
            NSString *path = g_photo_preview;
            run(@"Uploading photo…", ^uint64_t(napple_online_done_fn d, void *u) { return API->upload_photo(CTX, pid.UTF8String, path.UTF8String, d, u); },
                ^(int rc, NSDictionary *j) {
                    if (rc == 0) { g_photo_preview = nil; set_status(@"Photo updated.", NO); } else set_status(reason_text(rc, j), YES);
                });
        }) : (NSView *)[NSView new],
    ])];
    [p addArrangedSubview:field(@"Nickname", @"p_nick", @"nickname", NO, 20, NO)];
    [p addArrangedSubview:field(@"Bio", @"p_bio", @"up to 160 characters", NO, 160, NO)];
    for (int i = 0; i < NAPPLE_ONLINE_MAX_LINKS; i++) {
        NSString *kk = [NSString stringWithFormat:@"p_lk%d", i], *vk = [NSString stringWithFormat:@"p_lv%d", i];
        NSPopUpButton *pop = fe_popup(kKinds(), (NSInteger)[kKinds() indexOfObject:fv(kk)], ^(NSPopUpButton *x) { form()[kk] = kKinds()[(NSUInteger)x.indexOfSelectedItem]; });
        pop.controlSize = NSControlSizeSmall;
        [pop.widthAnchor constraintEqualToConstant:84].active = YES;
        NSButton *vb = (NSButton *)field(@"", vk, @"link (optional)", NO, 120, NO).subviews.lastObject;
        [p addArrangedSubview:hrow(@[ pop, vb ])];
    }
    [p addArrangedSubview:fe_label(@"Icon", 11, YES)];
    NSStackView *grid = [NSStackView new];
    grid.orientation = NSUserInterfaceLayoutOrientationVertical;
    grid.spacing = 4;
    for (int r = 0; r < 6; r++) {
        NSMutableArray *row = [NSMutableArray new];
        for (int cc = 0; cc < 8; cc++) {
            int iid = r * 8 + cc;
            NSButton *b = [NSButton buttonWithImage:icon_image(iid, 30) target:nil action:nil];
            b.bordered = NO;
            b.imageScaling = NSImageScaleProportionallyUpOrDown;
            b.wantsLayer = YES;
            if ([form()[@"p_icon"] intValue] == iid) {
                b.layer.borderWidth = 2;
                b.layer.borderColor = NSColor.controlAccentColor.CGColor;
                b.layer.cornerRadius = 16;
            }
            [b.widthAnchor constraintEqualToConstant:34].active = YES;
            [b.heightAnchor constraintEqualToConstant:34].active = YES;
            fe_bind(b, ^(id x) { (void)x; form()[@"p_icon"] = @(iid); rebuild(); });
            [row addObject:b];
        }
        NSStackView *rv = hrow(row);
        rv.spacing = 2;
        [grid addArrangedSubview:rv];
    }
    [p addArrangedSubview:grid];
    status_line(p);
    [p addArrangedSubview:btn(@"Save profile", 110, ^{
        if (!valid_nick(fv(@"p_nick"))) return fail(@"Nickname: 3-20 characters, letters, digits, _ . -");
        if (fv(@"p_bio").length > 160) return fail(@"The bio is limited to 160 characters.");
        NSMutableArray *links = [NSMutableArray new];
        for (int i = 0; i < NAPPLE_ONLINE_MAX_LINKS; i++) {
            NSString *v = fv([NSString stringWithFormat:@"p_lv%d", i]), *k = fv([NSString stringWithFormat:@"p_lk%d", i]);
            if (!v.length) continue;
            if ([k isEqual:@"web"] && ![v hasPrefix:@"https://"]) return fail(@"Web links must start with https://");
            [links addObject:@{ @"kind" : k, @"value" : v }];
        }
        NSData *js = [NSJSONSerialization dataWithJSONObject:@{ @"nickname" : fv(@"p_nick"), @"bio" : fv(@"p_bio"), @"links" : links, @"icon_id" : form()[@"p_icon"] ?: @0 } options:0 error:nil];
        NSString *json = [[NSString alloc] initWithData:js encoding:NSUTF8StringEncoding];
        run(@"Saving…", ^uint64_t(napple_online_done_fn d, void *u) { return API->update_profile(CTX, pid.UTF8String, json.UTF8String, d, u); },
            ^(int rc, NSDictionary *j) { set_status(rc == 0 ? @"Profile saved." : reason_text(rc, j), rc != 0); });
    })];
}
static void screen_tier(NSStackView *p, const napple_online_account *a) {
    [p addArrangedSubview:back_btn(@"home")];
    [p addArrangedSubview:head(@"Visibility")];
    [p addArrangedSubview:wrap(@"Who can find you and see you online. Applies to this profile and takes effect at once. Reports and bans always work, whatever you choose.", 10, nil)];
    NSString *pid = S(a->current.id);
    for (int t = 1; t <= 5; t++) {
        BOOL sel = a->current.tier == t;
        NSButton *b = btn([NSString stringWithFormat:@"%@%@", sel ? @"✓  " : @"     ", tier_name(t)], 0, ^{
            if (sel) return;
            run(@"Updating…", ^uint64_t(napple_online_done_fn d, void *u) { return API->set_tier(CTX, pid.UTF8String, t, d, u); },
                ^(int rc, NSDictionary *j) { set_status(rc == 0 ? [NSString stringWithFormat:@"Now %@.", tier_name(t)] : reason_text(rc, j), rc != 0); });
        });
        b.alignment = NSTextAlignmentLeft;
        [b.widthAnchor constraintEqualToConstant:340].active = YES;
        [p addArrangedSubview:b];
        [p addArrangedSubview:wrap(tier_desc(t), 10, nil)];
    }
    status_line(p);
}
static void screen_profiles(NSStackView *p, const napple_online_account *a) {
    [p addArrangedSubview:back_btn(@"home")];
    [p addArrangedSubview:head(@"Profiles")];
    [p addArrangedSubview:wrap(@"Each profile is a separate identity in games (up to 8). Nintendo Switch users map to a profile automatically.", 10, nil)];
    napple_online_profile ps[NAPPLE_ONLINE_MAX_PROFILES];
    size_t n = MIN(API->get_profiles(CTX, ps, NAPPLE_ONLINE_MAX_PROFILES), (size_t)NAPPLE_ONLINE_MAX_PROFILES);
    for (size_t i = 0; i < n; i++) {
        napple_online_profile pr = ps[i];
        BOOL cur = !strcmp(pr.id, a->current.id);
        NSTextField *nm = fe_label(S(pr.nickname), 12, cur);
        NSTextField *sb = fe_label(tier_name(pr.tier), 10, NO);
        sb.textColor = NSColor.secondaryLabelColor;
        NSStackView *col = [NSStackView stackViewWithViews:@[ nm, sb ]];
        col.orientation = NSUserInterfaceLayoutOrientationVertical;
        col.alignment = NSLayoutAttributeLeading;
        col.spacing = 0;
        [col.widthAnchor constraintGreaterThanOrEqualToConstant:150].active = YES;
        NSView *act = cur ? (NSView *)fe_label(@"In use", 10, NO) : btn(@"Use", 50, ^{
            run(@"Switching…", ^uint64_t(napple_online_done_fn d, void *u) { return API->switch_profile(CTX, pr.id, d, u); },
                ^(int rc, NSDictionary *j) { set_status(rc == 0 ? nil : reason_text(rc, j), rc != 0); });
        });
        [p addArrangedSubview:hrow(@[ avatar(pr.photo_path, pr.icon_id, 32), col, act ])];
    }
    if (n < NAPPLE_ONLINE_MAX_PROFILES) {
        [p addArrangedSubview:head(@"New profile")];
        [p addArrangedSubview:field(@"Nickname", @"new_nick", @"3-20 characters", NO, 20, NO)];
        [p addArrangedSubview:btn(@"Create profile", 110, ^{
            if (!valid_nick(fv(@"new_nick"))) return fail(@"Nickname: 3-20 characters, letters, digits, _ . -");
            run(@"Creating…", ^uint64_t(napple_online_done_fn d, void *u) { return API->create_profile(CTX, fv(@"new_nick").UTF8String, d, u); },
                ^(int rc, NSDictionary *j) { if (rc == 0) { form()[@"new_nick"] = @""; set_status(@"Profile created.", NO); } else set_status(reason_text(rc, j), YES); });
        })];
    } else [p addArrangedSubview:wrap(@"You have reached the limit of 8 profiles.", 10, nil)];
    status_line(p);
}
static NSString *ago(int64_t t) {
    if (!t) return @"never";
    NSDateFormatter *f = [NSDateFormatter new];
    f.dateStyle = NSDateFormatterMediumStyle;
    f.timeStyle = NSDateFormatterNoStyle;
    return [f stringFromDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)t]];
}
static void screen_devices(NSStackView *p, const napple_online_account *a) {
    (void)a;
    [p addArrangedSubview:back_btn(@"home")];
    [p addArrangedSubview:head(@"Devices")];
    [p addArrangedSubview:wrap(@"Devices that can read your cloud saves. A new device must be approved from one you already trust.", 10, nil)];
    napple_online_device ds[16];
    size_t n = MIN(API->get_devices(CTX, ds, 16), (size_t)16);
    for (size_t i = 0; i < n; i++) {
        napple_online_device d = ds[i];
        NSTextField *nm = fe_label(S(d.name), 12, d.current);
        NSTextField *sb = fe_label(d.current ? @"This device" : d.pending ? @"Waiting for approval" : [@"Last seen " stringByAppendingString:ago(d.last_seen_unix)], 10, NO);
        sb.textColor = d.pending ? NSColor.systemOrangeColor : NSColor.secondaryLabelColor;
        NSStackView *col = [NSStackView stackViewWithViews:@[ nm, sb ]];
        col.orientation = NSUserInterfaceLayoutOrientationVertical;
        col.alignment = NSLayoutAttributeLeading;
        col.spacing = 0;
        [col.widthAnchor constraintGreaterThanOrEqualToConstant:140].active = YES;
        NSMutableArray *r = [NSMutableArray arrayWithObject:col];
        if (d.pending)
            [r addObject:btn(@"Approve", 64, ^{
                run(@"Approving…", ^uint64_t(napple_online_done_fn dn, void *u) { return API->approve_device(CTX, d.id, dn, u); },
                    ^(int rc, NSDictionary *j) { set_status(rc == 0 ? @"Device approved." : reason_text(rc, j), rc != 0); });
            })];
        if (!d.current)
            [r addObject:btn(d.pending ? @"Deny" : @"Revoke", 56, ^{
                run(@"Removing…", ^uint64_t(napple_online_done_fn dn, void *u) { return API->revoke_device(CTX, d.id, dn, u); },
                    ^(int rc, NSDictionary *j) { set_status(rc == 0 ? @"Device removed." : reason_text(rc, j), rc != 0); });
            })];
        [p addArrangedSubview:hrow(r)];
    }
    status_line(p);
}
static void screen_saves(NSStackView *p, const napple_online_account *a) {
    (void)a;
    [p addArrangedSubview:back_btn(@"home")];
    [p addArrangedSubview:head(@"Cloud saves")];
    napple_online_save_status ss[64];
    size_t n = MIN(API->get_save_status(CTX, ss, 64), (size_t)64);
    if (!n) [p addArrangedSubview:wrap(@"No game has cloud saves yet. Saves sync once you play a game while signed in.", 11, nil)];
    NSArray *st = @[ @"Off", @"In sync", @"Uploading…", @"Downloading…", @"Conflict: open the game to choose", @"Error" ];
    for (size_t i = 0; i < n; i++) {
        napple_online_save_status s = ss[i];
        NSTextField *nm = fe_label(S(s.title_name[0] ? s.title_name : s.title_id), 12, NO);
        NSString *t = [NSString stringWithFormat:@"%@  ·  last sync %@  ·  %.1f MB", st[(NSUInteger)MIN(MAX(s.state, 0), 5)], ago(s.last_sync_unix), s.bytes / 1048576.0];
        NSTextField *sb = fe_label(t, 10, NO);
        sb.textColor = s.state == 4 || s.state == 5 ? NSColor.systemOrangeColor : NSColor.secondaryLabelColor;
        NSStackView *col = [NSStackView stackViewWithViews:@[ nm, sb ]];
        col.orientation = NSUserInterfaceLayoutOrientationVertical;
        col.alignment = NSLayoutAttributeLeading;
        col.spacing = 0;
        [p addArrangedSubview:col];
    }
}
static void screen_delete(NSStackView *p, const napple_online_account *a) {
    (void)a;
    [p addArrangedSubview:back_btn(@"home")];
    [p addArrangedSubview:head(@"Delete account")];
    [p addArrangedSubview:wrap(@"This permanently deletes your account, all profiles, friends and cloud saves. It cannot be undone.", 11, NSColor.systemRedColor)];
    [p addArrangedSubview:field(@"Password", @"delpw", @"your password", YES, 100, NO)];
    status_line(p);
    [p addArrangedSubview:btn(@"Continue…", 90, ^{
        if (!fv(@"delpw").length) return fail(@"Enter your password to continue.");
        go(@"delete_confirm");
    })];
}
static void screen_delete_confirm(NSStackView *p) {
    [p addArrangedSubview:head(@"Really delete your account?")];
    [p addArrangedSubview:wrap(@"Everything will be erased from our servers. Saves on this Mac stay.", 11, nil)];
    status_line(p);
    [p addArrangedSubview:hrow(@[
        btn(@"Cancel", 80, ^{ go(@"delete"); }),
        btn(@"Delete forever", 110, ^{
            run(@"Deleting…", ^uint64_t(napple_online_done_fn d, void *u) { return API->delete_account(CTX, fv(@"delpw").UTF8String, d, u); },
                ^(int rc, NSDictionary *j) {
                    if (rc == 0) { form()[@"delpw"] = @""; g_screen = @"signin"; set_status(@"Your account was deleted.", NO); }
                    else { g_screen = @"delete"; set_status(reason_text(rc, j), YES); }
                });
        }),
    ])];
}

/* ---- the page ---- */
@interface FEOnlineFlipped : NSView
@end
@implementation FEOnlineFlipped
- (BOOL)isFlipped { return YES; }
@end
static NSView *scrolled_online(NSStackView *p) {
    p.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 392, 500)];
    sv.hasVerticalScroller = YES;
    sv.drawsBackground = NO;
    sv.autohidesScrollers = YES;
    FEOnlineFlipped *doc = [[FEOnlineFlipped alloc] initWithFrame:NSMakeRect(0, 0, 392, 500)]; /* flipped: short pages start at the top */
    doc.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:p];
    [NSLayoutConstraint activateConstraints:@[
        [p.topAnchor constraintEqualToAnchor:doc.topAnchor], [p.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor],
        [p.trailingAnchor constraintEqualToAnchor:doc.trailingAnchor], [p.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor]
    ]];
    sv.documentView = doc;
    NSClipView *clip = sv.contentView;
    [NSLayoutConstraint activateConstraints:@[
        [doc.topAnchor constraintEqualToAnchor:clip.topAnchor], [doc.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
        [doc.widthAnchor constraintEqualToAnchor:clip.widthAnchor]
    ]];
    return sv;
}
static void rebuild(void) {
    if (!g_host) return;
    for (NSView *v in [g_host.subviews copy]) [v removeFromSuperview];
    napple_online_account acc;
    API->get_account(CTX, &acc);
    BOOL signed_in = acc.signed_in != 0;
    /* keep the screen consistent with the sign-in state */
    NSSet *out_screens = [NSSet setWithObjects:@"signin", @"signup", @"verify", @"reset_start", @"reset_finish", nil];
    BOOL is_out = [out_screens containsObject:g_screen];
    if (signed_in && is_out) g_screen = @"home";
    if (!signed_in && !is_out) g_screen = @"signin";

    NSView *content = nil;
    CGFloat top = 0;
    if (signed_in && g_subtabs.count) {
        NSMutableArray *labels = [NSMutableArray arrayWithObject:@"Account"];
        [labels addObjectsFromArray:g_subtab_titles];
        NSSegmentedControl *seg = [NSSegmentedControl segmentedControlWithLabels:labels trackingMode:NSSegmentSwitchTrackingSelectOne target:nil action:nil];
        seg.controlSize = NSControlSizeSmall;
        g_subtab = MIN(g_subtab, (NSInteger)g_subtabs.count);
        seg.selectedSegment = g_subtab;
        seg.frame = NSMakeRect(8, g_host.bounds.size.height - 26, g_host.bounds.size.width - 16, 22);
        seg.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
        fe_bind(seg, ^(NSSegmentedControl *x) { g_subtab = x.selectedSegment; rebuild(); });
        [g_host addSubview:seg];
        top = 28;
        if (g_subtab > 0) content = fe_online_subtab_make((NSUInteger)g_subtab - 1);
    }
    if (!content) {
        NSStackView *p = new_page();
        if ([g_screen isEqual:@"signin"]) screen_signin(p);
        else if ([g_screen isEqual:@"signup"]) screen_signup(p);
        else if ([g_screen isEqual:@"verify"]) screen_verify(p);
        else if ([g_screen isEqual:@"reset_start"]) screen_reset_start(p);
        else if ([g_screen isEqual:@"reset_finish"]) screen_reset_finish(p);
        else if ([g_screen isEqual:@"profile"]) screen_profile(p, &acc);
        else if ([g_screen isEqual:@"tier"]) screen_tier(p, &acc);
        else if ([g_screen isEqual:@"profiles"]) screen_profiles(p, &acc);
        else if ([g_screen isEqual:@"devices"]) screen_devices(p, &acc);
        else if ([g_screen isEqual:@"saves"]) screen_saves(p, &acc);
        else if ([g_screen isEqual:@"delete"]) screen_delete(p, &acc);
        else if ([g_screen isEqual:@"delete_confirm"]) screen_delete_confirm(p);
        else screen_home(p, &acc);
        content = scrolled_online(p);
    }
    content.frame = NSMakeRect(0, 0, g_host.bounds.size.width, g_host.bounds.size.height - top);
    content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [g_host addSubview:content];
    fe_nav_page_changed();
}
NSView *fe_online_page(NappleFE *fe) {
    g_fe = fe;
    g_host = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 392, 500)];
    API->set_change_callback(CTX, on_change, NULL);
    rebuild();
    return g_host;
}
void fe_online_debug_screen(NSString *name, NSString *status) {
    g_screen = name;
    g_busy = NO;
    set_status(nil, NO);
    if ([status hasPrefix:@"error:"]) set_status([status substringFromIndex:6], YES);
    else if ([status hasPrefix:@"busy:"]) { set_status([status substringFromIndex:5], NO); g_busy = YES; }
    else if (status.length) set_status(status, NO);
    rebuild();
}
