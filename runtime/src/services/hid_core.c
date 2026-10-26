#include "hid_core.h"
#include <string.h>

void hidc_init(HidCore *h) {
    memset(h, 0, sizeof *h);
    h->vibration_permitted = 1;
    h->comm_mode = 3; /* HidNpadCommunicationMode_Default */
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
