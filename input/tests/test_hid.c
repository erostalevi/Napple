/* HID shared memory writer: pure functions over a byte buffer. */
#include "check.h"
#include "napple_input.h"

static uint32_t r32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t r64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static float rf(const uint8_t *p) {
    float v;
    memcpy(&v, p, 4);
    return v;
}

static void t_layout(void) {
    napple_hid_layout l;
    napple_hid_layout_for(NAPPLE_HID_LAYOUT_LATEST, &l);
    CHECK(napple_hid_lifo_bytes(0x28) == 0x350, "npad lifo is 0x350");
    CHECK(napple_hid_lifo_bytes(0x60) == 0x708, "six-axis lifo is 0x708");
    CHECK(napple_hid_lifo_bytes(0x290) + 0x3C8 == 0x3000, "touch screen entry closes at 0x3000");
    CHECK(l.npad_lifo[0] == 0x28 && l.npad_lifo[1] == 0x378 && l.npad_lifo[6] == 0x1408, "npad lifo offsets");
    CHECK(l.npad_sixaxis[0] == 0x1758 && l.npad_sixaxis[5] == 0x3A80, "six-axis offsets");
    CHECK(l.npad_battery + 12 <= 0x5000 && l.npad_base == 0x9A00 && l.npad_base + 10 * l.npad_stride == 0x3BA00,
          "npad block ends where the gesture section starts");
    CHECK(l.keyboard + napple_hid_lifo_bytes(0x30) <= l.keyboard + 0x400 && l.mouse + napple_hid_lifo_bytes(0x28) <= l.mouse + 0x400,
          "keyboard/mouse fit 0x400");
    napple_hid_layout c;
    napple_hid_layout_for(NAPPLE_HID_LAYOUT_COMPAT, &c);
    CHECK(c.npad_lifo[5] == 0 && c.npad_sixaxis[0] == 0x28 + 5 * 0x350, "compat profile has five lifos then six-axis");
    CHECK(napple_npad_slot_from_id(0) == 0 && napple_npad_slot_from_id(7) == 7 && napple_npad_slot_from_id(0x10) == 8 && napple_npad_slot_from_id(0x20) == 9 &&
              napple_npad_slot_from_id(8) == -1,
          "npad id -> slot");
}

static void t_ring(void) {
    uint8_t *shm = calloc(1, NAPPLE_HID_SHM_SIZE);
    napple_hid_layout l;
    napple_hid_layout_for(NAPPLE_HID_LAYOUT_LATEST, &l);
    napple_hid_init(shm, &l);
    const uint32_t ring = l.npad_base + l.npad_lifo[0];
    CHECK(r64(shm + ring + 8) == 17 && napple_hid_lifo_count(shm, ring) == 17 && r64(napple_hid_lifo_latest(shm, ring, 0x28)) == 16,
          "initial ring is full of idle entries (S6: history reads of 16 entries)");
    for (uint64_t k = 0; k < 17; k++)
        CHECK(r64(shm + ring + 0x20 + k * 0x30) == 2 * k && r64(shm + ring + 0x28 + k * 0x30) == k,
              "prefilled entry %llu has outer 2*n and inner n sampling numbers", (unsigned long long)k);
    napple_hid_set_npad_style(shm, &l, 0, NAPPLE_STYLE_FULL_KEY | NAPPLE_STYLE_JOY_DUAL, 0);
    CHECK(r32(shm + l.npad_base) == (NAPPLE_STYLE_FULL_KEY | NAPPLE_STYLE_JOY_DUAL), "style set written at +0");
    napple_npad_state st = {.buttons = NAPPLE_BTN_MASK(NAPPLE_BTN_A) | NAPPLE_BTN_MASK(NAPPLE_BTN_ZR),
                            .left = {-1000, 32767},
                            .right = {5, -6},
                            .attributes = NAPPLE_ATTR_CONNECTED};
    uint64_t last = 0;
    for (int i = 0; i < 40; i++) {
        st.left.x = i;
        napple_hid_push_npad(shm, &l, 0, NAPPLE_STYLE_FULL_KEY, &st);
        const uint8_t *e = napple_hid_lifo_latest(shm, ring, 0x28);
        CHECK(e && r64(e) == (uint64_t)(17 + i) && (i == 0 || r64(e) == last + 1), "sampling number increments (%d)", i);
        last = r64(e);
        CHECK(r64(e + 8) == st.buttons && (int32_t)r32(e + 0x10) == i && (int32_t)r32(e + 0x14) == 32767 && (int32_t)r32(e + 0x18) == 5 &&
                  (int32_t)r32(e + 0x1c) == -6 && r32(e + 0x20) == NAPPLE_ATTR_CONNECTED,
              "state fields at libnx offsets (%d)", i);
        uint64_t count = napple_hid_lifo_count(shm, ring), tail = r64(shm + ring + 16);
        CHECK(count == 17 && tail == (uint64_t)(i % 17), "count %llu tail %llu at push %d", (unsigned long long)count, (unsigned long long)tail, i);
        /* outer copy of the sampling number */
        CHECK(r64(shm + ring + 0x20 + tail * 0x30) == 2 * (uint64_t)(17 + i), "outer sampling number is 2*n (even: odd means being written)");
    }
    /* the 17 entries hold the 17 newest states in ring order */
    int seen = 0;
    for (int k = 0; k < 17; k++) {
        uint64_t sn = r64(shm + ring + 0x20 + (uint64_t)k * 0x30);
        seen += sn >= 2 * 40;
    }
    CHECK(seen == 17, "ring holds the newest 17 samples");
    /* other styles go to their own lifo, others untouched */
    napple_hid_push_npad(shm, &l, 9, NAPPLE_STYLE_HANDHELD, &st);
    CHECK(r64(napple_hid_lifo_latest(shm, l.npad_base + 9 * l.npad_stride + l.npad_lifo[1], 0x28)) == 17 &&
              r64(napple_hid_lifo_latest(shm, l.npad_base + 9 * l.npad_stride + l.npad_lifo[0], 0x28)) == 16,
          "handheld lifo on slot 9 advanced, the full-key one untouched");
    free(shm);
}

static void t_sixaxis(void) {
    uint8_t *shm = calloc(1, NAPPLE_HID_SHM_SIZE);
    napple_hid_layout l;
    napple_hid_layout_for(NAPPLE_HID_LAYOUT_LATEST, &l);
    napple_hid_init(shm, &l);
    napple_sixaxis_state s = {.delta_time_ns = 5000000,
                              .acceleration = {0, 0, -1},
                              .angular_velocity = {.5f, 0, 0},
                              .angle = {0, .25f, 0},
                              .direction = {1, 0, 0, 0, 1, 0, 0, 0, 1},
                              .attributes = 1};
    napple_hid_push_sixaxis(shm, &l, 0, 0, &s);
    napple_hid_push_sixaxis(shm, &l, 0, 0, &s);
    const uint32_t ring = l.npad_base + l.npad_sixaxis[0];
    const uint8_t *e = napple_hid_lifo_latest(shm, ring, 0x60);
    CHECK(e && r64(e) == 5000000 && r64(e + 8) == 18, "six-axis: delta time then sampling number");
    CHECK(nearf(rf(e + 0x10 + 8), -1, 1e-6f) && nearf(rf(e + 0x1c), .5f, 1e-6f) && nearf(rf(e + 0x28 + 4), .25f, 1e-6f) && nearf(rf(e + 0x34 + 16), 1, 1e-6f) &&
              r32(e + 0x58) == 1,
          "six-axis fields");
    napple_hid_set_npad_device(shm, &l, 0, 3, 0xff, 4);
    CHECK(r32(shm + l.npad_base + l.npad_device_type) == 3 && r32(shm + l.npad_base + l.npad_battery) == 4, "device type / battery");
    free(shm);
}

static void t_touch_mouse_keyboard(void) {
    uint8_t *shm = calloc(1, NAPPLE_HID_SHM_SIZE);
    napple_hid_layout l;
    napple_hid_layout_for(NAPPLE_HID_LAYOUT_LATEST, &l);
    napple_hid_init(shm, &l);
    napple_touch_point t = {.finger_id = 3, .x = 640, .y = 360, .diameter_x = 15, .diameter_y = 15};
    napple_hid_push_touch(shm, &l, &t, 1);
    const uint8_t *e = napple_hid_lifo_latest(shm, l.touch_screen, 0x290);
    CHECK(e && r32(e + 8) == 1 && r32(e + 0x10 + 0xc) == 3 && r32(e + 0x10 + 0x10) == 640 && r32(e + 0x10 + 0x14) == 360 && (r32(e + 0x10 + 8) & 1),
          "touch point fields");
    napple_hid_push_touch(shm, &l, NULL, 0);
    e = napple_hid_lifo_latest(shm, l.touch_screen, 0x290);
    CHECK(e && r32(e + 8) == 0 && r64(e) == 18, "release = count 0 with next sampling number");
    napple_mouse_state m = {.x = 100, .y = 200, .delta_x = -3, .delta_y = 4, .wheel_y = 1, .buttons = 1, .attributes = 3};
    napple_hid_push_mouse(shm, &l, &m);
    e = napple_hid_lifo_latest(shm, l.mouse, 0x28);
    CHECK(e && (int32_t)r32(e + 8) == 100 && (int32_t)r32(e + 12) == 200 && (int32_t)r32(e + 16) == -3 && r32(e + 0x20) == 1 && r32(e + 0x24) == 3,
          "mouse fields");
    uint8_t keys[32] = {0};
    keys[0x04 / 8] |= 1 << (0x04 % 8); /* HID usage 4 = 'A' */
    napple_hid_push_keyboard(shm, &l, 2, keys);
    e = napple_hid_lifo_latest(shm, l.keyboard, 0x30);
    CHECK(e && r64(e + 8) == 2 && (e[16] & 0x10), "keyboard modifiers and usage bitmap");
    free(shm);
}

static void t_bounds(void) {
    /* exact-size heap block: any write outside would be caught by ASan */
    uint8_t *shm = malloc(NAPPLE_HID_SHM_SIZE);
    napple_hid_layout l;
    napple_hid_layout_for(NAPPLE_HID_LAYOUT_LATEST, &l);
    napple_hid_init(shm, &l);
    napple_npad_state st = {0};
    for (int slot = -3; slot < 14; slot++)
        for (uint32_t style = 0; style < 0x80; style = style ? style << 1 : 1) {
            napple_hid_push_npad(shm, &l, slot, style, &st);
            napple_hid_set_npad_style(shm, &l, slot, style, 0);
            napple_hid_push_sixaxis(shm, &l, slot, slot, (napple_sixaxis_state[]){{0}});
        }
    napple_touch_point many[40] = {{0}};
    napple_hid_push_touch(shm, &l, many, 40);
    CHECK(r32(napple_hid_lifo_latest(shm, l.touch_screen, 0x290) + 8) == 16, "touch count clamped to 16");
    for (int i = 0; i < 100000; i++) napple_hid_push_npad(shm, &l, i % 10, NAPPLE_STYLE_FULL_KEY, &st);
    CHECK(1, "no out-of-bounds writes");
    free(shm);
}

int main(void) {
    t_layout();
    t_ring();
    t_sixaxis();
    t_touch_mouse_keyboard();
    t_bounds();
    printf("{\"test\":\"hid\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
