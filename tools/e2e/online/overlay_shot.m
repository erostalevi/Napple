/* Overlay Online tab driven by the REAL client core (runtime/src/online/ over crates/napple-cloud) against live or local servers: signs in through the
 * screens' own buttons and renders the pages to PNGs. Env: NAPPLE_HOME (the "device"), NAPPLE_ACCOUNT_URL / NAPPLE_SOCIAL_URL, OVERLAY_EMAIL,
 * OVERLAY_PASSWORD, OVERLAY_SHOT_DIR. Built by tools/e2e/online/build_overlay_shot.sh. */
#import <Cocoa/Cocoa.h>
#include "overlay_online.h"
#include "online_ui.h"

static NSView *g_host;
static NSString *g_dir;
static void spin(double s) { [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:s]]; }
static void pump_for(double s) {
    for (double t = 0; t < s; t += 0.05) {
        napple_online_ui_pump();
        spin(0.05);
    }
}
static void find_buttons(NSView *v, NSMutableArray *out) {
    if ([v isKindOfClass:NSButton.class]) [out addObject:v];
    for (NSView *s in v.subviews) find_buttons(s, out);
}
static BOOL press(NSString *prefix) {
    NSMutableArray *a = [NSMutableArray new];
    find_buttons(g_host, a);
    for (NSButton *b in a)
        if ([b.title hasPrefix:prefix]) { [b performClick:nil]; pump_for(0.2); return YES; }
    fprintf(stderr, "no button '%s'\n", prefix.UTF8String);
    return NO;
}
static void text_in(NSView *v, NSMutableString *out) {
    if ([v isKindOfClass:NSTextField.class]) [out appendFormat:@"%@\n", ((NSTextField *)v).stringValue];
    for (NSView *s in v.subviews) text_in(s, out);
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
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[g_dir stringByAppendingFormat:@"/%@.png", name] atomically:YES];
    printf("shot %s\n", name.UTF8String);
}

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        const char *d = getenv("OVERLAY_SHOT_DIR");
        g_dir = [NSString stringWithUTF8String:d ? d : "/tmp/napple-overlay-shots"];
        [NSFileManager.defaultManager createDirectoryAtPath:g_dir withIntermediateDirectories:YES attributes:nil error:nil];
        const napple_online_ui_api *api = napple_online_runtime_start(NULL, NULL, NULL, "4e6170706c6555736572000000000001");
        if (!api) { fprintf(stderr, "client did not start\n"); return 1; }
        napple_frontend_set_online_ui(NULL, api);
        pump_for(2.0); /* the worker's first state refresh */
        napple_online_account a;
        api->get_account(api->ctx, &a);
        g_host = fe_online_page(nil);
        if (!a.signed_in) {
            shot(@"01-signin");
            fe_online_debug_set(@"email", [NSString stringWithUTF8String:getenv("OVERLAY_EMAIL") ?: ""]);
            fe_online_debug_set(@"password", [NSString stringWithUTF8String:getenv("OVERLAY_PASSWORD") ?: ""]);
            press(@"Sign in");
            pump_for(4.0);
            g_host = fe_online_page(nil);
        }
        api->get_account(api->ctx, &a);
        printf("signed_in=%d email=%s nickname=%s tier=%d friends=%zu\n", a.signed_in, a.email, a.current.nickname, a.current.tier, api->get_friends(api->ctx, NULL, 0));
        shot(@"02-home");
        fe_online_debug_screen(@"profile", nil); pump_for(0.3); shot(@"03-profile");
        fe_online_debug_screen(@"tier", nil); pump_for(0.3); shot(@"04-tier");
        fe_online_debug_screen(@"devices", nil); pump_for(0.3); shot(@"05-devices");
        fe_online_debug_screen(@"saves", nil); pump_for(0.3); shot(@"06-saves");
        fe_online_debug_screen(@"home", nil);
        g_host = fe_online_page(nil);
        for (NSView *v in g_host.subviews)
            if ([v isKindOfClass:NSSegmentedControl.class]) {
                NSSegmentedControl *sg = (NSSegmentedControl *)v;
                for (NSInteger i = 1; i < sg.segmentCount; i++) {
                    sg.selectedSegment = i;
                    [sg sendAction:sg.action to:sg.target];
                    pump_for(0.6);
                    shot([NSString stringWithFormat:@"07-sub%ld-%@", (long)i, [sg labelForSegment:i]]);
                    for (NSView *w in g_host.subviews) if ([w isKindOfClass:NSSegmentedControl.class]) sg = (NSSegmentedControl *)w;
                }
                break;
            }
        napple_online_runtime_stop();
    }
    return 0;
}
