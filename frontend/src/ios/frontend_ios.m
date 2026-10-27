/* frontend_ios.m - iPhone / iPad (iOS 26+) implementation of napple_frontend.h.
 *
 * Same C API as the macOS frontend (frontend.m, overlay.m, textbar.m) so runtime/src/app.m is shared:
 *   - UIKit app + UIScene life cycle (UIApplicationMain, NappleAppDelegate, NappleSceneDelegate; the Info.plist scene manifest names the delegate)
 *   - the CAMetalLayer gfx renders into is created in napple_frontend_create and hosted as a sublayer of the game view
 *   - touch screen: an on-screen controller (ios_logic.c geometry -> napple_input_virtual_pad, analog sticks), touches elsewhere act as the
 *     touch screen through napple_input_mouse_*; it hides while a physical controller is connected; hardware keyboards go through napple_input_key
 *   - settings sheet (gear button / controller Home button): video, audio, controls, speed, game (update + DLC, restart), screenshot, quit
 *   - boot view, shader-compile notice, statistics, toasts, nn::swkbd as an alert with a text field
 * Settings persistence and validation are the shared settings.c. Written without a Mac to build on: see docs/architecture/ios.md for what is verified. */
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "internal.h"
#import "fe_ios.h"
#include "ios_logic.h"
#include "napple_audio.h"
#include "napple_input.h"

#define LOGF(...)                                                                                                                                              \
    do {                                                                                                                                                       \
        fprintf(stderr, "[napple] frontend: ");                                                                                                                \
        fprintf(stderr, __VA_ARGS__);                                                                                                                          \
        fprintf(stderr, "\n");                                                                                                                                 \
    } while (0)

static const int kSpeedStepsIOS[] = NAPPLE_SPEED_STEPS;
struct napple_frontend {
    void *impl;
};
static NappleFEiOS *g_fe;
static dispatch_semaphore_t g_vsync;
static void fe_hotkey(void *user, int hotkey);

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

/* ------------------------------------------------------------------------------------------------------------------ */


/* ------------------------------------------------------------------------------------------------------------------ */
#pragma mark - on-screen controller

@interface PadView : UIView
@property (nonatomic, weak) NappleFEiOS *fe;
- (void)releaseAll;
@end
@implementation PadView {
    pad_ctl _ctl[PAD_MAX_CTLS];
    int _n;
    CAShapeLayer *_shape[PAD_MAX_CTLS];
    CATextLayer *_text[PAD_MAX_CTLS];
    CAShapeLayer *_knob[2];
    NSMapTable<UITouch *, NSNumber *> *_owner; /* touch -> control index */
    BOOL _down[PAD_MAX_CTLS];
    float _sx[2], _sy[2];
    UIImpactFeedbackGenerator *_haptic;
}
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        self.multipleTouchEnabled = YES;
        self.backgroundColor = UIColor.clearColor;
        self.opaque = NO;
        _owner = [NSMapTable strongToStrongObjectsMapTable];
        _haptic = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    }
    return self;
}
- (CGFloat)opacityPref {
    return pref_float(@"napple.padOpacity", 0.45f);
}
- (void)layoutSubviews {
    [super layoutSubviews];
    UIEdgeInsets in = self.safeAreaInsets;
    CGSize s = self.bounds.size;
    for (int i = 0; i < _n; i++) {
        [_shape[i] removeFromSuperlayer];
        [_text[i] removeFromSuperlayer];
        _shape[i] = nil;
        _text[i] = nil;
    }
    for (int k = 0; k < 2; k++) {
        [_knob[k] removeFromSuperlayer];
        _knob[k] = nil;
    }
    _n = pad_layout(_ctl, PAD_MAX_CTLS, (float)s.width, (float)s.height, (float)in.left, (float)in.right, (float)in.top, (float)in.bottom);
    UIColor *line = [UIColor colorWithWhite:1 alpha:0.9];
    for (int i = 0; i < _n; i++) {
        const pad_ctl *c = &_ctl[i];
        CAShapeLayer *sh = [CAShapeLayer layer];
        CGRect r = c->pill ? CGRectMake(c->cx - c->r * 1.25, c->cy - c->r, c->r * 2.5, c->r * 2) : CGRectMake(c->cx - c->r, c->cy - c->r, c->r * 2, c->r * 2);
        sh.path = [UIBezierPath bezierPathWithRoundedRect:r cornerRadius:c->pill ? c->r : r.size.width / 2].CGPath;
        sh.fillColor = [UIColor colorWithWhite:0.1 alpha:0.35].CGColor;
        sh.strokeColor = line.CGColor;
        sh.lineWidth = 2;
        sh.opacity = (float)self.opacityPref;
        [self.layer addSublayer:sh];
        _shape[i] = sh;
        if (c->kind == PAD_BUTTON) {
            CATextLayer *t = [CATextLayer layer];
            t.string = @(c->label);
            t.fontSize = c->r * 0.9;
            t.alignmentMode = kCAAlignmentCenter;
            t.contentsScale = UIScreen.mainScreen.scale;
            t.foregroundColor = line.CGColor;
            t.frame = CGRectMake(r.origin.x, c->cy - c->r * 0.55, r.size.width, c->r * 1.2);
            t.opacity = (float)self.opacityPref;
            [self.layer addSublayer:t];
            _text[i] = t;
        } else {
            CAShapeLayer *k = [CAShapeLayer layer];
            CGFloat kr = c->r * 0.45;
            k.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(-kr, -kr, kr * 2, kr * 2)].CGPath;
            k.fillColor = [UIColor colorWithWhite:1 alpha:0.5].CGColor;
            k.position = CGPointMake(c->cx, c->cy);
            k.opacity = (float)self.opacityPref;
            [self.layer addSublayer:k];
            _knob[c->stick] = k;
        }
    }
    [self releaseAll];
}
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)e {
    if (self.hidden || self.alpha == 0) return nil;
    return pad_hit(_ctl, _n, (float)p.x, (float)p.y) >= 0 ? self : nil; /* everything else falls through to the game view (touch screen) */
}
- (void)push {
    uint64_t b = 0;
    for (int i = 0; i < _n; i++)
        if (_ctl[i].kind == PAD_BUTTON && _down[i]) b |= NAPPLE_BTN_MASK(_ctl[i].button);
    if (self.fe.input) napple_input_virtual_pad(self.fe.input, b, _sx[0], _sy[0], _sx[1], _sy[1]);
    for (int i = 0; i < _n; i++) {
        if (_ctl[i].kind == PAD_BUTTON) _shape[i].fillColor = [UIColor colorWithWhite:_down[i] ? 0.9 : 0.1 alpha:_down[i] ? 0.5 : 0.35].CGColor;
        else {
            int k = _ctl[i].stick;
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            _knob[k].position = CGPointMake(_ctl[i].cx + _sx[k] * _ctl[i].r * 0.55, _ctl[i].cy - _sy[k] * _ctl[i].r * 0.55);
            [CATransaction commit];
        }
    }
}
- (void)releaseAll {
    memset(_down, 0, sizeof _down);
    memset(_sx, 0, sizeof _sx);
    memset(_sy, 0, sizeof _sy);
    [_owner removeAllObjects];
    [self push];
}
- (void)apply:(UITouch *)t index:(int)i {
    CGPoint p = [t locationInView:self];
    if (_ctl[i].kind == PAD_STICK) pad_stick_vec(&_ctl[i], (float)p.x, (float)p.y, &_sx[_ctl[i].stick], &_sy[_ctl[i].stick]);
    else _down[i] = YES;
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    for (UITouch *t in ts) {
        CGPoint p = [t locationInView:self];
        int i = pad_hit(_ctl, _n, (float)p.x, (float)p.y);
        if (getenv("NAPPLE_INPUT_LOG"))
            fprintf(stderr, "[napple] pad touch (%.0f,%.0f) in %.0fx%.0f -> control %d\n", p.x, p.y, self.bounds.size.width, self.bounds.size.height, i);
        if (i < 0) continue;
        if (_ctl[i].kind == PAD_BUTTON && pref_bool(@"napple.padHaptics", YES)) [_haptic impactOccurred];
        [_owner setObject:@(i) forKey:t];
        [self apply:t index:i];
    }
    [self push];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    for (UITouch *t in ts) {
        NSNumber *n = [_owner objectForKey:t];
        if (n && _ctl[n.intValue].kind == PAD_STICK) [self apply:t index:n.intValue];
    }
    [self push];
}
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    for (UITouch *t in ts) {
        NSNumber *n = [_owner objectForKey:t];
        if (!n) continue;
        int i = n.intValue;
        if (_ctl[i].kind == PAD_STICK) _sx[_ctl[i].stick] = _sy[_ctl[i].stick] = 0;
        else _down[i] = NO;
        [_owner removeObjectForKey:t];
    }
    [self push];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    [self touchesEnded:ts withEvent:e];
}
@end

/* ------------------------------------------------------------------------------------------------------------------ */
#pragma mark - game view (touch screen)

@interface GameView : UIView
@property (nonatomic, weak) NappleFEiOS *fe;
@end
@implementation GameView {
    __weak UITouch *_primary;
}
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        self.backgroundColor = UIColor.blackColor;
        self.multipleTouchEnabled = NO;
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    NappleFEiOS *fe = self.fe;
    if (!fe) return;
    CGFloat s = self.window.screen.scale ?: UIScreen.mainScreen.scale;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    fe.layer.frame = self.bounds;
    fe.layer.contentsScale = s;
    fe.layer.drawableSize = CGSizeMake(MAX(1, self.bounds.size.width * s), MAX(1, self.bounds.size.height * s));
    [CATransaction commit];
    UIScreen *sc = self.window.windowScene.screen;
    if (sc) fe.displayHz = (double)sc.maximumFramesPerSecond;
    if (fe.cfg.callbacks.on_resize) fe.cfg.callbacks.on_resize(fe.cfg.callbacks.user, (int)fe.layer.drawableSize.width, (int)fe.layer.drawableSize.height, s);
}
- (void)forward:(UITouch *)t down:(int)d {
    NappleFEiOS *fe = self.fe;
    if (!fe.input) return;
    CGPoint p = [t locationInView:self];
    napple_input_mouse_position(fe.input, (float)(p.x / MAX(1, self.bounds.size.width)), (float)(p.y / MAX(1, self.bounds.size.height)));
    if (d >= 0) napple_input_mouse_button(fe.input, 0, d);
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    if (_primary) return;
    _primary = ts.anyObject;
    [self forward:_primary down:1];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    if ([ts containsObject:_primary]) [self forward:_primary down:-1];
}
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    if ([ts containsObject:_primary]) {
        [self forward:_primary down:0];
        _primary = nil;
    }
}
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    [self touchesEnded:ts withEvent:e];
}
@end

/* ------------------------------------------------------------------------------------------------------------------ */
#pragma mark - root view controller

@interface NappleVC : UIViewController
@property (nonatomic, weak) NappleFEiOS *fe;
@property (nonatomic, strong) GameView *game;
@property (nonatomic, strong) PadView *pad;
@property (nonatomic, strong) UIButton *gear;
@property (nonatomic, strong) UIView *bootView, *notice;
@property (nonatomic, strong) UILabel *bootStage, *noticeText, *stats, *toastLabel;
@property (nonatomic, strong) UIProgressView *bootBar;
@property (nonatomic, strong) UIActivityIndicatorView *bootSpin, *noticeSpin;
@property (nonatomic) NSUInteger toastSerial;
- (void)refreshPadVisibility;
@end
@implementation NappleVC
- (BOOL)prefersStatusBarHidden {
    return YES;
}
- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
    return UIRectEdgeAll;
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}

static UILabel *makeLabel(CGFloat size, UIFontWeight w, BOOL mono) {
    UILabel *l = [UILabel new];
    l.textColor = UIColor.whiteColor;
    l.font = mono ? [UIFont monospacedSystemFontOfSize:size weight:w] : [UIFont systemFontOfSize:size weight:w];
    l.translatesAutoresizingMaskIntoConstraints = NO;
    return l;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    NappleFEiOS *fe = self.fe;
    self.view.backgroundColor = UIColor.blackColor;
    self.game = [[GameView alloc] initWithFrame:self.view.bounds];
    self.game.fe = fe;
    self.game.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.game.layer addSublayer:fe.layer];
    [self.view addSubview:self.game];
    self.pad = [[PadView alloc] initWithFrame:self.view.bounds];
    self.pad.fe = fe;
    self.pad.autoresizingMask = self.game.autoresizingMask;
    [self.view addSubview:self.pad];
    self.gear = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.gear setImage:[UIImage systemImageNamed:@"gearshape.fill"] forState:UIControlStateNormal];
    self.gear.tintColor = [UIColor colorWithWhite:1 alpha:0.55];
    self.gear.translatesAutoresizingMaskIntoConstraints = NO;
    [self.gear addTarget:fe action:@selector(toggleSettings) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:self.gear];
    UILayoutGuide *g = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [self.gear.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor], [self.gear.topAnchor constraintEqualToAnchor:g.topAnchor constant:6],
        [self.gear.widthAnchor constraintEqualToConstant:44], [self.gear.heightAnchor constraintEqualToConstant:44]
    ]];
    /* stats, toast */
    self.stats = makeLabel(11, UIFontWeightMedium, YES);
    self.stats.backgroundColor = [UIColor colorWithWhite:0 alpha:0.6];
    self.stats.hidden = YES;
    self.stats.userInteractionEnabled = NO;
    self.stats.translatesAutoresizingMaskIntoConstraints = YES; /* placed by refreshStats (corner setting) */
    [self.view addSubview:self.stats];
    self.toastLabel = makeLabel(15, UIFontWeightSemibold, NO);
    self.toastLabel.backgroundColor = [UIColor colorWithWhite:0 alpha:0.55];
    self.toastLabel.hidden = YES;
    self.toastLabel.layer.cornerRadius = 6;
    self.toastLabel.layer.masksToBounds = YES;
    [self.view addSubview:self.toastLabel];
    [NSLayoutConstraint activateConstraints:@[
        [self.toastLabel.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:14],
        [self.toastLabel.topAnchor constraintEqualToAnchor:g.topAnchor constant:14]
    ]];
    /* compile notice */
    self.notice = [UIView new];
    self.notice.translatesAutoresizingMaskIntoConstraints = NO;
    self.notice.backgroundColor = [UIColor colorWithWhite:0 alpha:0.6];
    self.notice.layer.cornerRadius = 8;
    self.notice.hidden = YES;
    self.notice.userInteractionEnabled = NO;
    self.noticeSpin = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    self.noticeSpin.color = UIColor.whiteColor;
    self.noticeSpin.translatesAutoresizingMaskIntoConstraints = NO;
    self.noticeText = makeLabel(12, UIFontWeightMedium, YES);
    UIStackView *ns = [[UIStackView alloc] initWithArrangedSubviews:@[ self.noticeSpin, self.noticeText ]];
    ns.spacing = 8;
    ns.translatesAutoresizingMaskIntoConstraints = NO;
    ns.layoutMarginsRelativeArrangement = YES;
    ns.layoutMargins = UIEdgeInsetsMake(6, 10, 6, 12);
    [self.notice addSubview:ns];
    [self.view addSubview:self.notice];
    [NSLayoutConstraint activateConstraints:@[
        [ns.leadingAnchor constraintEqualToAnchor:self.notice.leadingAnchor], [ns.trailingAnchor constraintEqualToAnchor:self.notice.trailingAnchor],
        [ns.topAnchor constraintEqualToAnchor:self.notice.topAnchor], [ns.bottomAnchor constraintEqualToAnchor:self.notice.bottomAnchor],
        [self.notice.topAnchor constraintEqualToAnchor:g.topAnchor constant:12],
        [self.notice.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-12]
    ]];
    /* boot view */
    self.bootView = [[UIView alloc] initWithFrame:self.view.bounds];
    self.bootView.backgroundColor = [UIColor colorWithWhite:0.08 alpha:1];
    self.bootView.autoresizingMask = self.game.autoresizingMask;
    UILabel *title = makeLabel(24, UIFontWeightBold, NO);
    title.text = fe.titleName;
    title.textAlignment = NSTextAlignmentCenter;
    title.numberOfLines = 2;
    self.bootStage = makeLabel(13, UIFontWeightRegular, NO);
    self.bootStage.text = @"Starting";
    self.bootStage.textAlignment = NSTextAlignmentCenter;
    self.bootStage.numberOfLines = 4;
    self.bootStage.textColor = [UIColor colorWithWhite:1 alpha:0.75];
    self.bootBar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    self.bootBar.translatesAutoresizingMaskIntoConstraints = NO;
    self.bootBar.hidden = YES;
    self.bootSpin = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    self.bootSpin.color = UIColor.whiteColor;
    [self.bootSpin startAnimating];
    UIStackView *st = [[UIStackView alloc] initWithArrangedSubviews:@[ title, self.bootStage, self.bootSpin, self.bootBar ]];
    st.axis = UILayoutConstraintAxisVertical;
    st.spacing = 12;
    st.alignment = UIStackViewAlignmentCenter;
    st.translatesAutoresizingMaskIntoConstraints = NO;
    [self.bootView addSubview:st];
    [self.view addSubview:self.bootView];
    [NSLayoutConstraint activateConstraints:@[
        [st.centerXAnchor constraintEqualToAnchor:self.bootView.centerXAnchor], [st.centerYAnchor constraintEqualToAnchor:self.bootView.centerYAnchor],
        [st.widthAnchor constraintLessThanOrEqualToConstant:460], [self.bootBar.widthAnchor constraintEqualToConstant:280]
    ]];
    [self refreshPadVisibility];
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    for (NSNotificationName n in @[ GCControllerDidConnectNotification, GCControllerDidDisconnectNotification, @"napple.padChanged" ])
        [nc addObserver:self selector:@selector(refreshPadVisibility) name:n object:nil];
    [NSTimer scheduledTimerWithTimeInterval:0.25
                                    repeats:YES
                                      block:^(NSTimer *t) {
                                          (void)t;
                                          [self refreshStats];
                                      }];
}
- (void)refreshPadVisibility {
#if TARGET_OS_SIMULATOR
    BOOL physical = NO; /* the iOS 26 Simulator always reports a phantom "Gamepad (MFi)": it must not hide the touch controller */
#else
    BOOL physical = GCController.controllers.count > 0;
#endif
    BOOL pad = pref_bool(@"napple.padVisible", YES) && !physical;
    self.pad.hidden = !pad;
    if (!pad) [self.pad releaseAll];
    for (CALayer *l in self.pad.layer.sublayers) l.opacity = (float)pref_float(@"napple.padOpacity", 0.45f);
    self.gear.hidden = NO;
}
- (void)refreshStats {
    NappleFEiOS *fe = self.fe;
    napple_settings s = [fe currentSettings];
    if (!s.show_stats || !fe.cfg.callbacks.stats_text) {
        self.stats.hidden = YES;
        return;
    }
    char buf[400] = "";
    fe.cfg.callbacks.stats_text(fe.cfg.callbacks.user, buf, sizeof buf);
    static const CGFloat sizes[3] = {10, 12, 15};
    self.stats.font = [UIFont monospacedSystemFontOfSize:sizes[s.stats_size] weight:UIFontWeightMedium];
    self.stats.backgroundColor = [UIColor colorWithWhite:0 alpha:s.stats_opacity / 100.0];
    self.stats.text = [NSString stringWithFormat:@" %s ", buf];
    [self.stats sizeToFit];
    UIEdgeInsets in = self.view.safeAreaInsets;
    CGSize vs = self.view.bounds.size, ss = self.stats.bounds.size;
    CGFloat x = (s.stats_corner & 1) ? vs.width - in.right - ss.width - 8 : in.left + 8;
    CGFloat y = (s.stats_corner & 2) ? vs.height - in.bottom - ss.height - 8 : in.top + 8;
    self.stats.frame = CGRectMake(x, y, ss.width, ss.height);
    self.stats.hidden = NO;
}
- (void)toast:(NSString *)text {
    self.toastLabel.text = [NSString stringWithFormat:@"  %@  ", text];
    self.toastLabel.hidden = NO;
    NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.6 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (serial == self->_toastSerial) self.toastLabel.hidden = YES;
    });
}
@end

/* ------------------------------------------------------------------------------------------------------------------ */
#pragma mark - NappleFEiOS

@implementation NappleFEiOS {
    pthread_mutex_t _lock;
    _Atomic uint64_t _frames;
    BOOL _turboHeld, _turboToggle, _slowToggle;
    NappleIOSPanel *_panel;
    BOOL _panelPadMode;
    CADisplayLink *_link;
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
- (NappleVC *)vc {
    return (NappleVC *)self.rootVC;
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
    _settings = s;
    pthread_mutex_unlock(&_lock);
    if (save) [self saveSettings];
    napple_audio_set_master_volume(s.master_volume);
    self.layer.developerHUDProperties = s.metal_hud ? @{@"mode" : @"default"} : nil; /* Apple's native Metal performance HUD */
    [self refreshPanelInputMode];
    if (notify && self.cfg.callbacks.on_settings_changed) self.cfg.callbacks.on_settings_changed(self.cfg.callbacks.user, &s);
    if (notify) [self updateSpeed];
}
- (void)mutate:(void (^)(napple_settings *))m {
    napple_settings s = [self currentSettings];
    m(&s);
    [self applySettings:s save:YES notify:YES];
}
- (void)updateSpeed {
    napple_settings s = [self currentSettings];
    int eff = napple_effective_speed_percent(&s, _turboHeld, _turboToggle, _slowToggle);
    if (self.cfg.callbacks.on_speed) self.cfg.callbacks.on_speed(self.cfg.callbacks.user, eff);
}
- (void)toast:(NSString *)text {
    [self.vc toast:text];
}

/* scene life cycle ---------------------------------------------------------------------------------------------- */
- (void)sceneConnected:(UIWindowScene *)scene {
    UIWindow *w = [[UIWindow alloc] initWithWindowScene:scene];
    NappleVC *vc = [NappleVC new];
    vc.fe = self;
    self.rootVC = vc;
    w.rootViewController = vc;
    [w makeKeyAndVisible];
    objc_setAssociatedObject(scene, "napple.window", w, OBJC_ASSOCIATION_RETAIN);
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    g_vsync = dispatch_semaphore_create(0);
    _link = [scene.screen displayLinkWithTarget:self selector:@selector(tick:)];
    _link.preferredFrameRateRange = CAFrameRateRangeMake(30, 120, 120);
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    self.displayHz = (double)scene.screen.maximumFramesPerSecond;
    napple_audio_set_master_volume(_settings.master_volume);
    if (self.input) napple_input_set_hotkey_callback(self.input, fe_hotkey, (__bridge void *)self);
    [self attachKeyboard];
    if (getenv("NAPPLE_FE_OVERLAY")) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 6 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [self showSettings:YES]; });
    dispatch_async(dispatch_get_main_queue(), ^{
        [vc.view layoutIfNeeded];
        if (self.cfg.callbacks.on_ready && !self.readyFired) {
            self.readyFired = YES;
            self.cfg.callbacks.on_ready(self.cfg.callbacks.user);
        }
    });
}
- (void)tick:(CADisplayLink *)l {
    (void)l;
    if (g_vsync) dispatch_semaphore_signal(g_vsync);
}
- (void)focus:(BOOL)on {
    if (self.input) napple_input_focus(self.input, on);
    if (!on) {
        _turboHeld = NO;
        [self updateSpeed];
        [(NappleVC *)self.rootVC refreshPadVisibility];
    }
    napple_audio_set_paused(!on); /* the process is suspended in the background; keep the audio engine in step */
    if (self.cfg.callbacks.on_focus) self.cfg.callbacks.on_focus(self.cfg.callbacks.user, on);
}

/* hardware keyboard (iPad) -------------------------------------------------------------------------------------------- */
- (void)attachKeyboard {
    __weak NappleFEiOS *me = self;
    void (^hook)(GCKeyboard *) = ^(GCKeyboard *kb) {
        kb.keyboardInput.keyChangedHandler = ^(GCKeyboardInput *input, GCControllerButtonInput *key, GCKeyCode code, BOOL pressed) {
            (void)input;
            (void)key;
            NappleFEiOS *s = me;
            if (!s) return;
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([s hardwareKey:(int)code down:pressed]) return;
                int mac = ios_hid_to_mac_keycode((int)code);
                if (mac >= 0 && s.input) napple_input_key(s.input, (uint16_t)mac, pressed ? 1 : 0);
            });
        };
    };
    if (GCKeyboard.coalescedKeyboard) hook(GCKeyboard.coalescedKeyboard);
    [NSNotificationCenter.defaultCenter addObserverForName:GCKeyboardDidConnectNotification
                                                    object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification *n) { hook(n.object); }];
}
/* F1 settings, F2 turbo (hold), F3 turbo toggle, F4 slow motion, F5/F6 speed -/+, F7 reset, F9 statistics, F12 screenshot; YES = handled. */
- (BOOL)hardwareKey:(int)u down:(BOOL)down {
    if (u < 58 || u > 69) return NO; /* F1..F12 usages */
    int f = u - 57;
    if (f == 2) {
        _turboHeld = down;
        [self updateSpeed];
        [self toast:down ? @"Turbo" : @"Turbo off"];
        return YES;
    }
    if (!down) return YES;
    napple_settings s = [self currentSettings];
    switch (f) {
        case 1: [self toggleSettings]; break;
        case 3:
            _turboToggle = !_turboToggle;
            [self updateSpeed];
            [self toast:_turboToggle ? @"Turbo on" : @"Turbo off"];
            break;
        case 4:
            _slowToggle = !_slowToggle;
            [self updateSpeed];
            [self toast:_slowToggle ? @"Slow motion 0.5×" : @"Slow motion off"];
            break;
        case 5:
        case 6: {
            int n = sizeof kSpeedStepsIOS / sizeof *kSpeedStepsIOS, cur = s.speed_percent, to = cur;
            if (f == 6) {
                for (int i = 0; i < n; i++)
                    if (kSpeedStepsIOS[i] > cur) {
                        to = kSpeedStepsIOS[i];
                        break;
                    }
            } else {
                for (int i = n - 1; i >= 0; i--)
                    if (kSpeedStepsIOS[i] < cur) {
                        to = kSpeedStepsIOS[i];
                        break;
                    }
            }
            _turboToggle = _slowToggle = NO;
            [self mutate:^(napple_settings *t) { t->speed_percent = to; }];
            [self toast:[NSString stringWithFormat:@"Game speed %.2g×", to / 100.0]];
            break;
        }
        case 7:
            _turboToggle = _slowToggle = NO;
            [self mutate:^(napple_settings *t) { t->speed_percent = 100; }];
            [self toast:@"Game speed 1×"];
            break;
        case 9: [self mutate:^(napple_settings *t) { t->show_stats = !t->show_stats; }]; break;
        case 12: [self screenshot]; break;
        default: return NO;
    }
    return YES;
}

/* settings sheet ---------------------------------------------------------------------------------------------------------- */
- (void)toggleSettings {
    [self toggleSettingsFromPad:NO];
}
- (void)toggleSettingsFromPad:(BOOL)pad {
    if (!self.overlayVisible) _panelPadMode = pad;
    [self showSettings:!self.overlayVisible];
}
/* The game keeps its touch / keyboard input while the panel is open, unless a controller opened it (the pad then navigates the panel). */
- (void)refreshPanelInputMode {
    if (self.input) napple_input_set_blocked(self.input, self.overlayVisible && (_panelPadMode || !self.settings.panel_passthrough));
}
- (void)showSettings:(BOOL)show {
    if (show == self.overlayVisible || !self.rootVC) return;
    self.overlayVisible = show;
    UIView *host = self.rootVC.view;
    UIImpactFeedbackGenerator *h = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleMedium];
    [h impactOccurred];
    if (show) {
        if (!_panel) _panel = [[NappleIOSPanel alloc] initWithFE:self];
        _panel.padMode = _panelPadMode;
        if (getenv("NAPPLE_FE_OVERLAY")) [_panel selectTab:atoi(getenv("NAPPLE_FE_OVERLAY"))]; /* test aid: open on a tab (SIMCTL_CHILD_NAPPLE_FE_OVERLAY) */
        [_panel reload];
        [host addSubview:_panel];
        UILayoutGuide *g = host.safeAreaLayoutGuide;
        NSLayoutConstraint *w = [_panel.widthAnchor constraintEqualToAnchor:host.widthAnchor multiplier:0.56];
        w.priority = UILayoutPriorityDefaultHigh;
        [NSLayoutConstraint activateConstraints:@[
            [_panel.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-12], [_panel.topAnchor constraintEqualToAnchor:g.topAnchor constant:12],
            [_panel.bottomAnchor constraintEqualToAnchor:g.bottomAnchor constant:-12], [_panel.widthAnchor constraintLessThanOrEqualToConstant:440],
            [_panel.widthAnchor constraintGreaterThanOrEqualToConstant:300], w
        ]];
        [host layoutIfNeeded];
        _panel.transform = CGAffineTransformMakeTranslation(_panel.bounds.size.width + 40, 0);
        _panel.alpha = 0;
        [UIView animateWithDuration:0.42 delay:0 usingSpringWithDamping:0.85 initialSpringVelocity:0.4 options:0 animations:^{
            self->_panel.transform = CGAffineTransformIdentity;
            self->_panel.alpha = 1;
        } completion:nil];
        [self.vc.pad releaseAll];
    } else {
        NappleIOSPanel *p = _panel;
        [UIView animateWithDuration:0.25 animations:^{
            p.transform = CGAffineTransformMakeTranslation(p.bounds.size.width + 40, 0);
            p.alpha = 0;
        } completion:^(BOOL f) {
            if (!self.overlayVisible) [p removeFromSuperview];
        }];
        _panelPadMode = NO;
        p.padMode = NO;
    }
    [self refreshPanelInputMode];
    if (self.cfg.callbacks.on_overlay) self.cfg.callbacks.on_overlay(self.cfg.callbacks.user, show);
}
static void fe_hotkey(void *user, int hotkey) {
    NappleFEiOS *fe = (__bridge NappleFEiOS *)user;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (hotkey == NAPPLE_HOTKEY_HOME) [fe toggleSettingsFromPad:YES];
        else if (hotkey == NAPPLE_HOTKEY_CAPTURE) [fe screenshot];
    });
}

- (void)screenshot {
    NSDateFormatter *df = [NSDateFormatter new];
    df.dateFormat = @"yyyyMMdd-HHmmss";
    NSString *dir = self.shotDir
                        ?: [[NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject
                               stringByAppendingPathComponent:@"Napple"] stringByAppendingPathComponent:@"Screenshots"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *safe = [[self.titleName componentsSeparatedByCharactersInSet:NSCharacterSet.alphanumericCharacterSet.invertedSet] componentsJoinedByString:@"_"];
    NSString *path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@.png", safe, [df stringFromDate:NSDate.date]]];
    if (self.cfg.callbacks.on_screenshot && self.cfg.callbacks.on_screenshot(self.cfg.callbacks.user, path.UTF8String) == 0) {
        LOGF("screenshot %s", path.UTF8String);
        [self toast:@"Screenshot saved to Files › Napple › Screenshots"];
        return;
    }
    [self toast:@"Screenshot failed"];
}
- (void)restartGuest {
    napple_settings s = [self currentSettings];
    LOGF("restart guest requested (update %s, %d DLC disabled)", s.update_version[0] ? s.update_version : "latest", s.disabled_dlc_count);
    if (self.cfg.callbacks.on_restart_guest) self.cfg.callbacks.on_restart_guest(self.cfg.callbacks.user, &s);
}
- (void)quit {
    if (self.quitting) return;
    self.quitting = YES;
    if (self.cfg.callbacks.on_quit) self.cfg.callbacks.on_quit(self.cfg.callbacks.user); /* writes the summary and _exit()s */
}

/* boot / notices ------------------------------------------------------------------------------------------------------------ */
- (void)boot:(NSString *)stage fraction:(double)f {
    dispatch_async(dispatch_get_main_queue(), ^{
        NappleVC *v = self.vc;
        v.bootStage.text = stage ?: @"Starting";
        if (f < 0) {
            v.bootBar.hidden = YES;
            [v.bootSpin startAnimating];
        } else {
            v.bootBar.hidden = NO;
            [v.bootSpin stopAnimating];
            v.bootBar.progress = (float)(f > 1 ? 1 : f);
        }
    });
}
- (void)bootDone {
    dispatch_async(dispatch_get_main_queue(), ^{ self.vc.bootView.hidden = YES; });
}
- (void)compileNotice:(NSString *)text {
    dispatch_async(dispatch_get_main_queue(), ^{
        NappleVC *v = self.vc;
        if (!text) {
            v.notice.hidden = YES;
            [v.noticeSpin stopAnimating];
            return;
        }
        v.noticeText.text = text;
        v.notice.hidden = NO;
        [v.noticeSpin startAnimating];
    });
}
- (void)framePresented {
    uint64_t n = atomic_fetch_add(&_frames, 1);
    if (n == 0) [self bootDone];
}
@end

/* ------------------------------------------------------------------------------------------------------------------ */
#pragma mark - UIKit delegates (named in Info.plist and below)

@interface NappleSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end
@implementation NappleSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)opts {
    (void)session;
    (void)opts;
    if ([scene isKindOfClass:UIWindowScene.class] && g_fe) [g_fe sceneConnected:(UIWindowScene *)scene];
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    [g_fe focus:YES];
}
- (void)sceneWillResignActive:(UIScene *)scene {
    (void)scene;
    [g_fe focus:NO];
}
@end

@interface NappleAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation NappleAppDelegate
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
    (void)a;
    (void)o;
    return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)a configurationForConnectingSceneSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    (void)a;
    (void)o;
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:s.role];
    c.delegateClass = NappleSceneDelegate.class;
    return c;
}
- (void)applicationDidReceiveMemoryWarning:(UIApplication *)a {
    (void)a;
    LOGF("iOS memory warning (the game may be terminated; see docs/architecture/ios.md: increased-memory-limit entitlement)");
}
@end

/* ------------------------------------------------------------------------------------------------------------------ */
#pragma mark - public C API

static NappleFEiOS *FE(napple_frontend *f) { return (__bridge NappleFEiOS *)f->impl; }

napple_frontend *napple_frontend_create(const napple_frontend_config *c) {
    if (!c || ![NSThread isMainThread]) return NULL;
    @autoreleasepool {
        NappleFEiOS *fe = [NappleFEiOS new];
        g_fe = fe;
        fe.cfg = *c;
        fe.input = c->input;
        fe.titleID = c->title_id ? @(c->title_id) : @"default";
        fe.titleName = c->title_name ? @(c->title_name) : @"Napple";
        fe.dir = c->config_dir ? @(c->config_dir) : nil;
        fe.shotDir = c->screenshot_dir ? @(c->screenshot_dir) : nil;
        [fe loadSettings];
        CAMetalLayer *l = [CAMetalLayer layer];
        l.pixelFormat = MTLPixelFormatBGRA8Unorm;
        l.framebufferOnly = YES;
        l.opaque = YES;
        l.backgroundColor = UIColor.blackColor.CGColor;
        fe.layer = l;
        fe.displayHz = 60;
        napple_frontend *f = calloc(1, sizeof *f);
        f->impl = (void *)CFBridgingRetain(fe);
        return f;
    }
}
void napple_frontend_destroy(napple_frontend *f) {
    if (!f) return;
    g_fe = nil;
    CFBridgingRelease(f->impl);
    free(f);
}
int napple_frontend_run(napple_frontend *f) {
    (void)f;
    @autoreleasepool {
        return UIApplicationMain(0, NULL, nil, NSStringFromClass(NappleAppDelegate.class));
    }
}
void napple_frontend_request_quit(napple_frontend *f) {
    NappleFEiOS *fe = FE(f);
    dispatch_async(dispatch_get_main_queue(), ^{ [fe quit]; });
}
void *napple_frontend_metal_layer(napple_frontend *f) { return (__bridge void *)FE(f).layer; }
double napple_frontend_display_hz(napple_frontend *f) {
    double h = FE(f).displayHz;
    return h >= 24 ? h : 60;
}
void napple_frontend_drawable_size(napple_frontend *f, int *w, int *h, double *scale) {
    NappleFEiOS *fe = FE(f);
    CGSize d = fe.layer.drawableSize;
    if (w) *w = (int)d.width;
    if (h) *h = (int)d.height;
    if (scale) *scale = fe.layer.contentsScale;
}
void napple_frontend_get_settings(napple_frontend *f, napple_settings *out) { *out = [FE(f) currentSettings]; }
int napple_frontend_set_settings(napple_frontend *f, const napple_settings *s) {
    if (!s) return -1;
    NappleFEiOS *fe = FE(f);
    napple_settings c = *s;
    if ([NSThread isMainThread]) [fe applySettings:c save:YES notify:YES];
    else dispatch_async(dispatch_get_main_queue(), ^{ [fe applySettings:c save:YES notify:YES]; });
    return 0;
}
void napple_frontend_set_manifest(napple_frontend *f, const napple_manifest *m) {
    NappleFEiOS *fe = FE(f);
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
    });
}
void napple_frontend_show_overlay(napple_frontend *f, int v) {
    NappleFEiOS *fe = FE(f);
    dispatch_async(dispatch_get_main_queue(), ^{ [fe showSettings:v != 0]; });
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

/* ---- swkbd: an alert with a text field; OK is enabled while the text satisfies the request ---- */
void napple_frontend_text_input(napple_frontend *f, const napple_text_request *req, napple_text_done_fn done, void *user) {
    NappleFEiOS *fe = FE(f);
    const char *ans = getenv("NAPPLE_SWKBD_ANSWER"); /* tests / automation */
    if (ans) {
        if (!strcmp(ans, "<cancel>")) done(user, 0, NULL);
        else if (!strcmp(ans, "<initial>")) done(user, req->initial != NULL, req->initial ? req->initial : "");
        else if (napple_text_validate(req, ans)) done(user, 1, ans);
        else done(user, 0, NULL);
        return;
    }
    napple_text_request q = *req;
    q.header = q.sub = q.guide = q.initial = NULL; /* the strings are copied into the alert below */
    NSString *header = req->header && *req->header ? @(req->header) : @"Enter text", *sub = req->sub ? @(req->sub) : nil,
             *guide = req->guide ? @(req->guide) : nil, *initial = req->initial ? @(req->initial) : @"";
    BOOL password = req->password != 0, numeric = req->numeric != 0;
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *top = fe.rootVC;
        while (top.presentedViewController) top = top.presentedViewController;
        if (!top) {
            done(user, 0, NULL);
            return;
        }
        UIAlertController *a = [UIAlertController alertControllerWithTitle:header message:sub preferredStyle:UIAlertControllerStyleAlert];
        __block BOOL finished = NO;
        void (^finish)(BOOL, NSString *) = ^(BOOL ok, NSString *text) {
            if (finished) return;
            finished = YES;
            [fe refreshPanelInputMode];
            NSString *copy = [text copy];
            done(user, ok, ok ? copy.UTF8String : NULL);
        };
        UIAlertAction *ok = [UIAlertAction actionWithTitle:@"OK"
                                                     style:UIAlertActionStyleDefault
                                                   handler:^(UIAlertAction *x) { finish(YES, a.textFields.firstObject.text ?: @""); }];
        [a addTextFieldWithConfigurationHandler:^(UITextField *tf) {
            tf.text = initial;
            tf.placeholder = guide;
            tf.secureTextEntry = password;
            tf.keyboardType = numeric ? UIKeyboardTypeNumberPad : UIKeyboardTypeDefault;
            ok.enabled = napple_text_validate(&q, initial.UTF8String);
            [tf addAction:[UIAction actionWithHandler:^(UIAction *x) { ok.enabled = napple_text_validate(&q, tf.text.UTF8String); }]
                forControlEvents:UIControlEventEditingChanged];
        }];
        [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { finish(NO, nil); }]];
        [a addAction:ok];
        if (fe.input) napple_input_set_blocked(fe.input, YES);
        [top presentViewController:a animated:YES completion:nil];
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
