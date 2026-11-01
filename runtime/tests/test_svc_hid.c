/* Unit tests: hid rows backed by the input library (debug pad, gesture, six-axis rest, npad assignment, touch resolution, vibration devices).
 * hid.c is #included for its static handlers; the input library is linked for real (headless: no GameController). */
#include "../src/services/hid.c"
#include "svc_test.h"
#include <unistd.h>

#define MS 1000000ull
static uint64_t T = 5000 * MS;
static void tick(unsigned n) { for (unsigned i = 0; i < n; i++) napple_input_update(g_input, T += 8 * MS); }
static uint32_t r32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static const uint8_t *shm(void) { return g_shmem->host; }
static void call(unsigned cmd, const void *in, uint32_t n, IpcCall *c) { IpcObject o = {"hid", hid_cmds, sizeof hid_cmds / sizeof hid_cmds[0], NULL, NULL, 1}; svc_call(&o, cmd, in, n, c); }

int main(void) {
    char dir[] = "/tmp/napple-svchid-XXXXXX"; if (!mkdtemp(dir)) return 1;
    g_cfg.headless = 1; g_cfg.save_root = dir; snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "0100ABCD12345678");
    IpcCall c;
    ensure_input();
    napple_hid_layout l; napple_hid_layout_for(NAPPLE_HID_LAYOUT_LATEST, &l);
    napple_input_set_supported_styles(g_input, NAPPLE_STYLE_FULL_KEY | NAPPLE_STYLE_JOY_DUAL);
    tick(2);

    /* no row is a stub any more */
    for (unsigned i = 0; i < sizeof hid_cmds / sizeof hid_cmds[0]; i++) CHECK(hid_cmds[i].status == IPC_IMPL, "hid row %u (%s) is resolved", hid_cmds[i].cmd, hid_cmds[i].name);

    /* debug pad */
    napple_input_key(g_input, 37, 1); /* A */
    tick(2);
    CHECK(r32(napple_hid_lifo_latest(shm(), l.debug_pad, 0x20) + 12) == 0, "debug pad silent before ActivateDebugPad");
    call(1, NULL, 0, &c); CHECK(c.result == 0 && HC()->debug_pad_active, "ActivateDebugPad");
    tick(2);
    CHECK(r32(napple_hid_lifo_latest(shm(), l.debug_pad, 0x20) + 12) & 1, "debug pad follows player 1 (A)");
    napple_input_key(g_input, 37, 0);

    /* gesture */
    call(91, NULL, 0, &c); CHECK(HC()->gesture_active, "ActivateGesture");
    napple_touch_point p = {.finger_id = 0, .x = 640, .y = 360};
    napple_input_touch_points(g_input, &p, 1); tick(1);
    CHECK(r32(napple_hid_lifo_latest(shm(), l.gesture, 0x60) + 16) == 3 /* Touch */, "gesture Touch from a touch frame");
    napple_input_touch_points(g_input, NULL, 0); tick(1);
    CHECK(r32(napple_hid_lifo_latest(shm(), l.gesture, 0x60) + 16) == 5 /* Tap */, "gesture Tap on release");

    /* six-axis at rest: the default pose is at rest; a shake is not */
    uint32_t h[4] = {0x0}; /* handle: player 1 */
    call(82, h, 16, &c); CHECK(c.out_size == 1 && c.out[0] == 1, "IsSixAxisSensorAtRest: at rest");
    napple_input_shake(g_input); tick(4);
    call(82, h, 16, &c); CHECK(c.out[0] == 0, "IsSixAxisSensorAtRest: not at rest while shaking");
    tick(200);
    call(82, h, 16, &c); CHECK(c.out[0] == 1, "at rest again after the shake");

    /* console six-axis needs Activate + Start */
    call(300, NULL, 0, &c); tick(1); CHECK(HC()->console_six_active && shm()[l.console_sixaxis + 8] == 0 && *(const uint64_t *)(shm() + l.console_sixaxis) == 0, "ActivateConsoleSixAxisSensor alone writes nothing");
    call(301, h, 8, &c); tick(1); CHECK(HC()->console_six_started && shm()[l.console_sixaxis + 8] == 1 && *(const uint64_t *)(shm() + l.console_sixaxis) >= 1, "StartConsoleSixAxisSensor: the record is sampled, at rest");
    call(302, h, 8, &c); CHECK(!HC()->console_six_started, "StopConsoleSixAxisSensor");

    /* npad assignment */
    uint32_t id = 0; uint8_t in16[16] = {0};
    memcpy(in16, &id, 4);
    call(107, in16, 16, &c); CHECK(c.result == 0 && (HC()->disconnected & 1), "DisconnectNpad(player 1)");
    tick(2); CHECK(napple_input_active_style(g_input, 0) == 0, "the backend reports the pad as gone");
    uint32_t bad = 77; memcpy(in16, &bad, 4); call(107, in16, 16, &c); CHECK(c.result != 0, "DisconnectNpad of an unknown id fails");
    call(103, NULL, 0, &c); tick(2); CHECK(HC()->disconnected == 0 && napple_input_active_style(g_input, 0) != 0, "ActivateNpad reconnects");
    call(104, NULL, 0, &c); CHECK(!HC()->npad_active, "DeactivateNpad");
    call(103, NULL, 0, &c); CHECK(HC()->npad_active, "ActivateNpad");
    uint32_t ab[4] = {0, 1, 0, 0}; call(130, ab, 16, &c); CHECK(c.result == 0 && hidc_pad_source(HC(), 0) == 1 && hidc_pad_source(HC(), 1) == 0, "SwapNpadAssignment");
    call(130, ab, 16, &c); CHECK(hidc_pad_source(HC(), 0) == 0, "swap back");
    ab[1] = 0; call(130, ab, 16, &c); CHECK(c.result != 0, "swap with itself fails");
    ab[0] = 0; ab[1] = 1; call(125, ab, 16, &c); CHECK(c.result == 0 && HC()->dual_of[0] == 2, "MergeSingleJoyAsDualJoy");
    tick(2); CHECK(napple_input_active_style(g_input, 0) == NAPPLE_STYLE_JOY_DUAL, "merged pad reports a Joy-Con pair");
    call(126, NULL, 0, &c); CHECK(HC()->lr_assign_mode == 1, "StartLrAssignmentMode"); call(127, NULL, 0, &c); CHECK(HC()->lr_assign_mode == 0, "StopLrAssignmentMode");
    uint8_t on = 1; call(134, &on, 1, &c); CHECK(HC()->center_clamp == 1, "center clamp flag (134)");
    uint8_t cap[24] = {0}; uint32_t style = NAPPLE_STYLE_FULL_KEY; uint64_t btn = 7; memcpy(cap, &style, 4); memcpy(cap + 16, &btn, 8);
    call(135, cap, 24, &c); CHECK(c.result == 0 && hidc_capture_get(HC(), style) == 7, "SetNpadCaptureButtonAssignment (135)");
    uint64_t aruid = 0x1000; call(136, &aruid, 8, &c); CHECK(c.result == 0 && hidc_capture_get(HC(), style) == -1, "ClearNpadCaptureButtonAssignment(aruid) (136) clears every style");
    style = 3; memcpy(cap, &style, 4); call(135, cap, 24, &c); CHECK(c.result != 0, "capture assignment needs one style");
    { /* 133 SetNpadJoyAssignmentModeSingleWithDestination(npad id, type, aruid) -> {bool, u32 id} */
        uint32_t in[4] = {0, 0, 0x1000, 0}; call(133, in, 16, &c);
        CHECK(c.result == 0 && c.out_size == 8 && c.out[0] == 0 && SVC_OUT(&c, uint32_t, 4) == 0, "SingleWithDestination: not reassigned, same npad");
        in[0] = 77; call(133, in, 16, &c); CHECK(c.result != 0, "SingleWithDestination: unknown npad id refused");
    }
    uint32_t lock = 1u << 3; call(32, &lock, 4, &c); tick(2);
    CHECK((HC()->kb_locks & 2) && (r32(napple_hid_lifo_latest(shm(), l.keyboard, 0x30) + 8) & (1u << 5)), "caps lock shows in the keyboard modifiers");

    /* touch resolution */
    uint32_t wh[4] = {1920, 1080, 0, 0}; call(1004, wh, 16, &c); CHECK(c.result == 0 && HC()->touch_w == 1920, "SetTouchScreenResolution");
    wh[0] = 0; call(1004, wh, 16, &c); CHECK(c.result != 0, "zero width refused");
    uint8_t mode = 1; call(1002, &mode, 1, &c); CHECK(HC()->touch_mode == 1, "SetTouchScreenConfiguration");
    call(1003, NULL, 0, &c); CHECK(c.out[0] == 0, "no firmware update needed");

    /* xpad: none on this host */
    call(55, NULL, 0, &c); CHECK(c.out_size == 8 && SVC_OUT(&c, int64_t, 0) == 0, "GetXpadIds: none");
    call(40, NULL, 0, &c); CHECK(c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "xpad id event never fires");

    /* vibration devices */
    call(203, NULL, 0, &c); IpcObject *list = c.out_objects[0];
    uint32_t vh = 0x01000000; svc_call(list, 0, &vh, 4, &c); CHECK(c.result == 0 && hidc_vib_is_active(HC(), vh), "ActivateVibrationDevice");
    vh = 2u << 24; svc_call(list, 0, &vh, 4, &c); CHECK(c.result != 0, "invalid handle refused");
    uint8_t mh[16] = {0}; memset(mh, 0, sizeof mh); call(211, mh, 16, &c); CHECK(c.out[0] == 1, "player 1's device is mounted");
    uint32_t p4 = 3u << 16; memcpy(mh, &p4, 4); call(211, mh, 16, &c); CHECK(c.out[0] == 0, "player 4 has no pad");
    { /* the style set the title announces reaches the backend untouched, GameCube / Palma / NES bits included */
        uint32_t want = NAPPLE_STYLE_GC | NAPPLE_STYLE_PALMA | NAPPLE_STYLE_LARK_NES_LEFT;
        call(100, &want, 4, &c); call(101, NULL, 0, &c);
        CHECK(SVC_OUT(&c, uint32_t, 0) == want, "GetSupportedNpadStyleSet returns the announced set (%#x)", SVC_OUT(&c, uint32_t, 0));
    }
    /* ---- ipc-depth-hid (2026-11-01-r2) rows ---- */
    { /* six-axis parameters per sensor handle */
        uint8_t in[16] = {0}; uint32_t sh = 0x00010000; memcpy(in, &sh, 4); float g = 0.25f, r = 0.5f; memcpy(in + 4, &g, 4); memcpy(in + 8, &r, 4);
        call(70, in, 16, &c); call(71, in, 16, &c); CHECK(c.out_size == 8 && SVC_OUT(&c, float, 0) == 0.25f && SVC_OUT(&c, float, 4) == 0.5f, "GetSixAxisSensorFusionParameters reads back 70");
        float ax = -0.05f, ay = 0.1f; memcpy(in + 4, &ax, 4); memcpy(in + 8, &ay, 4);
        call(73, in, 16, &c); call(74, in, 16, &c); CHECK(SVC_OUT(&c, float, 0) == -0.05f && SVC_OUT(&c, float, 4) == 0.1f, "accelerometer parameters round trip");
        call(75, in, 16, &c); call(74, in, 16, &c); CHECK(SVC_OUT(&c, float, 0) == 0.0f, "ResetAccelerometerParameters");
        uint32_t pm = 1; memcpy(in + 4, &pm, 4); call(76, in, 16, &c); call(77, in, 16, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 1, "accelerometer play mode round trip");
        call(78, in, 16, &c); call(77, in, 16, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "ResetAccelerometerPlayMode");
        uint8_t pt[16] = {1}; memcpy(pt + 4, &sh, 4); call(84, pt, 16, &c); call(85, in, 16, &c); CHECK(c.out[0] == 1, "unaltered passthrough flag");
        call(83, in, 16, &c); CHECK(c.out_size == 1 && c.out[0] == 0, "no six-axis firmware update");
    }
    { /* vibration permit session and GC ERM */
        uint8_t pv = 0; call(204, &pv, 1, &c); CHECK(!hidc_vibration_allowed(HC()), "PermitVibration(false) mutes");
        call(209, NULL, 0, &c); CHECK(hidc_vibration_allowed(HC()), "BeginPermitVibrationSession allows rumble");
        call(210, NULL, 0, &c); CHECK(!hidc_vibration_allowed(HC()), "EndPermitVibrationSession");
        pv = 1; call(204, &pv, 1, &c);
        uint64_t erm[3] = {0x00030000 | (5u << 8) /* GC style index */, 0x1000, 1}; call(207, erm, 24, &c); CHECK(c.result == 0, "SendVibrationGcErmCommand(Start)");
        call(208, erm, 16, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 1, "GetActualVibrationGcErmCommand reads back Start");
        erm[2] = 9; call(207, erm, 24, &c); CHECK(c.result != 0, "unknown ERM command refused");
        uint8_t vb[16] = {0}; vb[4] = 1; call(212, vb, 16, &c); CHECK(c.result == 0, "SendVibrationValueInBool");
    }
    { /* seven six-axis sensor */
        call(303, NULL, 0, &c); call(304, NULL, 0, &c); CHECK(HC()->seven_active && HC()->seven_started, "seven six-axis activate + start");
        float st = 2.5f; call(308, &st, 4, &c); call(309, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 2.5f, "seven six-axis fusion strength round trip");
        uint64_t init[3] = {0x1000, 0x1000, 0x7000}; call(306, init, 24, &c); CHECK(HC()->seven_init && HC()->seven_mem_size[1] == 0x7000, "InitializeSevenSixAxisSensor");
        call(307, NULL, 0, &c); CHECK(!HC()->seven_init && !HC()->seven_started, "FinalizeSevenSixAxisSensor");
        call(311, NULL, 0, &c); CHECK(HC()->console_six_active && HC()->console_six_started, "ForceActivateConsoleSixAxisSensor");
        call(312, NULL, 0, &c); CHECK(!HC()->console_six_active, "ForceDeactivateConsoleSixAxisSensor");
    }
    { /* controller properties of player 1 (connected, not handheld) and player 8 (absent) */
        uint32_t p1[4] = {0}; uint32_t p8[4] = {7};
        call(403, p1, 16, &c); CHECK(c.out[0] == 1, "HasBattery(player 1)"); call(403, p8, 16, &c); CHECK(c.out[0] == 0, "HasBattery(absent)");
        call(405, p1, 16, &c); CHECK(c.out[0] == 1, "GetNpadInterfaceType: Bluetooth"); call(405, p8, 16, &c); CHECK(c.out[0] == 0, "no interface for an absent pad");
        call(402, p1, 16, &c); CHECK(c.out[0] == 0, "no USB full-key controller");
        uint8_t en = 1; call(401, &en, 1, &c); call(400, NULL, 0, &c); CHECK(c.out[0] == 1, "EnableUsbFullKeyController round trip");
        uint32_t ids[2] = {7, 0}; IpcCall cc; memset(&cc, 0, sizeof cc); svc_set_in_buf(&cc, ids, sizeof ids);
        IpcObject o = {"hid", hid_cmds, sizeof hid_cmds / sizeof hid_cmds[0], NULL, NULL, 1}; svc_call_c(&o, 407, &cc);
        CHECK(SVC_OUT(&cc, uint32_t, 0) == 0, "GetNpadOfHighestBatteryLevel picks the connected pad");
    }
    { /* Palma: refused on a pad that is not a Palma; operations on a handle complete and signal */
        uint32_t p1[4] = {0}; call(500, p1, 16, &c); CHECK(c.result != 0, "GetPalmaConnectionHandle refused without the Palma style");
        uint64_t h = 0; call(501, &h, 8, &c); CHECK(c.result == 0 && HC()->palma[0].initialized, "InitializePalma");
        call(502, &h, 8, &c); CHECK(c.nout_copy == 1, "AcquirePalmaOperationCompleteEvent"); uint32_t ev = c.out_copy[0];
        call(503, &h, 8, &c); CHECK(c.result != 0, "no operation yet");
        call(506, &h, 8, &c); CHECK(event_is_signaled(ev), "ReadPalmaStep completes and signals");
        uint8_t data[0x140]; IpcCall cc; memset(&cc, 0, sizeof cc); cc.in = (const uint8_t *)&h; cc.in_size = 8; svc_set_out_buf(&cc, data, sizeof data);
        IpcObject o = {"hid", hid_cmds, sizeof hid_cmds / sizeof hid_cmds[0], NULL, NULL, 1}; svc_call_c(&o, 503, &cc);
        CHECK(cc.result == 0 && SVC_OUT(&cc, uint64_t, 0) == HIDC_PALMA_OP_READ_STEP, "GetPalmaOperationInfo: ReadStep");
        uint8_t blob[4] = {1, 2, 3, 4}; uint64_t wr[3] = {0, 0x10, 4}; memset(&cc, 0, sizeof cc); cc.in = (const uint8_t *)wr; cc.in_size = 24; svc_set_in_buf(&cc, blob, 4);
        svc_call_c(&o, 510, &cc); CHECK(cc.result == 0, "WritePalmaApplicationSection");
        call(509, wr, 24, &c); CHECK(c.result == 0 && HC()->palma[0].op_data[2] == 3, "ReadPalmaApplicationSection reads the bytes back");
        uint64_t dbv[2] = {5, 0}; call(516, dbv, 16, &c); call(517, &h, 8, &c); CHECK(HC()->palma[0].op_data[0] == 5, "database identification version round trip");
        call(519, &h, 8, &c); CHECK(c.result == 0, "GetPalmaOperationResult: success");
        call(528, &h, 8, &c); CHECK(c.out_size == 6 && c.out[0] == 0x02, "GetPalmaBluetoothAddress: locally administered");
        uint64_t bad = 77; call(501, &bad, 8, &c); CHECK(c.result != 0, "unknown connection handle refused");
    }
    { /* the rest: state rows */
        call(58, NULL, 0, &c); CHECK(c.result != 0, "GetJoyXpadLifoHandle: no xpad id exists");
        call(59, NULL, 0, &c); CHECK(c.out_size == 8 && SVC_OUT(&c, int64_t, 0) == 0, "GetJoyXpadIds: none");
        uint32_t v = 3; call(3150, &v, 4, &c); CHECK(HC()->mouse_lib_version == 3, "SetMouseLibraryVersion");
        call(2000, NULL, 0, &c); CHECK(HC()->digitizer_active, "ActivateDigitizer");
        call(320, NULL, 0, &c); CHECK(HC()->npad_imu == 1, "EnableNpadImu"); call(321, NULL, 0, &c); CHECK(HC()->npad_imu == 0, "DisableNpadImu");
    }
    printf("{\"test\":\"svc_hid\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    char cmd[200]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
    return g_failed != 0;
}
