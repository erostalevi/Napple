/* Host unit test (am-depth-applets, 2026-11-01): NAPPLE_INPUT_SCRIPT parsing and frame-based timing of input_script.c.
 *   S1  "5.0:A,12:PLUS:0.3,20:L+R:0.8" stays seconds; holds default to 0.15 s
 *   S2  "f600:A:f10,f700:LS_UP" counts frames; a frame-timed press defaults to a 9-frame hold
 *   S3  NAPPLE_INPUT_SCRIPT_FRAMES: plain numbers are frames; "f" still works, seconds can be mixed in for a hold
 *   S4  the script thread presses a frame-timed key only once the frame counter reaches it and releases it after the frame hold */
#include "../../src/input_script.c"
#include "../check.h"
#include <stdatomic.h>

/* stand-ins for the runtime log and the input library */
int log_verbose;
void log_line(const char *tag, const char *fmt, ...) { (void)tag, (void)fmt; }
static _Atomic int downs, ups;
static _Atomic uint64_t frame_at_down, frame_at_up;
static _Atomic uint64_t fake_frames;
static uint64_t frames(void) { return atomic_load(&fake_frames); }
int napple_input_get_bindings(napple_input *in, napple_bindings *out) {
    (void)in;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < NAPPLE_BTN_COUNT_BASIC; i++) out->key[i][0] = (uint16_t)(0x10 + i), out->key[i][1] = NAPPLE_KEY_NONE;
    for (int i = 0; i < 4; i++) out->stick_l[i] = (uint16_t)(0x40 + i), out->stick_r[i] = (uint16_t)(0x50 + i);
    return 0;
}
void napple_input_keys(napple_input *in, const uint16_t *k, int n, int down) {
    (void)in, (void)k, (void)n;
    if (down) atomic_fetch_add(&downs, 1), atomic_store(&frame_at_down, frames());
    else atomic_fetch_add(&ups, 1), atomic_store(&frame_at_up, frames());
}
const char *napple_button_name(int b) { (void)b; return "btn"; }

static void *runner(void *a) { input_script_run_for_test(a); return NULL; }

int main(void) {
    double at, hold;
    int af, hf;
    /* S1 */
    CHECK(input_script_parse("5.0:A,12:PLUS:0.3,20:L+R:0.8", 0) == 4, "seconds script: 4 events (a chord is two)");
    CHECK(input_script_event(0, &at, &hold, &af, &hf) == 0 && at == 5.0 && hold == 0.15 && !af && !hf, "A at 5 s, default hold 0.15 s");
    CHECK(input_script_event(1, &at, &hold, &af, &hf) == 10 && at == 12 && hold == 0.3, "PLUS at 12 s for 0.3 s");
    CHECK(input_script_event(3, &at, &hold, &af, &hf) == 7 && at == 20 && hold == 0.8, "chord L+R at 20 s");
    /* S2 */
    CHECK(input_script_parse("f600:A:f10,f700:LS_UP", 0) == 2, "frame script: 2 events");
    CHECK(input_script_event(0, &at, &hold, &af, &hf) == 0 && at == 600 && af && hold == 10 && hf, "A at frame 600 for 10 frames");
    CHECK(input_script_event(1, &at, &hold, &af, &hf) == -1 && at == 700 && af && hold == 9 && hf, "stick at frame 700, default 9-frame hold");
    /* S3 */
    CHECK(input_script_parse("300:B,f400:X:0.5", 1) == 2, "frames by default");
    CHECK(input_script_event(0, &at, &hold, &af, &hf) == 1 && at == 300 && af && hf && hold == 9, "plain 300 = frame 300");
    CHECK(input_script_event(1, &at, &hold, &af, &hf) == 2 && at == 400 && af && hf && hold == 0.5, "with frames as the default a hold of 0.5 is frames too");
    CHECK(input_script_parse("f100:A:0.05", 0) == 1 && input_script_event(0, &at, &hold, &af, &hf) == 0 && af && !hf && hold == 0.05, "frame press, seconds hold");
    /* S4 */
    {
        input_script_set_frame_source(frames);
        CHECK(input_script_parse("f50:A:f5", 0) == 1, "parse for the run");
        pthread_t t;
        pthread_create(&t, NULL, runner, NULL);
        for (uint64_t f = 0; f < 49; f++) { atomic_store(&fake_frames, f); usleep(1000); }
        CHECK(downs == 0, "no press before frame 50");
        for (uint64_t f = 49; f < 70 && !atomic_load(&ups); f++) { atomic_store(&fake_frames, f); usleep(5000); }
        pthread_join(t, NULL);
        CHECK(downs == 1 && ups == 1, "pressed and released once");
        CHECK(frame_at_down >= 50 && frame_at_down <= 51, "pressed at frame 50 (%llu)", (unsigned long long)frame_at_down);
        CHECK(frame_at_up >= frame_at_down + 5 && frame_at_up <= frame_at_down + 6, "released 5 frames later (%llu)", (unsigned long long)frame_at_up);
    }
    printf("{\"test\":\"input_script\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
