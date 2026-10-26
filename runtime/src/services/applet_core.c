#include "applet_core.h"
#include <string.h>

void applet_state_init(AppletState *s) {
    memset(s, 0, sizeof *s);
    s->main_volume = s->library_volume = 1.0f;
    s->transparent_volume_rate = 0.0f;
    s->screenshot_permission = APPLET_SCREENSHOT_INHERIT;
    s->restart_message = 1;
    s->crash_report_enabled = 0;
    s->copyright_visible = 0;
}
float applet_clamp_volume(float v, float fallback) {
    if (v != v) return fallback;
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}
int applet_valid_screenshot_permission(uint32_t v) { return v <= APPLET_SCREENSHOT_DISABLE; }
int applet_block_begin(unsigned *d) {
    (*d)++;
    return 1;
}
int applet_block_end(unsigned *d) {
    if (*d) (*d)--;
    return *d > 0;
}
int applet_screenshot_allowed(const AppletState *s) { return s->screenshot_permission != APPLET_SCREENSHOT_DISABLE; }
