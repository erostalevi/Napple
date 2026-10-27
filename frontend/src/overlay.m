/* The in-game overlay: a floating glass panel pinned to the top right (full height, with margins) (NSGlassEffectView on macOS 26, a vibrant material before) with four tabs:
   Audio/Video, FPS/Speed, Controls and Game/System. Toggled with F1, Cmd+, or the controller's Home / Guide button.
   Game input keeps flowing while the panel is open (settings.panel_passthrough); a panel opened with the controller takes the pad instead and is
   navigated with it (see FENav below). */
#include <stdlib.h>
#include <string.h>
#include "fe_internal.h"
#if __has_include(<MetalFX/MetalFX.h>)
#import <MetalFX/MetalFX.h>
#endif
#import <GameController/GameController.h>

static NSArray<NSString *> *kScales, *kFps, *kAspect, *kRenderMode, *kSuper, *kUpscaler, *kFilter, *kAniso, *kFgMult, *kFgMethod, *kSpeedMode, *kAudioSpeed;
static const int kScaleVals[] = {25, 50, 75, 100, 150, 200, 300, 400};

static NSStackView *page(void) {
    NSStackView *s = [NSStackView new];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 9;
    s.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
    return s;
}
static NSView *row(NSString *label, NSView *control) {
    NSTextField *l = fe_label(label, 12, NO);
    [l.widthAnchor constraintEqualToConstant:105].active = YES;
    if ([control isKindOfClass:NSPopUpButton.class]) [control.widthAnchor constraintLessThanOrEqualToConstant:250].active = YES; /* long titles must not push the label out */
    NSStackView *r = [NSStackView stackViewWithViews:@[ l, control ]];
    r.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    r.spacing = 8;
    return r;
}
static void update(NappleFE *fe, void (^mut)(napple_settings *)) {
    napple_settings s = [fe currentSettings];
    mut(&s);
    [fe applySettings:s save:YES notify:YES];
}

/* ---- Video / Speed / Audio ---- */
static NSButton *small_button(NSString *title, CGFloat w);
static NSView *scrolled(NSStackView *p) {
    p.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 330, 500)];
    sv.hasVerticalScroller = YES;
    sv.drawsBackground = NO;
    sv.autohidesScrollers = YES;
    sv.documentView = p;
    NSClipView *clip = sv.contentView;
    [NSLayoutConstraint activateConstraints:@[
        [p.topAnchor constraintEqualToAnchor:clip.topAnchor], [p.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
        [p.widthAnchor constraintEqualToAnchor:clip.widthAnchor]
    ]];
    dispatch_async(dispatch_get_main_queue(), ^{
        [sv layoutSubtreeIfNeeded];
        [p scrollPoint:NSMakePoint(0, p.frame.size.height)];
    }); /* start at the top */
    return sv;
}
static NSTextField *note(NSString *t) {
    NSTextField *n = fe_label(t, 10, NO);
    n.textColor = NSColor.secondaryLabelColor;
    n.lineBreakMode = NSLineBreakByWordWrapping;
    n.preferredMaxLayoutWidth = 300;
    return n;
}
static NSTextField *heading(NSString *t) { return fe_label(t, 12, YES); }
static NSInteger index_of(const int *vals, int n, int v, NSInteger fallback) {
    for (int i = 0; i < n; i++)
        if (vals[i] == v) return i;
    return fallback;
}
/* A slider with a live value label; `set` runs when the user releases it (continuous = NO) so the game is not rebuilt on every pixel of the drag. */
static NSView *setting_slider(NappleFE *fe, NSString *label, double lo, double hi, double val, NSString *fmt, void (^set)(napple_settings *, double)) {
    NSSlider *sl = [NSSlider sliderWithValue:val minValue:lo maxValue:hi target:nil action:nil];
    sl.continuous = NO;
    [sl.widthAnchor constraintEqualToConstant:140].active = YES;
    NSTextField *v = fe_label([NSString stringWithFormat:fmt, val], 11, NO);
    [v.widthAnchor constraintEqualToConstant:46].active = YES;
    fe_bind(sl, ^(NSSlider *x) {
        double f = x.doubleValue;
        v.stringValue = [NSString stringWithFormat:fmt, f];
        update(fe, ^(napple_settings *t) { set(t, f); });
    });
    NSStackView *r = [NSStackView stackViewWithViews:@[ sl, v ]];
    r.spacing = 6;
    return row(label, r);
}
static BOOL fe_metalfx_available(void) {
#if __has_include(<MetalFX/MetalFX.h>)
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    return d && [MTLFXSpatialScalerDescriptor supportsDevice:d];
#else
    return NO;
#endif
}
static const int kSuperVals[] = {100, 150, 200, 300, 400};
static const int kAnisoVals[] = {0, 2, 4, 8, 16};
static const int kFgMultVals[] = {0, 2, 3, 4, 6, 8};
static NSView *av_video_section(NappleFE *fe, NSStackView *p) {
    napple_settings s = [fe currentSettings];
    [p addArrangedSubview:heading(@"Window and picture shape")];
    [p addArrangedSubview:row(@"Window", fe_popup(kAspect, s.aspect,
                                                  ^(NSPopUpButton *b) { update(fe, ^(napple_settings *t) { t->aspect = (int)b.indexOfSelectedItem; }); }))];
    NSTextField *ow = [NSTextField textFieldWithString:@(s.output_width).stringValue], *oh = [NSTextField textFieldWithString:@(s.output_height).stringValue];
    for (NSTextField *f in @[ ow, oh ]) [f.widthAnchor constraintEqualToConstant:62].active = YES;
    void (^outsz)(id) = ^(id x) {
        (void)x;
        int a = ow.intValue, b = oh.intValue;
        update(fe, ^(napple_settings *t) {
            t->output_width = a;
            t->output_height = b;
        });
    };
    fe_bind(ow, outsz);
    fe_bind(oh, outsz);
    NSStackView *owh = [NSStackView stackViewWithViews:@[ ow, fe_label(@"x", 12, NO), oh ]];
    owh.spacing = 4;
    [p addArrangedSubview:row(@"Window size", owh)];
    [p addArrangedSubview:fe_check(@"Full screen", s.fullscreen,
                                   ^(NSButton *b) { update(fe, ^(napple_settings *t) { t->fullscreen = b.state == NSControlStateValueOn; }); })];
    [p addArrangedSubview:heading(@"Render target")];
    {   /* quick presets: 0.5x, 1x, 2x, 4x of the game's native render size */
        static const int pv[4] = {50, 100, 200, 400};
        NSMutableArray *bs = [NSMutableArray new];
        for (int i = 0; i < 4; i++) {
            int v = pv[i];
            NSButton *b = small_button(v == 50 ? @"0.5x" : [NSString stringWithFormat:@"%dx", v / 100], 52);
            BOOL on = s.render_mode == 1 && s.internal_scale_percent == v;
            b.bezelStyle = NSBezelStyleRounded;
            if (on) b.bezelColor = NSColor.controlAccentColor;
            fe_bind(b, ^(id x) {
                (void)x;
                update(fe, ^(napple_settings *t) { t->render_mode = 1; t->internal_scale_percent = v; });
                fe_overlay_refresh(fe);
            });
            [bs addObject:b];
        }
        /* output targets: render at 4K / 1440p whatever the game's native size is (fixed size mode) */
        static const int tw[2] = {3840, 2560}, th[2] = {2160, 1440};
        for (int i = 0; i < 2; i++) {
            int w = tw[i], h = th[i];
            NSButton *b = small_button(i == 0 ? @"4K" : @"1440p", 52);
            b.bezelStyle = NSBezelStyleRounded;
            if (s.render_mode == 3 && s.internal_width == w && s.internal_height == h) b.bezelColor = NSColor.controlAccentColor;
            fe_bind(b, ^(id x) {
                (void)x;
                update(fe, ^(napple_settings *t) { t->render_mode = 3; t->internal_width = w; t->internal_height = h; });
                fe_overlay_refresh(fe);
            });
            [bs addObject:b];
        }
        NSStackView *bv = [NSStackView stackViewWithViews:bs];
        bv.spacing = 5;
        [p addArrangedSubview:bv];
    }
    [p addArrangedSubview:row(@"Render at", fe_popup(kRenderMode, s.render_mode, ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { t->render_mode = (int)b.indexOfSelectedItem; });
                                  fe_overlay_refresh(fe);
                              }))];
    if (s.render_mode == 1) {
        NSInteger si = index_of(kScaleVals, 8, s.internal_scale_percent, 3);
        [p addArrangedSubview:row(@"Scale", fe_popup(kScales, si, ^(NSPopUpButton *b) {
                                      update(fe, ^(napple_settings *t) { t->internal_scale_percent = kScaleVals[b.indexOfSelectedItem]; });
                                      fe_overlay_refresh(fe);
                                  }))];
        /* any value from 10% to 800%, applied live (the game's screen-sized buffers are rebuilt on the fly) */
        NSTextField *cp = [NSTextField textFieldWithString:@(s.internal_scale_percent).stringValue];
        [cp.widthAnchor constraintEqualToConstant:62].active = YES;
        fe_bind(cp, ^(id x) {
            (void)x;
            int v = cp.intValue;
            update(fe, ^(napple_settings *t) { t->render_mode = 1; t->internal_scale_percent = v; });
        });
        NSStackView *cps = [NSStackView stackViewWithViews:@[ cp, fe_label(@"% (10-800)", 11, NO) ]];
        cps.spacing = 4;
        [p addArrangedSubview:row(@"Custom scale", cps)];
    } else if (s.render_mode == 3) {
        NSTextField *w = [NSTextField textFieldWithString:s.internal_width ? @(s.internal_width).stringValue : @""],
                    *h = [NSTextField textFieldWithString:s.internal_height ? @(s.internal_height).stringValue : @""];
        w.placeholderString = @"width";
        h.placeholderString = @"height";
        for (NSTextField *f in @[ w, h ]) [f.widthAnchor constraintEqualToConstant:62].active = YES;
        void (^custom)(id) = ^(id x) {
            (void)x;
            int cw = w.intValue, ch = h.intValue;
            update(fe, ^(napple_settings *t) {
                t->internal_width = cw;
                t->internal_height = ch;
            });
        };
        fe_bind(w, custom);
        fe_bind(h, custom);
        NSStackView *wh = [NSStackView stackViewWithViews:@[ w, fe_label(@"x", 12, NO), h ]];
        wh.spacing = 4;
        [p addArrangedSubview:row(@"Size", wh)];
    }
    [p addArrangedSubview:row(@"Supersampling", fe_popup(kSuper, index_of(kSuperVals, 5, s.supersample_percent, 0), ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { t->supersample_percent = kSuperVals[b.indexOfSelectedItem]; });
                              }))];
    [p addArrangedSubview:row(@"Upscaler", fe_popup(kUpscaler, s.upscaler,
                                                    ^(NSPopUpButton *b) { update(fe, ^(napple_settings *t) { t->upscaler = (int)b.indexOfSelectedItem; }); }))];
    if (s.upscaler == 1 && !fe_metalfx_available()) [p addArrangedSubview:note(@"MetalFX is not available on this Mac or build: the built-in upscaler is used instead. "
                                                                              @"(MetalFX temporal needs depth and motion vectors from the game; none of the supported titles provide them, so only the spatial scalers are offered.)")];
    [p addArrangedSubview:row(@"Output filter", fe_popup(kFilter, s.filter, ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { t->filter = (int)b.indexOfSelectedItem; });
                              }))];
    [p addArrangedSubview:setting_slider(fe, @"Sharpness", 0, 100, s.sharpness, @"%.0f", ^(napple_settings *t, double v) { t->sharpness = (int)v; })];
    [p addArrangedSubview:row(@"Anisotropic", fe_popup(kAniso, index_of(kAnisoVals, 5, s.anisotropy, 0), ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { t->anisotropy = kAnisoVals[b.indexOfSelectedItem]; });
                              }))];
    [p addArrangedSubview:note(@"Render size changes rebuild the game's screen-sized buffers on the fly; if a game shows artifacts afterwards, restart it. "
                               @"Supersampling renders larger and filters down to the window.")];
    return nil;
}
static NSView *av_stats_section(NappleFE *fe, NSStackView *p) {
    napple_settings s = [fe currentSettings];
    [p addArrangedSubview:heading(@"Statistics")];
    [p addArrangedSubview:fe_check(@"Show statistics (F9)", s.show_stats, ^(NSButton *b) { update(fe, ^(napple_settings *t) { t->show_stats = b.state == NSControlStateValueOn; }); })];
    [p addArrangedSubview:row(@"Corner", fe_popup(@[ @"Top left", @"Top right", @"Bottom left", @"Bottom right" ], s.stats_corner,
                                                  ^(NSPopUpButton *b) { update(fe, ^(napple_settings *t) { t->stats_corner = (int)b.indexOfSelectedItem; }); }))];
    [p addArrangedSubview:row(@"Text size", fe_popup(@[ @"Small", @"Medium", @"Large" ], s.stats_size,
                                                     ^(NSPopUpButton *b) { update(fe, ^(napple_settings *t) { t->stats_size = (int)b.indexOfSelectedItem; }); }))];
    [p addArrangedSubview:setting_slider(fe, @"Background", 10, 100, s.stats_opacity, @"%.0f%%", ^(napple_settings *t, double v) { t->stats_opacity = (int)v; })];
    [p addArrangedSubview:fe_check(@"Show Metal statistics (native Metal HUD)", s.metal_hud,
                                   ^(NSButton *b) { update(fe, ^(napple_settings *t) { t->metal_hud = b.state == NSControlStateValueOn; }); })];
    [p addArrangedSubview:note(@"The Metal HUD is Apple's own performance overlay: frame rate, GPU and frame time, memory and shader compilation, drawn by the system.")];
    return nil;
}
static NSView *av_page(NappleFE *fe) {
    NSStackView *p = page();
    p.spacing = 7;
    napple_settings s = [fe currentSettings];
    [p addArrangedSubview:heading(@"Audio")];
    NSSlider *sl = [NSSlider sliderWithValue:s.master_volume minValue:0 maxValue:1 target:nil action:nil];
    [sl.widthAnchor constraintEqualToConstant:170].active = YES;
    fe_bind(sl, ^(NSSlider *x) {
        float v = (float)x.doubleValue;
        update(fe, ^(napple_settings *t) { t->master_volume = v; });
    });
    [p addArrangedSubview:row(@"Volume", sl)];
    [p addArrangedSubview:fe_check(@"Play audio while in the background", !s.mute_in_background,
                                   ^(NSButton *b) { update(fe, ^(napple_settings *t) { t->mute_in_background = b.state != NSControlStateValueOn; }); })];
    av_video_section(fe, p);
    [p addArrangedSubview:fe_check(@"Show mouse cursor", s.show_touch_cursor,
                                   ^(NSButton *b) { update(fe, ^(napple_settings *t) { t->show_touch_cursor = b.state == NSControlStateValueOn; }); })];
    av_stats_section(fe, p);
    return scrolled(p);
}
static NSView *fps_page(NappleFE *fe) {
    napple_settings s = [fe currentSettings];
    NSStackView *p = page();
    p.spacing = 7;
    [p addArrangedSubview:heading(@"Frame rate")];
    [p addArrangedSubview:row(@"Present rate", fe_popup(kFps, napple_fps_choice(&s), ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { napple_fps_choice_apply(t, (int)b.indexOfSelectedItem); });
                              }))];
    [p addArrangedSubview:fe_check(@"VSync", s.vsync, ^(NSButton *b) { update(fe, ^(napple_settings *t) { t->vsync = b.state == NSControlStateValueOn; }); })];
    [p addArrangedSubview:fe_check(@"Frame generation (in-between frames, F8)", s.frame_gen,
                                   ^(NSButton *b) { update(fe, ^(napple_settings *t) { t->frame_gen = b.state == NSControlStateValueOn; }); })];
    [p addArrangedSubview:row(@"Images per frame", fe_popup(kFgMult, index_of(kFgMultVals, 6, s.frame_gen_mult, 0), ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { t->frame_gen_mult = kFgMultVals[b.indexOfSelectedItem]; });
                              }))];
    [p addArrangedSubview:row(@"Method", fe_popup(kFgMethod, s.frame_gen_method, ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { t->frame_gen_method = (int)b.indexOfSelectedItem; });
                              }))];
    [p addArrangedSubview:
            note(@"Present rate: how often your Mac shows a new image. It never changes game speed: the game always runs at its own cadence (60 / 30 Hz as it asks) "
                 @"and the display repeats the last frame in between, so any rate is safe; use the speed controls below for turbo and slow motion. Frame generation shows several images per game frame "
                 @"without changing game speed; it adds about half a game frame of latency and can smear fast motion and text over moving scenes.")];
    [p addArrangedSubview:heading(@"Game speed")];
    [p addArrangedSubview:setting_slider(fe, @"Speed", 10, 400, MIN(s.speed_percent, 400), @"%.0f%%",
                                         ^(napple_settings *t, double v) { t->speed_percent = (int)v; })];
    NSMutableArray *btns = [NSMutableArray new];
    for (NSNumber *n in @[ @25, @50, @100, @200, @400 ]) {
        int v = n.intValue;
        NSButton *b = small_button([NSString stringWithFormat:@"%.2gx", v / 100.0], 44);
        fe_bind(b, ^(id x) {
            (void)x;
            update(fe, ^(napple_settings *t) { t->speed_percent = v; });
            fe_overlay_refresh(fe);
        });
        [btns addObject:b];
    }
    NSStackView *bs = [NSStackView stackViewWithViews:btns];
    bs.spacing = 4;
    [p addArrangedSubview:bs];
    [p addArrangedSubview:setting_slider(fe, @"Turbo speed", 110, 1000, s.turbo_percent, @"%.0f%%",
                                         ^(napple_settings *t, double v) { t->turbo_percent = (int)v; })];
    [p addArrangedSubview:row(@"Mode", fe_popup(kSpeedMode, s.speed_mode,
                                                ^(NSPopUpButton *b) { update(fe, ^(napple_settings *t) { t->speed_mode = (int)b.indexOfSelectedItem; }); }))];
    [p addArrangedSubview:row(@"Audio", fe_popup(kAudioSpeed, s.audio_speed_mode, ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { t->audio_speed_mode = (int)b.indexOfSelectedItem; });
                              }))];
    [p addArrangedSubview:note(@"Game speed changes how fast game time passes without changing how often the picture is shown: every game clock, timer and the "
                               @"virtual vsync are scaled together, so animation, physics and cutscenes run faster or slower. 'Game clock and vsync' is the "
                               @"right mode for almost every game. 'Vsync only' (frame skip turbo) leaves the clocks real and only runs the vsync-stepped "
                               @"logic faster. 'Game clock only' scales clocks but not the vsync. Saving, loading and movies keep working; audio is "
                               @"pitch-shifted between 0.5x and 2x and silent outside that range (Audio setting).")];
    [p addArrangedSubview:heading(@"Hot keys")];
    NSTextField *keys =
        fe_label(@"F2 or `  hold: turbo\nF3  toggle turbo\nF4  toggle slow motion (0.5x)\nF5 or [  slower one step\nF6 or ]  faster one step\nF7 or \\  back "
                 @"to 1x\nF8  frame generation on / off\nF9  statistics overlay\nF10  supersampling 1x / 1.5x / 2x / 3x\nF1  this menu     F12  screenshot",
                 11, NO);
    keys.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    keys.maximumNumberOfLines = 0;
    [p addArrangedSubview:keys];
    [p addArrangedSubview:note(@"On a laptop keyboard the function keys may need the Fn key (or enable standard function keys in System Settings > Keyboard). "
                               @"The ` [ ] \\ keys always work.")];
    return scrolled(p);
}
/* ---- Controls ---- */
/* g_slot: -1 edits the shared bindings, 0..7 edits the profile of that player (created from the shared set on first edit). */
static int g_slot = -1;
static void bget(NappleFE *fe, napple_bindings *b) {
    if (g_slot >= 0) napple_input_get_player_bindings(fe.input, g_slot, b);
    else napple_input_get_bindings(fe.input, b);
}
static int bset(NappleFE *fe, const napple_bindings *b) {
    return g_slot >= 0 ? napple_input_set_player_bindings(fe.input, g_slot, b) : napple_input_set_bindings(fe.input, b);
}
/* Everything here edits napple_bindings (input.json of the game): keys per Switch button (two slots), the four keys of each stick, one
   controller element per button, dead zone / outer range / response curve / trigger point, axis inversion, npad style, console mode. */
@interface CaptureBox : NSObject
@property (weak) NappleFE *fe;
@property (weak) NSButton *button;
@property int sw, slot, kind; /* kind 0 keyboard key of a button, 1 controller element, 2 stick key (sw 0..7 = left L/U/R/D, right L/U/R/D) */
@end
@implementation CaptureBox
@end
static void capture_done(void *u, int is_key, int code) {
    CaptureBox *b = (__bridge_transfer CaptureBox *)u;
    dispatch_async(dispatch_get_main_queue(), ^{
        NappleFE *fe = b.fe;
        if (!fe.input) return;
        napple_bindings bd;
        bget(fe, &bd);
        BOOL cancel = is_key && code == 53; /* Esc cancels */
        if (!cancel) {
            if (is_key && b.kind == 0) bd.key[b.sw][b.slot] = code == 51 ? NAPPLE_KEY_NONE : (uint16_t)code; /* Delete clears */
            else if (is_key && b.kind == 2) (b.sw < 4 ? bd.stick_l : bd.stick_r)[b.sw & 3] = code == 51 ? NAPPLE_KEY_NONE : (uint16_t)code;
            else if (!is_key && b.kind == 1) bd.pad[b.sw][b.slot] = (int16_t)code;
            bset(fe, &bd);
        }
        fe_overlay_refresh(fe);
    });
}
static void begin_capture(NappleFE *fe, NSButton *btn, int kind, int sw, int slot) {
    btn.title = @"press...";
    CaptureBox *cb = [CaptureBox new];
    cb.fe = fe;
    cb.button = btn;
    cb.kind = kind;
    cb.sw = sw;
    cb.slot = slot;
    napple_input_capture_next(fe.input, capture_done, (void *)CFBridgingRetain(cb));
}
static NSButton *small_button(NSString *title, CGFloat w) {
    NSButton *b = [NSButton buttonWithTitle:title target:nil action:nil];
    b.controlSize = NSControlSizeSmall;
    b.font = [NSFont systemFontOfSize:11];
    [b.widthAnchor constraintEqualToConstant:w].active = YES;
    return b;
}
static void edit_bindings(NappleFE *fe, void (^mut)(napple_bindings *)) {
    napple_bindings t;
    bget(fe, &t);
    mut(&t);
    if (bset(fe, &t) != 0) fe_overlay_refresh(fe); /* rejected (out of range): show the stored values again */
}
static NSView *slider_row(NappleFE *fe, NSString *label, double lo, double hi, double val, NSString *fmt, void (^set)(napple_bindings *, float)) {
    NSSlider *sl = [NSSlider sliderWithValue:val minValue:lo maxValue:hi target:nil action:nil];
    sl.continuous = NO;
    [sl.widthAnchor constraintEqualToConstant:140].active = YES;
    NSTextField *v = fe_label([NSString stringWithFormat:fmt, val], 11, NO);
    [v.widthAnchor constraintEqualToConstant:38].active = YES;
    fe_bind(sl, ^(NSSlider *x) {
        float f = (float)x.doubleValue;
        v.stringValue = [NSString stringWithFormat:fmt, f];
        edit_bindings(fe, ^(napple_bindings *t) { set(t, f); });
    });
    NSStackView *r = [NSStackView stackViewWithViews:@[ sl, v ]];
    r.spacing = 6;
    return row(label, r);
}

/* ---- Controls: players list + live controller view (Ryujinx-style player tabs, one profile per player) ---- */
@interface FEPadView : NSView
@property(nonatomic) napple_input *input;
@property(nonatomic) int slot;
@end
@implementation FEPadView
- (BOOL)isFlipped { return YES; }
static void pad_dot(CGFloat x, CGFloat y, CGFloat r, BOOL on, NSColor *c) {
    NSBezierPath *b = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(x - r, y - r, 2 * r, 2 * r)];
    [(on ? c : [NSColor colorWithWhite:1 alpha:0.14]) setFill];
    [b fill];
}
static void pad_pill(NSRect r, BOOL on, NSColor *c) {
    NSBezierPath *b = [NSBezierPath bezierPathWithRoundedRect:r xRadius:r.size.height / 2 yRadius:r.size.height / 2];
    [(on ? c : [NSColor colorWithWhite:1 alpha:0.14]) setFill];
    [b fill];
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    napple_npad_state st = {0};
    int used = self.input ? napple_input_get_pad_state(self.input, self.slot, &st) : 0;
    NSColor *hot = NSColor.controlAccentColor;
    CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
    NSBezierPath *body = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(8, 18, w - 16, h - 26) xRadius:40 yRadius:40];
    [[NSColor colorWithWhite:1 alpha:used ? 0.08 : 0.04] setFill];
    [body fill];
    [[NSColor colorWithWhite:1 alpha:0.18] setStroke];
    body.lineWidth = 1;
    [body stroke];
#define BTN(b) ((st.buttons >> (b)) & 1)
    pad_pill(NSMakeRect(34, 4, 46, 12), BTN(NAPPLE_BTN_ZL), hot);
    pad_pill(NSMakeRect(w - 80, 4, 46, 12), BTN(NAPPLE_BTN_ZR), hot);
    pad_pill(NSMakeRect(34, 20, 56, 9), BTN(NAPPLE_BTN_L), hot);
    pad_pill(NSMakeRect(w - 90, 20, 56, 9), BTN(NAPPLE_BTN_R), hot);
    CGFloat lx = 62, ly = 62, fx = w - 66, fy = 70;   /* left stick, face buttons centre */
    pad_dot(lx, ly, 17, BTN(NAPPLE_BTN_STICK_L), hot);
    pad_dot(lx + st.left.x / 32767.0 * 9, ly - st.left.y / 32767.0 * 9, 8, YES, NSColor.whiteColor);
    pad_dot(fx, fy - 17, 8, BTN(NAPPLE_BTN_X), hot);
    pad_dot(fx, fy + 17, 8, BTN(NAPPLE_BTN_B), hot);
    pad_dot(fx - 17, fy, 8, BTN(NAPPLE_BTN_Y), hot);
    pad_dot(fx + 17, fy, 8, BTN(NAPPLE_BTN_A), hot);
    CGFloat dx = 96, dy = h - 36;   /* d-pad */
    pad_pill(NSMakeRect(dx - 6, dy - 18, 12, 36), BTN(NAPPLE_BTN_UP) || BTN(NAPPLE_BTN_DOWN), hot);
    pad_pill(NSMakeRect(dx - 18, dy - 6, 36, 12), BTN(NAPPLE_BTN_LEFT) || BTN(NAPPLE_BTN_RIGHT), hot);
    CGFloat rx = w - 100, ry = h - 36;
    pad_dot(rx, ry, 17, BTN(NAPPLE_BTN_STICK_R), hot);
    pad_dot(rx + st.right.x / 32767.0 * 9, ry - st.right.y / 32767.0 * 9, 8, YES, NSColor.whiteColor);
    pad_dot(w / 2 - 16, 50, 4, BTN(NAPPLE_BTN_MINUS), hot);
    pad_dot(w / 2 + 16, 50, 4, BTN(NAPPLE_BTN_PLUS), hot);
#undef BTN
    if (!used) {
        NSDictionary *at = @{NSFontAttributeName : [NSFont systemFontOfSize:11], NSForegroundColorAttributeName : NSColor.secondaryLabelColor};
        [@"Not in use by the game" drawAtPoint:NSMakePoint(w / 2 - 54, h / 2 - 6) withAttributes:at];
    }
}
@end
static NSTimer *g_pad_timer;
static FEPadView *g_pad_view;

static NSString *style_name(uint32_t st) {
    switch (st) {
        case 0: return @"-";
        default: return st & NAPPLE_STYLE_FULL_KEY ? @"Pro Controller" : st & NAPPLE_STYLE_HANDHELD ? @"Handheld" : st & NAPPLE_STYLE_JOY_DUAL ? @"Joy-Con pair" : @"Joy-Con";
    }
}
static NSView *players_section(NappleFE *fe, const napple_input_snapshot *sn) {
    NSMutableArray *rows = [NSMutableArray new];
    [rows addObject:heading(@"Players")];
    NSInteger cur = g_slot;
    for (int i = -1; i < 8; i++) {
        BOOL shared = i < 0;
        if (!shared && !sn->player_style[i] && i > 0 && sn->controllers <= i) continue; /* hide unused players beyond the connected controllers */
        napple_bindings tmp;
        int own = shared ? 0 : napple_input_get_player_bindings(fe.input, i, &tmp);
        NSString *who = shared ? @"All players (shared)" : [NSString stringWithFormat:@"Player %d  -  %@", i + 1,
                            i < sn->controllers ? @(sn->controller_names[i]) : (i == 0 ? @"Keyboard" : @"No controller")];
        NSString *sub = shared ? @"Default for players without their own profile" : [NSString stringWithFormat:@"%@  -  %@", style_name(sn->player_style[i]), own ? @"own profile" : @"shared profile"];
        NSButton *b = [NSButton radioButtonWithTitle:who target:nil action:nil];
        b.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
        b.state = cur == i ? NSControlStateValueOn : NSControlStateValueOff;
        int slot = i;
        fe_bind(b, ^(id x) {
            (void)x;
            g_slot = slot;
            fe_overlay_refresh(fe);
        });
        NSTextField *n = fe_label(sub, 10, NO);
        n.textColor = NSColor.secondaryLabelColor;
        NSStackView *c = [NSStackView stackViewWithViews:@[ b, n ]];
        c.orientation = NSUserInterfaceLayoutOrientationVertical;
        c.alignment = NSLayoutAttributeLeading;
        c.spacing = 0;
        [rows addObject:c];
    }
    g_pad_view = [[FEPadView alloc] initWithFrame:NSMakeRect(0, 0, 310, 120)];
    g_pad_view.input = fe.input;
    g_pad_view.slot = g_slot < 0 ? 0 : g_slot;
    [g_pad_view.widthAnchor constraintEqualToConstant:310].active = YES;
    [g_pad_view.heightAnchor constraintEqualToConstant:120].active = YES;
    [rows addObject:g_pad_view];
    if (g_slot >= 0) {
        napple_bindings tmp;
        if (!napple_input_get_player_bindings(fe.input, g_slot, &tmp)) {
            NSButton *mk = [NSButton buttonWithTitle:[NSString stringWithFormat:@"Give player %d its own profile", g_slot + 1] target:nil action:nil];
            mk.controlSize = NSControlSizeSmall;
            fe_bind(mk, ^(id x) {
                (void)x;
                napple_bindings cp;
                napple_input_get_player_bindings(fe.input, g_slot, &cp);   /* the shared set as the starting point */
                napple_input_set_player_bindings(fe.input, g_slot, &cp);
                fe_overlay_refresh(fe);
            });
            [rows addObject:mk];
        }
        [rows addObject:note(@"Each player can have different keys, controller buttons, sticks and triggers. Player 1 also reads the keyboard.")];
    }
    NSStackView *v = [NSStackView stackViewWithViews:rows];
    v.orientation = NSUserInterfaceLayoutOrientationVertical;
    v.alignment = NSLayoutAttributeLeading;
    v.spacing = 4;
    return v;
}

static NSView *controls_page(NappleFE *fe) {
    NSStackView *p = page();
    p.spacing = 7;
    p.translatesAutoresizingMaskIntoConstraints = NO;
    if (!fe.input) {
        [p addArrangedSubview:fe_label(@"Input is not attached.", 12, NO)];
        return p;
    }
    napple_bindings bd;
    bget(fe, &bd);
    napple_input_snapshot sn;
    napple_input_get_snapshot(fe.input, &sn);
    [p addArrangedSubview:players_section(fe, &sn)];
    NSTextField *pads = fe_label(
        sn.controllers ? [NSString stringWithFormat:@"Controllers: %d (%s%@)", sn.controllers, sn.controller_names[0], sn.controllers > 1 ? @", ..." : @""]
                       : @"Controllers: none (keyboard drives player 1)",
        11, YES);
    pads.lineBreakMode = NSLineBreakByTruncatingTail;
    [pads.widthAnchor constraintLessThanOrEqualToConstant:300].active = YES;
    [p addArrangedSubview:pads];
    NSTextField *hint =
        fe_label(@"Click a slot, then press a key / controller button. Delete clears, Esc cancels. Home / Guide = this menu, Share = screenshot.", 10, NO);
    hint.textColor = NSColor.secondaryLabelColor;
    hint.lineBreakMode = NSLineBreakByWordWrapping;
    hint.preferredMaxLayoutWidth = 300;
    [p addArrangedSubview:hint];

    NSMutableArray *hv = [NSMutableArray new];
    for (NSString *t in @[ @"", @"Key 1", @"Key 2", @"Pad" ]) {
        NSTextField *l = fe_label(t, 10, NO);
        l.alignment = NSTextAlignmentCenter;
        [l.widthAnchor constraintEqualToConstant:hv.count ? 56 : 60].active = YES;
        [hv addObject:l];
    }
    NSStackView *hdr = [NSStackView stackViewWithViews:hv];
    hdr.spacing = 5;
    [p addArrangedSubview:hdr];
    NSMutableArray *rows = [NSMutableArray new];
    for (int b = 0; b < NAPPLE_BTN_COUNT_BASIC; b++) {
        NSTextField *lab = fe_label(@(napple_button_name(b)), 12, YES);
        [lab.widthAnchor constraintEqualToConstant:60].active = YES;
        NSMutableArray *views = [NSMutableArray arrayWithObject:lab];
        for (int slot = 0; slot < 2; slot++) {
            NSButton *kb = small_button(@(napple_keycode_name(bd.key[b][slot])), 56);
            __weak NSButton *wk = kb;
            int sb = b, ss = slot;
            fe_bind(kb, ^(id x) {
                (void)x;
                begin_capture(fe, wk, 0, sb, ss);
            });
            [views addObject:kb];
        }
        NSButton *pb = small_button(bd.pad[b][0] >= 0 ? @(napple_pad_element_name(bd.pad[b][0])) : @"-", 56);
        __weak NSButton *wp = pb;
        int sb = b;
        fe_bind(pb, ^(id x) {
            (void)x;
            begin_capture(fe, wp, 1, sb, 0);
        });
        [views addObject:pb];
        NSStackView *r = [NSStackView stackViewWithViews:views];
        r.spacing = 5;
        [rows addObject:r];
    }
    static const char *const dirs[4] = {"Left", "Up", "Right", "Down"};
    for (int sw = 0; sw < 8; sw++) {
        NSTextField *lab = fe_label([NSString stringWithFormat:@"%s %s", sw < 4 ? "LS" : "RS", dirs[sw & 3]], 12, YES);
        [lab.widthAnchor constraintEqualToConstant:60].active = YES;
        uint16_t key = (sw < 4 ? bd.stick_l : bd.stick_r)[sw & 3];
        NSButton *kb = small_button(@(napple_keycode_name(key)), 56);
        __weak NSButton *wk = kb;
        int s2 = sw;
        fe_bind(kb, ^(id x) {
            (void)x;
            begin_capture(fe, wk, 2, s2, 0);
        });
        NSStackView *r = [NSStackView stackViewWithViews:@[ lab, kb ]];
        r.spacing = 5;
        [rows addObject:r];
    }
    NSStackView *grid = [NSStackView stackViewWithViews:rows];
    grid.orientation = NSUserInterfaceLayoutOrientationVertical;
    grid.alignment = NSLayoutAttributeLeading;
    grid.spacing = 3;
    [p addArrangedSubview:grid];

    [p addArrangedSubview:fe_label(@"Sticks and triggers", 12, YES)];
    [p addArrangedSubview:slider_row(fe, @"Dead zone", 0.0, 0.8, bd.stick_deadzone, @"%.2f",
                                     ^(napple_bindings *t, float v) { t->stick_deadzone = MIN(v, t->stick_range - 0.05f); })];
    [p addArrangedSubview:slider_row(fe, @"Outer range", 0.5, 1.0, bd.stick_range, @"%.2f", ^(napple_bindings *t, float v) {
           t->stick_range = v;
           if (t->stick_deadzone >= v) t->stick_deadzone = v - 0.05f;
       })];
    [p addArrangedSubview:slider_row(fe, @"Curve", 0.5, 3.0, bd.stick_curve, @"%.2f", ^(napple_bindings *t, float v) { t->stick_curve = v; })];
    [p addArrangedSubview:slider_row(fe, @"Trigger point", 0.05, 0.95, bd.trigger_threshold, @"%.2f",
                                     ^(napple_bindings *t, float v) { t->trigger_threshold = v; })];
    NSMutableArray *inv = [NSMutableArray new];
    static const char *const invn[4] = {"L-X", "L-Y", "R-X", "R-Y"};
    for (int i = 0; i < 4; i++) {
        int k = i;
        [inv addObject:fe_check([NSString stringWithUTF8String:invn[i]], bd.invert[i], ^(NSButton *bt) {
                 BOOL on = bt.state == NSControlStateValueOn;
                 edit_bindings(fe, ^(napple_bindings *t) { t->invert[k] = on; });
             })];
    }
    NSStackView *invrow = [NSStackView stackViewWithViews:inv];
    invrow.spacing = 6;
    [p addArrangedSubview:row(@"Invert", invrow)];
    [p addArrangedSubview:fe_label(@"Controller and console", 12, YES)];
    [p addArrangedSubview:row(@"Controller type", fe_popup(@[ @"Automatic", @"Pro Controller", @"Joy-Con pair", @"Handheld" ], bd.style, ^(NSPopUpButton *b) {
                                  NSInteger i = b.indexOfSelectedItem;
                                  edit_bindings(fe, ^(napple_bindings *t) { t->style = (int)i; });
                              }))];
    [p addArrangedSubview:row(@"Console mode", fe_popup(@[ @"Docked (1080p)", @"Handheld (720p)" ], bd.operation_mode, ^(NSPopUpButton *b) {
                                  NSInteger i = b.indexOfSelectedItem;
                                  edit_bindings(fe, ^(napple_bindings *t) { t->operation_mode = (int)i; });
                              }))];
    [p addArrangedSubview:row(@"Face buttons", fe_popup(@[ @"Automatic", @"By position", @"By label" ], bd.face_layout, ^(NSPopUpButton *b) {
                                  NSInteger i = b.indexOfSelectedItem;
                                  edit_bindings(fe, ^(napple_bindings *t) { t->face_layout = (int)i; });
                              }))];
    NSTextField *mn = fe_label(@"Controller type is read by the game when it starts; console mode applies on restart.", 10, NO);
    mn.textColor = NSColor.secondaryLabelColor;
    mn.lineBreakMode = NSLineBreakByWordWrapping;
    mn.preferredMaxLayoutWidth = 300;
    [p addArrangedSubview:mn];
    NSButton *reset = [NSButton buttonWithTitle:(g_slot >= 0 ? @"Use shared profile" : @"Reset to defaults") target:nil action:nil];
    reset.controlSize = NSControlSizeSmall;
    fe_bind(reset, ^(id x) {
        (void)x;
        if (g_slot >= 0) napple_input_clear_player_bindings(fe.input, g_slot);
        else napple_input_reset_bindings(fe.input);
        fe_overlay_refresh(fe);
    });
    NSStackView *m = [NSStackView stackViewWithViews:@[
        fe_check(@"Mouse = touch screen", bd.mouse_touch,
                 ^(NSButton *bt) {
                     BOOL on = bt.state == NSControlStateValueOn;
                     edit_bindings(fe, ^(napple_bindings *t) { t->mouse_touch = on; });
                 }),
        reset
    ]];
    m.spacing = 10;
    [p addArrangedSubview:m];

    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 330, 500)];
    sv.hasVerticalScroller = YES;
    sv.drawsBackground = NO;
    sv.autohidesScrollers = YES;
    sv.documentView = p;
    NSClipView *clip = sv.contentView;
    [NSLayoutConstraint activateConstraints:@[
        [p.topAnchor constraintEqualToAnchor:clip.topAnchor], [p.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
        [p.widthAnchor constraintEqualToAnchor:clip.widthAnchor]
    ]];
    dispatch_async(dispatch_get_main_queue(), ^{
        [sv layoutSubtreeIfNeeded];
        [p scrollPoint:NSMakePoint(0, p.frame.size.height)];
    }); /* start at the top */
    return sv;
}

/* ---- Game: DLC + update picker ---- */
static NSView *game_page(NappleFE *fe) {
    NSStackView *p = page();
    if (!fe.manifestTitle) {
        [p addArrangedSubview:fe_label(@"No game manifest loaded.", 12, NO)];
        return p;
    }
    napple_settings s = [fe currentSettings];
    [p addArrangedSubview:fe_label(fe.manifestTitle, 14, YES)];
    [p addArrangedSubview:fe_label([NSString stringWithFormat:@"Base version %@  -  %@", fe.manifestBase, fe.titleID], 10, NO)];
    NSMutableArray *items = [NSMutableArray arrayWithObject:@"Latest installed"];
    NSInteger sel = 0;
    for (NSDictionary *u in fe.updates) {
        [items addObject:[NSString stringWithFormat:@"%@%@", u[@"version"], [u[@"name"] length] ? [@"  " stringByAppendingString:u[@"name"]] : @""]];
        if (!strcmp(s.update_version, [u[@"version"] UTF8String])) sel = (NSInteger)items.count - 1;
    }
    NSPopUpButton *pop = fe_popup(items, sel, ^(NSPopUpButton *b) {
        NSInteger i = b.indexOfSelectedItem;
        napple_settings t = [fe currentSettings];
        if (i == 0) t.update_version[0] = 0;
        else strlcpy(t.update_version, [fe.updates[i - 1][@"version"] UTF8String], sizeof t.update_version);
        [fe applySettings:t save:YES notify:NO];
    });
    [p addArrangedSubview:row(@"Update", pop)];
    [p addArrangedSubview:fe_label(@"Downloadable content", 12, YES)];
    if (!fe.dlc.count) [p addArrangedSubview:fe_label(@"None installed.", 11, NO)];
    for (NSDictionary *d in fe.dlc) {
        NSString *id = d[@"id"];
        BOOL off = NO;
        for (int i = 0; i < s.disabled_dlc_count; i++)
            if (!strcasecmp(s.disabled_dlc[i], id.UTF8String)) off = YES;
        NSString *title =
            [d[@"name"] length] ? [NSString stringWithFormat:@"%@ (%@)", d[@"name"], [id substringFromIndex:MAX(0, (NSInteger)id.length - 4)]] : id;
        [p addArrangedSubview:fe_check(title, !off, ^(NSButton *b) {
               napple_settings t = [fe currentSettings];
               const char *cid = id.UTF8String;
               BOOL enable = b.state == NSControlStateValueOn;
               int k = -1;
               for (int i = 0; i < t.disabled_dlc_count; i++)
                   if (!strcasecmp(t.disabled_dlc[i], cid)) k = i;
               if (enable && k >= 0) {
                   for (int i = k; i + 1 < t.disabled_dlc_count; i++) memcpy(t.disabled_dlc[i], t.disabled_dlc[i + 1], 20);
                   t.disabled_dlc_count--;
               } else if (!enable && k < 0 && t.disabled_dlc_count < 32) strlcpy(t.disabled_dlc[t.disabled_dlc_count++], cid, 20);
               [fe applySettings:t save:YES notify:NO];
           })];
    }
    NSButton *go = [NSButton buttonWithTitle:@"Apply and restart game" target:nil action:nil];
    go.bezelStyle = NSBezelStyleRounded;
    go.keyEquivalent = @"";
    fe_bind(go, ^(id x) {
        (void)x;
        [fe showOverlay:NO];
        [fe restartGuest];
    });
    [p addArrangedSubview:go];
    NSTextField *n = fe_label(@"Changes to DLC and the update version take effect when the game restarts.", 10, NO);
    n.textColor = NSColor.secondaryLabelColor;
    n.lineBreakMode = NSLineBreakByWordWrapping;
    n.preferredMaxLayoutWidth = 290;
    [p addArrangedSubview:n];
    return p;
}

/* ---- Game / System: game info, DLC + update, Mii, amiibo, camera, mods and cheats ---- */
static NSString *mods_dir(NappleFE *fe, NSString *sub) {
    char path[1100];
    napple_data_path(fe.cfg.config_dir, fe.titleID.UTF8String, "x", path, sizeof path);
    NSString *base = [@(path) stringByDeletingLastPathComponent];
    return sub.length ? [base stringByAppendingPathComponent:sub] : base;
}
static NSMutableSet<NSString *> *enabled_set(NappleFE *fe) {
    NSData *d = [NSData dataWithContentsOfFile:[mods_dir(fe, nil) stringByAppendingPathComponent:@"mods-enabled.json"]];
    NSArray *a = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL] : nil;
    return [NSMutableSet setWithArray:[a isKindOfClass:NSArray.class] ? a : @[]];
}
static void save_enabled(NappleFE *fe, NSSet *set) {
    NSString *dir = mods_dir(fe, nil);
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
    NSData *d = [NSJSONSerialization dataWithJSONObject:[set.allObjects sortedArrayUsingSelector:@selector(compare:)] options:NSJSONWritingPrettyPrinted error:NULL];
    [d writeToFile:[dir stringByAppendingPathComponent:@"mods-enabled.json"] atomically:YES];
}
static void list_toggles(NappleFE *fe, NSStackView *p, NSString *sub, NSString *kind) {
    NSString *dir = mods_dir(fe, sub);
    NSArray *names = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:NULL] sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    NSMutableSet *on = enabled_set(fe);
    int n = 0;
    for (NSString *name in names) {
        if ([name hasPrefix:@"."]) continue;
        NSString *key = [sub stringByAppendingPathComponent:name];
        [p addArrangedSubview:fe_check(name.stringByDeletingPathExtension, [on containsObject:key], ^(NSButton *b) {
               NSMutableSet *e = enabled_set(fe);
               if (b.state == NSControlStateValueOn) [e addObject:key];
               else [e removeObject:key];
               save_enabled(fe, e);
           })];
        n++;
    }
    if (!n) [p addArrangedSubview:note([NSString stringWithFormat:@"No %@ found. Put them in\n%@", kind, dir])];
}
static NSView *system_page(NappleFE *fe) {
    NSStackView *p = page();
    p.spacing = 8;
    p.translatesAutoresizingMaskIntoConstraints = NO;
    [p addArrangedSubview:heading(@"Game")];
    NSView *g = game_page(fe);
    [p addArrangedSubview:g];

    [p addArrangedSubview:heading(@"Mii")];
    [p addArrangedSubview:note(@"Every game reads the same Mii database (Application Support/Napple/system/mii, or NAPPLE_MII_DIR). The Mii editor and picker run from "
                               @"the firmware you built the game with. Back up, export or delete Miis in Napple Studio > Settings > Mii.")];

    [p addArrangedSubview:heading(@"amiibo")];
    NSView *am = fe_amiibo_page(fe);
    [p addArrangedSubview:am];

    [p addArrangedSubview:heading(@"IR camera")];
    [p addArrangedSubview:fe_camera_page(fe)];

    [p addArrangedSubview:heading(@"Mods")];
    list_toggles(fe, p, @"mods", @"mods");
    [p addArrangedSubview:heading(@"Cheats")];
    list_toggles(fe, p, @"cheats", @"cheat files (.txt)");
    NSButton *open = [NSButton buttonWithTitle:@"Open mods and cheats folder" target:nil action:nil];
    open.controlSize = NSControlSizeSmall;
    fe_bind(open, ^(id x) {
        (void)x;
        NSString *d = mods_dir(fe, nil);
        for (NSString *sub in @[ @"mods", @"cheats" ]) [NSFileManager.defaultManager createDirectoryAtPath:[d stringByAppendingPathComponent:sub] withIntermediateDirectories:YES attributes:nil error:NULL];
        [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:d]];
    });
    [p addArrangedSubview:open];
    [p addArrangedSubview:note(@"Your choices are saved next to the game's settings (mods-enabled.json) and apply when the game restarts.")];
    return scrolled(p);
}

/* ---- the floating panel ---- */
static NSSegmentedControl *g_seg;
static NSView *g_content, *g_panel, *g_root;
static NSInteger g_sel;
static const CGFloat kPanelW = 400;

/* header strip (not draggable: the panel is pinned top right) */
@interface FEDragHeader : NSView
@end
@implementation FEDragHeader
- (BOOL)mouseDownCanMoveWindow { return NO; }
@end

static void show_page(NappleFE *fe) {
    NSView *(*makers[])(NappleFE *) = {av_page, fps_page, controls_page, system_page};
    for (NSView *v in [g_content.subviews copy]) [v removeFromSuperview];
    [g_pad_timer invalidate];
    g_pad_timer = nil;
    g_pad_view = nil;
    NSView *v = makers[g_sel](fe);
    v.frame = g_content.bounds;
    v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [g_content addSubview:v];
    if (g_pad_view) {
        __weak FEPadView *wv = g_pad_view;
        g_pad_timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *t) {
            (void)t;
            wv.needsDisplay = YES;
        }];
    }
    fe_nav_page_changed();
}
void fe_overlay_refresh(NappleFE *fe) {
    if (g_content) show_page(fe);
}
void fe_overlay_select_tab(int i) {
    if (g_seg && i >= 0 && i < 4) {
        g_sel = i;
        g_seg.selectedSegment = i;
    }
}
/* The panel is pinned to the top right and spans the full height of the game view, with a margin so it never touches the window edges.
   Called when it is shown and after every resize. */
void fe_overlay_place(NappleFE *fe) {
    if (!g_panel || !fe.gameView) return;
    const CGFloat margin = 16;
    NSRect b = fe.gameView.bounds;
    CGFloat w = MIN(kPanelW, MAX(240, b.size.width - 2 * margin)), h = MAX(240, b.size.height - 2 * margin);
    g_panel.frame = NSMakeRect(NSMaxX(b) - w - margin, NSMaxY(b) - h - margin, w, h);
}

NSView *fe_build_overlay(NappleFE *fe) {
    kScales = @[ @"25%", @"50%", @"75%", @"100% (native)", @"150%", @"200%", @"300%", @"400%" ];
    kFps = @[ @"Game pacing (real time)", @"Match display", @"30 Hz", @"60 Hz", @"90 Hz", @"120 Hz", @"144 Hz", @"240 Hz", @"Uncapped" ];
    kAspect = @[
        @"Locked 16:9 (native)", @"Locked 16:9", @"Locked 4:3", @"Free: stretch the image", @"Free: keep aspect (bars)", @"Free: fill (crop)",
        @"Free: match window aspect", @"Free: whole-number multiple (bars)"
    ];
    kRenderMode = @[ @"Native size", @"Scale factor", @"Follow window", @"Fixed size" ];
    kSuper = @[ @"Off (1x)", @"1.5x", @"2x", @"3x", @"4x" ];
    kUpscaler = @[ @"None (filter only)", @"MetalFX spatial (render lower, upscale)", @"Built-in edge adaptive (FSR-like)", @"CAS (resample + sharpen)" ];
    kFilter = @[ @"Bilinear", @"Nearest", @"Box (area average)", @"Bicubic", @"Lanczos", @"Sharpened bicubic" ];
    kAniso = @[ @"As the game asks", @"2x", @"4x", @"8x", @"16x" ];
    kFgMult = @[ @"Automatic (fill the display)", @"2x", @"3x", @"4x", @"6x", @"8x" ];
    kFgMethod =
        @[ @"Best available", @"Optical flow (VideoToolbox)", @"Optical flow, low latency (VideoToolbox)", @"Motion-compensated (Metal)", @"Plain blend" ];
    kSpeedMode = @[ @"Game clock and vsync", @"Vsync only (frame skip)", @"Game clock only" ];
    kAudioSpeed = @[ @"Automatic", @"Always pitch shift", @"Silent when not 1x" ];

    NSRect r0 = NSMakeRect(0, 0, kPanelW, 640);
    NSView *root = [[NSView alloc] initWithFrame:r0];
    root.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSView *panel = nil;
    if (@available(macOS 26.0, *)) {
        NSGlassEffectView *gv = [[NSGlassEffectView alloc] initWithFrame:r0];
        gv.cornerRadius = 26;
        gv.contentView = root;
        panel = gv;
    }
    if (!panel) {
        NSVisualEffectView *v = [[NSVisualEffectView alloc] initWithFrame:r0];
        v.material = NSVisualEffectMaterialHUDWindow;
        v.blendingMode = NSVisualEffectBlendingModeWithinWindow;
        v.state = NSVisualEffectStateActive;
        v.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
        v.wantsLayer = YES;
        v.layer.cornerRadius = 26;
        v.layer.masksToBounds = YES;
        [v addSubview:root];
        panel = v;
    }
    panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    panel.wantsLayer = YES;
    panel.shadow = [NSShadow new];
    panel.layer.shadowColor = NSColor.blackColor.CGColor;
    panel.layer.shadowOpacity = 0.35f;
    panel.layer.shadowRadius = 24;
    panel.layer.shadowOffset = CGSizeMake(0, -6);
    panel.layer.masksToBounds = NO;
    g_panel = panel;
    g_root = root;
    panel.autoresizingMask = NSViewNotSizable;

    /* header: drag handle, title, game-input switch, close */
    FEDragHeader *hdr = [[FEDragHeader alloc] initWithFrame:NSMakeRect(0, 640 - 44, kPanelW, 44)];
    hdr.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    NSTextField *title = fe_label(@"Napple", 14, YES);
    title.frame = NSMakeRect(18, 12, 120, 20);
    [hdr addSubview:title];
    NSButton *pass = fe_check(@"Game input stays on", [fe currentSettings].panel_passthrough, ^(NSButton *b) {
        update(fe, ^(napple_settings *t) { t->panel_passthrough = b.state == NSControlStateValueOn; });
        [fe refreshOverlayInputMode];
    });
    pass.controlSize = NSControlSizeSmall;
    pass.font = [NSFont systemFontOfSize:10];
    pass.frame = NSMakeRect(150, 12, 190, 20);
    [hdr addSubview:pass];
    NSButton *close = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"xmark.circle.fill" accessibilityDescription:@"Close"] target:nil action:nil];
    close.bordered = NO;
    close.frame = NSMakeRect(kPanelW - 38, 10, 24, 24);
    close.autoresizingMask = NSViewMinXMargin;
    fe_bind(close, ^(id x) {
        (void)x;
        [fe showOverlay:NO];
    });
    [hdr addSubview:close];
    [root addSubview:hdr];

    g_seg = [NSSegmentedControl segmentedControlWithLabels:@[ @"Audio/Video", @"FPS/Speed", @"Controls", @"Game/System" ]
                                              trackingMode:NSSegmentSwitchTrackingSelectOne
                                                    target:nil
                                                    action:nil];
    g_seg.controlSize = NSControlSizeSmall;
    g_seg.font = [NSFont systemFontOfSize:11];
    g_seg.frame = NSMakeRect(14, 640 - 44 - 30, kPanelW - 28, 24);
    g_seg.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    g_seg.selectedSegment = g_sel;
    fe_bind(g_seg, ^(NSSegmentedControl *x) {
        g_sel = x.selectedSegment;
        show_page(fe);
    });
    [root addSubview:g_seg];
    g_content = [[NSView alloc] initWithFrame:NSMakeRect(4, 34, kPanelW - 8, 640 - 44 - 30 - 40)];
    g_content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [root addSubview:g_content];
    NSTextField *help = fe_label(@"F1 / Esc / Home closes  -  F12 screenshot  -  F2 turbo  -  F9 stats", 10, NO);
    help.textColor = NSColor.secondaryLabelColor;
    help.frame = NSMakeRect(18, 10, kPanelW - 36, 14);
    help.autoresizingMask = NSViewMaxYMargin;
    [root addSubview:help];
    panel.frame = r0;
    show_page(fe);
    return panel;
}

/* ---- controller navigation of the panel (GameController; independent of the game's input) ---- */
static NSMutableArray<NSControl *> *g_focus;
static NSInteger g_focus_i;
static NSView *g_ring;
static __weak NappleFE *g_nav_fe;
static NSTimer *g_rep;

static void collect(NSView *v, NSMutableArray *out) {
    if (v.hidden) return;
    if ([v isKindOfClass:NSControl.class] && ![v isKindOfClass:NSTextField.class] && ![v isKindOfClass:NSSegmentedControl.class] && ((NSControl *)v).enabled && v.frame.size.width > 8 &&
        ![v isKindOfClass:NSScroller.class] && ![v isKindOfClass:NSTableView.class])
        [out addObject:(NSControl *)v];
    for (NSView *s in v.subviews) collect(s, out);
}
static void show_ring(void) {
    [g_ring removeFromSuperview];
    g_ring = nil;
    if (!g_focus.count || !g_nav_fe.overlayPadMode) return;
    g_focus_i = MIN(MAX(g_focus_i, 0), (NSInteger)g_focus.count - 1);
    NSControl *c = g_focus[(NSUInteger)g_focus_i];
    [c scrollRectToVisible:c.bounds];
    NSView *ring = [[NSView alloc] initWithFrame:NSInsetRect(c.bounds, -3, -2)];
    ring.wantsLayer = YES;
    ring.layer.borderColor = NSColor.controlAccentColor.CGColor;
    ring.layer.borderWidth = 2;
    ring.layer.cornerRadius = 8;
    [c addSubview:ring];
    g_ring = ring;
}
void fe_nav_page_changed(void) {
    g_focus = [NSMutableArray new];
    collect(g_content, g_focus);
    g_focus_i = 0;
    dispatch_async(dispatch_get_main_queue(), ^{ show_ring(); });
}
static void nav_move(int d) {
    if (!g_focus.count) return;
    g_focus_i = (g_focus_i + d + (NSInteger)g_focus.count) % (NSInteger)g_focus.count;
    show_ring();
}
static void nav_adjust(int d) {
    if (!g_focus.count) return;
    NSControl *c = g_focus[(NSUInteger)MIN(MAX(g_focus_i, 0), (NSInteger)g_focus.count - 1)];
    if ([c isKindOfClass:NSSlider.class]) {
        NSSlider *s = (NSSlider *)c;
        s.doubleValue = MIN(s.maxValue, MAX(s.minValue, s.doubleValue + d * (s.maxValue - s.minValue) / 20));
        [s sendAction:s.action to:s.target];
    } else if ([c isKindOfClass:NSPopUpButton.class]) {
        NSPopUpButton *p = (NSPopUpButton *)c;
        NSInteger n = p.numberOfItems, i = (p.indexOfSelectedItem + d + n) % n;
        [p selectItemAtIndex:i];
        [p sendAction:p.action to:p.target];
    } else if ([c isKindOfClass:NSButton.class] && ((NSButton *)c).bezelStyle == NSBezelStyleRounded && d != 0 && ((NSButton *)c).title.length <= 5 && ((NSButton *)c).title.length > 0) {
        /* quick-preset rows (0.5x 1x 2x 4x): left / right moves along the row */
        nav_move(d);
    }
}
static void nav_activate(void) {
    if (!g_focus.count) return;
    NSControl *c = g_focus[(NSUInteger)MIN(MAX(g_focus_i, 0), (NSInteger)g_focus.count - 1)];
    if ([c isKindOfClass:NSPopUpButton.class]) nav_adjust(1);
    else if ([c isKindOfClass:NSButton.class]) [(NSButton *)c performClick:nil];
}
static void nav_tab(int d) {
    NSInteger n = (g_sel + d + 4) % 4;
    g_sel = n;
    g_seg.selectedSegment = n;
    show_page(g_nav_fe);
}
/* hold-to-repeat for the d-pad */
static void nav_repeat(void (^act)(void)) {
    [g_rep invalidate];
    act();
    g_rep = [NSTimer scheduledTimerWithTimeInterval:0.22 repeats:YES block:^(NSTimer *t) {
        (void)t;
        act();
    }];
}
static void nav_stop(void) {
    [g_rep invalidate];
    g_rep = nil;
}
static void nav_attach(GCController *c) {
    GCExtendedGamepad *g = c.extendedGamepad;
    if (!g) return;
    void (^dir)(GCControllerButtonInput *, float, BOOL, void (^)(void)) = ^(GCControllerButtonInput *b, float v, BOOL p, void (^act)(void)) {
        (void)b;
        (void)v;
        if (!g_nav_fe.overlayPadMode) return;
        if (p) nav_repeat(act);
        else nav_stop();
    };
    g.dpad.up.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { dir(b, v, p, ^{ nav_move(-1); }); };
    g.dpad.down.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { dir(b, v, p, ^{ nav_move(1); }); };
    g.dpad.left.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { dir(b, v, p, ^{ nav_adjust(-1); }); };
    g.dpad.right.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { dir(b, v, p, ^{ nav_adjust(1); }); };
    g.buttonA.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) {
        (void)b;
        (void)v;
        if (p && g_nav_fe.overlayPadMode) nav_activate();
    };
    g.buttonB.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) {
        (void)b;
        (void)v;
        if (p && g_nav_fe.overlayPadMode) [g_nav_fe showOverlay:NO];
    };
    g.leftShoulder.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) {
        (void)b;
        (void)v;
        if (p && g_nav_fe.overlayPadMode) nav_tab(-1);
    };
    g.rightShoulder.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) {
        (void)b;
        (void)v;
        if (p && g_nav_fe.overlayPadMode) nav_tab(1);
    };
    g.leftThumbstick.yAxis.valueChangedHandler = ^(GCControllerAxisInput *a, float v) {
        (void)a;
        (void)v; /* the sticks scroll the page */
    };
}
void fe_nav_start(NappleFE *fe) {
    g_nav_fe = fe;
    for (GCController *c in GCController.controllers) nav_attach(c);
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification
                                                    object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification *n) { nav_attach(n.object); }];
}
/* The panel was shown or hidden (or switched between pad and mouse mode): ring on / off. */
void fe_nav_mode_changed(void) {
    nav_stop();
    fe_nav_page_changed();
}
