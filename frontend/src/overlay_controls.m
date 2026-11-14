/* The overlay's Controls page and the players / controllers card (also shown on the Status page).
   Everything here edits napple_bindings (input.json of the game): keys per Switch button (two slots), the four keys of each stick, one
   controller element per button, dead zone / outer range / response curve / trigger point, axis inversion, npad style, console mode.
   One profile per player (Ryujinx-style player tabs): g_slot -1 edits the shared bindings, 0..7 the profile of that player. */
#include <stdlib.h>
#include <string.h>
#include "fe_internal.h"
#include "fe_ui.h"

static int g_slot = -1;
static void bget(NappleFE *fe, napple_bindings *b) {
    if (g_slot >= 0) napple_input_get_player_bindings(fe.input, g_slot, b);
    else napple_input_get_bindings(fe.input, b);
}
static int bset(NappleFE *fe, const napple_bindings *b) {
    return g_slot >= 0 ? napple_input_set_player_bindings(fe.input, g_slot, b) : napple_input_set_bindings(fe.input, b);
}
static void edit_bindings(NappleFE *fe, void (^mut)(napple_bindings *)) {
    napple_bindings t;
    bget(fe, &t);
    mut(&t);
    if (bset(fe, &t) != 0) fe_overlay_refresh(fe); /* rejected (out of range): show the stored values again */
}

/* ---- key / button capture ---- */
@interface CaptureBox : NSObject
@property(weak) NappleFE *fe;
@property(weak) NSButton *button;
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
static NSButton *slot_button(NSString *title, NSString *tip, void (^blk)(NSButton *)) {
    NSButton *b = [NSButton buttonWithTitle:title target:nil action:nil];
    b.controlSize = NSControlSizeSmall;
    b.bezelStyle = NSBezelStyleRounded;
    b.font = [NSFont systemFontOfSize:11];
    b.lineBreakMode = NSLineBreakByTruncatingTail;
    b.toolTip = tip;
    __weak NSButton *w = b;
    fe_bind(b, ^(id x) {
        (void)x;
        blk(w);
    });
    return b;
}
static NSView *binding_slider(NappleFE *fe, NSString *label, double lo, double hi, double val, NSString *tip, void (^set)(napple_bindings *, float)) {
    return fe_slider_row(label, lo, hi, val, @"%.2f", NO, tip, ^(double v) { edit_bindings(fe, ^(napple_bindings *t) { set(t, (float)v); }); });
}

/* ---- live controller drawing ---- */
@interface FEPadView : NSView
@property(nonatomic) napple_input *input;
@property(nonatomic) int slot;
@end
@implementation FEPadView
- (BOOL)isFlipped { return YES; }
static void pad_dot(CGFloat x, CGFloat y, CGFloat r, BOOL on, NSColor *hot) {
    NSBezierPath *b = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(x - r, y - r, 2 * r, 2 * r)];
    [(on ? hot : fe_color_dim()) setFill];
    [b fill];
}
static void pad_pill(NSRect r, BOOL on, NSColor *hot) {
    NSBezierPath *b = [NSBezierPath bezierPathWithRoundedRect:r xRadius:r.size.height / 2 yRadius:r.size.height / 2];
    [(on ? hot : fe_color_dim()) setFill];
    [b fill];
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    napple_npad_state st = {0};
    int used = self.input ? napple_input_get_pad_state(self.input, self.slot, &st) : 0;
    NSColor *hot = NSColor.controlAccentColor;
    CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
    CGFloat cx = w / 2 - 155; /* the drawing is 310 wide, centred */
    NSBezierPath *body = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(cx + 8, 18, 294, h - 26) xRadius:40 yRadius:40];
    [fe_color_card() setFill];
    [body fill];
    [fe_color_stroke() setStroke];
    body.lineWidth = 1;
    [body stroke];
#define BTN(b) ((st.buttons >> (b)) & 1)
    pad_pill(NSMakeRect(cx + 34, 4, 46, 12), BTN(NAPPLE_BTN_ZL), hot);
    pad_pill(NSMakeRect(cx + 310 - 80, 4, 46, 12), BTN(NAPPLE_BTN_ZR), hot);
    pad_pill(NSMakeRect(cx + 34, 20, 56, 9), BTN(NAPPLE_BTN_L), hot);
    pad_pill(NSMakeRect(cx + 310 - 90, 20, 56, 9), BTN(NAPPLE_BTN_R), hot);
    CGFloat lx = cx + 62, ly = 62, fx = cx + 310 - 66, fy = 70;
    pad_dot(lx, ly, 17, BTN(NAPPLE_BTN_STICK_L), hot);
    pad_dot(lx + st.left.x / 32767.0 * 9, ly - st.left.y / 32767.0 * 9, 8, YES, NSColor.labelColor);
    pad_dot(fx, fy - 17, 8, BTN(NAPPLE_BTN_X), hot);
    pad_dot(fx, fy + 17, 8, BTN(NAPPLE_BTN_B), hot);
    pad_dot(fx - 17, fy, 8, BTN(NAPPLE_BTN_Y), hot);
    pad_dot(fx + 17, fy, 8, BTN(NAPPLE_BTN_A), hot);
    CGFloat dx = cx + 96, dy = h - 36;
    pad_pill(NSMakeRect(dx - 6, dy - 18, 12, 36), BTN(NAPPLE_BTN_UP) || BTN(NAPPLE_BTN_DOWN), hot);
    pad_pill(NSMakeRect(dx - 18, dy - 6, 36, 12), BTN(NAPPLE_BTN_LEFT) || BTN(NAPPLE_BTN_RIGHT), hot);
    CGFloat rx = cx + 310 - 100, ry = h - 36;
    pad_dot(rx, ry, 17, BTN(NAPPLE_BTN_STICK_R), hot);
    pad_dot(rx + st.right.x / 32767.0 * 9, ry - st.right.y / 32767.0 * 9, 8, YES, NSColor.labelColor);
    pad_dot(cx + 155 - 16, 50, 4, BTN(NAPPLE_BTN_MINUS), hot);
    pad_dot(cx + 155 + 16, 50, 4, BTN(NAPPLE_BTN_PLUS), hot);
#undef BTN
    if (!used) {
        NSDictionary *at = @{NSFontAttributeName : [NSFont systemFontOfSize:11], NSForegroundColorAttributeName : NSColor.secondaryLabelColor};
        NSString *t = @"Not in use by the game";
        NSSize sz = [t sizeWithAttributes:at];
        [t drawAtPoint:NSMakePoint(w / 2 - sz.width / 2, h / 2 - sz.height / 2 + 4) withAttributes:at];
    }
}
@end

/* ---- players ---- */
static NSString *style_name(uint32_t st) {
    if (!st) return @"Not in use";
    return st & NAPPLE_STYLE_FULL_KEY ? @"Pro Controller" : st & NAPPLE_STYLE_HANDHELD ? @"Handheld" : st & NAPPLE_STYLE_JOY_DUAL ? @"Joy-Con pair" : @"Joy-Con";
}
/* Players worth showing: player 1 always, then every slot the game uses or a controller is attached to. */
static NSArray<NSNumber *> *visible_players(const napple_input_snapshot *sn) {
    NSMutableArray *a = [NSMutableArray new];
    for (int i = 0; i < 8; i++) {
        if (i > 0 && !sn->player_style[i] && sn->controllers <= i) continue;
        [a addObject:@(i)];
    }
    return a;
}
NSString *fe_players_signature(NappleFE *fe) {
    if (!fe.input) return @"";
    napple_input_snapshot sn;
    napple_input_get_snapshot(fe.input, &sn);
    NSMutableString *s = [NSMutableString stringWithFormat:@"%d", sn.controllers];
    for (int i = 0; i < 8; i++) [s appendFormat:@"|%u|%s", sn.player_style[i], i < sn.controllers ? sn.controller_names[i] : ""];
    return s;
}
static NSString *who_string(const napple_input_snapshot *sn, int i) {
    NSString *dev = i < sn->controllers ? @(sn->controller_names[i]) : (i == 0 ? @"Keyboard" : @"No controller");
    return [NSString stringWithFormat:@"Player %d  -  %@", i + 1, dev];
}
static NSView *player_line(NappleFE *fe, const napple_input_snapshot *sn, int i) {
    BOOL pad = i < sn->controllers;
    NSImage *img = fe_symbol(pad ? @"gamecontroller.fill" : (i == 0 ? @"keyboard.fill" : @"gamecontroller"), 14, NSFontWeightRegular);
    NSImageView *iv = [NSImageView imageViewWithImage:img ?: [NSImage new]];
    iv.contentTintColor = pad || i == 0 ? NSColor.controlAccentColor : NSColor.tertiaryLabelColor;
    [iv.widthAnchor constraintEqualToConstant:22].active = YES;
    napple_bindings tmp;
    int own = napple_input_get_player_bindings(fe.input, i, &tmp);
    NSTextField *n = fe_text(who_string(sn, i), FETypeBody, nil);
    NSTextField *d = fe_text(style_name(sn->player_style[i]), FETypeCaption, NSColor.secondaryLabelColor);
    NSStackView *col = fe_vstack(0, @[ n, d ]);
    col.alignment = NSLayoutAttributeLeading;
    [col setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [col setContentCompressionResistancePriority:250 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSView *chip = fe_chip(own ? @"own profile" : @"shared", own ? NSColor.controlAccentColor : NSColor.secondaryLabelColor);
    return fe_hstack(8, @[ iv, col, chip ]);
}
NSView *fe_players_card(NappleFE *fe, BOOL selectable) {
    (void)selectable;
    NSMutableArray *rows = [NSMutableArray new];
    if (!fe.input) {
        [rows addObject:fe_empty(@"gamecontroller", @"Input is not attached", @"This window has no controller input.")];
        return fe_card(@"Players and controllers", @"person.2.fill", rows);
    }
    napple_input_snapshot sn;
    napple_input_get_snapshot(fe.input, &sn);
    for (NSNumber *n in visible_players(&sn)) [rows addObject:player_line(fe, &sn, n.intValue)];
    NSString *sum = sn.controllers ? [NSString stringWithFormat:@"%d controller%s connected. Each one takes the next player slot, for split-screen and local multiplayer. Player 1 also reads the keyboard.",
                                                                  sn.controllers, sn.controllers == 1 ? "" : "s"]
                                   : @"No controller connected: the keyboard drives player 1. Connect controllers (up to 8) for split-screen and local multiplayer; they are picked up immediately.";
    [rows addObject:fe_note(sum)];
    return fe_card(@"Players and controllers", @"person.2.fill", rows);
}

/* ---- the page ---- */
NSView *fe_page_controls(NappleFE *fe) {
    NSMutableArray *sec = [NSMutableArray new];
    if (!fe.input) {
        [sec addObject:fe_card(nil, nil, @[ fe_empty(@"gamecontroller", @"Input is not attached", @"Controller and keyboard settings need the input system, which this window does not have.") ])];
        return fe_scroll_page(sec);
    }
    napple_input_snapshot sn;
    napple_input_get_snapshot(fe.input, &sn);
    NSArray<NSNumber *> *vis = visible_players(&sn);
    if (g_slot >= 0 && ![vis containsObject:@(g_slot)]) g_slot = -1;
    napple_bindings bd;
    bget(fe, &bd);

    /* player picker + live view */
    NSMutableArray *labels = [NSMutableArray arrayWithObject:@"All"];
    for (NSNumber *n in vis) [labels addObject:[NSString stringWithFormat:@"P%d", n.intValue + 1]];
    NSInteger selIdx = g_slot < 0 ? 0 : (NSInteger)[vis indexOfObject:@(g_slot)] + 1;
    NSSegmentedControl *seg = fe_segments(labels, selIdx, ^(NSInteger i) {
        g_slot = i == 0 ? -1 : vis[(NSUInteger)i - 1].intValue;
        fe_overlay_refresh(fe);
    });
    seg.toolTip = @"Edit the controls shared by everyone, or one player's own profile.";
    FEPadView *pv = [[FEPadView alloc] initWithFrame:NSMakeRect(0, 0, 340, 120)];
    pv.input = fe.input;
    pv.slot = g_slot < 0 ? 0 : g_slot;
    [pv.heightAnchor constraintEqualToConstant:120].active = YES;
    NSMutableArray *pr = [NSMutableArray arrayWithObjects:seg, nil];
    if (g_slot >= 0) {
        [pr addObject:player_line(fe, &sn, g_slot)];
    } else {
        NSTextField *t = fe_text(@"All players (shared)", FETypeBody, nil);
        [pr addObject:fe_vstack(0, @[ t, fe_text(@"Default for players without their own profile", FETypeCaption, NSColor.secondaryLabelColor) ])];
        ((NSStackView *)pr.lastObject).alignment = NSLayoutAttributeLeading;
    }
    [pr addObject:pv];
    if (g_slot >= 0) {
        napple_bindings tmp;
        if (!napple_input_get_player_bindings(fe.input, g_slot, &tmp)) {
            [pr addObject:fe_button([NSString stringWithFormat:@"Give player %d its own profile", g_slot + 1], @"person.crop.circle.badge.plus", FEButtonNormal, ^{
                napple_bindings cp;
                napple_input_get_player_bindings(fe.input, g_slot, &cp); /* the shared set as the starting point */
                napple_input_set_player_bindings(fe.input, g_slot, &cp);
                fe_overlay_refresh(fe);
            })];
        }
        [pr addObject:fe_note(@"Each player can have different keys, controller buttons, sticks and triggers. Player 1 also reads the keyboard.")];
    }
    [sec addObject:fe_card(@"Players", @"person.2.fill", pr)];
    __weak FEPadView *wpv = pv;
    __block NSString *sig = fe_players_signature(fe);
    __block int n = 0;
    __weak NappleFE *wfe = fe;
    fe_overlay_set_tick(1.0 / 30, ^{
        wpv.needsDisplay = YES;
        if (++n % 20 == 0 && wfe) {   /* a controller was attached or removed: rebuild the page */
            NSString *now = fe_players_signature(wfe);
            if (![now isEqualToString:sig] && !napple_input_capture_pending(wfe.input)) fe_overlay_refresh(wfe);
        }
    });

    /* button grid */
    NSMutableArray *rows = [NSMutableArray new];
    [rows addObject:@[ [NSView new], fe_text(@"Key 1", FETypeCaption, NSColor.secondaryLabelColor), fe_text(@"Key 2", FETypeCaption, NSColor.secondaryLabelColor),
                       fe_text(@"Pad", FETypeCaption, NSColor.secondaryLabelColor) ]];
    for (int b = 0; b < NAPPLE_BTN_COUNT_BASIC; b++) {
        NSMutableArray *r = [NSMutableArray arrayWithObject:fe_text(@(napple_button_name(b)), FETypeHeadline, nil)];
        for (int slot = 0; slot < 2; slot++) {
            int sb = b, ss = slot;
            [r addObject:slot_button(@(napple_keycode_name(bd.key[b][slot])), @"Click, then press a key. Delete clears, Esc cancels.",
                                     ^(NSButton *x) { begin_capture(fe, x, 0, sb, ss); })];
        }
        int sb = b;
        [r addObject:slot_button(bd.pad[b][0] >= 0 ? @(napple_pad_element_name(bd.pad[b][0])) : @"-", @"Click, then press a controller button.",
                                 ^(NSButton *x) { begin_capture(fe, x, 1, sb, 0); })];
        [rows addObject:r];
    }
    NSGridView *grid = fe_grid(rows);
    grid.rowAlignment = NSGridRowAlignmentNone;
    [grid columnAtIndex:0].width = 48;
    for (int c = 1; c < 4; c++) {
        [grid columnAtIndex:c].xPlacement = NSGridCellPlacementFill;
        [grid columnAtIndex:c].width = 96;
    }
    [sec addObject:fe_card(@"Buttons", @"keyboard", @[
        fe_note(@"Click a slot, then press a key or controller button. Delete clears, Esc cancels. Home / Guide opens this panel, Share takes a screenshot."), grid
    ])];

    static const char *const dirs[4] = {"Left", "Up", "Right", "Down"};
    NSMutableArray *srows = [NSMutableArray new];
    for (int sw = 0; sw < 8; sw++) {
        uint16_t key = (sw < 4 ? bd.stick_l : bd.stick_r)[sw & 3];
        int s2 = sw;
        [srows addObject:@[ fe_text([NSString stringWithFormat:@"%s %s", sw < 4 ? "Left" : "Right", dirs[sw & 3]], FETypeBody, nil),
                            slot_button(@(napple_keycode_name(key)), @"Keyboard key that tilts this stick.", ^(NSButton *x) { begin_capture(fe, x, 2, s2, 0); }) ]];
    }
    NSGridView *sg = fe_grid(srows);
    sg.rowAlignment = NSGridRowAlignmentNone;
    [sg columnAtIndex:0].width = 80;
    [sg columnAtIndex:1].xPlacement = NSGridCellPlacementFill;
    [sec addObject:fe_card(@"Stick keys", @"arrow.up.left.and.arrow.down.right", @[ sg ])];

    NSMutableArray *an = [NSMutableArray new];
    [an addObject:binding_slider(fe, @"Dead zone", 0.0, 0.8, bd.stick_deadzone, @"Ignore small stick movement (worn sticks).",
                                 ^(napple_bindings *t, float v) { t->stick_deadzone = MIN(v, t->stick_range - 0.05f); })];
    [an addObject:binding_slider(fe, @"Outer range", 0.5, 1.0, bd.stick_range, @"Raw deflection that counts as full. Lower it for sticks that do not reach the edge.", ^(napple_bindings *t, float v) {
              t->stick_range = v;
              if (t->stick_deadzone >= v) t->stick_deadzone = v - 0.05f;
          })];
    [an addObject:binding_slider(fe, @"Curve", 0.5, 3.0, bd.stick_curve, @"1 = linear. Above 1 gives finer control near the centre.", ^(napple_bindings *t, float v) { t->stick_curve = v; })];
    [an addObject:binding_slider(fe, @"Trigger point", 0.05, 0.95, bd.trigger_threshold, @"How far ZL / ZR must be pulled to count as pressed.",
                                 ^(napple_bindings *t, float v) { t->trigger_threshold = v; })];
    NSMutableArray *inv = [NSMutableArray new];
    static const char *const invn[4] = {"L-X", "L-Y", "R-X", "R-Y"};
    for (int i = 0; i < 4; i++) {
        int k = i;
        NSButton *c = fe_check(@(invn[i]), bd.invert[i], ^(NSButton *bt) {
            BOOL on = bt.state == NSControlStateValueOn;
            edit_bindings(fe, ^(napple_bindings *t) { t->invert[k] = on; });
        });
        c.controlSize = NSControlSizeSmall;
        [inv addObject:c];
    }
    [an addObject:fe_row(@"Invert axis", fe_hstack(10, inv), @"Flip a stick axis (also for the keyboard sticks).")];
    [sec addObject:fe_card(@"Sticks and triggers", @"dial.medium.fill", an)];

    NSMutableArray *co = [NSMutableArray new];
    [co addObject:fe_popup_row(@"Controller type", @[ @"Automatic", @"Pro Controller", @"Joy-Con pair", @"Handheld" ], bd.style,
                               @"The controller style the game sees. Read by the game when it starts.",
                               ^(NSInteger i) { edit_bindings(fe, ^(napple_bindings *t) { t->style = (int)i; }); })];
    [co addObject:fe_popup_row(@"Console mode", @[ @"Docked (1080p)", @"Handheld (720p)" ], bd.operation_mode, @"Applies when the game restarts.",
                               ^(NSInteger i) { edit_bindings(fe, ^(napple_bindings *t) { t->operation_mode = (int)i; }); })];
    [co addObject:fe_popup_row(@"Face buttons", @[ @"Automatic", @"By position", @"By label" ], bd.face_layout,
                               @"Map the A / B / X / Y of a non-Nintendo controller by where the buttons sit or by what they are called.",
                               ^(NSInteger i) { edit_bindings(fe, ^(napple_bindings *t) { t->face_layout = (int)i; }); })];
    [co addObject:fe_switch_row(@"Mouse acts as touch screen", @"Left click taps the screen.", bd.mouse_touch, nil, ^(BOOL on) {
              edit_bindings(fe, ^(napple_bindings *t) { t->mouse_touch = on; });
          })];
    [co addObject:fe_note(@"Controller type is read by the game when it starts; console mode applies on restart.")];
    NSString *rst = g_slot >= 0 ? @"Use shared profile" : @"Reset controls to defaults";
    [co addObject:fe_confirm_button(rst, @"arrow.counterclockwise", @"Sure?", @"Reset", ^{
              if (g_slot >= 0) napple_input_clear_player_bindings(fe.input, g_slot);
              else napple_input_reset_bindings(fe.input);
              fe_overlay_refresh(fe);
              [fe toast:g_slot >= 0 ? @"Using the shared profile" : @"Controls reset" symbol:@"gamecontroller.fill"];
          })];
    [sec addObject:fe_card(@"Controller and console", @"gamecontroller.fill", co)];
    return fe_scroll_page(sec);
}
