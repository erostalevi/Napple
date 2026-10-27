/* panel_ios.m - the iOS settings panel. Same four tabs as the Mac overlay (Audio/Video, FPS/Speed, Controls, Game/System), built for touch:
 *   - a glass panel pinned to the trailing edge (full height, margins, safe-area aware); the game and the on-screen controller stay usable beside it
 *   - big rows (52 pt), native menus for choices, sliders with live values, haptic ticks, swipe right to dismiss
 *   - controller navigation when it was opened with a physical pad's Home button: D-pad moves / adjusts, A activates, B closes, LB / RB switch tabs
 * Settings go through the shared settings.c (fe mutate:), controller profiles through napple_input_*_player_bindings. */
#import "fe_ios.h"
#include <string.h>
#include "sim_shared.h"

typedef NS_ENUM(int, RowKind) { RowToggle, RowSlider, RowMenu, RowAction, RowInfo, RowHeader, RowSearch };
@interface Row : NSObject
@property (nonatomic) RowKind kind;
@property (nonatomic, copy) NSString *title, *detail;
@property (nonatomic, copy) BOOL (^getBool)(void);
@property (nonatomic, copy) void (^setBool)(BOOL);
@property (nonatomic, copy) float (^getFloat)(void);
@property (nonatomic, copy) void (^setFloat)(float);
@property (nonatomic) float min, max;
@property (nonatomic, copy) NSString *fmt;
@property (nonatomic, copy) NSArray<NSString *> *options;
@property (nonatomic, copy) NSInteger (^getIndex)(void);
@property (nonatomic, copy) void (^setIndex)(NSInteger);
@property (nonatomic, copy) void (^action)(void);
@property (nonatomic) BOOL destructive;
@end
@implementation Row
- (BOOL)interactive {
    return self.kind != RowInfo && self.kind != RowHeader && self.kind != RowSearch;
}
@end

static Row *mk(RowKind k, NSString *t) {
    Row *r = [Row new];
    r.kind = k;
    r.title = t;
    return r;
}
static Row *toggle(NSString *t, BOOL (^g)(void), void (^s)(BOOL)) {
    Row *r = mk(RowToggle, t);
    r.getBool = g;
    r.setBool = s;
    return r;
}
static Row *slider(NSString *t, float lo, float hi, NSString *fmt, float (^g)(void), void (^s)(float)) {
    Row *r = mk(RowSlider, t);
    r.min = lo;
    r.max = hi;
    r.fmt = fmt;
    r.getFloat = g;
    r.setFloat = s;
    return r;
}
static Row *menu(NSString *t, NSArray<NSString *> *o, NSInteger (^g)(void), void (^s)(NSInteger)) {
    Row *r = mk(RowMenu, t);
    r.options = o;
    r.getIndex = g;
    r.setIndex = s;
    return r;
}
static Row *action(NSString *t, BOOL destructive, void (^a)(void)) {
    Row *r = mk(RowAction, t);
    r.action = a;
    r.destructive = destructive;
    return r;
}
static Row *info(NSString *t) { return mk(RowInfo, t); }

static const int kScalePct[] = {50, 100, 200, 400};
static const int kSuperPct[] = {100, 150, 200, 300, 400};
static const int kFpsVals[] = {0, 30, 60, 90, 120, 240, 1000};
static const int kAnisoVals[] = {0, 2, 4, 8, 16};
static const int kFgMultVals[] = {0, 2, 3, 4, 6, 8};
static const int kSpeedSteps[] = NAPPLE_SPEED_STEPS;

static NSInteger index_of(const int *vals, int n, int v, NSInteger fb) {
    for (int i = 0; i < n; i++)
        if (vals[i] == v) return i;
    return fb;
}

/* ---- mods / cheats toggles (same files as the Mac overlay: <game dir>/mods, <game dir>/cheats, mods-enabled.json) ---- */
static NSString *game_dir(NappleFEiOS *fe) {
    char path[1100];
    napple_data_path(fe.cfg.config_dir, fe.titleID.UTF8String, "x", path, sizeof path);
    return [@(path) stringByDeletingLastPathComponent];
}
static NSMutableSet<NSString *> *enabled_set(NappleFEiOS *fe) {
    NSData *d = [NSData dataWithContentsOfFile:[game_dir(fe) stringByAppendingPathComponent:@"mods-enabled.json"]];
    NSArray *a = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL] : nil;
    return [NSMutableSet setWithArray:[a isKindOfClass:NSArray.class] ? a : @[]];
}
static void save_enabled(NappleFEiOS *fe, NSSet *set) {
    [NSFileManager.defaultManager createDirectoryAtPath:game_dir(fe) withIntermediateDirectories:YES attributes:nil error:NULL];
    NSData *d = [NSJSONSerialization dataWithJSONObject:[set.allObjects sortedArrayUsingSelector:@selector(compare:)] options:NSJSONWritingPrettyPrinted error:NULL];
    [d writeToFile:[game_dir(fe) stringByAppendingPathComponent:@"mods-enabled.json"] atomically:YES];
}
static void add_toggles(NappleFEiOS *fe, NSMutableArray *rows, NSString *sub, NSString *kind) {
    NSString *dir = [game_dir(fe) stringByAppendingPathComponent:sub];
    NSArray *names = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:NULL] sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    int n = 0;
    for (NSString *name in names) {
        if ([name hasPrefix:@"."]) continue;
        NSString *key = [sub stringByAppendingPathComponent:name];
        [rows addObject:toggle(name.stringByDeletingPathExtension, ^BOOL { return [enabled_set(fe) containsObject:key]; },
                               ^(BOOL on) {
                                   NSMutableSet *e = enabled_set(fe);
                                   if (on) [e addObject:key];
                                   else [e removeObject:key];
                                   save_enabled(fe, e);
                               })];
        n++;
    }
    if (!n) [rows addObject:info([NSString stringWithFormat:@"No %@ yet. Add them with the Files app in Napple > %@ > %@.", kind, fe.titleID, sub])];
}

/* ---- controller capture ---- */
@interface CapBox : NSObject
@property (weak) NappleIOSPanel *panel;
@property (weak) NappleFEiOS *fe;
@property int kind, sw, slot, player;
@end
@implementation CapBox
@end

@interface NappleIOSPanel () <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic) NSInteger player, tab;
@property (nonatomic, weak) NappleFEiOS *fe;
@property (nonatomic, strong) UIImpactFeedbackGenerator *tick;
@property (nonatomic, strong) UISegmentedControl *seg;
@property (nonatomic) BOOL browsing; /* the amiibo list replaces the Game tab */
@property (nonatomic, copy) NSString *query, *amiiboStatus;
@end

@implementation NappleIOSPanel {
    UIVisualEffectView *_glass;
    UITableView *_table;
    NSArray<Row *> *_rows;
    NSInteger _cursor;       /* controller navigation: index into _rows */
    NSTimer *_rep;
    BOOL _capturing;
}

- (instancetype)initWithFE:(NappleFEiOS *)fe {
    if ((self = [super initWithFrame:CGRectZero])) {
        _fe = fe;
        _player = -1;
        _cursor = -1;
        _tick = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
        self.translatesAutoresizingMaskIntoConstraints = NO;
        UIVisualEffect *eff = nil;
        if (@available(iOS 26.0, *)) {
            UIGlassEffect *ge = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
            ge.interactive = NO;
            eff = ge;
        }
        if (!eff) eff = [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemChromeMaterialDark];
        _glass = [[UIVisualEffectView alloc] initWithEffect:eff];
        _glass.translatesAutoresizingMaskIntoConstraints = NO;
        _glass.layer.cornerRadius = 30;
        _glass.layer.cornerCurve = kCACornerCurveContinuous;
        _glass.clipsToBounds = YES;
        _glass.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
        [self addSubview:_glass];
        self.layer.shadowColor = UIColor.blackColor.CGColor;
        self.layer.shadowOpacity = 0.35f;
        self.layer.shadowRadius = 22;
        self.layer.shadowOffset = CGSizeMake(0, 6);
        UIView *c = _glass.contentView;

        UILabel *title = [UILabel new];
        title.text = fe.titleName.length ? fe.titleName : @"Napple";
        title.font = [UIFont systemFontOfSize:17 weight:UIFontWeightBold];
        title.textColor = UIColor.whiteColor;
        title.lineBreakMode = NSLineBreakByTruncatingTail;
        UIButton *close = [UIButton buttonWithType:UIButtonTypeSystem];
        UIButtonConfiguration *cc = [UIButtonConfiguration plainButtonConfiguration];
        cc.image = [UIImage systemImageNamed:@"xmark.circle.fill"];
        cc.baseForegroundColor = [UIColor colorWithWhite:1 alpha:0.6];
        cc.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration configurationWithPointSize:22];
        close.configuration = cc;
        close.accessibilityLabel = @"Close";
        [close addAction:[UIAction actionWithHandler:^(UIAction *a) { [fe showSettings:NO]; }] forControlEvents:UIControlEventTouchUpInside];
        UIStackView *head = [[UIStackView alloc] initWithArrangedSubviews:@[ title, close ]];
        head.alignment = UIStackViewAlignmentCenter;
        head.translatesAutoresizingMaskIntoConstraints = NO;

        _seg = [[UISegmentedControl alloc] initWithItems:@[ @"Audio/Video", @"FPS/Speed", @"Controls", @"Game" ]];
        _seg.selectedSegmentIndex = 0;
        _seg.translatesAutoresizingMaskIntoConstraints = NO;
        [_seg setTitleTextAttributes:@{NSFontAttributeName : [UIFont systemFontOfSize:12 weight:UIFontWeightSemibold]} forState:UIControlStateNormal];
        __weak NappleIOSPanel *me = self;
        [_seg addAction:[UIAction actionWithHandler:^(UIAction *a) { [me selectTab:me.seg.selectedSegmentIndex]; }] forControlEvents:UIControlEventValueChanged];

        _table = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped];
        _table.translatesAutoresizingMaskIntoConstraints = NO;
        _table.backgroundColor = UIColor.clearColor;
        _table.dataSource = self;
        _table.delegate = self;
        _table.rowHeight = UITableViewAutomaticDimension;
        _table.estimatedRowHeight = 52;
        _table.keyboardDismissMode = UIScrollViewKeyboardDismissModeOnDrag;
        _table.contentInset = UIEdgeInsetsMake(0, 0, 12, 0);
        [c addSubview:head];
        [c addSubview:_seg];
        [c addSubview:_table];
        [NSLayoutConstraint activateConstraints:@[
            [_glass.topAnchor constraintEqualToAnchor:self.topAnchor], [_glass.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [_glass.leadingAnchor constraintEqualToAnchor:self.leadingAnchor], [_glass.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
            [head.topAnchor constraintEqualToAnchor:c.topAnchor constant:14], [head.leadingAnchor constraintEqualToAnchor:c.leadingAnchor constant:20],
            [head.trailingAnchor constraintEqualToAnchor:c.trailingAnchor constant:-10],
            [_seg.topAnchor constraintEqualToAnchor:head.bottomAnchor constant:8], [_seg.leadingAnchor constraintEqualToAnchor:c.leadingAnchor constant:16],
            [_seg.trailingAnchor constraintEqualToAnchor:c.trailingAnchor constant:-16], [_seg.heightAnchor constraintEqualToConstant:34],
            [_table.topAnchor constraintEqualToAnchor:_seg.bottomAnchor constant:6], [_table.bottomAnchor constraintEqualToAnchor:c.bottomAnchor],
            [_table.leadingAnchor constraintEqualToAnchor:c.leadingAnchor], [_table.trailingAnchor constraintEqualToAnchor:c.trailingAnchor]
        ]];
        UISwipeGestureRecognizer *sw = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(swiped)];
        sw.direction = UISwipeGestureRecognizerDirectionRight;
        [self addGestureRecognizer:sw];
        [self rebuild];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(controllersChanged) name:GCControllerDidConnectNotification object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(controllersChanged) name:GCControllerDidDisconnectNotification object:nil];
        [self attachControllers];
    }
    return self;
}
- (void)swiped {
    [_fe showSettings:NO];
}
- (void)controllersChanged {
    [self attachControllers];
    if (_tab == 2) [self reload];
}
- (void)selectTab:(NSInteger)i {
    _tab = MIN(MAX(i, 0), 3);
    _browsing = NO;
    _seg.selectedSegmentIndex = _tab;
    _cursor = -1;
    [_tick impactOccurred];
    [self reload];
    [_table setContentOffset:CGPointMake(0, -_table.adjustedContentInset.top) animated:NO];
}
- (void)reload {
    [self rebuild];
    [_table reloadData];
    [self showCursor];
}
- (void)setPadMode:(BOOL)on {
    _padMode = on;
    _cursor = on ? [self firstInteractive] : -1;
    [self showCursor];
}

/* ---- row building ---------------------------------------------------------------------------------------------------- */
- (void)rebuild {
    NappleFEiOS *fe = _fe;
    NSMutableArray<Row *> *r = [NSMutableArray new];
#define S() [fe currentSettings]
#define MUT(...) [fe mutate:^(napple_settings *t) { __VA_ARGS__ }]
    switch (_tab) {
        case 0: { /* ---- Audio / Video ---- */
            [r addObject:mk(RowHeader, @"Audio")];
            [r addObject:slider(@"Volume", 0, 1, @"%.0f%%", ^float { return S().master_volume; }, ^(float v) { MUT(t->master_volume = v;); })];
            [r addObject:mk(RowHeader, @"Render target")];
            [r addObject:menu(@"Render resolution", @[ @"0.5x", @"1x (native)", @"2x", @"4x" ],
                              ^NSInteger {
                                  napple_settings s = S();
                                  return s.render_mode == 1 ? index_of(kScalePct, 4, s.internal_scale_percent, 1) : 1;
                              },
                              ^(NSInteger i) { MUT(t->render_mode = 1; t->internal_width = t->internal_height = 0; t->internal_scale_percent = kScalePct[i];); })];
            [r addObject:menu(@"Supersampling", @[ @"Off (1x)", @"1.5x", @"2x", @"3x", @"4x" ],
                              ^NSInteger { return index_of(kSuperPct, 5, S().supersample_percent, 0); },
                              ^(NSInteger i) { MUT(t->supersample_percent = kSuperPct[i];); })];
            [r addObject:menu(@"Upscaler", @[ @"None (filter only)", @"MetalFX spatial" ], ^NSInteger { return S().upscaler; }, ^(NSInteger i) { MUT(t->upscaler = (int)i;); })];
            [r addObject:menu(@"Output filter", @[ @"Bilinear", @"Nearest", @"Box", @"Bicubic", @"Lanczos", @"Sharpened bicubic" ],
                              ^NSInteger { return S().filter; }, ^(NSInteger i) { MUT(t->filter = (int)i;); })];
            [r addObject:slider(@"Sharpness", 0, 100, @"%.0f", ^float { return S().sharpness; }, ^(float v) { MUT(t->sharpness = (int)v;); })];
            [r addObject:menu(@"Anisotropic filtering", @[ @"As the game asks", @"2x", @"4x", @"8x", @"16x" ],
                              ^NSInteger { return index_of(kAnisoVals, 5, S().anisotropy, 0); }, ^(NSInteger i) { MUT(t->anisotropy = kAnisoVals[i];); })];
            [r addObject:mk(RowHeader, @"Statistics")];
            [r addObject:toggle(@"Show statistics", ^BOOL { return S().show_stats; }, ^(BOOL on) { MUT(t->show_stats = on;); })];
            [r addObject:menu(@"Corner", @[ @"Top left", @"Top right", @"Bottom left", @"Bottom right" ], ^NSInteger { return S().stats_corner; },
                              ^(NSInteger i) { MUT(t->stats_corner = (int)i;); })];
            [r addObject:menu(@"Text size", @[ @"Small", @"Medium", @"Large" ], ^NSInteger { return S().stats_size; }, ^(NSInteger i) { MUT(t->stats_size = (int)i;); })];
            [r addObject:slider(@"Background", 10, 100, @"%.0f%%", ^float { return S().stats_opacity; }, ^(float v) { MUT(t->stats_opacity = (int)v;); })];
            [r addObject:toggle(@"Show Metal statistics", ^BOOL { return S().metal_hud; }, ^(BOOL on) { MUT(t->metal_hud = on;); })];
            [r addObject:info(@"The Metal HUD is Apple's native performance overlay: frame rate, GPU and frame time, memory and shader compiles.")];
            break;
        }
        case 1: { /* ---- FPS / Speed ---- */
            [r addObject:mk(RowHeader, @"Frame rate")];
            [r addObject:menu(@"Display rate", @[ @"Game default", @"30 Hz", @"60 Hz", @"90 Hz", @"120 Hz", @"240 Hz", @"Uncapped" ],
                              ^NSInteger { return index_of(kFpsVals, 7, S().fps_cap, 0); }, ^(NSInteger i) { MUT(t->fps_cap = kFpsVals[i];); })];
            [r addObject:toggle(@"VSync", ^BOOL { return S().vsync; }, ^(BOOL on) { MUT(t->vsync = on;); })];
            [r addObject:toggle(@"Frame generation", ^BOOL { return S().frame_gen; }, ^(BOOL on) { MUT(t->frame_gen = on;); })];
            [r addObject:menu(@"Images per frame", @[ @"Automatic", @"2x", @"3x", @"4x", @"6x", @"8x" ],
                              ^NSInteger { return index_of(kFgMultVals, 6, S().frame_gen_mult, 0); }, ^(NSInteger i) { MUT(t->frame_gen_mult = kFgMultVals[i];); })];
            [r addObject:menu(@"Method", @[ @"Best available", @"Optical flow", @"Optical flow, low latency", @"Motion-compensated", @"Plain blend" ],
                              ^NSInteger { return S().frame_gen_method; }, ^(NSInteger i) { MUT(t->frame_gen_method = (int)i;); })];
            [r addObject:info(@"Frame generation shows several images per game frame without changing game speed. It adds about half a game frame of latency.")];
            [r addObject:mk(RowHeader, @"Game speed")];
            [r addObject:slider(@"Speed", 10, 400, @"%.0f%%", ^float { return MIN(S().speed_percent, 400); }, ^(float v) { MUT(t->speed_percent = (int)v;); })];
            NSMutableArray *names = [NSMutableArray new];
            for (unsigned i = 0; i < sizeof kSpeedSteps / sizeof *kSpeedSteps; i++) [names addObject:[NSString stringWithFormat:@"%.3gx", kSpeedSteps[i] / 100.0]];
            [r addObject:menu(@"Speed preset", names, ^NSInteger { return index_of(kSpeedSteps, (int)(sizeof kSpeedSteps / sizeof *kSpeedSteps), S().speed_percent, 4); },
                              ^(NSInteger i) { MUT(t->speed_percent = kSpeedSteps[i];); })];
            [r addObject:slider(@"Turbo speed", 110, 1000, @"%.0f%%", ^float { return S().turbo_percent; }, ^(float v) { MUT(t->turbo_percent = (int)v;); })];
            [r addObject:menu(@"Mode", @[ @"Game clock and vsync", @"Vsync only (frame skip)", @"Game clock only" ], ^NSInteger { return S().speed_mode; },
                              ^(NSInteger i) { MUT(t->speed_mode = (int)i;); })];
            [r addObject:menu(@"Audio at other speeds", @[ @"Automatic", @"Always pitch shift", @"Silent when not 1x" ], ^NSInteger { return S().audio_speed_mode; },
                              ^(NSInteger i) { MUT(t->audio_speed_mode = (int)i;); })];
            break;
        }
        case 2: [self buildControls:r]; break;
        default: [self buildSystem:r]; break;
    }
#undef MUT
#undef S
    _rows = r;
}

static NSString *style_name(uint32_t st) {
    if (!st) return @"not in use";
    return st & NAPPLE_STYLE_FULL_KEY ? @"Pro Controller" : st & NAPPLE_STYLE_HANDHELD ? @"Handheld" : st & NAPPLE_STYLE_JOY_DUAL ? @"Joy-Con pair" : @"Joy-Con";
}
- (void)bget:(napple_bindings *)b {
    if (_player >= 0) napple_input_get_player_bindings(_fe.input, (int)_player, b);
    else napple_input_get_bindings(_fe.input, b);
}
- (int)bset:(const napple_bindings *)b {
    return _player >= 0 ? napple_input_set_player_bindings(_fe.input, (int)_player, b) : napple_input_set_bindings(_fe.input, b);
}
- (void)edit:(void (^)(napple_bindings *))mut {
    napple_bindings t;
    [self bget:&t];
    mut(&t);
    if ([self bset:&t] != 0) [self reload];
}
- (void)capture:(int)kind sw:(int)sw slot:(int)slot {
    if (!_fe.input || _capturing) return;
    _capturing = YES;
    [_fe toast:@"Press a button on your controller"];
    CapBox *cb = [CapBox new];
    cb.panel = self;
    cb.kind = kind;
    cb.sw = sw;
    cb.slot = slot;
    cb.player = (int)_player;
    napple_input_capture_next(_fe.input, capture_done, (void *)CFBridgingRetain(cb));
}
static void capture_done(void *u, int is_key, int code) {
    CapBox *b = (__bridge_transfer CapBox *)u;
    dispatch_async(dispatch_get_main_queue(), ^{
        NappleIOSPanel *p = b.panel;
        if (!p) return;
        p->_capturing = NO;
        BOOL cancel = is_key && code == 53;
        if (!cancel) {
            [p edit:^(napple_bindings *t) {
                if (is_key && b.kind == 0) t->key[b.sw][b.slot] = code == 51 ? NAPPLE_KEY_NONE : (uint16_t)code;
                else if (!is_key && b.kind == 1) t->pad[b.sw][b.slot] = (int16_t)code;
            }];
        }
        [p reload];
    });
}
- (void)buildControls:(NSMutableArray<Row *> *)r {
    NappleFEiOS *fe = _fe;
    __weak NappleIOSPanel *me = self;
    [r addObject:mk(RowHeader, @"On-screen controller")];
    [r addObject:toggle(@"Show on-screen controls", ^BOOL { return pref_bool(@"napple.padVisible", YES); },
                        ^(BOOL on) {
                            [NSUserDefaults.standardUserDefaults setBool:on forKey:@"napple.padVisible"];
                            [NSNotificationCenter.defaultCenter postNotificationName:@"napple.padChanged" object:nil];
                        })];
    [r addObject:slider(@"Opacity", 0.15f, 1, @"%.0f%%", ^float { return pref_float(@"napple.padOpacity", 0.45f) * 100; },
                        ^(float v) {
                            [NSUserDefaults.standardUserDefaults setFloat:v / 100 forKey:@"napple.padOpacity"];
                            [NSNotificationCenter.defaultCenter postNotificationName:@"napple.padChanged" object:nil];
                        })];
    ((Row *)r.lastObject).min = 15;
    ((Row *)r.lastObject).max = 100;
    [r addObject:toggle(@"Haptic feedback", ^BOOL { return pref_bool(@"napple.padHaptics", YES); },
                        ^(BOOL on) { [NSUserDefaults.standardUserDefaults setBool:on forKey:@"napple.padHaptics"]; })];
    [r addObject:toggle(@"Mouse / touch = touch screen", ^BOOL { napple_bindings b; napple_input_get_bindings(fe.input, &b); return b.mouse_touch != 0; },
                        ^(BOOL on) { napple_bindings b; napple_input_get_bindings(fe.input, &b); b.mouse_touch = on; napple_input_set_bindings(fe.input, &b); })];
    if (!fe.input) {
        [r addObject:info(@"Input is not attached.")];
        return;
    }
    napple_input_snapshot sn;
    napple_input_get_snapshot(fe.input, &sn);
    [r addObject:mk(RowHeader, @"Players and controllers")];
    NSMutableArray *names = [NSMutableArray arrayWithObject:@"All players (shared)"];
    for (int i = 0; i < 8; i++) {
        if (i > 0 && !sn.player_style[i] && sn.controllers <= i) continue;
        [names addObject:[NSString stringWithFormat:@"Player %d  -  %@", i + 1, i < sn.controllers ? @(sn.controller_names[i]) : (i == 0 ? @"touch / keyboard" : @"no controller")]];
    }
    NSInteger sel = _player + 1 < (NSInteger)names.count ? _player + 1 : 0;
    [r addObject:menu(@"Editing", names, ^NSInteger { return sel; }, ^(NSInteger i) { me.player = i - 1; [me reload]; })];
    if (_player >= 0) {
        napple_bindings tmp;
        int own = napple_input_get_player_bindings(fe.input, (int)_player, &tmp);
        [r addObject:info([NSString stringWithFormat:@"%@  -  %@", style_name(sn.player_style[_player]), own ? @"own profile" : @"follows the shared profile"])];
        if (!own)
            [r addObject:action([NSString stringWithFormat:@"Give player %d its own profile", (int)_player + 1], NO, ^{
                      napple_bindings cp;
                      napple_input_get_player_bindings(fe.input, (int)me.player, &cp);
                      napple_input_set_player_bindings(fe.input, (int)me.player, &cp);
                      [me reload];
                  })];
    } else
        [r addObject:info(sn.controllers ? [NSString stringWithFormat:@"%d controller%@ connected: %@", sn.controllers, sn.controllers > 1 ? @"s" : @"", @(sn.controller_names[0])]
                                         : @"No controller connected: the on-screen controller drives player 1.")];
    napple_bindings bd;
    [self bget:&bd];
    [r addObject:mk(RowHeader, @"Controller buttons (tap, then press the button)")];
    for (int b = 0; b < NAPPLE_BTN_COUNT_BASIC; b++) {
        int bi = b;
        Row *row = action([NSString stringWithFormat:@"%s   ->   %@", napple_button_name(b), bd.pad[b][0] >= 0 ? @(napple_pad_element_name(bd.pad[b][0])) : @"unbound"], NO,
                          ^{ [me capture:1 sw:bi slot:0]; });
        [r addObject:row];
    }
    if (_player <= 0) {
        [r addObject:mk(RowHeader, @"Keyboard buttons (iPad)")];
        for (int b = 0; b < NAPPLE_BTN_COUNT_BASIC; b++) {
            int bi = b;
            [r addObject:action([NSString stringWithFormat:@"%s   ->   %@", napple_button_name(b), @(napple_keycode_name(bd.key[b][0]))], NO, ^{ [me capture:0 sw:bi slot:0]; })];
        }
    }
    [r addObject:mk(RowHeader, @"Sticks and triggers")];
    [r addObject:slider(@"Dead zone", 0, 0.8f, @"%.2f", ^float { napple_bindings t; [me bget:&t]; return t.stick_deadzone; },
                        ^(float v) { [me edit:^(napple_bindings *t) { t->stick_deadzone = MIN(v, t->stick_range - 0.05f); }]; })];
    [r addObject:slider(@"Outer range", 0.5f, 1, @"%.2f", ^float { napple_bindings t; [me bget:&t]; return t.stick_range; },
                        ^(float v) { [me edit:^(napple_bindings *t) { t->stick_range = v; if (t->stick_deadzone >= v) t->stick_deadzone = v - 0.05f; }]; })];
    [r addObject:slider(@"Response curve", 0.5f, 3, @"%.2f", ^float { napple_bindings t; [me bget:&t]; return t.stick_curve; },
                        ^(float v) { [me edit:^(napple_bindings *t) { t->stick_curve = v; }]; })];
    [r addObject:slider(@"Trigger point", 0.05f, 0.95f, @"%.2f", ^float { napple_bindings t; [me bget:&t]; return t.trigger_threshold; },
                        ^(float v) { [me edit:^(napple_bindings *t) { t->trigger_threshold = v; }]; })];
    static const char *const invn[4] = {"Invert left X", "Invert left Y", "Invert right X", "Invert right Y"};
    for (int i = 0; i < 4; i++) {
        int k = i;
        [r addObject:toggle(@(invn[i]), ^BOOL { napple_bindings t; [me bget:&t]; return t.invert[k] != 0; }, ^(BOOL on) { [me edit:^(napple_bindings *t) { t->invert[k] = on; }]; })];
    }
    [r addObject:mk(RowHeader, @"Controller and console")];
    [r addObject:menu(@"Controller type", @[ @"Automatic", @"Pro Controller", @"Joy-Con pair", @"Handheld" ], ^NSInteger { napple_bindings t; [me bget:&t]; return t.style; },
                      ^(NSInteger i) { [me edit:^(napple_bindings *t) { t->style = (int)i; }]; })];
    [r addObject:menu(@"Face buttons", @[ @"Automatic", @"By position", @"By label" ], ^NSInteger { napple_bindings t; [me bget:&t]; return t.face_layout; },
                      ^(NSInteger i) { [me edit:^(napple_bindings *t) { t->face_layout = (int)i; }]; })];
    [r addObject:menu(@"Console mode", @[ @"Docked (1080p)", @"Handheld (720p)" ], ^NSInteger { napple_bindings t; [me bget:&t]; return t.operation_mode; },
                      ^(NSInteger i) { [me edit:^(napple_bindings *t) { t->operation_mode = (int)i; }]; })];
    [r addObject:info(@"Controller type is read when the game starts; console mode applies on restart.")];
    [r addObject:action(_player >= 0 ? @"Use the shared profile" : @"Reset to defaults", YES, ^{
              if (me.player >= 0) napple_input_clear_player_bindings(fe.input, (int)me.player);
              else napple_input_reset_bindings(fe.input);
              [me reload];
          })];
}

- (void)buildSystem:(NSMutableArray<Row *> *)r {
    NappleFEiOS *fe = _fe;
    __weak NappleIOSPanel *me = self;
#define S() [fe currentSettings]
    if (self.browsing) {
        AmiiboPicker *ap = sim_amiibo_picker();
        [r addObject:action(@"‹  Back", NO, ^{ me.browsing = NO; [me reload]; })];
        Row *sr = mk(RowSearch, @"Search amiibo");
        sr.detail = self.query ?: @"";
        [r addObject:sr];
        if (self.amiiboStatus) [r addObject:info(self.amiiboStatus)];
        size_t cap = sim_amiibo_count(ap);
        if (cap) {
            size_t *idx = calloc(cap, sizeof *idx);
            size_t n = sim_amiibo_search(ap, (self.query ?: @"").UTF8String, idx, cap);
            for (size_t i = 0; i < n && i < 120; i++) {
                const AmiiboInfo *a = sim_amiibo_get(ap, idx[i]);
                if (!a) continue;
                NSString *hex = @(a->id);
                [r addObject:action([NSString stringWithFormat:@"%s   -   %s", a->name, a->series], NO, ^{
                          int rc = sim_amiibo_place(ap, hex.UTF8String, 0);
                          [fe toast:rc == 0 ? [NSString stringWithFormat:@"%@ placed on the reader", hex] : @"Could not place the amiibo"];
                          me.browsing = NO;
                          [me reload];
                      })];
            }
            if (n > 120) [r addObject:info([NSString stringWithFormat:@"%zu more: type to narrow the list.", n - 120])];
            free(idx);
        }
        (void)fe;
        return;
    }
    [r addObject:mk(RowHeader, @"Game")];
    if (fe.manifestTitle) {
        [r addObject:info([NSString stringWithFormat:@"%@\nBase version %@  -  %@", fe.manifestTitle, fe.manifestBase, fe.titleID])];
        if (fe.updates.count) {
            NSMutableArray *names = [NSMutableArray arrayWithObject:@"Latest installed"];
            for (NSDictionary *u in fe.updates) [names addObject:u[@"name"] ?: u[@"version"]];
            [r addObject:menu(@"Update version", names,
                              ^NSInteger {
                                  napple_settings s = S();
                                  if (!s.update_version[0]) return 0;
                                  for (NSUInteger i = 0; i < fe.updates.count; i++)
                                      if ([fe.updates[i][@"version"] isEqualToString:@(s.update_version)]) return (NSInteger)i + 1;
                                  return 0;
                              },
                              ^(NSInteger i) {
                                  [fe mutate:^(napple_settings *t) {
                                      if (i == 0) t->update_version[0] = 0;
                                      else strlcpy(t->update_version, [fe.updates[i - 1][@"version"] UTF8String], sizeof t->update_version);
                                  }];
                              })];
        }
        for (NSDictionary *d in fe.dlc) {
            NSString *tid = d[@"id"];
            [r addObject:toggle(d[@"name"] ?: tid,
                                ^BOOL {
                                    napple_settings s = S();
                                    for (int i = 0; i < s.disabled_dlc_count; i++)
                                        if (!strcmp(s.disabled_dlc[i], tid.UTF8String)) return NO;
                                    return YES;
                                },
                                ^(BOOL on) {
                                    [fe mutate:^(napple_settings *t) {
                                        int at = -1;
                                        for (int i = 0; i < t->disabled_dlc_count; i++)
                                            if (!strcmp(t->disabled_dlc[i], tid.UTF8String)) at = i;
                                        if (on && at >= 0) {
                                            memmove(t->disabled_dlc[at], t->disabled_dlc[at + 1], (size_t)(t->disabled_dlc_count - at - 1) * 20);
                                            t->disabled_dlc_count--;
                                        } else if (!on && at < 0 && t->disabled_dlc_count < 32)
                                            strlcpy(t->disabled_dlc[t->disabled_dlc_count++], tid.UTF8String, 20);
                                    }];
                                })];
        }
        [r addObject:action(@"Restart game to apply", NO, ^{
                  [fe showSettings:NO];
                  [fe restartGuest];
              })];
    } else
        [r addObject:info(@"No game manifest loaded.")];
    [r addObject:mk(RowHeader, @"Mii")];
    [r addObject:info(@"Games read their Miis from the system Mii database of the firmware the game was built with. The Mii editor and picker arrive with the firmware applet host.")];
    [r addObject:mk(RowHeader, @"amiibo")];
    AmiiboPicker *ap = sim_amiibo_picker();
    if (!ap) [r addObject:info(@"The amiibo reader is not available in this build.")];
    else {
        [r addObject:info([NSString stringWithFormat:@"%@  %zu amiibo in the list.%@", sim_amiibo_present(ap) ? @"An amiibo is on the reader." : @"No amiibo on the reader.", sim_amiibo_count(ap),
                                                    self.amiiboStatus ? [@"\n" stringByAppendingString:self.amiiboStatus] : @""])];
        [r addObject:action(@"Choose an amiibo…", NO, ^{
                  me.browsing = YES;
                  me.query = @"";
                  sim_amiibo_load(ap, sim_amiibo_count(ap) == 0, ^(NSString *st) { me.amiiboStatus = st; [me reload]; });
                  [me reload];
              })];
        [r addObject:action(@"Remove amiibo from the reader", YES, ^{ sim_amiibo_remove(ap); [me reload]; })];
    }
    [r addObject:mk(RowHeader, @"IR camera")];
    if (!sim_camera_available()) [r addObject:info(@"The IR camera simulator is not available in this build.")];
    else {
        sim_camera_install();
        sim_camera_on_change(^{ if (me.tab == 3 && !me.browsing) [me reload]; });
        static NSString *const states[4] = {@"Not asked yet. Napple asks for the camera only when a game starts the IR camera.", @"Allowed. The camera turns on when a game starts the IR camera.",
                                            @"Denied. Games see the IR sensor as not connected. Allow it in Settings > Napple > Camera.", @"Restricted on this device."};
        [r addObject:info(sim_camera_running() ? [NSString stringWithFormat:@"The camera is in use as the IR sensor (%@).", sim_camera_name() ?: @"camera"] : states[sim_camera_auth()])];
        if (sim_camera_auth() == 0) [r addObject:action(@"Allow camera now", NO, ^{ sim_camera_request(); })];
        if (sim_camera_auth() == 2) [r addObject:action(@"Open Settings", NO, ^{ [UIApplication.sharedApplication openURL:[NSURL URLWithString:UIApplicationOpenSettingsURLString] options:@{} completionHandler:nil]; })];
        [r addObject:toggle(@"Mirror the image", ^BOOL { return sim_camera_mirror(); }, ^(BOOL on) { sim_camera_set_mirror(on); })];
    }
    [r addObject:mk(RowHeader, @"Mods")];
    add_toggles(fe, r, @"mods", @"mods");
    [r addObject:mk(RowHeader, @"Cheats")];
    add_toggles(fe, r, @"cheats", @"cheat files (.txt)");
    [r addObject:info(@"Choices are saved with the game's settings and apply when the game restarts.")];
    [r addObject:mk(RowHeader, @"")];
    [r addObject:action(@"Take screenshot", NO, ^{
              [fe showSettings:NO];
              dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ [fe screenshot]; });
          })];
    [r addObject:action(@"Quit game", YES, ^{ [fe quit]; })];
#undef S
    (void)me;
}

/* ---- table ----------------------------------------------------------------------------------------------------------- */
- (NSInteger)numberOfSectionsInTableView:(UITableView *)t {
    return 1;
}
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s {
    return (NSInteger)_rows.count;
}
- (CGFloat)tableView:(UITableView *)t heightForRowAtIndexPath:(NSIndexPath *)ip {
    Row *r = _rows[(NSUInteger)ip.row];
    return r.kind == RowHeader ? (r.title.length ? 40 : 16) : UITableViewAutomaticDimension;
}
- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip {
    Row *r = _rows[(NSUInteger)ip.row];
    UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    c.backgroundColor = [UIColor colorWithWhite:1 alpha:r.kind == RowHeader ? 0 : 0.08];
    UIListContentConfiguration *cfg = [UIListContentConfiguration cellConfiguration];
    cfg.text = r.title;
    cfg.textProperties.color = UIColor.whiteColor;
    cfg.textProperties.font = [UIFont systemFontOfSize:16];
    c.selectionStyle = UITableViewCellSelectionStyleNone;
    __weak NappleIOSPanel *me = self;
    switch (r.kind) {
        case RowHeader:
            c.backgroundColor = UIColor.clearColor;
            cfg.textProperties.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
            cfg.textProperties.color = [UIColor colorWithWhite:1 alpha:0.6];
            cfg.textProperties.transform = UIListContentTextTransformUppercase;
            break;
        case RowInfo:
            cfg.textProperties.font = [UIFont systemFontOfSize:13];
            cfg.textProperties.color = [UIColor colorWithWhite:1 alpha:0.65];
            c.backgroundColor = UIColor.clearColor;
            break;
        case RowToggle: {
            UISwitch *sw = [UISwitch new];
            sw.on = r.getBool();
            [sw addAction:[UIAction actionWithHandler:^(UIAction *a) {
                     r.setBool(sw.on);
                     [me.tick impactOccurred];
                 }] forControlEvents:UIControlEventValueChanged];
            c.accessoryView = sw;
            break;
        }
        case RowSlider: {
            UISlider *sl = [[UISlider alloc] initWithFrame:CGRectMake(0, 0, 150, 40)];
            sl.minimumValue = r.min;
            sl.maximumValue = r.max;
            sl.value = r.getFloat();
            UILabel *val = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, 52, 24)];
            val.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightMedium];
            val.textColor = [UIColor colorWithWhite:1 alpha:0.7];
            val.textAlignment = NSTextAlignmentRight;
            BOOL pct = [r.fmt containsString:@"%%"] && r.max <= 1.0001f;
            val.text = [NSString stringWithFormat:r.fmt, pct ? sl.value * 100 : sl.value];
            [sl addAction:[UIAction actionWithHandler:^(UIAction *a) {
                     val.text = [NSString stringWithFormat:r.fmt, pct ? sl.value * 100 : sl.value];
                     r.setFloat(sl.value);
                 }] forControlEvents:UIControlEventValueChanged];
            UIStackView *st = [[UIStackView alloc] initWithArrangedSubviews:@[ sl, val ]];
            st.spacing = 8;
            st.frame = CGRectMake(0, 0, 214, 40);
            [sl.widthAnchor constraintEqualToConstant:150].active = YES;
            [val.widthAnchor constraintEqualToConstant:52].active = YES;
            c.accessoryView = st;
            break;
        }
        case RowMenu: {
            NSInteger i = r.getIndex();
            UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
            UIButtonConfiguration *bc = [UIButtonConfiguration plainButtonConfiguration];
            bc.title = (i >= 0 && i < (NSInteger)r.options.count) ? r.options[(NSUInteger)i] : @"";
            bc.image = [UIImage systemImageNamed:@"chevron.up.chevron.down"];
            bc.imagePlacement = NSDirectionalRectEdgeTrailing;
            bc.imagePadding = 6;
            bc.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration configurationWithPointSize:11];
            bc.titleTextAttributesTransformer = ^NSDictionary *(NSDictionary *a) {
                NSMutableDictionary *d = [a mutableCopy];
                d[NSFontAttributeName] = [UIFont systemFontOfSize:15];
                return d;
            };
            b.configuration = bc;
            NSMutableArray *acts = [NSMutableArray new];
            [r.options enumerateObjectsUsingBlock:^(NSString *o, NSUInteger k, BOOL *stop) {
                UIAction *a = [UIAction actionWithTitle:o image:nil identifier:nil handler:^(UIAction *x) {
                    r.setIndex((NSInteger)k);
                    [me.tick impactOccurred];
                    [me reloadRowAt:ip];
                }];
                a.state = (NSInteger)k == i ? UIMenuElementStateOn : UIMenuElementStateOff;
                [acts addObject:a];
            }];
            b.menu = [UIMenu menuWithChildren:acts];
            b.showsMenuAsPrimaryAction = YES;
            [b sizeToFit];
            c.accessoryView = b;
            break;
        }
        case RowSearch: {
            UITextField *tf = [[UITextField alloc] initWithFrame:CGRectMake(0, 0, 260, 36)];
            tf.text = r.detail;
            tf.placeholder = @"Search amiibo";
            tf.textColor = UIColor.whiteColor;
            tf.clearButtonMode = UITextFieldViewModeWhileEditing;
            tf.autocorrectionType = UITextAutocorrectionTypeNo;
            tf.returnKeyType = UIReturnKeyDone;
            [tf addAction:[UIAction actionWithHandler:^(UIAction *a) { [me searchChanged:tf.text]; }] forControlEvents:UIControlEventEditingChanged];
            [tf addAction:[UIAction actionWithHandler:^(UIAction *a) { [tf resignFirstResponder]; }] forControlEvents:UIControlEventEditingDidEndOnExit];
            tf.tag = 77;
            tf.translatesAutoresizingMaskIntoConstraints = NO;
            [c.contentView addSubview:tf];
            [NSLayoutConstraint activateConstraints:@[
                [tf.leadingAnchor constraintEqualToAnchor:c.contentView.layoutMarginsGuide.leadingAnchor], [tf.trailingAnchor constraintEqualToAnchor:c.contentView.layoutMarginsGuide.trailingAnchor],
                [tf.topAnchor constraintEqualToAnchor:c.contentView.topAnchor constant:8], [tf.bottomAnchor constraintEqualToAnchor:c.contentView.bottomAnchor constant:-8],
                [tf.heightAnchor constraintGreaterThanOrEqualToConstant:36]
            ]];
            cfg.text = nil;
            break;
        }
        case RowAction:
            cfg.textProperties.color = r.destructive ? UIColor.systemRedColor : self.tintColor;
            c.selectionStyle = UITableViewCellSelectionStyleDefault;
            break;
    }
    cfg.textProperties.numberOfLines = 0;
    c.contentConfiguration = cfg;
    if (_padMode && ip.row == _cursor) {
        c.layer.borderColor = self.tintColor.CGColor;
        c.layer.borderWidth = 2;
        c.layer.cornerRadius = 10;
    }
    return c;
}
/* Typing in the amiibo search: only the result rows (index 2 on) are replaced, so the field keeps the keyboard. */
- (void)searchChanged:(NSString *)q {
    self.query = q;
    NSInteger old = (NSInteger)_rows.count;
    [self rebuild];
    NSInteger now = (NSInteger)_rows.count;
    NSMutableArray *del = [NSMutableArray new], *ins = [NSMutableArray new];
    for (NSInteger i = 2; i < old; i++) [del addObject:[NSIndexPath indexPathForRow:i inSection:0]];
    for (NSInteger i = 2; i < now; i++) [ins addObject:[NSIndexPath indexPathForRow:i inSection:0]];
    [_table performBatchUpdates:^{
        [self->_table deleteRowsAtIndexPaths:del withRowAnimation:UITableViewRowAnimationNone];
        [self->_table insertRowsAtIndexPaths:ins withRowAnimation:UITableViewRowAnimationNone];
    } completion:nil];
}
- (void)reloadRowAt:(NSIndexPath *)ip {
    [self rebuild];
    [_table reloadData];
}
- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip {
    Row *r = _rows[(NSUInteger)ip.row];
    [tv deselectRowAtIndexPath:ip animated:YES];
    if (r.kind == RowAction) {
        [_tick impactOccurred];
        r.action();
    }
}

/* ---- controller navigation ------------------------------------------------------------------------------------------- */
- (NSInteger)firstInteractive {
    for (NSInteger i = 0; i < (NSInteger)_rows.count; i++)
        if (_rows[(NSUInteger)i].interactive) return i;
    return -1;
}
- (void)showCursor {
    [_table reloadData];
    if (_padMode && _cursor >= 0 && _cursor < (NSInteger)_rows.count)
        [_table scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:_cursor inSection:0] atScrollPosition:UITableViewScrollPositionMiddle animated:YES];
}
- (void)moveCursor:(int)d {
    if (!_rows.count) return;
    NSInteger i = _cursor < 0 ? 0 : _cursor, n = (NSInteger)_rows.count;
    for (int k = 0; k < n; k++) {
        i = (i + d + n) % n;
        if (_rows[(NSUInteger)i].interactive) break;
    }
    _cursor = i;
    [_tick impactOccurred];
    [self showCursor];
}
- (void)adjust:(int)d {
    if (_cursor < 0 || _cursor >= (NSInteger)_rows.count) return;
    Row *r = _rows[(NSUInteger)_cursor];
    if (r.kind == RowSlider) {
        float v = MIN(r.max, MAX(r.min, r.getFloat() + d * (r.max - r.min) / 20));
        r.setFloat(v);
    } else if (r.kind == RowMenu) {
        NSInteger n = (NSInteger)r.options.count;
        r.setIndex((r.getIndex() + d + n) % n);
    } else if (r.kind == RowToggle) r.setBool(d > 0);
    else return;
    [_tick impactOccurred];
    [self reload];
}
- (void)activate {
    if (_cursor < 0 || _cursor >= (NSInteger)_rows.count) return;
    Row *r = _rows[(NSUInteger)_cursor];
    if (r.kind == RowToggle) r.setBool(!r.getBool());
    else if (r.kind == RowAction) r.action();
    else if (r.kind == RowMenu) {
        NSInteger n = (NSInteger)r.options.count;
        r.setIndex((r.getIndex() + 1) % n);
    } else return;
    [_tick impactOccurred];
    [self reload];
}
- (void)repeat:(void (^)(void))act {
    [_rep invalidate];
    act();
    _rep = [NSTimer scheduledTimerWithTimeInterval:0.22 repeats:YES block:^(NSTimer *t) { act(); }];
}
- (void)stopRepeat {
    [_rep invalidate];
    _rep = nil;
}
- (void)attachControllers {
    __weak NappleIOSPanel *me = self;
    for (GCController *c in GCController.controllers) {
        GCExtendedGamepad *g = c.extendedGamepad;
        if (!g || objc_getAssociatedObject(c, "napple.panelnav")) continue;
        objc_setAssociatedObject(c, "napple.panelnav", @YES, OBJC_ASSOCIATION_RETAIN);
        void (^dir)(BOOL, void (^)(void)) = ^(BOOL p, void (^act)(void)) {
            if (!me.padMode) return;
            if (p) [me repeat:act];
            else [me stopRepeat];
        };
        g.dpad.up.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { dir(p, ^{ [me moveCursor:-1]; }); };
        g.dpad.down.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { dir(p, ^{ [me moveCursor:1]; }); };
        g.dpad.left.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { dir(p, ^{ [me adjust:-1]; }); };
        g.dpad.right.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { dir(p, ^{ [me adjust:1]; }); };
        g.buttonA.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { if (p && me.padMode) [me activate]; };
        g.buttonB.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { if (p && me.padMode) [me.fe showSettings:NO]; };
        g.leftShoulder.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { if (p && me.padMode) [me selectTab:me.tab - 1]; };
        g.rightShoulder.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { if (p && me.padMode) [me selectTab:me.tab + 1]; };
    }
}
@end
