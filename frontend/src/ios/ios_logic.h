/* Pure logic of the iOS frontend (no UIKit): on-screen controller geometry and hardware keyboard mapping. Tested on any host
 * (frontend/tests/test_ios_logic.c); frontend_ios.m only draws and forwards. */
#ifndef NAPPLE_IOS_LOGIC_H
#define NAPPLE_IOS_LOGIC_H
#include <stdint.h>

typedef enum { PAD_BUTTON, PAD_STICK } pad_kind;
typedef struct {
    pad_kind kind;
    int button;            /* PAD_BUTTON: NAPPLE_BTN_* index (input/include/napple_input.h) */
    int stick;             /* PAD_STICK: 0 left, 1 right */
    float cx, cy, r;       /* centre and radius in points, origin top-left */
    char label[8];
    int pill;              /* shoulder / system buttons are drawn as pills (wide), hit-tested as circles of radius r */
} pad_ctl;

#define PAD_MAX_CTLS 24
/* Lays the controller out for a w x h point view with safe-area insets. Returns the number of controls (<= PAD_MAX_CTLS):
 * left stick + D-pad (left side), ABXY + right stick (right side), L/ZL/R/ZR on top, minus/plus at the bottom centre. */
int pad_layout(pad_ctl *out, int max, float w, float h, float inset_l, float inset_r, float inset_t, float inset_b);
/* Index of the control under (x, y), or -1. Controls are hit slightly beyond their drawn radius so thumbs do not miss. */
int pad_hit(const pad_ctl *c, int n, float x, float y);
/* Stick deflection of a touch at (x, y) for stick control `c`: unit disc, y up, with a small dead centre. */
void pad_stick_vec(const pad_ctl *c, float x, float y, float *ox, float *oy);

/* USB HID keyboard usage (UIKey / GCKeyCode) -> macOS virtual key code used by napple_input_key. -1 when unknown. */
int ios_hid_to_mac_keycode(int usage);
#endif
