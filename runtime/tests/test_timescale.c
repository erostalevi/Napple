/* Unit tests: guest time scale (timescale.c): continuity, factors per mode, timeout conversion, wall-clock skew and the real-time
 * behaviour of the guest clock (1x, 2x, 0.5x measured against the host clock). */
#include "../src/timescale.c"
#include "check.h"
#include <math.h>
#include <time.h>
#include <unistd.h>

static void real_sleep_ms(int ms) { struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&ts, NULL); }

int main(void) {
    /* identity at start */
    uint64_t n = runtime_now_ns(), g = timescale_guest_ns();
    CHECK(g >= n && g - n < 5000000, "guest clock starts equal to the real clock");
    CHECK(timescale_clock_factor() == 1.0 && timescale_vsync_factor() == 1.0, "unity factors");

    /* factors per mode */
    timescale_set(2.0, TS_MODE_CLOCK_AND_VSYNC);
    CHECK(fabs(timescale_clock_factor() - 2.0) < 1e-9 && fabs(timescale_vsync_factor() - 2.0) < 1e-9, "mode 0 scales both");
    timescale_set(2.0, TS_MODE_VSYNC_ONLY);
    CHECK(timescale_clock_factor() == 1.0 && fabs(timescale_vsync_factor() - 2.0) < 1e-9, "mode 1 scales only the vsync");
    timescale_set(0.5, TS_MODE_CLOCK_ONLY);
    CHECK(fabs(timescale_clock_factor() - 0.5) < 1e-9 && timescale_vsync_factor() == 1.0, "mode 2 scales only the clock");
    timescale_set(1000.0, 0); CHECK(timescale_speed() == TS_MAX_SPEED, "speed clamps to 10x");
    timescale_set(0.0, 0); CHECK(timescale_speed() == TS_MIN_SPEED, "speed clamps to 0.1x");
    timescale_set(-3.0, 9); CHECK(timescale_speed() == TS_MIN_SPEED && timescale_mode() == TS_MODE_CLOCK_AND_VSYNC, "invalid mode falls back");

    /* conversions */
    timescale_set(4.0, TS_MODE_CLOCK_AND_VSYNC);
    CHECK(timescale_real_wait_ns(1000000000) == 250000000, "1 s of guest time at 4x is 250 ms of real time");
    CHECK(timescale_real_wait_ns(1) == 1 && timescale_real_wait_ns(0) == 0 && timescale_real_wait_ns(-1) == -1, "tiny / zero / infinite timeouts keep their meaning");
    CHECK(timescale_vsync_period_ns(16666666) == 4166666 || timescale_vsync_period_ns(16666666) == 4166667, "vsync period at 4x");
    timescale_set(0.5, TS_MODE_CLOCK_AND_VSYNC);
    CHECK(timescale_real_wait_ns(1000000) == 2000000, "slow motion doubles the real wait");

    /* continuity: the guest clock never jumps or runs backwards when the speed changes */
    timescale_set(1.0, 0);
    uint64_t prev = timescale_guest_ns(); int backwards = 0; uint64_t maxjump = 0;
    for (int i = 0; i < 2000; i++) {
        timescale_set(i % 2 ? 7.5 : 0.25, 0);
        uint64_t cur = timescale_guest_ns();
        if (cur < prev) backwards++;
        if (cur - prev > maxjump) maxjump = cur - prev;
        prev = cur;
    }
    CHECK(backwards == 0, "guest clock is monotonic across %d speed changes", 2000);
    CHECK(maxjump < 2000000, "no jump at a speed change (max step %llu ns)", (unsigned long long)maxjump);

    /* explicit-time maths: rebase at a known instant */
    timescale_set(1.0, 0);
    uint64_t t0 = runtime_now_ns();
    timescale_set(2.0, 0);
    uint64_t a = timescale_guest_ns_at(t0 + 1000000000ull), b = timescale_guest_ns_at(t0 + 3000000000ull);
    CHECK(b - a > 3990000000ull && b - a < 4010000000ull, "2 real seconds are 4 guest seconds at 2x (got %.3f)", (double)(b - a) / 1e9);

    /* measurement against the host clock: guest seconds per real second at 1x, 2x, 0.5x */
    double ratio[3]; double sp[3] = {1.0, 2.0, 0.5};
    for (int k = 0; k < 3; k++) {
        timescale_set(sp[k], 0);
        uint64_t r0 = runtime_now_ns(), g0 = timescale_guest_ns();
        real_sleep_ms(300);
        uint64_t r1 = runtime_now_ns(), g1 = timescale_guest_ns();
        ratio[k] = (double)(g1 - g0) / (double)(r1 - r0);
        CHECK(fabs(ratio[k] - sp[k]) < 0.02 * sp[k], "guest/real time ratio %.3f for speed %.2f", ratio[k], sp[k]);
    }
    /* guest ticks (19.2 MHz) agree with the guest clock */
    timescale_set(2.0, 0);
    uint64_t t_a = runtime_guest_ticks(); real_sleep_ms(100); uint64_t t_b = runtime_guest_ticks();
    double secs = (double)(t_b - t_a) / 19200000.0;
    CHECK(secs > 0.19 && secs < 0.23, "100 ms real at 2x = %.3f guest seconds of ticks", secs);

    /* wall clock skew is the accumulated difference (0 when the clock is never scaled) */
    timescale_set(3.0, 0); real_sleep_ms(200);
    int64_t skew = timescale_wall_skew_ns();
    CHECK(skew > 300000000 && skew < 700000000, "wall skew after 0.2 s at 3x about +0.4 s (got %.3f s)", (double)skew / 1e9);
    timescale_set(1.0, TS_MODE_VSYNC_ONLY); int64_t s1 = timescale_wall_skew_ns(); real_sleep_ms(100); int64_t s2 = timescale_wall_skew_ns();
    CHECK(s2 - s1 < 1000000 && s2 - s1 > -1000000, "vsync-only mode leaves the clock skew constant");

    printf("test_timescale: %d checks, %d failed\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
