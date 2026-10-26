/* psm: power supply manager. Battery percentage, charger type and voltage state come from psm_core (a configured host power situation);
 * the session object hands out state-change events that never fire (the situation is fixed for the life of the process). */
#include "svc_common.h"
#include "psm_core.h"
#include "hz_result.h"
#include <stdlib.h>

static PsmState psm;
static int psm_ready;
static PsmState *state(void) {
    if (!psm_ready) {
        psm_state_init(&psm, !g_cfg.op_mode_handheld, getenv("NAPPLE_BATTERY"));
        psm_ready = 1;
    }
    return &psm;
}

/* State-change events. A session (IPsmSession) binds one event and chooses which changes signal it; GetBatteryChargeInfoEvent hands out an
 * event that fires on any change. The writable ends are kept here so a change (a toggled emulation, or the host reporting a new power
 * situation through psm_host_power_changed) signals them. */
#define PSM_MAX_LISTENERS 32
typedef struct {
    uint32_t writable;
    unsigned mask; /* PSM_EVT_* bits this listener wants */
} PsmListener;
enum { PSM_EVT_CHARGER = 1, PSM_EVT_POWER_SUPPLY = 2, PSM_EVT_VOLTAGE = 4, PSM_EVT_ANY = 7 };
static PsmListener listeners[PSM_MAX_LISTENERS];
static unsigned nlisteners;
static PsmListener *listener_new(uint32_t *readable, unsigned mask) {
    if (nlisteners == PSM_MAX_LISTENERS) { /* a guest that binds endlessly: recycle the oldest slot */
        memmove(listeners, listeners + 1, (PSM_MAX_LISTENERS - 1) * sizeof listeners[0]);
        nlisteners--;
    }
    PsmListener *l = &listeners[nlisteners++];
    l->writable = event_create(readable);
    l->mask = mask;
    return l;
}
static void psm_fire(unsigned what) {
    for (unsigned i = 0; i < nlisteners; i++)
        if (listeners[i].mask & what) event_signal_handle(listeners[i].writable);
}
/* The host's power situation changed (a charger was plugged in, the battery level moved): update the state and signal the listeners. */
void psm_host_power_changed(int ac_online, uint32_t percent) {
    PsmState *st = state();
    unsigned what = 0;
    if (percent > 100) percent = 100;
    if ((st->ac_online != 0) != (ac_online != 0)) what |= PSM_EVT_CHARGER | PSM_EVT_POWER_SUPPLY;
    if (psm_voltage_state(st->percent) != psm_voltage_state(percent)) what |= PSM_EVT_VOLTAGE;
    st->ac_online = ac_online != 0;
    st->percent = percent;
    if (what) psm_fire(what);
}

static void psm_get_percentage(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = state()->percent;
    IPC_OUT(c, v);
}
static void psm_get_charger_type(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = psm_charger_type(state());
    IPC_OUT(c, v);
}
static void psm_enable_charging(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    state()->charging_enabled = 1;
}
static void psm_disable_charging(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    state()->charging_enabled = 0;
}
static void psm_is_charging_enabled(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)state()->charging_enabled;
    IPC_OUT(c, v);
}
static void psm_enable_emulation(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    PsmState *st = state();
    int before = psm_enough_power_supplied(st);
    st->enough_power_emulation = 1;
    if (before != psm_enough_power_supplied(st)) psm_fire(PSM_EVT_POWER_SUPPLY);
}
static void psm_disable_emulation(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    PsmState *st = state();
    int before = psm_enough_power_supplied(st);
    st->enough_power_emulation = 0;
    if (before != psm_enough_power_supplied(st)) psm_fire(PSM_EVT_POWER_SUPPLY);
}
static void psm_enable_fast(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    state()->fast_charging = 1;
}
static void psm_disable_fast(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    state()->fast_charging = 0;
}
static void psm_get_voltage_state(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = psm_voltage_state(state()->percent);
    IPC_OUT(c, v);
}
static void psm_get_raw_percentage(IpcObject *s, IpcCall *c) {
    (void)s;
    double v = state()->percent;
    IPC_OUT(c, v);
}
static void psm_is_enough_power(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)psm_enough_power_supplied(state());
    IPC_OUT(c, v);
}
/* The battery of a host that is not a console never ages: 100 percent of the design capacity. */
static void psm_get_age_percentage(IpcObject *s, IpcCall *c) {
    (void)s;
    double v = 100.0;
    IPC_OUT(c, v);
}

/* IPsmSession. BindStateChangeEvent returns the session's event (created on the first bind; later binds hand out the same state); the
 * three Set*ChangeEventEnabled commands choose which changes signal it, all off at first (as on a console). */
typedef struct {
    PsmListener *listener;
    uint32_t readable;
    int bound;
} PsmSessionState;
static void sess_bind(IpcObject *s, IpcCall *c) {
    PsmSessionState *st = s->state;
    if (!st->bound) {
        st->listener = listener_new(&st->readable, 0);
        st->bound = 1;
    }
    ipc_out_copy_handle(c, st->readable);
}
static void sess_unbind(IpcObject *s, IpcCall *c) {
    (void)c;
    PsmSessionState *st = s->state;
    if (st->bound) st->listener->mask = 0;
}
static void sess_set_enabled(IpcObject *s, IpcCall *c, unsigned bit) {
    PsmSessionState *st = s->state;
    uint8_t on = IPC_IN(c, uint8_t, 0);
    if (!st->bound) { /* not bound yet: remember nothing, the guest has no event to wait on */
        return;
    }
    if (on) st->listener->mask |= bit;
    else st->listener->mask &= ~bit;
}
static void sess_set_charger(IpcObject *s, IpcCall *c) { sess_set_enabled(s, c, PSM_EVT_CHARGER); }
static void sess_set_power(IpcObject *s, IpcCall *c) { sess_set_enabled(s, c, PSM_EVT_POWER_SUPPLY); }
static void sess_set_voltage(IpcObject *s, IpcCall *c) { sess_set_enabled(s, c, PSM_EVT_VOLTAGE); }
static const IpcCmd session_cmds[] = {
    {0, "BindStateChangeEvent", sess_bind, IPC_IMPL},
    {1, "UnbindStateChangeEvent", sess_unbind, IPC_IMPL},
    {2, "SetChargerTypeChangeEventEnabled", sess_set_charger, IPC_IMPL},
    {3, "SetPowerSupplyChangeEventEnabled", sess_set_power, IPC_IMPL},
    {4, "SetBatteryVoltageStateChangeEventEnabled", sess_set_voltage, IPC_IMPL},
};
static void sess_destroy(IpcObject *s) { free(s->state); }
static void psm_open_session(IpcObject *s, IpcCall *c) {
    (void)s;
    PsmSessionState *st = calloc(1, sizeof *st);
    ipc_out_object(c, IPC_OBJECT("IPsmSession", session_cmds, st, sess_destroy));
}
/* The controller power supply (USB charging of the pads) has no effect on the host; the acquire/release pair is counted so a guest that
 * releases more than it acquired gets the console's error, and the count is queryable by tests. */
static unsigned controller_supply;
static void psm_acquire_ctrl_supply(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    controller_supply++;
}
static void psm_release_ctrl_supply(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!controller_supply) {
        c->result = HZ_RESULT(HZ_MOD_PSM, 5); /* [unverified] not acquired */
        return;
    }
    controller_supply--;
}
static void psm_charge_info_event(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t r = 0;
    listener_new(&r, PSM_EVT_ANY);
    ipc_out_copy_handle(c, r);
}

static const IpcCmd psm_cmds[] = {
    {0, "GetBatteryChargePercentage", psm_get_percentage, IPC_IMPL},
    {1, "GetChargerType", psm_get_charger_type, IPC_IMPL},
    {2, "EnableBatteryCharging", psm_enable_charging, IPC_IMPL},
    {3, "DisableBatteryCharging", psm_disable_charging, IPC_IMPL},
    {4, "IsBatteryChargingEnabled", psm_is_charging_enabled, IPC_IMPL},
    {5, "AcquireControllerPowerSupply", psm_acquire_ctrl_supply, IPC_IMPL},
    {6, "ReleaseControllerPowerSupply", psm_release_ctrl_supply, IPC_IMPL},
    {7, "OpenSession", psm_open_session, IPC_IMPL},
    {8, "EnableEnoughPowerChargeEmulation", psm_enable_emulation, IPC_IMPL},
    {9, "DisableEnoughPowerChargeEmulation", psm_disable_emulation, IPC_IMPL},
    {10, "EnableFastBatteryCharging", psm_enable_fast, IPC_IMPL},
    {11, "DisableFastBatteryCharging", psm_disable_fast, IPC_IMPL},
    {12, "GetBatteryVoltageState", psm_get_voltage_state, IPC_IMPL},
    {13, "GetRawBatteryChargePercentage", psm_get_raw_percentage, IPC_IMPL},
    {14, "IsEnoughPowerSupplied", psm_is_enough_power, IPC_IMPL},
    {15, "GetBatteryAgePercentage", psm_get_age_percentage, IPC_IMPL},
    {16, "GetBatteryChargeInfoEvent", psm_charge_info_event, IPC_IMPL},
};
static IpcObject *psm_open(void) { return IPC_OBJECT("psm", psm_cmds, NULL, NULL); }
void service_register_psm(void) { ipc_register_service("psm", psm_open); }
