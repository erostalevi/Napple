/* Unit tests: apm performance configuration (session) and the clock table. */
#include "../src/services/apm.c"
#include "svc_test.h"

int main(void) {
    ApmClocks k;
    CHECK(apm_clocks_for(0x00010000, &k) && k.cpu_khz == 1020000, "known id");
    CHECK(!apm_clocks_for(0x12345678, &k) && k.cpu_khz == 0, "unknown id zeroes the output");
    CHECK(apm_config_docked_only(0x00020003) && !apm_config_docked_only(0x00010000), "table marks docked ids (documentation only)");
    CHECK(!apm_config_docked_only(0x77777777), "unknown id is not marked docked");

    IpcObject *o = apm_open(); IpcCall c;
    g_cfg.op_mode_handheld = 1; svc_call(o, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "GetPerformanceMode: handheld");
    g_cfg.op_mode_handheld = 0; svc_call(o, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1, "GetPerformanceMode: docked");
    svc_call(o, 0, NULL, 0, &c); IpcObject *se = c.out_objects[0];
    uint32_t in[2] = {1, 0x00020003}; svc_call(se, 0, in, 8, &c); CHECK(c.result == 0, "set docked config");
    uint32_t m = 1; svc_call(se, 1, &m, 4, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0x00020003, "read it back");
    m = 0; svc_call(se, 1, &m, 4, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "handheld config separate");
    in[0] = 0; svc_call(se, 0, in, 8, &c); CHECK(c.result == 0, "a docked id in handheld mode is accepted");
    /* Regression: Link's Awakening (01006BB00C6F0000) opens a session and sets mode 0 = 0x20004, then mode 1 = 0x10001; a refusal of the
     * first call made it stop with SetTerminateResult 0x67f. */
    svc_call(o, 0, NULL, 0, &c); IpcObject *las = c.out_objects[0];
    uint32_t las_h[2] = {0, 0x00020004}, las_d[2] = {1, 0x00010001};
    svc_call(las, 0, las_h, 8, &c); CHECK(c.result == 0, "LAS: SetPerformanceConfiguration(handheld, 0x20004) succeeds");
    svc_call(las, 0, las_d, 8, &c); CHECK(c.result == 0, "LAS: SetPerformanceConfiguration(docked, 0x10001) succeeds");
    m = 0; svc_call(las, 1, &m, 4, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0x00020004, "LAS: handheld config reads back");
    m = 1; svc_call(las, 1, &m, 4, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0x00010001, "LAS: docked config reads back");
    in[0] = 5; svc_call(se, 0, in, 8, &c); CHECK(c.result != 0, "bad mode fails");
    m = 5; svc_call(se, 1, &m, 4, &c); CHECK(c.result != 0, "bad mode on get fails");
    uint8_t on = 1; svc_call(se, 2, &on, 1, &c); svc_call(o, 6, NULL, 0, &c); CHECK(c.out[0] == 1, "overclock flag reads back");
    on = 0; svc_call(se, 2, &on, 1, &c); svc_call(o, 6, NULL, 0, &c); CHECK(c.out[0] == 0, "overclock off");
    printf("{\"test\":\"svc_apm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
