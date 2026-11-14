/* The overlay's Audio/Video, FPS/Speed, Game/System and Status pages.  Every page is a stack of cards built with fe_ui.h; every control
   edits napple_settings through fe_overlay_update (copy, mutate, apply + save + notify), so persistence is identical to before. */
#include <stdlib.h>
#include <string.h>
#include "fe_internal.h"
#include "fe_ui.h"
#include "overlay_logic.h"
#if __has_include(<MetalFX/MetalFX.h>)
#import <MetalFX/MetalFX.h>
#endif

static NSArray<NSString *> *kScales, *kFps, *kAspect, *kRenderMode, *kSuper, *kUpscaler, *kFilter, *kAniso, *kFgMult, *kFgMethod, *kSpeedMode, *kAudioSpeed;
static const int kScaleVals[] = {25, 50, 75, 100, 150, 200, 300, 400};
static const int kSuperVals[] = {100, 150, 200, 300, 400};
static const int kAnisoVals[] = {0, 2, 4, 8, 16};
static const int kFgMultVals[] = {0, 2, 3, 4, 6, 8};

static void init_names(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        kScales = @[ @"25%", @"50%", @"75%", @"100% (native)", @"150%", @"200%", @"300%", @"400%" ];
        kFps = @[ @"Game pacing (real time)", @"Match display", @"30 Hz", @"60 Hz", @"90 Hz", @"120 Hz", @"144 Hz", @"240 Hz", @"Uncapped" ];
        kAspect = @[
            @"Locked 16:9 (native)", @"Locked 16:9", @"Locked 4:3", @"Free: stretch the image", @"Free: keep aspect (bars)", @"Free: fill (crop)",
            @"Free: match window aspect", @"Free: whole-number multiple (bars)"
        ];
        kRenderMode = @[ @"Native size", @"Scale factor", @"Follow window", @"Fixed size" ];
        kSuper = @[ @"Off (1x)", @"1.5x", @"2x", @"3x", @"4x" ];
        kUpscaler = @[ @"None (filter only)", @"MetalFX spatial", @"Built-in edge adaptive", @"CAS (resample + sharpen)" ];
        kFilter = @[ @"Bilinear", @"Nearest", @"Box (area average)", @"Bicubic", @"Lanczos", @"Sharpened bicubic" ];
        kAniso = @[ @"As the game asks", @"2x", @"4x", @"8x", @"16x" ];
        kFgMult = @[ @"Automatic (fill the display)", @"2x", @"3x", @"4x", @"6x", @"8x" ];
        kFgMethod = @[ @"Best available", @"Optical flow (VideoToolbox)", @"Optical flow, low latency", @"Motion-compensated (Metal)", @"Plain blend" ];
        kSpeedMode = @[ @"Game clock and vsync", @"Vsync only (frame skip)", @"Game clock only" ];
        kAudioSpeed = @[ @"Automatic", @"Always pitch shift", @"Silent when not 1x" ];
    });
}
static NSInteger index_of(const int *vals, int n, int v, NSInteger fallback) {
    for (int i = 0; i < n; i++)
        if (vals[i] == v) return i;
    return fallback;
}
static BOOL metalfx_available(void) {
#if __has_include(<MetalFX/MetalFX.h>)
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    return d && [MTLFXSpatialScalerDescriptor supportsDevice:d];
#else
    return NO;
#endif
}
static NSTextField *num_field(NSString *text, NSString *placeholder, CGFloat w) {
    NSTextField *f = [NSTextField textFieldWithString:text ?: @""];
    f.controlSize = NSControlSizeSmall;
    f.font = [NSFont monospacedDigitSystemFontOfSize:11.5 weight:NSFontWeightRegular];
    f.alignment = NSTextAlignmentCenter;
    f.placeholderString = placeholder;
    [f.widthAnchor constraintEqualToConstant:w].active = YES;
    return f;
}
/* "W x H" pair of fields; blk(w, h) on Return / leaving the field. */
static NSView *size_fields(int w, int h, void (^blk)(int, int)) {
    NSTextField *a = num_field(w ? @(w).stringValue : @"", @"width", 64), *b = num_field(h ? @(h).stringValue : @"", @"height", 64);
    void (^go)(id) = ^(id x) {
        (void)x;
        blk(a.intValue, b.intValue);
    };
    fe_bind(a, go);
    fe_bind(b, go);
    return fe_hstack(5, @[ a, fe_text(@"x", FETypeBody, NSColor.secondaryLabelColor), b ]);
}

/* ============================================================== Audio / Video ============================================================== */
NSView *fe_page_video(NappleFE *fe) {
    init_names();
    napple_settings s = [fe currentSettings];
    NSMutableArray *sec = [NSMutableArray new];

    [sec addObject:fe_card(@"Audio", @"speaker.wave.2.fill", @[
        fe_slider_row(@"Volume", 0, 100, s.master_volume * 100, @"%.0f%%", YES, @"Master volume of the game (the system volume is never touched).",
                      ^(double v) { fe_overlay_update(fe, ^(napple_settings *t) { t->master_volume = (float)(v / 100); }); }),
        fe_switch_row(@"Play audio in the background", @"Keep the sound when this window loses focus.", !s.mute_in_background, nil,
                      ^(BOOL on) { fe_overlay_update(fe, ^(napple_settings *t) { t->mute_in_background = !on; }); })
    ])];

    NSMutableArray *win = [NSMutableArray new];
    [win addObject:fe_popup_row(@"Window shape", kAspect, s.aspect, @"How the picture fits the window. Locked shapes resize the window; free shapes let you drag it to any size.",
                                ^(NSInteger i) { fe_overlay_update(fe, ^(napple_settings *t) { t->aspect = (int)i; }); })];
    [win addObject:fe_row(@"Window size", size_fields(s.output_width, s.output_height, ^(int w, int h) {
                              fe_overlay_update(fe, ^(napple_settings *t) {
                                  t->output_width = w;
                                  t->output_height = h;
                              });
                          }),
                          @"Content size in points when windowed. 0 x 0 = the default 1280 x 720.")];
    [win addObject:fe_switch_row(@"Full screen", @"Cmd+Ctrl+F or Cmd+Return also toggles it.", s.fullscreen, nil,
                                 ^(BOOL on) { fe_overlay_update(fe, ^(napple_settings *t) { t->fullscreen = on; }); })];
    [win addObject:fe_switch_row(@"Show mouse cursor", nil, s.show_touch_cursor, @"Keep the mouse cursor visible over the game (touch-screen games).",
                                 ^(BOOL on) { fe_overlay_update(fe, ^(napple_settings *t) { t->show_touch_cursor = on; }); })];
    [sec addObject:fe_card(@"Window", @"macwindow", win)];

    /* render target */
    NSMutableArray *rt = [NSMutableArray new];
    {
        static const int pv[4] = {50, 100, 200, 400};
        static const int tw[2] = {2560, 3840}, th[2] = {1440, 2160};
        NSInteger sel = -1;
        for (int i = 0; i < 4; i++)
            if (s.render_mode == 1 && s.internal_scale_percent == pv[i]) sel = i;
        for (int i = 0; i < 2; i++)
            if (s.render_mode == 3 && s.internal_width == tw[i] && s.internal_height == th[i]) sel = 4 + i;
        NSSegmentedControl *seg = fe_segments(@[ @"0.5x", @"1x", @"2x", @"4x", @"1440p", @"4K" ], sel, ^(NSInteger i) {
            fe_overlay_update(fe, ^(napple_settings *t) {
                if (i < 4) {
                    t->render_mode = 1;
                    t->internal_scale_percent = pv[i];
                } else {
                    t->render_mode = 3;
                    t->internal_width = tw[i - 4];
                    t->internal_height = th[i - 4];
                }
            });
            fe_overlay_refresh(fe);
        });
        seg.toolTip = @"Quick presets: a multiple of the game's native render size, or a fixed 1440p / 4K target.";
        [rt addObject:seg];
    }
    [rt addObject:fe_popup_row(@"Render at", kRenderMode, s.render_mode, @"The size the game draws at before the picture is filtered or upscaled to the window.", ^(NSInteger i) {
        fe_overlay_update(fe, ^(napple_settings *t) { t->render_mode = (int)i; });
        fe_overlay_refresh(fe);
    })];
    if (s.render_mode == 1) {
        [rt addObject:fe_popup_row(@"Scale", kScales, index_of(kScaleVals, 8, s.internal_scale_percent, 3), nil, ^(NSInteger i) {
            fe_overlay_update(fe, ^(napple_settings *t) { t->internal_scale_percent = kScaleVals[i]; });
            fe_overlay_refresh(fe);
        })];
        NSTextField *cp = num_field(@(s.internal_scale_percent).stringValue, @"%", 64);
        fe_bind(cp, ^(id x) {
            (void)x;
            int v = fe_parse_percent(cp.stringValue.UTF8String, 10, 800, s.internal_scale_percent);
            fe_overlay_update(fe, ^(napple_settings *t) {
                t->render_mode = 1;
                t->internal_scale_percent = v;
            });
            fe_overlay_refresh(fe);
        });
        [rt addObject:fe_row(@"Custom scale", fe_hstack(6, @[ cp, fe_text(@"% (10 - 800)", FETypeCaption, NSColor.secondaryLabelColor) ]),
                             @"Any value from 10% to 800%, applied live.")];
    } else if (s.render_mode == 3) {
        [rt addObject:fe_row(@"Size", size_fields(s.internal_width, s.internal_height, ^(int w, int h) {
                                fe_overlay_update(fe, ^(napple_settings *t) {
                                    t->internal_width = w;
                                    t->internal_height = h;
                                });
                            }),
                             @"Render target in pixels, whatever the game's native size is.")];
    }
    [rt addObject:fe_popup_row(@"Supersampling", kSuper, index_of(kSuperVals, 5, s.supersample_percent, 0),
                               @"Render larger than the chosen size and filter down to the window (F10 cycles it).",
                               ^(NSInteger i) { fe_overlay_update(fe, ^(napple_settings *t) { t->supersample_percent = kSuperVals[i]; }); })];
    [rt addObject:fe_note(@"Render size changes rebuild the game's screen-sized buffers on the fly. If a game shows artifacts afterwards, restart it.")];
    [sec addObject:fe_card(@"Resolution", @"rectangle.expand.vertical", rt)];

    NSMutableArray *up = [NSMutableArray new];
    [up addObject:fe_popup_row(@"Upscaler", kUpscaler, s.upscaler, @"Render below the window size and scale up. MetalFX falls back to the built-in upscaler where the OS has none.",
                               ^(NSInteger i) {
                                   fe_overlay_update(fe, ^(napple_settings *t) { t->upscaler = (int)i; });
                                   fe_overlay_refresh(fe);
                               })];
    if (s.upscaler == 1 && !metalfx_available())
        [up addObject:fe_note(@"MetalFX is not available on this Mac or build: the built-in upscaler is used instead. (MetalFX temporal needs depth and motion "
                              @"vectors from the game, so only the spatial scalers are offered.)")];
    [up addObject:fe_popup_row(@"Output filter", kFilter, s.filter, nil, ^(NSInteger i) { fe_overlay_update(fe, ^(napple_settings *t) { t->filter = (int)i; }); })];
    [up addObject:fe_slider_row(@"Sharpness", 0, 100, s.sharpness, @"%.0f", NO, @"Strength of the sharpening (sharpened filter, MetalFX and CAS).",
                                ^(double v) { fe_overlay_update(fe, ^(napple_settings *t) { t->sharpness = (int)v; }); })];
    [up addObject:fe_popup_row(@"Anisotropic", kAniso, index_of(kAnisoVals, 5, s.anisotropy, 0), @"Texture filtering at grazing angles: at least this many samples.",
                               ^(NSInteger i) { fe_overlay_update(fe, ^(napple_settings *t) { t->anisotropy = kAnisoVals[i]; }); })];
    [sec addObject:fe_card(@"Upscaling and filtering", @"sparkles", up)];
    return fe_scroll_page(sec);
}

/* ============================================================== FPS / Speed ============================================================== */
NSView *fe_page_speed(NappleFE *fe) {
    init_names();
    napple_settings s = [fe currentSettings];
    NSMutableArray *sec = [NSMutableArray new];

    NSMutableArray *fr = [NSMutableArray new];
    [fr addObject:fe_popup_row(@"Present rate", kFps, napple_fps_choice(&s),
                               @"How often your Mac shows a new image. It never changes game speed: the game keeps its own cadence and the display repeats the last frame in between.",
                               ^(NSInteger i) { fe_overlay_update(fe, ^(napple_settings *t) { napple_fps_choice_apply(t, (int)i); }); })];
    [fr addObject:fe_switch_row(@"VSync", @"Present on the display's refresh (no tearing).", s.vsync, nil,
                                ^(BOOL on) { fe_overlay_update(fe, ^(napple_settings *t) { t->vsync = on; }); })];
    [fr addObject:fe_switch_row(@"Frame generation", @"Show in-between frames (F8). Adds about half a game frame of latency.", s.frame_gen, nil, ^(BOOL on) {
        fe_overlay_update(fe, ^(napple_settings *t) { t->frame_gen = on; });
        fe_overlay_refresh(fe);
    })];
    NSView *mult = fe_popup_row(@"Images per frame", kFgMult, index_of(kFgMultVals, 6, s.frame_gen_mult, 0), nil,
                                ^(NSInteger i) { fe_overlay_update(fe, ^(napple_settings *t) { t->frame_gen_mult = kFgMultVals[i]; }); });
    NSView *meth = fe_popup_row(@"Method", kFgMethod, s.frame_gen_method, @"Optical flow gives the smoothest result; blending is the cheapest.",
                                ^(NSInteger i) { fe_overlay_update(fe, ^(napple_settings *t) { t->frame_gen_method = (int)i; }); });
    if (s.frame_gen) {
        [fr addObject:mult];
        [fr addObject:meth];
    }
    [sec addObject:fe_card(@"Frame rate", @"film", fr)];

    char sp[24];
    fe_format_speed(s.speed_percent, sp, sizeof sp);
    NSMutableArray *gs = [NSMutableArray new];
    {
        static const int pv[5] = {25, 50, 100, 200, 400};
        NSInteger sel = -1;
        for (int i = 0; i < 5; i++)
            if (s.speed_percent == pv[i]) sel = i;
        NSSegmentedControl *seg = fe_segments(@[ @"0.25x", @"0.5x", @"1x", @"2x", @"4x" ], sel, ^(NSInteger i) {
            fe_overlay_update(fe, ^(napple_settings *t) { t->speed_percent = pv[i]; });
            fe_overlay_refresh(fe);
        });
        [gs addObject:seg];
    }
    [gs addObject:fe_slider_row(@"Speed", 10, 400, MIN(s.speed_percent, 400), @"%.0f%%", NO, @"How fast game time passes (F5 / F6 step it, F7 resets).", ^(double v) {
        fe_overlay_update(fe, ^(napple_settings *t) { t->speed_percent = (int)v; });
        fe_overlay_refresh(fe);
    })];
    [gs addObject:fe_slider_row(@"Turbo speed", 110, 1000, s.turbo_percent, @"%.0f%%", NO, @"Speed while the turbo key (F2, hold) is held or toggled (F3).",
                                ^(double v) { fe_overlay_update(fe, ^(napple_settings *t) { t->turbo_percent = (int)v; }); })];
    [gs addObject:fe_popup_row(@"Mode", kSpeedMode, s.speed_mode,
                               @"Game clock and vsync is right for almost every game. Vsync only is a frame-skip turbo that leaves the clocks real. Game clock only scales the clocks but not the vsync.",
                               ^(NSInteger i) { fe_overlay_update(fe, ^(napple_settings *t) { t->speed_mode = (int)i; }); })];
    [gs addObject:fe_popup_row(@"Audio", kAudioSpeed, s.audio_speed_mode, @"Audio is pitch-shifted between 0.5x and 2x and silent outside that range.",
                               ^(NSInteger i) { fe_overlay_update(fe, ^(napple_settings *t) { t->audio_speed_mode = (int)i; }); })];
    [gs addObject:fe_note(@"Game speed scales every game clock, timer and the virtual vsync together, so animation, physics and cutscenes run faster or slower. "
                          @"Saving, loading and movies keep working.")];
    [sec addObject:fe_card([NSString stringWithFormat:@"Game speed (%s)", sp], @"gauge.with.dots.needle.67percent", gs)];

    static const char *const keys[][2] = {
        {"F2  or  `", "Hold for turbo"}, {"F3", "Toggle turbo"}, {"F4", "Slow motion (0.5x)"}, {"F5  or  [", "Slower, one step"}, {"F6  or  ]", "Faster, one step"},
        {"F7  or  \\", "Back to 1x"}, {"F8", "Frame generation on / off"}, {"F9", "Performance HUD"}, {"F10", "Supersampling 1x - 3x"}, {"F12", "Screenshot"}
    };
    NSMutableArray *grid = [NSMutableArray new];
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        [grid addObject:@[ fe_keycap(@(keys[i][0])), fe_text(@(keys[i][1]), FETypeBody, nil) ]];
    NSGridView *g = fe_grid(grid);
    [g columnAtIndex:0].width = 86;
    [sec addObject:fe_card(@"Hot keys", @"keyboard", @[ g, fe_note(@"On a laptop keyboard the function keys may need the Fn key. The ` [ ] \\ keys always work.") ])];
    return fe_scroll_page(sec);
}

/* ============================================================== Game / System ============================================================== */
static NSString *game_dir(NappleFE *fe) {
    char path[1100];
    napple_data_path(fe.cfg.config_dir, fe.titleID.UTF8String, "x", path, sizeof path);
    return [@(path) stringByDeletingLastPathComponent];
}
static NSString *mods_dir(NappleFE *fe, NSString *sub) {
    NSString *base = game_dir(fe);
    return sub.length ? [base stringByAppendingPathComponent:sub] : base;
}
static NSMutableSet<NSString *> *enabled_set(NappleFE *fe) {
    NSData *d = [NSData dataWithContentsOfFile:[mods_dir(fe, nil) stringByAppendingPathComponent:@"mods-enabled.json"]];
    NSArray *a = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL] : nil;
    return [NSMutableSet setWithArray:[a isKindOfClass:NSArray.class] ? a : @[]];
}
static void save_enabled(NappleFE *fe, NSSet *set) {
    NSString *dir = mods_dir(fe, nil);
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
    NSData *d = [NSJSONSerialization dataWithJSONObject:[set.allObjects sortedArrayUsingSelector:@selector(compare:)] options:NSJSONWritingPrettyPrinted error:NULL];
    [d writeToFile:[dir stringByAppendingPathComponent:@"mods-enabled.json"] atomically:YES];
}
static void open_dir(NSString *dir, NSArray *make) {
    for (NSString *sub in make) [NSFileManager.defaultManager createDirectoryAtPath:[dir stringByAppendingPathComponent:sub] withIntermediateDirectories:YES attributes:nil error:NULL];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
    [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:dir]];
}
/* A card listing the files of mods/ or cheats/ as switches; an empty state with an "open folder" button when there are none. */
static NSView *toggles_card(NappleFE *fe, NSString *title, NSString *symbol, NSString *sub, NSString *kind, NSString *what) {
    NSString *dir = mods_dir(fe, sub);
    NSArray *names = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:NULL] sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    NSMutableSet *on = enabled_set(fe);
    NSMutableArray *rows = [NSMutableArray new];
    for (NSString *name in names) {
        if ([name hasPrefix:@"."]) continue;
        NSString *key = [sub stringByAppendingPathComponent:name];
        [rows addObject:fe_switch_row(name.stringByDeletingPathExtension, nil, [on containsObject:key], name, ^(BOOL en) {
            NSMutableSet *e = enabled_set(fe);
            if (en) [e addObject:key];
            else [e removeObject:key];
            save_enabled(fe, e);
        })];
    }
    if (!rows.count) {
        [rows addObject:fe_empty(symbol, [NSString stringWithFormat:@"No %@ yet", kind], what)];
    } else
        [rows addObject:fe_note(@"Changes apply when the game restarts.")];
    [rows addObject:fe_leading(fe_button(@"Open folder", @"folder", FEButtonNormal, ^{ open_dir(dir, @[]); }))];
    return fe_card(title, symbol, rows);
}

NSView *fe_page_game(NappleFE *fe) {
    napple_settings s = [fe currentSettings];
    NSMutableArray *sec = [NSMutableArray new];

    /* game */
    NSMutableArray *gm = [NSMutableArray new];
    if (fe.manifestTitle) {
        NSTextField *t = fe_text(fe.manifestTitle, FETypeHeadline, nil);
        t.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
        NSStackView *chips = fe_hstack(6, @[ fe_chip([@"v" stringByAppendingString:fe.manifestBase ?: @"-"], nil), fe_chip(fe.titleID ?: @"-", nil) ]);
        NSView *sp = [NSView new];
        [sp setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [chips addArrangedSubview:sp];
        [gm addObject:t];
        [gm addObject:chips];
        NSMutableArray *items = [NSMutableArray arrayWithObject:@"Latest installed"];
        NSInteger sel = 0;
        for (NSDictionary *u in fe.updates) {
            [items addObject:[NSString stringWithFormat:@"%@%@", u[@"version"], [u[@"name"] length] ? [@"  " stringByAppendingString:u[@"name"]] : @""]];
            if (!strcmp(s.update_version, [u[@"version"] UTF8String])) sel = (NSInteger)items.count - 1;
        }
        [gm addObject:fe_popup_row(@"Update", items, sel, @"Which installed update the game starts with. Takes effect when the game restarts.", ^(NSInteger i) {
            napple_settings tt = [fe currentSettings];
            fe_update_choose(&tt, i == 0 ? NULL : [fe.updates[i - 1][@"version"] UTF8String]);
            [fe applySettings:tt save:YES notify:NO];
        })];
    } else {
        [gm addObject:fe_empty(@"shippingbox", @"No game information", @"The game's manifest has not been loaded yet.")];
    }
    [sec addObject:fe_card(@"Game", @"gamecontroller.fill", gm)];

    if (fe.manifestTitle) {
        NSMutableArray *dl = [NSMutableArray new];
        if (!fe.dlc.count) [dl addObject:fe_empty(@"puzzlepiece.extension", @"No downloadable content", @"DLC found when the game was built shows up here as switches.")];
        for (NSDictionary *d in fe.dlc) {
            NSString *did = d[@"id"];
            NSString *title = [d[@"name"] length] ? d[@"name"] : did;
            [dl addObject:fe_switch_row(title, [@"Title ID " stringByAppendingString:did], !fe_dlc_is_disabled(&s, did.UTF8String), nil, ^(BOOL on) {
                       napple_settings tt = [fe currentSettings];
                       if (fe_dlc_set_enabled(&tt, did.UTF8String, on)) [fe applySettings:tt save:YES notify:NO];
                   })];
        }
        [dl addObject:fe_note(@"Changes to DLC and the update version take effect when the game restarts.")];
        [dl addObject:fe_button(@"Apply and restart game", @"arrow.clockwise", FEButtonProminent, ^{
            [fe showOverlay:NO];
            [fe restartGuest];
        })];
        [sec addObject:fe_card(@"Downloadable content", @"puzzlepiece.extension.fill", dl)];
    }

    /* saves */
    NSString *saves = [game_dir(fe) stringByAppendingPathComponent:@"save"];
    NSTextField *path = fe_text([saves stringByAbbreviatingWithTildeInPath], FETypeMono, NSColor.secondaryLabelColor);
    path.lineBreakMode = NSLineBreakByTruncatingMiddle;
    path.toolTip = saves;
    path.selectable = YES;
    [sec addObject:fe_card(@"Saves", @"externaldrive.fill", @[
        path,
        fe_leading(
            fe_button(@"Show in Finder", @"folder", FEButtonNormal, ^{
                BOOL isdir = NO;
                if ([NSFileManager.defaultManager fileExistsAtPath:saves isDirectory:&isdir] && isdir) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:saves] ]];
                else open_dir(game_dir(fe), @[]);
            })),
        fe_note(@"Saves stay on this Mac, one folder per user. Copy the folder to back them up or to move them to another Mac.")
    ])];

    [sec addObject:toggles_card(fe, @"Mods", @"puzzlepiece.fill", @"mods", @"mods", @"Drop mod folders into the folder below, then switch them on here.")];
    [sec addObject:toggles_card(fe, @"Cheats", @"wand.and.stars", @"cheats", @"cheat files", @"Put cheat files (.txt) into the folder below, then switch them on here.")];

    [sec addObject:fe_card(@"Mii", @"person.crop.circle.fill", @[ fe_note(@"Every game reads the same Mii database. The Mii editor and picker run from the firmware you built the game with. "
                                                                         @"Back up, export or delete Miis in Napple Studio > Settings > Mii.") ])];
    [sec addObject:fe_card(@"amiibo", @"person.crop.square.fill", @[ fe_amiibo_page(fe) ])];
    [sec addObject:fe_card(@"IR camera", @"camera.fill", @[ fe_camera_page(fe) ])];

    /* actions */
    [sec addObject:fe_card(@"Session", @"power", @[
        fe_row(@"Screenshot", fe_button(@"Take screenshot", @"camera", FEButtonNormal, ^{
                   [fe showOverlay:NO];
                   dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ [fe screenshot]; });
               }),
               @"Saves a PNG of the game (without this panel) to ~/Pictures/Napple. F12 does the same."),
        fe_row(@"Restart", fe_confirm_button(@"Restart game", @"arrow.clockwise", @"Restart now?", @"Restart", ^{
                   [fe showOverlay:NO];
                   [fe restartGuest];
               }),
               @"Restart the game from the title screen. Unsaved progress is lost."),
        fe_row(@"Display", fe_confirm_button(@"Reset video and speed", @"arrow.counterclockwise", @"Reset all?", @"Reset", ^{
                   fe_overlay_update(fe, ^(napple_settings *t) { fe_reset_display_speed(t); });
                   [fe toast:@"Video and speed settings reset" symbol:@"arrow.counterclockwise"];
                   fe_overlay_refresh(fe);
               }),
               @"Back to the default window, render, filter, frame rate, speed and HUD settings. Audio, update and DLC choices are kept."),
        fe_row(@"Quit", fe_confirm_button(@"Quit game", @"power", @"Quit now?", @"Quit", ^{ [fe quit]; }), @"Close the game. Unsaved progress is lost.")
    ])];
    return fe_scroll_page(sec);
}

/* ============================================================== Status ============================================================== */
static NSString *build_string(void) {
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    NSString *v = info[@"CFBundleShortVersionString"], *b = info[@"CFBundleVersion"];
    NSString *date = @(__DATE__);
    if (v.length) return b.length && ![b isEqual:v] ? [NSString stringWithFormat:@"%@ (%@)  -  %@", v, b, date] : [NSString stringWithFormat:@"%@  -  %@", v, date];
    return [NSString stringWithFormat:@"development build  -  %@", date];
}
static NSString *render_string(const napple_settings *s) {
    char b[48];
    switch (s->render_mode) {
        case 1: fe_format_scale(s->internal_scale_percent, b, sizeof b); return [NSString stringWithFormat:@"%s of native", b];
        case 2: return @"Follows the window";
        case 3: if (s->internal_width && s->internal_height) { fe_format_resolution(s->internal_width, s->internal_height, b, sizeof b); return @(b); } return @"Fixed size";
        default: return @"Native size";
    }
}
NSView *fe_page_status(NappleFE *fe) {
    napple_settings s = [fe currentSettings];
    NSMutableArray *sec = [NSMutableArray new];

    NSTextField *fps = fe_text(@"", FETypeMono, nil), *win = fe_text(@"", FETypeMono, nil), *spd = fe_text(@"", FETypeMono, nil);
    fps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSString *title = fe.manifestTitle ?: fe.titleName ?: @"Napple";
    NSTextField *tt = fe_text(title, FETypeHeadline, nil);
    tt.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    [sec addObject:fe_card(@"Now playing", @"waveform.path.ecg", @[
        tt,
        fe_row(@"Frame rate", fps, @"Live frame rate, GPU time and latency, as in the performance HUD."),
        fe_row(@"Window", win, @"Size of the drawable in pixels, and the display's refresh rate."),
        fe_row(@"Render", fe_text(render_string(&s), FETypeMono, nil), @"The size the game draws at."),
        fe_row(@"Game speed", spd, nil),
        fe_row(@"Build", fe_text(build_string(), FETypeMono, NSColor.secondaryLabelColor), nil)
    ])];
    __weak NappleFE *wfe = fe;
    void (^tick)(void) = ^{
        NappleFE *f = wfe;
        if (!f) return;
        char buf[400] = "";
        if (f.cfg.callbacks.stats_text) f.cfg.callbacks.stats_text(f.cfg.callbacks.user, buf, sizeof buf);
        NSString *line = [[@(buf) stringByReplacingOccurrencesOfString:@"\n" withString:@"   "] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        fps.stringValue = line.length ? line : (f.cfg.callbacks.stats_text ? @"waiting for frames..." : @"not reported by this build");
        CGSize d = f.layer.drawableSize;
        char r[40];
        fe_format_resolution((int)d.width, (int)d.height, r, sizeof r);
        win.stringValue = [NSString stringWithFormat:@"%s @ %.0f Hz", r, f.displayHz >= 24 ? f.displayHz : 60.0];
        napple_settings cur = [f currentSettings];
        char sp[24];
        fe_format_speed(cur.speed_percent, sp, sizeof sp);
        spd.stringValue = @(sp);
    };
    tick();
    fe_overlay_set_tick(0.25, tick);

    [sec addObject:fe_card(@"Performance HUD", @"speedometer", @[
        fe_switch_row(@"Show HUD (F9)", @"Frame rate, GPU time and latency in a corner of the game view.", s.show_stats, nil, ^(BOOL on) {
            fe_overlay_update(fe, ^(napple_settings *t) { t->show_stats = on; });
            [fe toast:on ? @"Performance HUD on" : @"Performance HUD off" symbol:@"speedometer"];
        }),
        fe_popup_row(@"Corner", @[ @"Top left", @"Top right", @"Bottom left", @"Bottom right" ], s.stats_corner, nil,
                     ^(NSInteger i) { fe_overlay_update(fe, ^(napple_settings *t) { t->stats_corner = (int)i; }); }),
        fe_row(@"Text size", fe_segments(@[ @"Small", @"Medium", @"Large" ], s.stats_size, ^(NSInteger i) { fe_overlay_update(fe, ^(napple_settings *t) { t->stats_size = (int)i; }); }), nil),
        fe_slider_row(@"Background", 10, 100, s.stats_opacity, @"%.0f%%", NO, @"Opacity of the HUD's background.",
                      ^(double v) { fe_overlay_update(fe, ^(napple_settings *t) { t->stats_opacity = (int)v; }); }),
        fe_switch_row(@"Metal HUD", @"Apple's own overlay: GPU and frame time, memory, shader compiles.", s.metal_hud, nil,
                      ^(BOOL on) { fe_overlay_update(fe, ^(napple_settings *t) { t->metal_hud = on; }); })
    ])];

    [sec addObject:fe_players_card(fe, NO)];

    [sec addObject:fe_card(@"Panel", @"sidebar.right", @[
        fe_switch_row(@"Game input stays on", @"Keep sending the keyboard and mouse to the game while this panel is open. A panel opened with the controller always takes the pad.",
                      s.panel_passthrough, nil, ^(BOOL on) {
                          fe_overlay_update(fe, ^(napple_settings *t) { t->panel_passthrough = on; });
                          [fe refreshOverlayInputMode];
                      }),
        fe_grid(@[
            @[ fe_keycap(@"F1  /  Cmd ,"), fe_text(@"Open or close this panel", FETypeBody, nil) ],
            @[ fe_keycap(@"Cmd 1 - 5"), fe_text(@"Switch tab", FETypeBody, nil) ],
            @[ fe_keycap(@"Cmd 0"), fe_text(@"Move keyboard focus into the panel", FETypeBody, nil) ],
            @[ fe_keycap(@"Home"), fe_text(@"Controller: open / close; D-pad, A, B, L, R to navigate", FETypeBody, nil) ]
        ])
    ])];
    return fe_scroll_page(sec);
}
