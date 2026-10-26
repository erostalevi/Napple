/* Debug pad, gesture and console six-axis sections written by the backend once the guest activates them; at-rest detection. */
#include "check.h"
#include "napple_input.h"
#include <unistd.h>

static uint32_t r32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t r64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static float rf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
#define MS 1000000ull

int main(void) {
    char dir[] = "/tmp/napple-hidsec-XXXXXX";
    if (!mkdtemp(dir)) return 1;
    uint8_t *shm = aligned_alloc(16384, NAPPLE_HID_SHM_SIZE);
    napple_input_config c = {.title_id = "0100ABCD12345678", .config_dir = dir, .shm = shm, .use_game_controller = 0};
    napple_input *in;
    CHECK(napple_input_create(&c, &in) == 0, "create");
    napple_hid_layout l; napple_hid_layout_for(0, &l);
    CHECK(l.gesture == 0x3BA00 && l.console_sixaxis == 0x3C200 && l.console_sixaxis + 0x20 <= NAPPLE_HID_SHM_SIZE, "gesture section follows the npad entries");
    uint64_t t = 1000 * MS;
    napple_input_update(in, t);
    CHECK(napple_hid_lifo_count(shm, l.gesture) == 17 && r64(napple_hid_lifo_latest(shm, l.gesture, 0x60)) == 16, "gesture ring starts prefilled and idle");

    /* debug pad stays idle until activated, then follows player 1: A + ZR + stick */
    napple_input_key(in, 37, 1); napple_input_key(in, 49, 1); napple_input_key(in, 13, 1); /* A, ZR, stick up */
    napple_input_update(in, t += 8 * MS);
    const uint8_t *d = napple_hid_lifo_latest(shm, l.debug_pad, 0x20);
    CHECK(d && r32(d + 12) == 0, "debug pad is silent before ActivateDebugPad");
    napple_input_set_debug_pad_enabled(in, 1);
    napple_input_update(in, t += 8 * MS);
    d = napple_hid_lifo_latest(shm, l.debug_pad, 0x20);
    CHECK(d && r32(d + 8) == 1 && r32(d + 12) == ((1u << 0) | (1u << 7)) && (int32_t)r32(d + 28) == 32767 && (int32_t)r32(d + 24) == 0, "debug pad: connected, A + ZR, left stick up (%#x)", r32(d + 12));
    uint64_t n0 = napple_hid_lifo_count(shm, l.debug_pad), sampling = r64(d);
    napple_input_update(in, t += 8 * MS);
    d = napple_hid_lifo_latest(shm, l.debug_pad, 0x20);
    CHECK(r64(d) == sampling && napple_hid_lifo_count(shm, l.debug_pad) == n0, "unchanged state is not pushed again");
    napple_input_key(in, 37, 0); napple_input_key(in, 49, 0); napple_input_key(in, 13, 0);
    napple_input_update(in, t += 8 * MS);
    d = napple_hid_lifo_latest(shm, l.debug_pad, 0x20);
    CHECK(r32(d + 12) == 0 && r64(d) == sampling + 1, "release is a new sample");

    /* gesture: a tap from injected touch frames */
    napple_input_set_gesture_enabled(in, 1);
    napple_touch_point p = {.finger_id = 0, .x = 300, .y = 400, .diameter_x = 15, .diameter_y = 15};
    napple_input_touch_points(in, &p, 1);
    napple_input_update(in, t += 8 * MS);
    const uint8_t *g = napple_hid_lifo_latest(shm, l.gesture, 0x60);
    CHECK(g && r32(g + 16) == 3 /* Touch */ && (int32_t)r32(g + 24) == 300 && (int32_t)r32(g + 28) == 400 && r32(g + 60) == 1 && (r32(g + 48) & 1), "gesture: Touch at (300,400)");
    uint64_t ctx = r64(g + 8);
    napple_input_touch_points(in, NULL, 0);
    napple_input_update(in, t += 40 * MS);
    g = napple_hid_lifo_latest(shm, l.gesture, 0x60);
    CHECK(r32(g + 16) == 5 /* Tap */ && r64(g + 8) == ctx && (int32_t)r32(g + 24) == 300 && rf(g + 52) == 1.0f, "gesture: Tap, same context, scale 1");
    CHECK(r64(g + 0) == r64(g - 0), "sampling number present");
    /* touch section carries the injected frame too */
    napple_input_touch_points(in, &p, 1);
    napple_input_update(in, t += 8 * MS);
    const uint8_t *ts = napple_hid_lifo_latest(shm, l.touch_screen, 0x290);
    CHECK(ts && r32(ts + 8) == 1 && r32(ts + 0x10 + 0x10) == 300, "touch section shows the injected finger");
    /* pinch out with two fingers */
    napple_touch_point q[2] = {{.finger_id = 0, .x = 500, .y = 300}, {.finger_id = 1, .x = 600, .y = 300}};
    napple_input_touch_points(in, NULL, 0); napple_input_update(in, t += 8 * MS);
    napple_input_touch_points(in, q, 2); napple_input_update(in, t += 8 * MS);
    q[0].x = 450; q[1].x = 650; napple_input_touch_points(in, q, 2); napple_input_update(in, t += 8 * MS);
    g = napple_hid_lifo_latest(shm, l.gesture, 0x60);
    CHECK(r32(g + 16) == 8 /* Pinch */ && fabsf(rf(g + 52) - 2.0f) < 0.01f && r32(g + 60) == 2, "gesture: Pinch scale 2 with two points");

    /* six-axis at rest */
    napple_sixaxis_state six = {.acceleration = {0, 0, -1}};
    CHECK(napple_sixaxis_at_rest(&six), "1 G, no rotation: at rest");
    six.angular_velocity[1] = 0.2f; CHECK(!napple_sixaxis_at_rest(&six), "rotating: not at rest");
    six.angular_velocity[1] = 0; six.acceleration[0] = 2.0f; CHECK(!napple_sixaxis_at_rest(&six), "shaken: not at rest");
    napple_sixaxis_state got;
    CHECK(napple_input_get_sixaxis(in, 0, &got) && napple_sixaxis_at_rest(&got), "player 1's latest sample is at rest by default");
    CHECK(!napple_input_get_sixaxis(in, 5, &got), "an unused slot has no sample");
    napple_input_set_console_sixaxis_enabled(in, 1);
    napple_input_update(in, t += 8 * MS);
    CHECK(shm[l.console_sixaxis + 8] == 1 && r64(shm + l.console_sixaxis) >= 1, "console six-axis record: at rest, sampled");
    napple_input_shake(in);
    napple_input_update(in, t += 5 * MS); napple_input_update(in, t += 25 * MS);
    napple_input_get_sixaxis(in, 0, &got);
    CHECK(!napple_sixaxis_at_rest(&got) && shm[l.console_sixaxis + 8] == 0, "shaking: not at rest in the record either");

    /* npad assignment: deactivate, disconnect, swap via an injected second pad, merge, keyboard locks, touch size */
    napple_npad_assign a = {.active = 1};
    for (int i = 0; i < 10; i++) a.perm[i] = (uint8_t)i;
    napple_input_set_player_count(in, 2);
    napple_input_set_supported_styles(in, NAPPLE_STYLE_FULL_KEY | NAPPLE_STYLE_JOY_DUAL);
    napple_input_update(in, t += 8 * MS);
    const uint32_t ring0 = l.npad_base + l.npad_lifo[0];
    uint64_t c0 = napple_hid_lifo_count(shm, ring0), s0 = r64(napple_hid_lifo_latest(shm, ring0, 0x28));
    a.active = 0; napple_input_set_assignment(in, &a);
    napple_input_update(in, t += 8 * MS);
    CHECK(r64(napple_hid_lifo_latest(shm, ring0, 0x28)) == s0 && napple_hid_lifo_count(shm, ring0) == c0, "DeactivateNpad: npad section frozen");
    a.active = 1; napple_input_set_assignment(in, &a);
    napple_input_update(in, t += 8 * MS);
    CHECK(r64(napple_hid_lifo_latest(shm, ring0, 0x28)) > s0, "ActivateNpad: updating again");
    a.disconnected = 1; napple_input_set_assignment(in, &a);
    napple_input_update(in, t += 8 * MS);
    CHECK(napple_input_active_style(in, 0) == 0, "DisconnectNpad: slot 0 reports no style");
    a.disconnected = 0; napple_input_set_assignment(in, &a);
    napple_input_update(in, t += 8 * MS);
    CHECK(napple_input_active_style(in, 0) == NAPPLE_STYLE_FULL_KEY, "reconnected");
    a.dual_of[0] = 2; napple_input_set_assignment(in, &a);
    napple_input_update(in, t += 8 * MS);
    CHECK(napple_input_active_style(in, 0) == NAPPLE_STYLE_JOY_DUAL, "merged Joy-Cons: slot 0 is a dual pair");
    a.dual_of[0] = 0; napple_input_set_assignment(in, &a);
    napple_input_update(in, t += 8 * MS);
    a.keyboard_locks = 2 | 1; napple_input_set_assignment(in, &a);
    napple_input_update(in, t += 8 * MS);
    const uint8_t *kb = napple_hid_lifo_latest(shm, l.keyboard, 0x30);
    CHECK(kb && (r64(kb + 8) & ((1u << 5) | (1u << 7))) == ((1u << 5) | (1u << 7)) && !(r64(kb + 8) & (1u << 6)), "caps + num lock modifiers");
    napple_input_set_screen_size(in, 1920, 1080); napple_input_mouse_position(in, 1.0f, 1.0f);
    napple_input_update(in, t += 8 * MS);
    const uint8_t *mo = napple_hid_lifo_latest(shm, l.mouse, 0x28);
    CHECK(mo && (int32_t)r32(mo + 8) == 1919 && (int32_t)r32(mo + 12) == 1079, "SetTouchScreenResolution: positions use the new space (%d,%d)", (int)r32(mo + 8), (int)r32(mo + 12));
    napple_input_destroy(in);
    printf("{\"test\":\"hid_sections\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
