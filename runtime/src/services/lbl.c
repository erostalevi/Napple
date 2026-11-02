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
/* am ICommonStateGetter::SetLcdBacklighOffEnabled: the application asks the system to switch the backlight off (1) or back on (0); the same switch state
 * lbl's own SwitchBacklightOn/Off keep and GetBacklightSwitchStatus reads. */
void lbl_app_backlight_off(int off) { lbl_switch(state(), !off); }
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
/* VR mode (Labo VR): 26 EnableVrMode applies the VR brightness, 27 DisableVrMode goes back to the normal one, 28 IsVrModeEnabled. Row 26 was
 * named ApplyCurrentBrightnessSettingToBacklightForVrMode before 2026-11-01 (switchbrew: EnableVrMode); its effect on the backlight is the same. */
static void lbl_enable_vr(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    state()->vr_mode = 1;
    lbl_apply_vr(state());
}
static void lbl_disable_vr(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    state()->vr_mode = 0;
    lbl_apply(state());
}
static void lbl_is_vr(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)state()->vr_mode;
    IPC_OUT(c, v);
}

/* 19 / 20 Set/GetCurrentBrightnessMapping and 21 / 22 Set/GetCurrentAmbientLightSensorMapping: the curves auto-brightness uses (0xC bytes each, three f32
 * per switchbrew, unverified). The host panel is not driven by them, but the controller keeps what it was given and hands it back (all zero until set). */
#define LBL_MAPPING_SIZE 0xCu
static uint8_t g_brightness_mapping[LBL_MAPPING_SIZE], g_als_mapping[LBL_MAPPING_SIZE];
static uint8_t *mapping_for(unsigned cmd) { return cmd <= 20 ? g_brightness_mapping : g_als_mapping; }
static void lbl_set_mapping(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t n = c->in_size < LBL_MAPPING_SIZE ? c->in_size : LBL_MAPPING_SIZE;
    if (n) memcpy(mapping_for(c->cmd), c->in, n);
}
static void lbl_get_mapping(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out(c, mapping_for(c->cmd), LBL_MAPPING_SIZE);
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
    {19, "SetCurrentBrightnessMapping", lbl_set_mapping, IPC_IMPL},
    {20, "GetCurrentBrightnessMapping", lbl_get_mapping, IPC_IMPL},
    {21, "SetCurrentAmbientLightSensorMapping", lbl_set_mapping, IPC_IMPL},
    {22, "GetCurrentAmbientLightSensorMapping", lbl_get_mapping, IPC_IMPL},
    {23, "IsAmbientLightSensorAvailable", svc_ret_u8_0, IPC_IMPL},   /* the host exposes no sensor */
    {24, "SetCurrentBrightnessSettingForVrMode", lbl_set_vr, IPC_IMPL},
    {25, "GetCurrentBrightnessSettingForVrMode", lbl_get_vr, IPC_IMPL},
    {26, "EnableVrMode", lbl_enable_vr, IPC_IMPL},
    {27, "DisableVrMode", lbl_disable_vr, IPC_IMPL},
    {28, "IsVrModeEnabled", lbl_is_vr, IPC_IMPL},
    {29, "IsAutoBrightnessControlSupported", svc_ret_u8_0, IPC_IMPL}, /* no ambient light sensor on the host (23 says the same) */
};
static IpcObject *lbl_open(void) { return IPC_OBJECT("lbl", lbl_cmds, NULL, NULL); }
void service_register_lbl(void) { ipc_register_service("lbl", lbl_open); }
