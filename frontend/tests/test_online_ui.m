/* The overlay Online tab against the stub API (no servers, no frontend window): drives the screens by pressing their buttons, checks the state the
   API ends in, and renders every screen to a PNG (NAPPLE_ONLINE_SHOT_DIR, default $TMPDIR/napple-online-ui). */
#import <Cocoa/Cocoa.h>
#include <stdlib.h>
#include <string.h>
#include "overlay_online.h"

static int g_checked, g_failed;
#define CHECK(c, ...)                                                                                                                                          \
    do {                                                                                                                                                       \
        g_checked++;                                                                                                                                           \
        if (!(c)) {                                                                                                                                            \
            g_failed++;                                                                                                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                                                               \
            fprintf(stderr, __VA_ARGS__);                                                                                                                      \
            fprintf(stderr, "\n");                                                                                                                             \
        }                                                                                                                                                      \
    } while (0)

static NSView *g_host;
static NSString *g_dir;
static void spin(double s) { [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:s]]; }
static void find_buttons(NSView *v, NSMutableArray *out) {
    if ([v isKindOfClass:NSButton.class]) [out addObject:v];
    for (NSView *s in v.subviews) find_buttons(s, out);
}
static NSButton *button(NSString *prefix) {
    NSMutableArray *a = [NSMutableArray new];
    find_buttons(g_host, a);
    for (NSButton *b in a)
        if ([b.title hasPrefix:prefix]) return b;
    return nil;
}
static BOOL press(NSString *prefix) {
    NSButton *b = button(prefix);
    if (!b) {
        fprintf(stderr, "no button '%s'\n", prefix.UTF8String);
        return NO;
    }
    [b performClick:nil];
    spin(0.05);
    return YES;
}
static void text_in(NSView *v, NSMutableString *out) {
    if ([v isKindOfClass:NSTextField.class]) [out appendFormat:@"%@\n", ((NSTextField *)v).stringValue];
    if ([v isKindOfClass:NSButton.class]) [out appendFormat:@"[%@]\n", ((NSButton *)v).title];
    for (NSView *s in v.subviews) text_in(s, out);
}
static NSString *texts(void) {
    NSMutableString *m = [NSMutableString new];
    text_in(g_host, m);
    return m;
}
static void shot(NSString *name) {
    NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 392, 520)];
    box.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    box.wantsLayer = YES;
    box.layer.backgroundColor = [NSColor colorWithWhite:0.14 alpha:1].CGColor;
    g_host.frame = NSMakeRect(0, 10, 392, 500);
    [box addSubview:g_host];
    [box layoutSubtreeIfNeeded];
    spin(0.05);
    NSBitmapImageRep *rep = [box bitmapImageRepForCachingDisplayInRect:box.bounds];
    [box cacheDisplayInRect:box.bounds toBitmapImageRep:rep];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    [png writeToFile:[g_dir stringByAppendingFormat:@"/%@.png", name] atomically:YES];
    CHECK(png.length > 2000, "screenshot %s is %lu bytes", name.UTF8String, (unsigned long)png.length);
}
static napple_online_change_fn g_installed_cb;
static void spy_set_cb(void *ctx, napple_online_change_fn fn, void *user) {
    (void)ctx;
    (void)user;
    g_installed_cb = fn;
}
static napple_online_account account(void) {
    napple_online_account a;
    fe_online_api()->get_account(fe_online_api()->ctx, &a);
    return a;
}

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        setenv("NAPPLE_ONLINE_STUB_INSTANT", "1", 1);
        const char *d = getenv("NAPPLE_ONLINE_SHOT_DIR");
        g_dir = d ? @(d) : [NSTemporaryDirectory() stringByAppendingPathComponent:@"napple-online-ui"];
        [NSFileManager.defaultManager createDirectoryAtPath:g_dir withIntermediateDirectories:YES attributes:nil error:nil];
        CHECK(fe_online_missing_icon_symbols() == 0, "%d icon symbols missing on this macOS", fe_online_missing_icon_symbols());
        fe_online_stub_reset(0);
        g_host = fe_online_page(nil);
        shot(@"01-signin");
        CHECK(!account().signed_in, "starts signed out");

        /* wrong password -> error, stays signed out */
        fe_online_debug_set(@"email", @"player@example.com");
        fe_online_debug_set(@"password", @"wrongpassword");
        press(@"Sign in");
        spin(0.1);
        CHECK(!account().signed_in && [texts() containsString:@"Email or password is wrong"], "wrong password shows an error");
        shot(@"02-signin-error");

        /* local validation: nothing is sent for a short password on sign-up */
        press(@"Create account");
        fe_online_debug_set(@"nickname", @"Eros");
        fe_online_debug_set(@"password", @"short");
        shot(@"03-signup");
        press(@"Create account");
        CHECK([texts() containsString:@"at least 10 characters"], "short password rejected locally");
        fe_online_debug_set(@"password", @"correct-horse-battery");
        NSMutableArray *bs = [NSMutableArray new];
        find_buttons(g_host, bs);
        for (NSButton *b in bs)
            if ([b.title hasPrefix:@"I am 13"] || [b.title hasPrefix:@"I accept"]) [b performClick:nil];
        press(@"Create account");
        spin(0.1);
        CHECK([texts() containsString:@"6-digit"], "verify screen after signup: %s", texts().UTF8String);
        shot(@"04-verify");

        /* wrong code, then the right one */
        fe_online_debug_set(@"code", @"000000");
        press(@"Verify");
        spin(0.1);
        CHECK(!account().signed_in && [texts() containsString:@"wrong or expired"], "bad code rejected");
        fe_online_debug_set(@"code", @"123456");
        press(@"Verify");
        spin(0.1);
        CHECK(account().signed_in, "verified code signs in");
        CHECK([texts() containsString:@"Eros"] && [texts() containsString:@"Public"], "header shows nickname and tier");
        shot(@"05-home");

        /* tier picker */
        press(@"Visibility");
        shot(@"06-tier");
        press(@"     Ghost");
        spin(0.1);
        CHECK(account().current.tier == NAPPLE_TIER_GHOST, "tier set to ghost (%d)", account().current.tier);
        press(@"‹ Back");

        /* profile editor */
        press(@"Edit profile");
        fe_online_debug_set(@"p_bio", @"Hello there");
        fe_online_debug_set(@"p_nick", @"Eros_2");
        shot(@"07-profile");
        press(@"Save profile");
        spin(0.1);
        CHECK(!strcmp(account().current.nickname, "Eros_2") && !strcmp(account().current.bio, "Hello there"), "profile saved (%s)", account().current.nickname);
        fe_online_debug_set(@"p_nick", @"no");
        press(@"Save profile");
        CHECK([texts() containsString:@"3-20"], "short nickname rejected locally");
        fe_online_debug_set(@"p_nick", @"taken");
        press(@"Save profile");
        spin(0.1);
        CHECK([texts() containsString:@"taken"], "server nickname_taken shown");
        press(@"‹ Back");

        /* profiles */
        press(@"Profiles");
        shot(@"08-profiles");
        fe_online_debug_set(@"new_nick", @"Third.one");
        press(@"Create profile");
        spin(0.1);
        CHECK(account().profile_count == 3, "profile created (%d)", account().profile_count);
        press(@"Use");
        spin(0.1);
        CHECK(strcmp(account().current.id, "prof-1"), "switched profile");
        press(@"‹ Back");

        /* devices */
        press(@"Devices");
        shot(@"09-devices");
        press(@"Approve");
        spin(0.1);
        napple_online_device ds[8];
        size_t n = fe_online_api()->get_devices(fe_online_api()->ctx, ds, 8);
        int pend = 0;
        for (size_t i = 0; i < n; i++) pend += ds[i].pending;
        CHECK(pend == 0, "device approved");
        press(@"Revoke");
        spin(0.1);
        CHECK(fe_online_api()->get_devices(fe_online_api()->ctx, ds, 8) == 2, "device revoked");
        press(@"‹ Back");

        /* cloud saves */
        press(@"Cloud saves");
        shot(@"10-saves");
        CHECK([texts() containsString:@"Mario Kart"] && [texts() containsString:@"Conflict"], "save status listed");
        press(@"‹ Back");

        /* busy and delete flow */
        fe_online_debug_screen(@"home", @"busy:Working…");
        shot(@"11-busy");
        fe_online_debug_screen(@"delete", nil);
        shot(@"12-delete");
        fe_online_debug_set(@"delpw", @"wrongpassword");
        press(@"Continue");
        shot(@"13-delete-confirm");
        press(@"Delete forever");
        spin(0.1);
        CHECK(account().signed_in && [texts() containsString:@"wrong"], "wrong password does not delete");
        fe_online_debug_set(@"delpw", @"correct-horse-battery");
        press(@"Continue");
        press(@"Delete forever");
        spin(0.1);
        CHECK(!account().signed_in && [texts() containsString:@"deleted"], "account deleted, back to sign in");

        /* password reset */
        press(@"Forgot");
        fe_online_debug_set(@"email", @"player@example.com");
        press(@"Send code");
        spin(0.1);
        fe_online_debug_set(@"code", @"123456");
        fe_online_debug_set(@"newpw", @"another-long-password");
        shot(@"14-reset");
        press(@"Change password");
        spin(0.1);
        CHECK([texts() containsString:@"Password changed"], "reset finished");

        /* unverified login goes to the code screen */
        fe_online_debug_set(@"email", @"unverified@napple.lat");
        fe_online_debug_set(@"password", @"whatever-pass");
        press(@"Sign in");
        spin(0.1);
        CHECK([texts() containsString:@"Verify"], "unverified login opens the code screen");

        /* the shell registers overlay-social's pages itself (napple_online_social_init): Account + six social sub-tabs, only while signed in */
        CHECK(fe_online_subtab_count() == 6, "shell registered the six social sub-tabs, got %d", (int)fe_online_subtab_count());
        fe_online_stub_reset(0);
        g_host = fe_online_page(nil);
        int out_segs = 0;
        for (NSView *v in g_host.subviews)
            if ([v isKindOfClass:NSSegmentedControl.class]) out_segs++;
        CHECK(out_segs == 0, "no sub-tab strip while signed out");
        fe_online_stub_reset(1);
        g_host = fe_online_page(nil);
        NSMutableArray *segs = [NSMutableArray new];
        for (NSView *v in g_host.subviews)
            if ([v isKindOfClass:NSSegmentedControl.class]) [segs addObject:v];
        NSSegmentedControl *sg = segs.count == 1 ? segs[0] : nil;
        CHECK(sg && sg.segmentCount == 7, "sub-tab strip shown with Account + six social pages");
        NSArray *want = @[ @"Account", @"Friends", @"Requests", @"Add", @"Recent", @"Blocked", @"Stats" ];
        for (NSInteger i = 0; sg && i < 7; i++) CHECK([[sg labelForSegment:i] isEqualToString:want[i]], "segment %d is %s", (int)i, [want[i] UTF8String]);
        shot(@"15-subtabs");
        for (NSInteger i = 1; sg && i < 7; i++) {
            sg.selectedSegment = i;
            [sg sendAction:sg.action to:sg.target];
            spin(0.05);
            NSMutableArray *again = [NSMutableArray new];
            for (NSView *v in g_host.subviews)
                if ([v isKindOfClass:NSSegmentedControl.class]) [again addObject:v];
            CHECK(again.count == 1 && g_host.subviews.count == 2, "sub-tab %d page built next to the strip", (int)i);
            if (again.count == 1) sg = again[0];
        }
        shot(@"16-social-stats");

        /* installing a runtime table keeps the shell's change callback on it for good (social toasts while the overlay is closed) */
        napple_online_ui_api spy = *napple_online_ui_stub();
        spy.set_change_callback = spy_set_cb;
        napple_frontend_set_online_ui(NULL, &spy);
        CHECK(g_installed_cb != NULL, "set_online_ui installs the shell change callback");
        g_installed_cb(NULL); /* rebuilds the page (and feeds the social toasts) without crashing */
        CHECK(fe_online_api()->ctx == spy.ctx, "installed table is the active one");
        napple_frontend_set_online_ui(NULL, NULL);

        printf("online ui: %d checks, %d failed; screenshots in %s\n", g_checked, g_failed, g_dir.UTF8String);
    }
    return g_failed ? 1 : 0;
}
