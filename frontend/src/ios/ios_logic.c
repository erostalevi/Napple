#include "ios_logic.h"
#include "napple_input.h"
#include <math.h>
#include <string.h>

static void add_btn(pad_ctl *o, int *n, int max, int button, const char *label, float cx, float cy, float r, int pill) {
    if (*n >= max) return;
    pad_ctl *c = &o[(*n)++];
    memset(c, 0, sizeof *c);
    c->kind = PAD_BUTTON; c->button = button; c->cx = cx; c->cy = cy; c->r = r; c->pill = pill;
    strncpy(c->label, label, sizeof c->label - 1);
}
static void add_stick(pad_ctl *o, int *n, int max, int which, float cx, float cy, float r) {
    if (*n >= max) return;
    pad_ctl *c = &o[(*n)++];
    memset(c, 0, sizeof *c);
    c->kind = PAD_STICK; c->stick = which; c->cx = cx; c->cy = cy; c->r = r;
}

int pad_layout(pad_ctl *o, int max, float w, float h, float il, float ir, float it, float ib) {
    int n = 0;
    /* One unit u: the left (or right) column stacks shoulders 1.9u, D-pad / face cluster 4u and stick 3.8u, about 10.4u in all. */
    float height = h - it - ib, extra;
    float u = fminf(fminf(height / 10.6f, 46.f), (w - il - ir) / 18.f);
    if (u < 14.f) u = 14.f;
    extra = height - 10.6f * u; if (extra < 0) extra = 0;           /* tall screens (iPad): the lower controls sit at the bottom edge, shoulders stay on top */
    float lx = il + 2.8f * u, rx = w - ir - 2.8f * u;               /* centres of the two thumb columns */
    float sy = it + 0.95f * u;                                      /* shoulder row */
    float fy = sy + 3.35f * u + extra, ky = fy + 4.1f * u;          /* D-pad / face cluster, stick */
    float d = 1.15f * u, br = 0.85f * u, f = 1.25f * u, fr = 0.95f * u;
    add_stick(o, &n, max, 0, lx, ky, 1.9f * u);
    add_btn(o, &n, max, NAPPLE_BTN_UP, "^", lx, fy - d, br, 0);
    add_btn(o, &n, max, NAPPLE_BTN_DOWN, "v", lx, fy + d, br, 0);
    add_btn(o, &n, max, NAPPLE_BTN_LEFT, "<", lx - d, fy, br, 0);
    add_btn(o, &n, max, NAPPLE_BTN_RIGHT, ">", lx + d, fy, br, 0);
    /* Switch face layout: X top, B bottom, Y left, A right */
    add_btn(o, &n, max, NAPPLE_BTN_X, "X", rx, fy - f, fr, 0);
    add_btn(o, &n, max, NAPPLE_BTN_B, "B", rx, fy + f, fr, 0);
    add_btn(o, &n, max, NAPPLE_BTN_Y, "Y", rx - f, fy, fr, 0);
    add_btn(o, &n, max, NAPPLE_BTN_A, "A", rx + f, fy, fr, 0);
    add_stick(o, &n, max, 1, rx, ky, 1.9f * u);
    /* shoulders along the top: ZL outermost, L inside it; mirrored on the right */
    add_btn(o, &n, max, NAPPLE_BTN_ZL, "ZL", il + 1.2f * u, sy, 0.8f * u, 1);
    add_btn(o, &n, max, NAPPLE_BTN_L, "L", il + 3.4f * u, sy, 0.8f * u, 1);
    add_btn(o, &n, max, NAPPLE_BTN_ZR, "ZR", w - ir - 1.2f * u, sy, 0.8f * u, 1);
    add_btn(o, &n, max, NAPPLE_BTN_R, "R", w - ir - 3.4f * u, sy, 0.8f * u, 1);
    /* system buttons at the bottom centre */
    add_btn(o, &n, max, NAPPLE_BTN_MINUS, "-", w / 2 - 1.3f * u, h - ib - 0.9f * u, 0.55f * u, 1);
    add_btn(o, &n, max, NAPPLE_BTN_PLUS, "+", w / 2 + 1.3f * u, h - ib - 0.9f * u, 0.55f * u, 1);
    return n;
}

int pad_hit(const pad_ctl *c, int n, float x, float y) {
    int best = -1; float bd = 1e30f;
    for (int i = 0; i < n; i++) {
        float dx = x - c[i].cx, dy = y - c[i].cy, d = sqrtf(dx * dx + dy * dy);
        float reach = c[i].r * (c[i].kind == PAD_STICK ? 1.25f : 1.2f);
        if (d <= reach && d < bd) { bd = d; best = i; }
    }
    return best;
}

void pad_stick_vec(const pad_ctl *c, float x, float y, float *ox, float *oy) {
    float dx = (x - c->cx) / c->r, dy = -(y - c->cy) / c->r, m = sqrtf(dx * dx + dy * dy);
    if (m < 0.08f) { *ox = *oy = 0; return; }
    if (m > 1.f) { dx /= m; dy /= m; }
    *ox = dx; *oy = dy;
}

int ios_hid_to_mac_keycode(int u) {
    static const signed char letters[26] = {0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6};   /* a..z */
    static const signed char digits[10] = {18, 19, 20, 21, 23, 22, 26, 28, 25, 29};                                                            /* 1..9, 0 */
    static const signed char fkeys[12] = {122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111};
    if (u >= 4 && u <= 29) return letters[u - 4];
    if (u >= 30 && u <= 39) return digits[u - 30];
    if (u >= 58 && u <= 69) return fkeys[u - 58];
    switch (u) {
    case 40: return 36; case 41: return 53; case 42: return 51; case 43: return 48; case 44: return 49;
    case 45: return 27; case 46: return 24; case 47: return 33; case 48: return 30; case 49: return 42;
    case 51: return 41; case 52: return 39; case 53: return 50; case 54: return 43; case 55: return 47; case 56: return 44; case 57: return 57;
    case 79: return 124; case 80: return 123; case 81: return 125; case 82: return 126;
    case 224: return 59; case 225: return 56; case 226: return 58; case 227: return 55; case 228: return 62; case 229: return 60; case 230: return 61; case 231: return 54;
    default: return -1;
    }
}
