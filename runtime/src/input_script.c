/* Scripted input for automated runs: NAPPLE_INPUT_SCRIPT="5.0:A,12:PLUS:0.3,20:LS_UP:1.5" presses the named Switch button (or left-stick
 * direction) at that many seconds after the guest first configures HID, held for the given seconds (default 0.15). It injects host key
 * events through the same bindings a human uses (input.json defaults), so the whole input path is exercised. Names: A B X Y L R ZL ZR PLUS
 * MINUS LEFT UP RIGHT DOWN LS_LEFT LS_UP LS_RIGHT LS_DOWN RS_LEFT RS_UP RS_RIGHT RS_DOWN.
 * Frame-based timing (am-depth-applets, 2026-11-01): a time or hold written "f<n>" counts presented guest frames instead of seconds
 * ("f600:A:f10" = press A once the guest has presented 600 frames, hold for 10 frames), so a script does not drift with load or a slow first start;
 * NAPPLE_INPUT_SCRIPT_FRAMES=1 makes every plain number a frame count. Frames come from the graphics driver (napple_gfx_frame_count). */
#include "napple.h"
#include "napple_input.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    double at, hold;
    int at_frames, hold_frames; /* at / hold count presented frames, not seconds */
    int is_stick;
    int stick_right;
    int idx;
} Ev;
static Ev evs[4096];
static int nev;
static napple_input *g_in;
/* Presented frames: the graphics driver's counter (napple_gfx_frame_count, looked up at run time: stub builds and tests have none), or a source a test sets. */
static uint64_t (*g_frames)(void);
void input_script_set_frame_source(uint64_t (*fn)(void)) { g_frames = fn; }
static uint64_t frames_now(void) {
    if (!g_frames) g_frames = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "napple_gfx_frame_count");
    return g_frames ? g_frames() : 0;
}
static void wait_frames(uint64_t n) {
    while (frames_now() < n) usleep(2000);
}

static int button_index(const char *n) {
    static const char *const names[] = {"A", "B", "X", "Y", "STICK_L", "STICK_R", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    for (int i = 0; i < 16; i++)
        if (!strcmp(n, names[i])) return i;
    return -1;
}
static void sleep_until(double t0, double t) {
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double d = t - ((double)now.tv_sec + now.tv_nsec / 1e9 - t0);
        if (d <= 0) return;
        usleep((useconds_t)(d > 0.05 ? 50000 : d * 1e6));
    }
}
static uint16_t ev_key(const napple_bindings *b, const Ev *e) { return e->is_stick ? (e->stick_right ? b->stick_r : b->stick_l)[e->idx] : b->key[e->idx][0]; }
static double now_s(double t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + now.tv_nsec / 1e9 - t0;
}
static void *script_thread(void *arg) {
    (void)arg;
    struct timespec s;
    clock_gettime(CLOCK_MONOTONIC, &s);
    double t0 = (double)s.tv_sec + s.tv_nsec / 1e9;
    napple_bindings b;
    napple_input_get_bindings(g_in, &b);
    for (int i = 0; i < nev;) {
        int j = i + 1;
        while (j < nev && evs[j].at == evs[i].at && evs[j].at_frames == evs[i].at_frames) j++; /* "A+B" = one chord */
        if (evs[i].at_frames) wait_frames((uint64_t)evs[i].at);
        else sleep_until(t0, evs[i].at);
        /* the release: the longest hold of the chord, from the moment it was pressed (frames or seconds) */
        uint64_t f_press = frames_now();
        double t_press = now_s(t0), end_s = t_press;
        uint64_t end_f = f_press;
        for (int k = i; k < j; k++) {
            if (evs[k].hold_frames) end_f = f_press + (uint64_t)evs[k].hold > end_f ? f_press + (uint64_t)evs[k].hold : end_f;
            else end_s = t_press + evs[k].hold > end_s ? t_press + evs[k].hold : end_s;
        }
        uint16_t keys[64];
        int nk = 0;
        for (int k = i; k < j && nk < 64; k++) {
            uint16_t key = ev_key(&b, &evs[k]);
            if (key == NAPPLE_KEY_NONE) {
                LOG("input", "script: no key bound for event %d", k);
                continue;
            }
            if (evs[k].at_frames)
                LOG("input", "script: press %s at frame %llu (%.2f s)", evs[k].is_stick ? "stick" : napple_button_name(evs[k].idx), (unsigned long long)frames_now(), now_s(t0));
            else LOG("input", "script: press %s at %.2f s", evs[k].is_stick ? "stick" : napple_button_name(evs[k].idx), evs[k].at);
            keys[nk++] = key;
        }
        napple_input_keys(g_in, keys, nk, 1); /* a chord is one atomic press: the game sees both buttons in the same sample */
        sleep_until(t0, end_s);
        wait_frames(end_f);
        napple_input_keys(g_in, keys, nk, 0);
        i = j;
    }
    return NULL;
}
/* "f<n>" = n frames, a plain number = seconds (or frames when frames_default). */
static double parse_when(const char *t, int frames_default, int *is_frames) {
    *is_frames = frames_default;
    if (*t == 'f' || *t == 'F') {
        *is_frames = 1;
        t++;
    }
    return atof(t);
}
int input_script_parse(const char *e, int frames_default) {
    nev = 0;
    if (!e || !*e) return 0;
    char *copy = strdup(e);
    for (char *tok = strtok(copy, ","); tok && nev < 4096; tok = strtok(NULL, ",")) {
        char *name = strchr(tok, ':');
        if (!name) continue;
        *name++ = 0;
        int at_frames, hold_frames = 0;
        double at = parse_when(tok, frames_default, &at_frames), hold = at_frames ? 9 : 0.15; /* default hold: 0.15 s, or 9 frames for a frame-timed press */
        hold_frames = at_frames;
        char *h = strchr(name, ':');
        if (h) {
            *h++ = 0;
            hold = parse_when(h, frames_default, &hold_frames);
        }
        char *save = NULL;
        for (char *nm = strtok_r(name, "+", &save); nm && nev < 4096; nm = strtok_r(NULL, "+", &save)) { /* "L+R": chord */
            Ev ev = {at, hold, at_frames, hold_frames, 0, 0, 0};
            if (!strncmp(nm, "LS_", 3) || !strncmp(nm, "RS_", 3)) {
                static const char *const dirs[] = {"LEFT", "UP", "RIGHT", "DOWN"};
                int k = -1;
                for (int i = 0; i < 4; i++)
                    if (!strcmp(nm + 3, dirs[i])) k = i;
                if (k < 0) continue;
                ev.is_stick = 1;
                ev.stick_right = nm[0] == 'R';
                ev.idx = k;
            } else {
                ev.idx = button_index(nm);
                if (ev.idx < 0) {
                    LOG("input", "script: unknown button %s", nm);
                    continue;
                }
            }
            evs[nev++] = ev;
        }
    }
    free(copy);
    return nev;
}
/* For tests: event i as (at, hold, at_frames, hold_frames, button index or -1 for a stick). */
int input_script_event(int i, double *at, double *hold, int *at_frames, int *hold_frames) {
    if (i < 0 || i >= nev) return -2;
    *at = evs[i].at, *hold = evs[i].hold, *at_frames = evs[i].at_frames, *hold_frames = evs[i].hold_frames;
    return evs[i].is_stick ? -1 : evs[i].idx;
}
void input_script_run_for_test(napple_input *in) { g_in = in; script_thread(NULL); }
void input_script_start(napple_input *in) {
    const char *f = getenv("NAPPLE_INPUT_SCRIPT_FRAMES");
    if (!input_script_parse(getenv("NAPPLE_INPUT_SCRIPT"), f && *f && *f != '0')) return;
    g_in = in;
    pthread_t t;
    pthread_create(&t, NULL, script_thread, NULL);
    pthread_detach(t);
    LOG("input", "input script: %d events", nev);
}
