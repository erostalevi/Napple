/* Unit tests: hid settings core (portable). The hid IPC glue needs Mach and the input library, so it is tested on macOS only (test_hid_depth.c). */
#include "../src/services/hid_core.h"
#include "check.h"

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
    printf("{\"test\":\"svc_hidcore\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
