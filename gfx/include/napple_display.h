/*
 * napple_display.h - display pipeline settings of libnapple_gfx: render size, supersampling, output filter, frame generation, statistics.
 *
 * Separate from napple_gfx.h on purpose: this is the player-facing "Video" configuration, applied live (thread-safe) by the runtime from the
 * frontend settings. Vocabulary (see docs/graphics-and-speed-settings.md):
 *   base size    the size the game believes its screen has (1920x1080 docked, 1280x720 handheld; the window's aspect when report_window_aspect is on),
 *   render size  the size the screen-sized render targets are really allocated at (base x scale; sx, sy per axis),
 *   output size  the Metal drawable (the window's pixel size),
 *   display rate how often the output changes; with frame generation it is a multiple of the game's presents.
 */
#ifndef NAPPLE_DISPLAY_H
#define NAPPLE_DISPLAY_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NAPPLE_RENDER_NATIVE = 0, NAPPLE_RENDER_SCALE = 1, NAPPLE_RENDER_WINDOW = 2, NAPPLE_RENDER_FIXED = 3 };
enum { NAPPLE_FIT_LETTERBOX = 0, NAPPLE_FIT_STRETCH = 1, NAPPLE_FIT_FILL = 2 };
enum { NAPPLE_FILTER_BILINEAR = 0, NAPPLE_FILTER_NEAREST = 1, NAPPLE_FILTER_BOX = 2, NAPPLE_FILTER_BICUBIC = 3, NAPPLE_FILTER_LANCZOS = 4, NAPPLE_FILTER_SHARP = 5 };
enum { NAPPLE_FG_AUTO = 0, NAPPLE_FG_FLOW = 1, NAPPLE_FG_FLOW_LOW_LATENCY = 2, NAPPLE_FG_METAL_FLOW = 3, NAPPLE_FG_BLEND = 4 };

typedef struct napple_display_settings {
    uint32_t size;               /* sizeof(napple_display_settings) */
    uint32_t render_mode;        /* NAPPLE_RENDER_* */
    float    scale;              /* NAPPLE_RENDER_SCALE: multiplier on the base size (1 = native) */
    uint32_t fixed_w, fixed_h;   /* NAPPLE_RENDER_FIXED: the size to render at (fit into it keeping the aspect unless stretch) */
    float    supersample;        /* >= 1: render this much larger than the chosen size and filter down (applies to every mode) */
    uint32_t fit;                /* NAPPLE_FIT_*: what to do when the image's aspect differs from the window's */
    uint32_t report_window_aspect; /* tell the game the window's aspect through vi / applet (engines that honour it extend their field of view) */
    uint32_t filter;             /* NAPPLE_FILTER_* */
    float    sharpness;          /* 0..1 */
    uint32_t upscaler;           /* 0 none, 1 MetalFX spatial: render at window / upscale_factor and upscale */
    float    upscale_factor;     /* > 1, default 1.5 */
    uint32_t anisotropy;         /* 0 = as the game asks, else the minimum for every sampler */
    uint32_t frame_gen;          /* generate in-between frames */
    uint32_t frame_gen_mult;     /* 0 = automatic (fill the display's refresh), 2..8 */
    uint32_t frame_gen_method;   /* NAPPLE_FG_* */
    uint32_t show_stats;         /* collect the per-frame statistics (cheap either way) */
    float    speed;              /* game speed as seen by the virtual vsync (runtime timescale): presents are paced by it, frames beyond the display rate are dropped */
    uint32_t rscale;             /* 1 = allow gfx level render target scaling (default 1); 0 = never (per game escape hatch) */
    uint32_t rscale_min_div;     /* a render target counts as screen sized when its width >= base width / this (default 8) */
    uint32_t reserved[6];
} napple_display_settings;

void napple_display_default(napple_display_settings *s);
void napple_display_set(const napple_display_settings *s);      /* thread-safe, applies from the next frame / next render target */
void napple_display_get(napple_display_settings *s);
/* Window (drawable) size in pixels and the refresh rate of its display, from the frontend. */
void napple_display_set_window(uint32_t width_px, uint32_t height_px, double refresh_hz);

/* Size the game should believe its display has: base_w x base_h (operation mode) unless report_window_aspect is on and the window is known:
 * then the height stays and the width follows the window's aspect (even, 16..16384). */
void napple_display_game_size(uint32_t base_w, uint32_t base_h, uint32_t *w, uint32_t *h);

/* The scale factors (sx, sy) screen-sized render targets are allocated with, for a base size and window, under the given settings (pure function,
 * unit tested). Stays inside the device budget: no side above max_dim (<= 16384) and at most max_pixels in total (0 = defaults 16384 / 48 Mpixel). */
void napple_display_compute_scale(const napple_display_settings *s, uint32_t base_w, uint32_t base_h, uint32_t win_w, uint32_t win_h,
                                  uint32_t max_dim, uint64_t max_pixels, float *sx, float *sy);

typedef struct napple_display_stats {
    uint32_t size;
    uint32_t window_w, window_h; double refresh_hz;
    uint32_t base_w, base_h;                    /* the game's screen size as seen by gfx (size of the swapchain textures) */
    uint32_t render_w, render_h;                /* the scaled screen-sized targets */
    float scale_x, scale_y;
    double game_fps;                            /* presents per second by the game (real time) */
    double out_fps;                             /* images shown per second (game frames + generated frames) */
    double gpu_ms;                              /* GPU time of the last game frame (submit to present) */
    double output_ms;                           /* GPU time of the output pass (filter / upscaler) */
    double fg_ms;                               /* GPU time of the last generated frame */
    double latency_ms;                          /* extra latency frame generation adds (estimate) */
    uint64_t game_frames, shown_frames, generated_frames, dropped_frames, scene_cuts;
    uint32_t fg_active; uint32_t fg_method;     /* method really in use (NAPPLE_FG_*) */
    uint32_t rt_scaled, rt_total;               /* render target textures: scaled / all created */
    uint64_t rt_scaled_bytes;                   /* extra host memory of the scaled targets */
} napple_display_stats;
void napple_display_get_stats(napple_display_stats *out);
/* One line for the in-game statistics overlay. */
void napple_display_stats_text(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif
#endif
