/* Shared declarations of the iOS frontend (frontend_ios.m) and its settings panel (panel_ios.m). */
#ifndef NAPPLE_FE_IOS_H
#define NAPPLE_FE_IOS_H
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include "internal.h"
#include "napple_audio.h"
#include "napple_input.h"
#include "napple_frontend.h"

/* per-device conveniences that are not part of the portable settings file */
static inline BOOL pref_bool(NSString *k, BOOL d) {
    id v = [NSUserDefaults.standardUserDefaults objectForKey:k];
    return v ? [v boolValue] : d;
}
static inline float pref_float(NSString *k, float d) {
    id v = [NSUserDefaults.standardUserDefaults objectForKey:k];
    return v ? [v floatValue] : d;
}

@interface NappleFEiOS : NSObject
@property (nonatomic) napple_frontend_config cfg;
@property (nonatomic, copy) NSString *titleID, *titleName, *dir, *shotDir;
@property (nonatomic, strong) CAMetalLayer *layer;
@property (nonatomic) napple_settings settings;
@property (nonatomic) napple_input *input;
@property (nonatomic) double displayHz;
@property (nonatomic) BOOL readyFired, quitting, overlayVisible;
@property (nonatomic, strong) NSArray<NSDictionary *> *dlc, *updates;
@property (nonatomic, copy) NSString *manifestTitle, *manifestBase;
@property (nonatomic, weak) UIViewController *rootVC;
- (napple_settings)currentSettings;
- (void)applySettings:(napple_settings)s save:(BOOL)save notify:(BOOL)notify;
- (void)mutate:(void (^)(napple_settings *))m;
- (void)updateSpeed;
- (void)toast:(NSString *)text;
- (void)toggleSettings;
- (void)toggleSettingsFromPad:(BOOL)pad;
- (void)refreshPanelInputMode;
- (void)showSettings:(BOOL)show;
- (void)screenshot;
- (void)restartGuest;
- (void)quit;
- (void)focus:(BOOL)on;
- (BOOL)hardwareKey:(int)usage down:(BOOL)down;
- (void)boot:(NSString *)stage fraction:(double)f;
- (void)bootDone;
- (void)compileNotice:(NSString *)text;
- (void)framePresented;
@end

/* The floating settings panel (panel_ios.m): four tabs like the Mac overlay, pinned to the trailing edge of the game view. */
@interface NappleIOSPanel : UIView
- (instancetype)initWithFE:(NappleFEiOS *)fe;
@property (nonatomic) BOOL padMode; /* opened with a physical controller's Home button: the pad navigates the panel */
- (void)reload;
- (void)selectTab:(NSInteger)i;
@end
#endif
