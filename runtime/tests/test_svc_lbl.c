/* Unit tests: lbl state and IPC table. */
#include "../src/services/lbl.c"
#include "svc_test.h"

int main(void) {
    LblState st; lbl_state_init(&st);
    CHECK(st.brightness == 0.5f && st.switch_status == LBL_SWITCH_ENABLED, "defaults");
    lbl_set_brightness(&st, 1.5f); CHECK(st.brightness == 1.0f, "clamp high");
    lbl_set_brightness(&st, -0.2f); CHECK(st.brightness == 0.0f, "clamp low");
    lbl_set_brightness(&st, 0.0f / 0.0f); CHECK(st.brightness == 0.5f, "NaN falls back to default");
    lbl_set_brightness(&st, 0.25f); CHECK(st.applied == 0.5f, "setting alone does not change the applied value");
    lbl_apply(&st); CHECK(st.applied == 0.25f, "apply copies the setting");

    IpcObject *o = lbl_open(); IpcCall c; float f;
    f = 0.8f; svc_call(o, 2, &f, 4, &c); CHECK(c.result == 0, "SetCurrentBrightnessSetting");
    svc_call(o, 3, NULL, 0, &c); CHECK(c.out_size == 4 && SVC_OUT(&c, float, 0) == 0.8f, "GetCurrentBrightnessSetting reads back the value");
    svc_call(o, 5, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 0.5f, "applied value unchanged before Apply");
    svc_call(o, 4, NULL, 0, &c); svc_call(o, 5, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 0.8f, "applied value after Apply");
    f = 7.0f; svc_call(o, 2, &f, 4, &c); svc_call(o, 3, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 1.0f, "out-of-range input is clamped");
    svc_call(o, 8, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == LBL_SWITCH_ENABLED, "backlight starts on");
    uint64_t fade = 0; svc_call(o, 7, &fade, 8, &c); svc_call(o, 8, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == LBL_SWITCH_DISABLED, "SwitchBacklightOff");
    svc_call(o, 6, &fade, 8, &c); svc_call(o, 8, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == LBL_SWITCH_ENABLED, "SwitchBacklightOn");
    svc_call(o, 10, NULL, 0, &c); svc_call(o, 11, NULL, 0, &c); CHECK(c.out[0] == 0, "DisableDimming");
    svc_call(o, 9, NULL, 0, &c); svc_call(o, 11, NULL, 0, &c); CHECK(c.out[0] == 1, "EnableDimming");
    svc_call(o, 12, NULL, 0, &c); svc_call(o, 14, NULL, 0, &c); CHECK(c.out[0] == 1, "EnableAutoBrightnessControl");
    svc_call(o, 23, NULL, 0, &c); CHECK(c.out[0] == 0, "no ambient light sensor");
    svc_call(o, 16, NULL, 0, &c); CHECK(c.out_size == 8 && c.out[0] == 0 && SVC_OUT(&c, float, 4) == 0.0f, "ambient value: {over=0, lux=0}");
    f = 0.3f; svc_call(o, 24, &f, 4, &c); svc_call(o, 25, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 0.3f, "VR-mode brightness is kept apart");
    /* save/load round trip, ambient sensor value, reflection delay, VR apply */
    f = 0.9f; svc_call(o, 2, &f, 4, &c); svc_call(o, 0, NULL, 0, &c);
    f = 0.1f; svc_call(o, 2, &f, 4, &c); svc_call(o, 1, NULL, 0, &c); svc_call(o, 3, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 0.9f, "Load restores what Save wrote");
    uint8_t amb[8] = {1}; float lux = 1234.5f; memcpy(amb + 4, &lux, 4); svc_call(o, 15, amb, 8, &c);
    svc_call(o, 16, NULL, 0, &c); CHECK(c.out[0] == 1 && SVC_OUT(&c, float, 4) == 1234.5f, "ambient value reads back");
    lux = -3.0f; memcpy(amb + 4, &lux, 4); svc_call(o, 15, amb, 8, &c); svc_call(o, 16, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 4) == 0.0f, "negative lux clamps to 0");
    uint8_t dl[8] = {1}; float lv = 0.75f; memcpy(dl + 4, &lv, 4); svc_call(o, 17, dl, 8, &c); uint32_t d1 = 1, d0 = 0;
    svc_call(o, 18, &d1, 4, &c); CHECK(SVC_OUT(&c, float, 0) == 0.75f, "delay level for direction 1");
    svc_call(o, 18, &d0, 4, &c); CHECK(SVC_OUT(&c, float, 0) == 0.0f, "direction 0 untouched");
    uint32_t d2 = 2; svc_call(o, 18, &d2, 4, &c); CHECK(c.result != 0, "bad direction");
    f = 0.35f; svc_call(o, 24, &f, 4, &c); svc_call(o, 26, NULL, 0, &c); svc_call(o, 5, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 0.35f, "VR apply sets the applied brightness");
    svc_call(o, 19, NULL, 0, &c); CHECK(c.result == 0, "brightness mapping accepted");
    printf("{\"test\":\"svc_lbl\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
