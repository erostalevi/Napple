#include "gesture.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void napple_gesture_init(napple_gesture *g) { memset(g, 0, sizeof *g); }

static void centre(const napple_touch_point *p, uint32_t n, int32_t *cx, int32_t *cy) {
    int64_t sx = 0, sy = 0;
    for (uint32_t i = 0; i < n; i++) {
        sx += p[i].x;
        sy += p[i].y;
    }
    *cx = n ? (int32_t)(sx / n) : 0;
    *cy = n ? (int32_t)(sy / n) : 0;
}
static float dist2(const napple_touch_point *p) { return hypotf((float)p[0].x - (float)p[1].x, (float)p[0].y - (float)p[1].y); }
static float angle2(const napple_touch_point *p) { return atan2f((float)p[1].y - (float)p[0].y, (float)p[1].x - (float)p[0].x) * 57.29578f; }
static float wrap_degrees(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}
static void hist_push(napple_gesture *g, int64_t ns, int32_t x, int32_t y) {
    if (g->nhist == NAPPLE_GESTURE_HISTORY) {
        memmove(g->hist_ns, g->hist_ns + 1, (NAPPLE_GESTURE_HISTORY - 1) * sizeof g->hist_ns[0]);
        memmove(g->hist_x, g->hist_x + 1, (NAPPLE_GESTURE_HISTORY - 1) * sizeof g->hist_x[0]);
        memmove(g->hist_y, g->hist_y + 1, (NAPPLE_GESTURE_HISTORY - 1) * sizeof g->hist_y[0]);
        g->nhist--;
    }
    g->hist_ns[g->nhist] = ns;
    g->hist_x[g->nhist] = x;
    g->hist_y[g->nhist] = y;
    g->nhist++;
}
/* Velocity over the last ~120 ms of history, px/s. */
static void velocity(const napple_gesture *g, int64_t now_ns, float *vx, float *vy) {
    *vx = *vy = 0.0f;
    if (g->nhist < 2) return;
    unsigned first = g->nhist - 1;
    while (first > 0 && now_ns - g->hist_ns[first - 1] <= 120000000ll) first--;
    unsigned last = g->nhist - 1;
    int64_t dt = g->hist_ns[last] - g->hist_ns[first];
    if (dt <= 0) return;
    *vx = (float)(g->hist_x[last] - g->hist_x[first]) * 1e9f / (float)dt;
    *vy = (float)(g->hist_y[last] - g->hist_y[first]) * 1e9f / (float)dt;
}
static void fill_points(napple_gesture_state *o, const napple_touch_point *p, uint32_t n) {
    o->point_count = (int32_t)(n > NAPPLE_GESTURE_MAX_POINTS ? NAPPLE_GESTURE_MAX_POINTS : n);
    for (int i = 0; i < o->point_count; i++) {
        o->points[i].x = (int32_t)p[i].x;
        o->points[i].y = (int32_t)p[i].y;
    }
}
static uint32_t direction_of(float vx, float vy) {
    if (fabsf(vx) >= fabsf(vy)) return vx < 0 ? NAPPLE_GESTURE_DIR_LEFT : NAPPLE_GESTURE_DIR_RIGHT;
    return vy < 0 ? NAPPLE_GESTURE_DIR_UP : NAPPLE_GESTURE_DIR_DOWN;
}
static int emit(napple_gesture *g, napple_gesture_state *out, uint32_t type) {
    out->context_number = g->context;
    out->type = type;
    g->phase = type;
    return 1;
}

int napple_gesture_update(napple_gesture *g, int64_t now_ns, const napple_touch_point *pts, uint32_t count, napple_gesture_state *out) {
    memset(out, 0, sizeof *out);
    out->scale = 1.0f;
    if (count > NAPPLE_GESTURE_MAX_POINTS) count = NAPPLE_GESTURE_MAX_POINTS;

    if (!count) {
        if (!g->active) return 0;
        g->active = 0;
        int32_t cx = g->last_x, cy = g->last_y;
        out->x = cx;
        out->y = cy;
        out->context_number = g->context;
        float vx, vy;
        velocity(g, now_ns, &vx, &vy);
        if (g->phase == NAPPLE_GESTURE_PAN && hypotf(vx, vy) >= NAPPLE_GESTURE_SWIPE_SPEED && g->start_count == 1) {
            out->velocity_x = vx;
            out->velocity_y = vy;
            out->direction = direction_of(vx, vy);
            out->point_count = 1;
            out->points[0].x = cx;
            out->points[0].y = cy;
            return emit(g, out, NAPPLE_GESTURE_SWIPE);
        }
        if (g->phase == NAPPLE_GESTURE_TOUCH && !g->moved && g->start_count == 1 && now_ns - g->down_ns <= NAPPLE_GESTURE_TAP_MAX_NS) {
            int dbl = g->have_last_tap && now_ns - g->last_tap_ns <= NAPPLE_GESTURE_DOUBLE_TAP_NS &&
                      abs(cx - g->last_tap_x) <= NAPPLE_GESTURE_DOUBLE_TAP_DISTANCE && abs(cy - g->last_tap_y) <= NAPPLE_GESTURE_DOUBLE_TAP_DISTANCE;
            g->have_last_tap = !dbl; /* a double tap consumes the pair */
            g->last_tap_ns = now_ns;
            g->last_tap_x = cx;
            g->last_tap_y = cy;
            out->attributes = dbl ? NAPPLE_GESTURE_ATTR_DOUBLE_TAP : 0;
            out->point_count = 1;
            out->points[0].x = cx;
            out->points[0].y = cy;
            return emit(g, out, NAPPLE_GESTURE_TAP);
        }
        return emit(g, out, NAPPLE_GESTURE_COMPLETE);
    }

    int32_t cx, cy;
    centre(pts, count, &cx, &cy);
    if (!g->active) { /* first finger down: a new gesture */
        g->active = 1;
        g->context++;
        g->down_ns = now_ns;
        g->start_x = g->last_x = cx;
        g->start_y = g->last_y = cy;
        g->start_count = count;
        g->start_dist = count >= 2 ? dist2(pts) : 0.0f;
        g->start_angle = count >= 2 ? angle2(pts) : 0.0f;
        g->nhist = 0;
        g->pressed = g->moved = 0;
        hist_push(g, now_ns, cx, cy);
        out->x = cx;
        out->y = cy;
        out->attributes = NAPPLE_GESTURE_ATTR_NEW_TOUCH;
        fill_points(out, pts, count);
        return emit(g, out, NAPPLE_GESTURE_TOUCH);
    }
    if (count > g->start_count) { /* a second finger joined: pinch / rotate reference taken now */
        g->start_count = count;
        if (count >= 2) {
            g->start_dist = dist2(pts);
            g->start_angle = angle2(pts);
        }
    }
    hist_push(g, now_ns, cx, cy);

    if (count >= 2 && g->start_dist > 1.0f) {
        float scale = dist2(pts) / g->start_dist;
        float rot = wrap_degrees(angle2(pts) - g->start_angle);
        int pinch = fabsf(scale - 1.0f) >= NAPPLE_GESTURE_PINCH_RATIO, rotate = fabsf(rot) >= NAPPLE_GESTURE_ROTATE_DEGREES;
        if (pinch || rotate || g->phase == NAPPLE_GESTURE_PINCH || g->phase == NAPPLE_GESTURE_ROTATE) {
            uint32_t type = g->phase == NAPPLE_GESTURE_PINCH || g->phase == NAPPLE_GESTURE_ROTATE
                                ? g->phase
                                : (fabsf(scale - 1.0f) / NAPPLE_GESTURE_PINCH_RATIO >= fabsf(rot) / NAPPLE_GESTURE_ROTATE_DEGREES ? NAPPLE_GESTURE_PINCH
                                                                                                                                  : NAPPLE_GESTURE_ROTATE);
            out->x = cx;
            out->y = cy;
            out->delta_x = cx - g->last_x;
            out->delta_y = cy - g->last_y;
            out->scale = scale;
            out->rotation_angle = rot;
            g->last_x = cx;
            g->last_y = cy;
            g->moved = 1;
            fill_points(out, pts, count);
            return emit(g, out, type);
        }
        return 0;
    }

    if (!g->moved && hypotf((float)(cx - g->start_x), (float)(cy - g->start_y)) >= NAPPLE_GESTURE_PAN_DISTANCE) g->moved = 1;
    if (g->moved) {
        if (cx == g->last_x && cy == g->last_y && g->phase == NAPPLE_GESTURE_PAN) return 0;
        out->x = cx;
        out->y = cy;
        out->delta_x = cx - g->last_x;
        out->delta_y = cy - g->last_y;
        velocity(g, now_ns, &out->velocity_x, &out->velocity_y);
        g->last_x = cx;
        g->last_y = cy;
        fill_points(out, pts, count);
        return emit(g, out, NAPPLE_GESTURE_PAN);
    }
    if (!g->pressed && now_ns - g->down_ns >= NAPPLE_GESTURE_PRESS_NS) {
        g->pressed = 1;
        out->x = cx;
        out->y = cy;
        fill_points(out, pts, count);
        return emit(g, out, NAPPLE_GESTURE_PRESS);
    }
    return 0;
}
