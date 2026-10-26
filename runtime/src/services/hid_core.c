#include "hid_core.h"
#include <string.h>

void hidc_init(HidCore *h) {
    memset(h, 0, sizeof *h);
    h->vibration_permitted = 1;
    h->comm_mode = 3; /* HidNpadCommunicationMode_Default */
    h->npad_active = 1;
    for (int i = 0; i < 10; i++) h->perm[i] = (uint8_t)i;
}
void hidc_reset_fusion(HidcSensor *s) {
    s->fusion_enabled = 1;
    s->gain = HIDC_DEFAULT_GAIN;
    s->reduction = HIDC_DEFAULT_REDUCTION;
}
HidcSensor *hidc_sensor(HidCore *h, uint32_t handle) {
    for (int i = 0; i < HIDC_MAX_SENSORS; i++)
        if (h->sensors[i].used && h->sensors[i].handle == handle) return &h->sensors[i];
    for (int i = 0; i < HIDC_MAX_SENSORS; i++)
        if (!h->sensors[i].used) {
            HidcSensor *s = &h->sensors[i];
            memset(s, 0, sizeof *s);
            s->used = 1;
            s->handle = handle;
            s->zero_drift = HIDC_ZERO_DRIFT_STANDARD;
            hidc_reset_fusion(s);
            return s;
        }
    return 0;
}
int hidc_set_zero_drift(HidcSensor *s, uint32_t mode) {
    if (mode > HIDC_ZERO_DRIFT_TIGHT) return -1;
    s->zero_drift = mode;
    return 0;
}

uint64_t hidc_led_pattern(uint32_t id) {
    static const uint8_t p[8] = {0x1, 0x3, 0x7, 0xF, 0x9, 0xA, 0xB, 0x6}; /* players 1..8 */
    return id < 8 ? p[id] : 0;
}
void hidc_home_protection_set(HidCore *h, uint32_t id, int on) {
    for (unsigned i = 0; i < h->nhome; i++)
        if (h->home_ids[i] == id) {
            h->home_on[i] = on;
            return;
        }
    if (h->nhome < HIDC_MAX_HOME_IDS) {
        h->home_ids[h->nhome] = id;
        h->home_on[h->nhome++] = on;
    }
}
int hidc_home_protection_get(const HidCore *h, uint32_t id) {
    for (unsigned i = 0; i < h->nhome; i++)
        if (h->home_ids[i] == id) return h->home_on[i];
    return 0;
}

int hidc_slot_from_id(uint32_t id) {
    if (id <= 7) return (int)id;
    if (id == 0x10) return 8;
    if (id == 0x20) return 9;
    return -1;
}
int hidc_disconnect(HidCore *h, uint32_t id) {
    int s = hidc_slot_from_id(id);
    if (s < 0) return -1;
    h->disconnected |= 1u << s;
    return 0;
}
void hidc_reconnect_all(HidCore *h) { h->disconnected = 0; }
int hidc_pad_source(const HidCore *h, int slot) { return slot >= 0 && slot < 10 ? h->perm[slot] : slot; }
int hidc_swap(HidCore *h, uint32_t a, uint32_t b) {
    if (a > 7 || b > 7) return -1;
    if (a == b) return -2;
    uint8_t t = h->perm[a];
    h->perm[a] = h->perm[b];
    h->perm[b] = t;
    return 0;
}
int hidc_merge(HidCore *h, uint32_t a, uint32_t b) {
    if (a > 7 || b > 7) return -1;
    if (a == b) return -2;
    h->dual_of[a] = (uint8_t)(b + 1);
    h->disconnected |= 1u << b;
    return 0;
}
uint32_t hidc_keyboard_lock_event(HidCore *h, uint32_t ev) {
    for (unsigned k = 0; k < 3; k++) {
        uint32_t bit = 1u << k;
        if (ev & (1u << (3 * k + 2))) h->kb_locks ^= bit;
        if (ev & (1u << (3 * k))) h->kb_locks |= bit;
        if (ev & (1u << (3 * k + 1))) h->kb_locks &= ~bit;
    }
    return h->kb_locks;
}
static int single_style(uint32_t style) { return style && !(style & (style - 1)); }
int hidc_capture_assign(HidCore *h, uint32_t style, int button) {
    if (!single_style(style)) return -1;
    for (unsigned i = 0; i < h->ncapture; i++)
        if (h->capture[i].style == style) {
            h->capture[i].button = button;
            return 0;
        }
    if (h->ncapture == HIDC_MAX_CAPTURE) return -1;
    h->capture[h->ncapture].style = style;
    h->capture[h->ncapture++].button = button;
    return 0;
}
int hidc_capture_clear(HidCore *h, uint32_t style) {
    if (!single_style(style)) return -1;
    for (unsigned i = 0; i < h->ncapture; i++)
        if (h->capture[i].style == style) h->capture[i].button = -1;
    return 0;
}
int hidc_capture_get(const HidCore *h, uint32_t style) {
    for (unsigned i = 0; i < h->ncapture; i++)
        if (h->capture[i].style == style) return h->capture[i].button;
    return -1;
}
int hidc_set_touch_resolution(HidCore *h, uint32_t w, uint32_t hgt) {
    if (!w || !hgt || w > 4096 || hgt > 4096) return -1;
    h->touch_w = w;
    h->touch_h = hgt;
    return 0;
}
int hidc_vib_handle_decode(uint32_t handle, uint32_t *npad_id, uint32_t *side) {
    uint32_t type = handle & 0xff, style = (handle >> 8) & 0xff, id = (handle >> 16) & 0xff, dev = (handle >> 24) & 0xff;
    if (dev > 1 || type > 7 || style > 8) return -1;
    if (style == 1) id = 0x20; /* handheld style index */
    if (hidc_slot_from_id(id) < 0) return -1;
    if (npad_id) *npad_id = id;
    if (side) *side = dev;
    return 0;
}
int hidc_vib_activate(HidCore *h, uint32_t handle) {
    if (hidc_vib_handle_decode(handle, 0, 0)) return -1;
    if (hidc_vib_is_active(h, handle)) return 0;
    if (h->nvib_active == HIDC_MAX_VIB_ACTIVE) return -2;
    h->vib_active[h->nvib_active++] = handle;
    return 0;
}
int hidc_vib_is_active(const HidCore *h, uint32_t handle) {
    for (unsigned i = 0; i < h->nvib_active; i++)
        if (h->vib_active[i] == handle) return 1;
    return 0;
}
