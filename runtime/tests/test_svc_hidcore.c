/* Unit tests: hid settings core (portable). The hid IPC glue needs Mach and the input library, so it is tested on macOS only (test_hid_depth.c). */
#include "../src/services/hid_core.h"
#include "check.h"
#include <string.h>

int main(void) {
    HidCore h; hidc_init(&h);
    CHECK(h.vibration_permitted == 1 && h.comm_mode == 3 && !h.styles_set && h.joy_hold_type == 0 && h.handheld_mode == 0, "defaults");
    HidcSensor *a = hidc_sensor(&h, 0x100), *b = hidc_sensor(&h, 0x200);
    CHECK(a && b && a != b && hidc_sensor(&h, 0x100) == a, "a handle gets one entry");
    CHECK(a->fusion_enabled == 1 && a->gain == HIDC_DEFAULT_GAIN && a->reduction == HIDC_DEFAULT_REDUCTION && a->zero_drift == HIDC_ZERO_DRIFT_STANDARD, "sensor defaults");
    a->fusion_enabled = 0; a->gain = 0.5f; hidc_reset_fusion(a);
    CHECK(a->fusion_enabled == 1 && a->gain == HIDC_DEFAULT_GAIN, "reset restores the defaults");
    CHECK(hidc_set_zero_drift(a, HIDC_ZERO_DRIFT_TIGHT) == 0 && a->zero_drift == 2 && b->zero_drift == 1, "zero-drift mode is per handle");
    CHECK(hidc_set_zero_drift(a, 3) == -1 && a->zero_drift == 2, "invalid mode refused, value kept");
    for (uint32_t i = 0; i < HIDC_MAX_SENSORS; i++) hidc_sensor(&h, 0x1000 + i);
    CHECK(hidc_sensor(&h, 0xdead) == NULL, "table full");
    CHECK(hidc_led_pattern(0) == 0x1 && hidc_led_pattern(1) == 0x3 && hidc_led_pattern(3) == 0xF && hidc_led_pattern(4) == 0x9 && hidc_led_pattern(7) == 0x6, "player LED patterns");
    CHECK(hidc_led_pattern(0x20) == 0 && hidc_led_pattern(0x10) == 0 && hidc_led_pattern(8) == 0, "handheld / other / unknown: no LED");
    CHECK(hidc_home_protection_get(&h, 0) == 0, "home protection default off");
    hidc_home_protection_set(&h, 0, 1); hidc_home_protection_set(&h, 0x20, 1); hidc_home_protection_set(&h, 0, 0);
    CHECK(hidc_home_protection_get(&h, 0) == 0 && hidc_home_protection_get(&h, 0x20) == 1 && h.nhome == 2, "home protection per npad id, replace in place");
    for (uint32_t i = 0; i < 40; i++) hidc_home_protection_set(&h, 100 + i, 1);
    CHECK(h.nhome == HIDC_MAX_HOME_IDS, "home table is bounded");
    /* npad assignment */
    HidCore n; hidc_init(&n);
    CHECK(n.npad_active == 1 && hidc_pad_source(&n, 3) == 3 && hidc_slot_from_id(0x20) == 9 && hidc_slot_from_id(0x10) == 8 && hidc_slot_from_id(8) == -1, "defaults: active, identity source, id -> slot");
    CHECK(hidc_swap(&n, 0, 2) == 0 && hidc_pad_source(&n, 0) == 2 && hidc_pad_source(&n, 2) == 0 && hidc_pad_source(&n, 1) == 1, "swap two players");
    CHECK(hidc_swap(&n, 2, 0) == 0 && hidc_pad_source(&n, 0) == 0, "swapping back restores the identity");
    CHECK(hidc_swap(&n, 1, 1) == -2 && hidc_swap(&n, 0, 0x20) == -1 && hidc_swap(&n, 8, 0) == -1, "swap rejects the same pad and non-player ids");
    CHECK(hidc_merge(&n, 0, 1) == 0 && n.dual_of[0] == 2 && (n.disconnected & 2), "merge: slot 1 joins slot 0 and is freed");
    CHECK(hidc_merge(&n, 3, 3) == -2 && hidc_merge(&n, 0, 9) == -1, "merge validation");
    CHECK(hidc_disconnect(&n, 0x20) == 0 && (n.disconnected & (1u << 9)) && hidc_disconnect(&n, 99) == -1, "disconnect by npad id");
    hidc_reconnect_all(&n); CHECK(n.disconnected == 0, "re-arming reconnects");
    CHECK(hidc_keyboard_lock_event(&n, 1u << 3) == 2 && hidc_keyboard_lock_event(&n, 1u << 2) == 3 && hidc_keyboard_lock_event(&n, 1u << 4) == 1 && hidc_keyboard_lock_event(&n, (1u << 0) | (1u << 8)) == 5, "lock key on / toggle / off");
    CHECK(hidc_capture_get(&n, 1) == -1 && hidc_capture_assign(&n, 1, 12) == 0 && hidc_capture_get(&n, 1) == 12 && hidc_capture_assign(&n, 1, 3) == 0 && hidc_capture_get(&n, 1) == 3 && n.ncapture == 1, "capture assignment per style, replaced in place");
    CHECK(hidc_capture_assign(&n, 3, 1) == -1 && hidc_capture_assign(&n, 0, 1) == -1, "not a single style bit");
    hidc_capture_clear(&n, 1); CHECK(hidc_capture_get(&n, 1) == -1, "cleared");
    CHECK(hidc_set_touch_resolution(&n, 1920, 1080) == 0 && n.touch_w == 1920 && hidc_set_touch_resolution(&n, 0, 5) == -1 && hidc_set_touch_resolution(&n, 5000, 5) == -1 && n.touch_w == 1920, "touch resolution validated, last good kept");
    uint32_t id, side;
    CHECK(hidc_vib_handle_decode(0x01000000u | (2u << 16), &id, &side) == 0 && id == 2 && side == 1, "right device of player 3");
    CHECK(hidc_vib_handle_decode(1u << 8, &id, &side) == 0 && id == 0x20 && side == 0, "handheld style index maps to 0x20");
    CHECK(hidc_vib_handle_decode(2u << 24, 0, 0) == -1 && hidc_vib_handle_decode(9u << 16, 0, 0) == -1, "bad device index / npad id");
    CHECK(hidc_vib_activate(&n, 0x01000000u) == 0 && hidc_vib_is_active(&n, 0x01000000u) && !hidc_vib_is_active(&n, 0) && hidc_vib_activate(&n, 0x01000000u) == 0 && n.nvib_active == 1, "activation is idempotent");
    CHECK(hidc_vib_activate(&n, 2u << 24) == -1, "invalid handle refused");
    for (uint32_t i = 0; i < 40; i++) hidc_vib_activate(&n, i << 16 & 0x00070000u | 0x00000000u | ((i & 1) << 24));
    CHECK(n.nvib_active <= HIDC_MAX_VIB_ACTIVE, "active list is bounded");
    { /* npad styles */
        HidCore s; hidc_init(&s);
        CHECK(HIDC_STYLE_GC == 0x20 && HIDC_STYLE_PALMA == 0x40 && HIDC_STYLE_LARK == 0x80 && HIDC_STYLE_LUCIA == 0x200 && HIDC_STYLE_LAGER == 0x800 && HIDC_STYLE_SYSTEM == 0x40000000u, "style bits are libnx's");
        CHECK(hidc_style_supported(&s, HIDC_STYLE_FULL_KEY | HIDC_STYLE_JOY_DUAL) && !hidc_style_supported(&s, HIDC_STYLE_GC) && !hidc_style_supported(&s, HIDC_STYLE_PALMA) && !hidc_style_supported(&s, 0), "before any Set: the five standard styles");
        s.styles = HIDC_STYLE_GC | HIDC_STYLE_PALMA | HIDC_STYLE_LARK | 0x00ff0000u; s.styles_set = 1;
        CHECK(hidc_style_supported(&s, HIDC_STYLE_GC) && hidc_style_supported(&s, HIDC_STYLE_PALMA | HIDC_STYLE_LARK) && !hidc_style_supported(&s, HIDC_STYLE_FULL_KEY), "GameCube / Palma / NES supported only when set");
        CHECK(hidc_styles_sanitize(s.styles) == (HIDC_STYLE_GC | HIDC_STYLE_PALMA | HIDC_STYLE_LARK) && hidc_styles_sanitize(0xffffffffu) == HIDC_STYLES_KNOWN, "unowned bits dropped");
        CHECK(!strcmp(hidc_style_name(HIDC_STYLE_GC), "GameCube") && !strcmp(hidc_style_name(HIDC_STYLE_PALMA), "Palma") && !strcmp(hidc_style_name(HIDC_STYLE_LARK), "Lark") && !hidc_style_name(3) && !hidc_style_name(1u << 15) && !hidc_style_name(0), "style names; none for sets or unowned bits");
        unsigned named = 0; for (unsigned b = 0; b < 32; b++) if (hidc_style_name(1u << b)) named++;
        CHECK(named == 14, "fourteen named styles");
        CHECK(hidc_style_is_retro(HIDC_STYLE_LARK) && hidc_style_is_retro(HIDC_STYLE_LAGON | HIDC_STYLE_LUCIA) && !hidc_style_is_retro(HIDC_STYLE_GC) && !hidc_style_is_retro(HIDC_STYLE_LARK | HIDC_STYLE_PALMA) && !hidc_style_is_retro(0), "Switch Online pads");
        CHECK((HIDC_STYLES_ALL_PAD & HIDC_STYLE_GC) && (HIDC_STYLES_ALL_PAD & HIDC_STYLE_PALMA) && (HIDC_STYLES_ALL_PAD & HIDC_STYLE_LARK) && (HIDC_STYLES_ALL_PAD & HIDC_STYLES_STANDARD) == HIDC_STYLES_STANDARD, "system policy covers GameCube, Palma, NES");
        CHECK(hidc_capture_assign(&s, HIDC_STYLE_GC, 3) == 0 && hidc_capture_get(&s, HIDC_STYLE_GC) == 3, "capture assignment accepts the GameCube style");
    }
    printf("{\"test\":\"svc_hidcore\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
