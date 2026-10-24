/* Unit tests: hid IPC handlers (vibration handle decoding, operation mode), without a guest. hid.c is included to reach its static handlers. */
#include "../src/services/hid.c"
#include "check.h"

static void call(IpcCmdFn fn, const void *in, uint32_t n, IpcCall *c) { memset(c, 0, sizeof *c); c->in = in; c->in_size = n; fn(NULL, c); }

int main(void) {
    char dir[] = "/tmp/napple-hidsvc-XXXXXX"; mkdtemp(dir);
    g_cfg.headless = 1; g_cfg.save_root = dir; snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "0100ABCD12345678");
    ensure_input();
    CHECK(g_input != NULL && g_shmem != NULL, "input backend created lazily");
    IpcCall c; napple_vibration got;

    /* SendVibrationValue: {handle u32, value 16 bytes, aruid u64}; handle bytes {type, style index, npad id, device} */
    struct { uint32_t h; napple_vibration v; uint64_t aruid; } __attribute__((packed)) req = {0x00000000u | (0u << 0) | (0u << 8) | (0u << 16) | (1u << 24), {0.5f, 160.f, 0.25f, 320.f}, 0x1000};
    call(hid_send_vibration, &req, sizeof req, &c);
    CHECK(c.result == 0 && napple_input_get_vibration(g_input, 0, 1, &got) && got.amp_low == 0.5f && got.freq_high == 320.f, "player 1 right device receives the value");
    CHECK(!napple_input_get_vibration(g_input, 0, 0, &got), "left device untouched");
    /* handheld style index (1) routes to the handheld npad (slot 9) */
    req.h = (0u) | (1u << 8) | (0x20u << 16) | (0u << 24); req.v.amp_low = 0.75f;
    call(hid_send_vibration, &req, sizeof req, &c);
    CHECK(napple_input_get_vibration(g_input, 9, 0, &got) && got.amp_low == 0.75f, "handheld handle maps to slot 9");
    /* player 3, left */
    req.h = (0u) | (0u << 8) | (2u << 16) | (0u << 24); req.v.amp_low = 0.125f;
    call(hid_send_vibration, &req, sizeof req, &c);
    CHECK(napple_input_get_vibration(g_input, 2, 0, &got) && got.amp_low == 0.125f, "player 3 left");
    /* an invalid device index is ignored, a short request does not crash the runtime */
    req.h = (3u << 24); call(hid_send_vibration, &req, sizeof req, &c); CHECK(c.result == 0, "device index 3 ignored");
    /* GetActualVibrationValue reads back what was sent */
    uint32_t h = (0u) | (0u << 8) | (0u << 16) | (1u << 24); uint8_t in8[16] = {0}; memcpy(in8, &h, 4);
    call(hid_get_actual_vibration, in8, sizeof in8, &c);
    napple_vibration back; memcpy(&back, c.out, sizeof back); CHECK(c.out_size == sizeof back && back.amp_low == 0.5f, "GetActualVibrationValue returns the stored value");
    /* GetVibrationDeviceInfo: left LRA type 1 position 0, right type 2 position 1 */
    uint32_t l = 0, r = 1u << 24; uint32_t info[2];
    call(hid_vibration_device_info, &l, 4, &c); memcpy(info, c.out, 8); CHECK(info[0] == 1 && info[1] == 0, "left device info");
    call(hid_vibration_device_info, &r, 4, &c); memcpy(info, c.out, 8); CHECK(info[0] == 2 && info[1] == 1, "right device info");
    /* supported styles reach the backend */
    uint32_t styles = NAPPLE_STYLE_HANDHELD; call(hid_set_styles, &styles, 4, &c); napple_input_update(g_input, 1000000000ull);
    CHECK(napple_input_active_style(g_input, 0) == NAPPLE_STYLE_HANDHELD, "SetSupportedNpadStyleSet is applied");
    napple_input_stop(g_input);
    char cmd[200]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)system(cmd);
    printf("{\"test\":\"hid_service\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
