/* Internal Objective-C declarations shared by frontend.m, overlay.m and textbar.m. */
#ifndef NAPPLE_FE_INTERNAL_H
#define NAPPLE_FE_INTERNAL_H
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include "internal.h"
#include "napple_input.h"

@class NappleFE;

/* Blocks as control actions: bind(control, ^(id sender){...}). */
void fe_bind(NSControl *c, void (^blk)(id sender));
NSButton *fe_check(NSString *title, BOOL on, void (^blk)(id));
NSPopUpButton *fe_popup(NSArray<NSString *> *items, NSInteger sel, void (^blk)(id));
NSTextField *fe_label(NSString *text, CGFloat size, BOOL bold);

/* The frontend state. All UI methods run on the main thread. */
@interface NappleFE : NSObject <NSWindowDelegate, NSApplicationDelegate>
@property(nonatomic) napple_frontend_config cfg;
@property(nonatomic, copy) NSString *titleID, *titleName, *dir, *shotDir;
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSView *gameView;
@property(nonatomic, strong) CAMetalLayer *layer;
@property(nonatomic, strong) NSView *overlayView, *textBarView, *bootView;
@property(nonatomic) napple_settings settings;
@property(nonatomic, strong) NSArray<NSDictionary *> *dlc, *updates;
@property(nonatomic, copy) NSString *manifestTitle, *manifestBase;
@property(nonatomic) BOOL quitting, overlayVisible, readyFired;
@property(nonatomic) BOOL overlayPadMode; /* the panel was opened with the controller: the pad navigates it and the game sees neutral input */
@property(nonatomic) napple_input *input;
@property(nonatomic) double displayHz;
- (napple_settings)currentSettings;
- (void)applySettings:(napple_settings)s save:(BOOL)save notify:(BOOL)notify;
- (void)toggleOverlay;
- (void)toggleOverlayFromPad:(BOOL)pad;
- (void)refreshOverlayInputMode; /* recompute whether the game still receives input while the panel is open */
- (void)showOverlay:(BOOL)show;
- (void)screenshot;
- (void)quit;
- (void)restartGuest;
- (void)resignToGame;   /* give keyboard focus back to the game view */
- (BOOL)speedKey:(unsigned short)code down:(BOOL)down repeat:(BOOL)repeat;   /* speed / frame generation / statistics hot keys; YES = handled */
- (void)updateSpeed;                       /* recompute the effective speed and tell the runtime */
- (void)toast:(NSString *)text;            /* transient pill at the top of the game view (symbol picked from the text) */
- (void)toast:(NSString *)text symbol:(NSString *)sf_symbol; /* same with an explicit SF Symbol name (nil = none) */
@end

/* overlay.m */
NSView *fe_build_overlay(NappleFE *fe);
void fe_overlay_place(NappleFE *fe);
void fe_nav_start(NappleFE *fe);
void fe_nav_mode_changed(void);
void fe_nav_page_changed(void);
void fe_overlay_refresh(NappleFE *fe);
void fe_overlay_select_tab(int index);   /* 0 Audio/Video, 1 FPS/Speed, 2 Controls, 3 Game/System, 4 Status */
void fe_overlay_goto_tab(NappleFE *fe, int index); /* shows the panel if hidden, then selects the tab (menu Cmd+1..5) */
void fe_overlay_set_visible(NappleFE *fe, BOOL show); /* animated show / hide of the panel view (frontend.m showOverlay) */
void fe_collect_focusable(NSView *root, NSMutableArray *out); /* the controls the pad / keyboard navigation can reach */
void fe_overlay_focus_controls(NappleFE *fe);       /* keyboard: move focus into the panel (Cmd+0); Tab / Shift+Tab / Space then work, Esc returns to the game */
/* overlay_pages.m / overlay_controls.m: one builder per tab (main thread). */
NSView *fe_page_video(NappleFE *fe);
NSView *fe_page_speed(NappleFE *fe);
NSView *fe_page_controls(NappleFE *fe);
NSView *fe_page_game(NappleFE *fe);
NSView *fe_page_status(NappleFE *fe);
/* settings edit helper shared by the pages: copy, mutate, apply + save + notify */
void fe_overlay_update(NappleFE *fe, void (^mut)(napple_settings *));
/* A page asks for a periodic refresh of its live labels (cleared whenever the page changes or the panel hides). */
void fe_overlay_set_tick(NSTimeInterval period, void (^tick)(void));
/* controls: players snapshot line used by Controls and Status; signature changes when a controller is attached or removed */
NSString *fe_players_signature(NappleFE *fe);
NSView *fe_players_card(NappleFE *fe, BOOL selectable);

/* simulators.m: amiibo picker and IR camera pages of the overlay; the webcam IR source is installed on first use */
NSView *fe_amiibo_page(NappleFE *fe);
NSView *fe_camera_page(NappleFE *fe);
void fe_simulators_install(void);
/* textbar.m */
/* The NappleFE behind a public handle (nil for NULL). */
NappleFE *fe_from_handle(struct napple_frontend *f);
void fe_textbar_show(NappleFE *fe, const napple_text_request *req, napple_text_done_fn done, void *user);
BOOL fe_textbar_visible(NappleFE *fe);
/* applet_sheets.m test aids (NAPPLE_SHEET_AUTO / NAPPLE_SHEET_SHOT), also used by the text bar */
BOOL auto_action(NSString **action, double *ms);
void shot_window(NSWindow *w, NSString *name);
void shot_rect(NSRect screen_rect, NSString *name);
/* frontend.m: applet sheets block the game's input while shown */
void fe_sheet_begin(napple_frontend *f);
void fe_sheet_end(napple_frontend *f);
#endif
