/* The in-game overlay shell: a floating glass panel pinned to the top right (NSGlassEffectView on macOS 26, a vibrant material before) with five
   tabs: Audio/Video, FPS/Speed, Controls, Game/System and Status.  The pages themselves live in overlay_pages.m and overlay_controls.m and are
   built from fe_ui.h; this file owns the panel chrome, tab switching, animation, live-label ticks and the controller navigation.
   Toggled with F1, Cmd+, or the controller's Home / Guide button; Cmd+1..5 pick a tab, Cmd+0 moves keyboard focus into the panel.
   Game input keeps flowing while the panel is open (settings.panel_passthrough); a panel opened with the controller takes the pad instead and
   is navigated with it (see the navigation section below). */
#include <stdlib.h>
#include <string.h>
#include "fe_internal.h"
#include "fe_ui.h"
#include "overlay_logic.h"
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>

void fe_overlay_update(NappleFE *fe, void (^mut)(napple_settings *)) {
    napple_settings s = [fe currentSettings];
    mut(&s);
    [fe applySettings:s save:YES notify:YES];
}

/* ---- the floating panel ---- */
static NSSegmentedControl *g_seg;
static NSView *g_content, *g_panel, *g_root;
static NSTextField *g_title, *g_sub;
static NSImageView *g_icon;
static NSInteger g_sel;
static NSTimer *g_tick_timer;
static __weak NappleFE *g_nav_fe;

static NSString *const kTabTitles[FE_TAB_COUNT] = {@"Audio and Video", @"FPS and Speed", @"Controls", @"Game and System", @"Status"};
static NSString *const kTabTips[FE_TAB_COUNT] = {@"Audio and Video  (Cmd 1)", @"FPS and Speed  (Cmd 2)", @"Controls  (Cmd 3)", @"Game and System  (Cmd 4)", @"Status  (Cmd 5)"};
static NSString *const kTabSymbols[FE_TAB_COUNT] = {@"display", @"gauge.with.dots.needle.67percent", @"gamecontroller", @"gearshape", @"waveform.path.ecg"};

/* The panel's root: Esc and F1 close it even when a control inside has the keyboard focus. */
@interface FEPanelRoot : NSView
@end
@implementation FEPanelRoot
- (void)keyDown:(NSEvent *)e {
    if ((e.keyCode == 53 || e.keyCode == 122) && !e.isARepeat) {
        [g_nav_fe showOverlay:NO];
        return;
    }
    [super keyDown:e];
}
- (void)cancelOperation:(id)sender {
    (void)sender;
    [g_nav_fe showOverlay:NO];
}
@end

void fe_overlay_set_tick(NSTimeInterval period, void (^tick)(void)) {
    [g_tick_timer invalidate];
    g_tick_timer = nil;
    if (!tick) return;
    g_tick_timer = [NSTimer scheduledTimerWithTimeInterval:period repeats:YES block:^(NSTimer *t) {
        (void)t;
        tick();
    }];
}

static void show_page(NappleFE *fe, BOOL animate) {
    NSView *(*makers[FE_TAB_COUNT])(NappleFE *) = {fe_page_video, fe_page_speed, fe_page_controls, fe_page_game, fe_page_status};
    CGFloat keep_y = -1;
    NSScrollView *old = [g_content.subviews.firstObject isKindOfClass:NSScrollView.class] ? g_content.subviews.firstObject : nil;
    if (old && !animate) keep_y = old.contentView.bounds.origin.y;
    fe_overlay_set_tick(0, nil);
    for (NSView *v in [g_content.subviews copy]) [v removeFromSuperview];
    NSView *v = makers[MIN(MAX(g_sel, 0), FE_TAB_COUNT - 1)](fe);
    v.translatesAutoresizingMaskIntoConstraints = NO;
    [g_content addSubview:v];
    [NSLayoutConstraint activateConstraints:@[
        [v.leadingAnchor constraintEqualToAnchor:g_content.leadingAnchor], [v.trailingAnchor constraintEqualToAnchor:g_content.trailingAnchor],
        [v.topAnchor constraintEqualToAnchor:g_content.topAnchor], [v.bottomAnchor constraintEqualToAnchor:g_content.bottomAnchor]
    ]];
    if (keep_y > 0 && [v isKindOfClass:NSScrollView.class]) {
        NSScrollView *sv = (NSScrollView *)v;
        [g_content layoutSubtreeIfNeeded];
        CGFloat maxy = MAX(0, sv.documentView.frame.size.height - sv.contentView.bounds.size.height);
        [sv.contentView scrollToPoint:NSMakePoint(0, MIN(keep_y, maxy))];
        [sv reflectScrolledClipView:sv.contentView];
    }
    if (animate && !fe_reduce_motion()) {
        v.alphaValue = 0;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *c) {
            c.duration = 0.16;
            v.animator.alphaValue = 1;
        }];
    }
    g_title.stringValue = kTabTitles[MIN(MAX(g_sel, 0), FE_TAB_COUNT - 1)];
    g_icon.image = fe_symbol(kTabSymbols[MIN(MAX(g_sel, 0), FE_TAB_COUNT - 1)], 17, NSFontWeightSemibold);
    g_sub.stringValue = fe.manifestTitle ?: fe.titleName ?: @"Napple";
    g_seg.selectedSegment = g_sel;
    fe_nav_page_changed();
}
void fe_overlay_refresh(NappleFE *fe) {
    if (g_content && fe.overlayVisible) show_page(fe, NO);
}
void fe_overlay_select_tab(int i) {
    if (i >= 0 && i < FE_TAB_COUNT) {
        g_sel = i;
        g_seg.selectedSegment = i;
    }
}
void fe_overlay_goto_tab(NappleFE *fe, int i) {
    if (i < 0 || i >= FE_TAB_COUNT) return;
    g_sel = i;
    if (!fe.overlayVisible) [fe showOverlay:YES];
    else show_page(fe, YES);
}
void fe_overlay_focus_controls(NappleFE *fe) {
    if (!fe.overlayVisible) [fe showOverlay:YES];
    NSScrollView *sv = [g_content.subviews.firstObject isKindOfClass:NSScrollView.class] ? g_content.subviews.firstObject : nil;
    NSMutableArray *a = [NSMutableArray new];
    fe_collect_focusable(sv ?: g_content, a);
    if (a.count) [fe.window makeFirstResponder:a.firstObject];
}
/* The panel is pinned to the top right and spans the full height of the game view, with a margin so it never touches the window edges.
   Called when it is shown and after every resize. */
static NSRect panel_frame(NappleFE *fe) {
    const CGFloat margin = 16;
    NSRect b = fe.gameView.bounds;
    CGFloat w = MIN(FE_PANEL_W, MAX(300, b.size.width - 2 * margin)), h = MAX(300, b.size.height - 2 * margin);
    return NSMakeRect(NSMaxX(b) - w - margin, NSMaxY(b) - h - margin, w, h);
}
void fe_overlay_place(NappleFE *fe) {
    if (!g_panel || !fe.gameView) return;
    g_panel.frame = panel_frame(fe);
}
void fe_overlay_set_visible(NappleFE *fe, BOOL show) {
    NSView *p = g_panel;
    if (!p) return;
    BOOL anim = !fe_reduce_motion() && fe.window.isVisible;
    if (show) {
        fe_overlay_place(fe);
        p.hidden = NO;
        fe_overlay_refresh(fe);
        if (anim) {
            NSRect target = p.frame;
            p.alphaValue = 0;
            p.frame = NSOffsetRect(target, 26, 0);
            [NSAnimationContext runAnimationGroup:^(NSAnimationContext *c) {
                c.duration = 0.2;
                c.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
                p.animator.alphaValue = 1;
                p.animator.frame = target;
            }];
        } else p.alphaValue = 1;
    } else {
        fe_overlay_set_tick(0, nil);
        if (!anim) {
            p.hidden = YES;
            return;
        }
        NSRect from = p.frame;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *c) {
            c.duration = 0.14;
            c.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseIn];
            p.animator.alphaValue = 0;
            p.animator.frame = NSOffsetRect(from, 20, 0);
        } completionHandler:^{
            if (!fe.overlayVisible) {
                p.hidden = YES;
                p.alphaValue = 1;
                fe_overlay_place(fe);
            }
        }];
    }
}

NSView *fe_build_overlay(NappleFE *fe) {
    g_nav_fe = fe;
    if (getenv("NAPPLE_FE_APPEARANCE")) { /* test aid: light / dark */
        BOOL dark = !strcmp(getenv("NAPPLE_FE_APPEARANCE"), "dark");
        NSApp.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    }
    NSRect r0 = NSMakeRect(0, 0, FE_PANEL_W, 640);
    FEPanelRoot *root = [[FEPanelRoot alloc] initWithFrame:r0];
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
        v.wantsLayer = YES;
        v.layer.cornerRadius = 26;
        v.layer.cornerCurve = kCACornerCurveContinuous;
        v.layer.masksToBounds = YES;
        [v addSubview:root];
        panel = v;
    }
    panel.wantsLayer = YES;
    panel.layer.shadowColor = NSColor.blackColor.CGColor;
    panel.layer.shadowOpacity = 0.3f;
    panel.layer.shadowRadius = 24;
    panel.layer.shadowOffset = CGSizeMake(0, -6);
    panel.layer.masksToBounds = NO;
    g_panel = panel;
    g_root = root;
    panel.autoresizingMask = NSViewNotSizable;
    panel.accessibilityLabel = @"Napple settings";

    /* header: tab icon + title / game name, close */
    g_icon = [NSImageView imageViewWithImage:fe_symbol(kTabSymbols[0], 17, NSFontWeightSemibold) ?: [NSImage new]];
    g_icon.contentTintColor = NSColor.controlAccentColor;
    g_icon.translatesAutoresizingMaskIntoConstraints = NO;
    [g_icon.widthAnchor constraintEqualToConstant:24].active = YES;
    g_title = fe_text(kTabTitles[0], FETypeTitle, nil);
    g_sub = fe_text(fe.manifestTitle ?: fe.titleName ?: @"Napple", FETypeCaption, NSColor.secondaryLabelColor);
    NSStackView *tcol = fe_vstack(0, @[ g_title, g_sub ]);
    tcol.alignment = NSLayoutAttributeLeading;
    NSImage *xi = fe_symbol(@"xmark.circle.fill", 18, NSFontWeightRegular);
    NSButton *close = [NSButton buttonWithImage:xi ?: [NSImage new] target:nil action:nil];
    close.bordered = NO;
    close.contentTintColor = NSColor.tertiaryLabelColor;
    close.toolTip = @"Close  (F1 or Esc)";
    close.accessibilityLabel = @"Close";
    fe_bind(close, ^(id x) {
        (void)x;
        [fe showOverlay:NO];
    });
    NSView *grow = [NSView new];
    [grow setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *hdr = fe_hstack(8, @[ g_icon, tcol, grow, close ]);
    hdr.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:hdr];

    NSMutableArray *imgs = [NSMutableArray new];
    for (int i = 0; i < FE_TAB_COUNT; i++) {
        NSImage *im = [NSImage imageWithSystemSymbolName:kTabSymbols[i] accessibilityDescription:kTabTitles[i]];
        [imgs addObject:[im imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:13 weight:NSFontWeightMedium]] ?: [NSImage new]];
    }
    g_seg = [NSSegmentedControl segmentedControlWithImages:imgs trackingMode:NSSegmentSwitchTrackingSelectOne target:nil action:nil];
    g_seg.segmentDistribution = NSSegmentDistributionFillEqually;
    g_seg.selectedSegment = g_sel;
    for (int i = 0; i < FE_TAB_COUNT; i++) [g_seg setToolTip:kTabTips[i] forSegment:i];
    g_seg.translatesAutoresizingMaskIntoConstraints = NO;
    fe_bind(g_seg, ^(NSSegmentedControl *x) {
        g_sel = x.selectedSegment;
        show_page(fe, YES);
    });
    [root addSubview:g_seg];

    g_content = [NSView new];
    g_content.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:g_content];

    NSTextField *help = fe_text(@"F1 / Esc close    Cmd 1-5 tabs    Cmd 0 focus    F9 HUD    F12 screenshot", FETypeCaption, NSColor.tertiaryLabelColor);
    help.translatesAutoresizingMaskIntoConstraints = NO;
    help.alignment = NSTextAlignmentCenter;
    [root addSubview:help];

    [NSLayoutConstraint activateConstraints:@[
        [hdr.topAnchor constraintEqualToAnchor:root.topAnchor constant:16], [hdr.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:18],
        [hdr.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-16],
        [g_seg.topAnchor constraintEqualToAnchor:hdr.bottomAnchor constant:12], [g_seg.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:FE_PAGE_INSET],
        [g_seg.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-FE_PAGE_INSET],
        [g_content.topAnchor constraintEqualToAnchor:g_seg.bottomAnchor constant:10], [g_content.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [g_content.trailingAnchor constraintEqualToAnchor:root.trailingAnchor], [g_content.bottomAnchor constraintEqualToAnchor:help.topAnchor constant:-6],
        [help.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:14], [help.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-14],
        [help.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-12]
    ]];
    panel.frame = r0;
    return panel;
}

/* ============================================================ controller / keyboard navigation of the panel ============================================================
   GameController, independent of the game's input.  D-pad up / down move a focus ring over the controls, left / right adjust sliders, pop-ups, segmented rows
   and move along button rows, A activates, B closes, L / R switch tabs. */
static NSMutableArray<NSControl *> *g_focus;
static NSInteger g_focus_i;
static NSView *g_ring;
static NSTimer *g_rep;

void fe_collect_focusable(NSView *v, NSMutableArray *out) {
    if (v.hidden) return;
    BOOL ctl = [v isKindOfClass:NSControl.class] && ![v isKindOfClass:NSTextField.class] && ![v isKindOfClass:NSScroller.class] && ![v isKindOfClass:NSTableView.class] &&
               ![v isKindOfClass:NSImageView.class] && v != g_seg && ((NSControl *)v).enabled && v.frame.size.width > 8;
    if (ctl) [out addObject:v];
    for (NSView *s in v.subviews) fe_collect_focusable(s, out);
}
static void show_ring(void) {
    [g_ring removeFromSuperview];
    g_ring = nil;
    if (!g_focus.count || !g_nav_fe.overlayPadMode) return;
    g_focus_i = MIN(MAX(g_focus_i, 0), (NSInteger)g_focus.count - 1);
    NSControl *c = g_focus[(NSUInteger)g_focus_i];
    [c scrollRectToVisible:NSInsetRect(c.bounds, 0, -24)];
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
    fe_collect_focusable(g_content, g_focus);
    g_focus_i = 0;
    dispatch_async(dispatch_get_main_queue(), ^{ show_ring(); });
}
static void nav_move(int d) {
    if (!g_focus.count) return;
    g_focus_i = (g_focus_i + d + (NSInteger)g_focus.count) % (NSInteger)g_focus.count;
    show_ring();
}
static NSControl *cur_control(void) {
    return g_focus.count ? g_focus[(NSUInteger)MIN(MAX(g_focus_i, 0), (NSInteger)g_focus.count - 1)] : nil;
}
static void nav_adjust(int d) {
    NSControl *c = cur_control();
    if (!c) return;
    if ([c isKindOfClass:NSSlider.class]) {
        NSSlider *s = (NSSlider *)c;
        s.doubleValue = MIN(s.maxValue, MAX(s.minValue, s.doubleValue + d * (s.maxValue - s.minValue) / 20));
        [s sendAction:s.action to:s.target];
    } else if ([c isKindOfClass:NSPopUpButton.class]) {
        NSPopUpButton *p = (NSPopUpButton *)c;
        NSInteger n = p.numberOfItems, i = (p.indexOfSelectedItem + d + n) % n;
        [p selectItemAtIndex:i];
        [p sendAction:p.action to:p.target];
    } else if ([c isKindOfClass:NSSegmentedControl.class]) {
        NSSegmentedControl *s = (NSSegmentedControl *)c;
        NSInteger i = fe_tab_wrap((int)MAX(s.selectedSegment, 0), d, (int)s.segmentCount);
        s.selectedSegment = i;
        [s sendAction:s.action to:s.target];
    } else if ([c isKindOfClass:NSSwitch.class]) {
        NSSwitch *w = (NSSwitch *)c;
        w.state = d > 0 ? NSControlStateValueOn : NSControlStateValueOff;
        [w sendAction:w.action to:w.target];
    } else if ([c isKindOfClass:NSButton.class] && d != 0) {
        /* rows of small buttons (key slots, confirm / cancel): left / right moves along the row */
        NSInteger j = g_focus_i + d;
        if (j >= 0 && j < (NSInteger)g_focus.count && g_focus[(NSUInteger)j].superview == c.superview) nav_move(d);
    }
}
static void nav_activate(void) {
    NSControl *c = cur_control();
    if (!c) return;
    if ([c isKindOfClass:NSPopUpButton.class]) nav_adjust(1);
    else if ([c isKindOfClass:NSSwitch.class]) {
        NSSwitch *w = (NSSwitch *)c;
        w.state = w.state == NSControlStateValueOn ? NSControlStateValueOff : NSControlStateValueOn;
        [w sendAction:w.action to:w.target];
    } else if ([c isKindOfClass:NSSegmentedControl.class]) nav_adjust(1);
    else if ([c isKindOfClass:NSButton.class]) [(NSButton *)c performClick:nil];
}
static void nav_tab(int d) {
    g_sel = fe_tab_wrap((int)g_sel, d, FE_TAB_COUNT);
    show_page(g_nav_fe, YES);
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
    void (^dir)(BOOL, void (^)(void)) = ^(BOOL p, void (^act)(void)) {
        if (!g_nav_fe.overlayPadMode) return;
        if (p) nav_repeat(act);
        else nav_stop();
    };
    g.dpad.up.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; dir(p, ^{ nav_move(-1); }); };
    g.dpad.down.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; dir(p, ^{ nav_move(1); }); };
    g.dpad.left.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; dir(p, ^{ nav_adjust(-1); }); };
    g.dpad.right.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; dir(p, ^{ nav_adjust(1); }); };
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
}
void fe_nav_start(NappleFE *fe) {
    g_nav_fe = fe;
    for (GCController *c in GCController.controllers) nav_attach(c);
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification
                                                    object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification *n) {
                                                    GCController *c = n.object;
                                                    nav_attach(c);
                                                    [fe toast:[NSString stringWithFormat:@"%@ connected", c.vendorName ?: @"Controller"] symbol:@"gamecontroller.fill"];
                                                    fe_overlay_refresh(fe);
                                                }];
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification
                                                    object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification *n) {
                                                    GCController *c = n.object;
                                                    [fe toast:[NSString stringWithFormat:@"%@ disconnected", c.vendorName ?: @"Controller"] symbol:@"gamecontroller"];
                                                    fe_overlay_refresh(fe);
                                                }];
}
/* The panel was shown or hidden (or switched between pad and mouse mode): ring on / off. */
void fe_nav_mode_changed(void) {
    nav_stop();
    fe_nav_page_changed();
}
