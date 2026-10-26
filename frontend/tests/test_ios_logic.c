/* frontend/src/ios/ios_logic.c on any host: controller geometry and HID -> mac key code mapping. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ios_logic.h"
#include "napple_input.h"
static int g_checked, g_failed;
#define CHECK(c, ...)                                                                                                                                          \
    do {                                                                                                                                                       \
        g_checked++;                                                                                                                                           \
        if (!(c)) {                                                                                                                                            \
            g_failed++;                                                                                                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                                                        \
            printf(__VA_ARGS__);                                                                                                                               \
            printf("\n");                                                                                                                                      \
        }                                                                                                                                                      \
    } while (0)

static int find_btn(const pad_ctl *c, int n, int b) {
    for (int i = 0; i < n; i++)
        if (c[i].kind == PAD_BUTTON && c[i].button == b) return i;
    return -1;
}

static void check_layout(float w, float h, float il, float ir, float it, float ib) {
    pad_ctl c[PAD_MAX_CTLS];
    int n = pad_layout(c, PAD_MAX_CTLS, w, h, il, ir, it, ib);
    CHECK(n == 16, "%gx%g: %d controls", w, h, n);
    int have[NAPPLE_BTN_COUNT_BASIC] = {0};
    for (int i = 0; i < n; i++) {
        CHECK(c[i].cx - c[i].r >= il - 0.5f && c[i].cx + c[i].r <= w - ir + 0.5f && c[i].cy - c[i].r >= 0 && c[i].cy + c[i].r <= h - ib + 0.5f,
              "%gx%g: control %d (%s) is inside the safe area (%g,%g r%g)", w, h, i, c[i].label, c[i].cx, c[i].cy, c[i].r);
        if (c[i].kind == PAD_BUTTON) have[c[i].button]++;
        for (int j = 0; j < i; j++) {
            float d = hypotf(c[i].cx - c[j].cx, c[i].cy - c[j].cy);
            CHECK(d >= (c[i].r + c[j].r) * 0.85f, "%gx%g: controls %s and %s overlap (d %g)", w, h, c[i].label, c[j].label, d);
        }
    }
    for (int b = 0; b < 16; b++)
        if (b != NAPPLE_BTN_STICK_L && b != NAPPLE_BTN_STICK_R) CHECK(have[b] == 1, "%gx%g: button %d placed %d times", w, h, b, have[b]);
    int x = find_btn(c, n, NAPPLE_BTN_X), bb = find_btn(c, n, NAPPLE_BTN_B), y = find_btn(c, n, NAPPLE_BTN_Y), a = find_btn(c, n, NAPPLE_BTN_A);
    CHECK(c[x].cy < c[bb].cy && c[y].cx < c[a].cx, "Switch face layout (X top, B bottom, Y left, A right)");
    for (int i = 0; i < n; i++) CHECK(pad_hit(c, n, c[i].cx, c[i].cy) == i, "centre of %s hits itself", c[i].label);
}

int main(void) {
    check_layout(852, 393, 59, 59, 0, 21); /* iPhone 15 Pro landscape with safe areas */
    check_layout(932, 430, 59, 59, 0, 21); /* Pro Max */
    check_layout(667, 375, 0, 0, 0, 0);    /* iPhone SE */
    check_layout(1194, 834, 0, 0, 24, 20); /* iPad Pro 11 */
    pad_ctl c[PAD_MAX_CTLS];
    int n = pad_layout(c, PAD_MAX_CTLS, 852, 393, 59, 59, 0, 21);
    CHECK(pad_hit(c, n, 426, 100) == -1, "the middle of the screen belongs to the game");
    int s = -1;
    for (int i = 0; i < n; i++)
        if (c[i].kind == PAD_STICK && c[i].stick == 0) s = i;
    float ox, oy;
    pad_stick_vec(&c[s], c[s].cx + c[s].r, c[s].cy, &ox, &oy);
    CHECK(fabsf(ox - 1) < 1e-3f && fabsf(oy) < 1e-3f, "right = +x");
    pad_stick_vec(&c[s], c[s].cx, c[s].cy - c[s].r / 2, &ox, &oy);
    CHECK(fabsf(oy - 0.5f) < 1e-3f, "up is +y (screen y grows down)");
    pad_stick_vec(&c[s], c[s].cx + 5 * c[s].r, c[s].cy, &ox, &oy);
    CHECK(fabsf(ox - 1) < 1e-3f, "clamped to the unit disc");
    pad_stick_vec(&c[s], c[s].cx + 0.01f * c[s].r, c[s].cy, &ox, &oy);
    CHECK(ox == 0 && oy == 0, "dead centre");
    /* keyboard */
    CHECK(ios_hid_to_mac_keycode(4) == 0 && ios_hid_to_mac_keycode(26) == 13 && ios_hid_to_mac_keycode(22) == 1 && ios_hid_to_mac_keycode(7) == 2,
          "a, w, s, d");
    CHECK(ios_hid_to_mac_keycode(15) == 37 && ios_hid_to_mac_keycode(44) == 49 && ios_hid_to_mac_keycode(82) == 126 && ios_hid_to_mac_keycode(41) == 53,
          "l, space, up arrow, escape");
    CHECK(ios_hid_to_mac_keycode(58) == 122 && ios_hid_to_mac_keycode(69) == 111 && ios_hid_to_mac_keycode(225) == 56 && ios_hid_to_mac_keycode(30) == 18 &&
              ios_hid_to_mac_keycode(39) == 29,
          "F1, F12, shift, 1, 0");
    CHECK(ios_hid_to_mac_keycode(0) == -1 && ios_hid_to_mac_keycode(3) == -1 && ios_hid_to_mac_keycode(1000) == -1, "unknown usages");
    printf("{\"test\":\"ios_logic\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
