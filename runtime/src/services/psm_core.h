/* psm core: battery and charger state (portable). The host reports a fixed, configurable power situation: docked = on AC at the given
 * charge, handheld = on battery. NAPPLE_BATTERY="NN" or "NN,ac" / "NN,battery" overrides percent and source. */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t percent;           /* 0..100 */
    int ac_online;              /* a charger is connected */
    int charging_enabled;       /* EnableBatteryCharging / DisableBatteryCharging */
    int enough_power_emulation; /* EnableEnoughPowerChargeEmulation */
    int fast_charging;
} PsmState;

enum { PSM_CHARGER_NONE = 0, PSM_CHARGER_ENOUGH_POWER = 1, PSM_CHARGER_LOW_POWER = 2, PSM_CHARGER_NOT_SUPPORTED = 3 };
enum { PSM_VOLTAGE_NEEDS_SHUTDOWN = 0, PSM_VOLTAGE_NEEDS_SLEEP = 1, PSM_VOLTAGE_NO_BOOST = 2, PSM_VOLTAGE_NORMAL = 3 };

void psm_state_init(PsmState *s, int docked, const char *env); /* env: NAPPLE_BATTERY value or NULL */
uint32_t psm_charger_type(const PsmState *s);
uint32_t psm_voltage_state(uint32_t percent); /* host-chosen thresholds, not Horizon's exact ones */
int psm_enough_power_supplied(const PsmState *s);
