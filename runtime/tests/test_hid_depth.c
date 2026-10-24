/* macOS-only unit tests: hid IPC handlers that are backed by hid_core (fusion, zero drift, LED pattern, hold type, vibration permission, home
 * protection, styles). hid.c is included to reach its static handlers, as test_hid_service.c does. NOT RUN on Linux: needs Mach and the input library. */
#include "../src/services/hid.c"
#include "check.h"

static void call(IpcCmdFn fn, const void *in, uint32_t n, IpcCall *c) { memset(c, 0, sizeof *c); c->in = in; c->in_size = n; fn(NULL, c); }

int main(void) {
    char dir[] = "/tmp/napple-hiddepth-XXXXXX"; mkdtemp(dir);
    g_cfg.headless = 1; g_cfg.save_root = dir; snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "0100ABCD12345678");
    IpcCall c;
    uint8_t in[16] = {0}; uint32_t h = 0x55; memcpy(in, &h, 4);
    call(hid_fusion_enabled, in, sizeof in, &c); CHECK(c.out_size == 1 && c.out[0] == 1, "fusion enabled by default");
    uint8_t en[16] = {0}; en[0] = 0; memcpy(en + 4, &h, 4); call(hid_enable_fusion, en, sizeof en, &c);
    call(hid_fusion_enabled, in, sizeof in, &c); CHECK(c.out[0] == 0, "EnableSixAxisSensorFusion(false) is read back");
    uint8_t zd[16] = {0}; memcpy(zd, &h, 4); uint32_t mode = 2; memcpy(zd + 4, &mode, 4); call(hid_set_zero_drift, zd, sizeof zd, &c);
    call(hid_get_zero_drift, in, sizeof in, &c); uint32_t got; memcpy(&got, c.out, 4); CHECK(got == 2, "gyroscope zero-drift mode is read back");
    mode = 9; memcpy(zd + 4, &mode, 4); call(hid_set_zero_drift, zd, sizeof zd, &c); CHECK(c.result != 0, "invalid zero-drift mode refused");
    call(hid_reset_zero_drift, in, sizeof in, &c); call(hid_get_zero_drift, in, sizeof in, &c); memcpy(&got, c.out, 4); CHECK(got == 1, "reset to Standard");
    uint32_t npad = 2; call(hid_get_player_led_pattern, &npad, 4, &c); uint64_t led; memcpy(&led, c.out, 8); CHECK(led == 0x7, "player 3 LED pattern");
    uint64_t hold[2] = {0x1000, 1}; call(hid_set_joy_hold, hold, sizeof hold, &c); call(hid_get_joy_hold, hold, 8, &c); int64_t hv; memcpy(&hv, c.out, 8); CHECK(hv == 1, "joy hold type {aruid, value} order");
    uint64_t hold2[2] = {0, 0x1000}; call(hid_set_joy_hold, hold2, sizeof hold2, &c); call(hid_get_joy_hold, hold, 8, &c); memcpy(&hv, c.out, 8); CHECK(hv == 0, "joy hold type {value, aruid} order");
    uint8_t pv = 0; call(hid_permit_vibration, &pv, 1, &c); call(hid_is_vibration_permitted, NULL, 0, &c); CHECK(c.out[0] == 0, "PermitVibration(false) is read back");
    pv = 1; call(hid_permit_vibration, &pv, 1, &c); call(hid_is_vibration_permitted, NULL, 0, &c); CHECK(c.out[0] == 1, "PermitVibration(true)");
    uint8_t hp[16] = {1}; uint32_t id = 0x20; memcpy(hp + 4, &id, 4); call(hid_home_protect_set, hp, sizeof hp, &c); call(hid_home_protect_get, &id, 4, &c); CHECK(c.out[0] == 1, "home button protection per npad id");
    uint64_t cm = 1; call(hid_set_comm_mode, &cm, 8, &c); call(hid_get_comm_mode, NULL, 0, &c); memcpy(&cm, c.out, 8); CHECK(cm == 1, "communication mode round trip");
    call(hid_get_styles, NULL, 0, &c); memcpy(&got, c.out, 4); CHECK(got == 0x1f, "styles default before any Set");
    call(hid_fusion_enabled, in, 2, &c); CHECK(c.result == 0, "short request does not fail the runtime");
    char cmd[200]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)system(cmd);
    printf("{\"test\":\"hid_depth\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
