/* The in-game overlay: a compact translucent panel on the right edge with Video, Audio, Controls and Game tabs.
   Toggled with F1 or Cmd+,.  While it is visible game input reads as neutral (napple_input_set_blocked). */
#include <stdlib.h>
#include <string.h>
#include "fe_internal.h"

static NSArray<NSString *> *kScales, *kFps, *kAspect, *kRenderMode, *kSuper, *kUpscaler, *kFilter, *kAniso, *kFgMult, *kFgMethod, *kSpeedMode, *kAudioSpeed;
static const int kScaleVals[] = {25, 50, 75, 100, 150, 200, 300, 400};
static const int kFpsVals[] = {0, 30, 60, 90, 120, 240, 1000}; /* fps_cap values behind kFps */

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
static const int kSuperVals[] = {100, 150, 200, 300, 400};
static const int kAnisoVals[] = {0, 2, 4, 8, 16};
static const int kFgMultVals[] = {0, 2, 3, 4, 6, 8};
static NSView *video_page(NappleFE *fe) {
    napple_settings s = [fe currentSettings];
    NSStackView *p = page();
    p.spacing = 7;
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
    [p addArrangedSubview:heading(@"Render size")];
    [p addArrangedSubview:row(@"Render at", fe_popup(kRenderMode, s.render_mode, ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { t->render_mode = (int)b.indexOfSelectedItem; });
                                  fe_overlay_refresh(fe);
                              }))];
    if (s.render_mode == 1) {
        NSInteger si = index_of(kScaleVals, 8, s.internal_scale_percent, 3);
        [p addArrangedSubview:row(@"Scale", fe_popup(kScales, si, ^(NSPopUpButton *b) {
                                      update(fe, ^(napple_settings *t) { t->internal_scale_percent = kScaleVals[b.indexOfSelectedItem]; });
                                  }))];
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
    [p addArrangedSubview:row(@"Output filter", fe_popup(kFilter, s.filter, ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { t->filter = (int)b.indexOfSelectedItem; });
                              }))];
    [p addArrangedSubview:setting_slider(fe, @"Sharpness", 0, 100, s.sharpness, @"%.0f", ^(napple_settings *t, double v) { t->sharpness = (int)v; })];
    [p addArrangedSubview:row(@"Anisotropic", fe_popup(kAniso, index_of(kAnisoVals, 5, s.anisotropy, 0), ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { t->anisotropy = kAnisoVals[b.indexOfSelectedItem]; });
                              }))];
    [p addArrangedSubview:note(@"Render size changes rebuild the game's screen-sized buffers on the fly; if a game shows artifacts afterwards, restart it. "
                               @"Supersampling renders larger and filters down to the window.")];
    [p addArrangedSubview:heading(@"Frame rate")];
    NSInteger fi = index_of(kFpsVals, 7, s.fps_cap, 0);
    [p addArrangedSubview:row(@"Display rate", fe_popup(kFps, fi, ^(NSPopUpButton *b) {
                                  update(fe, ^(napple_settings *t) { t->fps_cap = kFpsVals[b.indexOfSelectedItem]; });
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
    [p addArrangedSubview:fe_check(@"Show statistics (F9)", s.show_stats,
                                   ^(NSButton *b) { update(fe, ^(napple_settings *t) { t->show_stats = b.state == NSControlStateValueOn; }); })];
    [p addArrangedSubview:fe_check(@"Show mouse cursor", s.show_touch_cursor,
                                   ^(NSButton *b) { update(fe, ^(napple_settings *t) { t->show_touch_cursor = b.state == NSControlStateValueOn; }); })];
    [p addArrangedSubview:
            note(@"Display rate: Game default keeps the game at its own speed (60 / 30 Hz as it asks). A fixed rate sets the rate the game is paced by, so its "
                 @"speed scales with it; use the Speed tab for turbo and slow motion instead. Frame generation shows several images per game frame without "
                 @"changing game speed; it adds about half a game frame of latency and can smear fast motion and text over moving scenes.")];
    return scrolled(p);
}
static NSView *speed_page(NappleFE *fe) {
    napple_settings s = [fe currentSettings];
    NSStackView *p = page();
    p.spacing = 7;
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
static NSView *audio_page(NappleFE *fe) {
    napple_settings s = [fe currentSettings];
    NSStackView *p = page();
    NSSlider *sl = [NSSlider sliderWithValue:s.master_volume minValue:0 maxValue:1 target:nil action:nil];
    [sl.widthAnchor constraintEqualToConstant:170].active = YES;
    fe_bind(sl, ^(NSSlider *x) {
        float v = (float)x.doubleValue;
        update(fe, ^(napple_settings *t) { t->master_volume = v; });
    });
    [p addArrangedSubview:row(@"Volume", sl)];
    [p addArrangedSubview:fe_check(@"Mute when in background", s.mute_in_background,
                                   ^(NSButton *b) { update(fe, ^(napple_settings *t) { t->mute_in_background = b.state == NSControlStateValueOn; }); })];
    return p;
}

/* ---- Controls ---- */
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
        napple_input_get_bindings(fe.input, &bd);
        BOOL cancel = is_key && code == 53; /* Esc cancels */
        if (!cancel) {
            if (is_key && b.kind == 0) bd.key[b.sw][b.slot] = code == 51 ? NAPPLE_KEY_NONE : (uint16_t)code; /* Delete clears */
            else if (is_key && b.kind == 2) (b.sw < 4 ? bd.stick_l : bd.stick_r)[b.sw & 3] = code == 51 ? NAPPLE_KEY_NONE : (uint16_t)code;
            else if (!is_key && b.kind == 1) bd.pad[b.sw][b.slot] = (int16_t)code;
            napple_input_set_bindings(fe.input, &bd);
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
    napple_input_get_bindings(fe.input, &t);
    mut(&t);
    if (napple_input_set_bindings(fe.input, &t) != 0) fe_overlay_refresh(fe); /* rejected (out of range): show the stored values again */
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
static NSView *controls_page(NappleFE *fe) {
    NSStackView *p = page();
    p.spacing = 7;
    p.translatesAutoresizingMaskIntoConstraints = NO;
    if (!fe.input) {
        [p addArrangedSubview:fe_label(@"Input is not attached.", 12, NO)];
        return p;
    }
    napple_bindings bd;
    napple_input_get_bindings(fe.input, &bd);
    napple_input_snapshot sn;
    napple_input_get_snapshot(fe.input, &sn);
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
    NSButton *reset = [NSButton buttonWithTitle:@"Reset to defaults" target:nil action:nil];
    reset.controlSize = NSControlSizeSmall;
    fe_bind(reset, ^(id x) {
        (void)x;
        napple_input_reset_bindings(fe.input);
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

/* ---- assembly ---- */
/* A segmented control and a content view instead of NSTabView: the selected NSTabView label rendered invisible in the vibrant dark appearance. */
static NSSegmentedControl *g_seg;
static NSView *g_content;
static NSInteger g_sel;
static void show_page(NappleFE *fe) {
    NSView *(*makers[])(NappleFE *) = {video_page, speed_page, audio_page, controls_page, game_page};
    for (NSView *v in [g_content.subviews copy]) [v removeFromSuperview];
    NSView *v = makers[g_sel](fe);
    v.frame = g_content.bounds;
    v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [g_content addSubview:v];
}
void fe_overlay_refresh(NappleFE *fe) {
    if (g_content) show_page(fe);
}
void fe_overlay_select_tab(int i) {
    if (g_seg && i >= 0 && i < 5) {
        g_sel = i;
        g_seg.selectedSegment = i;
    }
}

NSView *fe_build_overlay(NappleFE *fe) {
    kScales = @[ @"25%", @"50%", @"75%", @"100% (native)", @"150%", @"200%", @"300%", @"400%" ];
    kFps = @[ @"Game default (real time)", @"30 Hz", @"60 Hz", @"90 Hz", @"120 Hz", @"240 Hz", @"Uncapped" ];
    kAspect = @[
        @"Locked 16:9 (native)", @"Locked 16:9", @"Locked 4:3", @"Free: stretch the image", @"Free: keep aspect (bars)", @"Free: fill (crop)",
        @"Free: match window aspect"
    ];
    kRenderMode = @[ @"Native size", @"Scale factor", @"Follow window", @"Fixed size" ];
    kSuper = @[ @"Off (1x)", @"1.5x", @"2x", @"3x", @"4x" ];
    kUpscaler = @[ @"None (filter only)", @"MetalFX spatial (render lower, upscale)" ];
    kFilter = @[ @"Bilinear", @"Nearest", @"Box (area average)", @"Bicubic", @"Lanczos", @"Sharpened bicubic" ];
    kAniso = @[ @"As the game asks", @"2x", @"4x", @"8x", @"16x" ];
    kFgMult = @[ @"Automatic (fill the display)", @"2x", @"3x", @"4x", @"6x", @"8x" ];
    kFgMethod =
        @[ @"Best available", @"Optical flow (VideoToolbox)", @"Optical flow, low latency (VideoToolbox)", @"Motion-compensated (Metal)", @"Plain blend" ];
    kSpeedMode = @[ @"Game clock and vsync", @"Vsync only (frame skip)", @"Game clock only" ];
    kAudioSpeed = @[ @"Automatic", @"Always pitch shift", @"Silent when not 1x" ];
    NSVisualEffectView *v = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, 350, 600)];
    v.material = NSVisualEffectMaterialHUDWindow;
    v.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    v.state = NSVisualEffectStateActive;
    v.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
    v.autoresizingMask = NSViewHeightSizable | NSViewMinXMargin;
    g_seg = [NSSegmentedControl segmentedControlWithLabels:@[ @"Video", @"Speed", @"Audio", @"Controls", @"Game" ]
                                              trackingMode:NSSegmentSwitchTrackingSelectOne
                                                    target:nil
                                                    action:nil];
    g_seg.frame = NSMakeRect(12, 560, 326, 26);
    g_seg.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    g_seg.selectedSegment = g_sel;
    fe_bind(g_seg, ^(NSSegmentedControl *x) {
        g_sel = x.selectedSegment;
        show_page(fe);
    });
    [v addSubview:g_seg];
    g_content = [[NSView alloc] initWithFrame:NSMakeRect(4, 40, 342, 512)];
    g_content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [v addSubview:g_content];
    NSTextField *help = fe_label(@"F1 or Esc closes  -  F12 screenshot  -  F2 turbo  -  F9 stats", 10, NO);
    help.textColor = NSColor.secondaryLabelColor;
    help.frame = NSMakeRect(12, 14, 320, 14);
    help.autoresizingMask = NSViewMaxYMargin;
    [v addSubview:help];
    NSRect b = fe.gameView.bounds;
    v.frame = NSMakeRect(NSMaxX(b) - 350, 0, 350, b.size.height);
    show_page(fe);
    return v;
}
