/* psm: power supply manager. Battery percentage, charger type and voltage state come from psm_core (a configured host power situation);
 * the session object hands out state-change events that never fire (the situation is fixed for the life of the process). */
#include "svc_common.h"
#include "psm_core.h"
#include <stdlib.h>

static PsmState psm; static int psm_ready;
static PsmState *state(void) {
    if (!psm_ready) { psm_state_init(&psm, !g_cfg.op_mode_handheld, getenv("NAPPLE_BATTERY")); psm_ready = 1; }
    return &psm;
}

static void psm_get_percentage(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = state()->percent; IPC_OUT(c, v); }
static void psm_get_charger_type(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = psm_charger_type(state()); IPC_OUT(c, v); }
static void psm_enable_charging(IpcObject *s, IpcCall *c) { (void)s; (void)c; state()->charging_enabled = 1; }
static void psm_disable_charging(IpcObject *s, IpcCall *c) { (void)s; (void)c; state()->charging_enabled = 0; }
static void psm_is_charging_enabled(IpcObject *s, IpcCall *c) { (void)s; uint8_t v = (uint8_t)state()->charging_enabled; IPC_OUT(c, v); }
static void psm_enable_emulation(IpcObject *s, IpcCall *c) { (void)s; (void)c; state()->enough_power_emulation = 1; }
static void psm_disable_emulation(IpcObject *s, IpcCall *c) { (void)s; (void)c; state()->enough_power_emulation = 0; }
static void psm_enable_fast(IpcObject *s, IpcCall *c) { (void)s; (void)c; state()->fast_charging = 1; }
static void psm_disable_fast(IpcObject *s, IpcCall *c) { (void)s; (void)c; state()->fast_charging = 0; }
static void psm_get_voltage_state(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = psm_voltage_state(state()->percent); IPC_OUT(c, v); }
static void psm_get_raw_percentage(IpcObject *s, IpcCall *c) { (void)s; double v = state()->percent; IPC_OUT(c, v); }
static void psm_is_enough_power(IpcObject *s, IpcCall *c) { (void)s; uint8_t v = (uint8_t)psm_enough_power_supplied(state()); IPC_OUT(c, v); }
static void psm_get_age_percentage(IpcObject *s, IpcCall *c) { (void)s; double v = 100.0; IPC_OUT(c, v); }   /* a new battery */

static const IpcCmd session_cmds[] = {
    {0, "BindStateChangeEvent", svc_ret_event, IPC_STUB}, {1, "UnbindStateChangeEvent", svc_noop, IPC_STUB},
    {2, "SetChargerTypeChangeEventEnabled", svc_noop, IPC_STUB}, {3, "SetPowerSupplyChangeEventEnabled", svc_noop, IPC_STUB},
    {4, "SetBatteryVoltageStateChangeEventEnabled", svc_noop, IPC_STUB},
};
static void psm_open_session(IpcObject *s, IpcCall *c) { (void)s; ipc_out_object(c, IPC_OBJECT("IPsmSession", session_cmds, NULL, NULL)); }
static const IpcCmd psm_cmds[] = {
    {0, "GetBatteryChargePercentage", psm_get_percentage, IPC_IMPL}, {1, "GetChargerType", psm_get_charger_type, IPC_IMPL},
    {2, "EnableBatteryCharging", psm_enable_charging, IPC_IMPL}, {3, "DisableBatteryCharging", psm_disable_charging, IPC_IMPL},
    {4, "IsBatteryChargingEnabled", psm_is_charging_enabled, IPC_IMPL},
    {5, "AcquireControllerPowerSupply", svc_noop, IPC_STUB}, {6, "ReleaseControllerPowerSupply", svc_noop, IPC_STUB},
    {7, "OpenSession", psm_open_session, IPC_IMPL},
    {8, "EnableEnoughPowerChargeEmulation", psm_enable_emulation, IPC_IMPL}, {9, "DisableEnoughPowerChargeEmulation", psm_disable_emulation, IPC_IMPL},
    {10, "EnableFastBatteryCharging", psm_enable_fast, IPC_IMPL}, {11, "DisableFastBatteryCharging", psm_disable_fast, IPC_IMPL},
    {12, "GetBatteryVoltageState", psm_get_voltage_state, IPC_IMPL}, {13, "GetRawBatteryChargePercentage", psm_get_raw_percentage, IPC_IMPL},
    {14, "IsEnoughPowerSupplied", psm_is_enough_power, IPC_IMPL}, {15, "GetBatteryAgePercentage", psm_get_age_percentage, IPC_STUB},
    {16, "GetBatteryChargeInfoEvent", svc_ret_event, IPC_STUB},
};
static IpcObject *psm_open(void) { return IPC_OBJECT("psm", psm_cmds, NULL, NULL); }
void service_register_psm(void) { ipc_register_service("psm", psm_open); }
