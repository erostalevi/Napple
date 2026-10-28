/* nn::swkbd replacement: a text bar at the bottom of the game window.  The Mac keyboard (and input methods) type
   into a normal text field; Return confirms when the text is valid, Esc cancels.  The caller (the runtime's applet
   code) parses the guest's KeyboardConfig and passes a napple_text_request. */
#include <stdlib.h>
#include <string.h>
#include "fe_internal.h"

@interface TextBar : NSVisualEffectView <NSTextFieldDelegate>
@property (nonatomic) napple_text_request req;
@property (nonatomic, strong) NSTextField *field, *counter;
@property (nonatomic, strong) NSButton *ok;
@property (nonatomic, copy) void (^finish)(BOOL, NSString *);
@end
@implementation TextBar
- (BOOL)valid {
    return napple_text_validate(&_req, self.field.stringValue.UTF8String);
}
- (void)refresh {
    NSString *t = self.field.stringValue;
    napple_text_request q = self.req;
    BOOL ok = [self valid];
    NSUInteger n = [t lengthOfBytesUsingEncoding:NSUTF32StringEncoding] / 4;
    self.counter.stringValue = [NSString stringWithFormat:@"%lu/%u", (unsigned long)n, q.max_length ? q.max_length : 500];
    self.counter.textColor = ok ? NSColor.secondaryLabelColor : NSColor.systemRedColor;
    self.ok.enabled = ok;
}
- (void)controlTextDidChange:(NSNotification *)n {
    (void)n;
    [self refresh];
}
- (BOOL)control:(NSControl *)c textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)c;
    (void)tv;
    if (sel == @selector(insertNewline:)) {
        if ([self valid]) self.finish(YES, self.field.stringValue);
        else NSBeep();
        return YES;
    }
    if (sel == @selector(cancelOperation:)) {
        self.finish(NO, nil);
        return YES;
    }
    return NO;
}
@end

static TextBar *g_bar;
BOOL fe_textbar_visible(NappleFE *fe) {
    (void)fe;
    return g_bar != nil;
}

void fe_textbar_show(NappleFE *fe, const napple_text_request *req, napple_text_done_fn done, void *user) {
    if (g_bar) {
        done(user, 0, NULL);
        return;
    } /* one keyboard at a time */
    NSRect b = fe.gameView.bounds;
    CGFloat h = 118;
    TextBar *bar = [[TextBar alloc] initWithFrame:NSMakeRect(0, 0, b.size.width, h)];
    bar.material = NSVisualEffectMaterialHUDWindow;
    bar.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    bar.state = NSVisualEffectStateActive;
    bar.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    napple_text_request q = *req;
    q.header = q.sub = q.guide = q.initial = NULL;
    bar.req = q;
    NSTextField *head = fe_label(req->header && *req->header ? @(req->header) : @"Enter text", 15, YES);
    NSTextField *sub = fe_label(req->sub ? @(req->sub) : @"", 11, NO);
    sub.textColor = NSColor.secondaryLabelColor;
    NSTextField *f = req->password ? [NSSecureTextField new] : [NSTextField new];
    f.stringValue = req->initial ? @(req->initial) : @"";
    f.placeholderString = req->guide ? @(req->guide) : @"";
    f.delegate = bar;
    f.font = [NSFont systemFontOfSize:16];
    bar.field = f;
    bar.counter = fe_label(@"", 11, NO);
    NSButton *ok = [NSButton buttonWithTitle:@"OK" target:nil action:nil], *cancel = [NSButton buttonWithTitle:@"Cancel" target:nil action:nil];
    bar.ok = ok;
    NSStackView *buttons = [NSStackView stackViewWithViews:@[ bar.counter, cancel, ok ]];
    buttons.spacing = 8;
    NSStackView *col = [NSStackView stackViewWithViews:@[ head, sub, f, buttons ]];
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.alignment = NSLayoutAttributeLeading;
    col.spacing = 6;
    col.translatesAutoresizingMaskIntoConstraints = NO;
    [bar addSubview:col];
    [NSLayoutConstraint activateConstraints:@[
        [col.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:20], [col.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-20],
        [col.topAnchor constraintEqualToAnchor:bar.topAnchor constant:10], [f.trailingAnchor constraintEqualToAnchor:col.trailingAnchor],
        [buttons.trailingAnchor constraintEqualToAnchor:col.trailingAnchor]
    ]];
    __block BOOL finished = NO;
    __weak TextBar *wb = bar;
    bar.finish = ^(BOOL accepted, NSString *text) {
        if (finished) return;
        finished = YES;
        NSString *copy = [text copy];
        [wb removeFromSuperview];
        g_bar = nil;
        if (fe.input) napple_input_set_blocked(fe.input, fe.overlayVisible);
        [fe resignToGame];
        done(user, accepted, accepted ? copy.UTF8String : NULL);
    };
    fe_bind(ok, ^(id x) {
        (void)x;
        TextBar *b = wb;
        if ([b valid]) b.finish(YES, b.field.stringValue);
    });
    fe_bind(cancel, ^(id x) {
        (void)x;
        wb.finish(NO, nil);
    });
    g_bar = bar;
    [fe.gameView addSubview:bar];
    if (fe.input) napple_input_set_blocked(fe.input, YES);
    [bar refresh];
    [fe.window makeFirstResponder:f];
    NSString *act;
    double ms;
    if (auto_action(&act, &ms) && !getenv("NAPPLE_SWKBD_ANSWER")) { /* test aid: the bar is shot, then answered like a sheet (cancel -> Cancel, anything else -> OK) */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(ms * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
            shot_rect([fe.window convertRectToScreen:[bar convertRect:bar.bounds toView:nil]], @"swkbd"); /* the bar only */
            if ([act isEqualToString:@"cancel"]) [cancel performClick:nil];
            else [ok performClick:nil];
        });
    }
}
