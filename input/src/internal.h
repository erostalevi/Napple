/* Internal declarations of the input library (not part of the public API). */
#ifndef NAPPLE_INPUT_INTERNAL_H
#define NAPPLE_INPUT_INTERNAL_H
#include "napple_input.h"
#include <stdarg.h>

void napple_input_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- names / key tables (names.c) ---- */
uint8_t napple_mac_to_hid_usage(uint16_t mac_keycode);   /* USB HID usage id, 0 = none */
uint64_t napple_mac_modifier_bit(uint16_t mac_keycode);  /* HidKeyboardModifier bit for a modifier key, 0 otherwise */
int napple_bindings_validate(const napple_bindings *b);      /* 0 if acceptable */
int napple_button_from_name(const char *name);           /* -1 if unknown */
int napple_pad_element_from_name(const char *name);      /* -1 if unknown */

/* ---- motion fusion (motion.c): gyro + accelerometer -> Switch six-axis state ---- */
typedef struct napple_motion {
    float q[4];            /* orientation quaternion w,x,y,z (sensor -> world) */
    float angle[3];        /* integrated rotation in revolutions */
    int initialised;
} napple_motion;
void napple_motion_reset(napple_motion *m);
/* dt seconds; acc in G; gyro in rad/s. Fills the Switch state (angular velocity in revolutions/s). */
void napple_motion_step(napple_motion *m, float dt, const float acc[3], const float gyro[3], napple_sixaxis_state *out);

/* ---- GameController glue (gamepad.m): physical controllers as plain data ---- */
#define NG_MAX_PADS 8
typedef struct ng_pad {
    int connected;
    char name[64];
    int family;               /* 0 other/Nintendo, 1 Xbox, 2 PlayStation */
    int by_label;             /* 1: face buttons keep GameController's labels (Nintendo pads) */
    uint32_t elements;        /* bit per NAPPLE_PAD_* element held (position based) */
    uint32_t hot;             /* bit0 Home / Guide, bit1 Share / Capture */
    float lx, ly, rx, ry;     /* -1..1, y up */
    float lt, rt;             /* 0..1 */
    int has_motion;
    float acc[3], gyro[3];    /* G, rad/s */
    uint64_t motion_seq;
    int battery;              /* 0..4 levels (HID battery level), 4 = full / unknown */
} ng_pad;
int ng_start(void);
void ng_stop(void);
int ng_snapshot(ng_pad *out, int max);
void ng_rumble(int pad, int side, const napple_vibration *v);
void ng_rumble_quiet(void);
void ng_set_motion(int enabled);

#endif
