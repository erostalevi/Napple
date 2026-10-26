/* lbl: backlight controller (nn::lbl). Brightness and switch state are real values the guest can read back; nothing reaches the Mac's
 * display. The light sensor is reported as unavailable. Commands whose argument layout is not certain are accepted without reading them. */
#include "svc_common.h"
#include "lbl_core.h"
#include "hz_result.h"

static LblState lbl;
static int lbl_ready;
static LblState *state(void) {
    if (!lbl_ready) {
        lbl_state_init(&lbl);
        lbl_ready = 1;
    }
    return &lbl;
}

static void lbl_load_current(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    lbl_load(state());
}
static void lbl_save_current(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    lbl_save(state());
}
static void lbl_set_current(IpcObject *s, IpcCall *c) {
    (void)s;
    lbl_set_brightness(state(), IPC_IN(c, float, 0));
}
static void lbl_get_current(IpcObject *s, IpcCall *c) {
    (void)s;
    float v = state()->brightness;
    IPC_OUT(c, v);
}
static void lbl_apply_current(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    lbl_apply(state());
}
static void lbl_get_applied(IpcObject *s, IpcCall *c) {
    (void)s;
    float v = state()->applied;
    IPC_OUT(c, v);
}
static void lbl_switch_on(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    lbl_switch(state(), 1);
}
static void lbl_switch_off(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    lbl_switch(state(), 0);
}
static void lbl_get_switch_status(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = state()->switch_status;
    IPC_OUT(c, v);
}
static void lbl_enable_dimming(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    state()->dimming = 1;
}
static void lbl_disable_dimming(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    state()->dimming = 0;
}
static void lbl_is_dimming(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)state()->dimming;
    IPC_OUT(c, v);
}
static void lbl_enable_auto(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    state()->auto_brightness = 1;
}
static void lbl_disable_auto(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    state()->auto_brightness = 0;
}
static void lbl_is_auto(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)state()->auto_brightness;
    IPC_OUT(c, v);
}
static void lbl_get_ambient(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t over = (uint8_t)state()->ambient_over_limit;
    float lux = state()->ambient_lux;
    IPC_OUT(c, over);
    IPC_OUT(c, lux);
}
static void lbl_set_vr(IpcObject *s, IpcCall *c) {
    (void)s;
    state()->vr_brightness = lbl_clamp(IPC_IN(c, float, 0));
}
static void lbl_get_vr(IpcObject *s, IpcCall *c) {
    (void)s;
    float v = state()->vr_brightness;
    IPC_OUT(c, v);
}
static void lbl_set_ambient_value(IpcObject *s, IpcCall *c) { /* (u8 over limit, float lux) */
    (void)s;
    lbl_set_ambient(state(), IPC_IN(c, uint8_t, 0), IPC_IN(c, float, 4));
}
static void lbl_set_delay_level(IpcObject *s, IpcCall *c) { /* (u32 direction, float level) */
    (void)s;
    uint32_t dir = IPC_IN(c, uint32_t, 0);
    if (dir > 1) {
        c->result = HZ_RESULT(HZ_MOD_LBL, 2); /* [unverified] invalid direction */
        return;
    }
    state()->reflection_delay[dir] = lbl_clamp(IPC_IN(c, float, 4));
}
static void lbl_get_delay_level(IpcObject *s, IpcCall *c) { /* (u32 direction) */
    (void)s;
    uint32_t dir = IPC_IN(c, uint32_t, 0);
    if (dir > 1) {
        c->result = HZ_RESULT(HZ_MOD_LBL, 2);
        return;
    }
    float v = state()->reflection_delay[dir];
    IPC_OUT(c, v);
}
static void lbl_apply_vr_current(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    lbl_apply_vr(state());
}

static const IpcCmd lbl_cmds[] = {
    {0, "SaveCurrentSetting", lbl_save_current, IPC_IMPL},
    {1, "LoadCurrentSetting", lbl_load_current, IPC_IMPL},
    {2, "SetCurrentBrightnessSetting", lbl_set_current, IPC_IMPL},
    {3, "GetCurrentBrightnessSetting", lbl_get_current, IPC_IMPL},
    {4, "ApplyCurrentBrightnessSettingToBacklight", lbl_apply_current, IPC_IMPL},
    {5, "GetBrightnessSettingAppliedToBacklight", lbl_get_applied, IPC_IMPL},
    {6, "SwitchBacklightOn", lbl_switch_on, IPC_IMPL},
    {7, "SwitchBacklightOff", lbl_switch_off, IPC_IMPL},
    {8, "GetBacklightSwitchStatus", lbl_get_switch_status, IPC_IMPL},
    {9, "EnableDimming", lbl_enable_dimming, IPC_IMPL},
    {10, "DisableDimming", lbl_disable_dimming, IPC_IMPL},
    {11, "IsDimmingEnabled", lbl_is_dimming, IPC_IMPL},
    {12, "EnableAutoBrightnessControl", lbl_enable_auto, IPC_IMPL},
    {13, "DisableAutoBrightnessControl", lbl_disable_auto, IPC_IMPL},
    {14, "IsAutoBrightnessControlEnabled", lbl_is_auto, IPC_IMPL},
    {15, "SetAmbientLightSensorValue", lbl_set_ambient_value, IPC_IMPL},
    {16, "GetAmbientLightSensorValue", lbl_get_ambient, IPC_IMPL},
    {17, "SetBrightnessReflectionDelayLevel", lbl_set_delay_level, IPC_IMPL},
    {18, "GetBrightnessReflectionDelayLevel", lbl_get_delay_level, IPC_IMPL},
    {19, "SetCurrentBrightnessMapping", svc_noop,
     IPC_IMPL}, /* the curve shapes a real panel's auto-brightness; the host backlight is not driven: documented no-op */
    {21, "SetCurrentAmbientLightSensorMapping", svc_noop, IPC_IMPL}, /* same: sensor curve, no sensor on the host */
    {23, "IsAmbientLightSensorAvailable", svc_ret_u8_0, IPC_IMPL},   /* the host exposes no sensor */
    {24, "SetCurrentBrightnessSettingForVrMode", lbl_set_vr, IPC_IMPL},
    {25, "GetCurrentBrightnessSettingForVrMode", lbl_get_vr, IPC_IMPL},
    {26, "ApplyCurrentBrightnessSettingToBacklightForVrMode", lbl_apply_vr_current, IPC_IMPL},
};
static IpcObject *lbl_open(void) { return IPC_OBJECT("lbl", lbl_cmds, NULL, NULL); }
void service_register_lbl(void) { ipc_register_service("lbl", lbl_open); }
