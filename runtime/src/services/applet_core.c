#include "applet_core.h"
#include <stdlib.h>
#include <string.h>

void applet_state_init(AppletState *s) {
    memset(s, 0, sizeof *s);
    s->main_volume = s->library_volume = 1.0f;
    s->transparent_volume_rate = 0.0f;
    s->screenshot_permission = APPLET_SCREENSHOT_INHERIT;
    s->restart_message = 1;
    s->crash_report_enabled = 0;
    s->copyright_visible = 0;
    s->window_visible = 1;
    s->foreground_rights = 1;
    s->platform_region = 1;
    s->illuminance = 100.0f;
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

int applet_sleep_lock_acquire(AppletState *s) {
    s->sleep_lock_held = 1;
    s->sleep_lock_transient = 0;
    s->sleep_lock_acquires++;
    return 1;
}
void applet_sleep_lock_release(AppletState *s) {
    s->sleep_lock_held = 0;
    s->sleep_lock_transient = 0;
}
void applet_sleep_lock_release_transiently(AppletState *s) {
    if (s->sleep_lock_held) s->sleep_lock_transient = 1;
}
int applet_fatal_enter(AppletState *s) {
    s->fatal_section++;
    return 1;
}
int applet_fatal_leave(AppletState *s) {
    if (s->fatal_section) s->fatal_section--;
    return s->fatal_section > 0;
}
void applet_illuminance_set(AppletState *s, int available, float lux) {
    s->illuminance_available = available != 0;
    if (lux != lux || lux < 0.0f) lux = 0.0f;
    s->illuminance = lux;
}
float applet_illuminance_get(const AppletState *s, int *over_limit) {
    float v = s->illuminance;
    int over = v > 100000.0f;
    if (over) v = 100000.0f;
    if (over_limit) *over_limit = over;
    return v;
}

static AppletFrameSource g_cap_source;
static uint8_t *g_cap[APPLET_CAP_SLOTS];
static int g_cap_has[APPLET_CAP_SLOTS];
static int g_app_buffer_held;
void applet_capture_set_source(AppletFrameSource src) { g_cap_source = src; }
static uint8_t *cap_slot(unsigned slot) {
    if (slot >= APPLET_CAP_SLOTS) return NULL;
    if (!g_cap[slot]) g_cap[slot] = calloc(1, APPLET_CAP_BYTES);
    return g_cap[slot];
}
static void cap_black(uint8_t *p) {
    for (size_t i = 0; i < APPLET_CAP_BYTES; i += 4) {
        p[i] = p[i + 1] = p[i + 2] = 0;
        p[i + 3] = 0xff;
    }
}
int applet_capture_update(unsigned slot) {
    uint8_t *p = cap_slot(slot);
    if (!p) return 0;
    if (g_cap_source && g_cap_source(p, APPLET_CAP_W, APPLET_CAP_H)) {
        g_cap_has[slot] = 1;
        return 1;
    }
    cap_black(p);
    g_cap_has[slot] = 0;
    return 0;
}
size_t applet_capture_read(unsigned slot, uint8_t *out, size_t cap, int *has_frame) {
    uint8_t *p = cap_slot(slot);
    if (!p) return 0;
    if (!g_cap_has[slot] && !p[3]) cap_black(p); /* never filled: opaque black */
    size_t n = cap < APPLET_CAP_BYTES ? cap : APPLET_CAP_BYTES;
    memcpy(out, p, n);
    if (has_frame) *has_frame = g_cap_has[slot];
    return n;
}
int applet_capture_copy(unsigned dst, unsigned src) {
    uint8_t *d = cap_slot(dst), *s = cap_slot(src);
    if (!d || !s) return 0;
    if (!g_cap_has[src] && !s[3]) cap_black(s);
    memcpy(d, s, APPLET_CAP_BYTES);
    g_cap_has[dst] = g_cap_has[src];
    return 1;
}
int applet_capture_has_frame(unsigned slot) { return slot < APPLET_CAP_SLOTS && g_cap_has[slot]; }
int applet_capture_acquire_app_buffer(void) {
    if (g_app_buffer_held) return 0;
    g_app_buffer_held = 1;
    return 1;
}
void applet_capture_release_app_buffer(void) { g_app_buffer_held = 0; }
void applet_capture_reset(void) {
    for (unsigned i = 0; i < APPLET_CAP_SLOTS; i++) {
        free(g_cap[i]);
        g_cap[i] = NULL;
        g_cap_has[i] = 0;
    }
    g_app_buffer_held = 0;
    g_cap_source = NULL;
}
