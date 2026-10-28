/* Library applet sheets (macOS): the AppKit side of the controller, player select, error, web and album applets. Every function blocks the calling
 * (guest) thread until the user answers and runs its UI on the main thread in a modal loop, like frontend_alert. Test aids: NAPPLE_SHEET_AUTO =
 * "<action>[:<ms>]" presses a button by itself (confirm / cancel / next / leave, default confirm after 400 ms) and NAPPLE_SHEET_SHOT = <dir> writes
 * <sheet>.png of the sheet before that press (frontend/tests/test_sheets.m, docs screenshots). */
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <objc/runtime.h>
#include <string.h>
#include "napple_frontend.h"
#include "fe_internal.h"

static int on_main(int (^blk)(void)) {
    if ([NSThread isMainThread]) return blk();
    __block int r = 0;
    dispatch_sync(dispatch_get_main_queue(), ^{ r = blk(); });
    return r;
}
/* GCD's main queue is not serviced by a modal loop; a timer in the common modes is. */
static void after_ms(double ms, void (^blk)(void)) {
    NSTimer *t = [NSTimer timerWithTimeInterval:ms / 1000 repeats:NO block:^(NSTimer *x) {
        (void)x;
        blk();
    }];
    [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
}
static BOOL auto_action(NSString **action, double *ms) {
    const char *e = getenv("NAPPLE_SHEET_AUTO");
    if (!e || !*e) return NO;
    NSArray *p = [@(e) componentsSeparatedByString:@":"];
    *action = p[0];
    *ms = p.count > 1 ? [p[1] doubleValue] : 400;
    return YES;
}
/* Screenshot aid: the window's screen rectangle is captured with screencapture (cacheDisplay renders neither text nor web content here). */
static void shot_window(NSWindow *w, NSString *name) {
    const char *dir = getenv("NAPPLE_SHEET_SHOT");
    if (!dir || !*dir || !w) return;
    [[NSFileManager defaultManager] createDirectoryAtPath:@(dir) withIntermediateDirectories:YES attributes:nil error:nil];
    NSRect r = w.frame;
    CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame) - NSMaxY(r);
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/screencapture"];
    t.arguments = @[ @"-x", @"-R", [NSString stringWithFormat:@"%d,%d,%d,%d", (int)r.origin.x, (int)top, (int)r.size.width, (int)r.size.height],
                     [NSString stringWithFormat:@"%s/%@.png", dir, name] ];
    [t launchAndReturnError:nil];
    [t waitUntilExit];
}
/* Runs `alert` modally: the shot and the scripted press happen once the loop runs. Returns the 1-based index of the pressed button. */
static int run_alert(NSAlert *a, NSString *name, NSArray<NSString *> *actions) {
    [NSApp activateIgnoringOtherApps:YES];
    NSString *act;
    double ms;
    if (auto_action(&act, &ms)) {
        after_ms(ms, ^{
            shot_window(a.window, name);
            NSUInteger i = [actions indexOfObject:act];
            if (i == NSNotFound) i = 0;
            if (getenv("NAPPLE_SHEET_DEBUG")) fprintf(stderr, "press %s idx %lu\n", act.UTF8String, (unsigned long)i);
            [NSApp stopModalWithCode:NSAlertFirstButtonReturn + (NSInteger)i];
        });
    }
    CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
    NSModalResponse mr = a.runModal;
    if (getenv("NAPPLE_SHEET_DEBUG")) fprintf(stderr, "runModal took %.0f ms\n", (CFAbsoluteTimeGetCurrent() - t0) * 1000);
    if (getenv("NAPPLE_SHEET_DEBUG")) fprintf(stderr, "sheet %s: act %s response %ld\n", name.UTF8String, act.UTF8String, (long)mr);
    return (int)(mr - NSAlertFirstButtonReturn) + 1;
}
static NSString *uid_text(const uint8_t u[16]) {
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < 16; i++) [s appendFormat:@"%02x", u[i]];
    return s;
}

int napple_frontend_sheet_controller(napple_frontend *f, unsigned min, unsigned max, int connected, int single_mode, int *players) {
    (void)f;
    return on_main(^{
        NSAlert *a = [NSAlert new];
        a.messageText = @"Controller support";
        NSString *want = min == max ? [NSString stringWithFormat:@"%u player%s", max, max == 1 ? "" : "s"] : [NSString stringWithFormat:@"%u to %u players", min, max];
        NSString *have = [NSString stringWithFormat:@"%d controller%s connected (a keyboard counts as one)", connected, connected == 1 ? "" : "s"];
        NSString *warn = (unsigned)connected < min ? @"\nFewer controllers than the game asked for: connect more, or continue with the ones you have." : @"";
        a.informativeText = [NSString stringWithFormat:@"The game asks for %@%@.\n%@%@", want, single_mode ? @", one controller" : @"", have, warn];
        NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 220, 26) pullsDown:NO];
        unsigned lo = min < 1 ? 1 : min, hi = max < lo ? lo : max;
        for (unsigned n = lo; n <= hi; n++) [pop addItemWithTitle:[NSString stringWithFormat:@"%u player%s", n, n == 1 ? "" : "s"]];
        int want_n = *players < (int)lo ? (int)lo : *players > (int)hi ? (int)hi : *players;
        [pop selectItemAtIndex:want_n - (int)lo];
        a.accessoryView = pop;
        [a addButtonWithTitle:@"Confirm"];
        [a addButtonWithTitle:@"Cancel"];
        int r = run_alert(a, @"controller", @[ @"confirm", @"cancel" ]);
        if (r != 1) return 0;
        *players = (int)lo + (int)pop.indexOfSelectedItem;
        return 1;
    });
}

int napple_frontend_sheet_player_select(napple_frontend *f, const uint8_t (*uids)[16], unsigned count) {
    (void)f;
    return on_main(^{
        NSAlert *a = [NSAlert new];
        a.messageText = @"Select a user";
        a.informativeText = @"The game asks which profile should play.";
        NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 300, 26) pullsDown:NO];
        for (unsigned i = 0; i < count; i++)
            [pop addItemWithTitle:[NSString stringWithFormat:@"User %u (%@)", i + 1, [uid_text(uids[i]) substringToIndex:8]]];
        a.accessoryView = pop;
        [a addButtonWithTitle:@"Select"];
        [a addButtonWithTitle:@"Cancel"];
        int r = run_alert(a, @"player_select", @[ @"confirm", @"cancel" ]);
        return r == 1 ? (int)pop.indexOfSelectedItem : -1;
    });
}

void napple_frontend_sheet_error(napple_frontend *f, const char *title, const char *message) {
    (void)f;
    NSString *t = @(title ? title : ""), *m = @(message ? message : "");
    on_main(^{
        NSAlert *a = [NSAlert new];
        a.alertStyle = NSAlertStyleCritical;
        a.messageText = t;
        a.informativeText = m;
        [a addButtonWithTitle:@"OK"];
        return run_alert(a, @"error", @[ @"confirm" ]);
    });
}

/* ---- window sheets (web, album): not a modal loop (a modal loop does not service the dispatch queues WebKit needs); the main thread keeps running and
 * the guest thread waits on a semaphore until a button or the close box finishes the sheet. ---- */
@interface SheetWin : NSObject <NSWindowDelegate>
@property(nonatomic) NSInteger code, closeCode;
@property(nonatomic) BOOL done;
@property(nonatomic, strong) NSWindow *win;
@property(nonatomic, strong) dispatch_semaphore_t sem;
@property(nonatomic, copy) void (^onFinish)(void);
- (void)finish:(NSInteger)code;
@end
@implementation SheetWin
- (void)finish:(NSInteger)c {
    if (self.done) return;
    self.done = YES;
    self.code = c;
    if (self.onFinish) self.onFinish();
    [self.win orderOut:nil];
    dispatch_semaphore_signal(self.sem);
}
- (BOOL)windowShouldClose:(NSWindow *)w {
    (void)w;
    [self finish:self.closeCode];
    return NO;
}
@end
/* Shows the window built by `setup` (main thread) and blocks the caller until it finishes; returns the code. */
static NSInteger run_sheet(SheetWin *(^setup)(void)) {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block SheetWin *owner = nil;
    on_main(^{
        owner = setup();
        if (!owner) return 0;
        owner.sem = sem;
        [NSApp activateIgnoringOtherApps:YES];
        [owner.win makeKeyAndOrderFront:nil];
        return 0;
    });
    if (!owner) return -1;
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    return owner.code;
}
static NSButton *sheet_button(NSString *title, NSInteger code, SheetWin *owner) {
    NSButton *b = [NSButton buttonWithTitle:title target:nil action:nil];
    fe_bind(b, ^(id s) {
        (void)s;
        [owner finish:code];
    });
    return b;
}
static NSWindow *sheet_window(NSString *title, NSSize size, SheetWin *owner) {
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    w.title = title;
    w.releasedWhenClosed = NO;
    w.delegate = owner;
    owner.win = w;
    [w center];
    return w;
}
static void press(NSArray<NSButton *> *buttons, NSArray<NSString *> *names) {
    NSString *act;
    double ms;
    if (!auto_action(&act, &ms)) return;
    after_ms(ms, ^{
        NSUInteger i = [names indexOfObject:act];
        NSButton *b = buttons[i == NSNotFound ? 0 : i];
        [b sendAction:b.action to:b.target];
    });
}

int napple_frontend_sheet_image(napple_frontend *f, const uint8_t *rgba, unsigned w, unsigned h, unsigned index, unsigned count, int can_select) {
    (void)f;
    NSData *px = [NSData dataWithBytes:rgba length:(NSUInteger)w * h * 4]; /* the caller's buffer is reused: copy before leaving the thread */
    NSInteger code = run_sheet(^SheetWin *{
        SheetWin *owner = [SheetWin new];
        owner.closeCode = -1;
        NSWindow *win = sheet_window([NSString stringWithFormat:@"Album %u of %u", index + 1, count], NSMakeSize(720, 480), owner);
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                        pixelsWide:w
                                                                        pixelsHigh:h
                                                                     bitsPerSample:8
                                                                   samplesPerPixel:4
                                                                          hasAlpha:YES
                                                                          isPlanar:NO
                                                                    colorSpaceName:NSDeviceRGBColorSpace
                                                                       bytesPerRow:w * 4
                                                                      bitsPerPixel:32];
        memcpy(rep.bitmapData, px.bytes, px.length);
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(w, h)];
        [img addRepresentation:rep];
        NSImageView *iv = [NSImageView imageViewWithImage:img];
        iv.imageScaling = NSImageScaleProportionallyUpOrDown;
        iv.translatesAutoresizingMaskIntoConstraints = NO;
        NSButton *sel = can_select ? sheet_button(@"Select", 1, owner) : nil;
        NSButton *next = sheet_button(index + 1 < count ? @"Next" : @"Done", index + 1 < count ? 0 : -1, owner);
        NSButton *leave = sheet_button(@"Close album", -1, owner);
        sel.keyEquivalent = @"\r";
        if (!sel) next.keyEquivalent = @"\r";
        NSMutableArray *views = [NSMutableArray arrayWithObject:leave];
        [views addObject:next];
        if (sel) [views addObject:sel];
        NSStackView *row = [NSStackView stackViewWithViews:views];
        row.spacing = 8;
        NSTextField *cap = fe_label([NSString stringWithFormat:@"%u / %u", index + 1, count], 12, NO);
        NSStackView *col = [NSStackView stackViewWithViews:@[ iv, cap, row ]];
        col.orientation = NSUserInterfaceLayoutOrientationVertical;
        col.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
        col.spacing = 8;
        win.contentView = col;
        [iv setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
        [iv setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        NSMutableArray *btns = [NSMutableArray array];
        NSMutableArray *names = [NSMutableArray array];
        if (sel) {
            [btns addObject:sel];
            [names addObject:@"confirm"];
        }
        [btns addObject:next];
        [names addObject:@"next"];
        [btns addObject:leave];
        [names addObject:@"leave"];
        press(btns, names);
        if (getenv("NAPPLE_SHEET_AUTO"))
            after_ms(200, ^{ shot_window(win, @"album"); });
        return owner;
    });
    return (int)code;
}

/* ---- web ---- */
@interface WebSheet : NSObject <WKNavigationDelegate>
@property(nonatomic) BOOL failed, loaded;
@property(nonatomic, strong) NSTextField *address;
@property(nonatomic, weak) SheetWin *owner;
@end
@implementation WebSheet
- (void)webView:(WKWebView *)w didFinishNavigation:(WKNavigation *)n {
    (void)n;
    self.loaded = YES;
    if (getenv("NAPPLE_SHEET_DEBUG"))
        [w evaluateJavaScript:@"document.body ? document.body.innerText : 'no body'" completionHandler:^(id r, NSError *e) { fprintf(stderr, "web page text: %s %s\n", [[r description] UTF8String], e.description.UTF8String); }];
    self.address.stringValue = w.URL.absoluteString ?: @"";
}
- (void)webView:(WKWebView *)w didFailNavigation:(WKNavigation *)n withError:(NSError *)e {
    (void)w;
    (void)n;
    if (getenv("NAPPLE_SHEET_DEBUG")) fprintf(stderr, "web: navigation failed: %s\n", e.description.UTF8String);
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)w {
    (void)w;
    if (getenv("NAPPLE_SHEET_DEBUG")) fprintf(stderr, "web: the web content process terminated\n");
}
- (void)webView:(WKWebView *)w didCommitNavigation:(WKNavigation *)n {
    (void)n;
    if (getenv("NAPPLE_SHEET_DEBUG")) fprintf(stderr, "web: commit %s\n", w.URL.absoluteString.UTF8String);
    self.address.stringValue = w.URL.absoluteString ?: @"";
}
- (void)webView:(WKWebView *)w didFailProvisionalNavigation:(WKNavigation *)n withError:(NSError *)e {
    (void)w;
    (void)n;
    if (!self.loaded && e.code != NSURLErrorCancelled) {
        self.failed = YES;
        [self.owner finish:-1];
    }
}
@end

int napple_frontend_sheet_web(napple_frontend *f, const char *url, const char *read_root, char *last_url, size_t cap) {
    (void)f;
    NSString *u = @(url ? url : ""), *root = read_root && *read_root ? @(read_root) : nil;
    __block NSString *last = u;
    NSInteger code = run_sheet(^SheetWin *{
        NSURL *target = [NSURL URLWithString:u];
        if (!target || (!target.isFileURL && ![@[ @"http", @"https" ] containsObject:target.scheme.lowercaseString])) return nil;
        SheetWin *owner = [SheetWin new];
        owner.closeCode = 1; /* the close box: window closed */
        NSWindow *win = sheet_window(target.isFileURL ? @"Offline page" : @"Web", NSMakeSize(1000, 700), owner);
        WKWebView *wv = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:[WKWebViewConfiguration new]];
        WebSheet *nav = [WebSheet new];
        nav.owner = owner;
        objc_setAssociatedObject(owner, "nav", nav, OBJC_ASSOCIATION_RETAIN); /* the web view only holds its delegate weakly */
        owner.onFinish = ^{
            if (!nav.failed) last = wv.URL.absoluteString ?: u;
        };
        wv.navigationDelegate = nav;
        NSButton *back = [NSButton buttonWithTitle:@"◀" target:nil action:nil], *fwd = [NSButton buttonWithTitle:@"▶" target:nil action:nil];
        fe_bind(back, ^(id s) {
            (void)s;
            [wv goBack];
        });
        fe_bind(fwd, ^(id s) {
            (void)s;
            [wv goForward];
        });
        NSButton *done = sheet_button(@"Done", 0, owner);
        done.keyEquivalent = @"\r";
        NSTextField *addr = fe_label(u, 12, NO);
        addr.lineBreakMode = NSLineBreakByTruncatingMiddle;
        [addr setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        nav.address = addr;
        NSStackView *bar = [NSStackView stackViewWithViews:@[ back, fwd, addr, done ]];
        bar.spacing = 8;
        NSStackView *col = [NSStackView stackViewWithViews:@[ bar, wv ]];
        col.orientation = NSUserInterfaceLayoutOrientationVertical;
        col.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
        col.spacing = 8;
        [wv setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
        [wv setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        win.contentView = col;
        if (target.isFileURL) { /* read scope: the title's HTML root when known, else the page's own folder */
            NSURL *scope = root ? [NSURL fileURLWithPath:root isDirectory:YES] : target.URLByDeletingLastPathComponent;
            [wv loadFileURL:target allowingReadAccessToURL:scope];
        } else [wv loadRequest:[NSURLRequest requestWithURL:target]];
        press(@[ done ], @[ @"confirm" ]);
        if (getenv("NAPPLE_SHEET_AUTO")) {
            after_ms(1200, ^{ shot_window(win, @"web"); });
        }
        return owner;
    });
    if (last_url && cap) snprintf(last_url, cap, "%s", last.UTF8String);
    return (int)code;
}
