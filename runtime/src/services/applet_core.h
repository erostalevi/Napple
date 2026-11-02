/* applet core: the application-visible settings the guest sets through ISelfController / IAudioController / IApplicationFunctions and
 * reads back (portable). Before this the setters were no-ops and the getters constants; now a set value is what a get returns. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { APPLET_SCREENSHOT_INHERIT = 0, APPLET_SCREENSHOT_ENABLE = 1, APPLET_SCREENSHOT_DISABLE = 2 };

/* am results for "nothing there". A storage channel that is empty (PopOutData, PopInteractiveOutData, PopInData, PopInteractiveInData,
 * PopLaunchParameter, the context stack, the TryPopFrom...Channel commands) answers NoDataInChannel, module 128 description 2: every nn::applet
 * TryPopFrom*Channel reader of the SDK treats exactly 0x480 as "empty" and aborts on any other error (MK8D v1376256 sdk: TryPopFromOutChannel
 * +0xd316c, TryPopFromInteractiveOutChannel +0xd3210, TryPopFromInChannel +0xd384c, TryPopFromInteractiveInChannel +0xd38f0,
 * TryPopFromApplicationParameterChannel +0xd475c, TryPopFromContextStack +0xd3ce4: `and w8, w0, #0x3fffff; cmp w8, #0x480`). The message queue
 * (ICommonStateGetter::ReceiveMessage) answers NoMessages, description 3 (nn::oe::TryPopNotificationMessage +0x15c214 compares 0x680). */
#define APPLET_R_NO_DATA_IN_CHANNEL 0x480u
#define APPLET_R_NO_MESSAGES 0x680u
/* ILibraryAppletAccessor::GetResult of an applet that ended without completing: LibraryAppletTerminated, module 128 description 22. The SDK's
 * nn::applet::JoinLibraryAppletImpl (MK8D v1376256 sdk +0xd2ddc) turns GetResult into the exit reason the applet readers switch on: 0 -> Normal,
 * module 128 description 22 -> Canceled, descriptions 20..49 -> Abnormal, anything else -> Unexpected (most readers abort on it). */
#define APPLET_R_LIBRARY_APPLET_TERMINATED 0x2c80u

typedef struct {
    float main_volume, library_volume, transparent_volume_rate; /* IAudioController: expected master volumes, 0..1 */
    int vr_mode, auto_sleep_disabled, restart_message, capture_short_press_message, album_image_taken_notification;
    int screenshot_permission; /* APPLET_SCREENSHOT_* */
    uint32_t idle_detection_extension, input_detection_source_set, screenshot_orientation, keyboard_layout, wireless_priority_mode;
    uint32_t cpu_boost_mode;
    unsigned cpu_boost_requests;
    uint32_t game_play_recording_state, media_playback_state;
    int controller_firmware_update_section, crash_report_enabled, copyright_visible;
    unsigned home_button_block, home_button_short_long_block; /* nesting depth of BeginBlocking... */
    uint32_t user_active_reports;
    uint32_t operation_mode_notification, performance_mode_notification, focus_handling_mode;
    /* sleep: Allow/DisallowToEnterSleep is a flag; the lock is a counted acquire (Request/Release); Transient drops it until the next Request */
    int sleep_disallowed, sleep_lock_held, sleep_lock_transient;
    unsigned sleep_lock_acquires;
    unsigned fatal_section; /* nesting depth of Enter/LeaveFatalSection */
    int window_visible, foreground_rights, reject_background, handles_request_to_display;
    unsigned approve_to_display_count;
    uint64_t gpu_time_slice; /* SetAppletGpuTimeSlice (ns); 0 = unset */
    int32_t auto_sleep_override[4];
    int auto_sleep_overridden;
    uint32_t last_multimedia_error;
    unsigned multimedia_error_reports;
    uint8_t screenshot_identity[0x18];
    int screenshot_identity_set;
    uint64_t album_user_data_size, album_user_data_hash;
    uint64_t suspended_ticks;           /* AccumulatedSuspendedTickValue: time the application spent suspended (never, while it runs in focus) */
    int request_exit_to_library_applet; /* SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled */
    int game_play_recording_initialized;
    unsigned game_play_recording_flushes;
    int copyright_frame_buffer_initialized;
    uint64_t copyright_image_size;
    uint32_t platform_region; /* 1 Global, 2 China */
    int illuminance_available;
    float illuminance; /* lux */
    int record_volume_muted;               /* SetRecordVolumeMuted: game play recordings without the game's sound */
    uint32_t layer_separation_mode;        /* SetManagedDisplayLayerSeparationMode */
    int recording_layer_composition;       /* SetRecordingLayerCompositionEnabled */
    unsigned screenshots_saved;            /* SaveCurrentScreenshot calls that stored an album entry */
} AppletState;

void applet_state_init(AppletState *s);
float applet_clamp_volume(float v, float fallback); /* NaN -> fallback, clamped to 0..1 */
int applet_valid_screenshot_permission(uint32_t v);
/* Begin/End nesting: End never goes below 0. Returns whether blocking is active afterwards. */
int applet_block_begin(unsigned *depth);
int applet_block_end(unsigned *depth);
/* Screenshots are allowed unless the guest disabled them. */
int applet_screenshot_allowed(const AppletState *s);

/* ---- sleep / illuminance / sleep-lock helpers -------------------------------------------------------------------------------------------- */
/* RequestToAcquireSleepLock: the lock is granted at once (nothing else contends for it); returns whether the acquired event must be signalled. */
int applet_sleep_lock_acquire(AppletState *s);
void applet_sleep_lock_release(AppletState *s);
void applet_sleep_lock_release_transiently(AppletState *s);
/* Enter/LeaveFatalSection nest like the home-button block (End below zero is ignored). */
int applet_fatal_enter(AppletState *s);
int applet_fatal_leave(AppletState *s);
/* The illuminance sensor: absent unless a host value is supplied (NAPPLE_ILLUMINANCE=<lux>). Reading while absent gives the nominal 100 lux the previous
 * answer used. `over_limit` is the Ex variant's flag: the value is above 100000 lux (clamped to it). */
void applet_illuminance_set(AppletState *s, int available, float lux);
float applet_illuminance_get(const AppletState *s, int *over_limit);

/* ---- capture slots (IDisplayController) -------------------------------------------------------------------------------------------------- */
/* The display controller keeps up to three 1280x720 RGBA8 images: the last foreground image, the last application image and the caller applet's.
 * They are filled from the host's last presented frame (a source the frontend registers; none = the slots stay empty and read as opaque black). */
enum { APPLET_CAP_LAST_FOREGROUND = 0, APPLET_CAP_LAST_APPLICATION = 1, APPLET_CAP_CALLER_APPLET = 2, APPLET_CAP_SLOTS = 3 };
#define APPLET_CAP_W 1280u
#define APPLET_CAP_H 720u
#define APPLET_CAP_BYTES (APPLET_CAP_W * APPLET_CAP_H * 4u)
/* Source: copies the last presented frame into `rgba` (w*h*4 bytes, row stride w*4) and returns 1, or returns 0 when there is no frame yet. */
typedef int (*AppletFrameSource)(uint8_t *rgba, uint32_t w, uint32_t h);
void applet_capture_set_source(AppletFrameSource src); /* NULL (the default) = none */
void applet_capture_register_host_source(void); /* applet_capture_host.c: the graphics back end's last completed frame (macOS); a no-op elsewhere */
/* Capture the source's frame into a slot; returns 1 when a frame was copied, 0 when the slot now holds black. */
int applet_capture_update(unsigned slot);
/* Read a slot into out (cap bytes, up to APPLET_CAP_BYTES); returns the bytes written (the slot is black until updated), 0 for a bad slot. */
size_t applet_capture_read(unsigned slot, uint8_t *out, size_t cap, int *has_frame);
int applet_capture_copy(unsigned dst, unsigned src); /* 0 bad slot */
int applet_capture_has_frame(unsigned slot);
/* The last application capture buffer can be held by one user at a time (Acquire/ReleaseLastApplicationCaptureBuffer): returns 0 when it is already held. */
int applet_capture_acquire_app_buffer(void);
void applet_capture_release_app_buffer(void);
void applet_capture_reset(void);

/* ---- host events (applet.c): wake the system events the guest holds; hooks for the frontend and tests ---------------------------------------- */
void applet_signal_gpu_error(void);           /* IApplicationFunctions::GetGpuErrorDetectedSystemEvent */
void applet_signal_friend_invitation(void);   /* GetFriendInvitationStorageChannelEvent */
void applet_display_resolution_changed(void); /* GetDefaultDisplayResolutionChangeEvent (also raised by an operation-mode message) */
int applet_sleep_disallowed(void);            /* the guest asked the host not to sleep (Disallow, a held sleep lock, or auto sleep disabled) */
