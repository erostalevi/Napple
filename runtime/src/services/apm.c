/* apm: performance manager. The host schedules CPU/GPU itself; the guest's requests are accepted and remembered. */
#include "../ipc.h"
#include <stdio.h>

static uint32_t performance_config[2];

static void session_set_config(IpcObject *self, IpcCall *c) {
    (void)self;
    uint32_t mode = IPC_IN(c, uint32_t, 0), config = IPC_IN(c, uint32_t, 4);
    if (mode < 2) performance_config[mode] = config;
    LOGV("apm", "SetPerformanceConfiguration mode=%u config=%#x", mode, config);
}
static void session_get_config(IpcObject *self, IpcCall *c) {
    (void)self;
    uint32_t mode = IPC_IN(c, uint32_t, 0), v = mode < 2 ? performance_config[mode] : 0;
    IPC_OUT(c, v);
}
static void session_set_cpu_overclock(IpcObject *self, IpcCall *c) {
    (void)self;
    (void)c;
}
static const IpcCmd session_cmds[] = {
    {0, "SetPerformanceConfiguration", session_set_config, IPC_IMPL},
    {1, "GetPerformanceConfiguration", session_get_config, IPC_IMPL},
    {2, "SetCpuOverclockEnabled", session_set_cpu_overclock, IPC_STUB},
};
static void apm_open_session(IpcObject *self, IpcCall *c) {
    (void)self;
    ipc_out_object(c, IPC_OBJECT("apm::ISession", session_cmds, NULL, NULL));
}
static void apm_get_performance_mode(IpcObject *self, IpcCall *c) {
    (void)self;
    uint32_t m = g_cfg.op_mode_handheld ? 0 : 1;
    IPC_OUT(c, m);
}
static void apm_is_cpu_overclock(IpcObject *self, IpcCall *c) {
    (void)self;
    uint8_t v = 0;
    IPC_OUT(c, v);
}
static const IpcCmd apm_cmds[] = {
    {0, "OpenSession", apm_open_session, IPC_IMPL},
    {1, "GetPerformanceMode", apm_get_performance_mode, IPC_IMPL},
    {6, "IsCpuOverclockEnabled", apm_is_cpu_overclock, IPC_STUB},
};
static IpcObject *apm_open(void) { return IPC_OBJECT("apm", apm_cmds, NULL, NULL); }
void service_register_apm(void) { ipc_register_service("apm", apm_open); }
