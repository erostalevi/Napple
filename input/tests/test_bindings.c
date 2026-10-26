#include "check.h"
#include "napple_input.h"
#include "../src/internal.h"

int main(void) {
    napple_bindings d, r;
    napple_bindings_default(&d);
    CHECK(napple_bindings_validate(&d) == 0, "default valid");
    char json[16384];
    size_t n = napple_bindings_to_json(&d, json, sizeof json);
    CHECK(n > 100 && n < sizeof json, "json size %zu", n);
    CHECK(napple_bindings_from_json(json, n, &r) == 0 && !memcmp(&d, &r, sizeof d), "round trip equals default");
    napple_bindings m = d;
    m.key[NAPPLE_BTN_A][0] = 0;
    m.key[NAPPLE_BTN_A][1] = 49;
    m.pad[NAPPLE_BTN_B][1] = NAPPLE_PAD_WEST;
    m.stick_deadzone = 0.3f;
    m.mouse_touch = 0;
    m.face_layout = 2;
    m.stick_l[0] = NAPPLE_KEY_NONE;
    n = napple_bindings_to_json(&m, json, sizeof json);
    CHECK(napple_bindings_from_json(json, n, &r) == 0 && !memcmp(&m, &r, sizeof m), "round trip modified");
    CHECK(napple_bindings_to_json(&m, NULL, 0) == n, "size query matches");
    napple_bindings t = d;
    t.stick_range = 0.9f;
    t.stick_curve = 1.7f;
    t.trigger_threshold = 0.3f;
    t.invert[1] = 1;
    t.invert[2] = 1;
    t.style = NAPPLE_STYLE_PREF_JOYCON;
    t.operation_mode = 1;
    n = napple_bindings_to_json(&t, json, sizeof json);
    CHECK(napple_bindings_from_json(json, n, &r) == 0 && !memcmp(&t, &r, sizeof t), "round trip analog tuning, style and mode");
    CHECK(napple_bindings_validate(&(napple_bindings){.stick_deadzone = 0.1f, .stick_range = 0.4f, .stick_curve = 1, .trigger_threshold = .5f}) != 0,
          "range below 0.5 rejected");
    CHECK(napple_bindings_validate(&(napple_bindings){.stick_deadzone = 0.6f, .stick_range = 0.55f, .stick_curve = 1, .trigger_threshold = .5f}) != 0,
          "dead zone must stay under the range");
    CHECK(napple_bindings_validate(&(napple_bindings){.stick_range = 1, .stick_curve = 9, .trigger_threshold = .5f}) != 0, "curve range");
    napple_bindings o2 = d;
    CHECK(napple_bindings_from_json("{\"style\":\"bogus\"}", 18, &o2) != 0 && napple_bindings_from_json("{\"operation_mode\":\"x\"}", 22, &o2) != 0,
          "bad style / mode rejected");
    CHECK(napple_bindings_from_json("{\"deadzone\":0.9,\"stick_range\":0.8}", 33, &o2) != 0, "dead zone above range rejected from JSON");
    /* errors leave out untouched */
    napple_bindings keep = d, o = d;
    const char *bad[] = {"",
                         "[]",
                         "{",
                         "{\"version\":2}",
                         "{\"deadzone\":2}",
                         "{\"keys\":{\"A\":[999,0]}}",
                         "{\"pad\":{\"A\":[\"nope\"]}}",
                         "{\"stick_left\":[1,2,3]}",
                         "{\"face_layout\":\"x\"}",
                         "{\"deadzone\":\"a\"}",
                         "{} trailing"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++)
        CHECK(napple_bindings_from_json(bad[i], strlen(bad[i]), &o) != 0 && !memcmp(&o, &keep, sizeof o), "bad json %u rejected", i);
    CHECK(napple_bindings_from_json("{}", 2, &o) == 0 && !memcmp(&o, &d, sizeof d), "empty object = defaults");
    CHECK(napple_bindings_from_json("{\"keys\":{\"Bogus\":[1]},\"future\":5}", 33, &o) == 0, "unknown keys ignored");
    /* fuzz: mutated documents never crash */
    srand(7);
    n = napple_bindings_to_json(&d, json, sizeof json);
    char buf[16384];
    for (int i = 0; i < 20000; i++) {
        memcpy(buf, json, n);
        for (int k = 0; k < 4; k++) buf[rand() % n] = (char)(rand() & 0xff);
        napple_bindings_from_json(buf, rand() % (n + 1), &o);
    }
    CHECK(1, "fuzz survived");
    CHECK(napple_bindings_validate(&(napple_bindings){.stick_deadzone = 5}) != 0, "deadzone range");
    CHECK(napple_mac_to_hid_usage(0) == 4 && napple_mac_to_hid_usage(126) == 82 && napple_mac_to_hid_usage(200) == 0, "usage table");
    CHECK(!strcmp(napple_keycode_name(13), "W") && !strcmp(napple_button_name(NAPPLE_BTN_ZL), "ZL"), "names");
    printf("{\"test\":\"bindings\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
