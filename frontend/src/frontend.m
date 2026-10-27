/* The macOS window, event loop, settings persistence, menus, focus and screenshot hotkey.
   UI of the overlay is in overlay.m, the swkbd text bar in textbar.m. */
#import <CoreVideo/CoreVideo.h>
#import <objc/runtime.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fe_internal.h"
#include "napple_audio.h"

struct napple_frontend {
    void *impl;
}; /* impl: retained NappleFE */
static NappleFE *FE(napple_frontend *f) { return (__bridge NappleFE *)f->impl; }
#define LOGF(...)                                                                                                                                              \
    do {                                                                                                                                                       \
        fprintf(stderr, "[napple] frontend: ");                                                                                                                \
        fprintf(stderr, __VA_ARGS__);                                                                                                                          \
        fprintf(stderr, "\n");                                                                                                                                 \
    } while (0)

/* ------------------------------------------------------------------ helpers */
@interface FEAct : NSObject
@property (copy) void (^blk)(id);
- (void)fire:(id)sender;
@end
@implementation FEAct
- (void)fire:(id)sender {
    if (self.blk) self.blk(sender);
}
@end
void fe_bind(NSControl *c, void (^blk)(id)) {
    FEAct *a = [FEAct new];
    a.blk = blk;
    c.target = a;
    c.action = @selector(fire:);
    objc_setAssociatedObject(c, "act", a, OBJC_ASSOCIATION_RETAIN);
}
NSButton *fe_check(NSString *t, BOOL on, void (^blk)(id)) {
    NSButton *b = [NSButton checkboxWithTitle:t target:nil action:nil];
    b.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    fe_bind(b, blk);
    return b;
}
NSPopUpButton *fe_popup(NSArray<NSString *> *items, NSInteger sel, void (^blk)(id)) {
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [p addItemsWithTitles:items];
    if (sel >= 0 && sel < (NSInteger)items.count) [p selectItemAtIndex:sel];
    fe_bind(p, blk);
    return p;
}
NSTextField *fe_label(NSString *text, CGFloat size, BOOL bold) {
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
    l.textColor = NSColor.labelColor;
    return l;
}

static int mkdir_p(const char *path) {
    char tmp[1100];
    strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
            *p = '/';
        }
    return mkdir(tmp, 0755) && errno != EEXIST ? -1 : 0;
}

/* ------------------------------------------------------------------ game view */
@interface GameView : NSView
@property (weak) NappleFE *fe;
@end
@implementation GameView
- (instancetype)initWithFrame:(NSRect)r {
    if ((self = [super initWithFrame:r])) { self.wantsLayer = YES; }
    return self;
}
- (CALayer *)makeBackingLayer {
    CAMetalLayer *l = [CAMetalLayer layer];
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = YES;
    l.opaque = YES;
    l.contentsGravity = kCAGravityResize;
    l.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    return l;
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent *)e {
    (void)e;
    return YES;
}
- (void)updateDrawable {
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    CGFloat s = self.window ? self.window.backingScaleFactor : 1;
    l.contentsScale = s;
    l.drawableSize = CGSizeMake(MAX(1, self.bounds.size.width * s), MAX(1, self.bounds.size.height * s));
    NappleFE *fe = self.fe;
    if (self.window.screen) fe.displayHz = (double)self.window.screen.maximumFramesPerSecond;
    if (fe && fe.cfg.callbacks.on_resize) fe.cfg.callbacks.on_resize(fe.cfg.callbacks.user, (int)l.drawableSize.width, (int)l.drawableSize.height, s);
}
- (void)setFrameSize:(NSSize)s {
    [super setFrameSize:s];
    [self updateDrawable];
    if (self.fe.overlayVisible) fe_overlay_place(self.fe);
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateDrawable];
}
- (void)updateTrackingAreas {
    for (NSTrackingArea *t in self.trackingAreas) [self removeTrackingArea:t];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                       options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                                         owner:self
                                                      userInfo:nil]];
    [super updateTrackingAreas];
}
- (void)resetCursorRects {
    NappleFE *fe = self.fe;
    if (fe && !fe.settings.show_touch_cursor) {
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
        [self addCursorRect:self.bounds cursor:[[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint]];
    }
}
- (void)forwardMouse:(NSEvent *)e {
    NappleFE *fe = self.fe;
    if (!fe.input) return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    napple_input_mouse_position(fe.input, (float)(p.x / MAX(1, self.bounds.size.width)), (float)(1.0 - p.y / MAX(1, self.bounds.size.height)));
}
- (void)mouseMoved:(NSEvent *)e {
    [self forwardMouse:e];
}
- (void)mouseDragged:(NSEvent *)e {
    [self forwardMouse:e];
}
- (void)rightMouseDragged:(NSEvent *)e {
    [self forwardMouse:e];
}
- (void)otherMouseDragged:(NSEvent *)e {
    [self forwardMouse:e];
}
- (void)button:(NSEvent *)e down:(BOOL)d {
    NappleFE *fe = self.fe;
    [self forwardMouse:e];
    int b = e.type == NSEventTypeLeftMouseDown || e.type == NSEventTypeLeftMouseUp     ? 0
            : e.type == NSEventTypeRightMouseDown || e.type == NSEventTypeRightMouseUp ? 1
                                                                                       : 2;
    if (fe.input) napple_input_mouse_button(fe.input, b, d);
}
- (void)mouseDown:(NSEvent *)e {
    [self.window makeFirstResponder:self];
    [self button:e down:YES];
}
- (void)mouseUp:(NSEvent *)e {
    [self button:e down:NO];
}
- (void)rightMouseDown:(NSEvent *)e {
    [self button:e down:YES];
}
- (void)rightMouseUp:(NSEvent *)e {
    [self button:e down:NO];
}
- (void)otherMouseDown:(NSEvent *)e {
    [self button:e down:YES];
}
- (void)otherMouseUp:(NSEvent *)e {
    [self button:e down:NO];
}
- (void)scrollWheel:(NSEvent *)e {
    NappleFE *fe = self.fe;
    if (fe.input) napple_input_mouse_scroll(fe.input, (float)e.scrollingDeltaX, (float)e.scrollingDeltaY);
}
- (void)keyDown:(NSEvent *)e {
    NappleFE *fe = self.fe;
    if (e.keyCode == 122 && !e.isARepeat) {
        [fe toggleOverlay];
        return;
    } /* F1 */
    if (e.keyCode == 111 && !e.isARepeat) {
        [fe screenshot];
        return;
    } /* F12 */
    if ([fe speedKey:e.keyCode down:YES repeat:e.isARepeat]) return; /* F2..F10, `, [, ], \ : speed, frame generation, statistics */
    if (e.keyCode == 53 && fe.overlayVisible && !e.isARepeat && !(fe.input && napple_input_capture_pending(fe.input))) {
        [fe showOverlay:NO];
        return;
    } /* Esc closes the overlay */
    if (e.modifierFlags & NSEventModifierFlagCommand) return; /* menu shortcuts, no keyUp follows */
    if (fe.input && !e.isARepeat) napple_input_key(fe.input, e.keyCode, 1);
}
- (void)keyUp:(NSEvent *)e {
    NappleFE *fe = self.fe;
    if ([fe speedKey:e.keyCode down:NO repeat:NO]) return;
    if (fe.input) napple_input_key(fe.input, e.keyCode, 0);
}
- (void)flagsChanged:(NSEvent *)e {
    NappleFE *fe = self.fe;
    if (!fe.input) return;
    NSEventModifierFlags f = e.modifierFlags;
    BOOL down = NO;
    switch (e.keyCode) {
        case 56:
        case 60: down = f & NSEventModifierFlagShift; break;
        case 59:
        case 62: down = f & NSEventModifierFlagControl; break;
        case 58:
        case 61: down = f & NSEventModifierFlagOption; break;
        case 55:
        case 54: down = f & NSEventModifierFlagCommand; break;
        case 57: down = f & NSEventModifierFlagCapsLock; break;
        default: return;
    }
    napple_input_key(fe.input, e.keyCode, down);
}
@end

/* ------------------------------------------------------------------ vsync */
static dispatch_semaphore_t g_vsync;
static CVDisplayLinkRef g_link;
static CVReturn link_cb(CVDisplayLinkRef l, const CVTimeStamp *n, const CVTimeStamp *o, CVOptionFlags fi, CVOptionFlags *fo, void *c) {
    (void)l;
    (void)n;
    (void)o;
    (void)fi;
    (void)fo;
    (void)c;
    dispatch_semaphore_signal(g_vsync);
    return kCVReturnSuccess;
}

/* ------------------------------------------------------------------ NappleFE */
@implementation NappleFE {
    pthread_mutex_t _lock;
    _Atomic uint64_t _frames;
    NSTimer *_fpsTimer;
    uint64_t _lastFrames;
    CFTimeInterval _lastT;
    NSTextField *_bootStage;
    NSProgressIndicator *_bootBar;
    NSVisualEffectView *_notice;
    NSTextField *_noticeText;
    NSProgressIndicator *_noticeSpin;
    BOOL _turboHeld, _turboToggle, _slowToggle, _hudApplied;
    NSTextField *_toast, *_stats;
    NSUInteger _toastSerial;
}
- (instancetype)init {
    if ((self = [super init])) pthread_mutex_init(&_lock, NULL);
    return self;
}
- (napple_settings)currentSettings {
    pthread_mutex_lock(&_lock);
    napple_settings s = _settings;
    pthread_mutex_unlock(&_lock);
    return s;
}

- (void)loadSettings {
    napple_settings s;
    napple_settings_default(&s);
    char path[1100];
    if (napple_data_path(self.cfg.config_dir, self.cfg.title_id, "settings.json", path, sizeof path) == 0) {
        FILE *f = fopen(path, "rb");
        if (f) {
            static char buf[65537];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            napple_settings r;
            if (napple_settings_from_json(buf, n, &r) == 0) s = r;
            else LOGF("%s is invalid: using defaults (file kept)", path);
        }
    }
    _settings = s;
}
- (void)saveSettings {
    napple_settings s = [self currentSettings];
    char path[1100], dir[1100];
    if (napple_data_path(self.cfg.config_dir, self.cfg.title_id, "settings.json", path, sizeof path)) return;
    strlcpy(dir, path, sizeof dir);
    *strrchr(dir, '/') = 0;
    char json[8192];
    size_t n = napple_settings_to_json(&s, json, sizeof json);
    if (n >= sizeof json || mkdir_p(dir)) return;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        LOGF("cannot write %s: %s", path, strerror(errno));
        return;
    }
    int ok = fwrite(json, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, path)) {
        unlink(tmp);
        LOGF("cannot save %s", path);
    }
}

- (void)applySettings:(napple_settings)s save:(BOOL)save notify:(BOOL)notify {
    napple_settings_clamp(&s);
    pthread_mutex_lock(&_lock);
    napple_settings old = _settings;
    _settings = s;
    pthread_mutex_unlock(&_lock);
    if (save) [self saveSettings];
    napple_audio_set_master_volume(s.master_volume);
    if (s.fullscreen != old.fullscreen && ((self.window.styleMask & NSWindowStyleMaskFullScreen) != 0) != (s.fullscreen != 0))
        [self.window toggleFullScreen:nil];
    if (!(self.window.styleMask & NSWindowStyleMaskFullScreen) && (s.output_width != old.output_width || s.output_height != old.output_height))
        [self.window setContentSize:NSMakeSize(s.output_width, s.output_height)];
    if (s.aspect != old.aspect) [self applyAspect];
    if (s.show_touch_cursor != old.show_touch_cursor) [self.window invalidateCursorRectsForView:self.gameView];
    if (self.layer) self.layer.displaySyncEnabled = s.vsync != 0;
    if (self.layer && (s.metal_hud != old.metal_hud || !_hudApplied)) {   /* Apple's native Metal performance HUD */
        _hudApplied = YES;
        if (@available(macOS 13.0, *)) self.layer.developerHUDProperties = s.metal_hud ? @{@"mode" : @"default"} : nil;
    }
    if (s.panel_passthrough != old.panel_passthrough) [self refreshOverlayInputMode];
    if (notify && self.cfg.callbacks.on_settings_changed) self.cfg.callbacks.on_settings_changed(self.cfg.callbacks.user, &s);
    if (notify) [self updateSpeed];
}
- (void)applyAspect {
    /* contentAspectRatio and resizeIncrements override each other: stretch uses 1-point increments */
    int a = _settings.aspect;
    if (a >= 3) {
        self.window.resizeIncrements = NSMakeSize(1, 1);
    } /* free window: any size and shape */
    else
        self.window.contentAspectRatio = a == 2 ? NSMakeSize(4, 3) : NSMakeSize(16, 9);
}

/* ---- speed / display hot keys ---- */
static const int kSpeedSteps[] = NAPPLE_SPEED_STEPS;
static const int kSuperSteps[] = {100, 150, 200, 300};
- (void)toast:(NSString *)text {
    if (!self.gameView) return;
    if (!_toast) {
        _toast = fe_label(@"", 15, YES);
        _toast.textColor = NSColor.whiteColor;
        _toast.drawsBackground = YES;
        _toast.backgroundColor = [NSColor colorWithWhite:0 alpha:0.55];
        _toast.wantsLayer = YES;
        _toast.layer.cornerRadius = 6;
        _toast.alignment = NSTextAlignmentCenter;
        [self.gameView addSubview:_toast];
    }
    _toast.stringValue = [NSString stringWithFormat:@"  %@  ", text];
    [_toast sizeToFit];
    NSRect b = self.gameView.bounds;
    _toast.frame = NSMakeRect(14, NSMaxY(b) - _toast.frame.size.height - 14, _toast.frame.size.width, _toast.frame.size.height + 4);
    _toast.hidden = NO;
    _toast.alphaValue = 1;
    NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.6 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (serial == self->_toastSerial) self->_toast.hidden = YES;
    });
}
- (void)updateSpeed {
    napple_settings s = [self currentSettings];
    int eff = napple_effective_speed_percent(&s, _turboHeld, _turboToggle, _slowToggle);
    if (self.cfg.callbacks.on_speed) self.cfg.callbacks.on_speed(self.cfg.callbacks.user, eff);
}
- (void)setSettingsMutating:(void (^)(napple_settings *))mut {
    napple_settings s = [self currentSettings];
    mut(&s);
    [self applySettings:s save:YES notify:YES];
}
- (BOOL)speedKey:(unsigned short)code down:(BOOL)down repeat:(BOOL)repeat {
    switch (code) {
        case 120:
        case 50: { /* F2 or ` : turbo while held */
            if (repeat) return YES;
            _turboHeld = down;
            [self updateSpeed];
            if (down) {
                napple_settings s = [self currentSettings];
                [self toast:[NSString stringWithFormat:@"Turbo %.1fx", s.turbo_percent / 100.0]];
            } else [self toast:@"Turbo off"];
            return YES;
        }
        case 99:
        case 118:
        case 96:
        case 33:
        case 97:
        case 30:
        case 98:
        case 42:
        case 100:
        case 101:
        case 109: break;
        default: return NO;
    }
    if (!down) return YES; /* act on key down only; key up of these keys is swallowed */
    if (repeat) return YES;
    napple_settings s = [self currentSettings];
    switch (code) {
        case 99:
            _turboToggle = !_turboToggle;
            [self updateSpeed];
            [self toast:_turboToggle ? [NSString stringWithFormat:@"Turbo on %.1fx", s.turbo_percent / 100.0] : @"Turbo off"];
            break; /* F3 */
        case 118:
            _slowToggle = !_slowToggle;
            [self updateSpeed];
            [self toast:_slowToggle ? @"Slow motion 0.5x" : @"Slow motion off"];
            break; /* F4 */
        case 96:
        case 33:
        case 97:
        case 30: { /* F5 / [ : slower, F6 / ] : faster */
            BOOL up = code == 97 || code == 30;
            int n = sizeof kSpeedSteps / sizeof *kSpeedSteps, cur = s.speed_percent, to = cur;
            if (up) {
                for (int i = 0; i < n; i++)
                    if (kSpeedSteps[i] > cur) {
                        to = kSpeedSteps[i];
                        break;
                    }
            } else {
                for (int i = n - 1; i >= 0; i--)
                    if (kSpeedSteps[i] < cur) {
                        to = kSpeedSteps[i];
                        break;
                    }
            }
            _turboToggle = _slowToggle = NO;
            [self setSettingsMutating:^(napple_settings *t) { t->speed_percent = to; }];
            [self updateSpeed];
            [self toast:[NSString stringWithFormat:@"Game speed %.2gx", to / 100.0]];
            fe_overlay_refresh(self);
            break;
        }
        case 98:
        case 42:
            _turboToggle = _slowToggle = NO;
            [self setSettingsMutating:^(napple_settings *t) { t->speed_percent = 100; }];
            [self updateSpeed];
            [self toast:@"Game speed 1x"];
            fe_overlay_refresh(self);
            break; /* F7 */
        case 100: {
            BOOL on = !s.frame_gen;
            [self setSettingsMutating:^(napple_settings *t) { t->frame_gen = on; }];
            [self toast:on ? @"Frame generation on" : @"Frame generation off"];
            fe_overlay_refresh(self);
            break;
        } /* F8 */
        case 101: {
            BOOL on = !s.show_stats;
            [self setSettingsMutating:^(napple_settings *t) { t->show_stats = on; }];
            fe_overlay_refresh(self);
            break;
        } /* F9 */
        case 109: { /* F10 */
            int n = sizeof kSuperSteps / sizeof *kSuperSteps, to = kSuperSteps[0];
            for (int i = 0; i < n; i++)
                if (kSuperSteps[i] == s.supersample_percent) to = kSuperSteps[(i + 1) % n];
            [self setSettingsMutating:^(napple_settings *t) { t->supersample_percent = to; }];
            [self toast:[NSString stringWithFormat:@"Supersampling %.1fx", to / 100.0]];
            fe_overlay_refresh(self);
            break;
        }
    }
    return YES;
}

- (void)refreshStats {
    napple_settings st = [self currentSettings];
    BOOL on = st.show_stats && self.cfg.callbacks.stats_text;
    if (!on) {
        if (_stats) _stats.hidden = YES;
        return;
    }
    if (!_stats) {
        _stats = [NSTextField labelWithString:@""];
        _stats.textColor = NSColor.whiteColor;
        _stats.drawsBackground = YES;
        [self.gameView addSubview:_stats positioned:NSWindowBelow relativeTo:self.overlayView];
    }
    static const CGFloat sizes[3] = {10, 12, 15};
    _stats.font = [NSFont monospacedSystemFontOfSize:sizes[st.stats_size] weight:NSFontWeightMedium];
    _stats.backgroundColor = [NSColor colorWithWhite:0 alpha:st.stats_opacity / 100.0];
    char buf[400] = "";
    self.cfg.callbacks.stats_text(self.cfg.callbacks.user, buf, sizeof buf);
    _stats.stringValue = [NSString stringWithFormat:@" %s ", buf];
    [_stats sizeToFit];
    NSRect gb = self.gameView.bounds;
    CGFloat w = MIN(_stats.frame.size.width, gb.size.width - 16), h = _stats.frame.size.height;
    CGFloat x = (st.stats_corner & 1) ? gb.size.width - w - 8 : 8;                  /* 1, 3 = right */
    CGFloat y = (st.stats_corner & 2) ? 8 : gb.size.height - h - 8;                 /* 2, 3 = bottom (the view is not flipped: y grows upwards) */
    _stats.frame = NSMakeRect(x, y, w, h);
    _stats.hidden = NO;
}

- (void)buildMenu {
    NSMenu *bar = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu *app = [NSMenu new];
    NSString *name = self.titleName;
    [app addItemWithTitle:[@"About " stringByAppendingString:name] action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [app addItem:NSMenuItem.separatorItem];
    NSMenuItem *set = [app addItemWithTitle:@"Settings (F1)" action:@selector(menuOverlay:) keyEquivalent:@","];
    set.target = self;
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
    [app addItem:NSMenuItem.separatorItem];
    NSMenuItem *q = [app addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(menuQuit:) keyEquivalent:@"q"];
    q.target = self;
    appItem.submenu = app;
    NSMenuItem *viewItem = [NSMenuItem new];
    [bar addItem:viewItem];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *fs = [view addItemWithTitle:@"Toggle Full Screen" action:@selector(menuFullscreen:) keyEquivalent:@"f"];
    fs.target = self;
    fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    NSMenuItem *fs2 = [view addItemWithTitle:@"Toggle Full Screen" action:@selector(menuFullscreen:) keyEquivalent:@"\r"];
    fs2.target = self;
    fs2.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    fs2.hidden = YES;
    fs2.allowsKeyEquivalentWhenHidden = YES;
    NSMenuItem *sh = [view addItemWithTitle:@"Take Screenshot (F12)" action:@selector(menuShot:) keyEquivalent:@""];
    sh.target = self;
    viewItem.submenu = view;
    NSApp.mainMenu = bar;
}
- (void)menuOverlay:(id)s {
    (void)s;
    [self toggleOverlay];
}
- (void)menuQuit:(id)s {
    (void)s;
    [self quit];
}
- (void)menuShot:(id)s {
    (void)s;
    [self screenshot];
}
- (void)menuFullscreen:(id)s {
    (void)s;
    [self.window toggleFullScreen:nil];
}

- (void)setUp {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSApp.delegate = self;
    [self loadSettings];
    NSRect r = NSMakeRect(0, 0, _settings.output_width, _settings.output_height);
    self.window =
        [[NSWindow alloc] initWithContentRect:r
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.window.title = self.titleName;
    self.window.delegate = self;
    self.window.releasedWhenClosed = NO;
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    self.window.acceptsMouseMovedEvents = YES;
    GameView *gv = [[GameView alloc] initWithFrame:r];
    gv.fe = self;
    gv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.gameView = gv;
    self.window.contentView = gv;
    self.layer = (CAMetalLayer *)gv.layer;
    self.layer.displaySyncEnabled = _settings.vsync != 0;
    [self.window center];
    [self applyAspect];
    [self buildMenu];
    [self buildBootView];
    [NSTimer scheduledTimerWithTimeInterval:0.25
                                    repeats:YES
                                      block:^(NSTimer *t) {
                                          (void)t;
                                          [self refreshStats];
                                      }];
    fe_nav_start(self);
    fe_simulators_install(); /* webcam -> IR camera source; the prompt appears only when a game starts an IR processor */
    self.overlayView = fe_build_overlay(self);
    self.overlayView.hidden = YES;
    [gv addSubview:self.overlayView];
    g_vsync = dispatch_semaphore_create(0);
    if (CVDisplayLinkCreateWithActiveCGDisplays(&g_link) == kCVReturnSuccess) {
        CVDisplayLinkSetOutputCallback(g_link, link_cb, NULL);
        CVDisplayLinkStart(g_link);
    }
    [gv updateDrawable];
    [self.window makeFirstResponder:gv]; /* key events must reach the game view without a click first */
    napple_audio_set_master_volume(_settings.master_volume);
}

- (void)buildBootView {
    NSVisualEffectView *v = [[NSVisualEffectView alloc] initWithFrame:self.gameView.bounds];
    v.material = NSVisualEffectMaterialHUDWindow;
    v.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    v.state = NSVisualEffectStateActive;
    v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSTextField *t = fe_label(self.titleName, 22, YES);
    _bootStage = [NSTextField wrappingLabelWithString:@"Starting"];
    _bootStage.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightRegular];
    _bootStage.alignment = NSTextAlignmentCenter;
    _bootStage.preferredMaxLayoutWidth = 420;
    _bootStage.maximumNumberOfLines = 3;
    _bootBar = [NSProgressIndicator new];
    _bootBar.style = NSProgressIndicatorStyleBar;
    _bootBar.indeterminate = YES;
    [_bootBar startAnimation:nil];
    _bootBar.minValue = 0;
    _bootBar.maxValue = 1;
    NSStackView *st = [NSStackView stackViewWithViews:@[ t, _bootStage, _bootBar ]];
    st.orientation = NSUserInterfaceLayoutOrientationVertical;
    st.spacing = 10;
    st.alignment = NSLayoutAttributeCenterX;
    st.translatesAutoresizingMaskIntoConstraints = NO;
    [v addSubview:st];
    [NSLayoutConstraint activateConstraints:@[
        [st.centerXAnchor constraintEqualToAnchor:v.centerXAnchor], [st.centerYAnchor constraintEqualToAnchor:v.centerYAnchor],
        [_bootBar.widthAnchor constraintEqualToConstant:280]
    ]];
    self.bootView = v;
    [self.gameView addSubview:v];
}

- (void)toggleOverlay {
    [self toggleOverlayFromPad:NO];
}
- (void)toggleOverlayFromPad:(BOOL)pad {
    if (!self.overlayVisible) self.overlayPadMode = pad;
    [self showOverlay:!self.overlayVisible];
}
/* The game keeps its input while the panel is open (keyboard / mouse), unless the pad opened it or the user turned pass-through off. */
- (void)refreshOverlayInputMode {
    if (self.input) napple_input_set_blocked(self.input, (self.overlayVisible && (self.overlayPadMode || !self.settings.panel_passthrough)) || fe_textbar_visible(self));
    fe_nav_mode_changed();
}
- (void)showOverlay:(BOOL)show {
    if (show == self.overlayVisible) return;
    self.overlayVisible = show;
    self.overlayView.hidden = !show;
    if (show) {
        fe_overlay_place(self);
        fe_overlay_refresh(self);
    } else self.overlayPadMode = NO;
    [self refreshOverlayInputMode];
    if (!show) [self resignToGame];
    if (self.cfg.callbacks.on_overlay) self.cfg.callbacks.on_overlay(self.cfg.callbacks.user, show);
}
- (void)resignToGame {
    [self.window makeFirstResponder:self.gameView];
}

- (void)screenshot {
    NSDateFormatter *df = [NSDateFormatter new];
    df.dateFormat = @"yyyyMMdd-HHmmss";
    NSString *dir = self.shotDir ?: [NSHomeDirectory() stringByAppendingPathComponent:@"Pictures/Napple"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *safe = [[self.titleName componentsSeparatedByCharactersInSet:NSCharacterSet.alphanumericCharacterSet.invertedSet] componentsJoinedByString:@"_"];
    NSString *path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@.png", safe, [df stringFromDate:NSDate.date]]];
    if (self.cfg.callbacks.on_screenshot) {
        if (self.cfg.callbacks.on_screenshot(self.cfg.callbacks.user, path.UTF8String) == 0) {
            LOGF("screenshot %s", path.UTF8String);
            return;
        }
    }
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/screencapture"];
    t.arguments = @[ @"-x", @"-o", [NSString stringWithFormat:@"-l%ld", (long)self.window.windowNumber], path ];
    NSError *err = nil;
    if ([t launchAndReturnError:&err]) LOGF("screenshot -> %s", path.UTF8String);
    else LOGF("screenshot failed: %s", err.localizedDescription.UTF8String);
}

- (void)restartGuest {
    napple_settings s = [self currentSettings];
    LOGF("restart guest requested (update %s, %d DLC disabled)", s.update_version[0] ? s.update_version : "latest", s.disabled_dlc_count);
    if (self.cfg.callbacks.on_restart_guest) self.cfg.callbacks.on_restart_guest(self.cfg.callbacks.user, &s);
}

- (void)quit {
    if (self.quitting) return;
    self.quitting = YES;
    if (self.cfg.callbacks.on_quit) self.cfg.callbacks.on_quit(self.cfg.callbacks.user);
    [NSApp stop:nil];
    NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                    location:NSZeroPoint
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:0
                                     context:nil
                                     subtype:0
                                       data1:0
                                       data2:0];
    [NSApp postEvent:e atStart:YES];
}

/* delegates */
- (BOOL)windowShouldClose:(id)s {
    (void)s;
    [self quit];
    return NO;
}
- (void)windowDidBecomeKey:(NSNotification *)n {
    (void)n;
    LOGF("window is key (first responder %s)", self.window.firstResponder == self.gameView ? "game view" : "other");
    if (!self.overlayVisible && !fe_textbar_visible(self)) [self.window makeFirstResponder:self.gameView];
    [self focus:YES];
}
- (void)windowDidResignKey:(NSNotification *)n {
    (void)n;
    LOGF("window lost key status");
    [self focus:NO];
}
- (void)windowDidChangeScreen:(NSNotification *)n {
    (void)n;
    [(GameView *)self.gameView updateDrawable];
}
- (void)windowDidEnterFullScreen:(NSNotification *)n {
    (void)n;
    [self syncFullscreen:YES];
}
- (void)windowDidExitFullScreen:(NSNotification *)n {
    (void)n;
    [self syncFullscreen:NO];
}
- (void)syncFullscreen:(BOOL)fs {
    pthread_mutex_lock(&_lock);
    BOOL changed = (_settings.fullscreen != 0) != fs;
    _settings.fullscreen = fs;
    pthread_mutex_unlock(&_lock);
    if (changed) [self saveSettings];
    if (self.cfg.callbacks.on_settings_changed) {
        napple_settings s = [self currentSettings];
        self.cfg.callbacks.on_settings_changed(self.cfg.callbacks.user, &s);
    }
}
- (void)focus:(BOOL)on {
    napple_settings s = [self currentSettings];
    if (!on && _turboHeld) {
        _turboHeld = NO;
        [self updateSpeed];
    } /* a held turbo key never gets its key up when the window loses focus */
    if (self.input) napple_input_focus(self.input, on);
    if (s.mute_in_background) napple_audio_set_paused(!on);
    if (self.cfg.callbacks.on_focus) self.cfg.callbacks.on_focus(self.cfg.callbacks.user, on);
}
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a {
    (void)a;
    return NO;
}

- (void)boot:(NSString *)stage fraction:(double)f {
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_bootStage.stringValue = stage ?: @"Starting";
        if (f < 0) {
            self->_bootBar.indeterminate = YES;
            [self->_bootBar startAnimation:nil];
        } else {
            self->_bootBar.indeterminate = NO;
            self->_bootBar.doubleValue = f > 1 ? 1 : f;
        }
    });
}
- (void)bootDone {
    dispatch_async(dispatch_get_main_queue(), ^{ self.bootView.hidden = YES; });
}
- (void)compileNotice:(NSString *)text {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!text) {
            if (self->_notice) {
                self->_notice.hidden = YES;
                [self->_noticeSpin stopAnimation:nil];
            }
            return;
        }
        if (!self->_notice) {
            NSVisualEffectView *v = [NSVisualEffectView new];
            v.material = NSVisualEffectMaterialHUDWindow;
            v.blendingMode = NSVisualEffectBlendingModeWithinWindow;
            v.state = NSVisualEffectStateActive;
            v.wantsLayer = YES;
            v.layer.cornerRadius = 8;
            v.translatesAutoresizingMaskIntoConstraints = NO;
            NSProgressIndicator *sp = [NSProgressIndicator new];
            sp.style = NSProgressIndicatorStyleSpinning;
            sp.controlSize = NSControlSizeSmall;
            sp.indeterminate = YES;
            NSTextField *l = fe_label(@"", 12, NO);
            l.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium];
            NSStackView *st = [NSStackView stackViewWithViews:@[ sp, l ]];
            st.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            st.spacing = 8;
            st.edgeInsets = NSEdgeInsetsMake(6, 10, 6, 12);
            st.translatesAutoresizingMaskIntoConstraints = NO;
            [v addSubview:st];
            [self.gameView addSubview:v positioned:NSWindowAbove relativeTo:nil];
            [NSLayoutConstraint activateConstraints:@[
                [st.leadingAnchor constraintEqualToAnchor:v.leadingAnchor], [st.trailingAnchor constraintEqualToAnchor:v.trailingAnchor],
                [st.topAnchor constraintEqualToAnchor:v.topAnchor], [st.bottomAnchor constraintEqualToAnchor:v.bottomAnchor],
                [v.topAnchor constraintEqualToAnchor:self.gameView.topAnchor constant:12],
                [v.trailingAnchor constraintEqualToAnchor:self.gameView.trailingAnchor constant:-12]
            ]];
            self->_notice = v;
            self->_noticeText = l;
            self->_noticeSpin = sp;
        }
        self->_noticeText.stringValue = text;
        self->_notice.hidden = NO;
        [self->_noticeSpin startAnimation:nil];
    });
}
- (void)framePresented {
    uint64_t n = atomic_fetch_add(&_frames, 1);
    if (n == 0) [self bootDone];
    if ((n & 31) == 0) {
        CFTimeInterval t = CACurrentMediaTime();
        if (t - _lastT >= 1.0) {
            double fps = (double)(n - _lastFrames) / (t - _lastT);
            _lastFrames = n;
            _lastT = t;
            dispatch_async(dispatch_get_main_queue(), ^{ self.window.title = [NSString stringWithFormat:@"%@ - %.0f fps", self.titleName, fps]; });
        }
    }
}
@end

/* Controller Home / Guide opens and closes the overlay, Share / Capture takes a screenshot (called on the input thread). */
static void fe_hotkey(void *user, int hotkey) {
    NappleFE *fe = (__bridge NappleFE *)user;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (hotkey == NAPPLE_HOTKEY_HOME) [fe toggleOverlayFromPad:YES];
        else if (hotkey == NAPPLE_HOTKEY_CAPTURE) [fe screenshot];
    });
}

/* ------------------------------------------------------------------ public C API */
napple_frontend *napple_frontend_create(const napple_frontend_config *c) {
    if (!c || ![NSThread isMainThread]) return NULL;
    @autoreleasepool {
        NappleFE *fe = [NappleFE new];
        fe.cfg = *c;
        fe.input = c->input;
        fe.titleID = c->title_id ? @(c->title_id) : @"default";
        fe.titleName = c->title_name ? @(c->title_name) : @"Napple";
        fe.dir = c->config_dir ? @(c->config_dir) : nil;
        fe.shotDir = c->screenshot_dir ? @(c->screenshot_dir) : nil;
        [fe setUp];
        if (c->input)
            napple_input_set_hotkey_callback(c->input, fe_hotkey,
                                             (__bridge void *)fe); /* pad Home / Share (the fe outlives the input thread: destroy stops input first) */
        if (!c->headless) { [fe.window makeKeyAndOrderFront:nil]; }
        if (getenv("NAPPLE_FE_VIEWSHOT")) { /* test aid: "<png path>:<seconds>" renders the overlay view to a PNG (the Metal layer itself is not captured) */
            NSString *spec = @(getenv("NAPPLE_FE_VIEWSHOT"));
            NSRange r = [spec rangeOfString:@":" options:NSBackwardsSearch];
            if (r.location != NSNotFound) {
                NSString *path = [spec substringToIndex:r.location];
                double secs = [spec substringFromIndex:r.location + 1].doubleValue;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                    NSView *v = fe.overlayView;
                    NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
                    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
                    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
                    LOGF("view snapshot %s", path.UTF8String);
                });
            }
        }
        if (getenv("NAPPLE_FE_OVERLAY")) {
            fe_overlay_select_tab(atoi(getenv("NAPPLE_FE_OVERLAY")));
            [fe showOverlay:YES];
        } /* test aid: open the overlay on a tab at start */
        napple_frontend *f = calloc(1, sizeof *f);
        f->impl = (void *)CFBridgingRetain(fe);
        return f;
    }
}
void napple_frontend_destroy(napple_frontend *f) {
    if (!f) return;
    NappleFE *fe = FE(f);
    if (g_link) {
        CVDisplayLinkStop(g_link);
        CVDisplayLinkRelease(g_link);
        g_link = NULL;
    }
    [fe.window orderOut:nil];
    fe.window.delegate = nil;
    CFBridgingRelease(f->impl);
    free(f);
}
int napple_frontend_run(napple_frontend *f) {
    NappleFE *fe = FE(f);
    if (!fe.cfg.headless) [NSApp activateIgnoringOtherApps:YES];
    napple_settings s = [fe currentSettings];
    if (s.fullscreen && !fe.cfg.headless) [fe.window toggleFullScreen:nil];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (fe.cfg.callbacks.on_ready && !fe.readyFired) {
            fe.readyFired = YES;
            fe.cfg.callbacks.on_ready(fe.cfg.callbacks.user);
        }
    });
    [NSApp run];
    return 0;
}
void napple_frontend_request_quit(napple_frontend *f) {
    NappleFE *fe = FE(f);
    dispatch_async(dispatch_get_main_queue(), ^{ [fe quit]; });
}
void *napple_frontend_metal_layer(napple_frontend *f) { return (__bridge void *)FE(f).layer; }
double napple_frontend_display_hz(napple_frontend *f) {
    double h = FE(f).displayHz;
    return h >= 24 ? h : 60;
}
void napple_frontend_drawable_size(napple_frontend *f, int *w, int *h, double *scale) {
    NappleFE *fe = FE(f);
    CGSize d = fe.layer.drawableSize;
    if (w) *w = (int)d.width;
    if (h) *h = (int)d.height;
    if (scale) *scale = fe.layer.contentsScale;
}
void napple_frontend_get_settings(napple_frontend *f, napple_settings *out) { *out = [FE(f) currentSettings]; }
int napple_frontend_set_settings(napple_frontend *f, const napple_settings *s) {
    if (!s) return -1;
    NappleFE *fe = FE(f);
    napple_settings c = *s;
    if ([NSThread isMainThread]) [fe applySettings:c save:YES notify:YES];
    else dispatch_async(dispatch_get_main_queue(), ^{ [fe applySettings:c save:YES notify:YES]; });
    return 0;
}
void napple_frontend_set_manifest(napple_frontend *f, const napple_manifest *m) {
    NappleFE *fe = FE(f);
    NSMutableArray *dlc = [NSMutableArray new], *upd = [NSMutableArray new];
    NSString *title = nil, *base = nil;
    if (m) {
        title = [NSString stringWithUTF8String:m->title_name];
        base = [NSString stringWithUTF8String:m->base_version];
        for (size_t i = 0; i < m->dlc_count && i < 64; i++) [dlc addObject:@{@"id" : @(m->dlc[i].title_id), @"name" : @(m->dlc[i].name)}];
        for (size_t i = 0; i < m->update_count && i < 64; i++) [upd addObject:@{@"version" : @(m->updates[i].version), @"name" : @(m->updates[i].name)}];
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        fe.manifestTitle = title;
        fe.manifestBase = base;
        fe.dlc = dlc;
        fe.updates = upd;
        if (fe.overlayVisible) fe_overlay_refresh(fe);
    });
}
void napple_frontend_show_overlay(napple_frontend *f, int v) {
    NappleFE *fe = FE(f);
    dispatch_async(dispatch_get_main_queue(), ^{ [fe showOverlay:v != 0]; });
}
void napple_frontend_boot_status(napple_frontend *f, const char *stage, double fraction) { [FE(f) boot:stage ? @(stage) : nil fraction:fraction]; }
void napple_frontend_boot_done(napple_frontend *f) { [FE(f) bootDone]; }
void napple_frontend_compile_notice(napple_frontend *f, const char *text) { [FE(f) compileNotice:text ? @(text) : nil]; }
void napple_frontend_frame_presented(napple_frontend *f) { [FE(f) framePresented]; }
int napple_frontend_wait_vsync(napple_frontend *f, int timeout_ms) {
    (void)f;
    if (!g_vsync) return 0;
    return dispatch_semaphore_wait(g_vsync, dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ms * NSEC_PER_MSEC)) == 0;
}

/* ---- swkbd ---- */
void napple_frontend_text_input(napple_frontend *f, const napple_text_request *req, napple_text_done_fn done, void *user) {
    NappleFE *fe = FE(f);
    /* tests / automation: answer without UI */
    const char *ans = getenv("NAPPLE_SWKBD_ANSWER");
    if (ans) {
        if (!strcmp(ans, "<cancel>")) done(user, 0, NULL);
        else if (!strcmp(ans, "<initial>")) done(user, req->initial != NULL, req->initial ? req->initial : "");
        else if (napple_text_validate(req, ans)) done(user, 1, ans);
        else done(user, 0, NULL);
        return;
    }
    /* deep copy the request: the caller's strings may not outlive this call */
    napple_text_request *copy = calloc(1, sizeof *copy);
    *copy = *req;
    copy->header = req->header ? strdup(req->header) : NULL;
    copy->sub = req->sub ? strdup(req->sub) : NULL;
    copy->guide = req->guide ? strdup(req->guide) : NULL;
    copy->initial = req->initial ? strdup(req->initial) : NULL;
    dispatch_async(dispatch_get_main_queue(), ^{
        fe_textbar_show(fe, copy, done, user);
        free((void *)copy->header);
        free((void *)copy->sub);
        free((void *)copy->guide);
        free((void *)copy->initial);
        free(copy);
    });
}
typedef struct {
    dispatch_semaphore_t sem;
    char *out;
    size_t cap;
    int accepted;
} Blocking;
static void blocking_done(void *u, int ok, const char *text) {
    Blocking *b = u;
    b->accepted = ok;
    if (ok && text && b->cap) strlcpy(b->out, text, b->cap);
    else if (b->cap) b->out[0] = 0;
    dispatch_semaphore_signal(b->sem);
}
int napple_frontend_text_input_blocking(napple_frontend *f, const napple_text_request *req, char *out, size_t cap) {
    if ([NSThread isMainThread]) return 0; /* would deadlock the UI thread */
    Blocking b = {dispatch_semaphore_create(0), out, cap, 0};
    napple_frontend_text_input(f, req, blocking_done, &b);
    dispatch_semaphore_wait(b.sem, DISPATCH_TIME_FOREVER);
    return b.accepted;
}
