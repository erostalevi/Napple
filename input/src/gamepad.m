/* GameController glue: physical pads as plain data, motion, battery and CoreHaptics rumble.

   Everything above this file sees only `ng_pad` snapshots (position based elements, normalised axes), so the
   mapping logic in input.c is portable and testable without hardware.

   Face buttons: GameController names them by the Xbox layout (buttonA at the bottom).  PlayStation, Xbox and other
   pads are therefore mapped by position; Nintendo pads (productCategory/vendorName mention Nintendo/Switch/Joy-Con)
   keep GameController's own labels, so each Switch prompt sits where the player expects.

   Rumble: each Switch vibration device (left, right) is a Core Haptics engine on that handle of the controller
   (GCHapticsLocalityLeftHandle / RightHandle) playing a 30 s continuous event whose intensity and sharpness follow
   the game's value through dynamic parameters (simple pattern players only: the advanced looping player is not
   reliable on game controllers).  The game sends values from its own threads; they only store the latest one and a
   10 ms timer applies it.  A value older than 250 ms counts as silence so a paused game never leaves a pad buzzing. */
#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "internal.h"

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* ------------------------------------------------------------------ pads */
@interface NGPad : NSObject
@property (strong) GCController *controller;
@property (assign) float acc0, acc1, acc2, gyro0, gyro1, gyro2;
@property (assign) uint64_t motionSeq;
@property (assign) BOOL hasMotion;
@end
@implementation NGPad
@end

static NSMutableArray<NGPad *> *g_pads;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static BOOL g_started, g_motion = YES;
static id g_obs[2];

static int is_nintendo(NSString *s) {
    if (!s) return 0;
    NSString *l = s.lowercaseString;
    return [l containsString:@"nintendo"] || [l containsString:@"switch"] || [l containsString:@"joy-con"] || [l containsString:@"joycon"];
}

static void enable_motion(NGPad *p) {
    GCMotion *m = p.controller.motion;
    if (!m) return;
    m.sensorsActive = g_motion;
    __weak NGPad *weak = p;
    m.valueChangedHandler = ^(GCMotion *motion) {
        NGPad *s = weak;
        if (!s) return;
        float ax, ay, az;
        if (motion.hasGravityAndUserAcceleration) {
            ax = (float)(motion.gravity.x + motion.userAcceleration.x);
            ay = (float)(motion.gravity.y + motion.userAcceleration.y);
            az = (float)(motion.gravity.z + motion.userAcceleration.z);
        } else {
            ax = (float)motion.acceleration.x;
            ay = (float)motion.acceleration.y;
            az = (float)motion.acceleration.z;
        }
        pthread_mutex_lock(&g_lock);
        s.acc0 = ax;
        s.acc1 = ay;
        s.acc2 = az;
        if (motion.hasRotationRate) {
            s.gyro0 = (float)motion.rotationRate.x;
            s.gyro1 = (float)motion.rotationRate.y;
            s.gyro2 = (float)motion.rotationRate.z;
        }
        s.motionSeq++;
        s.hasMotion = YES;
        pthread_mutex_unlock(&g_lock);
    };
}

static void attach(GCController *c) {
    if (!c.extendedGamepad) return;
    pthread_mutex_lock(&g_lock);
    for (NGPad *p in g_pads)
        if (p.controller == c) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
    NGPad *p = [NGPad new];
    p.controller = c;
    [g_pads addObject:p];
    pthread_mutex_unlock(&g_lock);
    enable_motion(p);
    napple_input_log("controller connected: %s (%s)", (c.vendorName ?: @"unknown").UTF8String, (c.productCategory ?: @"unknown").UTF8String);
}

static void detach(GCController *c) {
    pthread_mutex_lock(&g_lock);
    for (NSUInteger i = 0; i < g_pads.count; i++)
        if (g_pads[i].controller == c) {
            [g_pads removeObjectAtIndex:i];
            break;
        }
    pthread_mutex_unlock(&g_lock);
    napple_input_log("controller disconnected: %s", (c.vendorName ?: @"unknown").UTF8String);
}

int ng_start(void) {
    @autoreleasepool {
        if (g_started) return 0;
        g_started = YES;
        g_pads = [NSMutableArray new];
        if (@available(macOS 11.3, *)) GCController.shouldMonitorBackgroundEvents = YES; /* focus is gated by napple_input_focus */
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        g_obs[0] = [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:nil usingBlock:^(NSNotification *n) { attach(n.object); }];
        g_obs[1] = [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil usingBlock:^(NSNotification *n) { detach(n.object); }];
        for (GCController *c in GCController.controllers) attach(c);
        [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
    }
    return 0;
}

void ng_stop(void) {
    ng_rumble_quiet();
    @autoreleasepool {
        if (!g_started) return;
        g_started = NO;
        for (int i = 0; i < 2; i++) {
            if (g_obs[i]) [NSNotificationCenter.defaultCenter removeObserver:g_obs[i]];
            g_obs[i] = nil;
        }
        pthread_mutex_lock(&g_lock);
        for (NGPad *p in g_pads) p.controller.motion.valueChangedHandler = nil;
        [g_pads removeAllObjects];
        pthread_mutex_unlock(&g_lock);
    }
}

void ng_set_motion(int enabled) {
    g_motion = enabled != 0;
    pthread_mutex_lock(&g_lock);
    for (NGPad *p in g_pads) p.controller.motion.sensorsActive = g_motion;
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------ snapshot */
int ng_snapshot(ng_pad *out, int max) {
    @autoreleasepool {
        if (!g_started) return 0;
        int n = 0;
        pthread_mutex_lock(&g_lock);
        NSArray<NGPad *> *pads = [g_pads copy];
        pthread_mutex_unlock(&g_lock);
        for (NGPad *p in pads) {
            if (n >= max) break;
            GCController *c = p.controller;
            GCExtendedGamepad *g = c.extendedGamepad;
            if (!g) continue;
            ng_pad *o = &out[n++];
            memset(o, 0, sizeof *o);
            o->connected = 1;
            strlcpy(o->name, (c.vendorName ?: @"Controller").UTF8String, sizeof o->name);
            NSString *cat = c.productCategory ?: @"";
            o->by_label = is_nintendo(cat) || is_nintendo(c.vendorName);
            NSString *l = cat.lowercaseString;
            o->family = [l containsString:@"dualsense"] || [l containsString:@"dualshock"] || [l containsString:@"playstation"] ? 2
                        : [l containsString:@"xbox"]                                                                            ? 1
                                                                                                                                : 0;
            uint32_t e = 0;
#define SET(cond, el)                                                                                                                                          \
    do {                                                                                                                                                       \
        if (cond) e |= 1u << (el);                                                                                                                             \
    } while (0)
            /* GameController buttonA/B/X/Y: bottom, right, left, top (Xbox layout); Nintendo pads: labels A/B/X/Y */
            if (!o->by_label) {
                SET(g.buttonA.isPressed, NAPPLE_PAD_SOUTH);
                SET(g.buttonB.isPressed, NAPPLE_PAD_EAST);
                SET(g.buttonX.isPressed, NAPPLE_PAD_WEST);
                SET(g.buttonY.isPressed, NAPPLE_PAD_NORTH);
            } else {
                SET(g.buttonA.isPressed, NAPPLE_PAD_EAST);
                SET(g.buttonB.isPressed, NAPPLE_PAD_SOUTH);
                SET(g.buttonX.isPressed, NAPPLE_PAD_NORTH);
                SET(g.buttonY.isPressed, NAPPLE_PAD_WEST);
            }
            SET(g.leftThumbstickButton.isPressed, NAPPLE_PAD_L3);
            SET(g.rightThumbstickButton.isPressed, NAPPLE_PAD_R3);
            SET(g.leftShoulder.isPressed, NAPPLE_PAD_LB);
            SET(g.rightShoulder.isPressed, NAPPLE_PAD_RB);
            SET(g.leftTrigger.isPressed || g.leftTrigger.value > 0.5f, NAPPLE_PAD_LT);
            SET(g.rightTrigger.isPressed || g.rightTrigger.value > 0.5f, NAPPLE_PAD_RT);
            SET(g.buttonMenu.isPressed, NAPPLE_PAD_MENU);
            SET(g.buttonOptions.isPressed, NAPPLE_PAD_OPTIONS);
            SET(g.dpad.left.isPressed, NAPPLE_PAD_DLEFT);
            SET(g.dpad.up.isPressed, NAPPLE_PAD_DUP);
            SET(g.dpad.right.isPressed, NAPPLE_PAD_DRIGHT);
            SET(g.dpad.down.isPressed, NAPPLE_PAD_DDOWN);
            o->elements = e;
            {
                uint32_t hot = 0;
                if (@available(macOS 11.0, *)) {
                    if (g.buttonHome && g.buttonHome.isPressed) hot |= 1u;
                }
                GCControllerButtonInput *share = c.physicalInputProfile.buttons[@"Button Share"];
                if (share && share.isPressed) hot |= 2u;
                o->hot = hot;
            }
            o->lx = g.leftThumbstick.xAxis.value;
            o->ly = g.leftThumbstick.yAxis.value;
            o->rx = g.rightThumbstick.xAxis.value;
            o->ry = g.rightThumbstick.yAxis.value;
            o->lt = g.leftTrigger.value;
            o->rt = g.rightTrigger.value;
            float level = c.battery ? c.battery.batteryLevel : 1.f;
            o->battery = level <= 0.05f ? 0 : level <= 0.25f ? 1 : level <= 0.5f ? 2 : level <= 0.8f ? 3 : 4;
            pthread_mutex_lock(&g_lock);
            o->has_motion = p.hasMotion && g_motion;
            o->acc[0] = p.acc0;
            o->acc[1] = p.acc1;
            o->acc[2] = p.acc2;
            o->gyro[0] = p.gyro0;
            o->gyro[1] = p.gyro1;
            o->gyro[2] = p.gyro2;
            o->motion_seq = p.motionSeq;
            pthread_mutex_unlock(&g_lock);
        }
        return n;
    }
}

/* ------------------------------------------------------------------ rumble */
enum { TICK_MS = 10, STALE_MS = 250, HOLD_MS = 150, EVENT_S = 30, RENEW_MS = 25000 };
typedef struct {
    CHHapticEngine *engine;
    id<CHHapticPatternPlayer> player;
    BOOL playing;
    float sentI, sentS;
    uint64_t quietSince, retryAt, playerSince;
} Side;
typedef struct {
    Side side[2];
    float want[2][4];
    uint64_t wantAt[2];
} PadRumble;
static PadRumble g_rumble[NG_MAX_PADS];
static pthread_mutex_t g_want_lock = PTHREAD_MUTEX_INITIALIZER;
static dispatch_queue_t g_rq;
static dispatch_source_t g_timer;
static CHHapticPattern *g_pattern;

static float intensity_of(const float v[4]) {
    float a = sqrtf(v[0] * v[0] + v[2] * v[2]);
    return a > 0 ? (a < 1 ? a : 1) : 0;
}
static float sharpness_of(const float v[4]) {
    float w = v[0] + v[2];
    if (w <= 1e-4f) return 0.5f;
    float f = (v[0] * v[1] + v[2] * v[3]) / w;
    f = f < 40 ? 40 : f > 1250 ? 1250 : f;
    return log2f(f / 40.f) / log2f(1250.f / 40.f);
}
static void forget(Side *s) {
    s->player = nil;
    s->engine = nil;
    s->playing = NO;
    s->sentI = s->sentS = -1;
}
static void failed(Side *s, const char *what, NSError *err) {
    napple_input_log("rumble: %s failed: %s (retry in 5 s)", what, err ? err.localizedDescription.UTF8String : "no reason");
    forget(s);
    s->retryAt = now_ns() + 5000000000ull;
}
static BOOL ensure_engine(GCController *c, int sideIndex, Side *s) {
    if (s->engine) return YES;
    if (!c.haptics || now_ns() < s->retryAt) return NO;
    NSError *err = nil;
    if (!g_pattern) {
        CHHapticEventParameter *i = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
        CHHapticEventParameter *sh = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f];
        CHHapticEvent *ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[ i, sh ] relativeTime:0 duration:EVENT_S];
        g_pattern = [[CHHapticPattern alloc] initWithEvents:@[ ev ] parameters:@[] error:&err];
        if (!g_pattern) {
            failed(s, "building the pattern", err);
            return NO;
        }
    }
    GCHapticsLocality where = sideIndex ? GCHapticsLocalityRightHandle : GCHapticsLocalityLeftHandle;
    if (![c.haptics.supportedLocalities containsObject:where]) where = GCHapticsLocalityDefault;
    CHHapticEngine *e = [c.haptics createEngineWithLocality:where];
    if (!e) {
        failed(s, "creating the engine", nil);
        return NO;
    }
    Side *sp = s;
    e.resetHandler = ^{ dispatch_async(g_rq, ^{ forget(sp); }); };
    e.stoppedHandler = ^(CHHapticEngineStoppedReason r) {
        (void)r;
        dispatch_async(g_rq, ^{ forget(sp); });
    };
    if (![e startAndReturnError:&err]) {
        failed(s, "starting the engine", err);
        return NO;
    }
    s->engine = e;
    return YES;
}
static BOOL send_params(Side *s, float inten, float sharp) {
    NSError *err = nil;
    CHHapticDynamicParameter *i = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                                                                                  value:inten
                                                                           relativeTime:0];
    CHHapticDynamicParameter *sh = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl
                                                                                   value:sharp - 0.5f
                                                                            relativeTime:0];
    if (![s->player sendParameters:@[ i, sh ] atTime:CHHapticTimeImmediate error:&err]) {
        failed(s, "updating the player", err);
        return NO;
    }
    s->sentI = inten;
    s->sentS = sharp;
    return YES;
}
static BOOL start_player(Side *s, float inten, float sharp, uint64_t now) {
    NSError *err = nil;
    id<CHHapticPatternPlayer> p = [s->engine createPlayerWithPattern:g_pattern error:&err];
    if (!p) {
        failed(s, "creating the player", err);
        return NO;
    }
    id<CHHapticPatternPlayer> old = s->player;
    s->player = p;
    if (!send_params(s, inten, sharp)) {
        s->player = old;
        return NO;
    }
    if (![p startAtTime:CHHapticTimeImmediate error:&err]) {
        failed(s, "starting the player", err);
        return NO;
    }
    if (old) [old stopAtTime:CHHapticTimeImmediate error:nil];
    s->playerSince = now_ns();
    (void)now;
    s->playing = YES;
    return YES;
}
static void tick(void) {
    uint64_t now = now_ns();
    NSArray<NGPad *> *pads;
    pthread_mutex_lock(&g_lock);
    pads = [g_pads copy];
    pthread_mutex_unlock(&g_lock);
    for (NSUInteger pi = 0; pi < pads.count && pi < NG_MAX_PADS; pi++) {
        GCController *c = pads[pi].controller;
        for (int si = 0; si < 2; si++) {
            Side *s = &g_rumble[pi].side[si];
            float v[4];
            uint64_t at;
            pthread_mutex_lock(&g_want_lock);
            memcpy(v, g_rumble[pi].want[si], sizeof v);
            at = g_rumble[pi].wantAt[si];
            pthread_mutex_unlock(&g_want_lock);
            float inten = at && now - at < STALE_MS * 1000000ull ? intensity_of(v) : 0, sharp = sharpness_of(v);
            if (inten < 0.02f) {
                if (!s->playing) continue;
                if (s->sentI != 0 && !send_params(s, 0, sharp)) continue;
                if (!s->quietSince) s->quietSince = now;
                else if (now - s->quietSince > HOLD_MS * 1000000ull) {
                    [s->player stopAtTime:CHHapticTimeImmediate error:nil];
                    s->player = nil;
                    s->playing = NO;
                }
                continue;
            }
            s->quietSince = 0;
            if (!ensure_engine(c, si, s)) continue;
            if (!s->playing || now - s->playerSince > RENEW_MS * 1000000ull) {
                start_player(s, inten, sharp, now);
                continue;
            }
            if (fabsf(inten - s->sentI) > 0.01f || fabsf(sharp - s->sentS) > 0.02f) send_params(s, inten, sharp);
        }
    }
}
static void ensure_timer(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_rq = dispatch_queue_create("napple.input.rumble", DISPATCH_QUEUE_SERIAL);
        g_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_rq);
        dispatch_source_set_timer(g_timer, DISPATCH_TIME_NOW, TICK_MS * NSEC_PER_MSEC, NSEC_PER_MSEC);
        dispatch_source_set_event_handler(g_timer, ^{
            @autoreleasepool {
                tick();
            }
        });
        dispatch_resume(g_timer);
    });
}
void ng_rumble(int pad, int side, const napple_vibration *v) {
    if (pad < 0 || pad >= NG_MAX_PADS || side < 0 || side > 1 || !v) return;
    ensure_timer();
    pthread_mutex_lock(&g_want_lock);
    float *w = g_rumble[pad].want[side];
    w[0] = v->amp_low;
    w[1] = v->freq_low;
    w[2] = v->amp_high;
    w[3] = v->freq_high;
    g_rumble[pad].wantAt[side] = now_ns();
    pthread_mutex_unlock(&g_want_lock);
}
void ng_rumble_quiet(void) {
    pthread_mutex_lock(&g_want_lock);
    for (int i = 0; i < NG_MAX_PADS; i++) g_rumble[i].wantAt[0] = g_rumble[i].wantAt[1] = 0;
    pthread_mutex_unlock(&g_want_lock);
}
