/* Gesture recognition from touch frames. */
#include "check.h"
#include "../src/gesture.h"

#define MS 1000000ll
static napple_touch_point P(uint32_t id, uint32_t x, uint32_t y) { return (napple_touch_point){.finger_id = id, .x = x, .y = y}; }

int main(void) {
    napple_gesture g; napple_gesture_state s; napple_touch_point a[2];
    napple_gesture_init(&g);
    CHECK(!napple_gesture_update(&g, 0, NULL, 0, &s), "idle: nothing to publish");

    /* tap */
    a[0] = P(0, 100, 200);
    CHECK(napple_gesture_update(&g, 0, a, 1, &s) && s.type == NAPPLE_GESTURE_TOUCH && s.x == 100 && s.y == 200 && (s.attributes & NAPPLE_GESTURE_ATTR_NEW_TOUCH) && s.point_count == 1, "touch down");
    uint64_t ctx = s.context_number;
    CHECK(!napple_gesture_update(&g, 16 * MS, a, 1, &s), "holding still publishes nothing yet");
    CHECK(napple_gesture_update(&g, 80 * MS, NULL, 0, &s) && s.type == NAPPLE_GESTURE_TAP && s.x == 100 && s.y == 200 && s.context_number == ctx && !(s.attributes & NAPPLE_GESTURE_ATTR_DOUBLE_TAP), "quick lift is a Tap");
    CHECK(!napple_gesture_update(&g, 90 * MS, NULL, 0, &s), "nothing after the lift");
    /* second tap close in time and place: double tap */
    CHECK(napple_gesture_update(&g, 200 * MS, a, 1, &s) && s.type == NAPPLE_GESTURE_TOUCH && s.context_number == ctx + 1, "new context");
    CHECK(napple_gesture_update(&g, 240 * MS, NULL, 0, &s) && s.type == NAPPLE_GESTURE_TAP && (s.attributes & NAPPLE_GESTURE_ATTR_DOUBLE_TAP), "double tap");
    /* third tap right after is a single again (the pair was consumed) */
    napple_gesture_update(&g, 260 * MS, a, 1, &s); napple_gesture_update(&g, 290 * MS, NULL, 0, &s);
    CHECK(s.type == NAPPLE_GESTURE_TAP && !(s.attributes & NAPPLE_GESTURE_ATTR_DOUBLE_TAP), "tap after a double tap is single");
    /* a tap far away or much later is single */
    a[0] = P(0, 900, 600); napple_gesture_update(&g, 2000 * MS, a, 1, &s); napple_gesture_update(&g, 2040 * MS, NULL, 0, &s);
    CHECK(s.type == NAPPLE_GESTURE_TAP && !(s.attributes & NAPPLE_GESTURE_ATTR_DOUBLE_TAP), "distant tap");

    /* press: held 600 ms without moving, then released = Complete (not a Tap) */
    napple_gesture_init(&g); a[0] = P(0, 300, 300);
    napple_gesture_update(&g, 0, a, 1, &s);
    CHECK(!napple_gesture_update(&g, 400 * MS, a, 1, &s), "not yet a press");
    CHECK(napple_gesture_update(&g, 520 * MS, a, 1, &s) && s.type == NAPPLE_GESTURE_PRESS, "held still: Press");
    CHECK(!napple_gesture_update(&g, 600 * MS, a, 1, &s), "Press is published once");
    CHECK(napple_gesture_update(&g, 700 * MS, NULL, 0, &s) && s.type == NAPPLE_GESTURE_COMPLETE, "release after a press completes");

    /* slow pan then release: Complete, with deltas along the way */
    napple_gesture_init(&g); a[0] = P(0, 100, 100);
    napple_gesture_update(&g, 0, a, 1, &s);
    a[0] = P(0, 105, 100); CHECK(!napple_gesture_update(&g, 16 * MS, a, 1, &s), "inside the pan radius");
    a[0] = P(0, 130, 100); CHECK(napple_gesture_update(&g, 32 * MS, a, 1, &s) && s.type == NAPPLE_GESTURE_PAN && s.delta_x == 30 && s.x == 130, "pan starts with the full movement so far (delta %d)", s.delta_x);
    a[0] = P(0, 140, 104); CHECK(napple_gesture_update(&g, 400 * MS, a, 1, &s) && s.type == NAPPLE_GESTURE_PAN && s.delta_x == 10 && s.delta_y == 4, "pan delta");
    CHECK(napple_gesture_update(&g, 800 * MS, NULL, 0, &s) && s.type == NAPPLE_GESTURE_COMPLETE, "slow release: Complete");

    /* fast flick right: Swipe with direction and velocity */
    napple_gesture_init(&g); a[0] = P(0, 100, 300); napple_gesture_update(&g, 0, a, 1, &s);
    for (int i = 1; i <= 5; i++) { a[0] = P(0, 100 + 40 * i, 300 + i); napple_gesture_update(&g, i * 16 * MS, a, 1, &s); }
    CHECK(napple_gesture_update(&g, 96 * MS, NULL, 0, &s) && s.type == NAPPLE_GESTURE_SWIPE && s.direction == NAPPLE_GESTURE_DIR_RIGHT && s.velocity_x > 1000.0f, "swipe right (vx %.0f dir %u type %u)", s.velocity_x, s.direction, s.type);
    napple_gesture_init(&g); a[0] = P(0, 600, 600); napple_gesture_update(&g, 0, a, 1, &s);
    for (int i = 1; i <= 5; i++) { a[0] = P(0, 600, 600 - 50 * i); napple_gesture_update(&g, i * 16 * MS, a, 1, &s); }
    napple_gesture_update(&g, 96 * MS, NULL, 0, &s); CHECK(s.type == NAPPLE_GESTURE_SWIPE && s.direction == NAPPLE_GESTURE_DIR_UP, "swipe up");

    /* pinch out */
    napple_gesture_init(&g); a[0] = P(0, 500, 300); a[1] = P(1, 600, 300);
    CHECK(napple_gesture_update(&g, 0, a, 2, &s) && s.type == NAPPLE_GESTURE_TOUCH && s.point_count == 2 && s.x == 550, "two-finger touch");
    a[0] = P(0, 450, 300); a[1] = P(1, 650, 300);
    CHECK(napple_gesture_update(&g, 16 * MS, a, 2, &s) && s.type == NAPPLE_GESTURE_PINCH && nearf(s.scale, 2.0f, 0.01f) && nearf(s.rotation_angle, 0.0f, 0.1f), "pinch out: scale 2");
    a[0] = P(0, 540, 300); a[1] = P(1, 560, 300); CHECK(napple_gesture_update(&g, 32 * MS, a, 2, &s) && s.type == NAPPLE_GESTURE_PINCH && s.scale < 0.3f, "pinch in keeps the type");
    CHECK(napple_gesture_update(&g, 48 * MS, NULL, 0, &s) && s.type == NAPPLE_GESTURE_COMPLETE, "pinch ends with Complete");

    /* rotate 90 degrees (distance unchanged) */
    napple_gesture_init(&g); a[0] = P(0, 500, 300); a[1] = P(1, 600, 300); napple_gesture_update(&g, 0, a, 2, &s);
    a[0] = P(0, 550, 250); a[1] = P(1, 550, 350);
    CHECK(napple_gesture_update(&g, 16 * MS, a, 2, &s) && s.type == NAPPLE_GESTURE_ROTATE && nearf(s.rotation_angle, 90.0f, 0.5f) && nearf(s.scale, 1.0f, 0.01f), "rotate: %.1f degrees", s.rotation_angle);
    printf("{\"test\":\"gesture\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
