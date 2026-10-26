/* Unit tests: psm core state and the psm IPC table (handlers reached by #including the service source). */
#include "../src/services/psm.c"
#include "svc_test.h"

int main(void) {
    PsmState st;
    psm_state_init(&st, 1, NULL);
    CHECK(st.percent == 100 && st.ac_online && psm_charger_type(&st) == PSM_CHARGER_ENOUGH_POWER, "docked default: 100%% on AC");
    psm_state_init(&st, 0, NULL);
    CHECK(st.percent == 100 && !st.ac_online && psm_charger_type(&st) == PSM_CHARGER_NONE && !psm_enough_power_supplied(&st), "handheld default: on battery");
    psm_state_init(&st, 0, "42,ac");
    CHECK(st.percent == 42 && st.ac_online && psm_charger_type(&st) == PSM_CHARGER_ENOUGH_POWER, "NAPPLE_BATTERY=42,ac");
    psm_state_init(&st, 1, "7,battery");
    CHECK(st.percent == 7 && !st.ac_online, "NAPPLE_BATTERY=7,battery on a docked console");
    psm_state_init(&st, 1, "250"); CHECK(st.percent == 100, "percent is clamped to 100");
    psm_state_init(&st, 1, "-5"); CHECK(st.percent == 0, "negative percent is clamped to 0");
    psm_state_init(&st, 0, "abc"); CHECK(st.percent == 100 && !st.ac_online, "garbage keeps the defaults");
    CHECK(psm_voltage_state(0) == PSM_VOLTAGE_NEEDS_SHUTDOWN && psm_voltage_state(3) == PSM_VOLTAGE_NEEDS_SLEEP &&
          psm_voltage_state(10) == PSM_VOLTAGE_NO_BOOST && psm_voltage_state(16) == PSM_VOLTAGE_NORMAL && psm_voltage_state(100) == PSM_VOLTAGE_NORMAL, "voltage state thresholds");

    /* IPC table */
    g_cfg.op_mode_handheld = 0; psm_ready = 0;
    IpcObject *o = psm_open(); IpcCall c;
    svc_call(o, 0, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 4 && SVC_OUT(&c, uint32_t, 0) == 100, "GetBatteryChargePercentage = 100");
    svc_call(o, 1, NULL, 0, &c); CHECK(c.out_size == 4 && SVC_OUT(&c, uint32_t, 0) == PSM_CHARGER_ENOUGH_POWER, "docked: charger = enough power");
    svc_call(o, 14, NULL, 0, &c); CHECK(c.out_size == 1 && c.out[0] == 1, "IsEnoughPowerSupplied when docked");
    svc_call(o, 12, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == PSM_VOLTAGE_NORMAL, "GetBatteryVoltageState normal at 100%%");
    svc_call(o, 13, NULL, 0, &c); CHECK(c.out_size == 8 && SVC_OUT(&c, double, 0) == 100.0, "GetRawBatteryChargePercentage is a double");
    svc_call(o, 3, NULL, 0, &c); svc_call(o, 4, NULL, 0, &c); CHECK(c.out[0] == 0, "DisableBatteryCharging is reflected");
    svc_call(o, 2, NULL, 0, &c); svc_call(o, 4, NULL, 0, &c); CHECK(c.out[0] == 1, "EnableBatteryCharging is reflected");
    g_cfg.op_mode_handheld = 1; psm_ready = 0;
    svc_call(o, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == PSM_CHARGER_NONE, "handheld: no charger");
    svc_call(o, 14, NULL, 0, &c); CHECK(c.out[0] == 0, "handheld: not enough power");
    svc_call(o, 8, NULL, 0, &c); svc_call(o, 14, NULL, 0, &c); CHECK(c.out[0] == 1, "enough-power emulation");
    svc_call(o, 7, NULL, 0, &c); CHECK(c.nout_objects == 1, "OpenSession returns an object");
    IpcObject *sess = c.out_objects[0];
    svc_call(sess, 0, NULL, 0, &c); CHECK(c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "BindStateChangeEvent: unsignalled event");
    svc_call(sess, 2, "\x01", 1, &c); CHECK(c.result == 0, "SetChargerTypeChangeEventEnabled accepted");

    /* state-change events: nothing fires until the session enables a kind, then a toggled emulation / host change signals */
    g_cfg.op_mode_handheld = 1; psm_ready = 0;
    svc_call(o, 7, NULL, 0, &c); IpcObject *se = c.out_objects[0];
    svc_call(se, 0, NULL, 0, &c); uint32_t ev = c.out_copy[0];
    svc_call(o, 8, NULL, 0, &c); CHECK(!event_is_signaled(ev), "no kind enabled: the emulation toggle does not signal");
    svc_call(o, 9, NULL, 0, &c);
    uint8_t on = 1; svc_call(se, 3, &on, 1, &c);
    svc_call(o, 8, NULL, 0, &c); CHECK(event_is_signaled(ev), "power-supply change signals the bound event");
    event_clear_handle(ev); svc_call(o, 8, NULL, 0, &c); CHECK(!event_is_signaled(ev), "enabling emulation again changes nothing");
    uint8_t off = 0; svc_call(se, 3, &off, 1, &c);
    psm_host_power_changed(1, 100); CHECK(!event_is_signaled(ev), "charger change not enabled on this session");
    svc_call(se, 2, &on, 1, &c);
    psm_host_power_changed(0, 100); CHECK(event_is_signaled(ev), "charger removal signals once the charger kind is enabled");
    event_clear_handle(ev); svc_call(se, 1, NULL, 0, &c); psm_host_power_changed(1, 100); CHECK(!event_is_signaled(ev), "unbound: silent");
    svc_call(o, 16, NULL, 0, &c); uint32_t info = c.out_copy[0]; CHECK(!event_is_signaled(info), "charge info event starts clear");
    psm_host_power_changed(1, 4); CHECK(event_is_signaled(info) && state()->percent == 4 && psm_voltage_state(state()->percent) == PSM_VOLTAGE_NEEDS_SLEEP, "voltage change signals the info event");
    svc_call(o, 6, NULL, 0, &c); CHECK(c.result != 0, "release without acquire");
    svc_call(o, 5, NULL, 0, &c); svc_call(o, 6, NULL, 0, &c); CHECK(c.result == 0, "acquire then release");
    svc_call(o, 15, NULL, 0, &c); CHECK(SVC_OUT(&c, double, 0) == 100.0, "battery age 100");
    printf("{\"test\":\"svc_psm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
