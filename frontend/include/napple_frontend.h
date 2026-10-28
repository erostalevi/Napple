/* napple_frontend.h - the macOS window, event loop, settings and in-game overlay of a Napple game.

   Owns: the NSWindow + CAMetalLayer that gfx renders into, the AppKit event loop, fullscreen, the persisted
   settings (resolution, frame-rate cap, vsync, ...), the in-game overlay menu (Video / Controls / Game with DLC
   toggles and an update version picker), the Mac-keyboard text bar for nn::swkbd, focus handling, the
   screenshot hotkey and the app menu.

   Threading: napple_frontend_run() must be called on the process main thread and does not return until the
   window closes or napple_frontend_request_quit() is called.  The guest and gfx run on other threads
   (start them from on_ready).  Everything else may be called from any thread.

   See docs/architecture/audio-input-frontend.md. */
#ifndef NAPPLE_FRONTEND_H
#define NAPPLE_FRONTEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAPPLE_FRONTEND_API_VERSION 1

struct napple_input; /* input/include/napple_input.h (optional) */

/* ---- settings (persisted at ~/Library/Application Support/Napple/<titleid>/settings.json) ---- */
typedef struct napple_settings {
    /* Video */
    int internal_width, internal_height; /* guest render target size the game is told about; 0,0 = game native */
    int internal_scale_percent;          /* 10..800 (free value); used when internal_width/height are 0 (100 = native) */
    int output_width, output_height;     /* window content size in points (windowed); 0 = default 1280x720 */
    int fps_mode; /* host present rate; never changes game speed: 0 = game pacing (the title's own cadence), 1 = match display refresh, 2 = custom (fps_cap), 3 = uncapped */
    int fps_cap;  /* the custom present rate for fps_mode 2 (15..999). Files from before fps_mode stored a speed-scaling rate here; they migrate to mode + Speed */
    int vsync; /* 1 = present on display refresh */
    int fullscreen;
    int aspect; /* window / image shape: 0 = locked 16:9 (native), 1 = locked 16:9, 2 = locked 4:3, 3 = free window, stretch the image,
                                              4 = free window, keep the game's aspect (bars), 5 = free window, fill (crop), 6 = free window, tell the game the window's aspect (engines
                                              that support it extend their field of view; others fall back to 4), 7 = free window, whole-number multiple of the game's size with bars */
    /* Display (render size, supersampling, output filter, frame generation) */
    int render_mode;         /* 0 = native size, 1 = scale factor (internal_scale_percent), 2 = follow window (render at the window's pixel size),
                                              3 = fixed size (internal_width x internal_height) */
    int supersample_percent; /* 100..400: render this much larger than the chosen size and filter down to the window (1.5x, 2x, 3x, 4x) */
    int filter;              /* output filter: 0 bilinear, 1 nearest, 2 box (area average), 3 bicubic (Catmull-Rom), 4 Lanczos, 5 sharpened bilinear */
    int sharpness;           /* 0..100: strength of the sharpening (filter 5, and the MetalFX upscaler's post sharpen) */
    int upscaler;            /* 0 = none (the filter alone), 1 = MetalFX spatial (falls back to 2 where the OS has none), 2 = built-in edge adaptive upscaler, 3 = CAS (resample + contrast adaptive sharpening); all render below the window size and upscale */
    int anisotropy;          /* 0 = as the game asks, 2 / 4 / 8 / 16 = at least this many samples */
    int frame_gen;           /* 1 = generate in-between frames */
    int frame_gen_mult;      /* 0 = automatic (as many as the display can show), 2..8 = output rate = multiplier x the game's rate */
    int frame_gen_method; /* 0 = best available, 1 = optical flow (VideoToolbox), 2 = optical flow low latency (VideoToolbox), 3 = motion-compensated blend (Metal), 4 = plain blend */
    int show_stats; /* 1 = frame rate / GPU time / latency overlay in the corner of the game view */
    int stats_corner;  /* 0 top left, 1 top right, 2 bottom left (default), 3 bottom right */
    int stats_size;    /* 0 small, 1 medium, 2 large */
    int stats_opacity; /* 10..100: opacity of the statistics background (percent) */
    int metal_hud;     /* 1 = Apple's native Metal performance HUD (CAMetalLayer.developerHUDProperties: frame rate, GPU time, memory, shader compiles) */
    int panel_passthrough; /* 1 (default) = the game keeps receiving keyboard / mouse input while the floating panel is open (controller-opened panels always take the pad) */
    /* Game speed (independent of the frame rate) */
    int speed_percent;    /* 10..1000: how fast game time passes (100 = normal) */
    int turbo_percent;    /* speed while the turbo key is held / toggled (default 300) */
    int speed_mode;       /* 0 = game clock and vsync (everything runs faster), 1 = vsync only (frame skip turbo, clocks stay real), 2 = game clock only */
    int audio_speed_mode; /* 0 = automatic (pitch shift between 0.5x and 2x, silent outside), 1 = always pitch shift, 2 = silent while the speed is not 1x */
    /* Audio */
    float master_volume;    /* 0..1 */
    int mute_in_background; /* 1 = pause audio when the window loses focus */
    /* Input */
    int show_touch_cursor; /* mouse cursor visible over the game */
    /* Game: applied at the next guest restart */
    char update_version[32];   /* "" = latest installed; otherwise a manifest version string */
    char disabled_dlc[32][20]; /* title ids (16 hex) of DLC switched off */
    int disabled_dlc_count;
} napple_settings;

void napple_settings_default(napple_settings *s);
/* Pure JSON round trip (unit tested). from_json clamps values; unknown keys are ignored; returns 0 or <0. */
size_t napple_settings_to_json(const napple_settings *s, char *out, size_t cap);
int napple_settings_from_json(const char *json, size_t len, napple_settings *out);
/* Absolute path of ~/Library/Application Support/Napple/<titleid>/<file> into out. config_dir overrides. */
int napple_data_path(const char *config_dir, const char *title_id, const char *file, char *out, size_t cap);

/* Hot keys handled by the frontend (not remappable, chosen outside the default keyboard bindings): F1 overlay, F12 screenshot, F2 hold = turbo (the key held down),
   F3 toggle turbo, F4 toggle slow motion (0.5x), F5 speed - (one step), F6 speed + (one step), F7 reset to 1x, F8 frame generation on / off, F9 statistics overlay,
   F10 cycle supersampling (1x, 1.5x, 2x, 3x). Controller: none (they would collide with games' own bindings). Steps: 10 25 50 75 100 150 200 300 400 600 800 1000 %. */
#define NAPPLE_SPEED_STEPS                                                                                                                                     \
    { 10, 25, 50, 75, 100, 150, 200, 300, 400, 600, 800, 1000 }
/* Resolved speed, for the runtime: the effective game speed in percent given the hold / toggle state; implemented in settings.c (pure, unit tested). */
/* Present-rate menu shared by the macOS and iOS panels: 0 game pacing, 1 match display, 2..7 custom 30/60/90/120/144/240 Hz, 8 uncapped. Never changes game speed. */
#define NAPPLE_FPS_CHOICES 9
int napple_fps_choice(const napple_settings *s);
void napple_fps_choice_apply(napple_settings *s, int choice);
int napple_effective_speed_percent(const napple_settings *s, int turbo_held, int turbo_toggled, int slow_toggled);

/* ---- game manifest: what the runtime tells the overlay about the installed game ---- */
typedef struct napple_manifest_dlc {
    char title_id[20];
    char name[64];
} napple_manifest_dlc;
typedef struct napple_manifest_update {
    char version[32];
    char name[64];
} napple_manifest_update;
typedef struct napple_manifest {
    char title_id[20];
    char title_name[96];
    char base_version[32];
    const napple_manifest_dlc *dlc;
    size_t dlc_count;
    const napple_manifest_update *updates;
    size_t update_count; /* newest first */
} napple_manifest;

/* ---- text input (nn::swkbd replacement) ---- */
typedef struct napple_text_request {
    const char *header; /* UTF-8 */
    const char *sub;
    const char *guide;
    const char *initial;
    uint32_t max_length; /* characters; 0 = 500 */
    uint32_t min_length;
    int password;
    int numeric;
    uint32_t disabled_keys; /* bit1 space, bit2 '@', bit3 '%', bit4 '/', bit5 '\\', bit6 digits (swkbd order) */
} napple_text_request;
/* Called once: accepted=1 with UTF-8 text (valid only during the call), or accepted=0 on cancel. */
typedef void (*napple_text_done_fn)(void *user, int accepted, const char *utf8);

/* ---- callbacks ---- */
typedef struct napple_frontend_callbacks {
    void *user;
    /* Main thread, once the window and Metal layer exist: start guest / gfx threads here. */
    void (*on_ready)(void *user);
    /* Any change of settings (overlay or API); gfx re-reads video fields. Called on the main thread. */
    void (*on_settings_changed)(void *user, const napple_settings *s);
    /* The user pressed "Restart game" in the overlay (or after changing DLC/update). settings holds the choice. */
    void (*on_restart_guest)(void *user, const napple_settings *s);
    /* Window size / backing scale changed (drawable size in pixels). */
    void (*on_resize)(void *user, int pixel_width, int pixel_height, double scale);
    /* Focus lost/gained. Default behaviour (pause audio per settings, release input) happens before. */
    void (*on_focus)(void *user, int focused);
    /* The window is closing / app quits. Return after saving; the process exits afterwards. */
    void (*on_quit)(void *user);
    /* Screenshot hotkey (F12). If NULL, the window is captured with screencapture. path is the PNG to write. */
    int (*on_screenshot)(void *user, const char *path);
    /* Overlay visible/hidden (gfx may pause or dim). */
    void (*on_overlay)(void *user, int visible);
    /* The effective game speed changed (settings, hot keys: hold / toggle turbo, slow motion, steps); percent 10..1000. Called on the main thread. */
    void (*on_speed)(void *user, int percent);
    /* Statistics overlay (settings.show_stats): fill `out` with one line of text; called about four times a second on the main thread. */
    void (*stats_text)(void *user, char *out, size_t cap);
} napple_frontend_callbacks;

typedef struct napple_frontend_config {
    const char *title_id;       /* 16 hex digits */
    const char *title_name;     /* window title; NULL = "Napple" */
    const char *config_dir;     /* override of the per-game directory (tests); NULL = default */
    const char *screenshot_dir; /* NULL = ~/Pictures/Napple */
    napple_frontend_callbacks callbacks;
    struct napple_input *input; /* optional: key/mouse/focus events are routed here and the remap UI is enabled */
    int headless;               /* 1: no window shown (offscreen; tests) */
} napple_frontend_config;

typedef struct napple_frontend napple_frontend;

napple_frontend *napple_frontend_create(const napple_frontend_config *config);
void napple_frontend_destroy(napple_frontend *fe);
/* Main thread. Runs the event loop until quit. */
int napple_frontend_run(napple_frontend *fe);
void napple_frontend_request_quit(napple_frontend *fe);

/* The CAMetalLayer* (as void*) gfx must render into; valid after create. Retained by the frontend. */
void *napple_frontend_metal_layer(napple_frontend *fe);
/* Refresh rate of the display the window is on (maximumFramesPerSecond, 120 on a ProMotion laptop); 60 when unknown. */
double napple_frontend_display_hz(napple_frontend *fe);
/* Current drawable size in pixels. */
void napple_frontend_drawable_size(napple_frontend *fe, int *w, int *h, double *scale);

void napple_frontend_get_settings(napple_frontend *fe, napple_settings *out);
int napple_frontend_set_settings(napple_frontend *fe, const napple_settings *s); /* clamps, saves, notifies */
/* The overlay's Game tab; copies the manifest. NULL clears. */
void napple_frontend_set_manifest(napple_frontend *fe, const napple_manifest *m);
void napple_frontend_show_overlay(napple_frontend *fe, int visible);

/* Boot / status line shown over the window until napple_frontend_boot_done (loading screen). */
void napple_frontend_boot_status(napple_frontend *fe, const char *stage, double fraction /* <0 = indeterminate */);
void napple_frontend_boot_done(napple_frontend *fe);
/* Small notice with a spinner in the top-right corner of the game view while the game waits on shader compiles; NULL hides it. Any thread. */
void napple_frontend_compile_notice(napple_frontend *fe, const char *text);
/* gfx calls this per presented frame (FPS counter in the title, boot overlay hide). */
void napple_frontend_frame_presented(napple_frontend *fe);

/* swkbd: asynchronous (any thread) and blocking (guest thread: returns when the user finishes). */
void napple_frontend_text_input(napple_frontend *fe, const napple_text_request *req, napple_text_done_fn done, void *user);
int napple_frontend_text_input_blocking(napple_frontend *fe, const napple_text_request *req, char *out, size_t cap);
/* ---- library applet sheets (macOS, frontend/src/applet_sheets.m): each blocks the calling guest thread until the user answers ---- */
/* Controller support: confirm (1) with *players (in/out, within min..max) or cancel (0). */
int napple_frontend_sheet_controller(napple_frontend *fe, unsigned min, unsigned max, int connected, int single_mode, int *players);
/* Player select: the chosen index, or -1 on cancel. */
int napple_frontend_sheet_player_select(napple_frontend *fe, const uint8_t (*uids)[16], unsigned count);
/* Error applet: a critical alert, returns when dismissed. */
void napple_frontend_sheet_error(napple_frontend *fe, const char *title, const char *message);
/* Web / offline web: a WKWebView window for an http(s) or file:// URL (read_root: the folder file:// pages may read, may be NULL). Returns 0 when the user
 * pressed Done, 1 when they closed the window, -1 when the page could not be shown. last_url gets the page the user left on. */
int napple_frontend_sheet_web(napple_frontend *fe, const char *url, const char *read_root, char *last_url, size_t cap);
/* Album image (RGBA8 w x h, copied): 1 = selected, 0 = next image, -1 = close the album. can_select hides the Select button (view mode). */
int napple_frontend_sheet_image(napple_frontend *fe, const uint8_t *rgba, unsigned w, unsigned h, unsigned index, unsigned count, int can_select);
/* Pure validation used by the bar (unit tested): returns 1 if `utf8` satisfies the request. */
int napple_text_validate(const napple_text_request *req, const char *utf8);
/* UTF-16 <-> UTF-8 helpers for the swkbd applet result buffers. Return units/bytes written (no NUL counted). */
size_t napple_utf8_to_utf16(const char *s, uint16_t *out, size_t cap_units);
size_t napple_utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap);

/* ---- frame limiter / vsync helpers for gfx ---- */
typedef struct napple_frame_limiter {
    uint64_t next_ns;
    uint64_t period_ns;
} napple_frame_limiter;
void napple_frame_limiter_init(napple_frame_limiter *l, int fps_cap /* 0 = off */);
/* Sleep until the next frame is due (precise: nanosleep then spin the last 200 us). Returns ns slept. now_ns is
   CLOCK_UPTIME_RAW based; pass 0 to read the clock. Pure maths exposed for tests via napple_frame_limiter_step. */
uint64_t napple_frame_limiter_wait(napple_frame_limiter *l);
uint64_t napple_frame_limiter_step(napple_frame_limiter *l, uint64_t now_ns); /* ns to wait from now_ns; advances state */
/* Blocks until the next display refresh of the game window (CVDisplayLink), or timeout_ms. 1 if a refresh, 0 on timeout. */
int napple_frontend_wait_vsync(napple_frontend *fe, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
