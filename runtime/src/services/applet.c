/* appletOE: the application's view of the applet manager (nn::oe). One process, always in focus: operation mode and
 * performance mode come from the launch configuration, messages are queued and signalled through readable events. */
#include "../ipc.h"
#include "napple_display.h"
#include "host_info.h"
#include "applet_core.h"
#include "applet_lib_core.h"
#include "applet_web_core.h"
#include "applet_album_core.h"
#include "../frontend.h"
#include "../gfx.h"
#include "napple_frontend.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MSG_EXIT_REQUESTED = 4, MSG_FOCUS_CHANGED = 15, MSG_RESUME = 16, MSG_OPERATION_MODE_CHANGED = 30, MSG_PERFORMANCE_MODE_CHANGED = 31 };
#define R_NO_MESSAGES 0x680u /* applet module 128 desc 3 */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t queue[32];
static unsigned qhead, qcount;
static uint32_t msg_events[8];
static unsigned nmsg_events; /* writable ends of the message events handed to the guest */
static int started;
static uint32_t focus_state = 1; /* 1 InFocus */

static void signal_events(void) {
    for (unsigned i = 0; i < nmsg_events; i++) event_signal_handle(msg_events[i]);
}
void applet_push_message(uint32_t message) {
    pthread_mutex_lock(&lock);
    if (qcount < 32) {
        queue[(qhead + qcount++) % 32] = message;
        signal_events();
    }
    pthread_mutex_unlock(&lock);
    if (message == MSG_OPERATION_MODE_CHANGED) applet_display_resolution_changed();
}
/* While a library applet is on screen the application is out of focus (FocusState 2, message FocusStateChanged): on the console that is what
 * makes it stop drawing (it would otherwise alternate frames with the applet on the one screen); back to InFocus when the applet returns. */
void applet_set_focus(int in_focus) {
    gfx_set_hidden(!in_focus);
    pthread_mutex_lock(&lock);
    focus_state = in_focus ? 1 : 2;
    pthread_mutex_unlock(&lock);
    applet_push_message(MSG_FOCUS_CHANGED);
}
/* 1 while a library applet owns the screen (the game is out of focus). */
int applet_game_hidden(void) {
    pthread_mutex_lock(&lock);
    int h = focus_state != 1;
    pthread_mutex_unlock(&lock);
    return h;
}
static void applet_start_once(void) {
    if (started) return;
    started = 1;
    queue[0] = MSG_FOCUS_CHANGED;
    qcount = 1; /* a freshly launched application is told it has focus */
}
static uint32_t new_message_event(void) {
    uint32_t readable = 0, writable = event_create(&readable);
    pthread_mutex_lock(&lock);
    if (nmsg_events < 8) msg_events[nmsg_events++] = writable;
    if (qcount) event_signal_handle(writable);
    pthread_mutex_unlock(&lock);
    return readable;
}
/* Events the guest holds that the host signals later: one list per kind (the writable ends), so a state change can wake every holder. */
enum { EV_SLEEP_LOCK, EV_RESOLUTION, EV_SUSPENDED_TICK, EV_GPU_ERROR, EV_FRIEND_INVITE, EV_AF_210, EV_KINDS };
#define EV_MAX 8
static uint32_t tracked_ev[EV_KINDS][EV_MAX];
static unsigned ntracked_ev[EV_KINDS];
static uint32_t plain_event(int signaled);
static uint32_t tracked_event(unsigned kind, int signaled) {
    uint32_t readable = 0, writable = event_create(&readable);
    pthread_mutex_lock(&lock);
    if (ntracked_ev[kind] < EV_MAX) tracked_ev[kind][ntracked_ev[kind]++] = writable;
    pthread_mutex_unlock(&lock);
    if (signaled) event_signal_handle(writable);
    return readable;
}
static void signal_tracked(unsigned kind) {
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < ntracked_ev[kind]; i++) event_signal_handle(tracked_ev[kind][i]);
    pthread_mutex_unlock(&lock);
}
static void clear_tracked(unsigned kind) {
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < ntracked_ev[kind]; i++) event_clear_handle(tracked_ev[kind][i]);
    pthread_mutex_unlock(&lock);
}
/* Raise the system events the SDK polls but that nothing in an offline run triggers by itself (the GPU error event, a friend invitation); hooks for the
 * frontend/tests, and the way the rows stay honest: the event is real and signals when its cause happens. */
void applet_signal_gpu_error(void) { signal_tracked(EV_GPU_ERROR); }
void applet_signal_friend_invitation(void) { signal_tracked(EV_FRIEND_INVITE); }
/* The display resolution changed (docked/handheld switch): wakes GetDefaultDisplayResolutionChangeEvent. */
void applet_display_resolution_changed(void) { signal_tracked(EV_RESOLUTION); }
static uint32_t plain_event(int signaled) {
    uint32_t readable = 0, writable = event_create(&readable);
    if (signaled) event_signal_handle(writable);
    return readable;
}


/* Application-visible settings (applet_core.h): what the guest sets is what it reads back. A request too short for the argument it should carry is
 * ignored rather than failing the runtime (the argument layouts are from the author's memory of the public documentation, unverified). */
static AppletState astate;
static int astate_ready;
static AppletState *AS(void) {
    if (!astate_ready) {
        applet_state_init(&astate);
        astate_ready = 1;
        const char *e = getenv("NAPPLE_ILLUMINANCE"); /* a host light sensor value (lux): the sensor is reported as present */
        if (e && *e) applet_illuminance_set(&astate, 1, (float)atof(e));
        e = getenv("NAPPLE_PLATFORM_REGION");
        if (e && atoi(e) == 2) astate.platform_region = 2;
    }
    return &astate;
}
#define IN_OK(c, off, T) ((c)->in_size >= (uint32_t)((off) + sizeof(T)))
int applet_screenshot_permitted(void) { return applet_screenshot_allowed(AS()); }

/* ---- ICommonStateGetter ------------------------------------------------------------------------------------------ */
static void csg_get_event_handle(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_copy_handle(c, new_message_event());
}
static void csg_receive_message(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&lock);
    if (!qcount) {
        pthread_mutex_unlock(&lock);
        c->result = R_NO_MESSAGES;
        return;
    }
    uint32_t m = queue[qhead];
    qhead = (qhead + 1) % 32;
    qcount--;
    if (qcount) signal_events();
    else
        for (unsigned i = 0; i < nmsg_events; i++) event_clear_handle(msg_events[i]); /* the real queue de-asserts its event when it empties */
    pthread_mutex_unlock(&lock);
    LOGV("applet", "ReceiveMessage -> %u", m);
    IPC_OUT(c, m);
}
uint32_t g_applet_kind = 1; /* AppletType: 1 Application; a firmware applet process (applet_ae.c) is 3 LibraryApplet */
static void csg_get_this_applet_kind(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = g_applet_kind;
    IPC_OUT(c, v);
}
/* Also the NotifyMessageToHomeMenuForDebug row: with no Home menu process the console's debug message has no receiver, so accepting it silently is the answer. */
static void csg_noop(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static void csg_get_operation_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = g_cfg.op_mode_handheld ? 0 : 1;
    IPC_OUT(c, v);
}
static void csg_get_performance_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = g_cfg.op_mode_handheld ? 0 : 1;
    IPC_OUT(c, v);
}
static void csg_get_cradle_status(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = g_cfg.op_mode_handheld ? 0 : 1;
    IPC_OUT(c, v);
}
static void csg_get_boot_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 0;
    IPC_OUT(c, v);
}
static void csg_get_focus_state(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)focus_state;
    IPC_OUT(c, v);
}
static void csg_get_resolution(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t gw, gh;
    napple_display_game_size(g_cfg.op_mode_handheld ? 1280 : 1920, g_cfg.op_mode_handheld ? 720 : 1080, &gw, &gh);
    int32_t w = (int32_t)gw, h = (int32_t)gh;
    IPC_OUT(c, w);
    IPC_OUT(c, h); /* the window's aspect when the player asked for it (settings: aspect 6) */
}
static void csg_get_resolution_event(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_copy_handle(c, tracked_event(EV_RESOLUTION, 0));
}
/* Sleep: AllowToEnterSleep/DisallowToEnterSleep set a flag the host's idle-sleep handling reads (applet_sleep_disallowed); the sleep lock is acquired at
 * once (nothing contends for it) and its event is signalled while the lock is held. */
int applet_sleep_disallowed(void) { return AS()->sleep_disallowed || AS()->sleep_lock_held || AS()->auto_sleep_disabled; }
static void csg_allow_sleep(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    AS()->sleep_disallowed = 0;
}
static void csg_disallow_sleep(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    AS()->sleep_disallowed = 1;
}
static void csg_acquire_sleep_lock(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    if (applet_sleep_lock_acquire(AS())) signal_tracked(EV_SLEEP_LOCK);
}
static void csg_release_sleep_lock(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    applet_sleep_lock_release(AS());
    clear_tracked(EV_SLEEP_LOCK);
}
static void csg_release_sleep_lock_transiently(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    applet_sleep_lock_release_transiently(AS());
    clear_tracked(EV_SLEEP_LOCK);
}
static void csg_get_sleep_lock_event(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_copy_handle(c, tracked_event(EV_SLEEP_LOCK, AS()->sleep_lock_held && !AS()->sleep_lock_transient));
}
static void csg_set_request_exit_to_la(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->request_exit_to_library_applet = IPC_IN(c, uint8_t, 0) != 0;
}
static void csg_is_vr_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)AS()->vr_mode;
    IPC_OUT(c, v);
}
static void csg_set_vr_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->vr_mode = IPC_IN(c, uint8_t, 0) != 0;
}
static void csg_set_cpu_boost(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint32_t)) {
        AS()->cpu_boost_mode = IPC_IN(c, uint32_t, 0);
        AS()->cpu_boost_requests++;
    }
}
static void csg_cancel_cpu_boost(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    AS()->cpu_boost_mode = 0;
}
static void csg_set_perf_notification(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->performance_mode_notification = IPC_IN(c, uint8_t, 0) != 0;
}
static void csg_get_current_perf_config(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = g_cfg.op_mode_handheld ? 0x00020001 : 0x00020003;
    IPC_OUT(c, v);
}
static void csg_get_builtin_display_type(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = 0;
    IPC_OUT(c, v);
}
/* GetSettingsPlatformRegion (300): 1 Global, 2 China. */
static void csg_get_platform_region(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = AS()->platform_region;
    IPC_OUT(c, v);
}
static const IpcCmd csg_cmds[] = {
    {300, "GetSettingsPlatformRegion", csg_get_platform_region, IPC_IMPL},
    {0, "GetEventHandle", csg_get_event_handle, IPC_IMPL},
    {1, "ReceiveMessage", csg_receive_message, IPC_IMPL},
    {2, "GetThisAppletKind", csg_get_this_applet_kind, IPC_IMPL},
    {3, "AllowToEnterSleep", csg_allow_sleep, IPC_IMPL},
    {4, "DisallowToEnterSleep", csg_disallow_sleep, IPC_IMPL},
    {5, "GetOperationMode", csg_get_operation_mode, IPC_IMPL},
    {6, "GetPerformanceMode", csg_get_performance_mode, IPC_IMPL},
    {7, "GetCradleStatus", csg_get_cradle_status, IPC_IMPL},
    {8, "GetBootMode", csg_get_boot_mode, IPC_IMPL},
    {9, "GetCurrentFocusState", csg_get_focus_state, IPC_IMPL},
    {10, "RequestToAcquireSleepLock", csg_acquire_sleep_lock, IPC_IMPL},
    {11, "ReleaseSleepLock", csg_release_sleep_lock, IPC_IMPL},
    {12, "ReleaseSleepLockTransiently", csg_release_sleep_lock_transiently, IPC_IMPL},
    {13, "GetAcquiredSleepLockEvent", csg_get_sleep_lock_event, IPC_IMPL},
    {50, "IsVrModeEnabled", csg_is_vr_mode, IPC_IMPL},
    {51, "SetVrModeEnabled", csg_set_vr_mode, IPC_IMPL},
    {60, "GetDefaultDisplayResolution", csg_get_resolution, IPC_IMPL},
    {61, "GetDefaultDisplayResolutionChangeEvent", csg_get_resolution_event, IPC_IMPL},
    {66, "SetCpuBoostMode", csg_set_cpu_boost, IPC_IMPL},
    {67, "CancelCpuBoostMode", csg_cancel_cpu_boost, IPC_IMPL},
    {68, "GetBuiltInDisplayType", csg_get_builtin_display_type, IPC_IMPL},
    {900, "SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled", csg_set_request_exit_to_la, IPC_IMPL},
    {90, "SetPerformanceConfigurationChangedNotification", csg_set_perf_notification, IPC_IMPL},
    {91, "GetCurrentPerformanceConfiguration", csg_get_current_perf_config, IPC_IMPL},
};

/* ---- ISelfController ---------------------------------------------------------------------------------------------- */
static void sc_launchable_event(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_copy_handle(c, plain_event(1));
} /* library applets can always be launched: signalled */
static void sc_create_managed_layer(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t id = 1;
    IPC_OUT(c, id);
}
/* CreateManagedDisplaySeparableLayer (10.0.0+): out display layer id, recording layer id. Layer 1 is the one vi's OpenLayer hands out; the recording layer is unused (no capture). */
static void sc_create_separable_layer(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t id[2] = {1, 2};
    IPC_OUT(c, id);
}
static void sc_is_auto_sleep_disabled(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)AS()->auto_sleep_disabled;
    IPC_OUT(c, v);
}
static void sc_set_auto_sleep_disabled(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->auto_sleep_disabled = IPC_IN(c, uint8_t, 0) != 0;
}
static void sc_set_screenshot_permission(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!IN_OK(c, 0, uint32_t)) return;
    uint32_t v = IPC_IN(c, uint32_t, 0);
    if (!applet_valid_screenshot_permission(v)) {
        c->result = 0x1d80;
        return;
    }
    AS()->screenshot_permission = (int)v;
    LOGV("applet", "SetScreenShotPermission %u", v);
}
static void sc_set_restart_message(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->restart_message = IPC_IN(c, uint8_t, 0) != 0;
}
static void sc_set_fw_update_section(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->controller_firmware_update_section = IPC_IN(c, uint8_t, 0) != 0;
}
static void sc_set_capture_short_press(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->capture_short_press_message = IPC_IN(c, uint8_t, 0) != 0;
}
static void sc_set_orientation(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint32_t)) AS()->screenshot_orientation = IPC_IN(c, uint32_t, 0);
}
static void sc_set_keyboard_layout(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint32_t)) AS()->keyboard_layout = IPC_IN(c, uint32_t, 0);
}
static void sc_set_media_playback(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->media_playback_state = IPC_IN(c, uint8_t, 0);
}
static void sc_set_idle_ext(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint32_t)) AS()->idle_detection_extension = IPC_IN(c, uint32_t, 0);
}
static void sc_get_idle_ext(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = AS()->idle_detection_extension;
    IPC_OUT(c, v);
}
static void sc_set_input_sources(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint32_t)) AS()->input_detection_source_set = IPC_IN(c, uint32_t, 0);
}
static void sc_report_user_active(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    AS()->user_active_reports++;
}
static void sc_set_wireless_priority(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint32_t)) AS()->wireless_priority_mode = IPC_IN(c, uint32_t, 0);
}
static void sc_set_album_notification(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->album_image_taken_notification = IPC_IN(c, uint8_t, 0) != 0;
}
static void sc_get_illuminance(IpcObject *s, IpcCall *c) {
    (void)s;
    float v = applet_illuminance_get(AS(), NULL);
    IPC_OUT(c, v);
}
/* GetCurrentIlluminanceEx: {f32 lux, bool over the sensor's limit} [layout unverified]. */
static void sc_get_illuminance_ex(IpcObject *s, IpcCall *c) {
    (void)s;
    int over;
    float v = applet_illuminance_get(AS(), &over);
    uint8_t o = (uint8_t)over;
    IPC_OUT(c, v);
    IPC_OUT(c, o);
}
static void sc_is_illuminance_available(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = (uint8_t)AS()->illuminance_available;
    IPC_OUT(c, v);
}
static void sc_suspended_tick_value(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = AS()->suspended_ticks;
    IPC_OUT(c, v);
}
static void sc_suspended_tick_event(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_copy_handle(c, tracked_event(EV_SUSPENDED_TICK, 0));
}
static void sc_enter_fatal(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    applet_fatal_enter(AS());
}
static void sc_leave_fatal(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    applet_fatal_leave(AS());
}
static void sc_set_screenshot_identity(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < sizeof AS()->screenshot_identity) return;
    memcpy(AS()->screenshot_identity, c->in, sizeof AS()->screenshot_identity);
    AS()->screenshot_identity_set = 1;
}
static void sc_set_handles_request_to_display(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->handles_request_to_display = IPC_IN(c, uint8_t, 0) != 0;
}
static void sc_approve_to_display(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    AS()->approve_to_display_count++;
    AS()->window_visible = 1;
}
static void sc_override_auto_sleep(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!IN_OK(c, 15, uint8_t)) return;
    for (unsigned i = 0; i < 4; i++) AS()->auto_sleep_override[i] = IPC_IN(c, int32_t, 4 * i);
    AS()->auto_sleep_overridden = 1;
}
static void sc_report_multimedia_error(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint32_t)) AS()->last_multimedia_error = IPC_IN(c, uint32_t, 0);
    AS()->multimedia_error_reports++;
    LOG("applet", "ReportMultimediaError %#x", AS()->last_multimedia_error);
}
/* SetApplicationAlbumUserData: the album user data blob (a 0x400-byte buffer in the SDK): kept as size and an FNV-1a hash (the host album does not store it). */
static void sc_set_album_user_data(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b ? ipc_buf_ptr(b) : NULL;
    if (!p) return;
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < b->size; i++) h = (h ^ p[i]) * 1099511628211ull;
    AS()->album_user_data_size = b->size;
    AS()->album_user_data_hash = h;
}
static void sc_exit(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    LOG("applet", "ISelfController::Exit: the guest asked to exit");
    summary_note("guest called appletExit");
}
/* GetSystemSharedLayerHandle: {shared buffer handle, shared layer handle}; a firmware applet that shares the system buffer gets layer 1 (the window's). */
/* IsSystemBufferSharingEnabled (ISelfController) answers like a console process that was not granted the shared system buffer: a failing result, so the applet draws into its own layer
 * (no system compositor shares buffers with this process; the answer is the console's own for an unprivileged process, hence impl). GetSystemSharedLayerHandle stays a
 * stub on purpose: it is only reachable after sharing was reported enabled, which never happens here, and the {1, 1} reply is a placeholder. */
static void sc_no_shared_buffers(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = 0x1480;
}
static void sc_shared_layer(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v[2] = {1, 1};
    IPC_OUT(c, v);
}
static const IpcCmd sc_cmds[] = {
    {0, "Exit", sc_exit, IPC_IMPL},
    {1, "LockExit", csg_noop, IPC_IMPL},
    {2, "UnlockExit", csg_noop, IPC_IMPL},
    {3, "EnterFatalSection", sc_enter_fatal, IPC_IMPL},
    {4, "LeaveFatalSection", sc_leave_fatal, IPC_IMPL},
    {9, "GetLibraryAppletLaunchableEvent", sc_launchable_event, IPC_IMPL},
    {10, "SetScreenShotPermission", sc_set_screenshot_permission, IPC_IMPL},
    {11, "SetOperationModeChangedNotification", csg_noop, IPC_IMPL},
    {12, "SetPerformanceModeChangedNotification", csg_noop, IPC_IMPL},
    {13, "SetFocusHandlingMode", csg_noop, IPC_IMPL},
    {14, "SetRestartMessageEnabled", sc_set_restart_message, IPC_IMPL},
    {15, "SetScreenShotAppletIdentityInfo", sc_set_screenshot_identity, IPC_IMPL},
    {16, "SetOutOfFocusSuspendingEnabled", csg_noop, IPC_IMPL},
    {17, "SetControllerFirmwareUpdateSection", sc_set_fw_update_section, IPC_IMPL},
    {18, "SetRequiresCaptureButtonShortPressedMessage", sc_set_capture_short_press, IPC_IMPL},
    {19, "SetScreenShotImageOrientation", sc_set_orientation, IPC_IMPL},
    {20, "SetDesirableKeyboardLayout", sc_set_keyboard_layout, IPC_IMPL},
    {41, "IsSystemBufferSharingEnabled", sc_no_shared_buffers, IPC_IMPL},
    {42, "GetSystemSharedLayerHandle", sc_shared_layer, IPC_STUB},
    {40, "CreateManagedDisplayLayer", sc_create_managed_layer, IPC_IMPL},
    {44, "CreateManagedDisplaySeparableLayer", sc_create_separable_layer, IPC_IMPL},
    {50, "SetHandlesRequestToDisplay", sc_set_handles_request_to_display, IPC_IMPL},
    {51, "ApproveToDisplay", sc_approve_to_display, IPC_IMPL},
    {60, "OverrideAutoSleepTimeAndDimmingTime", sc_override_auto_sleep, IPC_IMPL},
    {61, "SetMediaPlaybackState", sc_set_media_playback, IPC_IMPL},
    {62, "SetIdleTimeDetectionExtension", sc_set_idle_ext, IPC_IMPL},
    {63, "GetIdleTimeDetectionExtension", sc_get_idle_ext, IPC_IMPL},
    {64, "SetInputDetectionSourceSet", sc_set_input_sources, IPC_IMPL},
    {65, "ReportUserIsActive", sc_report_user_active, IPC_IMPL},
    {66, "GetCurrentIlluminance", sc_get_illuminance, IPC_IMPL},
    {67, "IsIlluminanceAvailable", sc_is_illuminance_available, IPC_IMPL},
    {68, "SetAutoSleepDisabled", sc_set_auto_sleep_disabled, IPC_IMPL},
    {69, "IsAutoSleepDisabled", sc_is_auto_sleep_disabled, IPC_IMPL},
    {70, "ReportMultimediaError", sc_report_multimedia_error, IPC_IMPL},
    {71, "GetCurrentIlluminanceEx", sc_get_illuminance_ex, IPC_IMPL},
    {80, "SetWirelessPriorityMode", sc_set_wireless_priority, IPC_IMPL},
    {90, "GetAccumulatedSuspendedTickValue", sc_suspended_tick_value, IPC_IMPL},
    {91, "GetAccumulatedSuspendedTickChangedEvent", sc_suspended_tick_event, IPC_IMPL},
    {100, "SetAlbumImageTakenNotificationEnabled", sc_set_album_notification, IPC_IMPL},
    {110, "SetApplicationAlbumUserData", sc_set_album_user_data, IPC_IMPL},
};

/* ---- IWindowController / IAudioController / IDisplayController ------------------------------------------------------------ */
static void wc_get_resource_user_id(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = 0x1000;
    IPC_OUT(c, v);
}
static void wc_acquire_foreground(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    AS()->foreground_rights = 1;
}
static void wc_release_foreground(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    AS()->foreground_rights = 0;
}
static void wc_reject_background(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->reject_background = IPC_IN(c, uint8_t, 0) != 0;
}
static void wc_set_window_visibility(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->window_visible = IPC_IN(c, uint8_t, 0) != 0;
}
static void wc_set_gpu_time_slice(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint64_t)) AS()->gpu_time_slice = IPC_IN(c, uint64_t, 0);
}
static const IpcCmd wc_cmds[] = {
    {1, "GetAppletResourceUserId", wc_get_resource_user_id, IPC_IMPL},     {10, "AcquireForegroundRights", wc_acquire_foreground, IPC_IMPL},
    {11, "ReleaseForegroundRights", wc_release_foreground, IPC_IMPL},      {12, "RejectToChangeIntoBackground", wc_reject_background, IPC_IMPL},
    {20, "SetAppletWindowVisibility", wc_set_window_visibility, IPC_IMPL}, {21, "SetAppletGpuTimeSlice", wc_set_gpu_time_slice, IPC_IMPL},
};
static void ac_set_volumes(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!IN_OK(c, 4, float)) return;
    AS()->main_volume = applet_clamp_volume(IPC_IN(c, float, 0), AS()->main_volume);
    AS()->library_volume = applet_clamp_volume(IPC_IN(c, float, 4), AS()->library_volume);
}
static void ac_get_main_volume(IpcObject *s, IpcCall *c) {
    (void)s;
    float v = AS()->main_volume;
    IPC_OUT(c, v);
}
static void ac_get_library_volume(IpcObject *s, IpcCall *c) {
    (void)s;
    float v = AS()->library_volume;
    IPC_OUT(c, v);
}
static void ac_change_main_volume(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, float)) AS()->main_volume = applet_clamp_volume(IPC_IN(c, float, 0), AS()->main_volume);
}
static void ac_set_transparent_rate(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, float)) AS()->transparent_volume_rate = applet_clamp_volume(IPC_IN(c, float, 0), AS()->transparent_volume_rate);
}
static const IpcCmd ac_cmds[] = {
    {0, "SetExpectedMasterVolume", ac_set_volumes, IPC_IMPL},
    {1, "GetMainAppletExpectedMasterVolume", ac_get_main_volume, IPC_IMPL},
    {2, "GetLibraryAppletExpectedMasterVolume", ac_get_library_volume, IPC_IMPL},
    {3, "ChangeMainAppletMasterVolume", ac_change_main_volume, IPC_IMPL},
    {4, "SetTransparentVolumeRate", ac_set_transparent_rate, IPC_IMPL},
};
/* IDisplayController capture images: three 1280x720 RGBA slots (applet_core.h) filled from the last presented frame when the frontend registered a frame source
 * (applet_capture_set_source), else opaque black. Get* write the slot to the output buffer and return {bool has_frame}; layouts [unverified against a title]. */
static void dc_get_slot(IpcCall *c, unsigned slot) {
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b && b->size) {
        uint8_t *tmp = malloc(b->size);
        int has = 0;
        size_t n = applet_capture_read(slot, tmp, b->size, &has);
        if (n < b->size) memset(tmp + n, 0, b->size - n);
        ipc_write_buf(c, b, tmp, b->size);
        free(tmp);
        uint8_t v = (uint8_t)has;
        IPC_OUT(c, v);
    } else {
        uint8_t v = (uint8_t)applet_capture_has_frame(slot);
        IPC_OUT(c, v);
    }
}
static void dc_get_last_foreground(IpcObject *s, IpcCall *c) {
    (void)s;
    dc_get_slot(c, APPLET_CAP_LAST_FOREGROUND);
}
static void dc_get_last_application(IpcObject *s, IpcCall *c) {
    (void)s;
    dc_get_slot(c, APPLET_CAP_LAST_APPLICATION);
}
static void dc_get_caller_applet(IpcObject *s, IpcCall *c) {
    (void)s;
    dc_get_slot(c, APPLET_CAP_CALLER_APPLET);
}
static void dc_update_last_foreground(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    applet_capture_update(APPLET_CAP_LAST_FOREGROUND);
}
static void dc_update_caller_applet(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    applet_capture_update(APPLET_CAP_CALLER_APPLET);
}
/* TakeScreenShotOfOwnLayer: refreshes the application slot unless the guest forbade screenshots (SetScreenShotPermission). */
static void dc_take_own_layer(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    if (!applet_screenshot_allowed(AS())) {
        LOGV("applet", "TakeScreenShotOfOwnLayer: screenshots are disabled by the guest");
        return;
    }
    applet_capture_update(APPLET_CAP_LAST_APPLICATION);
}
/* CopyBetweenCaptureBuffers(dst, src): slot numbers as the capture buffer indices (0 foreground, 1 application, 2 caller); a bad index is an invalid argument. */
static void dc_copy_between(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!IN_OK(c, 4, uint32_t) || !applet_capture_copy(IPC_IN(c, uint32_t, 0), IPC_IN(c, uint32_t, 4))) c->result = 0x1d80;
}
static void dc_acquire_app_buffer(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!applet_capture_acquire_app_buffer()) c->result = 0x1480; /* already held */
}
static void dc_release_app_buffer(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    applet_capture_release_app_buffer();
}
static const IpcCmd dc_cmds[] = {
    {0, "GetLastForegroundCaptureImage", dc_get_last_foreground, IPC_IMPL},
    {1, "UpdateLastForegroundCaptureImage", dc_update_last_foreground, IPC_IMPL},
    {2, "GetLastApplicationCaptureImage", dc_get_last_application, IPC_IMPL},
    {5, "GetLastForegroundCaptureImageEx", dc_get_last_foreground, IPC_IMPL},
    {3, "GetCallerAppletCaptureImage", dc_get_caller_applet, IPC_IMPL},
    {4, "UpdateCallerAppletCaptureImage", dc_update_caller_applet, IPC_IMPL},
    {6, "GetLastApplicationCaptureImageEx", dc_get_last_application, IPC_IMPL},
    {7, "GetCallerAppletCaptureImageEx", dc_get_caller_applet, IPC_IMPL},
    {8, "TakeScreenShotOfOwnLayer", dc_take_own_layer, IPC_IMPL},
    {9, "CopyBetweenCaptureBuffers", dc_copy_between, IPC_IMPL},
    {10, "AcquireLastApplicationCaptureBuffer", dc_acquire_app_buffer, IPC_IMPL},
    {11, "ReleaseLastApplicationCaptureBuffer", dc_release_app_buffer, IPC_IMPL},
};

/* ---- IStorage / IStorageAccessor: launch parameters and library-applet storage ------------------------------------------------- */
typedef struct {
    uint8_t *data;
    uint64_t size;
    uint64_t gaddr;
} Storage; /* gaddr != 0: the bytes live in guest memory (transfer memory storage) */
static void storage_destroy(IpcObject *o) {
    Storage *st = o->state;
    if (st) {
        free(st->data);
        free(st);
    }
}
static const uint8_t *storage_bytes(const Storage *st) { return st->gaddr ? (const uint8_t *)(uintptr_t)st->gaddr : st->data; }
static void accessor_get_size(IpcObject *s, IpcCall *c) {
    Storage *st = s->state;
    int64_t v = (int64_t)st->size;
    IPC_OUT(c, v);
}
static void accessor_write(IpcObject *s, IpcCall *c) {
    Storage *st = s->state;
    int64_t off = IPC_IN(c, int64_t, 0);
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *p = b ? ipc_buf_ptr(b) : NULL;
    if (!p || off < 0 || (uint64_t)off + b->size > st->size) {
        c->result = 0x1d80;
        return;
    }
    memcpy((uint8_t *)storage_bytes(st) + off, p, b->size);
}
static void accessor_read(IpcObject *s, IpcCall *c) {
    Storage *st = s->state;
    int64_t off = IPC_IN(c, int64_t, 0);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || off < 0 || (uint64_t)off + b->size > st->size) {
        LOG("applet", "IStorageAccessor::Read: offset %lld size %llu outside the %llu-byte storage", (long long)off, b ? (unsigned long long)b->size : 0ull,
            (unsigned long long)st->size);
        c->result = 0x1d80;
        return;
    }
    ipc_write_buf(c, b, storage_bytes(st) + off, b->size);
}
static const IpcCmd accessor_cmds[] = {
    {0, "GetSize", accessor_get_size, IPC_IMPL}, {10, "Write", accessor_write, IPC_IMPL}, {11, "Read", accessor_read, IPC_IMPL}};
static IpcObject *storage_new(uint64_t size);
static void storage_open(IpcObject *s, IpcCall *c) {
    Storage *st = s->state;
    IpcObject *acc = IPC_OBJECT("IStorageAccessor", accessor_cmds, st, NULL); /* shares the storage: the accessor must not free it */
    ipc_out_object(c, acc);
}
static const IpcCmd storage_cmds[] = {{0, "Open", storage_open, IPC_IMPL}};
static IpcObject *storage_new(uint64_t size) {
    Storage *st = calloc(1, sizeof *st);
    st->size = size;
    st->data = calloc(1, size ? size : 1);
    return IPC_OBJECT("IStorage", storage_cmds, st, storage_destroy);
}

/* ---- ILibraryAppletCreator / ILibraryAppletAccessor: the software keyboard ------------------------------------------------------------
 * The only library applet an application needs offline in practice is swkbd (name entry); it is answered by the host's text bar
 * (frontend_text_input). Protocol as driven by nn::swkbd::ShowKeyboard in the SDK of Link's Awakening (SDK 8.x):
 *   CreateLibraryApplet(0x11, mode) -> PushInData(common args 0x10) -> PushInData(KeyboardConfig 0x4C8) -> PushInData(work buffer, a
 *   transfer-memory storage holding the initial text) -> Start -> wait GetAppletStateChangedEvent -> GetResult -> PopOutData.
 * Output storage: u32 CloseResult (0 enter, 1 cancel) followed by the UTF-16 text (the SDK reads 0x7D4 bytes of it).
 * KeyboardConfig fields used (from the SDK's own copy code, keyboardConfig2kbdConfig, and the public format): +4 ok text, +0x1c invalid
 * character flags, +0x24 header (65 u16), +0xA6 sub text (129 u16), +0x1A8 guide text (257 u16), +0x3AC max length, +0x3B0 min length,
 * +0x3B4 password mode, +0x3C0/+0x3C4 initial string offset (bytes) and length (characters) inside the work buffer. */
enum { APPLET_ID_SWKBD = LIBAPPLET_ID_SWKBD, APPLET_ID_CONTROLLER = LIBAPPLET_ID_CONTROLLER };
#define SWKBD_OUT_SIZE 0x800u
#define LA_MAX 8
typedef struct {
    uint32_t applet_id, mode;
    Storage in[LA_MAX], iin[LA_MAX], extra[LA_MAX]; /* pushed: PushInData, PushInteractiveInData, PushExtraStorage */
    unsigned nin, niin, nextra;
    Storage *outq[LA_MAX], *ioutq[LA_MAX]; /* answers: PopOutData / PopInteractiveOutData pop from the head */
    unsigned nout, out_head, niout, iout_head;
    int started, done;
    uint32_t result;
    uint32_t ev_state_w, ev_out_w, ev_iout_w;
    int unsupported;
    int exit_requested, terminated, gpu_slice_zero, foreground_requested;
    int kbd_active; /* the inline keyboard: running until the game finalizes it */
    AppletSwkbdInline kbd;
} LibApplet;
/* The live accessors (TerminateAllLibraryApplets / AreAnyLibraryAppletsLeft walk this list). */
static LibApplet *live_las[32];
static void la_register(LibApplet *a) {
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < 32; i++)
        if (!live_las[i]) {
            live_las[i] = a;
            break;
        }
    pthread_mutex_unlock(&lock);
}
static void la_unregister(LibApplet *a) {
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < 32; i++)
        if (live_las[i] == a) live_las[i] = NULL;
    pthread_mutex_unlock(&lock);
}
static void la_free_storage(Storage *st) {
    if (!st) return;
    free(st->data);
    free(st);
}
/* Queue an output storage the game pops; takes ownership of `o`. A full queue drops the oldest-unpopped newest answer (logged). */
static void la_push_out(LibApplet *a, Storage *o) {
    if (a->nout >= LA_MAX) {
        LOG("applet", "library applet %#x: output queue full, dropping a storage", a->applet_id);
        la_free_storage(o);
        return;
    }
    a->outq[a->nout++] = o;
}
static void la_push_iout(LibApplet *a, Storage *o) {
    if (a->niout >= LA_MAX) {
        LOG("applet", "library applet %#x: interactive output queue full, dropping a storage", a->applet_id);
        la_free_storage(o);
        return;
    }
    a->ioutq[a->niout++] = o;
    if (a->ev_iout_w) event_signal_handle(a->ev_iout_w);
}
static Storage *la_storage_from(const void *data, size_t size) {
    Storage *o = calloc(1, sizeof *o);
    o->size = size;
    o->data = calloc(1, size ? size : 1);
    if (data && size) memcpy(o->data, data, size);
    return o;
}
static void libapplet_destroy(IpcObject *o) {
    LibApplet *a = o->state;
    if (!a) return;
    la_unregister(a);
    for (unsigned i = 0; i < a->nin; i++) free(a->in[i].data);
    for (unsigned i = 0; i < a->niin; i++) free(a->iin[i].data);
    for (unsigned i = 0; i < a->nextra; i++) free(a->extra[i].data);
    for (unsigned i = a->out_head; i < a->nout; i++) la_free_storage(a->outq[i]);
    for (unsigned i = a->iout_head; i < a->niout; i++) la_free_storage(a->ioutq[i]);
    free(a);
}
static void utf16_to_utf8(const uint8_t *p, unsigned max_units, char *out, size_t cap) {
    uint16_t u[600];
    unsigned n = 0;
    for (; n < max_units && n < 599; n++) {
        memcpy(&u[n], p + 2 * n, 2);
        if (!u[n]) break;
    }
    napple_utf16_to_utf8(u, n, out, cap);
}
static void libapplet_run_swkbd(LibApplet *a) {
    const Storage *cfg = NULL, *wb = NULL;
    for (unsigned i = 0; i < a->nin; i++) {
        if (a->in[i].size >= 0x4C8 && !a->in[i].gaddr && !cfg) cfg = &a->in[i];
        else if (a->in[i].gaddr && !wb) wb = &a->in[i];
    }
    uint8_t blank[0x4C8] = {0};
    const uint8_t *k = cfg ? cfg->data : blank;
    char header[400] = "", sub[800] = "", guide[800] = "", initial[1500] = "", text[1500] = "";
    uint32_t max_len = 0, min_len = 0, password = 0, flags = 0, init_off = 0, init_len = 0;
    memcpy(&max_len, k + 0x3AC, 4);
    memcpy(&min_len, k + 0x3B0, 4);
    memcpy(&password, k + 0x3B4, 4);
    memcpy(&flags, k + 0x1C, 4);
    memcpy(&init_off, k + 0x3C0, 4);
    memcpy(&init_len, k + 0x3C4, 4);
    utf16_to_utf8(k + 0x24, 65, header, sizeof header);
    utf16_to_utf8(k + 0xA6, 129, sub, sizeof sub);
    utf16_to_utf8(k + 0x1A8, 257, guide, sizeof guide);
    if (wb && init_len && init_len < 600 && (uint64_t)init_off + 2ull * init_len <= wb->size)
        utf16_to_utf8(storage_bytes(wb) + init_off, init_len, initial, sizeof initial);
    if (max_len > 500) max_len = 500;
    LOG("applet", "swkbd: header \"%s\" sub \"%s\" guide \"%s\" initial \"%s\" length %u..%u password %u invalid-flags %#x (config %s, work buffer %s)", header,
        sub, guide, initial, min_len, max_len, password, flags, cfg ? "ok" : "missing", wb ? "ok" : "missing");
    {
        char hx[200];
        int n = 0;
        for (unsigned i = 0x3AC; i < 0x3E0 && n < 190; i++) n += snprintf(hx + n, sizeof hx - (size_t)n, "%02x%s", k[i], (i & 3) == 3 ? " " : "");
        LOGV("applet", "swkbd config +0x3ac: %s; +0x4b0: %02x%02x%02x%02x %02x%02x%02x%02x", hx, k[0x4b0], k[0x4b1], k[0x4b2], k[0x4b3], k[0x4b4], k[0x4b5],
             k[0x4b6], k[0x4b7]);
    }
    AppletSwkbdConfig sc;
    applet_swkbd_parse(cfg ? cfg->data : NULL, cfg ? cfg->size : 0, &sc);
    int ok = frontend_text_input(header, sub, guide, initial, max_len, min_len, password != 0, sc.numeric, text, sizeof text);
    if (ok) { /* the applet enforces the limits itself: over-long text is cut, too-short text cannot be confirmed (headless: a cancel) */
        char fit[1500];
        int fits = applet_swkbd_fit(&sc, text, fit, sizeof fit);
        if (strcmp(fit, text)) LOG("applet", "swkbd: text cut to the %u character limit", sc.max_len);
        snprintf(text, sizeof text, "%s", fit);
        if (!fits) {
            LOG("applet", "swkbd: text shorter than the minimum %u: treated as cancel", sc.min_len);
            ok = 0;
        }
    }
    Storage *o = calloc(1, sizeof *o);
    o->size = SWKBD_OUT_SIZE;
    o->data = calloc(1, SWKBD_OUT_SIZE);
    uint32_t close = ok ? 0 : 1;
    memcpy(o->data, &close, 4);
    /* Encoding of the returned text: Link's Awakening (and so the SDK path it uses: use-UTF-8 byte of the config is 0) reads it as UTF-8 (verified: the
     * name shows correctly; UTF-16 units come out as a single "L"). NAPPLE_SWKBD_UTF16=1 switches to UTF-16 for titles that need it. */
    if (ok) {
        if (!getenv("NAPPLE_SWKBD_UTF16") && g_applet_kind != 3) strlcpy((char *)o->data + 4, text, SWKBD_OUT_SIZE - 4);
        else {
            uint16_t u16[600];
            size_t n = napple_utf8_to_utf16(text, u16, 599);
            memcpy(o->data + 4, u16, n * 2);
        }
    }
    la_push_out(a, o);
    a->result = 0;
    LOG("applet", "swkbd: %s%s%s", ok ? "entered \"" : "cancelled", ok ? text : "", ok ? "\"" : "");
}
/* Controller support applet (id 0xc, libnx hidShowControllerSupport / nn::hid::ShowControllerSupport): the guest pushes the common args, the private
 * header (0x14) and ControllerSupportArg, then reads ControllerSupportResultInfo from the output storage. The answer is applet_controller_answer
 * (applet_lib_core.c): the user confirmed with the one virtual pad. */
static void libapplet_run_controller(LibApplet *a) {
    const uint8_t *arg = NULL;
    size_t arg_size = 0;
    /* ControllerSupportArg is the large storage (0x21C-ish for 4 players, 0x42C-ish for 8: header, colours, explain texts); the 0x20-byte
     * storage is the CommonArguments (version, size, ...), whose first bytes are not player counts. */
    for (unsigned i = 0; i < a->nin; i++) {
        LOG("applet", "controller support: input storage %u size %#llx%s", i, (unsigned long long)a->in[i].size, a->in[i].gaddr ? " (transfer memory)" : "");
        if (a->in[i].size >= 0x100 && !a->in[i].gaddr && a->in[i].data) {
            arg = a->in[i].data;
            arg_size = a->in[i].size;
        }
    }
    AppletControllerAnswer ans;
    applet_controller_answer(arg, arg_size, frontend_connected_pads(), &ans);
    Storage *o = calloc(1, sizeof *o);
    o->size = APPLET_CONTROLLER_OUT_SIZE;
    o->data = calloc(1, APPLET_CONTROLLER_OUT_SIZE);
    uint32_t cres;
    const char *cspec = getenv("NAPPLE_CONTROLLER_SUPPORT");
    int ui = cspec && *cspec ? -2 : applet_controller_ask(&ans); /* scripted answers win over the sheet */
    applet_controller_apply_policy(cspec, &ans, &cres);
    if (ui == 0) {
        cres = APPLET_CONTROLLER_CANCELLED;
        ans.players = 0;
        ans.selected_id = 0xffffffffu;
    }
    applet_controller_encode_result(&ans, cres, o->data);
    la_push_out(a, o);
    a->result = 0;
    LOG("applet", "controller support: %s with %d player(s) of %d connected (game asked %u..%u%s%s), selected id %#x", cres ? "cancelled" : "confirmed",
        ans.players, ans.connected, ans.min, ans.max, ans.single_mode ? ", single mode" : "", ans.short_of_min ? ", fewer pads than asked" : "",
        ans.selected_id);
}
/* Error applet (id 0xe, libnx errorCodeShow / errorResultShow / the SDK's error viewer): nothing to read back; the guest waits for it to end. The
 * headless answer is "the user dismissed it" and the request is logged and noted in the run summary, since a game that shows an error screen
 * is the most important thing for an unattended run to report. A jump-to-settings request is not followed. */
static void libapplet_run_error(LibApplet *a) {
    const uint8_t *hdr = NULL, *arg = NULL;
    size_t hn = 0, an = 0;
    for (unsigned i = 0; i < a->nin; i++) {
        if (a->in[i].gaddr || !a->in[i].data) continue;
        if (a->in[i].size <= 0x20 && !hdr && i > 0) {
            hdr = a->in[i].data;
            hn = a->in[i].size;
        } else if (a->in[i].size > 0x20 && !arg) {
            arg = a->in[i].data;
            an = a->in[i].size;
        }
    }
    AppletErrorInfo e;
    char d[160];
    applet_error_parse(hdr, hn, arg, an, &e);
    applet_error_describe(&e, d, sizeof d);
    char code[32];
    applet_error_code_text(&e, code, sizeof code);
    LOG("applet", "error applet: %s%s%s (dismissed)", d, code[0] ? ", code " : "", code);
    if (!applet_error_notify(&e, code, d)) { /* no frontend hook registered: the generic alert */
        char msg[300];
        snprintf(msg, sizeof msg, "The game reported an error%s%s.\n%s", code[0] ? " " : "", code, d);
        frontend_alert("Napple: error applet", msg);
    }
    summary_note("the guest showed the error applet (%s)", d);
    a->result = 0;
}
/* Player select (id 0x10, libnx playerSelectUiShow): answered with the one profile, g_user_id (applet_lib_core.c). */
extern const uint8_t g_user_id[16];
static void libapplet_run_player_select(LibApplet *a) {
    for (unsigned i = 0; i < a->nin; i++) LOG("applet", "player select: input storage %u size %#llx", i, (unsigned long long)a->in[i].size);
    Storage *o = calloc(1, sizeof *o);
    o->size = APPLET_PLAYER_SELECT_OUT_SIZE;
    o->data = calloc(1, APPLET_PLAYER_SELECT_OUT_SIZE);
    /* NAPPLE_PLAYER_SELECT = <index> | cancel. The profile list is the account service's (it registers applet_set_profile_lister); without one the local
     * user is the only profile. */
    uint8_t tab[8][16];
    unsigned np = applet_profiles(tab, 8, g_user_id);
    int index;
    const char *pspec = getenv("NAPPLE_PLAYER_SELECT");
    int cancel = applet_player_select_spec(pspec, &index);
    if (!(pspec && *pspec)) {
        int r = applet_player_select_ask((const uint8_t(*)[16])tab, np);
        if (r == -1) cancel = 1;
        else if (r >= 0) index = r;
    }
    const uint8_t *uid = applet_player_select_pick((const uint8_t(*)[16])tab, np, index, cancel);
    applet_player_select_answer(uid, uid == NULL, o->data);
    la_push_out(a, o);
    a->result = 0;
    if (uid) LOG("applet", "player select: confirmed with profile %d of %u", index >= 0 && (unsigned)index < np ? index : 0, np);
    else LOG("applet", "player select: cancelled");
}
/* ---- the firmware applet host (parent side) --------------------------------------------------------------------------------------------------
 * A library applet that has a program in the user's firmware (the Mii Maker, applet id 0x12) can run for real: `napple build` wrote it beside the game as
 * <title dir>/applets/<program id>/<build id>/ (a data directory like any title's). The game's guest stays blocked in Start; this process starts
 * `napple-runtime <that directory>` as a child, the pushed storages go to it as files (NAPPLE_APPLET_IN/in<N>.bin), the child's PushOutData storages come back
 * (NAPPLE_APPLET_OUT/out<N>.bin) when it exits, and the game then pops them like any applet's output. On by default (NAPPLE_FIRMWARE_APPLETS=0 turns it off); a missing applet, a failing child or the timeout (NAPPLE_APPLET_TIMEOUT seconds, default 600) answer as cancelled. See docs/archive/changes/2026-10-26-mii-firmware.md. */
#include <dirent.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static int write_file(const char *path, const void *p, uint64_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = n == 0 || fwrite(p, 1, (size_t)n, f) == (size_t)n;
    return fclose(f) == 0 && ok ? 0 : -1;
}
static int find_applet_dir(const char *program_id, char *out, size_t cap) {
    char base[1300];
    snprintf(base, sizeof base, "%s/../applets/%s", g_cfg.data_dir, program_id);
    DIR *d = opendir(base);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char m[1500];
        snprintf(m, sizeof m, "%s/%s/manifest.json", base, e->d_name);
        if (!access(m, R_OK)) {
            snprintf(out, cap, "%s/%s", base, e->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
}
static void remove_run_dir(const char *work, const char *in_dir, const char *out_dir) {
    char p[1500];
    for (unsigned i = 0; i < 16; i++) {
        snprintf(p, sizeof p, "%s/in%u.bin", in_dir, i);
        remove(p);
        snprintf(p, sizeof p, "%s/out%u.bin", out_dir, i);
        remove(p);
        snprintf(p, sizeof p, "%s/iin%u.bin", in_dir, i);
        remove(p);
        snprintf(p, sizeof p, "%s/extra%u.bin", in_dir, i);
        remove(p);
        snprintf(p, sizeof p, "%s/iout%u.bin", out_dir, i);
        remove(p);
    }
    snprintf(p, sizeof p, "%s/result.txt", out_dir);
    remove(p);
    rmdir(in_dir);
    rmdir(out_dir);
    rmdir(work);
}
/* Returns 1 when the child ran to completion (output storages queued on the accessor, or its result file written), 0 otherwise. */
/* In-process mode (iOS always, macOS with NAPPLE_APPLET_INPROC=1): iOS cannot spawn a process, so the applet runs as a second copy of this runtime, loaded from
 * libnapple-applet.dylib (make applet-lib; NAPPLE_APPLET_LIB overrides the path, default: beside the executable, then ../Frameworks) with its own globals,
 * on a thread of this process. Same storage-file protocol as the child process. The applet's guest ending returns napple_applet_main (src/libmode.c). */
#include <dlfcn.h>
typedef struct {
    int (*entry)(int, char **);
    int argc;
    char **argv;
    int code;
} InprocRun;
static void *inproc_thread(void *p) {
    InprocRun *r = p;
    r->code = r->entry(r->argc, r->argv);
    return NULL;
}
static void *inproc_open_lib(void) {
    static void *h;
    if (h) return h;
    char path[2048];
    const char *env = getenv("NAPPLE_APPLET_LIB");
    if (env && *env) snprintf(path, sizeof path, "%s", env);
    else {
        char exe[2048];
        uint32_t sz = sizeof exe;
        if (_NSGetExecutablePath(exe, &sz)) return NULL;
        char *slash = strrchr(exe, '/');
        if (slash) *slash = 0;
        snprintf(path, sizeof path, "%s/libnapple-applet.dylib", exe);
        if (access(path, R_OK)) snprintf(path, sizeof path, "%s/Frameworks/libnapple-applet.dylib", exe);
        if (access(path, R_OK)) snprintf(path, sizeof path, "%s/../Frameworks/libnapple-applet.dylib", exe);
        if (access(path, R_OK)) snprintf(path, sizeof path, "%s/../build-lib/libnapple-applet.dylib", exe); /* dev tree: runtime/build/napple-runtime */
        if (access(path, R_OK)) snprintf(path, sizeof path, "%s/../../runtime/build-lib/libnapple-applet.dylib", exe);
    }
    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) LOG("applet", "in-process applet host: cannot load %s: %s", path, dlerror());
    return h;
}
static int inproc_run(const char *dir) {
    void *h = inproc_open_lib();
    if (!h) return 0;
    InprocRun r = {.entry = dlsym(h, "napple_applet_main")};
    if (!r.entry) return 0;
    void (*attach)(void *, void *) = dlsym(h, "napple_applet_attach");
    if (attach) { attach(g_cfg.headless ? NULL : frontend_metal_layer(), hid_input_get()); }
    char *argv[10];
    int n = 0;
    argv[n++] = (char *)"napple-applet";
    argv[n++] = (char *)dir;
    argv[n++] = (char *)"--save-root";
    argv[n++] = (char *)g_cfg.save_root;
    if (g_cfg.headless) argv[n++] = (char *)"--headless";
    argv[n] = NULL;
    r.argc = n;
    r.argv = argv;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t t;
    if (pthread_create(&t, &at, inproc_thread, &r)) return 0;
    pthread_join(t, NULL);
    LOG("applet", "in-process applet returned %d", r.code);
    return 1;
}
static int libapplet_run_firmware(LibApplet *a, const char *program_id) {
    const char *on = getenv("NAPPLE_FIRMWARE_APPLETS");
    char dir[1500];
    if (on && !strcmp(on, "0")) return 0; /* on by default; NAPPLE_FIRMWARE_APPLETS=0 turns it off */
    if (!find_applet_dir(program_id, dir, sizeof dir)) {
        LOG("applet", "firmware applet %s: not built beside this game (napple build --firmware writes it)", program_id);
        return 0;
    }
    char work[1400], in_dir[1450], out_dir[1450], path[1500], exe[2048];
    snprintf(work, sizeof work, "%s/system/applet-run-%d", napple_support_dir(), (int)getpid());
    snprintf(in_dir, sizeof in_dir, "%s/in", work);
    snprintf(out_dir, sizeof out_dir, "%s/out", work);
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s/system", napple_support_dir());
    mkdir(tmp, 0755);
    mkdir(work, 0755);
    mkdir(in_dir, 0755);
    mkdir(out_dir, 0755);
    for (unsigned i = 0; i < a->nin; i++) {
        snprintf(path, sizeof path, "%s/in%u.bin", in_dir, i);
        if (write_file(path, storage_bytes(&a->in[i]), a->in[i].size)) {
            LOG("applet", "firmware applet: cannot write %s", path);
            return 0;
        }
    }
    for (unsigned i = 0; i < a->niin; i++) { /* the interactive channel: popped by PopInteractiveInData of the applet */
        snprintf(path, sizeof path, "%s/iin%u.bin", in_dir, i);
        if (write_file(path, storage_bytes(&a->iin[i]), a->iin[i].size)) {
            LOG("applet", "firmware applet: cannot write %s", path);
            return 0;
        }
    }
    for (unsigned i = 0; i < a->nextra; i++) {
        snprintf(path, sizeof path, "%s/extra%u.bin", in_dir, i);
        if (write_file(path, storage_bytes(&a->extra[i]), a->extra[i].size)) {
            LOG("applet", "firmware applet: cannot write %s", path);
            return 0;
        }
    }
    int inproc = NAPPLE_IOS || (getenv("NAPPLE_APPLET_INPROC") && !getenv("NAPPLE_APPLET_EXE"));
    const char *exe_override = getenv("NAPPLE_APPLET_EXE"); /* tests run a stand-in program */
    if (exe_override) snprintf(exe, sizeof exe, "%s", exe_override);
    else {
#ifdef __APPLE__
        uint32_t sz = sizeof exe;
        if (_NSGetExecutablePath(exe, &sz)) return 0;
#else
        ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (len <= 0) return 0;
        exe[len] = 0;
#endif
    }
    char idbuf[32], modebuf[32], envin[1600], envout[1600];
    snprintf(idbuf, sizeof idbuf, "NAPPLE_APPLET_ID=%#x", a->applet_id);
    snprintf(modebuf, sizeof modebuf, "NAPPLE_APPLET_MODE=%u", a->mode);
    snprintf(envin, sizeof envin, "NAPPLE_APPLET_IN=%s", in_dir);
    snprintf(envout, sizeof envout, "NAPPLE_APPLET_OUT=%s", out_dir);
    applet_set_focus(0);
    if (inproc) {
        setenv("NAPPLE_APPLET_ID", idbuf + 17, 1);
        setenv("NAPPLE_APPLET_MODE", modebuf + 19, 1);
        setenv("NAPPLE_APPLET_IN", in_dir, 1);
        setenv("NAPPLE_APPLET_OUT", out_dir, 1);
        LOG("applet", "firmware applet %s: running in-process (storages in %s)", program_id, in_dir);
        int ran = inproc_run(dir);
        unsetenv("NAPPLE_APPLET_ID");
        unsetenv("NAPPLE_APPLET_MODE");
        unsetenv("NAPPLE_APPLET_IN");
        unsetenv("NAPPLE_APPLET_OUT");
        if (!ran) {
            remove_run_dir(work, in_dir, out_dir);
            applet_set_focus(1);
            return 0;
        }
        goto collect;
    }
    size_t ne = 0;
    while (environ[ne]) ne++;
    char **envp = calloc(ne + 6, sizeof *envp);
    size_t k = 0;
    for (size_t i = 0; i < ne; i++)
        if (strncmp(environ[i], "NAPPLE_DEBUG_LAUNCH_APPLET=", 27)) envp[k++] = environ[i]; /* the debug launch is the host's, not the child's */
    ne = k;
    envp[ne++] = idbuf;
    envp[ne++] = modebuf;
    envp[ne++] = envin;
    envp[ne++] = envout;
    char parentbuf[48];
    snprintf(parentbuf, sizeof parentbuf, "NAPPLE_APPLET_PARENT=%d", (int)getpid());
    envp[ne++] = parentbuf; /* the child exits when this process does (main.c applet_parent_watch) */
    char *argv[8];
    int n = 0;
    char seconds[16] = "0";
    argv[n++] = exe;
    argv[n++] = dir;
    argv[n++] = (char *)"--save-root";
    argv[n++] = (char *)g_cfg.save_root;
    if (g_cfg.headless) argv[n++] = (char *)"--headless";
    /* Scripted exit for tests (tools/e2e): the child ends itself after N seconds, as if the player had left the editor. */
    const char *sx = getenv("NAPPLE_APPLET_SECONDS");
    if (sx && atoi(sx) > 0) {
        snprintf(seconds, sizeof seconds, "%d", atoi(sx));
        argv[n++] = (char *)"--seconds";
        argv[n++] = seconds;
    }
    argv[n] = NULL;
    pid_t pid;
    LOG("applet", "firmware applet %s: starting %s (storages in %s)", program_id, dir, in_dir);
    int rc = posix_spawn(&pid, exe, NULL, NULL, argv, envp);
    free(envp);
    if (rc) {
        LOG("applet", "firmware applet: cannot start the child (%d)", rc);
        applet_set_focus(1);
        return 0;
    }
    const char *to = getenv("NAPPLE_APPLET_TIMEOUT");
    long limit = to ? atol(to) : 600;
    time_t t0 = time(NULL);
    int status = 0, done = 0;
    while (!done) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) done = 1;
        else if (time(NULL) - t0 > limit) {
            LOG("applet", "firmware applet: no answer after %ld s, terminating it", limit);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            applet_set_focus(1);
            return 0;
        } else usleep(50000);
    }
    LOG("applet", "firmware applet exited (status %d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
collect:
    applet_set_focus(1);
    /* Everything the applet pushed: out<N>.bin / iout<N>.bin in order, and result.txt (written by ExitProcessAndReturn: the exit result). An applet that
     * answered nothing and left no result (a crash, a kill) is a failed run: the caller falls back to the built-in answer. */
    unsigned nfound = 0, nifound = 0;
    int have_result = 0;
    uint32_t exit_result = 0;
    for (unsigned i = 0; i < LA_MAX; i++) {
        snprintf(path, sizeof path, "%s/out%u.bin", out_dir, i);
        FILE *f = fopen(path, "rb");
        if (!f) break;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        Storage *o = calloc(1, sizeof *o);
        o->size = len > 0 ? (uint64_t)len : 0;
        o->data = calloc(1, o->size ? o->size : 1);
        int ok = o->size == 0 || fread(o->data, 1, o->size, f) == o->size;
        fclose(f);
        if (!ok) la_free_storage(o);
        else {
            la_push_out(a, o);
            nfound++;
        }
    }
    for (unsigned i = 0; i < LA_MAX; i++) {
        snprintf(path, sizeof path, "%s/iout%u.bin", out_dir, i);
        FILE *f = fopen(path, "rb");
        if (!f) break;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        Storage *o = calloc(1, sizeof *o);
        o->size = len > 0 ? (uint64_t)len : 0;
        o->data = calloc(1, o->size ? o->size : 1);
        int ok = o->size == 0 || fread(o->data, 1, o->size, f) == o->size;
        fclose(f);
        if (!ok) la_free_storage(o);
        else {
            la_push_iout(a, o);
            nifound++;
        }
    }
    snprintf(path, sizeof path, "%s/result.txt", out_dir);
    FILE *rf = fopen(path, "r");
    if (rf) {
        unsigned long v = 0;
        have_result = 1;
        if (fscanf(rf, "%lu", &v) == 1) exit_result = (uint32_t)v;
        fclose(rf);
    }
    remove_run_dir(work, in_dir, out_dir);
    if (!nfound && !have_result) return 0;
    a->result = exit_result;
    LOG("applet", "firmware applet %s: %u output storage(s), %u interactive, result %#x", program_id, nfound, nifound, exit_result);
    return 1;
}
static void la_state_event(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    uint32_t r = 0;
    a->ev_state_w = event_create(&r);
    if (a->done) event_signal_handle(a->ev_state_w);
    ipc_out_copy_handle(c, r);
}
static void la_is_completed(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    uint8_t v = a->done;
    IPC_OUT(c, v);
}
/* A handler registered with libapplet_register_handler (applet_lib_core.h) answers before the built-in applets. */
static int libapplet_run_registered(LibApplet *a) {
    if (!libapplet_find_handler(a->applet_id)) return 0;
    LibAppletCall call;
    memset(&call, 0, sizeof call);
    call.applet_id = a->applet_id;
    call.mode = a->mode;
    for (unsigned i = 0; i < a->nin && i < LIBAPPLET_MAX_IN; i++) {
        call.in[call.nin].data = a->in[i].gaddr ? NULL : a->in[i].data;
        call.in[call.nin].size = a->in[i].size;
        call.in[call.nin++].transfer_memory = a->in[i].gaddr != 0;
    }
    for (unsigned i = 0; i < a->niin && i < LIBAPPLET_MAX_IN; i++) {
        call.interactive_in[call.ninteractive_in].data = a->iin[i].gaddr ? NULL : a->iin[i].data;
        call.interactive_in[call.ninteractive_in].size = a->iin[i].size;
        call.interactive_in[call.ninteractive_in++].transfer_memory = a->iin[i].gaddr != 0;
    }
    if (!libapplet_dispatch(&call)) {
        libapplet_call_free(&call);
        return 0;
    }
    if (call.out) {
        Storage *o = calloc(1, sizeof *o);
        o->size = call.out_size;
        o->data = call.out;
        la_push_out(a, o);
        call.out = NULL;
    }
    for (unsigned i = 0; i < call.ninteractive_out; i++) {
        Storage *o = calloc(1, sizeof *o);
        o->size = call.interactive_out_size[i];
        o->data = call.interactive_out[i];
        la_push_iout(a, o);
    }
    call.ninteractive_out = 0;
    a->result = call.result;
    a->unsupported = call.unsupported;
    LOG("applet", "library applet %#x answered by a registered handler", a->applet_id);
    return 1;
}
/* The library applet is finished: wake everything waiting on it. */
static void la_finish(LibApplet *a) {
    a->done = 1;
    a->kbd_active = 0;
    if (a->ev_state_w) event_signal_handle(a->ev_state_w);
    if (a->ev_out_w && a->out_head < a->nout) event_signal_handle(a->ev_out_w);
}
/* Software keyboard, inline mode (the game started it in a background mode): Start parses the config, answers FinishedInitialize and leaves the keyboard
 * running; requests arrive with PushInteractiveInData (la_push_interactive) and the replies are popped with PopInteractiveOutData. */
static void libapplet_start_swkbd_inline(LibApplet *a) {
    const Storage *cfg = NULL;
    for (unsigned i = 0; i < a->nin; i++)
        if (a->in[i].size >= 0x4C8 && !a->in[i].gaddr && !cfg) cfg = &a->in[i];
    AppletSwkbdConfig sc;
    applet_swkbd_parse(cfg ? cfg->data : NULL, cfg ? cfg->size : 0, &sc);
    applet_swkbd_inline_init(&a->kbd, &sc);
    a->kbd.initialized = 1;
    a->kbd_active = 1;
    LibAppletCall call;
    memset(&call, 0, sizeof call);
    applet_swkbd_inline_reply(&call, APPLET_SWKBD_REPLY_FINISHED_INITIALIZE, NULL);
    for (unsigned i = 0; i < call.ninteractive_out; i++) la_push_iout(a, la_storage_from(call.interactive_out[i], call.interactive_out_size[i]));
    libapplet_call_free(&call);
    LOG("applet", "swkbd inline: started (mode %u, length %u..%u), waiting for requests", a->mode, sc.min_len, sc.max_len);
}
static void la_start(IpcObject *s, IpcCall *c) {
    (void)c;
    LibApplet *a = s->state;
    a->started = 1;
    const LibAppletProgram *prog = libapplet_program_for(a->applet_id);
    int fw = prog && libapplet_firmware_enabled(prog, getenv("NAPPLE_FIRMWARE_APPLETS"), getenv("NAPPLE_FIRMWARE_APPLET_IDS"));
    if (libapplet_run_registered(a)) {
    } else if (fw && libapplet_run_firmware(a, prog->program_id)) { /* the applet of the user's firmware, run for real (Mii Maker by default) */
    } else if (a->applet_id == APPLET_ID_SWKBD && libapplet_mode_is_inline(a->mode)) {
        libapplet_start_swkbd_inline(a);
        return; /* still running: finished by the Finalize request */
    } else if (a->applet_id == APPLET_ID_SWKBD) libapplet_run_swkbd(a);
    else if (a->applet_id == APPLET_ID_CONTROLLER) libapplet_run_controller(a);
    else if (a->applet_id == LIBAPPLET_ID_ERROR) libapplet_run_error(a);
    else if (a->applet_id == LIBAPPLET_ID_PLAYER_SELECT) libapplet_run_player_select(a);
    else { /* unknown library applet: the guest sees it terminate without output (as if the user cancelled), instead of the process dying */
        LOG("applet", "library applet %#x is not implemented: reporting it as cancelled", a->applet_id);
        a->unsupported = 1;
        a->result = 0x1480;
    }
    la_finish(a);
}
static void la_get_result(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    if (a && a->result) c->result = a->result;
}
/* RequestExit: the caller asks the applet to leave; an applet still running (the inline keyboard) ends at once, a finished one ignores it.
 * Terminate: the applet is killed; its pending answers stay poppable. */
static void la_request_exit(IpcObject *s, IpcCall *c) {
    (void)c;
    LibApplet *a = s->state;
    a->exit_requested = 1;
    if (a->started && !a->done) la_finish(a);
}
static void la_terminate(IpcObject *s, IpcCall *c) {
    (void)c;
    LibApplet *a = s->state;
    a->terminated = 1;
    if (a->started && !a->done) la_finish(a);
}
static void la_preset_gpu_zero(IpcObject *s, IpcCall *c) {
    (void)c;
    ((LibApplet *)s->state)->gpu_slice_zero = 1;
}
static void la_request_foreground(IpcObject *s, IpcCall *c) {
    (void)c;
    ((LibApplet *)s->state)->foreground_requested = 1;
}
static Storage *la_copy_in(IpcCall *c) {
    if (!c->nin_objects || !c->in_objects[0]) return NULL;
    const Storage *src = c->in_objects[0]->state;
    Storage *d = calloc(1, sizeof *d);
    *d = *src;
    if (!src->gaddr) {
        d->data = malloc(src->size ? src->size : 1);
        memcpy(d->data, src->data, src->size);
    } else d->data = NULL;
    return d;
}
static void la_push_in(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    Storage *d = la_copy_in(c);
    if (!d || a->nin >= LA_MAX) {
        if (d) {
            free(d->data);
            free(d);
        }
        c->result = 0x1480;
        return;
    }
    a->in[a->nin++] = *d;
    LOGV("applet", "library applet %#x: PushInData %llu bytes (%s)", a->applet_id, (unsigned long long)d->size, d->gaddr ? "transfer memory" : "data");
    free(d);
}
static void la_push_extra(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    Storage *d = la_copy_in(c);
    if (!d || a->nextra >= LA_MAX) {
        if (d) {
            free(d->data);
            free(d);
        }
        c->result = 0x1480;
        return;
    }
    a->extra[a->nextra++] = *d;
    free(d);
}
/* PushInteractiveInData: a request to the running applet. The inline keyboard answers at once (replies queue for PopInteractiveOutData); a firmware applet
 * gets it as iin<N>.bin when it starts (the host runs it to completion inside Start, so the channel is one-shot there); the other built-in applets
 * ignore it. */
static void la_push_interactive(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    Storage *d = la_copy_in(c);
    if (!d) {
        c->result = 0x1480;
        return;
    }
    if (a->kbd_active) {
        LibAppletCall call;
        memset(&call, 0, sizeof call);
        char text[1500] = "";
        uint32_t cmd = d->size >= 4 && d->data ? *(uint32_t *)d->data : 0;
        int typed = 0;
        if (cmd == APPLET_SWKBD_CMD_CALC) /* the keyboard appears: the user types through the host text bar (or the headless script) */
            typed = frontend_text_input("", "", "", "", a->kbd.cfg.max_len, a->kbd.cfg.min_len, a->kbd.cfg.password, a->kbd.cfg.numeric, text, sizeof text);
        applet_swkbd_inline_request(&a->kbd, d->data, d->size, typed ? text : NULL, &call);
        for (unsigned i = 0; i < call.ninteractive_out; i++) la_push_iout(a, la_storage_from(call.interactive_out[i], call.interactive_out_size[i]));
        libapplet_call_free(&call);
        LOGV("applet", "swkbd inline: request %#x (%s)", cmd, typed ? "text entered" : "no text");
        if (a->kbd.finalized) la_finish(a);
        free(d->data);
        free(d);
        return;
    }
    if (a->niin >= LA_MAX) {
        free(d->data);
        free(d);
        c->result = 0x1480;
        return;
    }
    a->iin[a->niin++] = *d;
    free(d);
}
static void la_pop_out(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    if (a->out_head >= a->nout) {
        c->result = 0x680;
        return;
    }
    Storage *src = a->outq[a->out_head++];
    IpcObject *o = storage_new(src->size);
    Storage *st = o->state;
    memcpy(st->data, src->data, src->size);
    la_free_storage(src);
    ipc_out_object(c, o);
}
static void la_pop_interactive_out(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    if (a->iout_head >= a->niout) {
        c->result = 0x680;
        return;
    }
    Storage *src = a->ioutq[a->iout_head++];
    IpcObject *o = storage_new(src->size);
    Storage *st = o->state;
    memcpy(st->data, src->data, src->size);
    la_free_storage(src);
    if (a->iout_head >= a->niout && a->ev_iout_w) event_clear_handle(a->ev_iout_w);
    ipc_out_object(c, o);
}
static void la_pop_out_event(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    uint32_t r = 0;
    a->ev_out_w = event_create(&r);
    if (a->out_head < a->nout) event_signal_handle(a->ev_out_w);
    ipc_out_copy_handle(c, r);
}
static void la_pop_interactive_out_event(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    uint32_t r = 0;
    a->ev_iout_w = event_create(&r);
    if (a->iout_head < a->niout) event_signal_handle(a->ev_iout_w);
    ipc_out_copy_handle(c, r);
}
static void la_needs_exit(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    uint8_t v = (uint8_t)(a->exit_requested || a->terminated);
    IPC_OUT(c, v);
}
static void la_info(IpcObject *s, IpcCall *c) {
    LibApplet *a = s->state;
    uint32_t v[2] = {a->applet_id, a->mode};
    ipc_out(c, v, 8);
}
static const IpcCmd la_cmds[] = {
    {0, "GetAppletStateChangedEvent", la_state_event, IPC_IMPL},
    {1, "IsCompleted", la_is_completed, IPC_IMPL},
    {10, "Start", la_start, IPC_IMPL},
    {20, "RequestExit", la_request_exit, IPC_IMPL},
    {25, "Terminate", la_terminate, IPC_IMPL},
    {30, "GetResult", la_get_result, IPC_IMPL},
    {50, "PresetLibraryAppletGpuTimeSliceZero", la_preset_gpu_zero, IPC_IMPL},
    {100, "PushInData", la_push_in, IPC_IMPL},
    {101, "PopOutData", la_pop_out, IPC_IMPL},
    {102, "PushExtraStorage", la_push_extra, IPC_IMPL},
    {103, "PushInteractiveInData", la_push_interactive, IPC_IMPL},
    {104, "PopInteractiveOutData", la_pop_interactive_out, IPC_IMPL},
    {105, "GetPopOutDataEvent", la_pop_out_event, IPC_IMPL},
    {106, "GetPopInteractiveOutDataEvent", la_pop_interactive_out_event, IPC_IMPL},
    {110, "NeedsToExitProcess", la_needs_exit, IPC_IMPL},
    {120, "GetLibraryAppletInfo", la_info, IPC_IMPL},
    {150, "RequestForAppletToGetForeground", la_request_foreground, IPC_IMPL},
};
static void lac_create_library_applet(IpcObject *s, IpcCall *c) {
    (void)s;
    LibApplet *a = calloc(1, sizeof *a);
    a->applet_id = IPC_IN(c, uint32_t, 0);
    a->mode = IPC_IN(c, uint32_t, 4);
    LOG("applet", "CreateLibraryApplet id %#x mode %u", a->applet_id, a->mode);
    if (getenv("NAPPLE_APPLET_TRACE")) {
        extern void ipc_trace_guest(unsigned);
        ipc_trace_guest(10);
    }
    la_register(a);
    ipc_out_object(c, IPC_OBJECT("ILibraryAppletAccessor", la_cmds, a, libapplet_destroy));
}
static void lac_create_storage(IpcObject *s, IpcCall *c) {
    (void)s;
    int64_t size = IPC_IN(c, int64_t, 0);
    ipc_out_object(c, storage_new(size < 0 ? 0 : (uint64_t)size));
}
/* CreateTransferMemoryStorage(bool writable, i64 size, handle transfer memory): the storage is a window on guest memory. */
static void lac_create_tmem_storage(IpcObject *s, IpcCall *c) {
    (void)s;
    int64_t size = IPC_IN(c, int64_t, 8);
    IpcTransferMemory *t = c->ncopy ? handle_get(c->copy[0], OBJ_TMEM) : NULL;
    if (!t || size < 0 || (uint64_t)size > t->size) {
        c->result = 0x1480;
        return;
    }
    IpcObject *o = storage_new(0);
    Storage *st = o->state;
    st->size = (uint64_t)size;
    st->gaddr = t->addr;
    ipc_out_object(c, o);
}
/* AreAnyLibraryAppletsLeft: some accessor was started and has not finished (only the inline keyboard stays running past Start). */
static void lac_are_any_left(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 0;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < 32; i++)
        if (live_las[i] && live_las[i]->started && !live_las[i]->done) v = 1;
    pthread_mutex_unlock(&lock);
    IPC_OUT(c, v);
}
static void lac_terminate_all(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < 32; i++) {
        LibApplet *a = live_las[i];
        if (a && a->started && !a->done) {
            a->terminated = 1;
            a->done = 1;
            a->kbd_active = 0;
            if (a->ev_state_w) event_signal_handle(a->ev_state_w);
        }
    }
    pthread_mutex_unlock(&lock);
}
static const IpcCmd lac_cmds[] = {
    {0, "CreateLibraryApplet", lac_create_library_applet, IPC_IMPL},
    {1, "TerminateAllLibraryApplets", lac_terminate_all, IPC_IMPL},
    {2, "AreAnyLibraryAppletsLeft", lac_are_any_left, IPC_IMPL},
    {10, "CreateStorage", lac_create_storage, IPC_IMPL},
    {11, "CreateTransferMemoryStorage", lac_create_tmem_storage, IPC_IMPL},
};

/* ---- IApplicationFunctions ------------------------------------------------------------------------------------------------------ */
extern const uint8_t g_user_id[16];
static int preselected_popped;
/* PopLaunchParameter(kind): kind 2 is the preselected account (magic 0xC79497CA, selected flag, user id); anything else: no data. */
static void af_pop_launch_parameter(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IPC_IN(c, uint32_t, 0) == 2 && !preselected_popped) {
        preselected_popped = 1;
        IpcObject *st = storage_new(0x88);
        Storage *d = st->state;
        uint32_t magic = 0xC79497CA, sel = 1;
        memcpy(d->data, &magic, 4);
        memcpy(d->data + 4, &sel, 4);
        memcpy(d->data + 8, g_user_id, 16);
        ipc_out_object(c, st);
    } else c->result = 0x480;
}
/* EnsureSaveData(uid): the reply is the extra size the console needs to create the save (0 = it exists or fits). The host creates the save data on the first mount, so the
 * answer is always 0. */
static void af_ensure_save_data(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t required = 0;
    IPC_OUT(c, required);
    LOGV("applet", "EnsureSaveData: save data exists (created on first mount)");
}
static void af_set_terminate_result(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t r = IPC_IN(c, uint32_t, 0);
    LOG("applet", "SetTerminateResult %#x (module %u desc %u)", r, r & 0x1ff, (r >> 9) & 0x1fff);
}
static void af_get_desired_language(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t v = host_desired_language();
    IPC_OUT(c, v);
}
static void af_get_display_version(IpcObject *s, IpcCall *c) {
    (void)s;
    char v[16] = {0};
    host_display_version(v);
    ipc_out(c, v, 16);
}
static void af_notify_running(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 1;
    IPC_OUT(c, v);
    summary_stage("notify-running");
    LOG("applet", "the guest reports it is running (NotifyRunning)");
}
static void af_get_pseudo_device_id(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t id[16] = {0x4e, 0x61, 0x70, 0x70, 0x6c, 0x65};
    ipc_out(c, id, 16);
}
/* GetSaveDataSize(): {size, journal size} of the account save, from control.nacp (UserAccountSaveDataSize 0x3080, UserAccountSaveDataJournalSize 0x3088) when
 * `napple build` wrote one next to the data directory; 32 MiB each otherwise. */
static void af_get_save_data_size(IpcObject *s, IpcCall *c) {
    (void)s;
    int64_t a = 0x2000000, b = 0x2000000;
    char path[600];
    if (g_cfg.data_dir && snprintf(path, sizeof path, "%s/control.nacp", g_cfg.data_dir) < (int)sizeof path) {
        FILE *f = fopen(path, "rb");
        if (f) {
            uint8_t n[0x3090];
            if (fread(n, 1, sizeof n, f) == sizeof n) {
                int64_t x, y;
                memcpy(&x, n + 0x3080, 8);
                memcpy(&y, n + 0x3088, 8);
                if (x > 0 || y > 0) a = x, b = y;
            }
            fclose(f);
        }
    }
    IPC_OUT(c, a);
    IPC_OUT(c, b);
}
/* Game play recording (the console's "save the last 30 s of video" feature) is not offered: IsGamePlayRecordingSupported says no, and the Initialize/Flush
 * calls are accepted and counted. */
static void af_is_recording_supported(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 0;
    IPC_OUT(c, v);
}
static void af_init_recording(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    AS()->game_play_recording_initialized = 1;
}
static void af_flush_recording(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    AS()->game_play_recording_flushes++;
}
static void af_set_media_playback(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->media_playback_state = IPC_IN(c, uint8_t, 0);
}
static void af_init_copyright_fb(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    AS()->copyright_frame_buffer_initialized = 1;
}
static void af_set_copyright_image(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    if (b) AS()->copyright_image_size = b->size;
}
static void af_gpu_error_event(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_copy_handle(c, tracked_event(EV_GPU_ERROR, 0));
}
static void af_friend_invite_event(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_copy_handle(c, tracked_event(EV_FRIEND_INVITE, 0));
}
static void af_event_210(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_copy_handle(c, tracked_event(EV_AF_210, 0));
}
/* GetPreviousProgramIndex: -1 = this program was not started by another program of the same application (the console's answer for a single-program title). */
static void af_get_previous_program_index(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = -1;
    IPC_OUT(c, v);
}
static void af_begin_block_home_sl(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    applet_block_begin(&AS()->home_button_short_long_block);
}
static void af_end_block_home_sl(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    applet_block_end(&AS()->home_button_short_long_block);
}
static void af_begin_block_home(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    applet_block_begin(&AS()->home_button_block);
}
static void af_end_block_home(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    applet_block_end(&AS()->home_button_block);
}
static void af_set_recording_state(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint32_t)) AS()->game_play_recording_state = IPC_IN(c, uint32_t, 0);
}
static void af_enable_crash_report(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->crash_report_enabled = IPC_IN(c, uint8_t, 0) != 0;
}
static void af_set_copyright_visibility(IpcObject *s, IpcCall *c) {
    (void)s;
    if (IN_OK(c, 0, uint8_t)) AS()->copyright_visible = IPC_IN(c, uint8_t, 0) != 0;
}
/* cmd 210 on IApplicationFunctions returns a copy handle to a system event (the SDK's nn::oe layer polls it with WaitSynchronization(timeout 0) right
 * after apm OpenSession). An earlier revision answered with an IMovieMaker object: the reply then carried no handle, the guest kept handle 0 and the
 * boot thread of Animal Crossing 3.0.3 spun on WaitSynchronization(0) forever. Never signalled: no capture or error event is ever raised. */
/* Kept stubs in this table, each with its reason: PopLaunchParameter (only the preselected-account parameter exists; no ExecuteProgram / deep links),
 * EnsureSaveData and GetSaveDataSize (the data lives in fs.c, which owns it: the answer here is a fixed size), GetPreviousProgramIndex (no program switching, always -1). */
static const IpcCmd af_cmds[] = {
    {1, "PopLaunchParameter", af_pop_launch_parameter, IPC_IMPL},
    {20, "EnsureSaveData", af_ensure_save_data, IPC_IMPL},
    {21, "GetDesiredLanguage", af_get_desired_language, IPC_IMPL},
    {22, "SetTerminateResult", af_set_terminate_result, IPC_IMPL},
    {23, "GetDisplayVersion", af_get_display_version, IPC_IMPL},
    {26, "GetSaveDataSize", af_get_save_data_size, IPC_IMPL},
    {30, "BeginBlockingHomeButtonShortAndLongPressed", af_begin_block_home_sl, IPC_IMPL},
    {31, "EndBlockingHomeButtonShortAndLongPressed", af_end_block_home_sl, IPC_IMPL},
    {32, "BeginBlockingHomeButton", af_begin_block_home, IPC_IMPL},
    {33, "EndBlockingHomeButton", af_end_block_home, IPC_IMPL},
    {40, "NotifyRunning", af_notify_running, IPC_IMPL},
    {50, "GetPseudoDeviceId", af_get_pseudo_device_id, IPC_IMPL},
    {60, "SetMediaPlaybackStateForApplication", af_set_media_playback, IPC_IMPL},
    {65, "IsGamePlayRecordingSupported", af_is_recording_supported, IPC_IMPL},
    {66, "InitializeGamePlayRecording", af_init_recording, IPC_IMPL},
    {67, "SetGamePlayRecordingState", af_set_recording_state, IPC_IMPL},
    {68, "RequestFlushGamePlayingMovieForDebug", af_flush_recording, IPC_IMPL},
    {90, "EnableApplicationCrashReport", af_enable_crash_report, IPC_IMPL},
    {91, "InitializeApplicationCopyrightFrameBuffer", af_init_copyright_fb, IPC_IMPL},
    {92, "SetApplicationCopyrightImage", af_set_copyright_image, IPC_IMPL},
    {93, "SetApplicationCopyrightVisibility", af_set_copyright_visibility, IPC_IMPL},
    {110, "GetPreviousProgramIndex", af_get_previous_program_index, IPC_IMPL},
    {120, "GetGpuErrorDetectedSystemEvent", af_gpu_error_event, IPC_IMPL},
    {130, "GetFriendInvitationStorageChannelEvent", af_friend_invite_event, IPC_IMPL},
    {210, "GetSystemEvent210", af_event_210, IPC_IMPL},
};

/* ---- IApplicationProxy ------------------------------------------------------------------------------------------------------------ */
#define SUB(fn, name, table)                                                                                                                                   \
    static void fn(IpcObject *s, IpcCall *c) {                                                                                                                 \
        (void)s;                                                                                                                                               \
        ipc_out_object(c, IPC_OBJECT(name, table, NULL, NULL));                                                                                                \
    }
SUB(proxy_common_state_getter, "ICommonStateGetter", csg_cmds)
SUB(proxy_self_controller, "ISelfController", sc_cmds)
SUB(proxy_window_controller, "IWindowController", wc_cmds)
SUB(proxy_audio_controller, "IAudioController", ac_cmds)
SUB(proxy_display_controller, "IDisplayController", dc_cmds)
SUB(proxy_library_applet_creator, "ILibraryAppletCreator", lac_cmds)
SUB(proxy_application_functions, "IApplicationFunctions", af_cmds)
/* Kept stub: there is no home menu to notify. */
static const IpcCmd debug_cmds[] = {{0, "NotifyMessageToHomeMenuForDebug", csg_noop, IPC_IMPL}};
SUB(proxy_debug_functions, "IDebugFunctions", debug_cmds)
static const IpcCmd proxy_cmds[] = {
    {0, "GetCommonStateGetter", proxy_common_state_getter, IPC_IMPL},       {1, "GetSelfController", proxy_self_controller, IPC_IMPL},
    {2, "GetWindowController", proxy_window_controller, IPC_IMPL},          {3, "GetAudioController", proxy_audio_controller, IPC_IMPL},
    {4, "GetDisplayController", proxy_display_controller, IPC_IMPL},        {11, "GetLibraryAppletCreator", proxy_library_applet_creator, IPC_IMPL},
    {20, "GetApplicationFunctions", proxy_application_functions, IPC_IMPL}, {1000, "GetDebugFunctions", proxy_debug_functions, IPC_IMPL},
};

static void oe_open_application_proxy(IpcObject *s, IpcCall *c) {
    (void)s;
    pthread_mutex_lock(&lock);
    applet_start_once();
    pthread_mutex_unlock(&lock);
    if (c->ncopy && c->copy[0] != HANDLE_CURRENT_PROCESS) LOGV("applet", "OpenApplicationProxy: process handle %#x", c->copy[0]);
    ipc_out_object(c, IPC_OBJECT("IApplicationProxy", proxy_cmds, NULL, NULL));
}
static const IpcCmd oe_cmds[] = {{0, "OpenApplicationProxy", oe_open_application_proxy, IPC_IMPL}};
static IpcObject *oe_open(void) { return IPC_OBJECT("appletOE", oe_cmds, NULL, NULL); }
/* Debug/test aid: NAPPLE_DEBUG_LAUNCH_APPLET=<seconds> makes this process launch the Mii Maker as if the game had asked (ShowMiiEdit, no arguments but the
 * 0x100-byte one), so the in-process and child-process hosts can be exercised with any title. */
static void *debug_launch_thread(void *arg) {
    sleep((unsigned)(uintptr_t)arg);
    LibApplet a;
    memset(&a, 0, sizeof a);
    a.applet_id = 0x12;
    uint8_t *arg0 = calloc(1, 0x100);
    arg0[0] = 3;
    a.in[0] = (Storage){.data = arg0, .size = 0x100};
    a.nin = 1;
    int ok = libapplet_run_firmware(&a, "0100000000001009");
    LOG("applet", "debug launch: firmware applet %s, output %llu bytes", ok ? "ran" : "did not run", (unsigned long long)(a.nout ? a.outq[0]->size : 0));
    return NULL;
}
void service_register_applet(void) {
    ipc_register_service("appletOE", oe_open);
    /* The portable web / offline web (0x13, 0x17) and album (0x15) applets answer through the handler table (libapplet_run_registered). Their
     * offline documents come from <data-dir>/html when the build extracted the HtmlDocument romfs there (NAPPLE_HTML_ROOT overrides). */
    if (applet_web_register()) LOG("applet", "web applet handlers were not registered (table full)");
    if (applet_album_register()) LOG("applet", "album applet handler was not registered (table full)");
    if (g_cfg.data_dir) {
        char html[1300];
        snprintf(html, sizeof html, "%s/html", g_cfg.data_dir);
        if (!getenv("NAPPLE_HTML_ROOT") && access(html, R_OK) == 0) applet_web_set_html_root(html);
    }
    const char *d = getenv("NAPPLE_DEBUG_LAUNCH_APPLET");
#ifdef NAPPLE_LIB
    d = NULL; /* only the host image launches */
#endif
    if (d) {
        pthread_t t;
        pthread_create(&t, NULL, debug_launch_thread, (void *)(uintptr_t)atoi(d));
        pthread_detach(t);
    }
}

/* ---- shared with applet_ae.c: the objects of the proxy a firmware applet process gets from appletAE ---------------------------------------- */
IpcObject *applet_shared_object(const char *which) {
    if (!strcmp(which, "common")) return IPC_OBJECT("ICommonStateGetter", csg_cmds, NULL, NULL);
    if (!strcmp(which, "self")) {
        pthread_mutex_lock(&lock);
        applet_start_once();
        pthread_mutex_unlock(&lock);
        return IPC_OBJECT("ISelfController", sc_cmds, NULL, NULL);
    }
    if (!strcmp(which, "window")) return IPC_OBJECT("IWindowController", wc_cmds, NULL, NULL);
    if (!strcmp(which, "audio")) return IPC_OBJECT("IAudioController", ac_cmds, NULL, NULL);
    if (!strcmp(which, "display")) return IPC_OBJECT("IDisplayController", dc_cmds, NULL, NULL);
    if (!strcmp(which, "creator")) return IPC_OBJECT("ILibraryAppletCreator", lac_cmds, NULL, NULL);
    return NULL;
}
IpcObject *applet_storage_make(const void *data, uint64_t size) {
    IpcObject *o = storage_new(size);
    Storage *st = o->state;
    if (size) memcpy(st->data, data, size);
    return o;
}
int applet_storage_peek(const IpcObject *o, const uint8_t **data, uint64_t *size) {
    const Storage *st = o ? o->state : NULL;
    if (!st) return 0;
    *data = storage_bytes(st);
    *size = st->size;
    return 1;
}
