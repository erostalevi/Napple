/* Gesture recognition from the touch stream (portable, no OS calls). nn::hid::GestureState is what the SDK's gesture manager reads from
 * the HID shared memory; the console derives it from the same touch frames the touch screen section carries. This recogniser turns a
 * sequence of touch frames {time, points} into the state sequence a title sees: Touch, then Press / Pan / Pinch / Rotate while fingers stay
 * down, then Tap / Swipe / Complete when they lift. Thresholds are the author's choice (the console's are not documented) and live in the
 * constants below so a title that feels wrong can be tuned in one place. */
#pragma once
#include "napple_input.h"
#include <stdint.h>

/* GestureType */
enum {
    NAPPLE_GESTURE_IDLE = 0,
    NAPPLE_GESTURE_COMPLETE = 1,
    NAPPLE_GESTURE_CANCEL = 2,
    NAPPLE_GESTURE_TOUCH = 3,
    NAPPLE_GESTURE_PRESS = 4,
    NAPPLE_GESTURE_TAP = 5,
    NAPPLE_GESTURE_PAN = 6,
    NAPPLE_GESTURE_SWIPE = 7,
    NAPPLE_GESTURE_PINCH = 8,
    NAPPLE_GESTURE_ROTATE = 9
};
/* GestureDirection */
enum { NAPPLE_GESTURE_DIR_NONE = 0, NAPPLE_GESTURE_DIR_LEFT = 1, NAPPLE_GESTURE_DIR_UP = 2, NAPPLE_GESTURE_DIR_RIGHT = 3, NAPPLE_GESTURE_DIR_DOWN = 4 };
/* GestureAttribute bits */
enum { NAPPLE_GESTURE_ATTR_NEW_TOUCH = 1u << 0, NAPPLE_GESTURE_ATTR_DOUBLE_TAP = 1u << 1 };

#define NAPPLE_GESTURE_MAX_POINTS 4
typedef struct napple_gesture_state {
    uint64_t context_number; /* identifies one gesture from first touch to release */
    uint32_t type, direction;
    int32_t x, y;                 /* centre of the touching points (Tap: where it happened) */
    int32_t delta_x, delta_y;     /* movement since the previous state of this context */
    float velocity_x, velocity_y; /* pixels per second at release (Swipe) or current */
    uint32_t attributes;
    float scale;          /* Pinch: current distance / starting distance; 1.0 otherwise */
    float rotation_angle; /* Rotate: degrees since the start; 0 otherwise */
    int32_t point_count;
    struct {
        int32_t x, y;
    } points[NAPPLE_GESTURE_MAX_POINTS];
} napple_gesture_state;

/* Constants (pixels in the 1280x720 touch space, times in nanoseconds). */
#define NAPPLE_GESTURE_PAN_DISTANCE 12      /* movement from the touch-down position that starts a Pan */
#define NAPPLE_GESTURE_PRESS_NS 500000000ll /* held still this long: Press */
#define NAPPLE_GESTURE_TAP_MAX_NS 400000000ll
#define NAPPLE_GESTURE_DOUBLE_TAP_NS 350000000ll
#define NAPPLE_GESTURE_DOUBLE_TAP_DISTANCE 40
#define NAPPLE_GESTURE_SWIPE_SPEED 600.0f  /* px/s at release that turns a Pan into a Swipe */
#define NAPPLE_GESTURE_PINCH_RATIO 0.08f   /* |scale - 1| that starts a Pinch */
#define NAPPLE_GESTURE_ROTATE_DEGREES 6.0f /* angle change that starts a Rotate */

#define NAPPLE_GESTURE_HISTORY 8
typedef struct napple_gesture {
    int active;       /* fingers are down */
    uint64_t context; /* last context number handed out */
    uint32_t phase;   /* NAPPLE_GESTURE_* of the state last emitted for this context */
    int64_t down_ns;
    int32_t start_x, start_y, last_x, last_y;
    uint32_t start_count;
    float start_dist, start_angle;
    int64_t hist_ns[NAPPLE_GESTURE_HISTORY];
    int32_t hist_x[NAPPLE_GESTURE_HISTORY], hist_y[NAPPLE_GESTURE_HISTORY];
    unsigned nhist;
    int pressed; /* Press already emitted for this context */
    int moved;   /* the touch left the tap radius */
    int64_t last_tap_ns;
    int32_t last_tap_x, last_tap_y;
    int have_last_tap;
} napple_gesture;

void napple_gesture_init(napple_gesture *g);
/* Feeds one touch frame (count 0 = no finger down). Returns 1 and fills *out when a new gesture state must be published. */
int napple_gesture_update(napple_gesture *g, int64_t now_ns, const napple_touch_point *pts, uint32_t count, napple_gesture_state *out);
