/* lbl core: backlight (portable). The host does not drive the Mac's backlight; the state only has to be self-consistent. */
#pragma once
#include <stdint.h>

enum { LBL_SWITCH_DISABLED = 0, LBL_SWITCH_ENABLED = 1 }; /* LblBacklightSwitchStatus; the transitional states are not modelled */
typedef struct {
    float brightness; /* current setting, 0..1 */
    float applied;    /* brightness applied to the backlight */
    float vr_brightness;
    uint32_t switch_status;
    int dimming, auto_brightness;
    float ambient_lux;
    int ambient_over_limit;
} LblState;

void lbl_state_init(LblState *s);
float lbl_clamp(float v); /* NaN -> default, clamped to 0..1 */
void lbl_set_brightness(LblState *s, float v);
void lbl_apply(LblState *s); /* ApplyCurrentBrightnessSettingToBacklight */
void lbl_switch(LblState *s, int on);
