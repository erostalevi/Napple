/* fe_ui: the small design-token and widget helper behind the overlay (and the toasts): spacing, type, radii, cards, rows, switches,
   sliders, confirm buttons and empty states, so every page of the panel is built from the same few parts and looks the same in
   light and dark.  Main thread only. */
#ifndef NAPPLE_FE_UI_H
#define NAPPLE_FE_UI_H
#import <Cocoa/Cocoa.h>

/* ---- tokens ---- */
#define FE_PANEL_W 432.0   /* the overlay panel's width (it shrinks in a small window) */
#define FE_PAGE_INSET 14.0 /* panel edge to card edge */
#define FE_CARD_PAD 12.0   /* card edge to its rows */
#define FE_SECTION_GAP 12.0
#define FE_ROW_GAP 9.0
#define FE_CARD_RADIUS 12.0
#define FE_LABEL_W 112.0
#define FE_TEXT_W 372.0 /* width of wrapped text inside a card */

typedef NS_ENUM(NSInteger, FEType) { FETypeTitle, FETypeHeadline, FETypeBody, FETypeCaption, FETypeMono };
typedef NS_ENUM(NSInteger, FEButtonStyle) { FEButtonNormal, FEButtonProminent, FEButtonDestructive };

NSFont *fe_font(FEType t);
NSColor *fe_color_card(void);      /* subtle fill of a card, dynamic */
NSColor *fe_color_stroke(void);    /* hairline around cards / key caps */
NSColor *fe_color_dim(void);       /* inactive element of the controller drawing */
/* SF Symbol at a point size (nil when the OS has no such symbol). */
NSImage *fe_symbol(NSString *name, CGFloat pt, NSFontWeight weight);
BOOL fe_reduce_motion(void);

/* ---- text ---- */
NSTextField *fe_text(NSString *s, FEType t, NSColor *color); /* single line, truncates */
NSTextField *fe_note(NSString *s);                           /* wrapped secondary caption */
NSTextField *fe_wrapped(NSString *s, FEType t, NSColor *color);
NSView *fe_chip(NSString *text, NSColor *tint);              /* small rounded status label */

/* ---- containers ---- */
NSStackView *fe_vstack(CGFloat spacing, NSArray<NSView *> *views); /* fills the width */
NSStackView *fe_hstack(CGFloat spacing, NSArray<NSView *> *views);
/* A card: optional symbol + title header, then the rows (each fills the card's width). */
NSView *fe_card(NSString *title, NSString *symbol, NSArray<NSView *> *rows);
/* A scrolling page of sections (top aligned, fills the clip width). */
NSScrollView *fe_scroll_page(NSArray<NSView *> *sections);
/* Centered icon + title + detail, for lists that are empty. */
NSView *fe_empty(NSString *symbol, NSString *title, NSString *detail);
NSView *fe_spacer(CGFloat h);

/* ---- rows ---- */
NSView *fe_row(NSString *label, NSView *control, NSString *tip);
/* Title (and optional detail) on the left, a switch on the right; blk gets the new state. */
NSView *fe_switch_row(NSString *title, NSString *detail, BOOL on, NSString *tip, void (^blk)(BOOL on));
/* Slider with a live value label.  `live` NO applies the value when the user lets go (or for keyboard / controller steps); YES on every change. */
NSView *fe_slider_row(NSString *label, double lo, double hi, double val, NSString *fmt, BOOL live, NSString *tip, void (^blk)(double v));
NSView *fe_popup_row(NSString *label, NSArray<NSString *> *items, NSInteger sel, NSString *tip, void (^blk)(NSInteger i));
NSSegmentedControl *fe_segments(NSArray<NSString *> *labels, NSInteger sel, void (^blk)(NSInteger i));
NSButton *fe_button(NSString *title, NSString *symbol, FEButtonStyle style, void (^blk)(void));
/* A button that asks "Sure?" inline (Confirm / Cancel) before running blk; reverts after a few seconds. */
NSView *fe_confirm_button(NSString *title, NSString *symbol, NSString *prompt, NSString *confirm_title, void (^blk)(void));
/* Key cap: monospaced text in a rounded outline. */
NSView *fe_keycap(NSString *text);
/* Two column list (key caps / values) */
NSGridView *fe_grid(NSArray<NSArray<NSView *> *> *rows);

/* Keeps a button at its natural width, flush left, inside a full-width card. */
NSView *fe_leading(NSView *v);

#endif
