/* timescale.h - guest time scale ("game speed"), independent of the presented frame rate.
 *
 * Three different rates exist and must not be confused:
 *   display rate  = how often the Mac shows a new image (ProMotion 120 Hz, frame generation multiplies it); owned by gfx,
 *   game rate     = how often the game produces a frame (vsync-stepped games: 60 / swap interval per guest second),
 *   game clock    = how fast guest time (system tick, steady / system clock, timeouts, vsync) advances against the wall clock.
 * The time scale s multiplies the game clock: the guest sees one clock (`runtime_guest_ticks`) that advances s guest seconds per real
 * second, every guest-side timeout (svc SleepThread, WaitSynchronization, arbiters, condition variables) is converted from guest to
 * real time, and the virtual vsync fires every period / s of real time, so tick-driven and vsync-stepped games both run s times faster.
 * Modes choose which of the two sources is scaled (see TS_MODE_*). The mapping is continuous (no jump of the guest clock when s changes).
 * Pure maths with an explicit `now` is exposed for unit tests (runtime/tests/test_timescale.c). */
#pragma once
#include <stdint.h>

enum {
    TS_MODE_CLOCK_AND_VSYNC = 0, /* default: all clocks and the vsync run at s (smooth speed-up of tick-driven AND vsync-stepped games) */
    TS_MODE_VSYNC_ONLY = 1, /* frame-skip turbo: clocks stay real, only the virtual vsync runs at s (vsync-stepped games speed up, delta-time games do not) */
    TS_MODE_CLOCK_ONLY = 2, /* clocks at s, vsync stays at the game's rate (tick-driven games speed up without presenting more frames) */
};
#define TS_MIN_SPEED 0.1
#define TS_MAX_SPEED 10.0

void timescale_set(double speed, int mode); /* thread-safe; speed is clamped to 0.1..10 */
double timescale_speed(void);               /* the requested speed (1.0 = normal) */
int timescale_mode(void);
double timescale_clock_factor(void); /* factor applied to guest clocks (1 when the mode leaves the clocks alone) */
double timescale_vsync_factor(void); /* factor applied to the virtual vsync rate */

uint64_t timescale_guest_ns(void);                            /* guest monotonic clock in ns (continuous across changes of s) */
uint64_t timescale_guest_ns_at(uint64_t real_ns);             /* same for an explicit real time (tests) */
int64_t timescale_real_wait_ns(int64_t guest_ns);             /* real time needed for a guest duration at the current clock factor (>= 1 for guest_ns > 0) */
uint64_t timescale_vsync_period_ns(uint64_t guest_period_ns); /* real period of a guest-rate event */
int64_t timescale_wall_skew_ns(void);                         /* guest clock minus real clock: added to the wall clock the time services return */
/* Called when the settings change (runtime pushes them to gfx and audio from here). */
void timescale_set_observer(void (*fn)(double speed, int mode));

void timescale_count_present(void); /* measurement counters for NAPPLE_TIME_LOG */
void timescale_count_vsync(void);

/* ---- host present rate (fps_mode) -------------------------------------------------------------------------------------------------------------
 * The present rate is a HOST property: it never feeds the guest clocks or the virtual vsync, so no mode can change game speed. The guest's cadence is
 * TS_GAME_VSYNC_NS (60 Hz; a title that asks for swap interval 2 simply waits two vsyncs) scaled only by the Speed control above. */
enum { FPS_MODE_GAME = 0, FPS_MODE_DISPLAY = 1, FPS_MODE_CUSTOM = 2, FPS_MODE_UNCAPPED = 3 };
#define TS_GAME_VSYNC_NS 16666667ull
void timescale_set_fps(int fps_mode, int custom_hz);
int timescale_fps_mode(void);
/* Minimum host time between two presented images in ns: 0 = no limit (uncapped, or game pacing where the display's own refresh limits it).
 * display_hz is the current refresh rate of the window's display (0 = unknown, 60). Pure given the settings: exposed for tests. */
uint64_t timescale_present_period_ns(int fps_mode, int custom_hz, double display_hz);
uint64_t timescale_present_period_now_ns(double display_hz);
/* One step of an absolute-deadline periodic event: returns the real ns to sleep (0 = already due) and advances *next; a stall longer than 8 periods
 * resynchronises instead of firing a burst. Pure; vi.c's vsync thread and the tests share it. */
uint64_t timescale_deadline_step(uint64_t *next, uint64_t period, uint64_t now);

/* NAPPLE_FRAME_TRACE=<file.csv>: one line per guest vsync / presented frame: kind,real_ns,game_clock_ns (kind V = guest vsync, P = present). */
void timescale_trace(char kind);
