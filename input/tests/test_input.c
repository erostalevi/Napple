/* End-to-end headless: synthetic host events -> mapping -> HID shared memory (no controllers, temp config dir). */
#include "check.h"
#include "napple_input.h"
#include <unistd.h>

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
static const uint8_t *latest(napple_input *in, uint8_t *shm, const napple_hid_layout *l, int slot, uint32_t style_idx_off) {
    (void)in;
    return napple_hid_lifo_latest(shm, l->npad_base + (uint32_t)slot * l->npad_stride + style_idx_off, 0x28);
}

static int g_captured = -1, g_cap_key;
static void cap(void *u, int is_key, int code) {
    (void)u;
    g_cap_key = is_key;
    g_captured = code;
}

int main(void) {
    char dir[] = "/tmp/napple-input-test-XXXXXX";
    mkdtemp(dir);
    uint8_t *shm = aligned_alloc(16384, NAPPLE_HID_SHM_SIZE);
    napple_input_config c = {.title_id = "0100ABCD12345678", .config_dir = dir, .shm = shm, .use_game_controller = 0};
    napple_input *in;
    CHECK(napple_input_create(&c, &in) == 0, "create");
    napple_hid_layout l;
    napple_hid_layout_for(0, &l);
    napple_input_update(in, 1000000000ull);
    CHECK(r32(shm + l.npad_base) == (NAPPLE_STYLE_FULL_KEY) && napple_input_active_style(in, 0) == NAPPLE_STYLE_FULL_KEY, "player 1 reports a Pro Controller");
    const uint8_t *e = latest(in, shm, &l, 0, l.npad_lifo[0]);
    CHECK(e && r32(e + 0x20) == NAPPLE_ATTR_CONNECTED && r64(e + 8) == 0, "connected, no buttons");
    /* keyboard: L = A, W = stick up, Space = ZR */
    napple_input_key(in, 37, 1);
    napple_input_key(in, 13, 1);
    napple_input_key(in, 49, 1);
    napple_input_update(in, 1008000000ull);
    e = latest(in, shm, &l, 0, l.npad_lifo[0]);
    uint64_t b = r64(e + 8);
    CHECK((b & NAPPLE_BTN_MASK(NAPPLE_BTN_A)) && (b & NAPPLE_BTN_MASK(NAPPLE_BTN_ZR)) && !(b & NAPPLE_BTN_MASK(NAPPLE_BTN_B)), "buttons A+ZR (%llx)",
          (unsigned long long)b);
    CHECK((int32_t)r32(e + 0x14) == 32767 && (int32_t)r32(e + 0x10) == 0 && (b & NAPPLE_BTN_MASK(NAPPLE_BTN_STICK_L_UP)), "stick up full, direction bit");
    napple_input_key(in, 2, 1); /* D: diagonal is normalised to the unit circle */
    napple_input_update(in, 1016000000ull);
    e = latest(in, shm, &l, 0, l.npad_lifo[0]);
    float dx = (int32_t)r32(e + 0x10), dy = (int32_t)r32(e + 0x14);
    CHECK(fabsf(sqrtf(dx * dx + dy * dy) - 32767.f) < 3, "diagonal magnitude %f", sqrtf(dx * dx + dy * dy));
    /* keyboard section: usage 15 ('L') down */
    const uint8_t *k = napple_hid_lifo_latest(shm, l.keyboard, 0x30);
    CHECK(k && (k[16 + 1] & 0x80), "keyboard usage bitmap has L");
    /* shake: three whole side-to-side cycles on player 1's six-axis sensor, then back to rest */
    {
        const uint32_t ring = l.npad_base + l.npad_sixaxis[0];
        float peak = 0;
        uint64_t t = 1030000000ull;
        napple_input_shake(in);
        for (int i = 0; i < 200; i++, t += 5000000ull) {
            napple_input_update(in, t);
            const uint8_t *z = napple_hid_lifo_latest(shm, ring, 0x60);
            float a;
            uint32_t u = r32(z + 0x10);
            memcpy(&a, &u, 4);
            if (fabsf(a) > peak) peak = fabsf(a);
        }
        const uint8_t *z = napple_hid_lifo_latest(shm, ring, 0x60);
        float ax;
        uint32_t u = r32(z + 0x10);
        memcpy(&ax, &u, 4);
        CHECK(peak > 3.5f && fabsf(ax) < 1e-3f, "shake: x acceleration peaks at %f G and is back at rest (%f)", peak, ax);
    }
    /* on-screen controller: analog stick + buttons for player 1, merged with the keyboard (bigger deflection wins), cleared by focus loss */
    napple_input_key(in, 13, 0);
    napple_input_key(in, 2, 0);
    napple_input_key(in, 37, 0);
    napple_input_key(in, 49, 0);
    napple_input_virtual_pad(in, NAPPLE_BTN_MASK(NAPPLE_BTN_X) | NAPPLE_BTN_MASK(NAPPLE_BTN_L), 0.5f, 0.f, 0.f, -1.f);
    napple_input_update(in, 1020000000ull);
    e = latest(in, shm, &l, 0, l.npad_lifo[0]);
    b = r64(e + 8);
    CHECK((b & NAPPLE_BTN_MASK(NAPPLE_BTN_X)) && (b & NAPPLE_BTN_MASK(NAPPLE_BTN_L)) && !(b & NAPPLE_BTN_MASK(NAPPLE_BTN_A)), "virtual buttons X+L (%llx)",
          (unsigned long long)b);
    CHECK(abs((int32_t)r32(e + 0x10) - 16384) < 3 && (int32_t)r32(e + 0x14) == 0 && (int32_t)r32(e + 0x1c) == -32767, "virtual sticks are analog");
    napple_input_virtual_pad(in, 0, 0, 0, 0, 0);
    napple_input_update(in, 1022000000ull);
    e = latest(in, shm, &l, 0, l.npad_lifo[0]);
    CHECK((r64(e + 8) & 0xffff) == 0 && r32(e + 0x10) == 0, "virtual pad released");
    /* focus loss releases everything */
    napple_input_focus(in, 0);
    napple_input_update(in, 1024000000ull);
    e = latest(in, shm, &l, 0, l.npad_lifo[0]);
    CHECK((r64(e + 8) & 0xffff) == 0 && r32(e + 0x14) == 0, "unfocused = neutral");
    napple_input_focus(in, 1);
    /* blocked (overlay) swallows key presses */
    napple_input_set_blocked(in, 1);
    napple_input_key(in, 37, 1);
    napple_input_update(in, 1032000000ull);
    e = latest(in, shm, &l, 0, l.npad_lifo[0]);
    CHECK(!(r64(e + 8) & 1), "blocked ignores keys");
    napple_input_set_blocked(in, 0);
    napple_input_key(in, 37, 0);
    /* touch from the mouse */
    napple_input_mouse_position(in, 0.5f, 0.5f);
    napple_input_mouse_button(in, 0, 1);
    napple_input_update(in, 1040000000ull);
    const uint8_t *t = napple_hid_lifo_latest(shm, l.touch_screen, 0x290);
    CHECK(t && r32(t + 8) == 1 && r32(t + 0x10 + 0x10) == 639 && r32(t + 0x10 + 0x14) == 359, "touch at screen centre (%u,%u)", t ? r32(t + 0x20) : 0,
          t ? r32(t + 0x24) : 0);
    napple_input_mouse_button(in, 0, 0);
    napple_input_update(in, 1048000000ull);
    t = napple_hid_lifo_latest(shm, l.touch_screen, 0x290);
    CHECK(t && r32(t + 8) == 0, "release pushes count 0");
    /* styles: handheld-only title gets handheld npad too */
    napple_input_set_supported_styles(in, NAPPLE_STYLE_HANDHELD);
    napple_input_update(in, 1056000000ull);
    CHECK(napple_input_active_style(in, 0) == NAPPLE_STYLE_HANDHELD && napple_input_active_style(in, 9) == NAPPLE_STYLE_HANDHELD,
          "handheld-only: slots 0 and 9");
    CHECK(r32(shm + l.npad_base + 9 * l.npad_stride) == NAPPLE_STYLE_HANDHELD, "handheld slot style written");
    napple_input_set_supported_styles(in, NAPPLE_STYLE_JOY_DUAL | NAPPLE_STYLE_HANDHELD);
    napple_input_update(in, 1064000000ull);
    CHECK(napple_input_active_style(in, 0) == NAPPLE_STYLE_JOY_DUAL, "JoyDual preferred over Handheld");
    e = latest(in, shm, &l, 0, l.npad_lifo[2]);
    CHECK(e && (r32(e + 0x20) & 0x15) == 0x15, "dual attributes: connected + both halves");
    /* six-axis at rest */
    const uint8_t *six = napple_hid_lifo_latest(shm, l.npad_base + l.npad_sixaxis[2], 0x60);
    float az;
    memcpy(&az, six + 0x10 + 8, 4);
    CHECK(six && nearf(az, -1, 1e-6f), "six-axis at-rest pose");
    /* bindings persist per game and reload */
    napple_bindings bd;
    napple_input_get_bindings(in, &bd);
    bd.key[NAPPLE_BTN_A][0] = 0;
    CHECK(napple_input_set_bindings(in, &bd) == 0, "set bindings");
    char path[1100];
    snprintf(path, sizeof path, "%s/input.json", dir);
    CHECK(access(path, R_OK) == 0, "input.json written");
    napple_input *in2;
    napple_input_create(&(napple_input_config){.config_dir = dir, .shm = shm}, &in2);
    napple_bindings b2;
    napple_input_get_bindings(in2, &b2);
    CHECK(b2.key[NAPPLE_BTN_A][0] == 0, "bindings reloaded");
    napple_input_reset_bindings(in2);
    napple_input_destroy(in2);
    bd.stick_deadzone = 7;
    CHECK(napple_input_set_bindings(in, &bd) != 0, "invalid bindings rejected");
    /* capture for remap UI */
    napple_input_capture_next(in, cap, NULL);
    napple_input_key(in, 40, 1);
    CHECK(g_captured == 40 && g_cap_key == 1, "key captured");
    napple_input_key(in, 40, 0);
    /* analog tuning: invert Y of the left stick (keyboard W = up becomes down) */
    napple_input_set_supported_styles(in, NAPPLE_STYLE_FULL_KEY | NAPPLE_STYLE_JOY_DUAL | NAPPLE_STYLE_HANDHELD);
    napple_input_get_bindings(in, &bd);
    bd = (napple_bindings){0};
    napple_bindings_default(&bd);
    bd.invert[1] = 1;
    CHECK(napple_input_set_bindings(in, &bd) == 0, "set invert");
    napple_input_key(in, 13, 1);
    napple_input_update(in, 1072000000ull);
    e = latest(in, shm, &l, 0, l.npad_lifo[0]);
    CHECK((int32_t)r32(e + 0x14) == -32767 && (r64(e + 8) & NAPPLE_BTN_MASK(NAPPLE_BTN_STICK_L_DOWN)), "inverted left Y: W = down");
    napple_input_key(in, 13, 0);
    /* style preference: Joy-Con pair / handheld requested by the player win when supported, otherwise automatic */
    napple_bindings_default(&bd);
    bd.style = NAPPLE_STYLE_PREF_JOYCON;
    napple_input_set_bindings(in, &bd);
    napple_input_update(in, 1080000000ull);
    CHECK(napple_input_active_style(in, 0) == NAPPLE_STYLE_JOY_DUAL, "style preference Joy-Con");
    bd.style = NAPPLE_STYLE_PREF_HANDHELD;
    napple_input_set_bindings(in, &bd);
    napple_input_update(in, 1088000000ull);
    CHECK(napple_input_active_style(in, 0) == NAPPLE_STYLE_HANDHELD && napple_input_active_style(in, 9) == NAPPLE_STYLE_HANDHELD, "style preference handheld");
    napple_input_set_supported_styles(in, NAPPLE_STYLE_FULL_KEY);
    napple_input_update(in, 1096000000ull);
    CHECK(napple_input_active_style(in, 0) == NAPPLE_STYLE_FULL_KEY, "unsupported preference falls back");
    napple_bindings_default(&bd);
    bd.operation_mode = 1;
    napple_input_set_bindings(in, &bd);
    CHECK(napple_input_operation_mode(in) == 1, "operation mode handheld");
    napple_input_reset_bindings(in);
    CHECK(napple_input_operation_mode(in) == 0, "reset: docked");
    /* vibration from the guest is clamped and remembered */
    napple_vibration v = {5.f, 5000.f, -1.f, 1.f}, got;
    napple_input_set_vibration(in, 0, 1, &v);
    CHECK(napple_input_get_vibration(in, 0, 1, &got) && got.amp_low == 1.f && got.freq_low == 1250.f && got.amp_high == 0.f && got.freq_high == 40.f,
          "vibration clamped");
    CHECK(!napple_input_get_vibration(in, 0, 0, &got), "no vibration for the other side");
    /* per-player profile: player 1 gets A on keycode 40 (K) and nothing else; the shared set is untouched */
    {
        napple_bindings pb, shared, back;
        napple_bindings_default(&pb);
        for (int b = 0; b < NAPPLE_BTN_COUNT_BASIC; b++) pb.key[b][0] = pb.key[b][1] = NAPPLE_KEY_NONE;
        pb.key[NAPPLE_BTN_A][0] = 40;
        CHECK(napple_input_get_player_bindings(in, 0, &back) == 0, "player 1 follows the shared bindings");
        CHECK(napple_input_set_player_bindings(in, 0, &pb) == 0 && napple_input_get_player_bindings(in, 0, &back) == 1, "player 1 profile stored");
        napple_input_focus(in, 1);
        napple_input_key(in, 40, 1);
        napple_input_update(in, 1200000000ull);
        e = latest(in, shm, &l, 0, l.npad_lifo[0]);
        CHECK(e && (r64(e + 8) & NAPPLE_BTN_MASK(NAPPLE_BTN_A)), "profile key presses A");
        napple_input_key(in, 40, 0);
        napple_input_key(in, 0, 1); /* the shared A key (mac keycode 0) no longer does anything for player 1 */
        napple_input_update(in, 1300000000ull);
        e = latest(in, shm, &l, 0, l.npad_lifo[0]);
        CHECK(e && !(r64(e + 8) & NAPPLE_BTN_MASK(NAPPLE_BTN_A)), "shared key ignored while the profile is active");
        napple_input_key(in, 0, 0);
        char path[300];
        snprintf(path, sizeof path, "%s/input-p1.json", dir);
        CHECK(access(path, R_OK) == 0, "input-p1.json written");
        CHECK(napple_input_set_player_bindings(in, 8, &pb) != 0 && napple_input_get_player_bindings(in, -1, &back) == -1, "bad slot rejected");
        napple_input_get_bindings(in, &shared);
        CHECK(napple_input_clear_player_bindings(in, 0) == 0 && napple_input_get_player_bindings(in, 0, &back) == 0 && access(path, R_OK) != 0, "profile cleared");
    }
    /* thread mode keeps samples flowing */
    napple_input_start(in);
    usleep(300000);
    napple_input_stop(in);
    CHECK(napple_hid_lifo_count(shm, l.npad_base + l.npad_lifo[0]) == 17, "thread filled the ring");
    napple_input_destroy(in);
    free(shm);
    printf("{\"test\":\"input\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
