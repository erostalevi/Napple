/* Process shell: the frontend library window (window mode) or a bare process (headless), guest boot thread, life cycle. */
#include "platform.h"
#if NAPPLE_IOS
#import <UIKit/UIKit.h>
#else
#import <Cocoa/Cocoa.h>
#endif
#import <QuartzCore/CAMetalLayer.h>
#include "frontend.h"
#include "gfx.h"
#include "napple.h"
#include "napple_frontend.h"
#include "napple_input.h"
#include "napple_audio.h"
#include "napple_display.h"
#include "napple_gfx.h"
#include "timescale.h"
#include "mods_runtime.h"
#if !NAPPLE_IOS
#include "services/applet_lib_core.h"
#include "services/applet_ui.h"
#include "services/applet_web_core.h"
#endif
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

static napple_frontend *g_fe;
static FrontendBoot g_boot;
static int g_seconds;
static volatile int g_focus = 1;

void *frontend_metal_layer(void) {
#ifdef NAPPLE_LIB
    if (lib_host_layer()) return lib_host_layer();
#endif
    return g_fe ? napple_frontend_metal_layer(g_fe) : NULL;
}
int frontend_has_focus(void) { return g_focus; }
void frontend_frame_presented(void) {
    if (g_fe) napple_frontend_frame_presented(g_fe);
}

void fs_report_uncommitted(void);
void napple_audio_pcm_report(void);
void audio_report(void);
void frontend_quit(int code) {
    thread_dump_all();
    fs_report_uncommitted();
    napple_audio_pcm_report();
    audio_report();
    summary_write(code ? "fail" : "partial", code ? "stopped" : "stopped by the user or the run time limit");
    gfx_shutdown();
    GUEST_EXIT(code);
}

static void *boot_thread(void *unused) {
    (void)unused;
    g_boot();
    runtime_fail("boot returned");
}
static void start_boot_thread(void) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t t;
    if (pthread_create(&t, &a, boot_thread, NULL)) runtime_fail("cannot create the boot thread");
    pthread_attr_destroy(&a);
    pthread_detach(t);
}

static void on_ready(void *u) {
    (void)u;
    /* Warm the audio engine off the main thread (a cold Core Audio unit can take seconds), then apply the saved volume: the engine start resets
     * the master volume, and NAPPLE_AUDIO_VOLUME (tests) still wins. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        audio_init_once();
        if (g_fe && !getenv("NAPPLE_AUDIO_VOLUME")) {
            napple_settings s;
            napple_frontend_get_settings(g_fe, &s);
            napple_audio_set_master_volume(s.master_volume);
        }
    });
    start_boot_thread();
    if (g_seconds > 0) {
        int s = g_seconds;
        [NSTimer scheduledTimerWithTimeInterval:s
                                        repeats:NO
                                          block:^(NSTimer *t) {
                                              (void)t;
                                              LOG("main", "time limit of %d s reached", s);
                                              frontend_quit(0);
                                          }];
    }
}
/* Frontend settings -> graphics back end. Frame rate: 0 = the game's own pacing (real time), 1000 = uncapped (fast forward), anything else fixes the
 * display rate the game is paced by (the guest sees vsync at that rate, so its speed scales with it: documented in the overlay and docs/architecture/runtime-services.md). */
static volatile int g_gfx_ready;
static int g_audio_speed_mode; /* settings.audio_speed_mode */
/* Display pipeline settings (render size, supersampling, filter, frame generation): frontend -> gfx (napple_display.h). Applied live. */
static void push_display_settings(const napple_settings *s) {
    napple_display_settings d;
    napple_display_get(&d);
    d.render_mode = (uint32_t)s->render_mode;
    d.scale = (float)s->internal_scale_percent / 100.f;
    d.fixed_w = (uint32_t)s->internal_width;
    d.fixed_h = (uint32_t)s->internal_height;
    d.supersample = (float)s->supersample_percent / 100.f;
    d.fit = s->aspect == 3 ? NAPPLE_FIT_STRETCH : s->aspect == 5 ? NAPPLE_FIT_FILL : s->aspect == 7 ? NAPPLE_FIT_INTEGER : NAPPLE_FIT_LETTERBOX;
    d.report_window_aspect = s->aspect == 6;
    d.filter = (uint32_t)s->filter;
    d.sharpness = (float)s->sharpness / 100.f;
    d.upscaler = (uint32_t)s->upscaler;
    d.anisotropy = (uint32_t)s->anisotropy;
    d.frame_gen = (uint32_t)s->frame_gen;
    d.frame_gen_mult = (uint32_t)s->frame_gen_mult;
    d.frame_gen_method = (uint32_t)s->frame_gen_method;
    d.show_stats = (uint32_t)s->show_stats;
    {
        const char *e = getenv("NAPPLE_FG");
        if (e) d.frame_gen = atoi(e) != 0;
    } /* debug / headless overrides */
    {
        const char *e = getenv("NAPPLE_FG_MULT");
        if (e) d.frame_gen_mult = (uint32_t)atoi(e);
    }
    {
        const char *e = getenv("NAPPLE_FG_METHOD");
        if (e) d.frame_gen_method = (uint32_t)atoi(e);
    }
    {
        const char *e = getenv("NAPPLE_SUPERSAMPLE");
        if (e) d.supersample = (float)atof(e);
    }
    {
        const char *e = getenv("NAPPLE_RENDER_MODE");
        if (e) d.render_mode = (uint32_t)atoi(e);
    }
    {
        const char *e = getenv("NAPPLE_RENDER_SCALE");
        if (e) d.scale = (float)atof(e);
    }
    {
        const char *e = getenv("NAPPLE_FILTER");
        if (e) d.filter = (uint32_t)atoi(e);
    }
    {
        const char *e = getenv("NAPPLE_UPSCALER");
        if (e) d.upscaler = (uint32_t)atoi(e);
    }
    d.speed = (float)timescale_vsync_factor();
    napple_display_set(&d);
    g_audio_speed_mode = s->audio_speed_mode;
}
/* Game speed (percent): guest clocks, virtual vsync, audio pacing and the presenter's drop logic all follow it (docs/graphics-and-speed-settings.md). */
static int g_speed_mode;
static void speed_observer(double speed, int mode) {
    double clock = timescale_clock_factor();
    int mute = mode == TS_MODE_VSYNC_ONLY && speed != 1.0; /* the audio engine runs on the clock: a vsync-only turbo outruns it */
    napple_audio_set_speed(clock, mute ? NAPPLE_AUDIO_SPEED_MUTE : g_audio_speed_mode);
    napple_display_settings d;
    napple_display_get(&d);
    d.speed = (float)timescale_vsync_factor();
    napple_display_set(&d);
}
static void push_speed(int percent, int mode) {
    const char *e = getenv("NAPPLE_SPEED");
    if (e && atof(e) > 0) percent = (int)(atof(e) * 100 + 0.5);
    e = getenv("NAPPLE_SPEED_MODE");
    if (e) mode = atoi(e);
    g_speed_mode = mode;
    timescale_set((double)percent / 100.0, mode);
}
static void on_speed(void *u, int percent) {
    (void)u;
    if (g_fe) {
        napple_settings s;
        napple_frontend_get_settings(g_fe, &s);
        g_audio_speed_mode = s.audio_speed_mode;
        push_speed(percent, s.speed_mode);
    }
}
static void on_resize(void *u, int w, int h, double scale) {
    (void)u;
    (void)scale;
#if NAPPLE_IOS
    double hz = g_fe ? napple_frontend_display_hz(g_fe) : 60; /* the frontend tracks the refresh rate of the screen the game view is on */
#else
    double hz = 60;
    NSScreen *sc = NSApp.keyWindow.screen ?: NSScreen.mainScreen;
    if (sc) hz = (double)sc.maximumFramesPerSecond;
#endif
    napple_display_set_window((uint32_t)(w > 0 ? w : 0), (uint32_t)(h > 0 ? h : 0), hz);
}
static void on_stats_text(void *u, char *out, size_t cap) {
    (void)u;
    napple_display_stats_text(out, (unsigned)cap);
}
static void push_gfx_settings(const napple_settings *user) {
    napple_settings modded = *user; /* a mod's config.json overrides this run only; the user's settings.json is never rewritten */
    mods_config_apply_settings(&modded);
    const napple_settings *s = &modded;
    GfxSettings g = {0};
    g.docked = !g_cfg.op_mode_handheld;
    if (s->internal_width && s->internal_height) g.internal_scale = (float)s->internal_height / (g.docked ? 1080.f : 720.f);
    else g.internal_scale = (float)s->internal_scale_percent / 100.f;
    /* fps_mode only chooses the host present rate; the guest's cadence stays the game's (see timescale.h). Display matching needs no cap: the layer syncs. */
    g.pacing = s->fps_mode == 3 ? 2 : s->fps_mode == 2 ? 1 : 0;
    g.vsync = getenv("NAPPLE_VSYNC") ? atoi(getenv("NAPPLE_VSYNC")) : s->vsync; /* NAPPLE_VSYNC=0|1: soak-test override */
    g.target_fps = s->fps_mode == 2 ? (float)s->fps_cap : 0.f;
    const char *fe = getenv("NAPPLE_FPS_MODE"); /* headless runs: 0 game, 1 display, 2 custom (NAPPLE_FPS_HZ), 3 uncapped */
    if (fe) {
        int m = atoi(fe);
        const char *hz = getenv("NAPPLE_FPS_HZ");
        g.pacing = m == 3 ? 2 : m == 2 ? 1 : 0;
        g.target_fps = m == 2 ? (float)(hz && atoi(hz) > 0 ? atoi(hz) : 60) : 0.f;
        timescale_set_fps(m, (int)g.target_fps);
    } else timescale_set_fps(s->fps_mode, s->fps_cap);
    gfx_set_settings(&g);
    push_display_settings(s);
    LOG("main", "graphics settings: internal x%.2f, frame rate %s, vsync %s", g.internal_scale,
        g.pacing == 0   ? "game pacing / display"
        : g.pacing == 2 ? "uncapped"
                        : "custom present rate",
        s->vsync ? "on" : "off");
}
static void on_settings(void *u, const napple_settings *s) {
    (void)u;
    g_audio_speed_mode = s->audio_speed_mode;
    if (g_gfx_ready) push_gfx_settings(s);
}
/* main.c boot(): the graphics back end exists; apply what the player saved. */
void frontend_gfx_ready(void) {
    g_gfx_ready = 1;
    timescale_set_observer(speed_observer);
    if (g_fe) {
        napple_settings s;
        napple_frontend_get_settings(g_fe, &s);
        push_gfx_settings(&s);
        push_speed(napple_effective_speed_percent(&s, 0, 0, 0), s.speed_mode);
        return;
    }
    /* headless: no frontend, but the same settings.json drives the back end (tests of frame rate / scale without a window) */
    napple_settings s;
    napple_settings_default(&s); /* no settings.json: defaults (the NAPPLE_* test environment variables still apply) */
    char dir[1200], path[1300];
    snprintf(dir, sizeof dir, "%s/%s", g_cfg.save_root, g_cfg.title_id);
    FILE *f = napple_data_path(dir, g_cfg.title_id, "settings.json", path, sizeof path) ? NULL : fopen(path, "rb");
    if (f) {
        static char buf[65537];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        napple_settings r;
        if (napple_settings_from_json(buf, n, &r) == 0) s = r;
        else LOG("main", "%s is invalid: graphics settings left at their defaults", path);
    }
    push_gfx_settings(&s);
    push_speed(napple_effective_speed_percent(&s, 0, 0, 0), s.speed_mode);
}

/* Shader compile on the loading screen. Later launches: the back end compiles every cached shader at init and the game is held here behind a
 * progress bar (count and time left) until that is done, so play starts without compile stalls. First launch (no cached shaders): the
 * loading screen says the first run is slower. While playing, a corner notice appears when the game has waited on a compile for 300 ms. */
static int g_first_launch;
static void start_compile_notice(void) {
    static dispatch_source_t t;
    t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 0), 100 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(t, ^{
        static int busy, idle, shown;
        GfxCompileStatus st;
        gfx_compile_status(&st);
        /* The game has produced no frame yet: say so honestly. The wait is the game's own start-up, not shader work, unless a compile is blocking it. */
        static uint64_t t_start;
        static int no_draw_shown;
        if (!t_start) t_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        double up = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t_start) / 1e9;
        if (napple_gfx_frame_count() == 0 && up >= 8 && !st.blocking && st.pipelines == 0) {
            char msg[160];
            snprintf(msg, sizeof msg, "Still starting: the game has not drawn its first frame after %.0f s (no shaders needed yet)", up);
            napple_frontend_boot_status(g_fe, msg, -1);
            no_draw_shown = 1;
        } else if (no_draw_shown && napple_gfx_frame_count() > 0) {
            napple_frontend_boot_status(g_fe, "Starting", -1);
            no_draw_shown = 0;
        }
        if (st.blocking) {
            busy++;
            idle = 0;
        } else {
            idle++;
            busy = 0;
        }
        if (busy >= 3) {
            char msg[128];
            if (g_first_launch) snprintf(msg, sizeof msg, "First launch: compiling shaders (%llu ready)", st.pipelines);
            else snprintf(msg, sizeof msg, "Compiling shaders");
            napple_frontend_compile_notice(g_fe, msg);
            shown = 1;
        } else if (shown && idle >= 5) {
            napple_frontend_compile_notice(g_fe, NULL);
            shown = 0;
        }
    });
    dispatch_resume(t);
}
void frontend_wait_shader_prewarm(const char *cache_dir) {
    if (!g_fe) return;
    int cached = 0;
    if (cache_dir) {
        char d[1300];
        snprintf(d, sizeof d, "%s/shaders", cache_dir);
        DIR *dir = opendir(d);
        struct dirent *e;
        if (dir) {
            while (!cached && (e = readdir(dir))) cached = strstr(e->d_name, ".msl") != NULL;
            closedir(dir);
        }
    }
    g_first_launch = !cached;
    GfxCompileStatus st;
    gfx_compile_status(&st);
    if (g_first_launch) {
        napple_frontend_boot_status(
            g_fe, "Starting the game. First launch: shaders are compiled as the game draws them, which can slow the first minutes once the game is running.",
            -1);
    } else if (st.prewarm_total) {
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        while (gfx_compile_status(&st), st.prewarm_done < st.prewarm_total) {
            char msg[160];
            double el = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9;
            int n = snprintf(msg, sizeof msg, "Preparing shaders  %u / %u", st.prewarm_done, st.prewarm_total);
            if (el > 1 && st.prewarm_done) {
                double left = el / st.prewarm_done * (st.prewarm_total - st.prewarm_done);
                snprintf(msg + n, sizeof msg - (size_t)n, left < 1 ? "  ·  almost done" : "  ·  about %.0f s left", left);
            }
            napple_frontend_boot_status(g_fe, msg, (double)st.prewarm_done / st.prewarm_total);
            usleep(50000);
        }
        LOG("boot", "%u cached shaders compiled in %.1f s before start", st.prewarm_total, (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9);
        napple_frontend_boot_status(g_fe, "Starting", -1);
    }
    start_compile_notice();
}

static void on_quit(void *u) {
    (void)u;
    LOG("main", "window closed");
    frontend_quit(0);
}
static void on_focus(void *u, int f) {
    (void)u;
    g_focus = f;
}
static int on_screenshot(void *u, const char *path) {
    (void)u;
    return gfx_screenshot(path);
}

/* Debug aid: `kill -USR1 <pid>` logs every guest thread (state, wait, backtrace) without stopping the game. */
static dispatch_source_t g_usr1, g_usr2;
extern void gfx_capture_request(int frames);
static void install_usr1_dump(void) {
    signal(SIGUSR1, SIG_IGN);
    g_usr1 = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_event_handler(g_usr1, ^{ thread_dump_all(); });
    dispatch_resume(g_usr1);
    /* `kill -USR2 <pid>`: capture the next frames (screenshots + per-draw diagnostics when NAPPLE_CAPTURE_DIR / NAPPLE_DRAWLOG_* are set). */
    signal(SIGUSR2, SIG_IGN);
    g_usr2 = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR2, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_event_handler(g_usr2, ^{
        LOG("main", "capture requested (SIGUSR2)");
        gfx_capture_request(4);
    });
    dispatch_resume(g_usr2);
}

#if !NAPPLE_IOS
/* The library applets' sheets (frontend/src/applet_sheets.m), registered once the window exists. Scripted answers (NAPPLE_CONTROLLER_SUPPORT,
 * NAPPLE_PLAYER_SELECT, NAPPLE_WEB_ANSWER, NAPPLE_ALBUM_PICK) are checked by the applets before these hooks run. */
static void ui_error(const AppletErrorInfo *e, const char *code, const char *desc) {
    (void)e;
    char msg[400];
    snprintf(msg, sizeof msg, "%s%s%s", code && *code ? "Error code " : "", code ? code : "", code && *code ? "\n" : "");
    size_t n = strlen(msg);
    snprintf(msg + n, sizeof msg - n, "%s", desc ? desc : "");
    napple_frontend_sheet_error(g_fe, "The game reported an error", msg);
}
static int ui_controller(AppletControllerAnswer *a) {
    int players = a->players;
    if (!napple_frontend_sheet_controller(g_fe, a->min, a->max, a->connected, a->single_mode, &players)) return 0;
    a->players = players;
    return 1;
}
static int ui_player_select(const uint8_t (*uids)[16], unsigned count) { return napple_frontend_sheet_player_select(g_fe, uids, count); }
static int ui_open_web(const char *url, int offline, char *last_url, size_t cap) {
    (void)offline;
    int r = napple_frontend_sheet_web(g_fe, url, applet_web_html_root(), last_url, cap);
    if (r < 0) return -1;
    return r == 0 ? WEB_EXIT_BACK_BUTTON : WEB_EXIT_WINDOW_CLOSED;
}
static int ui_show_image(const uint8_t *rgba, unsigned w, unsigned h, unsigned index, unsigned count) {
    return napple_frontend_sheet_image(g_fe, rgba, w, h, index, count, applet_ui_album_selecting());
}

/* Test aid: NAPPLE_DEBUG_SHEETS=<seconds> raises every applet sheet (error, controller, player select, software keyboard, web, album) from a thread of
 * its own, the way a guest thread would, over the running title's window. Answers come from NAPPLE_SHEET_AUTO / NAPPLE_SWKBD_ANSWER, screenshots from
 * NAPPLE_SHEET_SHOT; every result is logged as "debug-sheets: <sheet> -> <result>" (tools/e2e/e2e.py, step applets). Window mode only. */
static void *debug_sheets_thread(void *arg) {
    sleep((unsigned)(uintptr_t)arg);
    const AppletErrorInfo *noerr = NULL;
    ui_error(noerr, "2002-0002", "The network service is not available.");
    LOG("main", "debug-sheets: error -> dismissed");
    AppletControllerAnswer ca = {.min = 1, .max = 4, .connected = frontend_connected_pads(), .single_mode = 0, .players = 2};
    int r = ui_controller(&ca);
    LOG("main", "debug-sheets: controller -> %d players=%d connected=%d", r, ca.players, ca.connected);
    uint8_t uids[2][16];
    memset(uids, 0x4e, sizeof uids);
    uids[1][0] = 1;
    LOG("main", "debug-sheets: player select -> %d", ui_player_select((const uint8_t(*)[16])uids, 2));
    char text[64] = "";
    int ok = frontend_text_input("Enter a nickname", "Up to 10 characters", "name", "Link", 10, 1, 0, 0, text, sizeof text);
    LOG("main", "debug-sheets: swkbd -> %d \"%s\"", ok, text);
    char path[1100], url[1200], last[1024] = "";
    snprintf(path, sizeof path, "%s/napple-debug-sheet.html", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    FILE *fp = fopen(path, "w");
    if (fp) {
        fputs("<html><body style='font:28px sans-serif'><h1>Napple web applet</h1><p>An offline page shown over the running title.</p></body></html>", fp);
        fclose(fp);
    }
    snprintf(url, sizeof url, "file://%s", path);
    LOG("main", "debug-sheets: web -> %d", ui_open_web(url, 1, last, sizeof last));
    static uint8_t px[96 * 64 * 4];
    for (unsigned i = 0; i < 96 * 64; i++) {
        px[i * 4] = (uint8_t)(i % 96 * 2);
        px[i * 4 + 1] = (uint8_t)(i / 96 * 3);
        px[i * 4 + 2] = 160;
        px[i * 4 + 3] = 255;
    }
    LOG("main", "debug-sheets: album -> %d", napple_frontend_sheet_image(g_fe, px, 96, 64, 0, 2, 1));
    LOG("main", "debug-sheets: done");
    return NULL;
}
static void register_applet_sheets(void) {
    applet_set_error_hook(ui_error);
    applet_set_controller_ui(ui_controller);
    applet_set_player_select_ui(ui_player_select);
    applet_ui_set_open_web(ui_open_web);
    applet_ui_set_show_image(ui_show_image);
    const char *ds = getenv("NAPPLE_DEBUG_SHEETS");
    if (ds) {
        pthread_t t;
        pthread_create(&t, NULL, debug_sheets_thread, (void *)(uintptr_t)atoi(ds));
        pthread_detach(t);
    }
}
#else
static void register_applet_sheets(void) {}
#endif

int frontend_run(FrontendBoot boot, int headless, int seconds) {
    g_boot = boot;
    g_seconds = seconds;
    install_usr1_dump();
    if (headless) {
        start_boot_thread();
#ifdef NAPPLE_LIB
        return lib_wait_guest_end(seconds);
#endif
        if (seconds > 0) {
            sleep((unsigned)seconds);
            LOG("main", "time limit of %d s reached", seconds);
            frontend_quit(0);
        }
        for (;;) pause();
    }
#ifdef NAPPLE_LIB
    if (lib_host_layer()) { /* in-process applet with a window: the host's layer and input, no window of our own */
        start_boot_thread();
        return lib_wait_guest_end(seconds);
    }
#endif
    @autoreleasepool {
        napple_frontend_config c = {0};
        c.title_id = g_cfg.title_id;
        static char title[128];
        snprintf(title, sizeof title, "%s", g_cfg.title_name[0] ? g_cfg.title_name : "Napple");
        c.title_name = title;
        static char cdir[1200];
        snprintf(cdir, sizeof cdir, "%s/%s", g_cfg.save_root, g_cfg.title_id);
        c.config_dir = cdir; /* --save-root moves settings.json too */
        c.input = hid_input_get();
        c.callbacks.on_resize = on_resize;
        c.callbacks.on_speed = on_speed;
        c.callbacks.stats_text = on_stats_text;
        c.callbacks.on_settings_changed = on_settings;
        c.callbacks.on_ready = on_ready;
        c.callbacks.on_quit = on_quit;
        c.callbacks.on_focus = on_focus;
        c.callbacks.on_screenshot = on_screenshot;
        g_fe = napple_frontend_create(&c);
        if (!g_fe) runtime_fail("frontend: cannot create the window");
        register_applet_sheets();
        napple_frontend_run(g_fe);
    }
    return 0;
}

int frontend_text_input(const char *header, const char *sub, const char *guide, const char *initial, unsigned max_len, unsigned min_len, int password,
                        int numeric, char *out, unsigned cap) {
    if (!g_fe) { /* headless: NAPPLE_SWKBD_ANSWER = text | <cancel> | <initial> (default: accept the initial text) */
        const char *ans = getenv("NAPPLE_SWKBD_ANSWER");
        if (ans && !strcmp(ans, "<cancel>")) {
            out[0] = 0;
            return 0;
        }
        if (ans && strcmp(ans, "<initial>")) {
            snprintf(out, cap, "%s", ans);
            return 1;
        }
        if (initial) snprintf(out, cap, "%s", initial);
        else out[0] = 0;
        return 1;
    }
    napple_text_request r = {.header = header,
                             .sub = sub,
                             .guide = guide,
                             .initial = initial,
                             .max_length = max_len,
                             .min_length = min_len,
                             .password = password,
                             .numeric = numeric};
    return napple_frontend_text_input_blocking(g_fe, &r, out, cap);
}

void frontend_alert(const char *title, const char *message) {
    if (!g_fe) {
        LOG("applet", "alert (headless): %s: %s", title, message);
        return;
    }
    NSString *t = [NSString stringWithUTF8String:title ? title : ""], *m = [NSString stringWithUTF8String:message ? message : ""];
#if NAPPLE_IOS
    void (^show)(void) = ^{
        UIWindow *window = nil;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIWindowScene.class]) continue;
            for (UIWindow *candidate in ((UIWindowScene *)scene).windows) {
                if (candidate.isKeyWindow) {
                    window = candidate;
                    break;
                }
            }
            if (window) break;
        }
        UIViewController *presenter = window.rootViewController;
        while (presenter.presentedViewController) presenter = presenter.presentedViewController;
        if (!presenter) return;
        UIAlertController *a = [UIAlertController alertControllerWithTitle:t message:m preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [presenter presentViewController:a animated:YES completion:nil];
    };
#else
    void (^show)(void) = ^{
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = t;
        a.informativeText = m;
        [a addButtonWithTitle:@"OK"];
        [a runModal];
    };
#endif
    if ([NSThread isMainThread]) show();
    else dispatch_sync(dispatch_get_main_queue(), show);
}

int frontend_connected_pads(void) {
    const char *e = getenv("NAPPLE_CONTROLLER_PADS");
    if (e && atoi(e) > 0) return atoi(e) > 8 ? 8 : atoi(e);
    if (!g_fe) return 1;
    napple_input_snapshot s;
    napple_input_get_snapshot(hid_input_get(), &s);
    return s.controllers > 0 ? s.controllers : 1;
}
