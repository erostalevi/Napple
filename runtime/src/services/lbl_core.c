#include "lbl_core.h"

#define LBL_DEFAULT 0.5f

void lbl_state_init(LblState *s) {
    s->brightness = s->applied = s->vr_brightness = s->saved = LBL_DEFAULT;
    s->reflection_delay[0] = s->reflection_delay[1] = 0.0f;
    s->switch_status = LBL_SWITCH_ENABLED;
    s->dimming = 1;
    s->auto_brightness = 0;
    s->ambient_lux = 0.0f;
    s->ambient_over_limit = 0;
}
float lbl_clamp(float v) {
    if (v != v) return LBL_DEFAULT;
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}
void lbl_set_brightness(LblState *s, float v) { s->brightness = lbl_clamp(v); }
void lbl_apply(LblState *s) { s->applied = s->brightness; }
void lbl_switch(LblState *s, int on) { s->switch_status = on ? LBL_SWITCH_ENABLED : LBL_SWITCH_DISABLED; }
void lbl_save(LblState *s) { s->saved = s->brightness; }
void lbl_load(LblState *s) { s->brightness = s->saved; }
void lbl_set_ambient(LblState *s, int over_limit, float lux) {
    s->ambient_over_limit = over_limit != 0;
    s->ambient_lux = (lux != lux || lux < 0.0f) ? 0.0f : lux;
}
void lbl_apply_vr(LblState *s) { s->applied = s->vr_brightness; }
