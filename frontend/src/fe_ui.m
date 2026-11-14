#import "fe_ui.h"
#import "fe_internal.h"

/* ---- tokens ---- */
NSFont *fe_font(FEType t) {
    switch (t) {
        case FETypeTitle: return [NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
        case FETypeHeadline: return [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
        case FETypeBody: return [NSFont systemFontOfSize:12 weight:NSFontWeightRegular];
        case FETypeCaption: return [NSFont systemFontOfSize:10.5 weight:NSFontWeightRegular];
        case FETypeMono: return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    }
    return [NSFont systemFontOfSize:12];
}
static NSColor *dyn(CGFloat dark_w, CGFloat dark_a, CGFloat light_w, CGFloat light_a) {
    return [NSColor colorWithName:nil
                  dynamicProvider:^NSColor *(NSAppearance *a) {
                      BOOL dark = [[a bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]] isEqualToString:NSAppearanceNameDarkAqua];
                      return dark ? [NSColor colorWithWhite:dark_w alpha:dark_a] : [NSColor colorWithWhite:light_w alpha:light_a];
                  }];
}
NSColor *fe_color_card(void) { return dyn(1, 0.075, 0, 0.05); }
NSColor *fe_color_stroke(void) { return dyn(1, 0.13, 0, 0.13); }
NSColor *fe_color_dim(void) { return dyn(1, 0.16, 0, 0.12); }
NSImage *fe_symbol(NSString *name, CGFloat pt, NSFontWeight weight) {
    NSImage *i = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    if (!i) return nil;
    return [i imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:pt weight:weight]];
}
BOOL fe_reduce_motion(void) { return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion; }

/* ---- text ---- */
NSTextField *fe_text(NSString *s, FEType t, NSColor *color) {
    NSTextField *l = [NSTextField labelWithString:s ?: @""];
    l.font = fe_font(t);
    l.textColor = color ?: NSColor.labelColor;
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    return l;
}
NSTextField *fe_wrapped(NSString *s, FEType t, NSColor *color) {
    NSTextField *l = [NSTextField wrappingLabelWithString:s ?: @""];
    l.font = fe_font(t);
    l.textColor = color ?: NSColor.labelColor;
    l.preferredMaxLayoutWidth = FE_TEXT_W;
    l.selectable = NO;
    return l;
}
NSTextField *fe_note(NSString *s) { return fe_wrapped(s, FETypeCaption, NSColor.secondaryLabelColor); }

@interface FEChip : NSView
@property(nonatomic, strong) NSColor *fill, *line;
@end
@implementation FEChip
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer {
    [self.effectiveAppearance performAsCurrentDrawingAppearance:^{
        self.layer.backgroundColor = self.fill.CGColor;
        self.layer.borderColor = self.line.CGColor;
    }];
}
- (void)viewDidChangeEffectiveAppearance { self.needsDisplay = YES; }
@end
static NSView *chip_view(NSString *text, NSFont *font, NSColor *textc, NSColor *fill, NSColor *line, CGFloat radius, CGFloat padx) {
    FEChip *v = [FEChip new];
    v.wantsLayer = YES;
    v.fill = fill;
    v.line = line;
    v.layer.cornerRadius = radius;
    v.layer.borderWidth = line ? 1 : 0;
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = font;
    l.textColor = textc;
    l.translatesAutoresizingMaskIntoConstraints = NO;
    [l setContentHuggingPriority:1000 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [l setContentCompressionResistancePriority:1000 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [v addSubview:l];
    [NSLayoutConstraint activateConstraints:@[
        [l.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:padx], [l.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-padx],
        [l.topAnchor constraintEqualToAnchor:v.topAnchor constant:2], [l.bottomAnchor constraintEqualToAnchor:v.bottomAnchor constant:-2]
    ]];
    [v setContentHuggingPriority:1000 forOrientation:NSLayoutConstraintOrientationHorizontal];
    return v;
}
NSView *fe_chip(NSString *text, NSColor *tint) {
    NSColor *c = tint ?: NSColor.secondaryLabelColor;
    return chip_view(text, [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold], c, [c colorWithAlphaComponent:0.16], nil, 8, 7);
}
NSView *fe_keycap(NSString *text) {
    return chip_view(text, [NSFont monospacedSystemFontOfSize:10.5 weight:NSFontWeightMedium], NSColor.labelColor, fe_color_card(), fe_color_stroke(), 5, 6);
}

/* ---- containers ---- */
NSStackView *fe_vstack(CGFloat spacing, NSArray<NSView *> *views) {
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = spacing;
    s.distribution = NSStackViewDistributionFill;
    for (NSView *v in views) [v.widthAnchor constraintEqualToAnchor:s.widthAnchor].active = YES; /* every row fills the stack's width */
    return s;
}
NSStackView *fe_hstack(CGFloat spacing, NSArray<NSView *> *views) {
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.alignment = NSLayoutAttributeCenterY;
    s.spacing = spacing;
    s.distribution = NSStackViewDistributionFill;
    return s;
}
NSView *fe_spacer(CGFloat h) {
    NSView *v = [NSView new];
    [v.heightAnchor constraintEqualToConstant:h].active = YES;
    return v;
}

@interface FECard : NSView
@end
@implementation FECard
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer {
    [self.effectiveAppearance performAsCurrentDrawingAppearance:^{
        self.layer.backgroundColor = fe_color_card().CGColor;
        self.layer.borderColor = fe_color_stroke().CGColor;
    }];
}
- (void)viewDidChangeEffectiveAppearance { self.needsDisplay = YES; }
@end

NSView *fe_card(NSString *title, NSString *symbol, NSArray<NSView *> *rows) {
    FECard *card = [FECard new];
    card.wantsLayer = YES;
    card.layer.cornerRadius = FE_CARD_RADIUS;
    card.layer.cornerCurve = kCACornerCurveContinuous;
    card.layer.borderWidth = 0.5;
    NSMutableArray *all = [NSMutableArray new];
    if (title) {
        NSMutableArray *h = [NSMutableArray new];
        NSImage *img = symbol ? fe_symbol(symbol, 12, NSFontWeightSemibold) : nil;
        if (img) {
            NSImageView *iv = [NSImageView imageViewWithImage:img];
            iv.contentTintColor = NSColor.controlAccentColor;
            [iv.widthAnchor constraintEqualToConstant:18].active = YES;
            [h addObject:iv];
        }
        NSTextField *t = fe_text(title, FETypeHeadline, nil);
        [h addObject:t];
        NSStackView *hs = fe_hstack(6, h);
        [all addObject:hs];
    }
    [all addObjectsFromArray:rows];
    NSStackView *st = fe_vstack(FE_ROW_GAP, all);
    st.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:st];
    [NSLayoutConstraint activateConstraints:@[
        [st.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:FE_CARD_PAD], [st.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-FE_CARD_PAD],
        [st.topAnchor constraintEqualToAnchor:card.topAnchor constant:FE_CARD_PAD - 1], [st.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-FE_CARD_PAD]
    ]];
    if (title) card.accessibilityLabel = title;
    return card;
}

@interface FEFlip : NSView
@end
@implementation FEFlip
- (BOOL)isFlipped { return YES; }
@end

NSScrollView *fe_scroll_page(NSArray<NSView *> *sections) {
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, FE_PANEL_W, 500)];
    sv.hasVerticalScroller = YES;
    sv.autohidesScrollers = YES;
    sv.drawsBackground = NO;
    sv.scrollerStyle = NSScrollerStyleOverlay;
    sv.automaticallyAdjustsContentInsets = NO;
    FEFlip *doc = [FEFlip new];
    doc.translatesAutoresizingMaskIntoConstraints = NO;
    NSStackView *st = fe_vstack(FE_SECTION_GAP, sections);
    st.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:st];
    sv.documentView = doc;
    NSClipView *clip = sv.contentView;
    [NSLayoutConstraint activateConstraints:@[
        [doc.topAnchor constraintEqualToAnchor:clip.topAnchor], [doc.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor], [doc.trailingAnchor constraintEqualToAnchor:clip.trailingAnchor],
        [st.topAnchor constraintEqualToAnchor:doc.topAnchor constant:4], [st.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor constant:-18],
        [st.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor constant:FE_PAGE_INSET], [st.trailingAnchor constraintEqualToAnchor:doc.trailingAnchor constant:-FE_PAGE_INSET]
    ]];
    return sv;
}

NSView *fe_empty(NSString *symbol, NSString *title, NSString *detail) {
    NSMutableArray *v = [NSMutableArray new];
    NSImage *img = fe_symbol(symbol, 26, NSFontWeightLight);
    if (img) {
        NSImageView *iv = [NSImageView imageViewWithImage:img];
        iv.contentTintColor = NSColor.tertiaryLabelColor;
        [v addObject:iv];
    }
    NSTextField *t = fe_text(title, FETypeHeadline, nil);
    t.alignment = NSTextAlignmentCenter;
    [v addObject:t];
    if (detail.length) {
        NSTextField *d = fe_note(detail);
        d.alignment = NSTextAlignmentCenter;
        d.preferredMaxLayoutWidth = FE_TEXT_W - 30;
        [v addObject:d];
    }
    NSStackView *s = fe_vstack(5, v);
    s.alignment = NSLayoutAttributeCenterX;
    s.edgeInsets = NSEdgeInsetsMake(8, 0, 6, 0);
    return s;
}

/* ---- rows ---- */
static BOOL stretches(NSView *c) {
    return [c isKindOfClass:NSPopUpButton.class] || [c isKindOfClass:NSSlider.class] || [c isKindOfClass:NSSegmentedControl.class] ||
           ([c isKindOfClass:NSStackView.class] && [c.identifier isEqualToString:@"fe.stretch"]);
}
NSView *fe_row(NSString *label, NSView *control, NSString *tip) {
    NSTextField *l = fe_text(label, FETypeBody, nil);
    [l.widthAnchor constraintEqualToConstant:FE_LABEL_W].active = YES;
    [l setContentCompressionResistancePriority:800 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSMutableArray *v = [NSMutableArray arrayWithObjects:l, control, nil];
    [control setContentCompressionResistancePriority:250 forOrientation:NSLayoutConstraintOrientationHorizontal];
    if (stretches(control)) [control setContentHuggingPriority:249 forOrientation:NSLayoutConstraintOrientationHorizontal];
    else {
        NSView *sp = [NSView new];
        [sp setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [sp setContentCompressionResistancePriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [v addObject:sp];
    }
    if ([control isKindOfClass:NSPopUpButton.class]) ((NSPopUpButton *)control).cell.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView *r = fe_hstack(8, v);
    if (tip) {
        l.toolTip = tip;
        control.toolTip = tip;
    }
    l.accessibilityElement = NO;
    control.accessibilityLabel = label;
    return r;
}

NSView *fe_switch_row(NSString *title, NSString *detail, BOOL on, NSString *tip, void (^blk)(BOOL)) {
    NSMutableArray *tv = [NSMutableArray arrayWithObject:fe_text(title, FETypeBody, nil)];
    if (detail.length) {
        NSTextField *d = fe_wrapped(detail, FETypeCaption, NSColor.secondaryLabelColor);
        d.preferredMaxLayoutWidth = FE_TEXT_W - 60;
        [tv addObject:d];
    }
    NSStackView *tcol = fe_vstack(1, tv);
    tcol.alignment = NSLayoutAttributeLeading;
    [tcol setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [tcol setContentCompressionResistancePriority:250 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSSwitch *sw = [NSSwitch new];
    sw.controlSize = NSControlSizeSmall;
    sw.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    sw.accessibilityLabel = title;
    fe_bind((NSControl *)sw, ^(NSSwitch *x) { blk(x.state == NSControlStateValueOn); });
    NSStackView *r = fe_hstack(8, @[ tcol, sw ]);
    r.alignment = detail.length ? NSLayoutAttributeCenterY : NSLayoutAttributeCenterY;
    if (tip) {
        tcol.toolTip = tip;
        sw.toolTip = tip;
    }
    return r;
}

NSView *fe_slider_row(NSString *label, double lo, double hi, double val, NSString *fmt, BOOL live, NSString *tip, void (^blk)(double)) {
    NSSlider *sl = [NSSlider sliderWithValue:val minValue:lo maxValue:hi target:nil action:nil];
    sl.controlSize = NSControlSizeSmall;
    sl.continuous = YES;
    NSTextField *v = fe_text([NSString stringWithFormat:fmt, val], FETypeBody, NSColor.secondaryLabelColor);
    v.font = [NSFont monospacedDigitSystemFontOfSize:11.5 weight:NSFontWeightRegular];
    v.alignment = NSTextAlignmentRight;
    [v.widthAnchor constraintEqualToConstant:46].active = YES;
    fe_bind(sl, ^(NSSlider *x) {
        double f = x.doubleValue;
        v.stringValue = [NSString stringWithFormat:fmt, f];
        NSEvent *e = NSApp.currentEvent;
        BOOL dragging = e && (e.type == NSEventTypeLeftMouseDragged || e.type == NSEventTypeLeftMouseDown);
        if (live || !dragging) blk(f);
    });
    sl.accessibilityLabel = label;
    NSStackView *r = fe_hstack(8, @[ sl, v ]);
    r.identifier = @"fe.stretch";
    [sl setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    return fe_row(label, r, tip);
}

NSView *fe_popup_row(NSString *label, NSArray<NSString *> *items, NSInteger sel, NSString *tip, void (^blk)(NSInteger)) {
    NSPopUpButton *p = fe_popup(items, sel, ^(NSPopUpButton *b) { blk(b.indexOfSelectedItem); });
    p.controlSize = NSControlSizeSmall;
    p.font = [NSFont systemFontOfSize:11.5];
    return fe_row(label, p, tip);
}

NSSegmentedControl *fe_segments(NSArray<NSString *> *labels, NSInteger sel, void (^blk)(NSInteger)) {
    NSSegmentedControl *s = [NSSegmentedControl segmentedControlWithLabels:labels trackingMode:NSSegmentSwitchTrackingSelectOne target:nil action:nil];
    s.controlSize = NSControlSizeSmall;
    s.font = [NSFont systemFontOfSize:11];
    s.segmentDistribution = NSSegmentDistributionFillEqually;
    s.selectedSegment = sel;
    fe_bind(s, ^(NSSegmentedControl *x) { blk(x.selectedSegment); });
    return s;
}

NSButton *fe_button(NSString *title, NSString *symbol, FEButtonStyle style, void (^blk)(void)) {
    NSButton *b = [NSButton buttonWithTitle:title target:nil action:nil];
    b.bezelStyle = NSBezelStyleRounded;
    b.controlSize = NSControlSizeSmall;
    b.font = [NSFont systemFontOfSize:11.5];
    NSImage *img = symbol ? fe_symbol(symbol, 11, NSFontWeightMedium) : nil;
    if (img) {
        b.image = img;
        b.imagePosition = NSImageLeading;
        b.imageHugsTitle = YES;
    }
    if (style == FEButtonProminent) b.bezelColor = NSColor.controlAccentColor;
    if (style == FEButtonDestructive) b.hasDestructiveAction = YES;
    fe_bind(b, ^(id x) {
        (void)x;
        blk();
    });
    return b;
}

NSView *fe_confirm_button(NSString *title, NSString *symbol, NSString *prompt, NSString *confirm_title, void (^blk)(void)) {
    NSStackView *box = fe_hstack(8, @[]);
    box.alignment = NSLayoutAttributeCenterY;
    NSStackView *ask = fe_hstack(8, @[]);
    NSTextField *q = fe_text(prompt, FETypeBody, NSColor.secondaryLabelColor);
    NSButton *first = fe_button(title, symbol, FEButtonNormal, ^{});
    __block NSUInteger serial = 0;
    __weak NSStackView *wask = ask;
    __weak NSButton *wfirst = first;
    void (^reset)(void) = ^{
        wask.hidden = YES;
        wfirst.hidden = NO;
    };
    NSButton *yes = fe_button(confirm_title ?: @"Confirm", nil, FEButtonDestructive, ^{
        serial++;
        reset();
        blk();
    });
    NSButton *no = fe_button(@"Cancel", nil, FEButtonNormal, ^{
        serial++;
        reset();
    });
    [ask addArrangedSubview:q];
    [ask addArrangedSubview:no];
    [ask addArrangedSubview:yes];
    ask.hidden = YES;
    fe_bind(first, ^(id x) {
        (void)x;
        wfirst.hidden = YES;
        wask.hidden = NO;
        NSUInteger me = ++serial;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 6 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            if (me == serial) reset();
        });
    });
    [box addArrangedSubview:first];
    [box addArrangedSubview:ask];
    return box;
}

NSGridView *fe_grid(NSArray<NSArray<NSView *> *> *rows) {
    NSGridView *g = [NSGridView gridViewWithViews:rows];
    g.columnSpacing = 10;
    g.rowSpacing = 6;
    g.rowAlignment = NSGridRowAlignmentNone;
    for (NSInteger i = 0; i < g.numberOfRows; i++) [g rowAtIndex:i].yPlacement = NSGridCellPlacementCenter;
    return g;
}

NSView *fe_leading(NSView *v) {
    NSView *sp = [NSView new];
    [sp setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [sp setContentCompressionResistancePriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    return fe_hstack(0, @[ v, sp ]);
}
