/* Scripted input for automated runs: NAPPLE_INPUT_SCRIPT="5.0:A,12:PLUS:0.3,20:LS_UP:1.5" presses the named Switch button (or left-stick
 * direction) at that many seconds after the guest first configures HID, held for the given seconds (default 0.15). It injects host key
 * events through the same bindings a human uses (input.json defaults), so the whole input path is exercised. Names: A B X Y L R ZL ZR PLUS
 * MINUS LEFT UP RIGHT DOWN LS_LEFT LS_UP LS_RIGHT LS_DOWN RS_LEFT RS_UP RS_RIGHT RS_DOWN. */
#include "napple.h"
#include "napple_input.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    double at, hold;
    int is_stick;
    int stick_right;
    int idx;
} Ev;
static Ev evs[4096];
static int nev;
static napple_input *g_in;

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
static void *script_thread(void *arg) {
    (void)arg;
    struct timespec s;
    clock_gettime(CLOCK_MONOTONIC, &s);
    double t0 = (double)s.tv_sec + s.tv_nsec / 1e9;
    napple_bindings b;
    napple_input_get_bindings(g_in, &b);
    for (int i = 0; i < nev;) {
        int j = i + 1;
        double end = evs[i].at + evs[i].hold;
        while (j < nev && evs[j].at == evs[i].at) {
            if (evs[j].at + evs[j].hold > end) end = evs[j].at + evs[j].hold;
            j++;
        } /* "A+B" = one chord */
        sleep_until(t0, evs[i].at);
        uint16_t keys[64];
        int nk = 0;
        for (int k = i; k < j && nk < 64; k++) {
            uint16_t key = ev_key(&b, &evs[k]);
            if (key == NAPPLE_KEY_NONE) {
                LOG("input", "script: no key bound for event %d", k);
                continue;
            }
            LOG("input", "script: press %s at %.2f s", evs[k].is_stick ? "stick" : napple_button_name(evs[k].idx), evs[k].at);
            keys[nk++] = key;
        }
        napple_input_keys(g_in, keys, nk, 1); /* a chord is one atomic press: the game sees both buttons in the same sample */
        sleep_until(t0, end);
        napple_input_keys(g_in, keys, nk, 0);
        i = j;
    }
    return NULL;
}
void input_script_start(napple_input *in) {
    const char *e = getenv("NAPPLE_INPUT_SCRIPT");
    if (!e || !*e) return;
    char *copy = strdup(e);
    for (char *tok = strtok(copy, ","); tok && nev < 4096; tok = strtok(NULL, ",")) {
        char *name = strchr(tok, ':');
        if (!name) continue;
        *name++ = 0;
        double at = atof(tok), hold = 0.15;
        char *h = strchr(name, ':');
        if (h) {
            *h++ = 0;
            hold = atof(h);
        }
        char *save = NULL;
        for (char *nm = strtok_r(name, "+", &save); nm && nev < 4096; nm = strtok_r(NULL, "+", &save)) { /* "L+R": chord */
            Ev ev = {at, hold, 0, 0, 0};
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
    g_in = in;
    if (!nev) return;
    pthread_t t;
    pthread_create(&t, NULL, script_thread, NULL);
    pthread_detach(t);
    LOG("input", "input script: %d events", nev);
}
