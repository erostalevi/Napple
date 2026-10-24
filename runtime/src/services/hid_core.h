/* hid core: per-session settings the hid service stores and returns (portable; no input library, no Mach). The shared-memory input state
 * itself is owned by input/; this is the part that used to be a constant: six-axis fusion and zero-drift mode per sensor handle, joy hold
 * type, handheld activation mode, home-button protection, vibration permission, communication mode, supported styles. */
#pragma once
#include <stdint.h>

#define HIDC_MAX_SENSORS 16
#define HIDC_MAX_HOME_IDS 16
typedef struct { uint32_t handle; int used, fusion_enabled; float gain, reduction; uint32_t zero_drift; } HidcSensor;
typedef struct {
    HidcSensor sensors[HIDC_MAX_SENSORS];
    uint32_t styles; int styles_set;
    int64_t joy_hold_type, handheld_mode;
    uint32_t home_ids[HIDC_MAX_HOME_IDS]; int home_on[HIDC_MAX_HOME_IDS]; unsigned nhome;
    int vibration_permitted; uint64_t comm_mode;
} HidCore;

enum { HIDC_ZERO_DRIFT_LOOSE = 0, HIDC_ZERO_DRIFT_STANDARD = 1, HIDC_ZERO_DRIFT_TIGHT = 2 };
#define HIDC_DEFAULT_GAIN 0.03f
#define HIDC_DEFAULT_REDUCTION 0.4f

void hidc_init(HidCore *h);
HidcSensor *hidc_sensor(HidCore *h, uint32_t handle);              /* created with defaults on first use; NULL if the table is full */
int hidc_set_zero_drift(HidcSensor *s, uint32_t mode);             /* 0, or -1 for a mode outside Loose..Tight */
void hidc_reset_fusion(HidcSensor *s);
uint64_t hidc_led_pattern(uint32_t npad_id);                       /* player LED bit pattern for an NpadIdType; 0 for handheld/other/unknown */
void hidc_home_protection_set(HidCore *h, uint32_t npad_id, int on);
int hidc_home_protection_get(const HidCore *h, uint32_t npad_id);
