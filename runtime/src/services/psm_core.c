#include "psm_core.h"
#include <stdlib.h>
#include <string.h>

void psm_state_init(PsmState *s, int docked, const char *env) {
    memset(s, 0, sizeof *s);
    s->percent = 100; s->ac_online = docked ? 1 : 0; s->charging_enabled = 1;
    if (!env || !*env) return;
    char *end = NULL; long v = strtol(env, &end, 10);
    if (end == env) return;                                       /* not a number: keep the defaults */
    s->percent = v < 0 ? 0 : v > 100 ? 100 : (uint32_t)v;
    if (end && *end == ',') {
        if (!strcmp(end + 1, "ac")) s->ac_online = 1;
        else if (!strcmp(end + 1, "battery")) s->ac_online = 0;
    }
}

uint32_t psm_charger_type(const PsmState *s) {
    if (!s->ac_online) return PSM_CHARGER_NONE;
    return PSM_CHARGER_ENOUGH_POWER;                              /* the Mac side is never "low power" */
}

uint32_t psm_voltage_state(uint32_t percent) {
    if (percent <= 2) return PSM_VOLTAGE_NEEDS_SHUTDOWN;
    if (percent <= 5) return PSM_VOLTAGE_NEEDS_SLEEP;
    if (percent <= 15) return PSM_VOLTAGE_NO_BOOST;
    return PSM_VOLTAGE_NORMAL;
}

int psm_enough_power_supplied(const PsmState *s) { return s->ac_online || s->enough_power_emulation; }
