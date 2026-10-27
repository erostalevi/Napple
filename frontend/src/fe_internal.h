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
- (void)toast:(NSString *)text;            /* transient message in the corner of the game view */
@end

/* overlay.m */
NSView *fe_build_overlay(NappleFE *fe);
void fe_overlay_place(NappleFE *fe);
void fe_nav_start(NappleFE *fe);
void fe_nav_mode_changed(void);
void fe_nav_page_changed(void);
void fe_overlay_refresh(NappleFE *fe);
void fe_overlay_select_tab(int index);   /* 0 Video, 1 Speed, 2 Audio, 3 Controls, 4 Game, 5 amiibo, 6 Camera */
/* simulators.m: amiibo picker and IR camera pages of the overlay; the webcam IR source is installed on first use */
NSView *fe_amiibo_page(NappleFE *fe);
NSView *fe_camera_page(NappleFE *fe);
void fe_simulators_install(void);
/* textbar.m */
void fe_textbar_show(NappleFE *fe, const napple_text_request *req, napple_text_done_fn done, void *user);
BOOL fe_textbar_visible(NappleFE *fe);
#endif
