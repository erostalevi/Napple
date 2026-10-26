/* Linux harness: no GameController. Pads can be injected by a test through ng_stub_set(). */
#include "internal.h"
#include <string.h>
static ng_pad g_pads[NG_MAX_PADS];
static int g_n;
void ng_stub_set(const ng_pad *p, int n) {
    g_n = n > NG_MAX_PADS ? NG_MAX_PADS : n;
    memcpy(g_pads, p, (size_t)g_n * sizeof *p);
}
int ng_start(void) { return 0; }
void ng_stop(void) {}
int ng_snapshot(ng_pad *out, int max) {
    int n = g_n < max ? g_n : max;
    memcpy(out, g_pads, (size_t)n * sizeof *out);
    return n;
}
void ng_rumble(int pad, int side, const napple_vibration *v) { (void)pad; (void)side; (void)v; }
void ng_rumble_quiet(void) {}
void ng_set_motion(int enabled) { (void)enabled; }
