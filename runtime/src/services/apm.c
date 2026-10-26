/* apm: performance manager. The host schedules CPU/GPU itself; the guest's requests are accepted and remembered. */
#include "../ipc.h"
#include "apm_core.h"
#include "hz_result.h"
#include <stdio.h>

static uint32_t performance_config[2];
static uint8_t cpu_overclock;
static uint32_t perf_mode_override = 2; /* 2: follow g_cfg.op_mode_handheld */

static void session_set_config(IpcObject *self, IpcCall *c) {
    (void)self;
    uint32_t mode = IPC_IN(c, uint32_t, 0), config = IPC_IN(c, uint32_t, 4);
    if (mode > 1) {
        c->result = HZ_RESULT(HZ_MOD_APM, 2); /* [unverified] invalid performance mode */
        return;
    }
    /* Every configuration id is accepted in both modes: the docked/handheld split of the [unverified] table is not checked against a
     * console, and Link's Awakening sets an id the table calls docked-only in handheld mode, then stops on a refusal (0x67f). */
    performance_config[mode] = config;
    ApmClocks k;
    apm_clocks_for(config, &k);
    LOGV("apm", "SetPerformanceConfiguration mode=%u config=%#x (cpu %u gpu %u emc %u kHz%s)", mode, config, k.cpu_khz, k.gpu_khz, k.emc_khz,
         mode == 0 && apm_config_docked_only(config) ? ", a docked id in the table" : "");
}
static void session_get_config(IpcObject *self, IpcCall *c) {
    (void)self;
    uint32_t mode = IPC_IN(c, uint32_t, 0), v = mode < 2 ? performance_config[mode] : 0;
    if (mode > 1) {
        c->result = HZ_RESULT(HZ_MOD_APM, 2);
        return;
    }
    IPC_OUT(c, v);
}
static void session_set_cpu_overclock(IpcObject *self, IpcCall *c) {
    (void)self;
    cpu_overclock = IPC_IN(c, uint8_t, 0) ? 1 : 0; /* remembered and reported back; the host never raises its own clock */
}
static const IpcCmd session_cmds[] = {
    {0, "SetPerformanceConfiguration", session_set_config, IPC_IMPL},
    {1, "GetPerformanceConfiguration", session_get_config, IPC_IMPL},
    {2, "SetCpuOverclockEnabled", session_set_cpu_overclock, IPC_IMPL},
};
static void apm_open_session(IpcObject *self, IpcCall *c) {
    (void)self;
    ipc_out_object(c, IPC_OBJECT("apm::ISession", session_cmds, NULL, NULL));
}
static void apm_get_performance_mode(IpcObject *self, IpcCall *c) {
    (void)self;
    uint32_t m = perf_mode_override < 2 ? perf_mode_override : (g_cfg.op_mode_handheld ? 0 : 1);
    IPC_OUT(c, m);
}
static void apm_is_cpu_overclock(IpcObject *self, IpcCall *c) {
    (void)self;
    uint8_t v = cpu_overclock;
    IPC_OUT(c, v);
}
static const IpcCmd apm_cmds[] = {
    {0, "OpenSession", apm_open_session, IPC_IMPL},
    {1, "GetPerformanceMode", apm_get_performance_mode, IPC_IMPL},
    {6, "IsCpuOverclockEnabled", apm_is_cpu_overclock, IPC_IMPL},
};
static IpcObject *apm_open(void) { return IPC_OBJECT("apm", apm_cmds, NULL, NULL); }
void service_register_apm(void) { ipc_register_service("apm", apm_open); }
